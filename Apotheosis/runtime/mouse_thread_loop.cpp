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
#include "mouse.h"
#include "Apotheosis.h"
#include "runtime/aim_telemetry.h"
#include "runtime/latency_probe.h"
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
//   2. snapshot hotkey + crosshair pivot (crosshair-color if enabled)
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

// ── 静态准星参考点 ────────────────────────────────────────────────────────
//
// 找色【不参与 PID 公式】: 它只回答"这一帧准星在画面哪个位置", 用来替代那个
// 原本写死的画面几何中心(静态准星)。所以它必须以一个【静态常量】的姿态进入
// 控制环, 而不是一个每帧都在跳的活信号 —— 否则 pivot 的压缩噪声/选簇翻转会
// 直接变成误差阶跃, 被控制器放大成抖动或"一顿一顿"。
//
// 三条约束(都只作用在参考点上, 不改变控制器本身):
//   1) 限速   : 参考点每拍位移不超过 kMaxSpeedPxPerSec×dt, 几十像素的假命中/
//               跳簇被摊到几拍里, 不会变成一次 20px 的大步;
//   2) 惯性   : 一阶低通, 强度由 crosshair_smooth 决定(和运行期那份滤波同向),
//               但保留一个最小基线, 保证参考点始终是"缓动"的;
//   3) 丢帧保持: 命中断掉先原地保持 kHoldMs, 而不是瞬间弹回几何中心; 超时后
//               再限速滑回中心 —— 避免"参考点跳回中心"这种几十像素的阶跃。
struct StaticCrosshairRef
{
    double x = 0.0;
    double y = 0.0;
    bool   engaged = false;

    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point last_hit{};
    bool have_update = false;

    // 参考点的最大移动速度。真实后坐力抬枪(几百 px/s)完全不受限, 只有单帧突跳
    // 会被削平: 56px 的假跳在 ~5 拍(42ms)内吸收完。
    static constexpr double kMaxSpeedPxPerSec = 1500.0;
    static constexpr double kMinTauSec        = 0.010;  // 基线惯性(最跟手时)
    static constexpr double kMaxTauSec        = 0.050;  // crosshair_smooth = 1 时
    static constexpr int    kHoldMs           = 120;

    void reset()
    {
        engaged = false;
        have_update = false;
    }

    void update(bool fresh, double tx, double ty,
                double centre, double smooth,
                std::chrono::steady_clock::time_point now)
    {
        double dt = have_update
            ? std::chrono::duration<double>(now - last_update).count()
            : (1.0 / 120.0);
        last_update = now;
        have_update = true;
        dt = std::clamp(dt, 1.0e-4, 0.1);

        if (fresh)
        {
            if (!engaged)
            {
                // 第一次拿到命中: 直接落位, 不做缓动(否则会从中心慢慢爬过去)。
                x = tx;
                y = ty;
                engaged = true;
                last_hit = now;
                return;
            }

            // crosshair_smooth: 越大越稳(与 crosshair_runtime 的 One-Euro 同向)。
            const double s = std::clamp(smooth, 0.0, 1.0);
            const double tau = kMinTauSec + (kMaxTauSec - kMinTauSec) * s;
            const double a = 1.0 - std::exp(-dt / tau);
            double nx = x + (tx - x) * a;
            double ny = y + (ty - y) * a;
            clamp_step(nx, ny, dt);
            last_hit = now;
            return;
        }

        if (!engaged)
            return;

        const auto since_hit = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_hit).count();
        if (since_hit <= kHoldMs)
            return;   // 丢帧保持: 参考点不动

        // 超时回收: 朝几何中心限速滑回, 到位后交还给几何中心。
        double nx = centre, ny = centre;
        if (!clamp_step(nx, ny, dt))
            engaged = false;   // 已经回到中心
    }

