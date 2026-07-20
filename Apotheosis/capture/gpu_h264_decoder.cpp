#include "gpu_h264_decoder.h"
#include "gpu_color_ops.h"

#include <iostream>
#include <cstring>

namespace capture {

GpuH264Decoder::GpuH264Decoder() = default;
GpuH264Decoder::~GpuH264Decoder() { cleanup(); }

void GpuH264Decoder::cleanup()
{
    // 关停前先把"延迟 unmap"清掉,否则 destroy decoder 会留一个未配对的 map ref。
    if (cuvid_ && decoder_ && has_pending_unmap_)
    {
        if (activeStream_) cudaStreamSynchronize(activeStream_);
        cuvid_->cuvidUnmapVideoFrame(decoder_, pending_unmap_devptr_);
        has_pending_unmap_ = false;
        pending_unmap_devptr_ = 0;
    }

    if (cuvid_ && parser_) { cuvid_->cuvidDestroyVideoParser(parser_); parser_ = nullptr; }
    if (cuvid_ && decoder_){ cuvid_->cuvidDestroyDecoder(decoder_);    decoder_ = nullptr; }
    if (cuvid_ && ctxLock_){ cuvid_->cuvidCtxLockDestroy(ctxLock_);    ctxLock_ = nullptr; }
    if (cuvid_)            { cuvid_free_functions(&cuvid_);            cuvid_ = nullptr; }
    if (cuda_)             { cuda_free_functions(&cuda_);              cuda_ = nullptr; }
    cuctx_ = nullptr;
    initialized_ = false;
}

bool GpuH264Decoder::init()
{
    if (initialized_) return true;

    // Dynlink nvcuvid.dll / nvcuda.dll (shipped with NVIDIA driver). The
    // FFmpeg helper fills cuvid_ / cuda_ function tables.
    if (cuda_load_functions(&cuda_, nullptr) < 0)  { std::cerr << "[GpuH264Decoder] cuda dynlink failed\n"; cleanup(); return false; }
    if (cuvid_load_functions(&cuvid_, nullptr) < 0){ std::cerr << "[GpuH264Decoder] nvcuvid dynlink failed\n"; cleanup(); return false; }

    if (cuda_->cuInit(0) != CUDA_SUCCESS) { std::cerr << "[GpuH264Decoder] cuInit failed\n"; cleanup(); return false; }

    // Use the same primary context the CUDA runtime is on -- shared with the
    // capture thread's other CUDA work, zero context-switch overhead.
    int devId = 0;
    cudaGetDevice(&devId);
    CUdevice dev = 0;
    if (cuda_->cuDeviceGet(&dev, devId) != CUDA_SUCCESS) { cleanup(); return false; }
    if (cuda_->cuDevicePrimaryCtxRetain(&cuctx_, dev) != CUDA_SUCCESS) { cleanup(); return false; }
    cuda_->cuCtxPushCurrent(cuctx_);

    if (cuvid_->cuvidCtxLockCreate(&ctxLock_, cuctx_) != CUDA_SUCCESS) { cleanup(); return false; }

    // Parser: HEVC, single-frame display delay (sender is all-intra).
    // 切 HEVC 是因为 NVDEC 不支持 H.264 4:4:4 (任何代都不支持). HEVC 4:4:4
    // 在 NVDEC 5th gen+ (Turing 起) 正常支持. CMP 40HX 是 TU106 NVDEC 5th gen.
    CUVIDPARSERPARAMS pp{};
    pp.CodecType            = cudaVideoCodec_HEVC;
    pp.ulMaxNumDecodeSurfaces = 4;
    pp.ulMaxDisplayDelay    = 0;             // no reorder; IDR in, frame out
    pp.pUserData            = this;
    pp.pfnSequenceCallback  = GpuH264Decoder::seqCb;
    pp.pfnDecodePicture     = GpuH264Decoder::decCb;
    pp.pfnDisplayPicture    = GpuH264Decoder::dispCb;
    if (cuvid_->cuvidCreateVideoParser(&parser_, &pp) != CUDA_SUCCESS) { cleanup(); return false; }

    cuda_->cuCtxPopCurrent(nullptr);
    initialized_ = true;
    return true;
}

bool GpuH264Decoder::ensureDecoder(const CUVIDEOFORMAT& fmt)
{
    if (decoder_ && (int)fmt.coded_width == cw_ && (int)fmt.coded_height == ch_)
        return true;

    if (decoder_) { cuvid_->cuvidDestroyDecoder(decoder_); decoder_ = nullptr; }

    CUVIDDECODECREATEINFO ci{};
    ci.CodecType            = cudaVideoCodec_HEVC;
    // HEVC 4:4:4: 发射端 NVENC HEVC FREXT profile + AYUV input. NVDEC 必须
    // ChromaFormat_444 + SurfaceFormat_YUV444 才能解 full chroma. H.264
    // 不支持 444 解码 (NVDEC 历史限制), 所以切 HEVC.
    ci.ChromaFormat         = cudaVideoChromaFormat_444;
    ci.OutputFormat         = cudaVideoSurfaceFormat_YUV444;
    ci.bitDepthMinus8       = 0;
    ci.DeinterlaceMode      = cudaVideoDeinterlaceMode_Weave;
    ci.ulWidth              = fmt.coded_width;
    ci.ulHeight             = fmt.coded_height;
    // pool 扩大: 延迟 unmap 流水线下,任意瞬间最多有 2 个 surface 处于 mapped
    // 状态(本帧 + 上一帧待 unmap)。原 ulNumOutputSurfaces=2 在小输入尺寸下也
    // 工作,但接收线程不再阻塞 sync 之后,NVDEC 可以与 BGR kernel + sink 完全
    // 并行入队多帧,适当放宽 surface 池避免突发负载触发 wait-for-surface 路径。
    ci.ulNumDecodeSurfaces  = 8;
    ci.ulNumOutputSurfaces  = 4;
    ci.ulTargetWidth        = fmt.coded_width;
    ci.ulTargetHeight       = fmt.coded_height;
    ci.ulCreationFlags      = cudaVideoCreate_PreferCUVID;
    ci.vidLock              = ctxLock_;

    if (cuvid_->cuvidCreateDecoder(&decoder_, &ci) != CUDA_SUCCESS) {
        std::cerr << "[GpuH264Decoder] cuvidCreateDecoder failed for " << fmt.coded_width
                  << "x" << fmt.coded_height << "\n";
        return false;
    }
    cw_ = fmt.coded_width;
    ch_ = fmt.coded_height;
    return true;
}

int CUDAAPI GpuH264Decoder::seqCb(void* user, CUVIDEOFORMAT* fmt)
{
    auto* self = static_cast<GpuH264Decoder*>(user);
    return self->ensureDecoder(*fmt) ? 1 : 0;
}

int CUDAAPI GpuH264Decoder::decCb(void* user, CUVIDPICPARAMS* params)
{
    auto* self = static_cast<GpuH264Decoder*>(user);
    if (!self->decoder_) return 0;
    return self->cuvid_->cuvidDecodePicture(self->decoder_, params) == CUDA_SUCCESS ? 1 : 0;
}

int CUDAAPI GpuH264Decoder::dispCb(void* user, CUVIDPARSERDISPINFO* disp)
{
    auto* self = static_cast<GpuH264Decoder*>(user);
    if (!self->decoder_ || !self->sink_) return 1;

    // --- 第一步:把"上一帧待 unmap 的 surface"清掉 ---
    // 旧实现在本帧 dispCb 里同步等本帧 kernel 再 unmap 本帧 surface,这一段
    // cudaStreamSynchronize 是 receive thread 的关键路径阻塞(PCIe Gen1 上每帧
    // 0.5-1.5ms,把 240fps 压到 180-190)。这里改成"延迟一帧 unmap":
    //   - frame N 入口先 sync stream(等 N-1 的 kernel 完成),unmap N-1 的 surface;
    //   - 然后 map N、launch N 的 kernel、record event 给 consumer、立刻 sink;
    //   - 保存 N 的 devPtr 作为"下一次 dispCb 入口"要 unmap 的目标。
    // 这样 N-1 的 kernel 在 N 的 wire/parse 期间已经跑过一段时间,sync 等的多半
    // 是个"已经完成"的状态——把 GPU sync 从关键路径上移走了。
    if (self->has_pending_unmap_)
    {
        cudaStreamSynchronize(self->activeStream_);
        self->cuvid_->cuvidUnmapVideoFrame(self->decoder_, self->pending_unmap_devptr_);
        self->has_pending_unmap_ = false;
        self->pending_unmap_devptr_ = 0;
    }

    CUVIDPROCPARAMS pp{};
    pp.progressive_frame = disp->progressive_frame;
    pp.top_field_first   = disp->top_field_first;
    pp.unpaired_field    = (disp->repeat_first_field < 0);
    pp.output_stream     = nullptr;

    CUdeviceptr devPtr = 0;
    unsigned int pitch = 0;
    if (self->cuvid_->cuvidMapVideoFrame(self->decoder_, disp->picture_index,
                                        &devPtr, &pitch, &pp) != CUDA_SUCCESS)
        return 0;

    // YUV444 planar layout: Y plane [pitch * H], then U plane [pitch * H],
    // then V plane [pitch * H]. NVDEC 444 surface 是 3 个独立 full-res 平面.
    const unsigned char* y = reinterpret_cast<const unsigned char*>(devPtr);
    const unsigned char* u = y + (size_t)pitch * self->ch_;
    const unsigned char* v = u + (size_t)pitch * self->ch_;

    GpuImage& slot = self->output_pool_[self->output_pool_index_];
    if (!slot.create(self->ch_, self->cw_, 3)) {
        // 失败也别忘了 unmap,否则 surface 永久泄漏。
        self->cuvid_->cuvidUnmapVideoFrame(self->decoder_, devPtr);
        return 0;
    }

    launch_yuv444_to_bgr_u8(
        y, u, v, (size_t)pitch,
        slot.data(), (size_t)slot.step(),
        self->cw_, self->ch_,
        self->activeStream_);

    // 标记本帧 surface 在下一次 dispCb 入口处 unmap。当前帧不 sync,kernel 异步。
    self->pending_unmap_devptr_ = devPtr;
    self->has_pending_unmap_ = true;

    // sink 不再等 kernel——consumer 通过附在 GpuImage 上的 readyEvent (由
    // EthCapture 的 sink 回调记录) 自己 cudaStreamWaitEvent 同步。
    GpuImage out = slot;
    self->output_pool_index_ =
        (self->output_pool_index_ + 1) % OUTPUT_POOL_SIZE;
    self->sink_(std::move(out));
    return 1;
}

bool GpuH264Decoder::parse(const uint8_t* nalu, size_t size, cudaStream_t stream)
{
    if (!initialized_ || !nalu || size == 0) return false;
    activeStream_ = stream;

    cuda_->cuCtxPushCurrent(cuctx_);

    CUVIDSOURCEDATAPACKET pkt{};
    pkt.flags        = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    pkt.payload_size = (unsigned long)size;
    pkt.payload      = nalu;
    pkt.timestamp    = 0;
    CUresult r = cuvid_->cuvidParseVideoData(parser_, &pkt);

    cuda_->cuCtxPopCurrent(nullptr);
    return r == CUDA_SUCCESS;
}

} // namespace capture
