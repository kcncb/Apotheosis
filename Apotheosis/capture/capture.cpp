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
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#include "Apotheosis.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "other_tools.h"
#include "udp_capture.h"
#include "tcp_capture.h"
#include "opencv_capture.h"
#include "capture_utils.h"

cv::Mat latestFrame;
std::mutex frameMutex;

int screenWidth = 0;
int screenHeight = 0;

std::atomic<int> captureFrameCount(0);
std::atomic<int> captureFps(0);
std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

std::deque<cv::Mat> frameQueue;

namespace
{
std::mutex g_detectionSuppressionMaskMutex;
cv::Mat g_detectionSuppressionMask;
}

static void UpdateDetectionSuppressionMask(const cv::Mat& mask)
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    if (!mask.empty() && mask.type() == CV_8UC1)
        g_detectionSuppressionMask = mask.clone();
    else
        g_detectionSuppressionMask.release();
}

cv::Mat getCurrentDetectionSuppressionMask()
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    return g_detectionSuppressionMask.clone();
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
    int opencv_capture_index = 0;
    std::string opencv_capture_api;
    std::string opencv_capture_url;
    int opencv_capture_width = 0;
    int opencv_capture_height = 0;
    int opencv_capture_fps = 0;
    std::string opencv_capture_format;
    int opencv_capture_crop_width = 0;
    int opencv_capture_crop_height = 0;
    std::string backend;
    std::vector<std::string> screenshot_button;
    int screenshot_delay = 0;
    bool show_window = false;
    bool verbose = false;
    bool depth_inference_enabled = false;
    bool depth_mask_enabled = false;
    std::string depth_model_path;
    int depth_mask_fps = 0;
    int depth_mask_near_percent = 0;
    int depth_mask_expand = 0;
    bool depth_mask_invert = false;
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
    snapshot.opencv_capture_index = config.opencv_capture_index;
    snapshot.opencv_capture_api = config.opencv_capture_api;
    snapshot.opencv_capture_url = config.opencv_capture_url;
    snapshot.opencv_capture_width = config.opencv_capture_width;
    snapshot.opencv_capture_height = config.opencv_capture_height;
    snapshot.opencv_capture_fps = config.opencv_capture_fps;
    snapshot.opencv_capture_format = config.opencv_capture_format;
    snapshot.opencv_capture_crop_width = config.opencv_capture_crop_width;
    snapshot.opencv_capture_crop_height = config.opencv_capture_crop_height;
    snapshot.backend = config.backend;
    snapshot.screenshot_button = config.screenshot_button;
    snapshot.screenshot_delay = config.screenshot_delay;
    snapshot.show_window = config.show_window;
    snapshot.verbose = config.verbose;
    snapshot.depth_inference_enabled = config.depth_inference_enabled;
    snapshot.depth_mask_enabled = config.depth_mask_enabled;
    snapshot.depth_model_path = config.depth_model_path;
    snapshot.depth_mask_fps = config.depth_mask_fps;
    snapshot.depth_mask_near_percent = config.depth_mask_near_percent;
    snapshot.depth_mask_expand = config.depth_mask_expand;
    snapshot.depth_mask_invert = config.depth_mask_invert;
    return snapshot;
}

