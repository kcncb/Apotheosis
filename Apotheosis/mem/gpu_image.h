#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cv { class Mat; }

// Lightweight non-owning view of a GPU image. Pass this to CUDA kernels in
// place of the cv::cuda::GpuMat the project used to depend on.
struct GpuFrame
{
    unsigned char* data = nullptr;  // device pointer to row 0 of this view
    int rows = 0;
    int cols = 0;
    int channels = 0;
    size_t step = 0;                // row stride in bytes

    bool empty() const noexcept { return data == nullptr || rows == 0 || cols == 0; }
};

// Owning GPU image with cv::cuda::GpuMat-like reference-counted semantics:
// copies share the underlying device buffer (cheap shared_ptr bump), and
// subRect() returns a zero-copy view that keeps the parent buffer alive.
//
// This replaces cv::cuda::GpuMat across the project so we no longer need
// OpenCV's CUDA modules to be built. All GPU work goes through hand-written
// CUDA / NPP / nvJPEG calls that operate on raw device pointers + step.
class GpuImage
{
public:
    GpuImage() = default;
    ~GpuImage() = default;

    GpuImage(const GpuImage&) = default;
    GpuImage& operator=(const GpuImage&) = default;
    GpuImage(GpuImage&&) noexcept = default;
    GpuImage& operator=(GpuImage&&) noexcept = default;

    // Allocate a row-major buffer with natural step (cols * channels). If the
    // current allocation already matches the requested shape this is a no-op
    // — important to avoid cudaMalloc churn on the per-frame hot paths.
    // Returns true on success.
    bool create(int rows, int cols, int channels);

    // Drop ownership. Other copies of this image keep the buffer alive.
    void release() noexcept;

    bool empty() const noexcept { return data_ == nullptr || rows_ == 0 || cols_ == 0; }
    int rows() const noexcept { return rows_; }
    int cols() const noexcept { return cols_; }
    int channels() const noexcept { return channels_; }
    size_t step() const noexcept { return step_; }
    unsigned char* data() noexcept { return data_; }
    const unsigned char* data() const noexcept { return data_; }

    // Upload from host pixels with possibly padded source stride. Allocates
    // (or reuses) the destination buffer to (rows, cols, channels) with
    // natural step. Async with respect to the caller's stream.
    bool upload(const unsigned char* src,
                int rows,
                int cols,
                int channels,
                size_t srcStep,
                cudaStream_t stream = nullptr);

    // Download to a host cv::Mat (allocates if shape/type mismatch). Async on
    // `stream`; caller must synchronize before reading host pixels.
    void download(cv::Mat& dst, cudaStream_t stream = nullptr) const;

    // Zero-copy sub-rectangle view; shares ownership of the underlying buffer.
    GpuImage subRect(int x, int y, int w, int h) const;

    GpuFrame view() const noexcept
    {
        return GpuFrame{ data_, rows_, cols_, channels_, step_ };
    }

    // Non-owning "this frame's GPU writes are complete" marker. The producer
    // (e.g. capture's decode stream) records it; a consumer on a different
    // stream waits via cudaStreamWaitEvent instead of a CPU-blocking
    // cudaStreamSynchronize. The event is owned by the producer's pool, not by
    // GpuImage — copies share it, matching the buffer's shared_ptr semantics.
    void setReadyEvent(cudaEvent_t e) noexcept { ready_event_ = e; }
    cudaEvent_t readyEvent() const noexcept { return ready_event_; }

    static uint64_t allocationCount() noexcept;
    static uint64_t allocationBytes() noexcept;

private:
    struct Storage
    {
        unsigned char* base = nullptr;
        size_t capacity = 0;
        ~Storage();
    };

    std::shared_ptr<Storage> storage_;
    unsigned char* data_ = nullptr;
    int rows_ = 0;
    int cols_ = 0;
    int channels_ = 0;
    size_t step_ = 0;
    cudaEvent_t ready_event_ = nullptr;
};
