#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AimbotTarget.h"
#include "active_hotkey.h"
#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "depth/depth_mask.h"
#include "mouse.h"
#include "Apotheosis.h"
#include "runtime/aim_telemetry.h"
#include "runtime/thread_loops.h"

extern std::atomic<bool> shouldExit;

std::mutex g_trackerDebugMutex;
std::vector<TrackDebugInfo> g_trackerDebugTracks;
int g_trackerLockedId = -1;

void createInputDevices();
void assignInputDevices();

namespace
{

// Build mouse runtime parameters from a hotkey. `class_id` is accepted for
// call-site compatibility but no longer used — per-class Kalman overrides were
// removed in the simplification (Kalman is now just 启用/平滑度/预测提前量).
MouseRuntimeParams build_params(const HotkeyProfile* profile, int class_id = -1)
{
    MouseRuntimeParams p;
    p.detection_resolution = config.detection_resolution;

    // When no hotkey is active the exact mouse params do not matter (we
    // aren't sending moves anyway); we fall back to the HotkeyProfile's
    // default initializer so PID / Kalman still have sane state.
    const HotkeyProfile fallback{};
    const HotkeyProfile& hk = profile ? *profile : fallback;

    p.fovX = hk.fovX;
    p.fovY = hk.fovY;
    // PID gains map directly (no scaling). err in detection pixels → raw
    // controller output → lround → driver counts. X / Y 各一套,每轴仅 P/I/D。
    p.pid_x.p = static_cast<double>(hk.pid_x_p);
    p.pid_x.i = static_cast<double>(hk.pid_x_i);
    p.pid_x.d = static_cast<double>(hk.pid_x_d);
    p.pid_y.p = static_cast<double>(hk.pid_y_p);
    p.pid_y.i = static_cast<double>(hk.pid_y_i);
    p.pid_y.d = static_cast<double>(hk.pid_y_d);

    p.predictionInterval = hk.predictionInterval;

    // Smart trigger wiring.
    p.smart_trigger_enabled      = hk.smart_trigger_enabled;
    p.smart_trigger_hit_scale_x  = hk.smart_trigger_hit_scale_x;
    p.smart_trigger_hit_scale_y  = hk.smart_trigger_hit_scale_y;
    p.smart_trigger_reaction_ms  = hk.smart_trigger_reaction_ms;
    p.smart_trigger_hold_ms      = hk.smart_trigger_hold_ms;
    p.smart_trigger_cooldown_ms  = hk.smart_trigger_cooldown_ms;

    // 卡尔曼:仅 启用 / 平滑度 / 预测提前量。时序相关项固定为合理默认(内部不再暴露)。
    p.kalman.enabled    = hk.kalman_enabled;
    p.kalman.smoothness = hk.kalman_smoothness;
    p.kalman.lead       = hk.kalman_lead;
    p.kalman_compensate_detection_delay = true;
    p.kalman_additional_prediction_ms   = 0.0f;
    p.kalman_reset_timeout_sec          = 0.5f;

    (void)class_id;   // 不再有每类别覆盖
    return p;
}

// Resolve the crosshair pivot for THIS tick. If the active hotkey opted into
// crosshair-color and the capture-thread snapshot is fresh AND a hit was
// found, use the detected reticle position. Otherwise fall back to the
// detection-image centre.
struct PivotResolved
{
    double x = 0.0;
    double y = 0.0;
    bool   from_color = false;
    std::chrono::steady_clock::time_point snap_ts{};   // only set when from_color = true
};

PivotResolved resolve_crosshair_pivot(const HotkeyProfile* profile,
                                      int detection_resolution)
{
    const double centre = detection_resolution * 0.5;
    PivotResolved out;
    out.x = centre;
    out.y = centre;
    // Use the published colour pivot when EITHER crosshair-colour or laser
    // detection is enabled for this hotkey (the runtime already applies the
    // crosshair-priority / laser-fallback policy before publishing).
    if (!profile || !(profile->crosshair_detect_enabled || profile->laser_detect_enabled))
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
    out.snap_ts    = snap.ts;
    return out;
}

struct AimSelection
{
    std::unordered_set<int> aim_class_ids;
    std::unordered_map<int, float> y_offsets;
    std::vector<int> priority_class_ids;
};

AimSelection build_selection(const HotkeyProfile* profile)
{
    AimSelection sel;
    if (!profile)
        return sel;
    sel.priority_class_ids.reserve(profile->aim_classes.size());
    for (const auto& ac : profile->aim_classes)
    {
        sel.aim_class_ids.insert(ac.class_id);
        sel.y_offsets[ac.class_id] = std::clamp(ac.y_offset, 0.0f, 1.0f);
        sel.priority_class_ids.push_back(ac.class_id);
    }
    return sel;
}

} // namespace

