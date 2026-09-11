#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <timeapi.h>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "tensorrt/nvinf.h"
#include "Apotheosis.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "other_tools.h"
#include "mf_capture.h"
#include "capture_card_probe.h"
#include "runtime/active_hotkey.h"
#include "runtime/latency_probe.h"
#include "gpu_color_ops.h"
#include <cuda_runtime.h>
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

struct CaptureThreadConfig
{
    // 只有「采集卡」一种采集方式; 所有参数都必须来自设备真实能力探测。
    std::string capture_device;      // friendly name (index 会随插拔变化)
    std::string capture_format;      // NV12 | MJPG | YUY2 | RGB32
    int  capture_width  = 0;
    int  capture_height = 0;
    int  capture_fps    = 0;
    bool capture_gpu_decode = true;
    int  detection_resolution = 0;   // = 模型输入边长, 同时就是中心裁切边长
    bool circle_mask = false;
    std::string backend;
    std::vector<std::string> screenshot_button;
    int  screenshot_delay = 0;
    bool show_window = false;
    bool verbose = false;

};

CaptureThreadConfig SnapshotCaptureConfig()
{
    std::lock_guard<std::recursive_mutex> cfgLock(configMutex);
    CaptureThreadConfig snapshot;
    snapshot.capture_device = config.capture_device;
    snapshot.capture_format = config.capture_format;
    snapshot.capture_width  = config.capture_width;
    snapshot.capture_height = config.capture_height;
    snapshot.capture_fps    = config.capture_fps;
    snapshot.capture_gpu_decode = config.capture_gpu_decode;
    snapshot.detection_resolution = config.detection_resolution;
    snapshot.circle_mask = config.circle_mask;
    snapshot.backend = config.backend;
    snapshot.screenshot_button = config.screenshot_button;
    snapshot.screenshot_delay = config.screenshot_delay;
    snapshot.show_window = config.show_window;
    snapshot.verbose = config.verbose;

    return snapshot;
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

// Dedicated latest-frame GPU worker for ordinary crosshair colour detection.
// It never downloads the image: CUDA transfers only {count,sumX,sumY} after
// reducing the tiny centre ROI. Submit is non-blocking and stale work is
// replaced when capture briefly outruns the worker.
class GpuCrosshairWorker
{
public:
    GpuCrosshairWorker() : thread_([this]() { Run(); }) {}
    ~GpuCrosshairWorker() { Stop(); }

    void Submit(GpuImage gpu)
    {
        if (gpu.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stop_) return;
            pending_ = std::move(gpu);
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
        if (thread_.joinable()) thread_.join();
    }

    void Run()
    {
        while (true)
        {
            GpuImage gpu;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]() { return stop_ || hasPending_; });
                if (stop_ && !hasPending_) break;
                gpu = std::move(pending_);
                pending_.release();
                hasPending_ = false;
            }
            if (!gpu.empty())
                crosshair_runtime::process_gpu_frame(gpu);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    GpuImage pending_;
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

        auto createCapturer = [&](const CaptureThreadConfig& cfg, int width, int height) -> std::unique_ptr<IScreenCapture>
        {
            (void)width; (void)height;
            try
            {
                // ── 唯一的采集路径: 采集卡 (Media Foundation 直采) ──
                //
                // 中心裁切【恒等于】模型输入边长 detection_resolution。
                // 不再有独立的 capture_crop 设置: 送进检测器的永远正好是模型要的
                // 尺寸, 不多裁也不少裁再缩 —— 省掉一次缩放(少一份延迟), 同时避免
                // "裁切尺寸与模型尺寸不一致"导致检测框和鼠标坐标空间错位。
                const bool crop_enabled = true;
                const int  out_side = std::max(1, cfg.detection_resolution);

                // 按 friendly name 找设备。index 会随插拔顺序变化, 名字不会。
                // 找不到【不换设备】: 上次选的卡没插就该报错, 而不是偷偷采了
                // 另一张卡的画面, 让用户对着错位的画面调半天参数。
                const auto devices = MFCapture::EnumerateDevices();
                int device_index = -1;
                for (const auto& d : devices)
                    if (d.friendly_name == cfg.capture_device) { device_index = d.index; break; }

                if (device_index < 0)
                {
                    std::cerr << "[Capture] Selected capture card \"" << cfg.capture_device
                              << "\" is NOT present (" << devices.size()
                              << " device(s) found). No substitution is performed; "
                                 "open the capture settings and pick a connected card."
                              << std::endl;
                    return nullptr;
                }

                if (cfg.verbose)
                    std::cout << "[Capture] Capture card: " << cfg.capture_device
                              << " | " << cfg.capture_format
                              << " " << cfg.capture_width << "x" << cfg.capture_height
                              << "@" << cfg.capture_fps << "fps"
                              << " | crop " << out_side << "x" << out_side
                              << " | " << (cfg.capture_gpu_decode ? "GPU" : "CPU") << std::endl;

                // 注意: 这里不传 device_index 之外的任何"兜底"。格式/分辨率/帧率
                // 只要设备对不上, MFCapture 会直接失败并在 LastError() 里写明
                // 设备实际支持什么 —— 不做任何替换。
                return std::make_unique<MFCapture>(
                    cfg.capture_width,
                    cfg.capture_height,
                    out_side,
                    crop_enabled,
                    cfg.capture_fps,
                    cfg.capture_format,
                    device_index,
                    cfg.capture_gpu_decode);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Failed to initialize capture card: " << e.what() << std::endl;
                return nullptr;
            }
        };

        std::unique_ptr<IScreenCapture> capturer = createCapturer(currentCfg, captureWidth, captureHeight);
        if (capturer)
            capturer->SetTargetFps(currentCfg.capture_fps);
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
            detectionBuffer.precise_boxes.clear();
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
        bool eventSourceAboveLimit = false;
        auto updateFrameDuration = [&](int captureFpsSetting)
        {
            eventSourceAboveLimit = false;
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
        // 路径节流到 ~60fps,严格消费者(DML detector / screenshot /
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
            std::array<cudaEvent_t, 8> maskEvents{};
            size_t maskEventIndex{ 0 };
            CaptureCudaGuard()
            {
                cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
                for (auto& event : maskEvents)
                    cudaEventCreateWithFlags(&event, cudaEventDisableTiming);
            }
            ~CaptureCudaGuard()
            {
                for (auto& event : maskEvents)
                    if (event) cudaEventDestroy(event);
                if (stream)    cudaStreamDestroy(stream);
            }
            cudaEvent_t nextMaskEvent()
            {
                cudaEvent_t event = maskEvents[maskEventIndex];
                maskEventIndex = (maskEventIndex + 1) % maskEvents.size();
                return event;
            }
        };
        CaptureCudaGuard captureCuda;

        // 异步 host 副本生产者。capture 主循环 Submit() 一份 GpuImage 引用就
        // 立刻返回,worker 独立做 D2H + latestFrame 刷新 + 准星检测。这样主循环
        // 完全没有 cudaEventSynchronize / cudaMemcpy2D / cv::Mat::copyTo 这些
        // host-blocking 操作,PCIe Gen1 的 D2H 也不会再压低 240fps 链路。
        HostCopyWorker hostCopyWorker;
        GpuCrosshairWorker gpuCrosshairWorker;

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
                eventSourceAboveLimit = false;
                setCaptureUnavailable();

                if (currentCfg.detection_resolution > 0)
                {
                    captureWidth = currentCfg.detection_resolution;
                    captureHeight = currentCfg.detection_resolution;
                }

                capturer.reset();
                capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                if (capturer)
                    capturer->SetTargetFps(currentCfg.capture_fps);
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
                    if (capturer)
                        capturer->SetTargetFps(currentCfg.capture_fps);
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



            // Prefer the zero-copy GPU path (nvJPEG output). If the backend
            // doesn't produce GPU frames or the per-iteration GPU queue is
            // empty, fall through to the CPU queue.
            GpuImage screenshotGpu = capturer->GetNextFrameGpu();
            bool freshCpuFrameThisIter = false;
            bool gpuMaskApplied = false;

            // ★ 端到端延迟探针的起点: 帧刚进入本进程, 这是 PC 侧能打的最早
            // 时刻。打点必须在下游任何处理之前 —— circle_mask 下推、D2H 下载、
            // 提交 detector、推理、发布、控制环唤醒, 全部计入总延迟。
            // (采集卡内部 HDMI->USB 那段在此刻已经花掉, 无法观测, 见探针头注释)
            if (!screenshotGpu.empty())
                runtime::latency::noteCaptureForStats(runtime::latency::markCapture());

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
                cudaEvent_t maskEvent = captureCuda.nextMaskEvent();
                if (maskEvent)
                {
                    cudaEventRecord(maskEvent, captureCuda.stream);
                    screenshotGpu.setReadyEvent(maskEvent);
                }
                else
                {
                    cudaStreamSynchronize(captureCuda.stream);
                    screenshotGpu.setReadyEvent(nullptr);
                }
                gpuMaskApplied = true;
            }

            // 消费者分两档:
            //   strict — 必须主循环里立刻拿到 host 副本: DML detector(GPU 路径
            //   不支持)、本帧截图保存。这些路径本来就在主循环
            //   后面用 screenshotCpu,挪不到 worker。
            //   async — preview show_window + 准星颜色检测(active hotkey 时)。
            //   这两个不在 detector / 主路径上,完全甩给 HostCopyWorker 去做 D2H。
            //   crosshair 现在跑在 worker 而不是 capture 线程,瞄准时不会再用
            //   crosshair 的 cv 工作量阻塞 240fps 链路。
            const bool detectorNeedsCpu = !g_detector
                || g_detector->backend() != DetectorBackend::TensorRT;
            const bool needCpuStrict = screenshotRequested
                || detectorNeedsCpu;
            const bool gpuCrosshairActive = crosshair_runtime::gpu_path_active();
            const bool cpuColourActive = crosshair_runtime::cpu_path_active();
            const bool needCpuAsync = currentCfg.show_window || cpuColourActive;

            if (!screenshotGpu.empty())
            {
                // Ordinary crosshair colour now sees every captured GPU frame.
                // The worker launches only a tiny ROI kernel and copies 12 bytes.
                if (gpuCrosshairActive)
                    gpuCrosshairWorker.Submit(screenshotGpu);

                if (needCpuStrict)
                {
                    // 主循环必须同步等 host 副本,接受这部分阻塞。
                    screenshotGpu.download(screenshotCpu);
                    freshCpuFrameThisIter = true;
                }
                if (needCpuAsync)
                {
                    // Preview stays near 60fps. Only laser line fitting retains
                    // the CPU colour path; ordinary crosshair colour runs above.
                    const auto now = std::chrono::steady_clock::now();
                    auto interval = kPreviewSubmitInterval;
                    if (cpuColourActive)
                        interval = std::chrono::microseconds(8000); // ~120fps
                    if (needFirstPreviewSubmit
                        || now - lastPreviewSubmitTime >= interval)
                    {
                        // 共享引用给 worker;detector 拿原引用 std::move 走也没
                        // 关系,storage 是 shared_ptr。
                        hostCopyWorker.Submit(screenshotGpu, cpuColourActive);
                        lastPreviewSubmitTime = now;
                        needFirstPreviewSubmit = false;
                    }
                }
            }
            else
            {
                screenshotCpu = capturer->GetNextFrameCpu();
                freshCpuFrameThisIter = !screenshotCpu.empty();
                if (freshCpuFrameThisIter)
                    runtime::latency::noteCaptureForStats(runtime::latency::markCapture());
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

            // CPU-only capture / DML fallback has no device image to search.
            // Preserve the established detector on that path.
            if (screenshotGpu.empty() && (gpuCrosshairActive || cpuColourActive)
                && !screenshotCpu.empty())
                crosshair_runtime::process_frame(screenshotCpu);

            detectionFrame = screenshotCpu;

            if (g_detector)
            {
                // Zero-copy GPU path when nvJPEG produced a GpuMat.
                const bool usedCpuDetectionOverride = false;
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

            // strict 消费者(DML/screenshot)路径的 host 副本仍然需要更新
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

                // 事件驱动后端已经由产帧通知精确控制消费节奏。源和
                // 目标同为 240fps 时若再 sleep,两套独立时钟会相位漂移,在
                // “最新帧”小队列下踏空/覆盖,把 240fps 压成约 200fps。
                //
                // 但 capture_fps 仍是上限:当观测到源帧率明显高于目标时锁定
                // limiter。加 1%/最少 2fps 容差是为了容纳 239.76/240Hz 的统计
                // 抖动;一旦确认源超限就保持限速,避免 UDP/TCP 回退统计在
                // 限速后变低导致 limiter 每秒开关振荡。
                bool shouldLimit = !capturer->SupportsEventWait();
                if (!shouldLimit
                    && !capturer->HandlesTargetFps()
                    && frameDuration.has_value()
                    && currentCfg.capture_fps > 0)
                {
                    int sourceEstimate = capturer->GetSourceFpsEstimate();
                    if (sourceEstimate <= 0)
                        sourceEstimate = captureSourceFps.load();
                    const int tolerance = std::max(2, currentCfg.capture_fps / 100);
                    if (sourceEstimate > currentCfg.capture_fps + tolerance)
                        eventSourceAboveLimit = true;
                    shouldLimit = eventSourceAboveLimit;
                }

                if (shouldLimit)
                    applyFrameLimiter();
                else
                    frameStartTime = std::chrono::steady_clock::now();
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
