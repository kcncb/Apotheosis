#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AimbotTarget.h"
#include "active_hotkey.h"
#include "boss_aim.h"
#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "crosshair/flashlight_runtime.h"
#include "crosshair/glass_filter.h"
#include "crosshair/glass_runtime.h"
#include "mouse.h"
#include "Apotheosis.h"
#include "runtime/aim_telemetry.h"
#include "runtime/thread_loops.h"

// ─────────────────────────────────────────────────────────────────────────────
// Boss AI aim loop — implementation of boss_aim_algorithm.md.
//
// Replaces the old PID + Kalman + MultiTargetTracker + smart-trigger pipeline.
// The Boss engine (see mouse/boss_aim.h) owns target management and the closed-
// form P + velocity-feedforward controller. This file is now a thin shim:
//
//   1. wait for detection / hotkey change
//   2. snapshot hotkey + crosshair pivot (crosshair-color / laser if enabled)
//   3. BossAimEngine.tick()  →  (dx, dy, fire)
//   4. MouseThread.sendRawMove() + pressLeftButton/releaseLeftButton
//   5. publish boss tracks to g_trackerDebugTracks for the overlay
//
// Everything the doc hard-codes (M_DROP, ALPHA_HYST, MATCH_RATIO,
// FIRE_RADIUS_RATIO) lives inside the engine. The five aim knobs are
// in HotkeyProfile::aim_* and passed via EngineInput::aim.
// ─────────────────────────────────────────────────────────────────────────────

extern std::atomic<bool> shouldExit;

std::mutex g_trackerDebugMutex;
std::vector<TrackDebugInfo> g_trackerDebugTracks;
int g_trackerLockedId = -1;

// PID / IMM telemetry atomics that the legacy overlay panels still reference.
// Defined in mouse.cpp; the boss loop just keeps them at neutral values.
extern std::atomic<float> g_pid_last_err_px;
extern std::atomic<bool>  g_pid_mode_track;
extern std::atomic<bool>  g_threat_depth_required;
extern std::atomic<float> g_dynamic_fov_radius_x_px;
extern std::atomic<float> g_dynamic_fov_radius_y_px;
extern std::atomic<float> g_smart_trigger_hit_prob;
extern std::atomic<float> g_smart_trigger_recent_variance_px;

void createInputDevices();
void assignInputDevices();

namespace
{

struct PivotResolved
{
    double x = 0.0;
    double y = 0.0;
    bool   from_color = false;
};

PivotResolved resolve_crosshair_pivot(const HotkeyProfile* profile,
                                      int detection_resolution)
{
    const double centre = detection_resolution * 0.5;
    PivotResolved out;
    out.x = centre;
    out.y = centre;
    // 智能扳机默认接入准星找色:启用 smart_trigger 的热键自动消费 pivot
    // 快照(crosshair_runtime 那侧已经在 smart_trigger_enabled 时隐式开启
    // crosshair 通路),无需用户额外勾选 crosshair/laser detect。
    if (!profile || !(profile->crosshair_detect_enabled
                      || profile->laser_detect_enabled
                      || profile->smart_trigger_enabled))
        return out;

    const auto snap = crosshair_runtime::read();
    if (!snap.valid)
        return out;

    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - snap.ts).count();
    if (age > crosshair_runtime::kFreshnessMs)
        return out;

    out.x = snap.x;
    out.y = snap.y;
    out.from_color = true;
    return out;
}

struct AimSelection
{
    std::unordered_set<int> aim_class_ids;
    std::unordered_map<int, float> y_offsets;
    // class_id -> priority rank; 0 = top of aim_classes list, 1 = second, ...
    // Same class_id appearing twice keeps the FIRST (highest-priority) rank.
    std::unordered_map<int, int> class_priority;
};