void mouseThreadFunction(MouseThread& mouseThread)
{
    int lastVersion = -1;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::vector<float> confidences;
    MultiTargetTracker targetTracker;
    std::optional<AimbotTarget> activeTarget;
    // Wall-clock of the last frame our LOCKED target was actually observed
    // (not merely "some detection arrived"). activeTarget staleness keys off
    // this so a vanished target releases promptly even while other targets
    // keep producing detections.
    auto lastAimObserved = std::chrono::steady_clock::time_point::min();

    int last_hotkey_index_seen = -2; // force first-pass update
    int last_resolution_seen = -1;
    int last_kalman_class_id = -1;   // track class id of last Kalman push so
                                     // we only re-apply on class change
    int last_locked_track_id = -1;   // id of the track we last aimed at; a
                                     // change means the lock handed off to a
                                     // new target and the predictor must be
                                     // reset so the old target's Kalman
                                     // position/velocity isn't applied to the
                                     // new one as a phantom jump

    // Track the last crosshair snapshot we acted on. When a new snapshot
    // arrives between detection events (recoil moved the visible reticle)
    // we re-push using the stale target — this is what doubles effective
    // control rate over a 120 Hz capture-card setup.
    auto last_consumed_pivot_ts = std::chrono::steady_clock::time_point::min();

    while (!shouldExit && !session_stop_requested.load())
    {
        bool hasNewDetection = false;
        bool hasAimObservation = false;
        bool detectionStale = false;

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

            // Inference stalled relative to the detector's own cadence: the
            // cached boxes/lock no longer reflect the live frame. Treat the
            // lock as "no target" below so we don't keep driving toward a
            // detection that's already gone.
            detectionStale = detectionBuffer.staleLocked();
        }

        if (input_method_changed.load())
        {
            createInputDevices();
            assignInputDevices();
            input_method_changed.store(false);
        }

        // Snapshot the currently-active hotkey under configMutex so the UI
        // can't mutate config.hotkeys out from under us while we read. Keep
        // the critical section to just the vector read; the atomic store and
        // anything that doesn't touch config.* is hoisted out so UI writes
        // aren't blocked on us.
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
            // On hotkey/resolution changes, reset Kalman baseline without a
            // class-specific override �?the loop below will re-push per-class
            // params as soon as a locked target's class is known.
            MouseRuntimeParams params = build_params(profile_ptr, -1);
            mouseThread.updateParams(params);
            last_hotkey_index_seen = active_idx;
            last_resolution_seen = config.detection_resolution;
            last_kalman_class_id = -1;
            detection_resolution_changed.store(false);

            if (resolution_changed)
            {
                last_locked_track_id = -1;
                targetTracker.reset();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks.clear();
                    g_trackerLockedId = -1;
                }
            }
        }

        AimSelection selection = build_selection(profile_ptr);
        const bool has_active_profile = profile_ptr != nullptr;

        if (hasNewDetection)
        {
            // Snapshot the current depth-suppression mask so pivots can be
            // biased toward the visible portion of a half-occluded target.
            // Size must match detection_resolution; otherwise we drop it and
            // fall back to geometric pivots inside the tracker.
            cv::Mat visibilityMask = getCurrentDetectionSuppressionMask();
            if (!visibilityMask.empty()
                && (visibilityMask.cols != config.detection_resolution
                    || visibilityMask.rows != config.detection_resolution))
            {
                visibilityMask.release();
            }

            // Snapshot the normalized depth map (CV_8UC1, 255 = closest) for
            // the threat-scoring "depth" term. Same dim/type validation as the
            // visibility mask; on mismatch drop and fall back to a neutral
            // depth score inside compute_threat_score.
            cv::Mat depthNormalized = depth_anything::GetDepthMaskGenerator().getDepthNormalized();
            if (!depthNormalized.empty()
                && (depthNormalized.cols != config.detection_resolution
                    || depthNormalized.rows != config.detection_resolution
                    || depthNormalized.type() != CV_8UC1))
            {
                depthNormalized.release();
            }

            TrackerUpdate in;
            in.boxes = &boxes;
            in.classes = &classes;
            in.confidences = &confidences;
            in.aim_class_ids = std::move(selection.aim_class_ids);
            in.y_offsets = std::move(selection.y_offsets);
            in.priority_class_ids = std::move(selection.priority_class_ids);
            in.screen_width = config.detection_resolution;
            in.screen_height = config.detection_resolution;
            in.keep_current_lock = has_active_profile;
            in.visibility_mask = visibilityMask.empty() ? nullptr : &visibilityMask;
            in.depth_normalized = depthNormalized.empty() ? nullptr : &depthNormalized;
            // Dynamic FOV: compute the effective aim region BEFORE
            // tracker.update() so the candidate gate is in effect this
            // frame. Uses last frame's locked target (one-frame delay,
            // acceptable). When no lock yet, falls back to base diameters.
            float fov_rx = 0.0f;
            float fov_ry = 0.0f;
            if (profile_ptr && profile_ptr->dynamic_fov_enabled)
            {
                const float base_rx = std::max(1.0f, profile_ptr->fovX * 0.5f);
                const float base_ry = std::max(1.0f, profile_ptr->fovY * 0.5f);
                LockedTargetInfo prev;
                if (targetTracker.getLockedTarget(prev))
                {
                    const double cxd = config.detection_resolution * 0.5;
                    const double cyd = config.detection_resolution * 0.5;
                    const double err_x = prev.target.pivotX - cxd;
                    const double err_y = prev.target.pivotY - cyd;
                    const double err   = std::hypot(err_x, err_y);
                    const double base_r = std::max(static_cast<double>(base_rx),
                                                   static_cast<double>(base_ry));
                    // shrink_alpha: 1 = pivot dead-on crosshair (tightest);
                    // 0 = pivot at base FOV edge or beyond (full base FOV).
                    const double shrink_alpha = std::clamp(1.0 - err / base_r, 0.0, 1.0);
                    const float margin = profile_ptr->dynamic_fov_margin_frac;
                    const float floor_rx = base_rx * profile_ptr->dynamic_fov_min_radius_frac;
                    const float floor_ry = base_ry * profile_ptr->dynamic_fov_min_radius_frac;
                    const float tight_rx = std::clamp(prev.target.w * margin * 0.5f,
                                                      floor_rx, base_rx);
                    const float tight_ry = std::clamp(prev.target.h * margin * 0.5f,
                                                      floor_ry, base_ry);
                    fov_rx = static_cast<float>(base_rx + (tight_rx - base_rx) * shrink_alpha);
                    fov_ry = static_cast<float>(base_ry + (tight_ry - base_ry) * shrink_alpha);
                }
                else
                {
                    fov_rx = base_rx;
                    fov_ry = base_ry;
                }
            }
            g_dynamic_fov_radius_x_px.store(fov_rx);
            g_dynamic_fov_radius_y_px.store(fov_ry);

            // Tell the capture thread whether the normalized depth map must be
            // produced for threat scoring (ratio < 1 => depth term has nonzero
            // weight), even when no depth display option is enabled. Cleared
            // when there is no active profile.
            g_threat_depth_required.store(
                profile_ptr != nullptr
                && profile_ptr->threat_priority_enabled
                && profile_ptr->threat_depth_head_ratio < 1.0f);

            if (profile_ptr)
            {
                in.lock_switch_score_margin       = profile_ptr->lock_switch_score_margin;
                in.lock_switch_min_frames         = profile_ptr->lock_switch_min_frames;
                in.lock_hold_min_frames           = profile_ptr->lock_hold_min_frames;
                in.y_offset_size_decay_enabled    = profile_ptr->y_offset_size_decay_enabled;
                in.y_offset_size_decay_low_frac   = profile_ptr->y_offset_size_decay_low_frac;
                in.y_offset_size_decay_high_frac  = profile_ptr->y_offset_size_decay_high_frac;
                in.threat_priority_enabled        = profile_ptr->threat_priority_enabled;
                in.threat_weight                  = profile_ptr->threat_weight;
                in.threat_head_class_id           = profile_ptr->threat_head_class_id;
                in.threat_depth_head_ratio        = profile_ptr->threat_depth_head_ratio;
                in.fov_radius_x_px                = fov_rx;
                in.fov_radius_y_px                = fov_ry;
                in.close_range_head_aim_enabled    = profile_ptr->close_range_head_aim_enabled;
                in.close_range_head_class_id       = profile_ptr->close_range_head_class_id;
                in.close_range_trigger_height_frac = profile_ptr->close_range_trigger_height_frac;
            }

            // Aim origin for target selection: rank candidates by proximity to
            // the live crosshair-colour pivot (where the reticle actually sits
            // after recoil), falling back to the detection-image centre when
            // crosshair-colour isn't active or hasn't hit this frame.
            {
                const auto sel_pivot =
                    resolve_crosshair_pivot(profile_ptr, config.detection_resolution);
                in.aim_origin_x = sel_pivot.x;
                in.aim_origin_y = sel_pivot.y;
            }

            targetTracker.update(in);

            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks = targetTracker.getDebugTracks();
                g_trackerLockedId = targetTracker.getLockedTrackId();
            }

            LockedTargetInfo lockInfo;
            if (has_active_profile && targetTracker.getLockedTarget(lockInfo) && lockInfo.observedThisFrame)
            {
                // Lock handoff. When the lock moves to a different track (the
                // previous target died/left and a new one was acquired) the
                // predictor still holds the OLD target's Kalman position and
                // velocity. Feeding the new target's pivot into that state
                // looks like a huge instantaneous jump, so the filter spits
                // out a wild velocity and the aim flings around / circles
                // while settling. Reset the predictor on the transition so the
                // new target starts from a clean state. (resetPrediction also
                // clears queued moves and target_detected_; we re-arm both
                // below.) Force a per-class param re-push too, since the reset
                // wiped the Kalman noise config.
                if (lockInfo.trackId != last_locked_track_id)
                {
                    mouseThread.resetPrediction();
                    last_locked_track_id = lockInfo.trackId;
                    last_kalman_class_id = -1;
                }

                // Re-push mouse params when the locked target's class changes
                // so the per-class Kalman override (e.g. head vs body) takes
                // effect. updateParams already resets the Kalman state when
                // the noise params shift, which is desired here �?we don't
                // want motion learned on one class's dynamics leaking into
                // another.
                const int locked_class = lockInfo.target.classId;
                if (locked_class != last_kalman_class_id)
                {
                    MouseRuntimeParams params = build_params(profile_ptr, locked_class);
                    mouseThread.updateParams(params);
                    last_kalman_class_id = locked_class;
                }

                activeTarget = lockInfo.target;
                hasAimObservation = true;
                lastAimObserved = std::chrono::steady_clock::now();
                mouseThread.setLastTargetTime(std::chrono::steady_clock::now());
                mouseThread.setTargetDetected(true);

                // Kalman measurement-noise scaling keys off the short-side
                // half-extent (tall-thin player boxes shouldn't claim large,
                // confident hitboxes for the filter).
                const double half_extent = std::min(activeTarget->w, activeTarget->h) * 0.5;
                mouseThread.setLockedTargetBboxHalfExtent(half_extent);

                // Hand the full observed box (aim anchor + per-axis half
                // extents) to the triggerbot so it can test the live
                // crosshair against the real, axis-aligned target rectangle.
                mouseThread.setLockedTargetBox(activeTarget->pivotX,
                                               activeTarget->pivotY,
                                               activeTarget->w * 0.5,
                                               activeTarget->h * 0.5);


                auto futurePositions = mouseThread.predictFuturePositions(
                    activeTarget->pivotX,
                    activeTarget->pivotY,
                    profile_snapshot.prediction_futurePositions
                );
                mouseThread.storeFuturePositions(futurePositions);
            }
            else if (!has_active_profile)
            {
                activeTarget.reset();
                mouseThread.clearFuturePositions();
                mouseThread.setTargetDetected(false);
                mouseThread.clearQueuedMoves();
                mouseThread.setLockedTargetBboxHalfExtent(0.0);
                mouseThread.setLockedTargetBox(0.0, 0.0, 0.0, 0.0);
            }

            // Aim-trajectory replay: push a single snapshot per detection
            // frame. Cheap when the buffer is disabled (early-out inside
            // push). The snapshot's hotkey_active reflects the live aiming
            // flag �?what the user actually had pressed when this detection
            // landed. Mouse dx/dy fields are 0 here; the per-step PID moves
            // are too high-frequency to mirror in this buffer cleanly. The
            // replay overlay relies on the pivot trail instead.
            auto& replay = runtime::ReplayBuffer::instance();
            replay.setEnabled(config.replay_record_enabled);
            replay.setRetentionSeconds(config.replay_seconds);
            if (config.replay_record_enabled)
            {
                runtime::ReplayFrame f;
                f.ts = std::chrono::steady_clock::now();
                f.boxes = boxes;
                f.class_ids = classes;
                f.locked_track_id = targetTracker.getLockedTrackId();
                f.hotkey_active = aiming.load();
                if (activeTarget)
                {
                    f.pivot_x = activeTarget->pivotX;
                    f.pivot_y = activeTarget->pivotY;
                }
                // Match boxes to track ids by IoU so the replay overlay can
                // colour the locked detection. The tracker stores a track
                // box per id; we just need each detection's nearest match.
                f.track_ids.assign(f.boxes.size(), -1);
                const auto debugTracks = targetTracker.getDebugTracks();
                for (size_t bi = 0; bi < f.boxes.size(); ++bi)
                {
                    const cv::Rect& b = f.boxes[bi];
                    int best = -1;
                    double bestIou = 0.30; // require meaningful overlap
                    for (const auto& dt : debugTracks)
                    {
                        const int x1 = std::max(b.x, dt.box.x);
                        const int y1 = std::max(b.y, dt.box.y);
                        const int x2 = std::min(b.x + b.width,  dt.box.x + dt.box.width);
                        const int y2 = std::min(b.y + b.height, dt.box.y + dt.box.height);
                        const int iw = std::max(0, x2 - x1);
                        const int ih = std::max(0, y2 - y1);
                        const double inter = iw * ih;
                        const double ua = b.area() + dt.box.area() - inter;
                        const double iou = (ua > 0.0) ? inter / ua : 0.0;
                        if (iou > bestIou)
                        {
                            bestIou = iou;
                            best = dt.trackId;
                        }
                    }
                    f.track_ids[bi] = best;
                }
                replay.push(f);
            }
        }

        // Detection went stale (inference stalled): drop the lock now instead
        // of waiting out the staleMs window. Without this the crosshair-feedback
        // re-push keeps oscillating toward the departed target until the next
        // detection lands — the "circles with nothing on screen" symptom.
        if (activeTarget && detectionStale)
        {
            activeTarget.reset();
            mouseThread.clearFuturePositions();
            mouseThread.setTargetDetected(false);
            mouseThread.setLockedTargetBox(0.0, 0.0, 0.0, 0.0);
        }

        if (activeTarget)
        {
            const int fps = std::max(1, captureFps.load());
            const int staleMs = std::clamp(2000 / fps, 25, 180);
            // Key off the locked target's last OBSERVATION, not the last
            // detection batch. Otherwise, when the first target disappears but
            // a replacement target keeps detections flowing, this never fires:
            // activeTarget stays pinned to the dead target's last position and
            // Path B keeps re-pushing toward that ghost point through the
            // crosshair-colour feedback loop — the "aim wobbles / circles in
            // place while hunting the next target" symptom. The short window
            // still preserves Path B re-push across brief recoil dropouts.
            if (std::chrono::steady_clock::now() - lastAimObserved > std::chrono::milliseconds(staleMs))
            {
                activeTarget.reset();
                mouseThread.clearFuturePositions();
                mouseThread.setTargetDetected(false);
                mouseThread.setLockedTargetBox(0.0, 0.0, 0.0, 0.0);
            }
        }

        if (has_active_profile)
        {
            const auto pivot = resolve_crosshair_pivot(profile_ptr, config.detection_resolution);
            const bool pivot_event = pivot.from_color && pivot.snap_ts != last_consumed_pivot_ts;

            if (activeTarget && hasAimObservation)
            {
                // Path A — fresh detection: feed Kalman, push controller.
                mouseThread.moveMouseToObservedTarget(activeTarget->pivotX,
                                                      activeTarget->pivotY,
                                                      pivot.x, pivot.y);
                if (pivot.from_color)
                    last_consumed_pivot_ts = pivot.snap_ts;
            }
            else if (activeTarget && pivot_event)
            {
                // Path B — fresh crosshair snapshot, no new detection.
                // Recoil moved the reticle: re-push using the stale target
                // (or Kalman extrapolation when enabled) without feeding
                // Kalman a duplicate observation.
                if (!mouseThread.moveMouseToPredictedTarget(pivot.x, pivot.y))
                {
                    mouseThread.moveMouseUsingLastTarget(activeTarget->pivotX,
                                                         activeTarget->pivotY,
                                                         pivot.x, pivot.y);
                }
                last_consumed_pivot_ts = pivot.snap_ts;
            }
            else if (!activeTarget)
            {
                // 目标已丢失:立即停止驱动,不再向卡尔曼最后锁定的位置外推。
                // 否则准星会滑向那个"幽灵点"(尤其打静止目标:目标消失后准星仍被
                // 推向它最后所在处,滑动一段距离,且与预测提前量无关 —— 之前的 bug)。
                // 短暂检测丢帧由上面"目标仍锁定"的 Path B 负责,这里只处理真正丢失。
                mouseThread.clearQueuedMoves();
            }
            // else: have target, no fresh event — sleep until something changes.
        }
        else
        {
            mouseThread.clearQueuedMoves();
        }

        mouseThread.checkAndResetPredictions();
    }
}