std::string NormalizeCaptureMethod(const std::string& method)
{
    if (method == "udp_capture" || method == "tcp_capture" || method == "opencv_capture")
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

                if (method == "opencv_capture")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using OpenCV capture card" << std::endl;
                    return std::make_unique<OpenCVCapture>(
                        width, height,
                        cfg.opencv_capture_index,
                        cfg.opencv_capture_api,
                        cfg.opencv_capture_width,
                        cfg.opencv_capture_height,
                        cfg.opencv_capture_fps,
                        cfg.opencv_capture_format,
                        cfg.opencv_capture_crop_width,
                        cfg.opencv_capture_crop_height,
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
            detectionBuffer.version++;
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

        auto frameStartTime = std::chrono::steady_clock::now();
        auto applyFrameLimiter = [&]()
        {
            if (frameDuration.has_value())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = now - frameStartTime;
                if (elapsed < frameDuration.value())
                {
                    std::this_thread::sleep_for(frameDuration.value() - elapsed);
                }
            }
            frameStartTime = std::chrono::steady_clock::now();
        };

        ScreenshotWriter screenshotWriter;
        auto lastSaveTime = std::chrono::steady_clock::now();
        auto lastSuccessfulFrameTime = std::chrono::steady_clock::now();
        constexpr auto staleFrameTimeout = std::chrono::milliseconds(500);

        while (!shouldExit && !session_stop_requested.load())
        {
            try
            {
                currentCfg = SnapshotCaptureConfig();

            if (capture_fps_changed.exchange(false))
            {
                updateFrameDuration(currentCfg.capture_fps);
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

            static bool lastDepthInferenceEnabled = true;
            if (!currentCfg.depth_inference_enabled)
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
            // doesn't produce GpuMats or the per-iteration GPU queue is
            // empty, fall through to the legacy CPU queue.
            cv::cuda::GpuMat screenshotGpu = capturer->GetNextFrameGpu();
            if (!screenshotGpu.empty())
            {
                // Only download to CPU when a downstream consumer actually
                // needs host pixels (depth mask, screenshot save). The
                // detector itself takes the GpuMat directly below.
                const bool needCpu = currentCfg.depth_inference_enabled
                    || screenshotRequested
                    || currentCfg.show_window;
                if (needCpu)
                    screenshotGpu.download(screenshotCpu);
            }
            else
            {
                screenshotCpu = capturer->GetNextFrameCpu();
            }

            if (screenshotGpu.empty() && screenshotCpu.empty())
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                    setCaptureUnavailable();

                if (!frameDuration.has_value())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                applyFrameLimiter();
                continue;
            }

            // circle_mask is CPU-only; apply it only when we already have a
            // CPU frame. With the GPU fast-path it is skipped silently.
            if (currentCfg.circle_mask && !screenshotCpu.empty())
                screenshotCpu = apply_circle_mask(screenshotCpu);

            detectionFrame = screenshotCpu;
            if (currentCfg.depth_inference_enabled && currentCfg.depth_mask_enabled)
            {
                cv::Mat mask;
                depth_anything::DepthMaskOptions maskOptions;
                maskOptions.enabled = currentCfg.depth_mask_enabled;
                maskOptions.fps = currentCfg.depth_mask_fps;
                maskOptions.near_percent = currentCfg.depth_mask_near_percent;
                maskOptions.expand = currentCfg.depth_mask_expand;
                maskOptions.invert = currentCfg.depth_mask_invert;

                auto& depthMask = depth_anything::GetDepthMaskGenerator();
                depthMask.update(screenshotCpu, maskOptions, currentCfg.depth_model_path, gLogger);
                mask = depthMask.getMask();

                if (!mask.empty() && mask.size() != screenshotCpu.size())
                    mask.release();

                if (mask.empty())
                {
                    if (currentCfg.depth_model_path.empty())
                    {
                        if (depthMaskFallbackModel.ready())
                            depthMaskFallbackModel.reset();
                        depthMaskFallbackModelPath.clear();
                    }
                    else if (depthMaskFallbackModelPath != currentCfg.depth_model_path || !depthMaskFallbackModel.ready())
                    {
                        if (depthMaskFallbackModel.initialize(currentCfg.depth_model_path, gLogger))
                        {
                            depthMaskFallbackModelPath = currentCfg.depth_model_path;
                        }
                    }

                    if (depthMaskFallbackModel.ready())
                    {
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
                if (!screenshotGpu.empty() && !usedCpuDetectionOverride)
                    g_detector->processFrameGpu(screenshotGpu);
                else if (!detectionFrame.empty())
                    g_detector->processFrame(detectionFrame);
            }

            lastSuccessfulFrameTime = std::chrono::steady_clock::now();
            setCaptureAvailable();

            {
                std::lock_guard<std::mutex> lock(frameMutex);
                latestFrame = screenshotCpu;
                if (frameQueue.size() >= 1)
                    frameQueue.pop_front();
                frameQueue.push_back(latestFrame);
            }
            frameCV.notify_one();

            if (screenshotRequested)
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
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsedTime = currentTime - captureFpsStartTime;
            if (elapsedTime.count() >= 1.0)
            {
                captureFps = static_cast<int>(captureFrameCount / elapsedTime.count());
                captureFrameCount = 0;
                captureFpsStartTime = currentTime;
            }

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
