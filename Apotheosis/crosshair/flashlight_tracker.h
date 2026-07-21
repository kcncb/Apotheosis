#ifndef FLASHLIGHT_TRACKER_H
#define FLASHLIGHT_TRACKER_H

#include <opencv2/core.hpp>
#include <vector>

#include "flashlight_tuning.h"

namespace crosshair
{

// Per-spot temporal verdict for the current frame (one per input spot).
struct FlashlightTrackVerdict
{
    bool confirmed = false; // track has persisted >= confirm_frames consecutively
    bool onset     = false; // track first appeared this frame (light just turned on)
    int  age       = 0;     // consecutive frames the track has been seen

    // EMA-smoothed track position / radius. Raw contour moments jitter by 1-3 px
    // per frame from sub-pixel shape changes; the downstream mover's one-euro
    // adapts cutoff to apparent velocity, so during camera rotation that noise
    // leaks straight to the aim output. Smoothing here in tracker-space kills
    // the buzz before the mover ever sees it. For an unmatched (onset) spot
    // the smoothed values equal the raw values.
    cv::Point2f smoothed_center{};
    float       smoothed_radius = 0.0f;
};

// Tiny nearest-neighbour tracker for flashlight spots. STATEFUL, single-thread
// (capture thread only). Each frame it associates the detector's spots to live
// tracks within `max_jump_px`, ages matched tracks, starts new tracks for
// unmatched spots (age 1 = onset), and forgets tracks unseen for `drop_after`
// ticks. A spot that scintillates (water / glass glint winking in and out)
// keeps starting fresh tracks and never reaches `confirm_frames`, so it never
// confirms — which is exactly how the time-gate kills shimmer. Pure geometry,
// no OpenCV image ops.
class FlashlightTracker
{
public:
    std::vector<FlashlightTrackVerdict> update(
        const std::vector<cv::Point2f>& spots,
        const std::vector<float>&       radii,
        const FlashlightTemporal&       params);

    void reset();

private:
    struct Track
    {
        cv::Point2f center{};            // last raw detection centre (assoc anchor)
        float       radius    = 0.0f;
        cv::Point2f smoothed_center{};   // EMA-filtered centre (output to downstream)
        float       smoothed_radius = 0.0f;
        int         age       = 0; // consecutive frames seen
        int         unseen    = 0; // ticks since last seen
        bool        seen_this = false;
    };
    std::vector<Track> tracks_;
};

} // namespace crosshair

#endif // FLASHLIGHT_TRACKER_H
