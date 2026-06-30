#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#include "AimbotTarget.h"
#include "active_hotkey.h"
#include "boss_aim.h"
#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "crosshair/flashlight_runtime.h"
#include "crosshair/glass_filter.h"
#include "crosshair/glass_runtime.h"
#include "crosshair/glass_tuning.h"
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
extern std::atomic<float> g_dynamic_fov_radius_x_px;
extern std::atomic<float> g_dynamic_fov_radius_y_px;

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
    if (!profile || !(profile->crosshair_detect_enabled
                      || profile->laser_detect_enabled
                      || profile->trigger_enabled))
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

enum class TriggerPhase { Idle, Delay, Pressed, Cooldown };

struct TriggerState
{
    TriggerPhase phase = TriggerPhase::Idle;
    int64_t phase_time_ms = 0;
    void reset() { *this = {}; }
};

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

    auto last_tick_ts = std::chrono::steady_clock::time_point::min();

    TriggerState trigger;

    g_pid_last_err_px.store(0.0f);
    g_pid_mode_track.store(false);

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
                    // Single macro knob → concrete params (ring fixed, min-box
                    // auto-scaled to detection resolution). See glass_tuning.h.
                    const auto gd = crosshair::glass_derive_settings(
                        config.glass_filter_strength,
                        cv::Size(config.detection_resolution, config.detection_resolution));
                    gs.edge_ring_frac       = gd.edge_ring_frac;
                    gs.coverage_threshold   = gd.coverage_threshold;
                    gs.min_box_short_side   = gd.min_box_short_side;
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

        // Flashlight halo injection. The detector + depth-gate + temporal tracker
        // run on the capture thread and publish RANKED candidates; here we apply
        // the final accept rule that needs the model boxes: a halo is aimable when
        // it is COLOCATED with a real model detection (someone the model already
        // sees → trust it; the daytime case) OR it cleared the depth + time gates
        // (an orphan light in the dark → the discriminators vouch for it). The
        // best accepted halo is spliced in under the fixed `shoudiantong` class
        // (kFlashlightClassId) so the tracker / FOV / smart-trigger treat it like
        // a real model detection. Whether it is actually aimed still depends on
        // the user routing `shoudiantong` into a target slot.
        {
            constexpr float kColocateOverlapFrac = 0.10f; // halo∩box / halo-area
            constexpr float kColocateBoost       = 0.25f; // confidence bump when colocated

            const auto fs = flashlight_runtime::read();
            const auto now = std::chrono::steady_clock::now();
            const bool fresh =
                fs.valid && fs.ts.time_since_epoch().count() != 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - fs.ts).count() < flashlight_runtime::kFreshnessMs;
            if (fresh && !fs.spots.empty())
            {
                int   best_idx  = -1;
                float best_conf = 0.0f;
                for (size_t si = 0; si < fs.spots.size(); ++si)
                {
                    const auto& sp = fs.spots[si];
                    if (sp.box.area() <= 0)
                        continue;

                    // Colocation: does any non-flashlight model box overlap the
                    // halo enough? (boxes/classes here are the model detections —
                    // the flashlight class is not injected yet this frame.)
                    bool colocated = false;
                    for (size_t bi = 0; bi < boxes.size(); ++bi)
                    {
                        if (bi < classes.size() && classes[bi] == kFlashlightClassId)
                            continue;
                        const cv::Rect inter = sp.box & boxes[bi];
                        if (inter.area() > 0 &&
                            static_cast<float>(inter.area()) /
                                    static_cast<float>(sp.box.area()) >= kColocateOverlapFrac)
                        {
                            colocated = true;
                            break;
                        }
                    }

                    const bool accept = colocated || (sp.passed_depth && sp.confirmed);
                    if (!accept)
                        continue;

                    const float conf = std::clamp(
                        sp.confidence + (colocated ? kColocateBoost : 0.0f), 0.0f, 1.0f);
                    if (best_idx < 0 || conf > best_conf)
                    {
                        best_idx  = static_cast<int>(si);
                        best_conf = conf;
                    }
                }

                if (best_idx >= 0)
                {
                    boxes.push_back(fs.spots[static_cast<size_t>(best_idx)].box);
                    classes.push_back(kFlashlightClassId);
                    confidences.push_back(best_conf);
                    hasNewDetection = true;
                }
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

        if (hotkey_changed || resolution_changed || detection_resolution_changed.load())
        {
            MouseRuntimeParams rp;
            rp.detection_resolution = config.detection_resolution;
            mouseThread.updateParams(rp);
            last_hotkey_index_seen = active_idx;
            last_resolution_seen = config.detection_resolution;
            detection_resolution_changed.store(false);

            if (resolution_changed || hotkey_changed)
            {
                engine.reset();
                if (trigger.phase == TriggerPhase::Pressed)
                    mouseThread.releaseLeftButton();
                trigger.reset();
                last_tick_ts = std::chrono::steady_clock::time_point::min();
                publish_boss_debug(engine);
            }
        }

        // No active hotkey → idle. Force-release the fire button and clear
        // any queued moves so the cursor stops drifting after the user lifts
        // the trigger.
        if (!profile_ptr)
        {
            if (trigger.phase == TriggerPhase::Pressed)
                mouseThread.releaseLeftButton();
            trigger.reset();
            mouseThread.clearQueuedMoves();
            if (hasNewDetection)
            {
                boss::EngineInput in;
                in.boxes = &boxes;
                in.classes = &classes;
                in.confidences = &confidences;
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
        in.target_slots[0] = { profile_ptr->target_class_1, profile_ptr->target_y_top_1,
                               profile_ptr->target_y_bot_1, profile_ptr->target_min_conf_1 };
        in.target_slots[1] = { profile_ptr->target_class_2, profile_ptr->target_y_top_2,
                               profile_ptr->target_y_bot_2, profile_ptr->target_min_conf_2 };
        in.target_slots[2] = { profile_ptr->target_class_3, profile_ptr->target_y_top_3,
                               profile_ptr->target_y_bot_3, profile_ptr->target_min_conf_3 };
        in.target_aim_range = profile_ptr->target_aim_range;
        in.deadzone_enabled = profile_ptr->deadzone_enabled;
        in.deadzone_percent = profile_ptr->deadzone_percent;
        in.crosshair_x = pivot.x;
        in.crosshair_y = pivot.y;
        in.fov_radius_x = fov_rx;
        in.fov_radius_y = fov_ry;
        in.image_size   = static_cast<double>(config.detection_resolution);
        in.aim.speed_x        = static_cast<double>(profile_ptr->speed_x);
        in.aim.speed_y        = static_cast<double>(profile_ptr->speed_y);
        in.aim.dead_zone_px   = static_cast<double>(profile_ptr->dead_zone_px);

        // 移动控制器选择 (0=微澜/Smooth, 1=疾风/Predictive, 2=天枢/Classic)。
        in.mover_kind = static_cast<mover::Kind>(
            std::clamp(profile_ptr->mover_kind, 0, 2));

        in.predictive_params.kp_x        = static_cast<double>(profile_ptr->predictive_kp_x);
        in.predictive_params.kp_y        = static_cast<double>(profile_ptr->predictive_kp_y);
        in.predictive_params.kd          = static_cast<double>(profile_ptr->predictive_kd);
        in.predictive_params.pred_weight = static_cast<double>(profile_ptr->predictive_pred_weight);

        // 天枢参数
        {
            auto& cp = in.classic_params;
            cp.aim_mode              = profile_ptr->classic_aim_mode;
            cp.simple_start_speed    = static_cast<double>(profile_ptr->classic_simple_start_speed);
            cp.simple_end_speed      = static_cast<double>(profile_ptr->classic_simple_end_speed);
            cp.simple_transition_ms  = profile_ptr->classic_simple_transition_ms;
            cp.simple_ki             = static_cast<double>(profile_ptr->classic_simple_ki);
            cp.simple_kd             = static_cast<double>(profile_ptr->classic_simple_kd);
            cp.adv_kpmin_x   = static_cast<double>(profile_ptr->classic_adv_kpmin_x);
            cp.adv_kpmax_x   = static_cast<double>(profile_ptr->classic_adv_kpmax_x);
            cp.adv_ki_x      = static_cast<double>(profile_ptr->classic_adv_ki_x);
            cp.adv_kd_x      = static_cast<double>(profile_ptr->classic_adv_kd_x);
            cp.adv_imax_x    = static_cast<double>(profile_ptr->classic_adv_imax_x);
            cp.adv_pfactor_x = static_cast<double>(profile_ptr->classic_adv_pfactor_x);
            cp.adv_time_x    = profile_ptr->classic_adv_time_x;
            cp.adv_time_dynamic_x = profile_ptr->classic_adv_time_dynamic_x;
            cp.adv_kpmin_y   = static_cast<double>(profile_ptr->classic_adv_kpmin_y);
            cp.adv_kpmax_y   = static_cast<double>(profile_ptr->classic_adv_kpmax_y);
            cp.adv_ki_y      = static_cast<double>(profile_ptr->classic_adv_ki_y);
            cp.adv_kd_y      = static_cast<double>(profile_ptr->classic_adv_kd_y);
            cp.adv_imax_y    = static_cast<double>(profile_ptr->classic_adv_imax_y);
            cp.adv_pfactor_y = static_cast<double>(profile_ptr->classic_adv_pfactor_y);
            cp.adv_time_y    = profile_ptr->classic_adv_time_y;
            cp.adv_time_dynamic_y = profile_ptr->classic_adv_time_dynamic_y;
            cp.prediction_mode       = profile_ptr->classic_prediction_mode;
            cp.velocity_lead_frames  = static_cast<double>(profile_ptr->classic_velocity_lead_frames);
            cp.independent_y         = profile_ptr->classic_independent_y;
            cp.kalman_q_pos      = static_cast<double>(profile_ptr->classic_kalman_q_pos);
            cp.kalman_q_vel      = static_cast<double>(profile_ptr->classic_kalman_q_vel);
            cp.kalman_r_obs      = static_cast<double>(profile_ptr->classic_kalman_r_obs);
            cp.kalman_lookahead  = static_cast<double>(profile_ptr->classic_kalman_lookahead);
        }

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

            // ─── 扳机 FSM ───
            if (profile_ptr->trigger_enabled)
            {
                const double tx_range = out.bbox.width  * profile_ptr->trigger_y_percent / 100.0 * 0.5;
                const double ty_range = out.bbox.height * profile_ptr->trigger_y_percent / 100.0 * 0.5;
                const bool in_zone = std::abs(pivot.x - static_cast<double>(out.anchor.x)) <= tx_range &&
                                     std::abs(pivot.y - static_cast<double>(out.anchor.y)) <= ty_range;

                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                switch (trigger.phase)
                {
                case TriggerPhase::Idle:
                    if (in_zone)
                    {
                        if (profile_ptr->trigger_fire_delay > 0)
                        {
                            trigger.phase = TriggerPhase::Delay;
                            trigger.phase_time_ms = now_ms;
                        }
                        else
                        {
                            mouseThread.pressLeftButton();
                            trigger.phase = TriggerPhase::Pressed;
                            trigger.phase_time_ms = now_ms;
                        }
                    }
                    break;
                case TriggerPhase::Delay:
                    if (!in_zone) { trigger.phase = TriggerPhase::Idle; break; }
                    if (now_ms - trigger.phase_time_ms >= profile_ptr->trigger_fire_delay)
                    {
                        mouseThread.pressLeftButton();
                        trigger.phase = TriggerPhase::Pressed;
                        trigger.phase_time_ms = now_ms;
                    }
                    break;
                case TriggerPhase::Pressed:
                    if (now_ms - trigger.phase_time_ms >= profile_ptr->trigger_fire_duration)
                    {
                        mouseThread.releaseLeftButton();
                        trigger.phase = TriggerPhase::Cooldown;
                        trigger.phase_time_ms = now_ms;
                    }
                    break;
                case TriggerPhase::Cooldown:
                    if (now_ms - trigger.phase_time_ms >= profile_ptr->trigger_fire_interval)
                        trigger.phase = TriggerPhase::Idle;
                    break;
                }
            }
            else
            {
                if (trigger.phase == TriggerPhase::Pressed)
                    mouseThread.releaseLeftButton();
                trigger.reset();
            }

            const float err = static_cast<float>(std::hypot(
                out.anchor.x - pivot.x, out.anchor.y - pivot.y));
            g_pid_last_err_px.store(err);
            g_pid_mode_track.store(true);
        }
        else
        {
            if (trigger.phase == TriggerPhase::Pressed)
                mouseThread.releaseLeftButton();
            trigger.reset();
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
