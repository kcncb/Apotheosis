#ifndef APOTHEOSIS_AVERMEDIA_CAPTURE_H
#define APOTHEOSIS_AVERMEDIA_CAPTURE_H

// 圆刚 (AVerMedia) 采集卡的 IScreenCapture 实现 — 走 AVerCapAPI SDK 原生路径。
//
// 用户 UI 上选 OpenCV 或 MF 模式时,capture.cpp 的工厂分发先 probe 设备名字,
// 命中圆刚关键字且 SDK 可用就改实例化本类,否则回退到原 OpenCVCapture / MFCapture。
//
// 输出与 OpenCVCapture 兼容: 同样的 out_side × out_side BGR 方图,启用 crop 时
// 中心裁出,否则缩放整帧。GPU 路径暂不实现 — 圆刚 USB 卡的瓶颈是 USB 总线/解码,
// SDK 走 BGRA 同步回调已能稳定吃满源帧率,后续如需 zero-copy 可再加一条。

#include "../capture.h"

#include <atomic>
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
                     bool prefer_hdmi_source);

    ~AverMediaCapture() override;

    AverMediaCapture(const AverMediaCapture&) = delete;
    AverMediaCapture& operator=(const AverMediaCapture&) = delete;

    cv::Mat GetNextFrameCpu() override;
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

    int src_width_;
    int src_height_;
    int out_side_;
    bool crop_enabled_;
    int capture_fps_;
    uint32_t device_index_;
    bool prefer_hdmi_source_;

    void* device_handle_{ nullptr };   // AVerMedia DeviceHandle
    bool  streaming_{ false };

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };
    std::atomic<int> source_fps_{ 0 };
    int source_frame_count_{ 0 };
    std::chrono::steady_clock::time_point source_fps_start_;

    // SDK 回调只复制设备共享内存中的最新 ROI，然后立即返回；颜色转换和缩放
    // 放到独立线程，避免回调耗时反向阻塞 SDK 的下一次取帧。
    std::thread transform_thread_;
    std::mutex raw_mutex_;
    std::condition_variable raw_cv_;
    cv::Mat raw_bgra_latest_;
    bool raw_is_cropped_{ false };
    bool has_raw_frame_{ false };

    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    cv::Mat latest_;
    bool has_frame_{ false };
};

#endif // APOTHEOSIS_AVERMEDIA_CAPTURE_H