AimSelection build_selection(const HotkeyProfile* profile)
{
    AimSelection sel;
    if (!profile)
        return sel;
    int rank = 0;
    for (const auto& ac : profile->aim_classes)
    {
        sel.aim_class_ids.insert(ac.class_id);
        sel.y_offsets[ac.class_id] = std::clamp(ac.y_offset, 0.0f, 1.0f);
        // first occurrence wins — preserves the head-first ordering even if
        // the user added the head class twice by mistake.
        sel.class_priority.emplace(ac.class_id, rank);
        ++rank;
    }
    return sel;
}

MouseRuntimeParams build_params(const HotkeyProfile* profile)
{
    MouseRuntimeParams p;
    p.detection_resolution = config.detection_resolution;

    if (!profile)
        return p;

    // Smart trigger:
    //   enable + hit_scale (X/Y 共用) 直接拷贝。
    //   reaction_ms 仍按 aggression [0..1] 反算: 0 → 80 ms, 1 → 0 ms。
    //   hold_ms / cooldown_ms 直接透传 (用户在 UI 里独立调)。
    p.smart_trigger_enabled     = profile->smart_trigger_enabled;
    p.smart_trigger_hit_scale_x = profile->smart_trigger_hit_scale;
    p.smart_trigger_hit_scale_y = profile->smart_trigger_hit_scale;

    const float a = std::clamp(profile->smart_trigger_aggression, 0.0f, 1.0f);
    p.smart_trigger_reaction_ms = static_cast<int>(std::lround(80.0f * (1.0f - a)));
    p.smart_trigger_hold_ms     = profile->smart_trigger_hold_ms;
    p.smart_trigger_cooldown_ms = profile->smart_trigger_cooldown_ms;

    return p;
}

void publish_boss_debug(const boss::AimEngine& engine)
{
    std::vector<TrackDebugInfo> out;
    const auto& tracks = engine.tracks();
    out.reserve(tracks.size());
    const int locked = engine.lockedTrackId();
    for (const auto& t : tracks)
    {
        TrackDebugInfo d;
        d.trackId = t.id;
        d.classId = t.class_id;
        d.box = cv::Rect(static_cast<int>(std::lround(t.bbox.x)),
                         static_cast<int>(std::lround(t.bbox.y)),
                         static_cast<int>(std::lround(t.bbox.width)),
                         static_cast<int>(std::lround(t.bbox.height)));
        d.pivotX = t.anchor.x;
        d.pivotY = t.anchor.y;
        d.observedThisFrame = t.observed_this_frame;
        d.missedFrames = t.missed;
        d.isLocked = (t.id == locked);
        d.threat = 0.5f;
        d.confidence = t.confidence;
        d.depth_at_pivot = -1.0f;
        out.push_back(std::move(d));
    }

    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
    g_trackerDebugTracks = std::move(out);
    g_trackerLockedId = locked;
}

} // namespace

