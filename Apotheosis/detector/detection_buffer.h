#pragma once
#include <algorithm>
#include <chrono>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <opencv2/opencv.hpp>

struct DetectionBuffer
{
    std::mutex mutex;
    std::condition_variable cv;
    int version = 0;
    std::vector<cv::Rect> boxes;
    // 模型解码后的浮点框，专供 AVA selector/tracker 使用。索引与 boxes、
    // classes、confidences 一一对应。
    std::vector<cv::Rect2f> precise_boxes;
    std::vector<int> classes;
    std::vector<float> confidences;

    // Wall-clock of the most recent publish and the gap to the previous one.
    // Detection runs on a worker thread behind the live capture frame, so
    // these let freshness-sensitive consumers (crosshair pivot, aim loop)
    // tell "detector is keeping up" from "detector stalled and the buffer is
    // reporting a frame that's already gone".
    std::chrono::steady_clock::time_point stamp{};
    double last_interval_ms = 0.0;

    // 产生这批检测的那一帧【被采集】的时刻 (steady_clock 纳秒)。
    //
    // 与上面的 stamp 关键区别: stamp 是【推理发布】时刻, 不含采集卡/解码/预处理;
    // 本字段是【像素被采集】时刻, 由 runtime::latency 探针在采集侧打点, detector
    // 取帧时带走、发布时写入。aim loop 用 (now - frame_stamp_ns) 即得到真正的
    // 端到端总延迟 —— 也就是决定"准星落后移动目标多少"(v × L)的那个 L。
    //
    // 0 表示无戳 (空检测帧 / 采集不可用), 消费方必须忽略而不是当成 0 延迟。
    int64_t frame_stamp_ns = 0;

    // Bump version + refresh the publish timestamp/interval. Caller must hold
    // `mutex` (every publish site already does).
    //
    // frame_capture_ns: 本批检测所依据的那一帧的采集时刻 (见 frame_stamp_ns)。
    // 传 0 (默认) 表示"无戳" —— 必须显式清零, 否则空检测帧会继承上一帧的戳,
    // 让下游把陈旧像素误判成新鲜数据。
    void bumpVersionLocked(int64_t frame_capture_ns = 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (version > 0 && stamp.time_since_epoch().count() != 0)
            last_interval_ms =
                std::chrono::duration<double, std::milli>(now - stamp).count();
        stamp = now;
        frame_stamp_ns = frame_capture_ns;
        ++version;
    }

    // True when the last published detection is old relative to the detector's
    // own recent cadence — i.e. inference stalled and the buffer no longer
    // reflects the current frame. Threshold scales with the measured
    // inter-detection interval so it adapts to slow hardware instead of using
    // a hard-coded ms value. Caller must hold `mutex`.
    bool staleLocked() const
    {
        if (version <= 1 || last_interval_ms <= 0.0)
            return false; // not enough history to judge
        const double thresholdMs =
            std::clamp(2.0 * last_interval_ms, 50.0, 600.0);
        const double ageMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stamp).count();
        return ageMs > thresholdMs;
    }

    void set(const std::vector<cv::Rect>& newBoxes,
             const std::vector<int>& newClasses,
             const std::vector<float>& newConfidences)
    {
        std::lock_guard<std::mutex> lock(mutex);
        boxes = newBoxes;
        precise_boxes.clear();
        precise_boxes.reserve(newBoxes.size());
        for (const auto& box : newBoxes)
            precise_boxes.emplace_back(
                static_cast<float>(box.x), static_cast<float>(box.y),
                static_cast<float>(box.width), static_cast<float>(box.height));
        classes = newClasses;
        confidences = newConfidences;
        bumpVersionLocked();
        cv.notify_all();
    }

    void get(std::vector<cv::Rect>& outBoxes,
             std::vector<int>& outClasses,
             std::vector<float>& outConfidences,
             int& outVersion)
    {
        std::lock_guard<std::mutex> lock(mutex);
        outBoxes = boxes;
        outClasses = classes;
        outConfidences = confidences;
        outVersion = version;
    }
};
