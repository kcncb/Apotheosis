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
        cv::Point2f center{};
        float       radius    = 0.0f;
        int         age       = 0; // consecutive frames seen
        int         unseen    = 0; // ticks since last seen
        bool        seen_this = false;
    };
    std::vector<Track> tracks_;
};

} // namespace crosshair

#endif // FLASHLIGHT_TRACKER_H
