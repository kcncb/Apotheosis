#pragma once
//
// GpuH264Decoder -- NVDEC-backed HEVC decoder for the ETH transport path.
//
// Three-stage cuvid pipeline:
//   parser (CUvideoparser) <- fed HEVC access-unit bytes via parse()
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
    void reapPendingSurfaces(bool waitAll);

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

    struct PendingSurface
    {
        CUdeviceptr devptr{ 0 };
        cudaEvent_t done{ nullptr };
        bool active{ false };
    };
    // Keep several mapped NVDEC surfaces in flight. Completed entries are
    // reaped with cudaEventQuery; only a completely full ring waits for the
    // oldest entry, eliminating the old unconditional per-frame sync.
    static constexpr size_t PENDING_SURFACES = 4;
    std::array<PendingSurface, PENDING_SURFACES> pending_surfaces_{};
    size_t pending_head_{ 0 };
    size_t pending_count_{ 0 };

    // Reuse BGR outputs instead of cudaMalloc/cudaFree on every decoded frame.
    // Per-frame allocator calls synchronize the device and serialize NVDEC
    // against TensorRT; a ring keeps capture and inference overlapped.
    static constexpr size_t OUTPUT_POOL_SIZE = 8;
    std::array<GpuImage, OUTPUT_POOL_SIZE> output_pool_{};
    std::array<cudaEvent_t, OUTPUT_POOL_SIZE> output_events_{};
    size_t output_pool_index_{ 0 };
};

} // namespace capture
