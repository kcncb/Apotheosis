#pragma once

#include <cuda_runtime.h>
#include <nvjpeg.h>

#include <cstddef>
#include <cstdint>

#include "../mem/gpu_image.h"

namespace capture {

// Minimal nvJPEG wrapper that decodes a single MJPEG frame to an interleaved
// BGR uint8 GpuImage on a caller-provided CUDA stream. One instance is tied
// to a single thread (the receive thread) — nvjpegHandle_t is thread-safe
// for independent calls but nvjpegJpegState_t is not, so keep it per-thread.
class GpuJpegDecoder
{
public:
    GpuJpegDecoder();
    ~GpuJpegDecoder();

    GpuJpegDecoder(const GpuJpegDecoder&) = delete;
    GpuJpegDecoder& operator=(const GpuJpegDecoder&) = delete;

    bool init();
    bool ready() const { return initialized_; }

    // Decode into dst (BGR8 interleaved). Reallocates dst if the size doesn't
    // match the JPEG dimensions. Returns false on any nvjpeg error; caller
    // should fall back to CPU cv::imdecode.
    bool decode(
        const uint8_t* data,
        size_t size,
        GpuImage& dst,
        cudaStream_t stream);

private:
    void cleanup();

    bool initialized_{ false };
    nvjpegHandle_t handle_{ nullptr };
    nvjpegJpegState_t state_{ nullptr };
};

} // namespace capture
