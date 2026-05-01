#ifndef CAPTURE_H
#define CAPTURE_H

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <deque>

extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::deque<cv::Mat> frameQueue;

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT);
extern int screenWidth;
extern int screenHeight;

extern std::atomic<int> captureFrameCount;
extern std::atomic<int> captureFps;
extern std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

extern cv::Mat latestFrame;

extern std::mutex frameMutex;
extern std::condition_variable frameCV;
extern std::atomic<bool> shouldExit;

class IScreenCapture
{
public:
    virtual ~IScreenCapture() {}
    virtual cv::Mat GetNextFrameCpu() = 0;

    // Zero-copy variant: returns a decoded frame that already lives on the
    // GPU (typically from nvJPEG). Default impl returns an empty GpuMat so
    // backends that don't support GPU decode fall back transparently to the
    // CPU path. Consumer (captureThread) prefers this when available.
    virtual cv::cuda::GpuMat GetNextFrameGpu() { return cv::cuda::GpuMat(); }
};

cv::Mat getCurrentDetectionSuppressionMask();

#endif // CAPTURE_H
