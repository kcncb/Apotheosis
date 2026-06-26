#ifndef FLASHLIGHT_RUNTIME_H
#define FLASHLIGHT_RUNTIME_H

#include <chrono>
#include <vector>

#include <opencv2/opencv.hpp>

namespace flashlight_runtime
{

// One candidate flashlight halo, in detection-image pixel space, AFTER the
// depth-gate + temporal-tracker have run on the capture thread. `confidence`
// is the adjusted score (appearance ∓ depth/positional/onset/confirmed terms)
// the spots are sorted by, best first. The mouse loop applies the final
// accept rule (colocation OR (passed_depth && confirmed)) because that needs
// the model boxes, which live there.
struct Spot
{
    cv::Rect    box{};                 // synthesized bbox (centre±radius), clamped
    cv::Point2f center{};              // halo centre (det-img px)
    float       radius       = 0.0f;
    float       confidence   = 0.0f;   // adjusted score, [0,1]
    bool        passed_depth = true;   // false only when depth-gate hard-rejected it
    bool        confirmed    = false;  // temporal tracker reached confirm_frames
    bool        onset        = false;  // light just turned on this frame
};

// Latest flashlight detection. `valid` means the most recent capture frame
// produced at least one publishable spot. Consumers must drop snapshots older
// than kFreshnessMs. Single-writer (capture thread), multi-reader.
struct Snapshot
{
    bool              valid = false;
    std::vector<Spot> spots;            // sorted by descending confidence
    std::chrono::steady_clock::time_point ts{};
};

inline constexpr int kFreshnessMs = 150;

Snapshot read();
void publish(const Snapshot& snap);

// Capture thread enters this once per published frame. Reads the global config
// + per-hotkey gate; derives all internal constants from the three macro knobs
// (crosshair::flashlight_derive_tuning), runs the appearance detector, then the
// depth-gate and temporal tracker. Publishes ranked candidates with metadata.
// Cheap when off.
void process_frame(const cv::Mat& bgrFrame);

} // namespace flashlight_runtime

#endif // FLASHLIGHT_RUNTIME_H
