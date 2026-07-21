#ifndef APOTHEOSIS_AVERMEDIA_CAPTURE_H
#define APOTHEOSIS_AVERMEDIA_CAPTURE_H

// 圆刚 (AVerMedia) 采集卡的 IScreenCapture 实现 — 走 AVerCapAPI SDK 原生路径。
//
// 仅在 UI 显式选择“圆刚 SDK 采集卡”时启用，不会覆盖用户选择的 MF GPU 路径。
//
// 输出同样是 out_side × out_side BGR 方图；GPU 模式用 NPP/CUDA 转换后直接交给
// TensorRT，CPU 模式保留 OpenCV 兼容路径。

#include "../capture.h"
#include "../../mem/gpu_image.h"

#include <cuda_runtime.h>
#include <npp.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

namespace avermedia { class SdkLoader; struct ApiTable; }

class AverMediaCapture : public IScreenCapture
{
public:
    AverMediaCapture(int src_width,
                     int src_height,
                     int out_side,
                     bool crop_enabled,
                     int capture_fps,
                     uint32_t device_index,
                     bool prefer_hdmi_source,
                     bool gpu_decode);

    ~AverMediaCapture() override;

    AverMediaCapture(const AverMediaCapture&) = delete;
    AverMediaCapture& operator=(const AverMediaCapture&) = delete;

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;
    int  GetSourceFpsEstimate() const override { return source_fps_.load(); }
    bool WaitFrame(int timeoutMs) override;
    bool SupportsEventWait() const override { return true; }

    bool IsOpen() const { return is_open_.load(); }

private:
    // SDK 帧回调,转发到实例方法。
    static void __stdcall FrameThunk(uint8_t* buffer,
                                     uint32_t length,
                                     uint64_t timestamp_100ns,
                                     void* user_ctx);
    void OnFrame(uint8_t* buffer, uint32_t length, uint64_t timestamp_100ns);
    void TransformLoop();

    bool OpenDevice();
    void CloseDevice();
    void TickFps();
    bool EnsureGpuContext();
    bool TransformGpu(const cv::Mat& raw, bool cropped);

    std::atomic<int> src_width_;
    std::atomic<int> src_height_;
    int out_side_;
    bool crop_enabled_;
    int capture_fps_;
    uint32_t device_index_;
    bool prefer_hdmi_source_;
    bool gpu_decode_;

    void* device_handle_{ nullptr };   // AVerMedia DeviceHandle
    bool  streaming_{ false };
    bool  nv12_format_{ false };

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };
    std::atomic<int> source_fps_{ 0 };
    int source_frame_count_{ 0 };
    double source_fps_smoothed_{ 0.0 };
    std::chrono::steady_clock::time_point source_fps_start_;

    // SDK 回调只复制设备共享内存中的最新 ROI，然后立即返回；颜色转换和缩放
    // 放到独立线程，避免回调耗时反向阻塞 SDK 的下一次取帧。
    std::thread transform_thread_;
    std::mutex raw_mutex_;
    std::condition_variable raw_cv_;
    struct RawSlot
    {
        cv::Mat image;
        bool cropped{ false };
        bool busy{ false };
        bool gpu_pending{ false };
        cudaEvent_t gpu_done{ nullptr };
    };
    static constexpr int RAW_POOL_SIZE = 3;
    std::array<RawSlot, RAW_POOL_SIZE> raw_pool_;
    int queued_raw_slot_{ -1 };

    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    cv::Mat latest_;
    GpuImage latest_gpu_;
    bool has_frame_{ false };
    bool has_gpu_frame_{ false };

    cudaStream_t gpu_stream_{ nullptr };
    NppStreamContext npp_ctx_{};
    GpuImage scratch_a_;
    GpuImage scratch_b_;
    GpuImage scratch_full_;
    static constexpr int GPU_POOL_SIZE = 8;
    std::array<GpuImage, GPU_POOL_SIZE> gpu_pool_;
    std::array<cudaEvent_t, GPU_POOL_SIZE> gpu_events_{};
    size_t gpu_pool_index_{ 0 };
};

#endif // APOTHEOSIS_AVERMEDIA_CAPTURE_H
