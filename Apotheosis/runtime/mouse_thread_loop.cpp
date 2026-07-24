#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <vector>

#include "AimbotTarget.h"
#include "active_hotkey.h"
#include "aim_path.h"
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
#include "runtime/event_orchestrator.h"
#include "runtime/config_snapshot.h"
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

std::pair<double, double> resolve_fov_radii(const HotkeyProfile& profile,
                                            const PivotResolved& pivot,
                                            const boss::AimEngine& engine)
{
    const double base_rx = std::max(1, profile.fovX) * 0.5;
    const double base_ry = std::max(1, profile.fovY) * 0.5;
    const double strength = std::clamp(static_cast<double>(profile.dynamic_fov_strength), 0.0, 1.0);
    if (!profile.dynamic_fov_enabled || strength <= 0.0 || engine.lockedTrackId() < 0)
        return { base_rx, base_ry };

    const auto it = std::find_if(engine.tracks().begin(), engine.tracks().end(),
        [&engine](const boss::Track& track) { return track.id == engine.lockedTrackId(); });
    if (it == engine.tracks().end() || it->bbox.width <= 0.0f || it->bbox.height <= 0.0f)
        return { base_rx, base_ry };

    const double left = it->bbox.x;
    const double right = it->bbox.x + it->bbox.width;
    const double top = it->bbox.y;
    const double bottom = it->bbox.y + it->bbox.height;
    const double center_x = (left + right) * 0.5;
    const double center_y = (top + bottom) * 0.5;

    // A distant lock keeps the base region so the controller has room to
    // converge. As the lock approaches the pivot, contract towards an ellipse
    // that still fully contains the locked box. Strength controls both how
    // tight the target ellipse is and how much of that contraction is applied.
    const double normalized_distance = std::clamp(std::hypot(
        (center_x - pivot.x) / std::max(1.0, base_rx),
        (center_y - pivot.y) / std::max(1.0, base_ry)), 0.0, 1.0);
    const double contraction = strength * (1.0 - normalized_distance);
    const double margin = 2.0 - strength;
    const double min_radius_fraction = 0.50 - 0.40 * strength;

    const double tight_rx = std::clamp(std::max(
        base_rx * min_radius_fraction,
        std::max(std::abs(left - pivot.x), std::abs(right - pivot.x)) * margin),
        1.0, base_rx);
    const double tight_ry = std::clamp(std::max(
        base_ry * min_radius_fraction,
        std::max(std::abs(top - pivot.y), std::abs(bottom - pivot.y)) * margin),
        1.0, base_ry);

    return {
        base_rx + (tight_rx - base_rx) * contraction,
        base_ry + (tight_ry - base_ry) * contraction
    };
}

enum class TriggerPhase { Idle, Delay, Pressed, Cooldown, SwitchCooldown };

struct TriggerState
{
    TriggerPhase phase = TriggerPhase::Idle;
    int64_t phase_time_ms = 0;
    // in_zone 起点时间戳:进入 Idle 后一旦目标进入命中区就 stamp,
    // 使 (now - in_zone_since_ms) ≥ delay 立即触发,不再空转一帧。
    int64_t in_zone_since_ms = -1;
    // 上次开火 / 上次锁定的 target track id,用于检测转火并进入 SwitchCooldown。
    int last_fire_track_id = -1;
    // 本轮 phase 目标时长(delay/duration/interval)已经算入随机抖动,
    // 避免在同一 phase 里每帧重摇导致门槛漂移。
    int32_t phase_target_ms = 0;
    void reset() { *this = {}; }
};

