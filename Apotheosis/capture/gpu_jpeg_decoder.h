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

    // Decode ONLY a centered cropW x cropH region using nvJPEG ROI decoding
    // (decoupled phase API + nvjpegDecodeParamsSetROI). Only the ROI is
    // reconstructed (IDCT / chroma upsample / color convert / output write are
    // limited to it) and dst is allocated at the ROI size, so on a weak GPU this
    // is far cheaper than a full-frame decode + crop. Entropy (Huffman) decode
    // still runs on the host from the top of the frame down to the ROI's bottom
    // edge. Returns false if ROI decoding is unavailable/fails (caller falls
    // back to decode() + manual crop). Safe to pipeline without a per-frame
    // CPU sync: the decoupled host/transfer/device buffers are ring-buffered.
    bool decodeCropped(
        const uint8_t* data,
        size_t size,
        int cropW,
        int cropH,
        GpuImage& dst,
        cudaStream_t stream);

private:
    void cleanup();
    bool initRoi();
    void cleanupRoi();

    bool initialized_{ false };
    nvjpegHandle_t handle_{ nullptr };
    nvjpegJpegState_t state_{ nullptr };

    // Decoupled ROI decode objects. The per-frame state, its attached
    // pinned/device scratch and the jpeg-stream parser are RING-BUFFERED: the
    // decoupled API splits a decode into a synchronous host (Huffman) phase that
    // writes the pinned buffer and async transfer/device phases on the caller's
    // stream that read it. With a single buffer set, a pipelined caller (one
    // that records a completion event instead of CPU-syncing each frame) would
    // let frame N+1's host phase overwrite buffers frame N's async transfer is
    // still reading — silent corruption. Round-robining ROI_RING independent
    // sets makes consecutive in-flight frames use disjoint scratch. The decoder
    // handle and ROI params are stateless across frames and stay shared.
    static constexpr int ROI_RING = 3;
    bool roi_initialized_{ false };
    nvjpegJpegDecoder_t roi_decoder_{ nullptr };
    nvjpegDecodeParams_t roi_params_{ nullptr };
    nvjpegJpegState_t roi_state_[ROI_RING]{};
    nvjpegBufferPinned_t roi_pinned_[ROI_RING]{};
    nvjpegBufferDevice_t roi_device_[ROI_RING]{};
    nvjpegJpegStream_t roi_stream_[ROI_RING]{};
    // Per-slot completion event: decodeCropped waits on it before reusing a slot
    // (so a slow GPU throttles the caller instead of clobbering a pinned buffer
    // whose transfer is still in flight) and records it after that frame's
    // device phase. No-op at steady state; pipeline runs ROI_RING-deep.
    cudaEvent_t roi_event_[ROI_RING]{};
    size_t roi_slot_{ 0 };
};

} // namespace capture