private:
    // 把 (nx,ny) 约束到"离当前参考点最多 kMaxSpeedPxPerSec×dt"。返回 false
    // 表示目标就是当前点(无位移)。
    bool clamp_step(double& nx, double& ny, double dt)
    {
        const double dx = nx - x;
        const double dy = ny - y;
        const double mag = std::hypot(dx, dy);
        if (mag <= 1e-9)
            return false;
        const double max_step = kMaxSpeedPxPerSec * dt;
        if (mag > max_step)
        {
            nx = x + dx / mag * max_step;
            ny = y + dy / mag * max_step;
        }
        x = nx;
        y = ny;
        return true;
    }
};

StaticCrosshairRef g_static_crosshair;

PivotResolved resolve_crosshair_pivot(const HotkeyProfile* profile,
                                      int detection_resolution,
                                      double crosshair_smooth,
                                      std::chrono::steady_clock::time_point now)
{
    const double centre = detection_resolution * 0.5;
    PivotResolved out;
    out.x = centre;
    out.y = centre;
    // 扳机只负责开火判定，不能隐式启用准星找色。否则枪口闪光或准星动画
    // 会在开火时移动 PID 参考点，表现为锁定后抖动。
    if (!profile || !profile->crosshair_detect_enabled)
    {
        g_static_crosshair.reset();
        crosshair_runtime::publish_static_ref(crosshair_runtime::PivotSnapshot{});
        return out;
    }

    const auto snap = crosshair_runtime::read();
    bool fresh = false;
    if (snap.valid)
    {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - snap.ts).count();
        fresh = age <= crosshair_runtime::kFreshnessMs;
    }

    g_static_crosshair.update(fresh, snap.x, snap.y, centre,
                             crosshair_smooth, now);

    // 参考点对外发布一份(预览画的就是这个, 和原始命中点分开看)。
    {
        crosshair_runtime::PivotSnapshot ref;
        ref.ts = now;
        ref.valid = g_static_crosshair.engaged;
        ref.x = g_static_crosshair.engaged ? g_static_crosshair.x : centre;
        ref.y = g_static_crosshair.engaged ? g_static_crosshair.y : centre;
        crosshair_runtime::publish_static_ref(ref);
    }

    if (!g_static_crosshair.engaged)
        return out;

    out.x = g_static_crosshair.x;
    out.y = g_static_crosshair.y;
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

void release_trigger_outputs(MouseThread& mouse, TriggerState& state)
{
    if (state.phase == TriggerPhase::Pressed)
        mouse.releaseLeftButton();
}

