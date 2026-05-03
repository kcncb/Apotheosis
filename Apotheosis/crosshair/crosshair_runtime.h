#ifndef CROSSHAIR_RUNTIME_H
#define CROSSHAIR_RUNTIME_H

#include <atomic>
#include <chrono>
#include <mutex>

#include <opencv2/opencv.hpp>

#include "crosshair_detector.h"

namespace crosshair_runtime
{

// Latest crosshair-color hit, in detection-image pixel space. `valid` is
// true when the most recent capture frame produced a hit; the consumer
// (mouse loop) is responsible for treating snapshots older than
// kFreshnessMs as "no hit". Lock-free read via the seqlock helpers below.
struct PivotSnapshot
{
    double x = 0.0;
    double y = 0.0;
    std::chrono::steady_clock::time_point ts{};
    bool valid = false;
};

inline constexpr int kFreshnessMs = 150;

// Single-writer (capture thread), multi-reader (mouse loop) snapshot.
// Implemented as mutex-protected POD — the work is dwarfed by the rest of
// each tick, so the lock cost is irrelevant.
PivotSnapshot read();
void publish(const PivotSnapshot& snap);

// Capture thread enters this once per published frame. Reads HotkeyProfile
// `crosshair_detect_enabled` flags from `config.hotkeys` (caller already
// holds a config snapshot if needed) — if any hotkey opted in AND the
// global palette has at least one enabled colour, runs the detector on the
// supplied BGR detection-resolution frame and publishes a snapshot.
// Otherwise publishes valid=false. Cheap when off.
void process_frame(const cv::Mat& bgrFrame);

} // namespace crosshair_runtime

#endif // CROSSHAIR_RUNTIME_H
