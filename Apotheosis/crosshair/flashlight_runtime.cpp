#include "flashlight_runtime.h"

#include <algorithm>
#include <mutex>

#include "Apotheosis.h"
#include "config.h"
#include "flashlight_detector.h"
#include "runtime/active_hotkey.h"

namespace flashlight_runtime
{

namespace
{
std::mutex g_mtx;
Snapshot   g_snap{};
crosshair::FlashlightDetector g_detector;
} // namespace

Snapshot read()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void publish(const Snapshot& snap)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_snap = snap;
}

void process_frame(const cv::Mat& bgrFrame)
{
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        publish(Snapshot{});
        return;
    }

    // Active-hotkey gate. The flashlight halo is a per-hotkey opt-in so it
    // can't steal focus on hotkeys that shouldn't be aiming at lights (e.g.
    // a melee profile). When no hotkey is active or the active one has the
    // flag off, clear the snapshot and bail.
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0)
    {
        publish(Snapshot{});
        return;
    }

    crosshair::FlashlightDetectorSettings settings;
    bool enabled = false;
    {
        std::lock_guard<std::recursive_mutex> cfg(configMutex);
        if (active_idx >= static_cast<int>(config.hotkeys.size()))
        {
            publish(Snapshot{});
            return;
        }
        const auto& hk = config.hotkeys[active_idx];
        if (!hk.flashlight_detect_enabled)
        {
            publish(Snapshot{});
            return;
        }
        enabled = true;
        settings.enabled              = true;
        settings.brightness_threshold = config.flashlight_brightness_threshold;
        settings.min_radius           = config.flashlight_min_radius;
        settings.max_radius           = config.flashlight_max_radius;
        settings.min_circularity      = config.flashlight_min_circularity;
        settings.open_radius          = config.flashlight_open_radius;
        settings.min_local_contrast   = config.flashlight_min_local_contrast;
    }
    if (!enabled)
    {
        publish(Snapshot{});
        return;
    }

    const auto spots = g_detector.detectAll(bgrFrame, settings);
    if (spots.empty())
    {
        publish(Snapshot{});
        return;
    }

    const auto& s = spots.front();

    // Synthesize a bbox: a square of side 2*radius centred on the halo,
    // clamped into the frame so downstream box-arithmetic never sees
    // negative offsets.
    const int diameter = std::max(2, static_cast<int>(std::lround(s.radius * 2.0f)));
    int bx = static_cast<int>(std::lround(s.center.x)) - diameter / 2;
    int by = static_cast<int>(std::lround(s.center.y)) - diameter / 2;
    int bw = diameter;
    int bh = diameter;
    cv::Rect raw(bx, by, bw, bh);
    const cv::Rect frame_rect(0, 0, bgrFrame.cols, bgrFrame.rows);
    cv::Rect clipped = raw & frame_rect;
    if (clipped.area() <= 0)
    {
        publish(Snapshot{});
        return;
    }

    Snapshot snap;
    snap.valid      = true;
    snap.box        = clipped;
    snap.center     = s.center;
    snap.radius     = s.radius;
    snap.confidence = s.confidence;
    snap.ts         = std::chrono::steady_clock::now();
    publish(snap);
}

} // namespace flashlight_runtime
