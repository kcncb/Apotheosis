#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <timeapi.h>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#include "Apotheosis.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "other_tools.h"
#include "udp_capture.h"
#include "tcp_capture.h"
#include "eth_capture.h"
#include "opencv_capture.h"
#include "mf_capture.h"
#include "avermedia/avermedia_capture.h"
#include "runtime/active_hotkey.h"
#include "gpu_color_ops.h"
#include <cuda_runtime.h>
#include "avermedia/avermedia_sdk.h"
#include "capture_utils.h"

// Declared in overlay.h; capture.cpp drives it when a capture-card backend's
// square crop changes the effective detection resolution so the detector
// rebuilds its input geometry.
extern std::atomic<bool> detector_model_changed;

cv::Mat latestFrame;
std::mutex frameMutex;

int screenWidth = 0;
int screenHeight = 0;

std::atomic<int> captureFrameCount(0);
std::atomic<int> captureFps(0);
std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

// Source FPS: counts every frame the capture loop SUCCEEDS at acquiring,
// before any frame-limiter sleep. captureFps measures effective processing
// rate (post-limiter, post-detector). When the source delivers e.g. 120 fps
// and we throttle to 60, captureSourceFps stays ~120 while captureFps shows
// ~60 — the stats panel uses the source value when non-zero so the user can
// tell "true input rate" from "internal processing rate".
std::atomic<int> captureSourceFps(0);
std::atomic<int> captureSourceFrameCount(0);

std::atomic<int> captureSenderSpanFps(0);
std::atomic<int> captureWireLostFps(0);
std::atomic<int> capturePartialLostFps(0);
std::atomic<int> capturePcapKernelDroppedFps(0);
std::atomic<int> capturePcapIfDroppedFps(0);
std::chrono::time_point<std::chrono::high_resolution_clock> captureSourceFpsStartTime;

std::deque<cv::Mat> frameQueue;

namespace
{
std::mutex g_detectionSuppressionMaskMutex;
cv::Mat g_detectionSuppressionMask;
// Fast-path flag: lets readers skip the lock entirely when no mask is live.
// Common case (depth_inference disabled) hits this on every detection frame.
std::atomic<bool> g_detectionSuppressionMaskHasData{false};
}

static void UpdateDetectionSuppressionMask(const cv::Mat& mask)
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    if (!mask.empty() && mask.type() == CV_8UC1)
    {
        g_detectionSuppressionMask = mask.clone();
        g_detectionSuppressionMaskHasData.store(true, std::memory_order_release);
    }
    else
    {
        g_detectionSuppressionMask.release();
        g_detectionSuppressionMaskHasData.store(false, std::memory_order_release);
    }
}

cv::Mat getCurrentDetectionSuppressionMask()
{
    // Lock-free fast path: no mask, no work.
    if (!g_detectionSuppressionMaskHasData.load(std::memory_order_acquire))
        return cv::Mat();

    // Shallow copy under the lock — cv::Mat is refcounted so the caller's
    // reference keeps the pixel buffer alive even after the storage is
    // replaced by the next UpdateDetectionSuppressionMask(). Saves a per-frame
    // O(W*H) clone in the inference and mouse threads.
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    return g_detectionSuppressionMask;
}

