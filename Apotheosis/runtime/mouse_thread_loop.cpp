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
#include "crosshair/crosshair_detector.h"
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

// Build mouse runtime parameters from a hotkey, optionally honouring a
// per-class Kalman override. When `class_id >= 0` and the matching
// HotkeyAimClass has `kalman_override_enabled = true`, the five overridden
// Kalman noise/damping/velocity fields replace the hotkey-level values. All
// other Kalman knobs (enabled flag, warmup, delay compensation, reset
// timeout) stay at the hotkey level â€?they are orthogonal to per-class
// motion characteristics.
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
    p.pid.p = static_cast<double>(hk.pid_p);
    p.pid.p_x = static_cast<double>(hk.pid_p_x);
    p.pid.p_y = static_cast<double>(hk.pid_p_y);
    p.pid.i = static_cast<double>(hk.pid_i);
    p.pid.d = static_cast<double>(hk.pid_d);

    p.predictionInterval = hk.predictionInterval;

    // Flick / Track dual-mode wiring.
    p.flick_track_enabled       = hk.flick_track_enabled;
    p.pid_track.p               = static_cast<double>(hk.pid_track_p);
    p.pid_track.p_x             = static_cast<double>(hk.pid_track_p_x);
    p.pid_track.p_y             = static_cast<double>(hk.pid_track_p_y);
    p.pid_track.i               = static_cast<double>(hk.pid_track_i);
    p.pid_track.d               = static_cast<double>(hk.pid_track_d);
    p.flick_track_threshold_px  = hk.flick_track_threshold_px;
    p.flick_track_hysteresis_px = hk.flick_track_hysteresis_px;

    // Smart trigger wiring.
    p.smart_trigger_enabled          = hk.smart_trigger_enabled;
    p.smart_trigger_hit_radius_frac  = hk.smart_trigger_hit_radius_frac;
    p.smart_trigger_variance_max_px  = hk.smart_trigger_variance_max_px;
    p.smart_trigger_window_frames    = hk.smart_trigger_window_frames;
    p.smart_trigger_min_prob         = hk.smart_trigger_min_prob;
    p.smart_trigger_fire_duration_ms = hk.smart_trigger_fire_duration_ms;

    p.kalman.enabled = hk.kalman_enabled;
    p.kalman.process_noise_position = hk.kalman_process_noise_position;
    p.kalman.process_noise_velocity = hk.kalman_process_noise_velocity;
    p.kalman.measurement_noise = hk.kalman_measurement_noise;
    p.kalman.velocity_damping = hk.kalman_velocity_damping;
    p.kalman.max_velocity = hk.kalman_max_velocity;
    p.kalman.warmup_frames = hk.kalman_warmup_frames;
    p.kalman_compensate_detection_delay = hk.kalman_compensate_detection_delay;
    p.kalman_additional_prediction_ms = hk.kalman_additional_prediction_ms;
    p.kalman_reset_timeout_sec = hk.kalman_reset_timeout_sec;

    if (class_id >= 0)
    {
        for (const auto& ac : hk.aim_classes)
        {
            if (ac.class_id != class_id || !ac.kalman_override_enabled)
                continue;
            p.kalman.process_noise_position = ac.kalman_process_noise_position;
            p.kalman.process_noise_velocity = ac.kalman_process_noise_velocity;
            p.kalman.measurement_noise      = ac.kalman_measurement_noise;
            p.kalman.velocity_damping       = ac.kalman_velocity_damping;
            p.kalman.max_velocity           = ac.kalman_max_velocity;
            break;
        }
    }

    p.aim_lock_strength = hk.aim_lock_strength;

    return p;
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
    MultiTargetTracker targetTracker;
    std::optional<AimbotTarget> activeTarget;
    auto lastTrackerUpdate = std::chrono::steady_clock::time_point::min();

    int last_hotkey_index_seen = -2; // force first-pass update
    int last_resolution_seen = -1;
    int last_kalman_class_id = -1;   // track class id of last Kalman push so
                                     // we only re-apply on class change

    while (!shouldExit && !session_stop_requested.load())
    {
        bool hasNewDetection = false;
        bool hasAimObservation = false;

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
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
            }
        }

        if (input_method_changed.load())
        {
            createInputDevices();
            assignInputDevices();
            input_method_changed.store(false);
        }

        // Snapshot the currently-active hotkey under configMutex so the UI
        // can't mutate config.hotkeys out from under us while we read.
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
            aiming.store(profile_ptr != nullptr);
        }

        const bool hotkey_changed = (active_idx != last_hotkey_index_seen);
        const bool resolution_changed = (config.detection_resolution != last_resolution_seen);

        if (hotkey_changed || resolution_changed || detection_resolution_changed.load())
        {
            // On hotkey/resolution changes, reset Kalman baseline without a
            // class-specific override â€?the loop below will re-push per-class
            // params as soon as a locked target's class is known.
            MouseRuntimeParams params = build_params(profile_ptr, -1);
            mouseThread.updateParams(params);
            last_hotkey_index_seen = active_idx;
            last_resolution_seen = config.detection_resolution;
            last_kalman_class_id = -1;
            detection_resolution_changed.store(false);

            if (resolution_changed)
            {
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

            TrackerUpdate in;
            in.boxes = &boxes;
            in.classes = &classes;
            in.aim_class_ids = std::move(selection.aim_class_ids);
            in.y_offsets = std::move(selection.y_offsets);
            in.priority_class_ids = std::move(selection.priority_class_ids);
            in.screen_width = config.detection_resolution;
            in.screen_height = config.detection_resolution;
            in.keep_current_lock = has_active_profile;
            in.visibility_mask = visibilityMask.empty() ? nullptr : &visibilityMask;
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

            if (profile_ptr)
            {
                in.lock_switch_score_margin       = profile_ptr->lock_switch_score_margin;
                in.lock_switch_min_frames         = profile_ptr->lock_switch_min_frames;
                in.y_offset_size_decay_enabled    = profile_ptr->y_offset_size_decay_enabled;
                in.y_offset_size_decay_low_frac   = profile_ptr->y_offset_size_decay_low_frac;
                in.y_offset_size_decay_high_frac  = profile_ptr->y_offset_size_decay_high_frac;
                in.threat_priority_enabled        = profile_ptr->threat_priority_enabled;
                in.threat_weight                  = profile_ptr->threat_weight;
                in.threat_head_class_id           = profile_ptr->threat_head_class_id;
                in.threat_body_class_id           = profile_ptr->threat_body_class_id;
                in.fov_radius_x_px                = fov_rx;
                in.fov_radius_y_px                = fov_ry;
            }
            targetTracker.update(in);
            lastTrackerUpdate = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks = targetTracker.getDebugTracks();
                g_trackerLockedId = targetTracker.getLockedTrackId();
            }

            LockedTargetInfo lockInfo;
            if (has_active_profile && targetTracker.getLockedTarget(lockInfo) && lockInfo.observedThisFrame)
            {
                // Re-push mouse params when the locked target's class changes
                // so the per-class Kalman override (e.g. head vs body) takes
                // effect. updateParams already resets the Kalman state when
                // the noise params shift, which is desired here â€?we don't
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
                mouseThread.setLastTargetTime(std::chrono::steady_clock::now());
                mouseThread.setTargetDetected(true);

                // Hand the bbox half-extent (short side / 2) to the mouse
                // loop so the smart trigger can compute hit probability
                // without re-reading detection state. Using the short side
                // keeps tall-thin targets (players) from falsely claiming
                // head-sized hitboxes.
                const double half_extent = std::min(activeTarget->w, activeTarget->h) * 0.5;
                mouseThread.setLockedTargetBboxHalfExtent(half_extent);


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
            }

            // Aim-trajectory replay: push a single snapshot per detection
            // frame. Cheap when the buffer is disabled (early-out inside
            // push). The snapshot's hotkey_active reflects the live aiming
            // flag â€?what the user actually had pressed when this detection
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

        if (activeTarget)
        {
            const int fps = std::max(1, captureFps.load());
            const int staleMs = std::clamp(2000 / fps, 25, 180);
            if (std::chrono::steady_clock::now() - lastTrackerUpdate > std::chrono::milliseconds(staleMs))
            {
                activeTarget.reset();
                mouseThread.clearFuturePositions();
                mouseThread.setTargetDetected(false);
            }
        }

        // Crosshair color detector: snapshot the latest BGR frame and look
        // for a red reticle in the center ROI. Feed the hit (if any) as the
        // dynamic aim center so PID corrects toward the player's real
        // crosshair rather than the static image midpoint.
        {
            static crosshair::CrosshairDetector crosshairDetector;
            crosshair::CrosshairDetectorSettings ch_settings;
            // Toggle lives on the active hotkey; palette + rect + area come
            // from global config and are shared across hotkeys.
            const bool per_hotkey_enabled = profile_ptr && profile_ptr->crosshair_detect_enabled;
            {
                std::lock_guard<std::recursive_mutex> cfg(configMutex);
                ch_settings.enabled  = per_hotkey_enabled;
                ch_settings.rect_w   = config.crosshair_rect_w;
                ch_settings.rect_h   = config.crosshair_rect_h;
                ch_settings.min_area = config.crosshair_min_area;
                ch_settings.max_area = config.crosshair_max_area;
                ch_settings.colors.reserve(config.crosshair_colors.size());
                for (const auto& c : config.crosshair_colors)
                {
                    crosshair::CrosshairColorBand b;
                    b.name    = c.name;
                    b.enabled = c.enabled;
                    b.h_low   = c.h_low;
                    b.h_high  = c.h_high;
                    b.s_min   = c.s_min;
                    b.s_max   = c.s_max;
                    b.v_min   = c.v_min;
                    b.v_max   = c.v_max;
                    ch_settings.colors.push_back(std::move(b));
                }
            }

            std::optional<std::pair<double, double>> dyn_center;
            if (ch_settings.enabled && has_active_profile)
            {
                cv::Mat frame_snapshot;
                {
                    std::lock_guard<std::mutex> lk(frameMutex);
                    if (!latestFrame.empty())
                        frame_snapshot = latestFrame.clone();
                }
                if (!frame_snapshot.empty())
                {
                    auto hit = crosshairDetector.detect(frame_snapshot, ch_settings);
                    if (hit)
                        dyn_center = std::make_pair(static_cast<double>(hit->x),
                                                    static_cast<double>(hit->y));
                }
            }
            mouseThread.setDynamicAimCenter(dyn_center);
        }

        if (has_active_profile)
        {
            if (activeTarget && hasAimObservation)
            {
                mouseThread.moveMouseToObservedTarget(activeTarget->pivotX, activeTarget->pivotY);
            }
            else
            {
                // Fallback path: PID driven by Kalman extrapolation if the
                // user opted into Kalman. If Kalman is off or no prior
                // observation exists, we just stop sending moves.
                if (!mouseThread.moveMouseToPredictedTarget())
                    mouseThread.clearQueuedMoves();
            }
        }
        else
        {
            mouseThread.clearQueuedMoves();
        }

        mouseThread.checkAndResetPredictions();
    }
}
