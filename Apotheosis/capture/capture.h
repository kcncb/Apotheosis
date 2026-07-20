#ifndef CAPTURE_H
#define CAPTURE_H

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "../mem/gpu_image.h"

extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::deque<cv::Mat> frameQueue;

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT);
extern int screenWidth;
extern int screenHeight;

extern std::atomic<int> captureFrameCount;
extern std::atomic<int> captureFps;
extern std::atomic<int> captureSourceFps;
// 接收侧诊断(仅 eth_capture 等会填充,其他后端保持 0)。给 Qt UI 的"接收
// 诊断"卡读用,把"产帧 fps 为什么上不去"拆成 wire / partial / pcap 内核 /
// NIC 驱动 四个互不重叠的桶,一目了然。
extern std::atomic<int> captureSenderSpanFps;
extern std::atomic<int> captureWireLostFps;
extern std::atomic<int> capturePartialLostFps;
extern std::atomic<int> capturePcapKernelDroppedFps;
extern std::atomic<int> capturePcapIfDroppedFps;
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
    // GPU (typically from nvJPEG). Default impl returns an empty GpuImage so
    // backends that don't support GPU decode fall back transparently to the
    // CPU path. Consumer (captureThread) prefers this when available.
    virtual GpuImage GetNextFrameGpu() { return GpuImage(); }
    virtual int GetSourceFpsEstimate() const { return 0; }

    // 事件驱动取帧。消费线程在队列取空时调用:支持的后端用 condition_variable
    // 在产帧入队后唤醒它,从而精确贴着产帧节奏取帧,而不是靠固定节拍轮询——后者
    // 在"消费节拍"和"产帧节拍"两个独立同频时钟相位漂移时会踏空,在队列容量为 1
    // 的低延迟设定下还会丢帧。返回 true 表示"可能已有新帧,请重试取帧";默认返回
    // false 表示该后端不支持事件等待,消费侧自行回退到短 sleep。
    virtual bool WaitFrame(int /*timeoutMs*/) { return false; }

    // 该后端是否支持事件驱动取帧(WaitFrame 由产帧 notify 唤醒)。为 true 时,
    // 源帧率未超过目标上限就跳过固定节拍 sleep,直接跟随产帧事件;只在源确实
    // 高于上限时才启用 limiter。这避免源/消费两个同频时钟错相丢帧。
    virtual bool SupportsEventWait() const { return false; }

    // 设置采集帧率上限(降采样限速)。支持的后端在解码前按目标间隔丢多余帧——不睡眠,
    // 所以不会像固定节拍 limiter 那样过冲压低帧率;丢的帧不解码,省 CPU。0 = 不限速。
    virtual void SetTargetFps(int /*fps*/) {}

    // true 表示 SetTargetFps 已在后端入队/解码前完成降采样；主循环不能再叠加
    // 第二个定时 limiter，否则两个独立节拍会错相并把 60/240 再压低一档。
    virtual bool HandlesTargetFps() const { return false; }

    // 接收诊断:每个 1 秒滚动窗口的统计。默认 0,只在能给出数据的后端(如
    // eth_capture)里 override。意义见 capture.h 顶部那几个 extern 注释。
    virtual int GetSenderSpanFps()          const { return 0; }
    virtual int GetWireLostFps()            const { return 0; }
    virtual int GetPartialLostFps()         const { return 0; }
    virtual int GetPcapKernelDroppedFps()   const { return 0; }
    virtual int GetPcapIfDroppedFps()       const { return 0; }
};

#endif // CAPTURE_H
