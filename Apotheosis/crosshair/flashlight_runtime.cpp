#include "flashlight_runtime.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "Apotheosis.h"
#include "config.h"
#include "runtime/config_snapshot.h"
#include "depth/depth_mask.h"
#include "flashlight_detector.h"
#include "flashlight_tracker.h"
#include "flashlight_tuning.h"
#include "runtime/active_hotkey.h"
#include "runtime/event_orchestrator.h"

// Defined here; declared in Apotheosis.h. Tells the capture thread to produce the
// normalized depth map while the 寻光 depth-gate is active (depth.mode > 0).
std::atomic<bool> g_flashlight_depth_required{false};

namespace flashlight_runtime
{

namespace
{
std::mutex                    g_mtx;
Snapshot                      g_snap{};
crosshair::FlashlightDetector g_detector;
crosshair::FlashlightTracker  g_tracker; // capture-thread only (process_frame)
} // namespace

Snapshot read()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void publish(const Snapshot& snap)
{
    // 事件编排:发布完整上升/下降边沿，供“持续期间循环”可靠启停。
    bool rising = false;
    bool falling = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const bool prev_valid = g_snap.valid && !g_snap.spots.empty();
        const bool now_valid  = snap.valid   && !snap.spots.empty();
        rising = now_valid && !prev_valid;
        falling = !now_valid && prev_valid;
        g_snap = snap;
    }
    if (rising) event_orch::publish(event_orch::EventType::FlashlightHit);
    if (falling) event_orch::publish(event_orch::EventType::FlashlightLost);
}