void reset_trigger(MouseThread& mouse, TriggerState& state)
{
    release_trigger_outputs(mouse, state);
    state.reset();
}

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

    // 本会话是否真的持有过锁定。重构时丢掉的老代码守卫 (ev_last_track_id != -1)
    // 表达的就是这件事: 缓存超时只应该在"锁定过又断流"时清链, 不该在本来就没
    // 锁定时反复 reset。
    bool had_lock = false;

    g_pid_last_err_px.store(0.0f);
    g_pid_mode_track.store(false);

    while (!shouldExit && !session_stop_requested.load())
    {
        bool hasNewDetection = false;
        double detection_age_ms = 0.0;
        double detection_interval_ms = 0.0;
        // 延迟探针: 本拍消费的那批检测, 其像素的采集时刻 / 发布时刻。
        int64_t probed_frame_capture_ns = 0;
        int64_t probed_publish_ns = 0;

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
                // 探针: 记录"看到了一批新检测"。与 markAimConsume(只统计被消费的)
                // 分开, 这样日志才能区分"瞄准键没按下"和"采集/推理真的停更了"。
                runtime::latency::noteDetectionSeen();
            }
            detection_interval_ms = detectionBuffer.last_interval_ms;
            if (detectionBuffer.stamp.time_since_epoch().count() != 0)
                detection_age_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - detectionBuffer.stamp).count();
            probed_frame_capture_ns = detectionBuffer.frame_stamp_ns;
            probed_publish_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                detectionBuffer.stamp.time_since_epoch()).count();
        }

        // 只用硬件工作线程已完成的位移更新控制器测量状态。
        // 队列中尚未发送的命令不再被误当成真实鼠标位移。
        const auto movement_feedback = mouseThread.consumeMovementFeedback();
        g_mouse_queue_latency_ms.store(static_cast<float>(movement_feedback.latency_ms));
        g_mouse_queue_backlog.store(static_cast<int>(movement_feedback.backlog));
        g_mouse_send_failures.store(movement_feedback.failed);
        const auto config_snapshot = runtime_config::read();


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
                engine.reset();
                aim_path_driver.reset();
                reset_trigger(mouseThread, trigger);
                mouseThread.clearQueuedMoves();
                last_tick_ts = std::chrono::steady_clock::time_point::min();
                had_lock = false;
                publish_boss_debug(engine);
            }
        }

        // No active hotkey → idle. Force-release the fire button and clear
        // any queued moves so the cursor stops drifting after the user lifts
        // the trigger.
        if (!profile_ptr)
        {
            reset_trigger(mouseThread, trigger);
            mouseThread.clearQueuedMoves();
            aim_path_driver.reset();
            had_lock = false;
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
        //
        // 超时下限 3 个采集周期: 这个分支的目的只是"发现检测流真的断了", 不该
        // 被正常的发布抖动(8.2ms 的节奏偶尔 9~10ms)触发 —— engine.reset() 会连
        // PIDF 的 residual 一起清掉, 那正是"逼近锚点后停住不再收敛"的直接原因。
        if (!hasNewDetection)
        {
            const double cadence_ms = (detection_interval_ms > 0.0)
                ? std::clamp(detection_interval_ms, 1.0, 1000.0)
                : (1000.0 / 60.0);
            const double cache_timeout_ms = cadence_ms
                * static_cast<double>(std::max(3, profile_ptr->lost_target_cache_frames + 1));
            if (had_lock && detection_age_ms > cache_timeout_ms)
            {
                engine.reset();
                publish_boss_debug(engine);
                mouseThread.clearQueuedMoves();
                reset_trigger(mouseThread, trigger);
                g_pid_last_err_px.store(0.0f);
                had_lock = false;
            }
            continue;
        }

        // 延迟探针 T3: 控制环此刻真正拿到了一批【新鲜】检测。到这里的路径
        // 已经排除了无新帧与陈旧缓存的 continue, 所以结算出来的是真实消费
        // 延迟, 不会被"读旧数据"污染。T4 用上一步刚取回的鼠标队列延迟。
        runtime::latency::markAimConsume(
            probed_frame_capture_ns, probed_publish_ns,
            static_cast<double>(g_mouse_queue_latency_ms.load()));

        const auto pivot = resolve_crosshair_pivot(
            profile_ptr, config_resolution, config_snapshot->crosshair_smooth,
            std::chrono::steady_clock::now());

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
            had_lock = true;
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

            // 扳机模式: 0 = 长按(按住不松手), >0 = 连点(按 duration 后松手)。
            const bool trigger_hold_mode = profile_ptr->trigger_fire_duration <= 0;

            if (out.coasting)
            {
                // 长按模式: 短暂滑行(检测丢一两帧, tracker 还在预测)不松手 ——
                // 松了再按就是"连点"那种顿挫。这里冻结扳机状态, 等目标观测
                // 回来再由 FSM 按"准星是否还在命中区"决定是否松手。目标真的
                // 丢了会走下面 !have_target 分支强制松开。
                if (!trigger_hold_mode)
                    reset_trigger(mouseThread, trigger);
            }

            // ─── 扳机 FSM (命中盒约束) ───
            // 长按模式 (trigger_fire_duration == 0): 进命中区按下, 一直按住,
            // 只有准星离开命中区才松手。连点模式 (>0): 老行为, 按 N ms 松手,
            // 冷却后再按。
            if (!out.coasting && profile_ptr->trigger_enabled)
            {
                const double scale = std::max(0.1, profile_ptr->trigger_y_percent / 100.0);
                const double tx_range = out.bbox.width * scale * 0.5;
                const double ty_range = out.bbox.height * scale * 0.5;

                // 1. 水平与垂直容差判定 (以瞄准受击锚点 anchor 为基准)
                const bool in_tolerance = std::abs(pivot.x - static_cast<double>(out.anchor.x)) <= tx_range &&
                                         std::abs(pivot.y - static_cast<double>(out.anchor.y)) <= ty_range;

                // 2. 真实人体边界门禁：彻底消除准星瞄准在头顶虚空导致的走火！
                // 向上不允许超过 bbox 顶部以上 12% 边缘，向下不超过 bbox 底部
                const double top_limit = static_cast<double>(out.bbox.y) - out.bbox.height * 0.12;
                const double bottom_limit = static_cast<double>(out.bbox.y + out.bbox.height);
                const bool inside_body_geometry = (pivot.y >= top_limit) && (pivot.y <= bottom_limit);

                const bool in_zone = in_tolerance && inside_body_geometry;

                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                const auto begin_fire = [&] {
                    trigger.in_zone_since_ms = -1;
                    trigger.phase_time_ms = now_ms;
                    mouseThread.pressLeftButton();
                    trigger.phase = TriggerPhase::Pressed;
                    // 长按模式不设按住上限(phase_target_ms 只在连点模式使用)。
                    trigger.phase_target_ms = trigger_hold_mode ? 0 : jitter_ms(
                        profile_ptr->trigger_fire_duration,
                        profile_ptr->trigger_duration_jitter_ms);
                };

                // 转火阻滞解除：只要转火后新的目标也在准星容许范围内，直接接力爆发，不产生卡壳死机
                if (out.current_track_id != trigger.last_fire_track_id &&
                    trigger.last_fire_track_id != -1 &&
                    profile_ptr->trigger_switch_cooldown_ms > 0 &&
                    trigger.phase != TriggerPhase::SwitchCooldown &&
                    !in_zone)
                {
                    release_trigger_outputs(mouseThread, trigger);
                    trigger.phase = TriggerPhase::SwitchCooldown;
                    trigger.phase_time_ms = now_ms;
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
                        // 零延迟直通模式：当 delay 为 0 时直接击发，实现机械级瞬秒
                        const int target_delay = jitter_ms(
                            profile_ptr->trigger_fire_delay,
                            profile_ptr->trigger_delay_jitter_ms);
                        if (target_delay <= 0)
                        {
                            begin_fire();
                        }
                        else
                        {
                            if (trigger.in_zone_since_ms < 0)
                            {
                                trigger.in_zone_since_ms = now_ms;
                                trigger.phase_target_ms = target_delay;
                            }
                            if (now_ms - trigger.in_zone_since_ms >= trigger.phase_target_ms)
                            {
                                begin_fire();
                            }
                            else
                            {
                                trigger.phase = TriggerPhase::Delay;
                                trigger.phase_time_ms = trigger.in_zone_since_ms;
                            }
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
                        begin_fire();
                    }
                    break;
                case TriggerPhase::Pressed:
                    if (trigger_hold_mode)
                    {
                        // 长按模式: 只要还在命中区就一直按住, 不做时长循环。
                        // 准星离开命中区才松手, 然后走一次冷却间隔 —— 避免在
                        // 判定边缘"按-松-按"变成连点。
                        if (!in_zone)
                        {
                            mouseThread.releaseLeftButton();
                            trigger.phase = TriggerPhase::Cooldown;
                            trigger.phase_time_ms = now_ms;
                            trigger.phase_target_ms = jitter_ms(
                                profile_ptr->trigger_fire_interval,
                                profile_ptr->trigger_interval_jitter_ms);
                        }
                        break;
                    }
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
                        // 冷却结束瞬间若依然在受击框内，无缝立即衔接下一轮爆发连发
                        if (in_zone && profile_ptr->trigger_fire_delay <= 0)
                        {
                            begin_fire();
                        }
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
                reset_trigger(mouseThread, trigger);
            }

            const float err = static_cast<float>(std::hypot(
                out.anchor.x - pivot.x, out.anchor.y - pivot.y));
            g_pid_last_err_px.store(err);
            g_pid_mode_track.store(true);
        }
        else
        {
            aim_path_driver.reset();
            reset_trigger(mouseThread, trigger);
            mouseThread.clearQueuedMoves();
            g_pid_last_err_px.store(0.0f);
            had_lock = false;

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

    reset_trigger(mouseThread, trigger);
    mouseThread.clearQueuedMoves();
}