namespace
{
struct CaptureThreadConfig
{
    std::string capture_method;
    int capture_fps = 0;
    int detection_resolution = 0;
    bool circle_mask = false;
    std::string udp_ip;
    int udp_port = 0;
    std::string tcp_ip;
    int tcp_port = 0;
    std::string eth_adapter;
    int eth_ethertype = 0x88B5;
    int opencv_capture_index = 0;
    std::string opencv_capture_api;
    std::string opencv_capture_url;
    int opencv_capture_width = 0;
    int opencv_capture_height = 0;
    int opencv_capture_fps = 0;
    int capture_crop = 0;
    std::string capture_format;
    bool capture_mf_gpu = true;
    std::string backend;
    std::vector<std::string> screenshot_button;
    int screenshot_delay = 0;
    bool show_window = false;
    bool verbose = false;
    bool depth_inference_enabled = false;
    bool depth_show_heatmap = false;
    bool depth_mask_enabled = false;
    std::string depth_model_path;
    int depth_mask_fps = 0;
    int depth_mask_near_percent = 0;
    int depth_mask_expand = 0;
    bool depth_mask_invert = false;
    int depth_colormap = 18;
    int depth_opt_input_size = 224;
    float depth_heatmap_gamma = 1.0f;
    float depth_norm_clip_low_pct = 0.0f;
    float depth_norm_clip_high_pct = 100.0f;
    bool depth_show_bbox_distance = false;
};

CaptureThreadConfig SnapshotCaptureConfig()
{
    std::lock_guard<std::recursive_mutex> cfgLock(configMutex);
    CaptureThreadConfig snapshot;
    snapshot.capture_method = config.capture_method;
    snapshot.capture_fps = config.capture_fps;
    snapshot.detection_resolution = config.detection_resolution;
    snapshot.circle_mask = config.circle_mask;
    snapshot.udp_ip = config.udp_ip;
    snapshot.udp_port = config.udp_port;
    snapshot.tcp_ip = config.tcp_ip;
    snapshot.tcp_port = config.tcp_port;
    snapshot.eth_adapter = config.eth_adapter;
    snapshot.eth_ethertype = config.eth_ethertype;
    snapshot.opencv_capture_index = config.opencv_capture_index;
    snapshot.opencv_capture_api = config.opencv_capture_api;
    snapshot.opencv_capture_url = config.opencv_capture_url;
    snapshot.opencv_capture_width = config.opencv_capture_width;
    snapshot.opencv_capture_height = config.opencv_capture_height;
    snapshot.opencv_capture_fps = config.opencv_capture_fps;
    snapshot.capture_crop = config.capture_crop;
    snapshot.capture_format = config.capture_format;
    snapshot.capture_mf_gpu = config.capture_mf_gpu;
    snapshot.backend = config.backend;
    snapshot.screenshot_button = config.screenshot_button;
    snapshot.screenshot_delay = config.screenshot_delay;
    snapshot.show_window = config.show_window;
    snapshot.verbose = config.verbose;
    snapshot.depth_inference_enabled = config.depth_inference_enabled;
    snapshot.depth_show_heatmap = config.depth_show_heatmap;
    snapshot.depth_mask_enabled = config.depth_mask_enabled;
    snapshot.depth_model_path = config.depth_model_path;
    snapshot.depth_mask_fps = config.depth_mask_fps;
    snapshot.depth_mask_near_percent = config.depth_mask_near_percent;
    snapshot.depth_mask_expand = config.depth_mask_expand;
    snapshot.depth_mask_invert = config.depth_mask_invert;
    snapshot.depth_colormap = config.depth_colormap;
    snapshot.depth_opt_input_size = config.depth_opt_input_size;
    snapshot.depth_heatmap_gamma = config.depth_heatmap_gamma;
    snapshot.depth_norm_clip_low_pct  = config.depth_norm_clip_low_pct;
    snapshot.depth_norm_clip_high_pct = config.depth_norm_clip_high_pct;
    snapshot.depth_show_bbox_distance = config.depth_show_bbox_distance;
    return snapshot;
}

std::string NormalizeCaptureMethod(const std::string& method)
{
    // 旧版本持久化的采集卡后端迁移到当前两套实现:
    //   裸 capture_card / _cv / _ds -> opencv_capture
    //   capture_card_mf            -> mf_capture(自写 Media Foundation)
    if (method == "capture_card"
        || method == "capture_card_cv"
        || method == "capture_card_ds")
        return "opencv_capture";
    if (method == "capture_card_mf")
        return "mf_capture";
    if (method == "udp_capture"
        || method == "tcp_capture"
        || method == "eth_capture"
        || method == "opencv_capture"
        || method == "mf_capture")
        return method;
    return "udp_capture";
}

class TimerResolutionGuard
{
public:
    void Enable()
    {
        if (!enabled_)
        {
            timeBeginPeriod(1);
            enabled_ = true;
        }
    }

    void Disable()
    {
        if (enabled_)
        {
            timeEndPeriod(1);
            enabled_ = false;
        }
    }

    ~TimerResolutionGuard()
    {
        Disable();
    }

private:
    bool enabled_{ false };
};

// 高精度等待器: 使用 Windows 10 1803+ 引入的高精度可等待计时器
// (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,100ns 粒度),配合极小的 busy-spin
// 收尾,把每帧的睡眠误差从 std::this_thread::sleep_until 的 ~1-2ms 降到
// ~50us 以内。是修复"capture_fps=240 实际跑 200"那个上限不准 bug 的核心。
//
// 旧版用 sleep_until + 1500us spin margin。Windows 调度器即使开了 1ms 计时
// 分辨率仍会过冲 1-2ms,在 4-5ms 的周期上等于 20-30% 误差,直接把 capture_fps
// 的上限值打偏。换成 high-res 计时器后,SetWaitableTimer 的硬件回调精度足以
// 让 200us 的 spin 兜底就能命中目标时刻。
//
// 不支持高精度标志的旧系统会回退到普通 waitable timer (~1ms 粒度);极旧的
// 系统再回退到 std::this_thread::sleep_until。任何分支都不会阻塞主循环。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class PreciseSleeper
{
public:
    PreciseSleeper()
    {
        // 先尝试高精度计时器 (Win10 1803+)。失败再退到普通 waitable timer。
        timer_ = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
            TIMER_ALL_ACCESS);
        if (!timer_)
        {
            timer_ = CreateWaitableTimerExW(
                nullptr, nullptr,
                CREATE_WAITABLE_TIMER_MANUAL_RESET,
                TIMER_ALL_ACCESS);
        }
    }

    ~PreciseSleeper()
    {
        if (timer_) CloseHandle(timer_);
    }

    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    bool sleep_until(std::chrono::steady_clock::time_point tp)
    {
        if (!timer_) return false;
        auto now = std::chrono::steady_clock::now();
        if (now >= tp) return true;
        const long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(tp - now).count();
        // SetWaitableTimer 用 100ns 单位,负值=相对时间。
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);
        if (due.QuadPart == 0) due.QuadPart = -1; // 至少睡一个 tick
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            return false;
        WaitForSingleObject(timer_, INFINITE);
        return true;
    }

private:
    HANDLE timer_{ nullptr };
};

class ScreenshotWriter
{
public:
    ScreenshotWriter()
    {
        writerThread_ = std::thread([this]() { Run(); });
    }

    ~ScreenshotWriter()
    {
        Stop();
    }

    void Enqueue(const std::string& filename, cv::Mat frame)
    {
        if (filename.empty() || frame.empty())
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= maxPendingFrames_)
            queue_.pop();
        queue_.emplace(filename, std::move(frame));
        cv_.notify_one();
    }

private:
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();

        if (writerThread_.joinable())
            writerThread_.join();
    }

    void Run()
    {
        std::error_code ec;
        std::filesystem::create_directories("screenshots", ec);

        while (true)
        {
            std::pair<std::string, cv::Mat> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    break;

                job = std::move(queue_.front());
                queue_.pop();
            }

            try
            {
                const std::filesystem::path outputPath = std::filesystem::path("screenshots") / job.first;
                cv::imwrite(outputPath.string(), job.second);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Screenshot save failed: " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[Capture] Screenshot save failed: unknown exception." << std::endl;
            }
        }
    }