void process_frame(const cv::Mat& bgrFrame)
{
    // Default: don't request depth. Set true below once we know the gate engages.
    ::g_flashlight_depth_required.store(false);

    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        g_tracker.reset();
        publish(Snapshot{});
        return;
    }

    // Active-hotkey gate (per-hotkey opt-in). Clear + reset tracks when off so
    // stale tracks never bleed across an enable/disable.
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0)
    {
        g_tracker.reset();
        publish(Snapshot{});
        return;
    }

    int sensitivity = 50;
    int reject      = 50;
    int spot_size   = 50;
    {
        const auto snapshot = runtime_config::read();
        const auto& cfg = *snapshot;
        if (active_idx >= static_cast<int>(cfg.hotkeys.size()))
        {
            g_tracker.reset();
            publish(Snapshot{});
            return;
        }
        const auto& hk = cfg.hotkeys[active_idx];
        if (!hk.flashlight_detect_enabled)
        {
            g_tracker.reset();
            publish(Snapshot{});
            return;
        }
        sensitivity = cfg.flashlight_sensitivity;
        reject      = cfg.flashlight_reject_strength;
        spot_size   = cfg.flashlight_spot_size;
    }

    const crosshair::FlashlightTuning tuning =
        crosshair::flashlight_derive_tuning(sensitivity, reject, spot_size, bgrFrame.size());

    // Request normalized depth from the capture thread iff the depth-gate engages.
    ::g_flashlight_depth_required.store(tuning.depth.mode > 0);

    const auto raw = g_detector.detectAll(bgrFrame, tuning.det);
    if (raw.empty())
    {
        // Tick the tracker with no spots so ages decay and dead tracks drop.
        g_tracker.update({}, {}, tuning.temporal);
        publish(Snapshot{});
        return;
    }

    // ---- Depth-gate prep (杀太阳/天空/远处泛光) ----
    cv::Mat depthN;
    bool    depth_map_ok = false;
    bool    sky_present  = false;
    if (tuning.depth.mode > 0)
    {
        depthN = depth_anything::GetDepthMaskGenerator().getDepthNormalized();
        depth_map_ok = !depthN.empty() && depthN.type() == CV_8UC1 &&
                       depthN.size() == bgrFrame.size();
        if (depth_map_ok)
        {
            // depthN: 255 = nearest, 0 = farthest (per-frame relative). A large
            // far-cluster means open sky is in view → safe to HARD-reject far
            // spots; without it we only soft-penalize (avoids killing a real
            // enemy at the far end of an indoor corridor).
            const cv::Mat farMask = depthN <= tuning.depth.far_level;
            const double frac = static_cast<double>(cv::countNonZero(farMask)) /
                                static_cast<double>(depthN.rows * depthN.cols);
            sky_present = frac >= tuning.depth.sky_cluster_frac;
        }
    }

    // ---- Temporal tracker (杀水面/玻璃闪烁反光) ----
    std::vector<cv::Point2f> centers;
    std::vector<float>       radii;
    centers.reserve(raw.size());
    radii.reserve(raw.size());
    for (const auto& s : raw)
    {
        centers.push_back(s.center);
        radii.push_back(s.radius);
    }
    const auto verdicts = g_tracker.update(centers, radii, tuning.temporal);

    // ---- Score + gate each spot ----
    const cv::Rect frame_rect(0, 0, bgrFrame.cols, bgrFrame.rows);
    std::vector<Spot> spots;
    spots.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const auto& s = raw[i];
        const crosshair::FlashlightTrackVerdict v =
            (i < verdicts.size()) ? verdicts[i] : crosshair::FlashlightTrackVerdict{};

        bool  passed_depth = true;
        float penalty      = 0.0f;
        if (tuning.depth.mode > 0)
        {
            if (depth_map_ok)
            {
                const int cx = std::clamp(static_cast<int>(std::lround(s.center.x)), 0, depthN.cols - 1);
                const int cy = std::clamp(static_cast<int>(std::lround(s.center.y)), 0, depthN.rows - 1);
                const bool is_far = depthN.at<uint8_t>(cy, cx) <= tuning.depth.far_level;
                if (is_far)
                {
                    if (tuning.depth.mode == 2 && sky_present)
                        passed_depth = false; // hard-reject sun/sky (loop can still
                                              // rescue via colocation)
                    else
                        penalty += tuning.depth.soft_penalty;
                }
            }
            else if (tuning.depth.top_band_penalty > 0.0f)
            {
                // No usable depth map → positional fallback: sun/sky sit high in
                // the frame, so penalize the top band.
                if (s.center.y < bgrFrame.rows * 0.30f)
                    penalty += tuning.depth.top_band_penalty;
            }
        }

        float conf = s.confidence - penalty;
        if (v.onset)     conf += tuning.temporal.onset_bonus;
        if (v.confirmed) conf += tuning.temporal.confirmed_bonus;
        conf = std::clamp(conf, 0.0f, 1.0f);

        if (conf < tuning.accept_conf_floor)
            continue;

        // Use the tracker's EMA-smoothed centre/radius (kills 1-3 px per-frame
        // contour-moment jitter that the mover's velocity-adaptive one-euro
        // would otherwise pass straight to the aim output during camera
        // rotation — "疾风" buzz when a halo is the active target). Onset
        // frames have smoothed == raw so the first lock is instant.
        const cv::Point2f sc = v.smoothed_center;
        const float       sr = (v.smoothed_radius > 0.0f) ? v.smoothed_radius : s.radius;

        const int diameter = std::max(2, static_cast<int>(std::lround(sr * 2.0f)));
        const int bx = static_cast<int>(std::lround(sc.x)) - diameter / 2;
        const int by = static_cast<int>(std::lround(sc.y)) - diameter / 2;
        const cv::Rect clipped = cv::Rect(bx, by, diameter, diameter) & frame_rect;
        if (clipped.area() <= 0)
            continue;

        Spot out;
        out.box          = clipped;
        out.center       = sc;
        out.radius       = sr;
        out.confidence   = conf;
        out.passed_depth = passed_depth;
        out.confirmed    = v.confirmed;
        out.onset        = v.onset;
        spots.push_back(out);
    }

    if (spots.empty())
    {
        publish(Snapshot{});
        return;
    }

    std::sort(spots.begin(), spots.end(),
              [](const Spot& a, const Spot& b) { return a.confidence > b.confidence; });

    Snapshot snap;
    snap.valid = true;
    snap.spots = std::move(spots);
    snap.ts    = std::chrono::steady_clock::now();
    publish(snap);
}

} // namespace flashlight_runtime