void mouseThreadFunction(MouseThread& mouseThread)
{
    int lastVersion = -1;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::vector<float> confidences;

    boss::AimEngine engine;

    int last_hotkey_index_seen = -2;
    int last_resolution_seen = -1;
    // Smart trigger params we last pushed to MouseThread. Compared every
    // tick so UI edits take effect immediately (build_params is otherwise
    // only re-called on hotkey switch / resolution change). Updating
    // MouseRuntimeParams forces a trigger reset, so we GATE the call on
    // an actual value change to avoid killing an in-flight burst.
    bool  last_st_enabled   = false;
    float last_st_hit_scale = -1.0f;
    float last_st_aggression = -1.0f;
    int   last_st_hold_ms = -1;
    int   last_st_cooldown_ms = -1;

    auto last_tick_ts = std::chrono::steady_clock::time_point::min();

    // Legacy telemetry — boss loop leaves these neutral.
    g_pid_last_err_px.store(0.0f);
    g_pid_mode_track.store(false);
    g_threat_depth_required.store(false);
    g_smart_trigger_hit_prob.store(0.0f);
    g_smart_trigger_recent_variance_px.store(0.0f);

    while (!shouldExit && !session_stop_requested.load())
    {
        bool hasNewDetection = false;

        {
            std::unique_lock<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return detectionBuffer.version > lastVersion || shouldExit;
            });

            if (shouldExit) break;

            if (detectionBuffer.version > lastVersion)
            {
                boxes = detectionBuffer.boxes;
                classes = detectionBuffer.classes;
                confidences = detectionBuffer.confidences;
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
            }
        }

        // Glass filter — drop boxes whose edge ring is dominated by glass-
        // film colour (the打不穿玻璃后面识别到的人形)。Runs BEFORE flashlight
        // injection so the synthesized halo never goes through this gate
        // (its edges are by definition all white). Per-hotkey opt-in;
        // colour palette + thresholds are global. Latency: O(perimeter)
        // per box on CPU, all 20 boxes < 1 ms.
        if (hasNewDetection && !boxes.empty())
        {
            int filter_active_idx = runtime::g_active_hotkey_index.load();
            bool glass_on = false;
            crosshair::GlassFilterSettings gs;
            if (filter_active_idx >= 0)
            {
                std::lock_guard<std::recursive_mutex> cfg(configMutex);
                if (filter_active_idx < static_cast<int>(config.hotkeys.size())
                    && config.hotkeys[filter_active_idx].glass_filter_enabled)
                {
                    glass_on = true;
                    gs.enabled              = true;
                    gs.edge_ring_frac       = config.glass_edge_ring_frac;
                    gs.coverage_threshold   = config.glass_coverage_threshold;
                    gs.min_box_short_side   = config.glass_min_box_short_side;
                    gs.colors.reserve(config.glass_colors.size());
                    for (const auto& c : config.glass_colors)
                    {
                        crosshair::CrosshairColorBand b;
                        b.name = c.name; b.enabled = c.enabled;
                        b.h_low = c.h_low; b.h_high = c.h_high;
                        b.s_min = c.s_min; b.s_max = c.s_max;
                        b.v_min = c.v_min; b.v_max = c.v_max;
                        gs.colors.push_back(std::move(b));
                    }
                }
            }

            if (glass_on)
            {
                cv::Mat frame;
                {
                    std::lock_guard<std::mutex> lk(frameMutex);
                    frame = latestFrame;
                }
                if (!frame.empty() && frame.type() == CV_8UC3)
                {
                    static crosshair::GlassFilter s_filter;
                    glass_runtime::Snapshot snap;
                    snap.judgements.reserve(boxes.size());
                    std::vector<size_t> kill;
                    for (size_t i = 0; i < boxes.size(); ++i)
                    {
                        const auto r = s_filter.check(frame, boxes[i], gs);
                        glass_runtime::BoxJudgement bj;
                        bj.box = boxes[i] & cv::Rect(0, 0, frame.cols, frame.rows);
                        bj.coverage  = r.coverage;
                        bj.is_glass  = r.is_behind_glass;
                        bj.evaluated = r.evaluated;
                        snap.judgements.push_back(bj);
                        if (r.is_behind_glass) kill.push_back(i);
                    }
                    // Reverse erase 避免索引漂移。
                    for (auto it = kill.rbegin(); it != kill.rend(); ++it)
                    {
                        const size_t i = *it;
                        boxes.erase(boxes.begin() + i);
                        classes.erase(classes.begin() + i);
                        confidences.erase(confidences.begin() + i);
                    }
                    snap.ts = std::chrono::steady_clock::now();
                    glass_runtime::publish(std::move(snap));
                }
                else
                {
                    glass_runtime::publish(glass_runtime::Snapshot{});
                }
            }
            else
            {
                glass_runtime::publish(glass_runtime::Snapshot{});
            }
        }

        // Flashlight halo injection. The detector runs on the capture thread
        // and publishes a separate snapshot; we splice the halo into the local
        // detection arrays under the fixed `shoudiantong` class
        // (kFlashlightClassId) so the existing target tracker, FOV gates and
        // smart trigger handle it identically to a real model detection.
        // Per-hotkey opt-in (whether the detector even runs) is enforced in the
        // runtime; whether the halo is actually aimed is decided by the user
        // adding `shoudiantong` to this hotkey's aim_classes. So gate only on
        // freshness here — an unrouted class is ignored by the tracker, exactly
        // like any non-aim model class.
        {
            const auto fs = flashlight_runtime::read();
            const auto now = std::chrono::steady_clock::now();
            const bool fresh =
                fs.valid && fs.ts.time_since_epoch().count() != 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - fs.ts).count() < flashlight_runtime::kFreshnessMs;
            if (fresh && fs.box.area() > 0)
            {
                boxes.push_back(fs.box);
                classes.push_back(kFlashlightClassId);
                confidences.push_back(std::clamp(fs.confidence, 0.0f, 1.0f));
                hasNewDetection = true;
            }
        }

        if (input_method_changed.load())
        {
            createInputDevices();
            assignInputDevices();
            input_method_changed.store(false);
        }

        // Snapshot active hotkey.
        HotkeyProfile profile_snapshot;
        const HotkeyProfile* profile_ptr = nullptr;
        int active_idx = runtime::g_active_hotkey_index.load();
        {
            std::lock_guard<std::recursive_mutex> cfg(configMutex);
            if (active_idx >= 0 && active_idx < static_cast<int>(config.hotkeys.size()))
            {
                profile_snapshot = config.hotkeys[active_idx];
                profile_ptr = &profile_snapshot;
            }
        }
        aiming.store(profile_ptr != nullptr);

        const bool hotkey_changed = (active_idx != last_hotkey_index_seen);
        const bool resolution_changed = (config.detection_resolution != last_resolution_seen);

        bool st_changed = false;
        if (profile_ptr)
        {
            st_changed =
                (profile_ptr->smart_trigger_enabled     != last_st_enabled) ||
                (profile_ptr->smart_trigger_hit_scale   != last_st_hit_scale) ||
                (profile_ptr->smart_trigger_aggression  != last_st_aggression) ||
                (profile_ptr->smart_trigger_hold_ms     != last_st_hold_ms) ||
                (profile_ptr->smart_trigger_cooldown_ms != last_st_cooldown_ms);
        }

        if (hotkey_changed || resolution_changed || st_changed || detection_resolution_changed.load())
        {
            mouseThread.updateParams(build_params(profile_ptr));
            last_hotkey_index_seen = active_idx;
            last_resolution_seen = config.detection_resolution;
            detection_resolution_changed.store(false);
            if (profile_ptr)
            {
                last_st_enabled     = profile_ptr->smart_trigger_enabled;
                last_st_hit_scale   = profile_ptr->smart_trigger_hit_scale;
                last_st_aggression  = profile_ptr->smart_trigger_aggression;
                last_st_hold_ms     = profile_ptr->smart_trigger_hold_ms;
                last_st_cooldown_ms = profile_ptr->smart_trigger_cooldown_ms;
            }

            if (resolution_changed || hotkey_changed)
            {
                engine.reset();
                last_tick_ts = std::chrono::steady_clock::time_point::min();
                publish_boss_debug(engine);
            }
        }

        // No active hotkey → idle. Force-release the fire button and clear
        // any queued moves so the cursor stops drifting after the user lifts
        // the trigger.
        if (!profile_ptr)
        {
            mouseThread.forceTriggerRelease();
            mouseThread.clearQueuedMoves();
            mouseThread.setLockedTargetBox(0.0, 0.0, 0.0, 0.0);
            // Decay engine tracks gradually (still call tick with empty input
            // so the doc's miss-count purge runs).
            if (hasNewDetection)
            {
                boss::EngineInput in;
                in.boxes = &boxes;
                in.classes = &classes;
                in.confidences = &confidences;
                static const std::unordered_set<int> kNoClasses;
                in.eligible_classes = &kNoClasses;
                in.crosshair_x = config.detection_resolution * 0.5;
                in.crosshair_y = config.detection_resolution * 0.5;
                const auto now = std::chrono::steady_clock::now();
                double dt = 1.0 / 60.0;
                if (last_tick_ts != std::chrono::steady_clock::time_point::min())
                    dt = std::chrono::duration<double>(now - last_tick_ts).count();
                last_tick_ts = now;
                engine.tick(in, dt);
                publish_boss_debug(engine);
            }
            continue;
        }

        // Only step the engine when a fresh detection arrives. Without new
        // observations there's no Layer A work to do, and Layer B without an
        // updated anchor would just oscillate around stale data.
        if (!hasNewDetection)
            continue;

        const AimSelection selection = build_selection(profile_ptr);

        // Resolve live crosshair pivot (crosshair-color / laser-tip when the
        // hotkey opted in, otherwise detection-image centre).
        const auto pivot = resolve_crosshair_pivot(profile_ptr, config.detection_resolution);

        // FOV ellipse radii: half of the user's fovX / fovY. The engine drops
        // detections outside this ellipse before tracker association.
        const double fov_rx = std::max(1, profile_ptr->fovX) * 0.5;
        const double fov_ry = std::max(1, profile_ptr->fovY) * 0.5;
        g_dynamic_fov_radius_x_px.store(static_cast<float>(fov_rx));
        g_dynamic_fov_radius_y_px.store(static_cast<float>(fov_ry));

        boss::EngineInput in;
        in.boxes = &boxes;
        in.classes = &classes;
        in.confidences = &confidences;
        in.eligible_classes = &selection.aim_class_ids;
        in.y_offsets = &selection.y_offsets;
        in.class_priority = &selection.class_priority;
        in.crosshair_x = pivot.x;
        in.crosshair_y = pivot.y;
        in.fov_radius_x = fov_rx;
        in.fov_radius_y = fov_ry;
        in.image_size   = static_cast<double>(config.detection_resolution);
        in.aim.speed_x        = static_cast<double>(profile_ptr->speed_x);
        in.aim.speed_y        = static_cast<double>(profile_ptr->speed_y);
        in.aim.dead_zone_px   = static_cast<double>(profile_ptr->dead_zone_px);

        // 移动控制器选择 (0=微澜/Smooth, 1=疾风/Predictive)。
        // 疾风的 PID 参数与 ART 的 speed_x/y/dead_zone 互不影响。
        in.mover_kind = static_cast<mover::Kind>(
            std::clamp(profile_ptr->mover_kind, 0, 1));

        in.predictive_params.kp_x        = static_cast<double>(profile_ptr->predictive_kp_x);
        in.predictive_params.kp_y        = static_cast<double>(profile_ptr->predictive_kp_y);
        in.predictive_params.kd          = static_cast<double>(profile_ptr->predictive_kd);
        in.predictive_params.pred_weight = static_cast<double>(profile_ptr->predictive_pred_weight);

        // Trajectory shaper: forward the user's chosen mode + parameters.
        // Bezier / Custom build on the same speed_x/y/dead_zone knobs but
        // route through AimPathDriver instead of ART's direct drive.
        in.path.mode = static_cast<boss::AimPathDriver::Mode>(
            std::clamp(profile_ptr->aim_path_mode, 0, 2));
        in.path.speed_x      = static_cast<double>(profile_ptr->speed_x);
        in.path.speed_y      = static_cast<double>(profile_ptr->speed_y);
        in.path.dead_zone_px = static_cast<double>(profile_ptr->dead_zone_px);
        in.path.cx1 = static_cast<double>(profile_ptr->aim_path_bezier_cx1);
        in.path.cy1 = static_cast<double>(profile_ptr->aim_path_bezier_cy1);
        in.path.cx2 = static_cast<double>(profile_ptr->aim_path_bezier_cx2);
        in.path.cy2 = static_cast<double>(profile_ptr->aim_path_bezier_cy2);
        {
            const int N = boss::AimPathDriver::kCustomSamples;
            for (int i = 0; i < N; ++i)
            {
                in.path.custom_samples[i] =
                    (i < static_cast<int>(profile_ptr->aim_path_custom_samples.size()))
                        ? profile_ptr->aim_path_custom_samples[i]
                        : 0.0f;
            }
        }

        // dt = wall-clock since the previous engine tick. Doc §9 §11:
        // 「dt 必须用真实帧间隔,不要硬编 1/60」。
        const auto now = std::chrono::steady_clock::now();
        double dt = 1.0 / 60.0;
        if (last_tick_ts != std::chrono::steady_clock::time_point::min())
            dt = std::chrono::duration<double>(now - last_tick_ts).count();
        last_tick_ts = now;

        const boss::EngineOutput out = engine.tick(in, dt);

        publish_boss_debug(engine);

        // ─── Drive mouse ───
        if (out.have_target)
        {
            mouseThread.sendRawMove(out.dx, out.dy);

            // Smart trigger: hand the locked bbox (OBSERVED anchor + half
            // extents, in detection pixels) to the mouse thread, then run
            // the dwell/hold/cooldown state machine against the live
            // crosshair pivot. The trigger writes the LMB itself — there's
            // no separate boss "fire when close" path any more.
            mouseThread.setLockedTargetBox(
                out.anchor.x, out.anchor.y,
                out.bbox.width  * 0.5, out.bbox.height * 0.5);
            mouseThread.updateSmartTrigger(pivot.x, pivot.y);

            // Error telemetry for the legacy "real-time" indicator on the
            // PID/Track panel. Boss has a single control regime so the mode
            // flag stays at "Track" (always).
            const float err = static_cast<float>(std::hypot(
                out.anchor.x - pivot.x, out.anchor.y - pivot.y));
            g_pid_last_err_px.store(err);
            g_pid_mode_track.store(true);
        }
        else
        {
            // No target this frame → release the trigger but do not enqueue a
            // counter-move; the boss algo is "do nothing when no target".
            mouseThread.setLockedTargetBox(0.0, 0.0, 0.0, 0.0);
            mouseThread.forceTriggerRelease();
            mouseThread.clearQueuedMoves();
            g_pid_last_err_px.store(0.0f);
        }

        // ─── Replay buffer (one snapshot per detection) ─────────────────
        auto& replay = runtime::ReplayBuffer::instance();
        replay.setEnabled(config.replay_record_enabled);
        replay.setRetentionSeconds(config.replay_seconds);
        if (config.replay_record_enabled)
        {
            runtime::ReplayFrame f;
            f.ts = now;
            f.boxes = boxes;
            f.class_ids = classes;
            f.locked_track_id = out.current_track_id;
            f.hotkey_active = true;
            if (out.have_target)
            {
                f.pivot_x = out.anchor.x;
                f.pivot_y = out.anchor.y;
            }
            // Match detections to engine tracks by bbox-center proximity so the
            // overlay can colour the locked detection (mirrors the legacy IoU
            // match — close-enough centre = same track).
            f.track_ids.assign(f.boxes.size(), -1);
            const auto& tracks = engine.tracks();
            for (size_t bi = 0; bi < f.boxes.size(); ++bi)
            {
                const cv::Rect& b = f.boxes[bi];
                const double bcx = b.x + b.width  * 0.5;
                const double bcy = b.y + b.height * 0.5;
                double best_d2 = std::numeric_limits<double>::infinity();
                int best_id = -1;
                for (const auto& t : tracks)
                {
                    const double tcx = t.bbox.x + t.bbox.width  * 0.5;
                    const double tcy = t.bbox.y + t.bbox.height * 0.5;
                    const double dx = tcx - bcx;
                    const double dy = tcy - bcy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < best_d2)
                    {
                        best_d2 = d2;
                        best_id = t.id;
                    }
                }
                // Same threshold as MATCH_RATIO in the engine: bbox short edge × 0.5.
                const double thresh = std::max(b.width, b.height) * 0.5;
                if (best_id >= 0 && best_d2 < thresh * thresh)
                    f.track_ids[bi] = best_id;
            }
            replay.push(f);
        }
    }

    // On shutdown make absolutely sure the fire button is released.
    mouseThread.releaseLeftButton();
}