private:
    static constexpr size_t maxPendingFrames_ = 8;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<std::string, cv::Mat>> queue_;
    std::thread writerThread_;
    bool stop_{ false };
};

// 把 GPU→Host 的 D2H + circle_mask 回填(纯 CPU 时) + latestFrame 更新 +
// 准星颜色检测 整段从 capture 主循环搬到独立线程。capture loop 只 push 一份
// GpuImage 引用 (storage 是 shared_ptr,detector 同时持有另一份引用,buffer
// 安全);worker 自己 cudaEventSynchronize + cudaMemcpy2D,主循环完全不被
// PCIe Gen1 的 D2H 阻塞,也不被 crosshair_runtime::process_frame 的 CPU 工作
// 阻塞。队列容量 1,新一帧到来时旧帧被丢——preview 60Hz 渲染本来就不需要
// 链路里所有的 240fps,worker 跟不上时丢 stale 帧是正确的低延迟策略。
class HostCopyWorker
{
public:
    HostCopyWorker()
    {
        thread_ = std::thread([this]() { Run(); });
    }

    ~HostCopyWorker()
    {
        Stop();
    }

    // 把一帧的 GpuImage 引用入队;capture loop 调用,完全非阻塞。
    void Submit(GpuImage gpu, bool needCrosshair)
    {
        if (gpu.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stop_) return;
            // 始终只保留最新一帧——队列里有积压时丢旧的。
            pending_ = std::move(gpu);
            pendingCrosshair_ = needCrosshair;
            hasPending_ = true;
        }
        cv_.notify_one();
    }

private:
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    void Run()
    {
        while (true)
        {
            GpuImage gpu;
            bool needCrosshair = false;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]() { return stop_ || hasPending_; });
                if (stop_ && !hasPending_) break;
                gpu = std::move(pending_);
                pending_.release();
                needCrosshair = pendingCrosshair_;
                hasPending_ = false;
            }
            if (gpu.empty()) continue;

            try
            {
                // download 内部已 cudaEventSynchronize(ready_event_) + 同步
                // cudaMemcpy2D,worker 自己阻塞、不影响 capture loop。
                cv::Mat host;
                gpu.download(host);
                gpu.release(); // 尽早释放 GPU 引用,detector 那条引用还在,
                               // 但本线程持有就可能延后 buffer 复用。

                if (host.empty()) continue;

                {
                    std::lock_guard<std::mutex> lk(frameMutex);
                    latestFrame = host;
                    if (frameQueue.size() >= 1)
                        frameQueue.pop_front();
                    frameQueue.push_back(latestFrame);
                }
                frameCV.notify_one();

                if (needCrosshair)
                    crosshair_runtime::process_frame(host);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[HostCopyWorker] " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[HostCopyWorker] unknown exception" << std::endl;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    GpuImage pending_;
    bool pendingCrosshair_{ false };
    bool hasPending_{ false };
    bool stop_{ false };
    std::thread thread_;
};
} // namespace

