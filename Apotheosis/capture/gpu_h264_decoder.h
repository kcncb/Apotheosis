#pragma once
//
// GpuH264Decoder -- NVDEC-backed H.264 decoder for the ETH transport path.
//
// Three-stage cuvid pipeline:
//   parser (CUvideoparser) <- fed H.264 NALU bytes via parse()
//      -> HandleVideoSequence callback: codec/dims known -> (re)create decoder
//      -> HandlePictureDecode callback: cuvidDecodePicture
//      -> HandlePictureDisplay callback: cuvidMapVideoFrame -> CUdeviceptr NV12
//                                       -> NV12->BGR8 kernel -> sink_(GpuImage)
//
// Sender ships all-intra so each NALU bundle is a self-contained IDR; the
// parser produces exactly one display frame per parse() call.
//
// dynlink_loader is header-only: loads nvcuda.dll / nvcuvid.dll (NVIDIA driver
// supplies them) at runtime. No additional .lib linking needed.
//
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>

#include "../mem/gpu_image.h"

extern "C" {
#include <ffnvcodec/dynlink_loader.h>
}

namespace capture {

class GpuH264Decoder
{
public:
    GpuH264Decoder();
    ~GpuH264Decoder();

    GpuH264Decoder(const GpuH264Decoder&) = delete;
    GpuH264Decoder& operator=(const GpuH264Decoder&) = delete;

    bool init();
    bool ready() const { return initialized_; }

    using FrameSink = std::function<void(GpuImage&&)>;
    void setSink(FrameSink sink) { sink_ = std::move(sink); }

    bool parse(const uint8_t* nalu, size_t size, cudaStream_t stream);

private:
    static int CUDAAPI seqCb(void* user, CUVIDEOFORMAT* fmt);
    static int CUDAAPI decCb(void* user, CUVIDPICPARAMS* params);
    static int CUDAAPI dispCb(void* user, CUVIDPARSERDISPINFO* disp);

    void cleanup();
    bool ensureDecoder(const CUVIDEOFORMAT& fmt);

    bool initialized_{ false };
    CuvidFunctions* cuvid_{ nullptr };
    CudaFunctions*  cuda_{ nullptr };

    CUcontext       cuctx_{ nullptr };
    CUvideoctxlock  ctxLock_{ nullptr };
    CUvideoparser   parser_{ nullptr };
    CUvideodecoder  decoder_{ nullptr };

    int             cw_{ 0 }, ch_{ 0 };
    cudaStream_t    activeStream_{ nullptr };
    FrameSink       sink_;

    // 一级"延迟 unmap"流水线:本次 dispCb 不立刻 cudaStreamSynchronize 等 BGR
    // 转换 kernel,把 unmap 推迟到下一次 dispCb 入口再做。等 N-1 的 kernel
    // 时,N 的 wire→parse 已经在跑——sync 落在 receive thread 的关键路径外。
    // 旧实现里这一行 cudaStreamSynchronize 是 PCIe Gen1 卡上每帧 ~0.5-1.5ms
    // 的隐藏阻塞,直接把接收吞吐压到了 180-190fps。
    CUdeviceptr     pending_unmap_devptr_{ 0 };
    bool            has_pending_unmap_{ false };

    // Reuse BGR outputs instead of cudaMalloc/cudaFree on every decoded frame.
    // Per-frame allocator calls synchronize the device and serialize NVDEC
    // against TensorRT; a ring keeps capture and inference overlapped.
    static constexpr size_t OUTPUT_POOL_SIZE = 8;
    std::array<GpuImage, OUTPUT_POOL_SIZE> output_pool_{};
    size_t output_pool_index_{ 0 };
};

} // namespace capture