// 对基础延迟做 ±jitter 抖动,结果不小于 0。thread_local RNG 避免锁竞争。
inline int jitter_ms(int base, int jitter)
{
    if (jitter <= 0) return std::max(0, base);
    thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> d(-jitter, jitter);
    return std::max(0, base + d(rng));
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
    std::vector<cv::Rect2f> precise_boxes;
    std::vector<int> classes;
    std::vector<float> confidences;

    boss::AimEngine engine;
    boss::AimPathDriver aim_path_driver;

    int last_hotkey_index_seen = -2;
    int last_resolution_seen = -1;

    auto last_tick_ts = std::chrono::steady_clock::time_point::min();

    TriggerState trigger;

    // 事件编排状态:跨帧追踪 target 锁定 id 与鼠标 fire 状态。
    int  ev_last_track_id = -1;   // -1 = 无锁定
    bool ev_fire_pressed  = false;

    g_pid_last_err_px.store(0.0f);
    g_pid_mode_track.store(false);

    while (!shouldExit && !session_stop_requested.load())
    {
        bool hasNewDetection = false;
        double detection_age_ms = 0.0;
        double detection_interval_ms = 0.0;

        {
            std::unique_lock<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return detectionBuffer.version > lastVersion || shouldExit;
            });

            if (shouldExit) break;

            if (detectionBuffer.version > lastVersion)
            {
                boxes = detectionBuffer.boxes;
                precise_boxes = detectionBuffer.precise_boxes;
                if (precise_boxes.size() != boxes.size()) {
                    precise_boxes.clear();
                    precise_boxes.reserve(boxes.size());
                    for (const auto& box : boxes) {
                        precise_boxes.emplace_back(
                            static_cast<float>(box.x), static_cast<float>(box.y),
                            static_cast<float>(box.width), static_cast<float>(box.height));
                    }
                }
                classes = detectionBuffer.classes;
                confidences = detectionBuffer.confidences;
                const size_t aligned = std::min({ boxes.size(), classes.size(), confidences.size() });
                boxes.resize(aligned);
                precise_boxes.resize(aligned);
                classes.resize(aligned);
                confidences.resize(aligned);
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
            }
            detection_interval_ms = detectionBuffer.last_interval_ms;
            if (detectionBuffer.stamp.time_since_epoch().count() != 0)
                detection_age_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - detectionBuffer.stamp).count();
        }

        // 只用硬件工作线程已完成的位移更新控制器测量状态。
        // 队列中尚未发送的命令不再被误当成真实鼠标位移。
        const auto movement_feedback = mouseThread.consumeMovementFeedback();
        g_mouse_queue_latency_ms.store(static_cast<float>(movement_feedback.latency_ms));
        g_mouse_queue_backlog.store(static_cast<int>(movement_feedback.backlog));
        g_mouse_send_failures.store(movement_feedback.failed);
        const auto config_snapshot = runtime_config::read();

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
                if (filter_active_idx < static_cast<int>(config_snapshot->hotkeys.size())
                    && config_snapshot->hotkeys[filter_active_idx].glass_filter_enabled)
                {
                    glass_on = true;
                    gs.enabled              = true;
                    // Single macro knob → concrete params (ring fixed, min-box
                    // auto-scaled to detection resolution). See glass_tuning.h.
                    const auto gd = crosshair::glass_derive_settings(
                        config_snapshot->glass_filter_strength,
                        cv::Size(config_snapshot->detection_resolution, config_snapshot->detection_resolution));
                    gs.edge_ring_frac       = gd.edge_ring_frac;
                    gs.coverage_threshold   = gd.coverage_threshold;
                    gs.min_box_short_side   = gd.min_box_short_side;
                    gs.colors.reserve(config_snapshot->glass_colors.size());
                    for (const auto& c : config_snapshot->glass_colors)
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
                        precise_boxes.erase(precise_boxes.begin() + i);
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

        // 寻光与 YOLO 发布严格同频：每个新推理结果只处理一次当前检测画面。
        // 若光核与模型框关联，只发布预览圆圈，原模型框原样进入瞄准管线，因而
        // 类别优先级、锁点偏移和置信度规则完全不被寻光篡改。只有没有关联框、
        // 且连续三次推理都通过严格判别的光核，才注入 shoudiantong 独立目标。
        if (hasNewDetection)
        {
            cv::Mat flashlight_frame;
            {
                std::lock_guard<std::mutex> lk(frameMutex);
                flashlight_frame = latestFrame;
            }
            flashlight_runtime::process_inference_frame(flashlight_frame, boxes);
            const auto fs = flashlight_runtime::read();
            if (fs.valid && !fs.spots.empty())
            {
                const auto& sp = fs.spots.front();
                if (sp.independent_aimable && sp.box.area() > 0)
                {
                    boxes.push_back(sp.box);
                    precise_boxes.emplace_back(
                        static_cast<float>(sp.box.x), static_cast<float>(sp.box.y),
                        static_cast<float>(sp.box.width), static_cast<float>(sp.box.height));
                    classes.push_back(kFlashlightClassId);
                    confidences.push_back(sp.confidence);
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
        const HotkeyProfile* profile_ptr = nullptr;
        const int config_resolution = config_snapshot->detection_resolution;
        const float config_confidence = static_cast<float>(config_snapshot->confidence_threshold);
        const bool replay_enabled = config_snapshot->replay_record_enabled;
        const int replay_seconds = config_snapshot->replay_seconds;
        int active_idx = runtime::g_active_hotkey_index.load();
        if (active_idx >= 0 && active_idx < static_cast<int>(config_snapshot->hotkeys.size()))
            profile_ptr = &config_snapshot->hotkeys[active_idx];
        aiming.store(profile_ptr != nullptr);

        const bool hotkey_changed = (active_idx != last_hotkey_index_seen);
        const bool resolution_changed = (config_resolution != last_resolution_seen);

        if (hotkey_changed || resolution_changed || detection_resolution_changed.load())
        {
            MouseRuntimeParams rp;
            rp.detection_resolution = config_resolution;
            mouseThread.updateParams(rp);
            last_hotkey_index_seen = active_idx;
            last_resolution_seen = config_resolution;
            detection_resolution_changed.store(false);

            if (resolution_changed || hotkey_changed)
            {
                if (ev_last_track_id != -1)
                    event_orch::publish(event_orch::EventType::TargetLost);
                if (ev_fire_pressed)
                    event_orch::publish(event_orch::EventType::FireReleased);
                ev_last_track_id = -1;
                ev_fire_pressed = false;
                engine.reset();
                aim_path_driver.reset();
                if (trigger.phase == TriggerPhase::Pressed)
                    mouseThread.releaseLeftButton();
                trigger.reset();
                mouseThread.clearQueuedMoves();
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
            aim_path_driver.reset();
            if (hasNewDetection)
            {
                boss::EngineInput in;
                in.boxes = &precise_boxes;
                in.classes = &classes;
                in.confidences = &confidences;
                in.crosshair_x = config_resolution * 0.5;
                in.crosshair_y = config_resolution * 0.5;
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

        // 正常缓存按“空检测帧”递增 missed；若采集/推理完全停更，就没有新
        // version 可递增。用最近检测节奏把缓存帧数换算成超时，避免旧锁定
        // 永久挂住，同时仍不拿陈旧 anchor 驱动 mover。
        if (!hasNewDetection)
        {
            const double cadence_ms = (detection_interval_ms > 0.0)
                ? std::clamp(detection_interval_ms, 1.0, 1000.0)
                : (1000.0 / 60.0);
            const double cache_timeout_ms = cadence_ms
                * static_cast<double>(std::max(1, profile_ptr->lost_target_cache_frames + 1));
            if (ev_last_track_id != -1 && detection_age_ms > cache_timeout_ms)
            {
                engine.reset();
                publish_boss_debug(engine);
                mouseThread.clearQueuedMoves();
                if (trigger.phase == TriggerPhase::Pressed)
                    mouseThread.releaseLeftButton();
                trigger.reset();
                event_orch::publish(event_orch::EventType::TargetLost);
                if (ev_fire_pressed)
                    event_orch::publish(event_orch::EventType::FireReleased);
                ev_last_track_id = -1;
                ev_fire_pressed = false;
                g_pid_last_err_px.store(0.0f);
            }
            continue;
        }

        const auto pivot = resolve_crosshair_pivot(profile_ptr, config_resolution);

        // The dynamic gate is based on the previous tick's locked track, so it
        // can constrain the candidate set before tracker association this tick.
        const auto [fov_rx, fov_ry] = resolve_fov_radii(*profile_ptr, pivot, engine);
        g_dynamic_fov_radius_x_px.store(profile_ptr->dynamic_fov_enabled
            ? static_cast<float>(fov_rx) : 0.0f);
        g_dynamic_fov_radius_y_px.store(profile_ptr->dynamic_fov_enabled
            ? static_cast<float>(fov_ry) : 0.0f);

        boss::EngineInput in;
        in.boxes = &precise_boxes;
        in.classes = &classes;
        in.confidences = &confidences;
        // 目标槽位 = 该热键的 aim_classes 列表 (顺序即优先级)。
        // 用户面范围语义:1 = 框顶,0 = 框底。引擎在每次新锁定时
        // 从范围内抽一次并保持，切换/重新锁定才重新抽样。
        in.target_slots.clear();
        in.target_slots.reserve(profile_ptr->aim_classes.size());
        // min_conf 语义: >0 视作该类别的私有阈值; ==0 视作 "跟随全局",
        // 用 AI 页的 config.confidence_threshold 顶上, 从而和 GPU/CPU 侧
        // 的 NMS/后处理阈值保持一致(不会低于全局设置)。
        const float global_conf = config_confidence;
        for (const auto& ac : profile_ptr->aim_classes)
        {
            boss::TargetSlot s;
            s.class_id = ac.class_id;
            s.y_offset_min = std::clamp(ac.y_offset, 0.0f, 1.0f);
            s.y_offset_max = std::clamp(ac.y_offset_max, 0.0f, 1.0f);
            if (s.y_offset_min > s.y_offset_max)
                std::swap(s.y_offset_min, s.y_offset_max);
            s.min_conf = (ac.min_conf > 0.0f) ? ac.min_conf : global_conf;
            in.target_slots.push_back(s);
        }
        in.lost_target_cache_frames = profile_ptr->lost_target_cache_frames;
        in.crosshair_x = pivot.x;
        in.crosshair_y = pivot.y;
        in.fov_radius_x = fov_rx;
        in.fov_radius_y = fov_ry;
        in.image_size   = static_cast<double>(config_resolution);

        in.pidf_params.kp_x = profile_ptr->pidf_kp_x; in.pidf_params.kp_y = profile_ptr->pidf_kp_y;
        in.pidf_params.ki_x = profile_ptr->pidf_ki_x; in.pidf_params.ki_y = profile_ptr->pidf_ki_y;
        in.pidf_params.kd_x = profile_ptr->pidf_kd_x; in.pidf_params.kd_y = profile_ptr->pidf_kd_y;
        in.pidf_params.kf_x = profile_ptr->pidf_kf_x; in.pidf_params.kf_y = profile_ptr->pidf_kf_y;
        in.pidf_params.lr_x = profile_ptr->pidf_lr_x; in.pidf_params.lr_y = profile_ptr->pidf_lr_y;
        in.pidf_params.deadzone_x = profile_ptr->pidf_deadzone_x; in.pidf_params.deadzone_y = profile_ptr->pidf_deadzone_y;
        in.pidf_params.movement_limit_x = profile_ptr->pidf_limit_x; in.pidf_params.movement_limit_y = profile_ptr->pidf_limit_y;

        // AVA 负责 selector/tracker、aimpoint 和 PIDF；用户自定义
        // AimPath 作为可选的后置轨迹整形，不参与 AVA 的目标预测状态。
        const auto now = std::chrono::steady_clock::now();
        double dt = (detection_interval_ms > 0.0)
            ? std::clamp(detection_interval_ms * 0.001, 1.0 / 1000.0, 0.1)
            : 1.0 / 60.0;
        last_tick_ts = now;

        const boss::EngineOutput out = engine.tick(in, dt);

        publish_boss_debug(engine);

        // ─── Drive mouse ───
        if (out.have_target)
        {
            // ─── 事件编排:target 锁定/切换事件 ───
            if (ev_last_track_id == -1)
                event_orch::publish(event_orch::EventType::TargetLocked);
            else if (ev_last_track_id != out.current_track_id)
                event_orch::publish(event_orch::EventType::TargetSwitched);
            ev_last_track_id = out.current_track_id;

            int drive_dx = out.dx;
            int drive_dy = out.dy;
            boss::AimPathDriver::Params path;
            path.mode = static_cast<boss::AimPathDriver::Mode>(
                std::clamp(profile_ptr->aim_path_mode, 0, 2));
            path.strength = std::clamp(
                static_cast<double>(profile_ptr->aim_path_influence) / 100.0,
                0.0, 1.0);
            path.cx1 = static_cast<double>(profile_ptr->aim_path_bezier_cx1);
            path.cy1 = static_cast<double>(profile_ptr->aim_path_bezier_cy1);
            path.cx2 = static_cast<double>(profile_ptr->aim_path_bezier_cx2);
            path.cy2 = static_cast<double>(profile_ptr->aim_path_bezier_cy2);
            path.custom_samples = profile_ptr->aim_path_custom_samples;
            path.neural_enabled = profile_ptr->aim_path_neural_enabled;
            path.neural_weights = profile_ptr->aim_path_neural_weights;
            aim_path_driver.configure(path);
            const auto shaped = aim_path_driver.step(
                static_cast<double>(out.anchor.x),
                static_cast<double>(out.anchor.y),
                pivot.x, pivot.y, dt, out.current_track_id,
                drive_dx, drive_dy);
            drive_dx = shaped.move_x;
            drive_dy = shaped.move_y;
            if (out.motion_suppressed)
                mouseThread.clearQueuedMoves();
            // CVM 在短暂丢检期间继续使用 tracker 的预测框驱动 PIDF；
            // coasting 只禁止开火，不再丢弃预测移动或重置控制器历史。
            mouseThread.sendRawMove(drive_dx, drive_dy);

            if (out.coasting)
            {
                if (trigger.phase == TriggerPhase::Pressed)
                    mouseThread.releaseLeftButton();
                trigger.reset();
            }

            // ─── 扳机 FSM ───
            if (!out.coasting && profile_ptr->trigger_enabled)
            {
                // trigger_y_percent > 100 = 命中区大于 bbox (预开火)。
                const double tx_range = out.bbox.width  * profile_ptr->trigger_y_percent / 100.0 * 0.5;
                const double ty_range = out.bbox.height * profile_ptr->trigger_y_percent / 100.0 * 0.5;
                const bool in_zone = std::abs(pivot.x - static_cast<double>(out.anchor.x)) <= tx_range &&
                                     std::abs(pivot.y - static_cast<double>(out.anchor.y)) <= ty_range;

                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                // 转火检测:锁定的 target track_id 变化 → 进 SwitchCooldown。
                // 首次锁定 (last_fire_track_id == -1) 不算转火,直接走 Idle。
                if (out.current_track_id != trigger.last_fire_track_id &&
                    trigger.last_fire_track_id != -1 &&
                    profile_ptr->trigger_switch_cooldown_ms > 0 &&
                    trigger.phase != TriggerPhase::SwitchCooldown)
                {
                    if (trigger.phase == TriggerPhase::Pressed)
                        mouseThread.releaseLeftButton();
                    trigger.phase = TriggerPhase::SwitchCooldown;
                    trigger.phase_time_ms = now_ms;
                    // 转火冷却本身可以带抖动(用 delay_jitter,懒得再加参数)。
                    trigger.phase_target_ms = jitter_ms(
                        profile_ptr->trigger_switch_cooldown_ms,
                        profile_ptr->trigger_delay_jitter_ms);
                    trigger.in_zone_since_ms = -1;
                }
                trigger.last_fire_track_id = out.current_track_id;

                switch (trigger.phase)
                {
                case TriggerPhase::Idle:
                    if (in_zone)
                    {
                        // 记录 in_zone 起点。已经 in_zone 就沿用旧 stamp,
                        // 达到累计时长即触发 —— 1ms delay 不再空转一整帧。
                        if (trigger.in_zone_since_ms < 0)
                        {
                            trigger.in_zone_since_ms = now_ms;
                            trigger.phase_target_ms = jitter_ms(
                                profile_ptr->trigger_fire_delay,
                                profile_ptr->trigger_delay_jitter_ms);
                        }
                        if (now_ms - trigger.in_zone_since_ms >= trigger.phase_target_ms)
                        {
                            mouseThread.pressLeftButton();
                            trigger.phase = TriggerPhase::Pressed;
                            trigger.phase_time_ms = now_ms;
                            trigger.phase_target_ms = jitter_ms(
                                profile_ptr->trigger_fire_duration,
                                profile_ptr->trigger_duration_jitter_ms);
                            trigger.in_zone_since_ms = -1;
                        }
                        else
                        {
                            trigger.phase = TriggerPhase::Delay;
                            trigger.phase_time_ms = trigger.in_zone_since_ms;
                        }
                    }
                    else
                    {
                        trigger.in_zone_since_ms = -1;
                    }
                    break;
                case TriggerPhase::Delay:
                    if (!in_zone)
                    {
                        trigger.phase = TriggerPhase::Idle;
                        trigger.in_zone_since_ms = -1;
                        break;
                    }
                    if (now_ms - trigger.phase_time_ms >= trigger.phase_target_ms)
                    {
                        mouseThread.pressLeftButton();
                        trigger.phase = TriggerPhase::Pressed;
                        trigger.phase_time_ms = now_ms;
                        trigger.phase_target_ms = jitter_ms(
                            profile_ptr->trigger_fire_duration,
                            profile_ptr->trigger_duration_jitter_ms);
                        trigger.in_zone_since_ms = -1;
                    }
                    break;
                case TriggerPhase::Pressed:
                    if (now_ms - trigger.phase_time_ms >= trigger.phase_target_ms)
                    {
                        mouseThread.releaseLeftButton();
                        trigger.phase = TriggerPhase::Cooldown;
                        trigger.phase_time_ms = now_ms;
                        trigger.phase_target_ms = jitter_ms(
                            profile_ptr->trigger_fire_interval,
                            profile_ptr->trigger_interval_jitter_ms);
                    }
                    break;
                case TriggerPhase::Cooldown:
                    if (now_ms - trigger.phase_time_ms >= trigger.phase_target_ms)
                    {
                        trigger.phase = TriggerPhase::Idle;
                        trigger.in_zone_since_ms = -1;
                    }
                    break;
                case TriggerPhase::SwitchCooldown:
                    if (now_ms - trigger.phase_time_ms >= trigger.phase_target_ms)
                    {
                        trigger.phase = TriggerPhase::Idle;
                        trigger.in_zone_since_ms = -1;
                    }
                    break;
                }
            }
            else if (!out.coasting)
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
            aim_path_driver.reset();
            if (trigger.phase == TriggerPhase::Pressed)
                mouseThread.releaseLeftButton();
            trigger.reset();
            mouseThread.clearQueuedMoves();
            g_pid_last_err_px.store(0.0f);

            // ─── 事件编排:target 丢失事件 ───
            if (ev_last_track_id != -1)
                event_orch::publish(event_orch::EventType::TargetLost);
            ev_last_track_id = -1;
        }

        // ─── 事件编排:扳机 Fire 状态 diff → FirePressed / FireReleased ───
        {
            const bool now_pressed = (trigger.phase == TriggerPhase::Pressed);
            if (now_pressed && !ev_fire_pressed)
                event_orch::publish(event_orch::EventType::FirePressed);
            else if (!now_pressed && ev_fire_pressed)
                event_orch::publish(event_orch::EventType::FireReleased);
            ev_fire_pressed = now_pressed;
        }

        // ─── Replay buffer (one snapshot per detection) ─────────────────
        auto& replay = runtime::ReplayBuffer::instance();
        replay.setEnabled(replay_enabled);
        replay.setRetentionSeconds(replay_seconds);
        if (replay_enabled)
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
    mouseThread.clearQueuedMoves();
    if (ev_last_track_id != -1)
        event_orch::publish(event_orch::EventType::TargetLost);
    if (ev_fire_pressed)
        event_orch::publish(event_orch::EventType::FireReleased);
}
