#ifndef FLASHLIGHT_RUNTIME_H
#define FLASHLIGHT_RUNTIME_H

#include <chrono>

#include <opencv2/opencv.hpp>

namespace flashlight_runtime
{

// Latest flashlight halo, in detection-image pixel space. `valid` means the
// most recent capture frame produced a hit. Consumers (mouse loop + preview)
// must drop snapshots older than kFreshnessMs to avoid acting on a halo that
// has already vanished. Single-writer (capture thread), multi-reader.
struct Snapshot
{
    bool       valid    = false;
    cv::Rect   box{};                  // synthesized bbox (centre±radius)
    cv::Point2f center{};              // halo centre (det-img px)
    float      radius   = 0.0f;
    float      confidence = 0.0f;
    std::chrono::steady_clock::time_point ts{};
};

inline constexpr int kFreshnessMs = 150;

Snapshot read();
void publish(const Snapshot& snap);

// Capture thread enters this once per published frame. Reads the global
// config + per-hotkey gate; runs the brightness-based detector when the
// active hotkey opted in AND any hotkey has flashlight_detect_enabled.
// Publishes valid=true when a halo passes the gates, valid=false otherwise.
// Cheap when off.
void process_frame(const cv::Mat& bgrFrame);

} // namespace flashlight_runtime

#endif // FLASHLIGHT_RUNTIME_H