std::vector<cv::Mat> getBatchFromQueue(int batch_size)
{
    std::vector<cv::Mat> batch;
    std::lock_guard<std::mutex> lk(frameMutex);
    const size_t target_size = (batch_size > 0) ? static_cast<size_t>(batch_size) : 0;
    const size_t n = std::min(frameQueue.size(), target_size);

    for (size_t i = 0; i < n; ++i)
        batch.push_back(frameQueue[frameQueue.size() - n + i]);

    while (batch.size() < target_size && !batch.empty())
        batch.push_back(batch.back().clone());
    return batch;
}

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT)
{
    try
    {
        CaptureThreadConfig currentCfg = SnapshotCaptureConfig();
        if (currentCfg.verbose)
            std::cout << "[Capture] OpenCV version: " << CV_VERSION << std::endl;

        int captureWidth = std::max(1, CAPTURE_WIDTH);
        int captureHeight = std::max(1, CAPTURE_HEIGHT);
        if (currentCfg.detection_resolution > 0)
        {
            captureWidth = currentCfg.detection_resolution;
            captureHeight = currentCfg.detection_resolution;
        }

        depth_anything::DepthAnythingTrt depthMaskFallbackModel;
        std::string depthMaskFallbackModelPath;

        auto createCapturer = [&](const CaptureThreadConfig& cfg, int width, int height) -> std::unique_ptr<IScreenCapture>
        {
            try
            {
                const std::string method = NormalizeCaptureMethod(cfg.capture_method);

                if (method == "tcp_capture")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using TCP capture" << std::endl;
                    return std::make_unique<TCPCapture>(width, height, cfg.tcp_ip, cfg.tcp_port);
                }

                if (method == "eth_capture")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using ETH (ProSexy raw L2) capture" << std::endl;
                    return std::make_unique<EthCapture>(width, height, cfg.eth_adapter, cfg.eth_ethertype);
                }

                if (method == "opencv_capture" || method == "mf_capture")
                {
                    // The square crop, when enabled, IS the inference frame and
                    // therefore drives detection_resolution so the detector input
                    // and the mouse/overlay coordinate space (which assume a
                    // square detection_resolution) stay aligned. Crop disabled =>
                    // fall back to scaling the whole frame to detection_resolution.
                    bool crop_enabled = cfg.capture_crop > 0;
                    int out_side = crop_enabled ? cfg.capture_crop : std::max(1, cfg.detection_resolution);
                    if (crop_enabled)
                    {
                        std::lock_guard<std::recursive_mutex> cfgLock(configMutex);
                        if (config.detection_resolution != out_side)
                        {
                            config.detection_resolution = out_side;
                            detector_model_changed.store(true);
                        }
                    }

                    // 圆刚 (AVerMedia) 采集卡探测: 用户在 UI 上选 opencv/mf,但若
                    // 设备名命中圆刚关键字且 AVerCapAPI*.dll 已加载,就改走 SDK 原生
                    // 路径。SDK 缺失 / 设备不是圆刚 / probe 失败 -> 走下方原 OpenCV/MF
                    // 分支,无副作用。
                    {
                        auto& aver = avermedia::SdkLoader::Instance();
                        if (aver.IsUsable() && aver.Api().GetDeviceFriendlyName)
                        {
                            char name[256] = { 0 };
                            const uint32_t rc = aver.Api().GetDeviceFriendlyName(
                                static_cast<uint32_t>(cfg.opencv_capture_index),
                                name, sizeof(name));
                            const std::string nameStr(name);
                            if (rc == avermedia::AVER_ERR_SUCCESS
                                && avermedia::IsAverMediaFriendlyName(nameStr))
                            {
                                if (cfg.verbose)
                                    std::cout << "[Capture] AVerMedia device detected ("
                                              << nameStr << "), using SDK path." << std::endl;
                                return std::make_unique<AverMediaCapture>(
                                    cfg.opencv_capture_width,
                                    cfg.opencv_capture_height,
                                    out_side,
                                    crop_enabled,
                                    cfg.opencv_capture_fps,
                                    static_cast<uint32_t>(cfg.opencv_capture_index),
                                    /*prefer_hdmi_source=*/true);
                            }
                        }
                    }

                    if (method == "mf_capture")
                    {
                        if (cfg.verbose)
                            std::cout << "[Capture] Using MF capture card (index="
                                      << cfg.opencv_capture_index << ", fmt="
                                      << cfg.capture_format << ", "
                                      << (cfg.capture_mf_gpu ? "GPU" : "CPU") << ")" << std::endl;
                        return std::make_unique<MFCapture>(
                            cfg.opencv_capture_width,
                            cfg.opencv_capture_height,
                            out_side,
                            crop_enabled,
                            cfg.opencv_capture_fps,
                            cfg.capture_format,
                            cfg.opencv_capture_index,
                            cfg.capture_mf_gpu);
                    }

                    if (cfg.verbose)
                        std::cout << "[Capture] Using OpenCV capture card (index="
                                  << cfg.opencv_capture_index << ", api="
                                  << cfg.opencv_capture_api << ", fmt="
                                  << cfg.capture_format << ")" << std::endl;
                    return std::make_unique<OpenCVCapture>(
                        cfg.opencv_capture_width,
                        cfg.opencv_capture_height,
                        out_side,
                        crop_enabled,
                        cfg.opencv_capture_fps,
                        cfg.capture_format,
                        cfg.opencv_capture_index,
                        cfg.opencv_capture_api,
                        cfg.opencv_capture_url);
                }

                if (cfg.verbose)
                    std::cout << "[Capture] Using UDP capture" << std::endl;
                return std::make_unique<UDPCapture>(width, height, cfg.udp_ip, cfg.udp_port);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Failed to initialize '" << cfg.capture_method
                    << "' capture: " << e.what() << std::endl;
                return nullptr;
            }
        };

        std::unique_ptr<IScreenCapture> capturer = createCapturer(currentCfg, captureWidth, captureHeight);
        auto lastCapturerCreateAttempt = std::chrono::steady_clock::now();

        auto clearCaptureFrames = [&]()
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame.release();
            frameQueue.clear();
        };

        auto clearDetections = [&]()
        {
            std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.boxes.clear();
            detectionBuffer.classes.clear();
            detectionBuffer.confidences.clear();
            detectionBuffer.bumpVersionLocked();
            detectionBuffer.cv.notify_all();
        };

        auto markCaptureUnavailable = [&]()
        {
            clearCaptureFrames();
            clearDetections();
            frameCV.notify_one();
        };

        bool captureUnavailable = false;
        auto setCaptureUnavailable = [&]()
        {
            if (captureUnavailable)
                return;
            captureUnavailable = true;
            markCaptureUnavailable();
        };
        auto setCaptureAvailable = [&]()
        {
            captureUnavailable = false;
        };

        setCaptureUnavailable();

        TimerResolutionGuard timerResolution;
        PreciseSleeper preciseSleeper;
        std::optional<std::chrono::steady_clock::duration> frameDuration;
        auto updateFrameDuration = [&](int captureFpsSetting)
        {
            if (captureFpsSetting > 0)
            {
                timerResolution.Enable();
                const auto frameMs = std::chrono::duration<double, std::milli>(1000.0 / captureFpsSetting);
                frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameMs);
            }
            else
            {
                timerResolution.Disable();
                frameDuration.reset();
            }
        };
        updateFrameDuration(currentCfg.capture_fps);

        captureFpsStartTime = std::chrono::high_resolution_clock::now();
        captureSourceFpsStartTime = captureFpsStartTime;

        auto frameStartTime = std::chrono::steady_clock::now();
        auto applyFrameLimiter = [&]()
        {
            if (frameDuration.has_value())
            {
                // 锚点是"上一帧目标 + 一个 frameDuration",不是"睡眠后的 now"。
                // 把每帧的过冲放到下一帧的预算里抵消,长期均值不漂。
                const auto target = frameStartTime + frameDuration.value();
                const auto now = std::chrono::steady_clock::now();
                if (now < target)
                {
                    // 用高精度可等待计时器睡到目标前 ~200us,再 busy-spin 收尾。
                    // 旧实现用 std::this_thread::sleep_until + 1.5ms spin,
                    // sleep_until 在 Windows 上即便开了 1ms 计时分辨率仍会过冲
                    // ~1-2ms;在 4-5ms 的小周期上等于 20-30% 误差——这是
                    // "capture_fps=240 实际跑 200"那个上限不准 bug 的直接成因。
                    // 改用 SetWaitableTimer 的高精度路径后误差降到 ~50us 量级,
                    // 200us 的 busy-spin 足以兜底,周期精度落到 ~100us 以内。
                    constexpr auto kSpinMargin = std::chrono::microseconds(200);
                    if (target - now > kSpinMargin)
                    {
                        if (!preciseSleeper.sleep_until(target - kSpinMargin))
                        {
                            // 极旧系统兜底:回退到原 std::this_thread 路径。
                            std::this_thread::sleep_until(target - kSpinMargin);
                        }
                    }
                    while (std::chrono::steady_clock::now() < target)
                    {
                        // busy-wait the sub-ms remainder
                    }
                }
                frameStartTime = target;
                // 若整轮严重落后(detector 卡住、采集卡 stall 等),把锚点抢
                // 一拍到 now,避免之后用一连串无 sleep 的迭代追帧,瞬间挤爆下游。
                const auto post = std::chrono::steady_clock::now();
                if (post - frameStartTime > frameDuration.value() * 4)
                    frameStartTime = post;
            }
            else
            {
                frameStartTime = std::chrono::steady_clock::now();
            }
        };

        ScreenshotWriter screenshotWriter;
        auto lastSaveTime = std::chrono::steady_clock::now();
        auto lastSuccessfulFrameTime = std::chrono::steady_clock::now();
        constexpr auto staleFrameTimeout = std::chrono::milliseconds(500);

        // Preview-CPU 下载节流时间戳。show_window 只是给 ImGui/preview_window
        // 看的,后者本身大约 60Hz 渲染节奏,没必要在 240fps 采集环里每帧都同步
        // 下载到 host——CMP 40HX 是 PCIe Gen1 卡,每帧 ~500KB D2H 加上事件
        // 同步要 1-2ms,直接把 capture 循环周期撑到 5-6ms 上,这就是"采集到
        // 推理"链路目前只有 180fps 的主要原因。这里把"仅 preview 需要"那条
        // 路径节流到 ~60fps,严格消费者(depth / DML detector / screenshot /
        // 准星检测激活时)仍然每帧下载,保证功能正确。
        //
        // 用 needFirstPreviewDownload 标记首帧必须下载,而不是把时间戳初始化
        // 为 time_point::min()——后者会让 `now - min()` 在 int64 ns 上溢出,
        // 导致第一次比较出非预期负值,preview 永远拿不到首帧,黑屏。
        auto lastPreviewSubmitTime = std::chrono::steady_clock::now();
        bool needFirstPreviewSubmit = true;
        constexpr auto kPreviewSubmitInterval = std::chrono::microseconds(16000); // ~60fps

        // 采集线程自用的 CUDA 工作流。circle_mask 等 in-place GPU 操作排在这条
        // 流上,通过 event 与 NVDEC 解码流串联——非阻塞、不接触 default stream,
        // 避免和 detector 推理流互相 implicit-sync。RAII 守卫负责退出时释放。
        struct CaptureCudaGuard
        {
            cudaStream_t stream{ nullptr };
            cudaEvent_t  maskEvent{ nullptr };
            CaptureCudaGuard()
            {
                cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
                cudaEventCreateWithFlags(&maskEvent, cudaEventDisableTiming);
            }
            ~CaptureCudaGuard()
            {
                if (maskEvent) cudaEventDestroy(maskEvent);
                if (stream)    cudaStreamDestroy(stream);
            }
        };
        CaptureCudaGuard captureCuda;

        // 异步 host 副本生产者。capture 主循环 Submit() 一份 GpuImage 引用就
        // 立刻返回,worker 独立做 D2H + latestFrame 刷新 + 准星检测。这样主循环
        // 完全没有 cudaEventSynchronize / cudaMemcpy2D / cv::Mat::copyTo 这些
        // host-blocking 操作,PCIe Gen1 的 D2H 也不会再压低 240fps 链路。
        HostCopyWorker hostCopyWorker;

        while (!shouldExit && !session_stop_requested.load())
        {
            try
            {
                currentCfg = SnapshotCaptureConfig();

            if (capture_fps_changed.exchange(false))
            {
                updateFrameDuration(currentCfg.capture_fps);
                // 事件驱动后端(MF)跳过 limiter,改由后端在解码前按此上限丢帧降采样。
                if (capturer) capturer->SetTargetFps(currentCfg.capture_fps);
            }

            const bool needsReinit =
                detection_resolution_changed.exchange(false) ||
                capture_method_changed.exchange(false);

            if (needsReinit)
            {
                setCaptureUnavailable();

                if (currentCfg.detection_resolution > 0)
                {
                    captureWidth = currentCfg.detection_resolution;
                    captureHeight = currentCfg.detection_resolution;
                }

                capturer.reset();
                capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                lastCapturerCreateAttempt = std::chrono::steady_clock::now();
                if (currentCfg.verbose)
                    std::cout << "[Capture] Reinitialized capture backend." << std::endl;
            }

            if (!capturer)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastCapturerCreateAttempt >= std::chrono::seconds(1))
                {
                    capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                    lastCapturerCreateAttempt = now;

                    if (capturer)
                    {
                        lastSuccessfulFrameTime = now;
                        if (currentCfg.verbose)
                            std::cout << "[Capture] Capture backend recovered." << std::endl;
                    }
                }

                setCaptureUnavailable();
                if (!frameDuration.has_value())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                applyFrameLimiter();
                continue;
            }

            const bool screenshotEnabled =
                !currentCfg.screenshot_button.empty() && currentCfg.screenshot_button[0] != "None";
            const auto screenshotNow = std::chrono::steady_clock::now();
            const auto screenshotElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                screenshotNow - lastSaveTime
            ).count();
            const bool screenshotRequested =
                screenshotEnabled &&
                isAnyKeyPressed(currentCfg.screenshot_button) &&
                screenshotElapsedMs >= currentCfg.screenshot_delay;

            cv::Mat screenshotCpu;
            cv::Mat detectionFrame;

            // Threat scoring (set by the mouse thread) also consumes the
            // normalized depth map, so it must keep depth inference alive even
            // when no depth display option is on — otherwise the depth term in
            // compute_threat_score silently falls back to a neutral score.
            const bool threatDepthRequired = g_threat_depth_required.load();
            const bool depthNeeded =
                currentCfg.depth_inference_enabled
                && (currentCfg.depth_mask_enabled
                    || currentCfg.depth_show_heatmap
                    || currentCfg.depth_show_bbox_distance
                    || threatDepthRequired);

            static bool lastDepthInferenceEnabled = true;
            if (!depthNeeded)
            {
                if (lastDepthInferenceEnabled)
                {
                    auto& depthMask = depth_anything::GetDepthMaskGenerator();
                    depthMask.reset();
                    depthMaskFallbackModel.reset();
                    depthMaskFallbackModelPath.clear();
                }
                UpdateDetectionSuppressionMask(cv::Mat());
                lastDepthInferenceEnabled = false;
            }
            else
            {
                lastDepthInferenceEnabled = true;
            }

            // Prefer the zero-copy GPU path (nvJPEG output). If the backend
            // doesn't produce GPU frames or the per-iteration GPU queue is
            // empty, fall through to the CPU queue.
            GpuImage screenshotGpu = capturer->GetNextFrameGpu();
            bool freshCpuFrameThisIter = false;
            bool gpuMaskApplied = false;

            // circle_mask 下推 GPU:在解码 BGR 之后、D2H/detector 之前 in-place
            // 跑一个圆形掩码 kernel,detector 直接吃 masked GpuImage,后续 D2H
            // 拷回 host 的也是 masked 副本——CPU 完全不再需要重做 cv::Mat 拷贝。
            // 工作排在 captureCuda.stream 上,先 cudaStreamWaitEvent 串解码事件,
            // 再 cudaEventRecord 出新的 readyEvent 给 detector 同步,保持非阻塞。
            if (currentCfg.circle_mask && !screenshotGpu.empty())
            {
                cudaEvent_t srcEvent = screenshotGpu.readyEvent();
                if (srcEvent)
                    cudaStreamWaitEvent(captureCuda.stream, srcEvent, 0);
                launch_circle_mask_bgr_u8(
                    screenshotGpu.data(), screenshotGpu.step(),
                    screenshotGpu.cols(), screenshotGpu.rows(),
                    captureCuda.stream);
                cudaEventRecord(captureCuda.maskEvent, captureCuda.stream);
                screenshotGpu.setReadyEvent(captureCuda.maskEvent);
                gpuMaskApplied = true;
            }

            // 消费者分两档:
            //   strict — 必须主循环里立刻拿到 host 副本: DML detector(GPU 路径
            //   不支持)、depth 推理/掩码、本帧截图保存。这些路径本来就在主循环
            //   后面用 screenshotCpu,挪不到 worker。
            //   async — preview show_window + 准星颜色检测(active hotkey 时)。
            //   这两个不在 detector / 主路径上,完全甩给 HostCopyWorker 去做 D2H。
            //   crosshair 现在跑在 worker 而不是 capture 线程,瞄准时不会再用
            //   crosshair 的 cv 工作量阻塞 240fps 链路。
            const bool detectorNeedsCpu = !g_detector
                || g_detector->backend() != DetectorBackend::TensorRT;
            const bool needCpuStrict = depthNeeded
                || screenshotRequested
                || detectorNeedsCpu;
            const bool crosshairActive =
                (runtime::g_active_hotkey_index.load() >= 0);
            const bool needCpuAsync = currentCfg.show_window || crosshairActive;

            if (!screenshotGpu.empty())
            {
                if (needCpuStrict)
                {
                    // 主循环必须同步等 host 副本,接受这部分阻塞。
                    screenshotGpu.download(screenshotCpu);
                    freshCpuFrameThisIter = true;
                }
                if (needCpuAsync)
                {
                    // preview 节流到 ~60fps;准星检测 active 时按 active 节流到
                    // ~120fps(给手感留点,即便 GPU mask kernel 没完成 worker
                    // 等就是了,这部分等待在 worker 线程,不挤占主循环)。
                    const auto now = std::chrono::steady_clock::now();
                    auto interval = kPreviewSubmitInterval;
                    if (crosshairActive)
                        interval = std::chrono::microseconds(8000); // ~120fps
                    if (needFirstPreviewSubmit
                        || now - lastPreviewSubmitTime >= interval)
                    {
                        // 共享引用给 worker;detector 拿原引用 std::move 走也没
                        // 关系,storage 是 shared_ptr。
                        hostCopyWorker.Submit(screenshotGpu, crosshairActive);
                        lastPreviewSubmitTime = now;
                        needFirstPreviewSubmit = false;
                    }
                }
            }
            else
            {
                screenshotCpu = capturer->GetNextFrameCpu();
                freshCpuFrameThisIter = !screenshotCpu.empty();
            }

            if (screenshotGpu.empty() && screenshotCpu.empty())
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                    setCaptureUnavailable();

                // 事件驱动:等后端产帧唤醒后立即重试取帧,而不是睡满一个 limiter
                // 节拍。固定节拍会和产帧时钟相位漂移而踏空,在队列容量 1 下还会把
                // 那一帧覆盖丢掉,从而把帧率压到源帧率以下。这里不再调用
                // applyFrameLimiter——空帧没有产出,不该占用一个限流节拍。
                // WaitFrame 返回 false(后端不支持事件等待)时回退到 1ms 短睡。
                if (!capturer->WaitFrame(4))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // GPU 路径已经在 GpuImage 上 in-place 做了 circle_mask,D2H 拿到的
            // 已经是 masked 副本;只有走纯 CPU 后端(没有 GpuImage)时才需要在
            // host 重做一次 OpenCV 圆形掩码。gpuMaskApplied 守卫避免重复 mask。
            if (currentCfg.circle_mask && !gpuMaskApplied && !screenshotCpu.empty())
                screenshotCpu = apply_circle_mask(screenshotCpu);

            detectionFrame = screenshotCpu;
            if (depthNeeded)
            {
                cv::Mat mask;
                depth_anything::DepthMaskOptions maskOptions;
                maskOptions.enabled = currentCfg.depth_mask_enabled;
                maskOptions.fps = currentCfg.depth_mask_fps;
                maskOptions.near_percent = currentCfg.depth_mask_near_percent;
                maskOptions.expand = currentCfg.depth_mask_expand;
                maskOptions.invert = currentCfg.depth_mask_invert;
                maskOptions.produce_colormap = currentCfg.depth_show_heatmap;
                maskOptions.produce_normalized = currentCfg.depth_show_bbox_distance
                    || threatDepthRequired;
                maskOptions.colormap_type = currentCfg.depth_colormap;
                maskOptions.opt_input_size = currentCfg.depth_opt_input_size;
                maskOptions.heatmap_gamma = currentCfg.depth_heatmap_gamma;
                maskOptions.norm_low_pct  = currentCfg.depth_norm_clip_low_pct;
                maskOptions.norm_high_pct = currentCfg.depth_norm_clip_high_pct;

                auto& depthMask = depth_anything::GetDepthMaskGenerator();
                depthMask.update(screenshotCpu, maskOptions, currentCfg.depth_model_path, gLogger);
                mask = currentCfg.depth_mask_enabled ? depthMask.getMask() : cv::Mat();

                if (!mask.empty() && mask.size() != screenshotCpu.size())
                    mask.release();

                if (mask.empty() && currentCfg.depth_mask_enabled)
                {
                    if (currentCfg.depth_model_path.empty())
                    {
                        if (depthMaskFallbackModel.ready())
                            depthMaskFallbackModel.reset();
                        depthMaskFallbackModelPath.clear();
                    }
                    else if (depthMaskFallbackModelPath != currentCfg.depth_model_path || !depthMaskFallbackModel.ready())
                    {
                        depthMaskFallbackModel.setOptInputSize(currentCfg.depth_opt_input_size);
                        depthMaskFallbackModel.setNormPercentiles(
                            currentCfg.depth_norm_clip_low_pct,
                            currentCfg.depth_norm_clip_high_pct);
                        if (depthMaskFallbackModel.initialize(currentCfg.depth_model_path, gLogger))
                        {
                            depthMaskFallbackModelPath = currentCfg.depth_model_path;
                        }
                    }

                    if (depthMaskFallbackModel.ready())
                    {
                        depthMaskFallbackModel.setNormPercentiles(
                            currentCfg.depth_norm_clip_low_pct,
                            currentCfg.depth_norm_clip_high_pct);
                        cv::Mat depthLocal = depthMaskFallbackModel.predictDepth(screenshotCpu);
                        if (!depthLocal.empty())
                        {
                            const int nearPercent = std::clamp(currentCfg.depth_mask_near_percent, 1, 100);
                            const bool invertMask = currentCfg.depth_mask_invert;
                            const int total = depthLocal.rows * depthLocal.cols;
                            if (total > 0)
                            {
                                int hist[256] = {};
                                for (int y = 0; y < depthLocal.rows; ++y)
                                {
                                    const uint8_t* row = depthLocal.ptr<uint8_t>(y);
                                    for (int x = 0; x < depthLocal.cols; ++x)
                                        hist[row[x]]++;
                                }

                                const int target = std::max(1, (total * nearPercent) / 100);
                                int threshold = 0;
                                if (!invertMask)
                                {
                                    int count = 0;
                                    for (int i = 0; i < 256; ++i)
                                    {
                                        count += hist[i];
                                        if (count >= target)
                                        {
                                            threshold = i;
                                            break;
                                        }
                                    }
                                    cv::compare(depthLocal, threshold, mask, cv::CMP_LE);
                                }
                                else
                                {
                                    int count = 0;
                                    for (int i = 255; i >= 0; --i)
                                    {
                                        count += hist[i];
                                        if (count >= target)
                                        {
                                            threshold = i;
                                            break;
                                        }
                                    }
                                    cv::compare(depthLocal, threshold, mask, cv::CMP_GE);
                                }

                                const int expand = std::clamp(currentCfg.depth_mask_expand, 0, 128);
                                if (expand > 0)
                                {
                                    const int kernelSize = 2 * expand + 1;
                                    cv::Mat kernel = cv::getStructuringElement(
                                        cv::MORPH_ELLIPSE, cv::Size(kernelSize, kernelSize));
                                    cv::dilate(mask, mask, kernel);
                                }
                            }
                        }
                    }
                }

                UpdateDetectionSuppressionMask(mask);
                if (!mask.empty() && mask.size() == screenshotCpu.size())
                {
                    detectionFrame = screenshotCpu.clone();
                    detectionFrame.setTo(cv::Scalar(0, 0, 0), mask);
                }
            }
            else
            {
                UpdateDetectionSuppressionMask(cv::Mat());
            }

            if (g_detector)
            {
                // Zero-copy GPU path when nvJPEG produced a GpuMat and no
                // CPU-only transform (depth mask suppression) has rewritten
                // detectionFrame on the host.
                const bool usedCpuDetectionOverride =
                    currentCfg.depth_inference_enabled && currentCfg.depth_mask_enabled
                    && !detectionFrame.empty();
                const bool detectorAcceptsGpu =
                    g_detector->backend() == DetectorBackend::TensorRT;
                if (!screenshotGpu.empty() && !usedCpuDetectionOverride && detectorAcceptsGpu)
                {
                    g_detector->processFrameGpu(std::move(screenshotGpu));
                }
                else
                {
                    if (detectionFrame.empty() && !screenshotGpu.empty())
                    {
                        // Synchronous D2H is sufficient (copy done on return);
                        // avoid cudaDeviceSynchronize so we don't stall on the
                        // detector's inference stream from the capture thread.
                        screenshotGpu.download(detectionFrame);
                    }
                    if (!detectionFrame.empty())
                        g_detector->processFrame(detectionFrame);
                }
            }

            lastSuccessfulFrameTime = std::chrono::steady_clock::now();
            setCaptureAvailable();

            // strict 消费者(DML/depth/screenshot)路径的 host 副本仍然需要更新
            // latestFrame——preview/crosshair 已由 worker 自己刷,但走 strict 时
            // 主循环本来就要 sync,顺手刷一下让 preview 拿到最新的也合理。
            if (freshCpuFrameThisIter && !screenshotCpu.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    latestFrame = screenshotCpu;
                    if (frameQueue.size() >= 1)
                        frameQueue.pop_front();
                    frameQueue.push_back(latestFrame);
                }
                frameCV.notify_one();
            }

            if (screenshotRequested && !screenshotCpu.empty())
            {
                cv::Mat saveMat = screenshotCpu.clone();
                if (!saveMat.empty())
                {
                    auto epoch_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                    std::string filename = std::to_string(epoch_time) + ".jpg";
                    screenshotWriter.Enqueue(filename, std::move(saveMat));
                    lastSaveTime = screenshotNow;
                }
            }

            captureFrameCount++;
            captureSourceFrameCount++;
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsedTime = currentTime - captureFpsStartTime;
            if (elapsedTime.count() >= 1.0)
            {
                captureFps = static_cast<int>(captureFrameCount / elapsedTime.count());
                captureFrameCount = 0;
                captureFpsStartTime = currentTime;
            }

            // Source FPS uses an independent 1-second window so a long
            // detector-stall doesn't poison both counters at once. Prefer the
            // backend's own producer-side estimate when it has one — the
            // consumer-side frame count only reflects what *we* dequeued and
            // hides upstream frames the producer dropped to keep its queue
            // bounded (true for the direct CaptureCard backend).
            std::chrono::duration<double> sourceElapsed = currentTime - captureSourceFpsStartTime;
            if (sourceElapsed.count() >= 1.0)
            {
                const int producerFps = capturer ? capturer->GetSourceFpsEstimate() : 0;
                captureSourceFps = producerFps > 0
                    ? producerFps
                    : static_cast<int>(captureSourceFrameCount / sourceElapsed.count());
                captureSourceFrameCount = 0;
                captureSourceFpsStartTime = currentTime;

                // 把后端的接收诊断同步推到全局 atomics 给 UI。eth_capture 自己
                // 已经按 1 秒滚动算好,这里只是搬运。
                if (capturer)
                {
                    captureSenderSpanFps.store(capturer->GetSenderSpanFps());
                    captureWireLostFps.store(capturer->GetWireLostFps());
                    capturePartialLostFps.store(capturer->GetPartialLostFps());
                    capturePcapKernelDroppedFps.store(capturer->GetPcapKernelDroppedFps());
                    capturePcapIfDroppedFps.store(capturer->GetPcapIfDroppedFps());
                }
                else
                {
                    captureSenderSpanFps.store(0);
                    captureWireLostFps.store(0);
                    capturePartialLostFps.store(0);
                    capturePcapKernelDroppedFps.store(0);
                    capturePcapIfDroppedFps.store(0);
                }
            }

                // limiter 现在用高精度可等待计时器,每帧误差 ~50us 量级,即便对
                // 事件驱动后端(eth_capture / mf_capture / avermedia)做帧率上限
                // 也不会像旧版那样把源 240fps 压到 175。所有后端统一走 limiter:
                //   - capture_fps = 0  -> frameDuration 为空, applyFrameLimiter
                //     直接走 else 分支,只刷新锚点,不睡眠;
                //   - capture_fps > 0  -> 严格执行帧周期,产帧更快时按目标节流,
                //     产帧更慢时不睡,自然贴产帧节奏。
                applyFrameLimiter();
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Loop exception: " << e.what() << std::endl;
                capturer.reset();
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            catch (...)
            {
                std::cerr << "[Capture] Loop exception: unknown." << std::endl;
                capturer.reset();
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Capture] Unhandled exception: " << e.what() << std::endl;
        throw;
    }
}
