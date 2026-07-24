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

    // Bump version + refresh the publish timestamp/interval. Caller must hold
    // `mutex` (every publish site already does).
    void bumpVersionLocked()
    {
        const auto now = std::chrono::steady_clock::now();
        if (version > 0 && stamp.time_since_epoch().count() != 0)
            last_interval_ms =
                std::chrono::duration<double, std::milli>(now - stamp).count();
        stamp = now;
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
