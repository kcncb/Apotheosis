#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <opencv2/opencv.hpp>

// Per-track debug telemetry surfaced to the overlay / debug views. Populated by
// the live aim pipeline (boss::AimEngine via runtime/mouse_thread_loop.cpp's
// publish_boss_debug) and consumed by the debug renderers through
// g_trackerDebugTracks.
struct TrackDebugInfo
{
    int trackId = -1;
    int classId = -1;
    cv::Rect box;
    double pivotX = 0.0;
    double pivotY = 0.0;
    bool observedThisFrame = false;
    int missedFrames = 0;
    bool isLocked = false;
    // Threat scoring telemetry. `threat` is a [0,1] blended threat score;
    // `confidence` is the latest matched detection confidence; `depth_at_pivot`
    // is the normalized depth (0..1, 1 = closest) sampled at the track pivot, or
    // -1 when depth inference is unavailable. The live pipeline currently emits
    // neutral placeholders for `threat`/`depth_at_pivot`; the fields remain so
    // the debug renderers keep a stable layout.
    float threat = 0.5f;
    float confidence = 0.0f;
    float depth_at_pivot = -1.0f;
};

#endif // AIMBOTTARGET_H
