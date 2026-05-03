#ifndef CAPTURE_CARD_H
#define CAPTURE_CARD_H

#include "capture.h"
#include "gpu_jpeg_decoder.h"

#include <cuda_runtime.h>
#include <npp.h>
#include <opencv2/opencv.hpp>

#include "../mem/gpu_image.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct CaptureCardDeviceInfo
{
    int index = 0;
    std::string name;
};

// Unified direct capture-card backend. Bypasses cv::VideoCapture entirely
// and talks to the device through Media Foundation (IMFSourceReader). The
// "MF" detail is intentionally hidden from the rest of the codebase: callers
// pick a device, geometry, fps, and a pixel format (or AUTO), and this class
// negotiates the closest native mode the device supports.
//
// Supported formats:
//   - NV12  : raw biplanar YUV; CPU NV12->BGR via cv::cvtColor (~1-2ms@1080p)
//   - MJPG  : raw JPEG; GPU decode via nvJPEG, delivered as GpuMat
//   - YUY2  : packed 4:2:2; CPU YUY2->BGR via cv::cvtColor
//   - RGB32 : BGRA; CPU BGRA->BGR via cv::cvtColor
//
// AUTO tries NV12 -> MJPG -> YUY2 -> RGB32 in order, falling back when the
// device doesn't expose a given format. The producer thread is decoupled
// from the consumer with a small queue so consumer-side jitter does not
// bound the source FPS.
class CaptureCard : public IScreenCapture
{
public:
    enum class Format
    {
        Auto,
        Nv12,
        Mjpg,
        Yuy2,
        Rgb32
    };

    CaptureCard(int detection_width,
                int detection_height,
                int device_index,
                int capture_width,
                int capture_height,
                int capture_fps,
                int crop_width,
                int crop_height,
                Format requested_format);
    ~CaptureCard() override;

    CaptureCard(const CaptureCard&) = delete;
    CaptureCard& operator=(const CaptureCard&) = delete;

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;
    int GetSourceFpsEstimate() const override { return source_fps_.load(); }

    bool IsOpen() const { return is_open_.load(); }

    static std::vector<CaptureCardDeviceInfo> EnumerateDevices();
    static Format ParseFormat(const std::string& s);
    static const char* FormatLabel(Format f);

private:
    enum class ActiveFormat
    {
        None,
        Nv12,
        MjpgGpu,
        Yuy2,
        Rgb32
    };

    void ReceiveThread();

    bool TickFps();

    bool PushNv12(const uint8_t* data, int width, int height, int stride);
    bool PushYuy2(const uint8_t* data, int width, int height, int stride);
    bool PushRgb32(const uint8_t* data, int width, int height, int stride);
    bool PushMjpgGpu(const uint8_t* data, size_t size);

    GpuImage ApplyCropGpu(const GpuImage& src) const;

    int detection_width_;
    int detection_height_;
    int device_index_;
    int capture_width_;
    int capture_height_;
    int capture_fps_;
    int crop_width_;
    int crop_height_;
    Format requested_format_;

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };
    std::atomic<int> source_fps_{ 0 };
    std::atomic<int> source_frame_count_{ 0 };
    std::atomic<int> dropped_frames_{ 0 };
    std::chrono::steady_clock::time_point source_fps_start_time_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::queue<cv::Mat> cpu_frame_queue_;
    std::queue<GpuImage> gpu_frame_queue_;

    ActiveFormat active_format_{ ActiveFormat::None };
    int frame_width_{ 0 };
    int frame_height_{ 0 };
    int frame_stride_{ 0 };

    // Shared CUDA stream for all GPU paths (NV12 / YUY2 / RGB32 / MJPG).
    cudaStream_t gpu_stream_{ nullptr };

    // NPP stream context filled in once when gpu_stream_ is created. NPP 12
    // dropped the old nppSetStream() global API, so every NPP call goes
    // through a _Ctx variant that takes this struct.
    NppStreamContext npp_ctx_{};

    // MJPG-specific state (only allocated when MJPG is the active format).
    std::unique_ptr<capture::GpuJpegDecoder> gpu_decoder_;
    uint8_t* pinned_jpeg_buffer_{ nullptr };
    size_t pinned_jpeg_capacity_{ 0 };

    static constexpr int MAX_QUEUE_SIZE = 5;
};

#endif // CAPTURE_CARD_H
