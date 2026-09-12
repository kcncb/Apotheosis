#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>   // chain log: 候选检测明细的 snprintf
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
#include "runtime/chain_log.h"
#include <ctime>    // chain log: 时间戳文件名
// 全链路排查日�?用法�?chain_log.h 头部说明)
#include <string>   // chain log: 文件路径拼接
#include "runtime/config_snapshot.h"
#include "runtime/thread_loops.h"

// ─────────────────────────────────────────────────────────────────────────────
// Boss AI aim loop �?implementation of boss_aim_algorithm.md.
//
// Replaces the old PID + Kalman + MultiTargetTracker + smart-trigger pipeline.
// The Boss engine (see mouse/boss_aim.h) owns target management and the closed-
// form P + velocity-feedforward controller. This file is now a thin shim:
//
//   1. wait for detection / hotkey change
//   2. snapshot hotkey + crosshair pivot (crosshair-color if enabled)
//   3. BossAimEngine.tick()  �? (dx, dy, fire)
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
// 找色【不参与 PID 公式�? 它只回答"这一帧准星在画面哪个位置", 用来替代那个
// 原本写死的画面几何中�?静态准�?。所以它必须以一个【静态常量】的姿态进�?
// 控制�? 而不是一个每帧都在跳的活信号 —�?否则 pivot 的压缩噪�?选簇翻转�?
// 直接变成误差阶跃, 被控制器放大成抖动或"一顿一�?�?
//
// 三条约束(都只作用在参考点�? 不改变控制器本身):
//   1) 限�?  : 参考点每拍位移不超�?kMaxSpeedPxPerSec×dt, 几十像素的假命中/
//               跳簇被摊到几拍里, 不会变成一�?20px 的大�?
//   2) 惯�?  : 一阶低�? 强度�?crosshair_smooth 决定(和运行期那份滤波同向),
//               但保留一个最小基�? 保证参考点始终�?缓动"�?
//   3) 丢帧保持: 命中断掉先原地保�?kHoldMs, 而不是瞬间弹回几何中�? 超时�?
//               再限速滑回中�?—�?避免"参考点跳回中心"这种几十像素的阶跃�?
struct StaticCrosshairRef
{
    double x = 0.0;
    double y = 0.0;
    bool   engaged = false;

    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point last_hit{};
    bool have_update = false;

    // 参考点的最大移动速度。真实后坐力抬枪(几百 px/s)完全不受�? 只有单帧突跳
    // 会被削平: 56px 的假跳在 ~5 �?42ms)内吸收完�?
    static constexpr double kMaxSpeedPxPerSec = 1500.0;
    static constexpr double kMinTauSec        = 0.010;  // 基线惯�?最跟手�?
    static constexpr double kMaxTauSec        = 0.050;  // crosshair_smooth = 1 �?
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
                // 第一次拿到命�? 直接落位, 不做缓动(否则会从中心慢慢爬过�?�?
                x = tx;
                y = ty;
                engaged = true;
                last_hit = now;
                return;
            }

            // crosshair_smooth: 越大越稳(�?crosshair_runtime �?One-Euro 同向)�?
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

        // 超时回收: 朝几何中心限速滑�? 到位后交还给几何中心�?
        double nx = centre, ny = centre;
        if (!clamp_step(nx, ny, dt))
            engaged = false;   // 已经回到中心
    }

private:
    // �?(nx,ny) 约束�?离当前参考点最�?kMaxSpeedPxPerSec×dt"。返�?false
    // 表示目标就是当前�?无位�?�?
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
    // 会在开火时移动 PID 参考点，表现为锁定后抖动�?
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

    // 参考点对外发布一�?预览画的就是这个, 和原始命中点分开�?�?
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
    // in_zone 起点时间�?进入 Idle 后一旦目标进入命中区�?stamp,
    // �?(now - in_zone_since_ms) �?delay 立即触发,不再空转一帧�?
    int64_t in_zone_since_ms = -1;
    // 上次开�?/ 上次锁定�?target track id,用于检测转火并进入 SwitchCooldown�?
    int last_fire_track_id = -1;
    // 本轮 phase 目标时长(delay/duration/interval)已经算入随机抖动,
    // 避免在同一 phase 里每帧重摇导致门槛漂移�?
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

// 对基础延迟�?±jitter 抖动,结果不小�?0。thread_local RNG 避免锁竞争�?
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

// ─── 全链路排查日志的辅助函数 ───────────────────────────────────────────────
// 主循环里只调用下面这些函�? 每个调用点一�?—�?详细字段在这里统一拼装, 免得�?
// 大段格式化代码散落到延迟敏感的主循环里。字段含义见 runtime/chain_log.h�?
namespace chain_detail
{
inline std::string dump_path(const char* tag)
{
    // 落在 exe 同级�?logs/ �? 文件名带时间戳与标签, 便于事后认领�?
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    std::string path = "logs/chain_";
    path += stamp;
    path += '_';
    path += tag;
    path += ".log";
    return path;
}
} // namespace chain_detail

// 有目标、且本帧真的走完了控制与下发流程时调用�?
inline void chain_log_aim_frame(std::int64_t frame_id,
                                int detection_count,
                                int resolution,
                                double detection_interval_ms,
                                const double* pivot_xy,
                                bool crosshair_used,
                                const double* crosshair_xy,
                                double fov_radius_x,
                                double fov_radius_y,
                                int track_id,
                                const double* anchor_xy,
                                double box_w,
                                double box_h,
                                int class_id,
                                double obs_x,
                                double obs_y,
                                int engine_dx,
                                int engine_dy,
                                bool coasting,
                                bool motion_suppressed,
                                int drive_dx,
                                int drive_dy,
                                int queue_backlog,
                                double queue_latency_ms,
                                int send_failures,
                                int trigger_phase,
                                bool trigger_enabled,
                                bool trigger_in_zone)
{
    if (!runtime::chainlog::enabled(runtime::chainlog::SecControl, 2))
        return;
    const auto latency = runtime::latency::snapshot();
    runtime::chainlog::write(
        runtime::chainlog::SecDetect, 2,
        ",count=%d,interval_ms=%.2f",
        detection_count, detection_interval_ms);
    runtime::chainlog::write(
        runtime::chainlog::SecCrosshair, 2,
        ",used=%d,px=%.2f,py=%.2f,cross_x=%.2f,cross_y=%.2f,"
        "fov_rx=%.1f,fov_ry=%.1f,res=%d",
        crosshair_used ? 1 : 0, pivot_xy[0], pivot_xy[1],
        crosshair_xy[0], crosshair_xy[1],
        fov_radius_x, fov_radius_y, resolution);
    runtime::chainlog::write(
        runtime::chainlog::SecControl, 2,
        ",track_id=%d,class=%d,anchor_x=%.2f,anchor_y=%.2f,"
        "obs_x=%.1f,obs_y=%.1f,box_w=%.1f,box_h=%.1f,"
        "err_x=%.2f,err_y=%.2f,dx=%d,dy=%d,coasting=%d,suppressed=%d",
        track_id, class_id, anchor_xy[0], anchor_xy[1],
        obs_x, obs_y, box_w, box_h,
        anchor_xy[0] - crosshair_xy[0], anchor_xy[1] - crosshair_xy[1],
        engine_dx, engine_dy, coasting ? 1 : 0, motion_suppressed ? 1 : 0);
    runtime::chainlog::write(
        runtime::chainlog::SecExec, 2,
        ",drive_dx=%d,drive_dy=%d,backlog=%d,queue_ms=%.2f,send_fail=%d",
        drive_dx, drive_dy, queue_backlog, queue_latency_ms, send_failures);
    runtime::chainlog::write(
        runtime::chainlog::SecTrigger, 2,
        ",enabled=%d,phase=%d,in_zone=%d",
        trigger_enabled ? 1 : 0, trigger_phase, trigger_in_zone ? 1 : 0);
    runtime::chainlog::write(
        runtime::chainlog::SecLatency, 2,
        ",capture_wait_ms=%.2f,inference_ms=%.2f,publish_to_aim_ms=%.2f,"
        "aim_to_move_ms=%.2f,total_ms=%.2f,end_to_end_ms=%.2f,"
        "capture_fps=%.1f,capture_interval_ms=%.2f,frame_age_us=%d,"
        "frames=%llu,detections=%llu,capture_frames=%llu,dropped=%llu,stale=%llu",
        latency.stages[runtime::latency::kCaptureWait].ema_ms,
        latency.stages[runtime::latency::kInference].ema_ms,
        latency.stages[runtime::latency::kPublishToAim].ema_ms,
        latency.stages[runtime::latency::kAimToMove].ema_ms,
        latency.stages[runtime::latency::kTotal].ema_ms,
        latency.stages[runtime::latency::kEndToEnd].ema_ms,
        latency.capture_fps, latency.capture_interval_ms,
        latency.device_frame_age_us,
        static_cast<unsigned long long>(latency.frames_consumed),
        static_cast<unsigned long long>(latency.detections_seen),
        static_cast<unsigned long long>(latency.capture_frames),
        static_cast<unsigned long long>(latency.dropped_capture),
        static_cast<unsigned long long>(latency.stale_consumes));
}

// "这一帧为什么没�?为什么松�? —�?排查时最常问的问�? 所以单独记成关键事件�?
inline void chain_log_skip(const char* reason)
{
    runtime::chainlog::event(",reason=%s", reason);
}

// 自动导出最�?N �?环形缓冲), 文件名带时间�? 失败时不抛异�? 只记一条事件�?
inline void chain_log_auto_dump(const char* tag)
{
    const std::string path = chain_detail::dump_path(tag);
    const std::int64_t written = runtime::chainlog::dump(path.c_str());
    if (written < 0)
        runtime::chainlog::event(",reason=chain_dump_failed,path=%s", path.c_str());
    else
        runtime::chainlog::event(",reason=chain_dumped,path=%s,records=%lld",
                                 path.c_str(), static_cast<long long>(written));
}

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


    TriggerState trigger;

    // 本会话是否真的持有过锁定。重构时丢掉的老代码守�?(ev_last_track_id != -1)
    // 表达的就是这件事: 缓存超时只应该在"锁定过又断流"时清�? 不该在本来就�?
    // 锁定时反�?reset�?
    bool had_lock = false;
    double last_source_age_ms = -1;
    // 上一次把检测喂�?PIDF 的墙钟时�?—�?用它�?dt, 与老实现一致�?
    auto last_tick_ts = std::chrono::steady_clock::time_point::min();

    g_pid_last_err_px.store(0.0f);
    g_pid_mode_track.store(false);

    while (!shouldExit && !session_stop_requested.load())
    {
        bool hasNewDetection = false;
        double detection_age_ms = 0.0;
        double detection_interval_ms = 0.0;
        // 延迟探针: 本拍消费的那批检�? 其像素的采集时刻 / 发布时刻�?
        int64_t probed_frame_capture_ns = 0;
        int64_t probed_publish_ns = 0;
        runtime::FrameContext frameContext;

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
                // 探针: 记录"看到了一批新检�?。与 markAimConsume(只统计被消费�?
                // 分开, 这样日志才能区分"瞄准键没按下"�?采集/推理真的停更�?�?
                runtime::latency::noteDetectionSeen();
            }
            detection_interval_ms = detectionBuffer.last_interval_ms;
            if (detectionBuffer.stamp.time_since_epoch().count() != 0)
                detection_age_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - detectionBuffer.stamp).count();
            probed_frame_capture_ns = detectionBuffer.frame_stamp_ns;
            frameContext = detectionBuffer.frame_context;
            probed_publish_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                detectionBuffer.stamp.time_since_epoch()).count();
        }

        // 只用硬件工作线程已完成的位移更新控制器测量状态�?
        // 队列中尚未发送的命令不再被误当成真实鼠标位移�?
        const auto movement_feedback = mouseThread.consumeMovementFeedback();
        g_mouse_queue_latency_ms.store(static_cast<float>(movement_feedback.latency_ms));
        g_mouse_queue_backlog.store(static_cast<int>(movement_feedback.backlog));
        g_mouse_send_failures.store(movement_feedback.failed);
        const auto config_snapshot = runtime_config::read();


        if (input_method_changed.exchange(false))
        {
            createInputDevices();
            assignInputDevices();
        }

        // 只有"采集回调前帧�?还会影响行为(延迟遥测的时间戳), 所以只有它需要重置控�?
        // 状态。像�?计数的映射与生效延迟估计随老链路恢复后已删�?—�?现役 PIDF 全程
        // 计数�? 不读这些值。这里保留重置是保守做法: 避免新旧时间模型混用�?
        if (last_source_age_ms != config_snapshot->capture_age_offset_ms)
        {
            engine.reset(); aim_path_driver.reset(); reset_trigger(mouseThread, trigger);
            mouseThread.clearQueuedMoves(); had_lock = false;
            last_source_age_ms = config_snapshot->capture_age_offset_ms;
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

            if (resolution_changed || hotkey_changed)
            {
                engine.reset();
                aim_path_driver.reset();
                reset_trigger(mouseThread, trigger);
                mouseThread.clearQueuedMoves();
                // 与老实现一�? 热键/分辨率切换后第一�?dt 退�?1/60, 不用跨越切换的间隔�?
                last_tick_ts = std::chrono::steady_clock::time_point::min();
                had_lock = false;
                publish_boss_debug(engine);
            }
        }

        // No active hotkey �?idle. Force-release the fire button and clear
        // any queued moves so the cursor stops drifting after the user lifts
        // the trigger.
        if (!profile_ptr)
        {
            // Hotkey transitions already cancel once above; never flood the
            // device with a cancellation packet on every idle poll.
            continue;
        }

        // 正常缓存按“空检测帧”递增 missed；若采集/推理完全停更，就没有�?
        // version 可递增。用最近检测节奏把缓存帧数换算成超时，避免旧锁�?
        // 永久挂住，同时仍不拿陈旧 anchor 驱动 mover�?
        //
        // 超时下限 3 个采集周�? 这个分支的目的只�?发现检测流真的断了", 不该
        // 被正常的发布抖动(8.2ms 的节奏偶�?9~10ms)触发 —�?engine.reset() 会连
        // PIDF �?residual 一起清�? 那正�?逼近锚点后停住不再收�?的直接原因�?
        if (!hasNewDetection)
        {
            runtime::chainlog::event(",reason=no_new_detection,age_ms=%.2f,"
                                     "interval_ms=%.2f", detection_age_ms,
                                     detection_interval_ms);
            const double cadence_ms = (detection_interval_ms > 0.0)
                ? std::clamp(detection_interval_ms, 1.0, 1000.0)
                : (1000.0 / 60.0);
            const double cache_timeout_ms = cadence_ms
                * 3.0;  // 缓存已删�? 固定 3 个采集周期就�?检测流断了"
            if (had_lock && detection_age_ms > cache_timeout_ms)
            {
                engine.reset();
                publish_boss_debug(engine);
                mouseThread.clearQueuedMoves();
                reset_trigger(mouseThread, trigger);
                g_pid_last_err_px.store(0.0f);
                had_lock = false;
            }
            runtime::chainlog::end_frame();
            // ── 控制节拍: 没有新检测就跳过, 与老实现一�?──────────────────────
            // �?AVA 链路(以及它的 PIDF)是【每个新检�?tick 一次】。重构把它改�?
            // "独立控制时钟", 去掉了这里的 continue, 于是 PIDF 每轮鼠标循环都被调用,
            // 频率约为检测率�?2.5 �?—�?�?dt 仍按检测间隔计算。这会同时改�?
            //   · D �?(Δerror/dt): 相邻两次误差几乎没变 -> 微分作用被稀�?
            //   · 前馈学习�?(ff_state += ff_error/dt * lr): 学习速度被改�?
            //   · 分数余量 (residual += step): 累积节奏被改�?
            // 结果就是"同一个参数手感完全不�?。所以这里必须保�?continue�?
            continue;
        }

        if (hasNewDetection && (probed_frame_capture_ns <= 0 || (frameContext.width>0 &&
            (frameContext.width!=config_resolution || frameContext.height!=config_resolution))))
        {
            engine.reset();
            if (had_lock) { reset_trigger(mouseThread, trigger); mouseThread.clearQueuedMoves(); }
            had_lock = false;
            continue;
        }

        // 延迟探针 T3: 控制环此刻真正拿到了一批【新鲜】检测。到这里的路�?
        // 已经排除了无新帧与陈旧缓存的 continue, 所以结算出来的是真实消�?
        // 延迟, 不会�?读旧数据"污染。T4 由携带本帧戳的指令发送完成后结算�?
        const int64_t aim_consume_ns = hasNewDetection
            ? runtime::latency::markAimConsume(probed_frame_capture_ns, probed_publish_ns)
            : runtime::latency::nowNs();

        // 全链路日�? 开一帧。本帧后续所有记录都会带上同一�?frame= 便于串联�?
        runtime::chainlog::begin_frame(static_cast<std::int64_t>(lastVersion));

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
        // 目标槽位 = 该热键的 aim_classes 列表 (顺序即优先级)�?
        // 用户面范围语�?1 = 框顶,0 = 框底。引擎在每次新锁定时
        // 从范围内抽一次并保持，切�?重新锁定才重新抽样�?
        in.target_slots.clear();
        in.target_slots.reserve(profile_ptr->aim_classes.size());
        // min_conf 语义: >0 视作该类别的私有阈�? ==0 视作 "跟随全局",
        // �?AI 页的 config.confidence_threshold 顶上, 从而和 GPU/CPU �?
        // �?NMS/后处理阈值保持一�?不会低于全局设置)�?
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
        // �?PID 参数(单位�?mouse/aim_pid.h):
        //   Kp  计数/(像素*�?   Ki  1/�?  Kd  �?
        //   延迟预测/提前�?�?  P 项饱和阈�?像素   限幅 计数/�?
        // 配置字段沿用 AVA 时代留下�?pidf_* 槽位, 语义已换成上面这�?界面标题同步改了)�?
        // 旧配置里这些值是�?每拍增益/前馈强度"写的, 范围完全不同: 超范围的一律按新默�?
        // 处理, 否则老配置文件会给出一个荒唐的回路增益�?
        auto pid_params = [](const HotkeyProfile& hp, bool x_axis) {
            boss::AimPidParams p;
            const double kp = static_cast<double>(x_axis ? hp.pidf_kp_x : hp.pidf_kp_y);
            const double ki = static_cast<double>(x_axis ? hp.pidf_ki_x : hp.pidf_ki_y);
            const double kd = static_cast<double>(x_axis ? hp.pidf_kd_x : hp.pidf_kd_y);
            const double predict = static_cast<double>(x_axis ? hp.pidf_lr_x : hp.pidf_lr_y);
            const double lead = static_cast<double>(x_axis ? hp.pidf_kf_x : hp.pidf_kf_y);
            const int deadzone = x_axis ? hp.pidf_deadzone_x : hp.pidf_deadzone_y;
            const int limit = x_axis ? hp.pidf_limit_x : hp.pidf_limit_y;
            const boss::AimPidParams fallback{};
            p.kp = (kp > 0.0 && kp <= 400.0) ? kp : fallback.kp;
            p.ki = (ki > 0.0 && ki <= 60.0) ? ki : fallback.ki;
            p.kd = (kd >= 0.0 && kd <= 0.3) ? kd : fallback.kd;
            p.predict_time_s = (predict >= 0.0 && predict <= 0.6) ? predict : fallback.predict_time_s;
            p.lead_time_s = (lead >= 0.0 && lead <= 0.6) ? lead : fallback.lead_time_s;
            // pidf_deadzone_* 这个槽位【不再当死区用�? 死区已整段删�?它会�?
            // 误差在瞄点周围永久丢精度, 并制�?10 �?秒的输出抖动)。槽位借来�?
            // P 项饱和阈�? 该像素数以内满增�? 超过则饱和�?
            //
            // �?语义冲突保护: 旧配置里这个值是【死区宽度�? 量级 3~5px。若原样�?
            //   饱和阈值用, |e|>5px 就全部削成常�? 等效增益变成 Kp*5/|e| —�?
            //   回路直接软掉, 比有死区时更糟。所以凡�?<= 20px 的旧值一律按"未设�?
            //   处理, 回落到内置默�?100px�?
            constexpr int kMinSanePSatPx = 20;   // 20px 以下当旧死区值丢�?
            p.p_full_scale_px = (deadzone > kMinSanePSatPx)
                ? static_cast<double>(deadzone)
                : fallback.p_full_scale_px;
            p.limit_counts = std::max(0, limit);
            return p;
        };
        in.pid_x = pid_params(*profile_ptr, true);
        in.pid_y = pid_params(*profile_ptr, false);
        // 「每计数像素�? 手填就用, 0 = 自动估算(�?config.h 注释)�?
        {
            const auto clean = [](float v) {
                const double d = static_cast<double>(v);
                return (std::isfinite(d) && d > 0.0 && d <= 20.0) ? d : 0.0;
            };
            in.px_per_count_x = clean(profile_ptr->aim_px_per_count_x);
            in.px_per_count_y = clean(profile_ptr->aim_px_per_count_y);
        }
        in.lost_target_cache_frames = 0;  // 缓存功能已按需求删除: 丢了就立刻释放, 不留滑行窗口
        // 瞄点滤波(anchor_filter_ms) 已移�? 位置不再平滑(一拍贴到瞄�?, 速度低通在
        // 观测器内部。见 mouse/anchor_observer.h�?
        // 生效参数一变就记一行。排�?手感突然不一�?�? 第一件要确认的事就是
        // "现在到底哪一组参数在生效" —�?之前有过一�?配置文件里是 100, 实际在跑 20"
        // 的情�? 就是因为看不到这一行才查了半天�?
        {
            static boss::AimPidParams last_x{};
            static boss::AimPidParams last_y{};
            static bool logged = false;
            const auto same = [](const boss::AimPidParams& a, const boss::AimPidParams& b) {
                return a.kp == b.kp && a.ki == b.ki && a.kd == b.kd
                    && a.predict_time_s == b.predict_time_s && a.lead_time_s == b.lead_time_s
                    && a.p_full_scale_px == b.p_full_scale_px && a.limit_counts == b.limit_counts;
            };
            if (!logged || !same(in.pid_x, last_x) || !same(in.pid_y, last_y))
            {
                runtime::chainlog::event(
                    ",reason=pid_params,"
                    "x=(kp=%.1f,ki=%.2f,kd=%.3f,predict=%.3f,lead=%.3f,psat=%.0f,lim=%d),"
                    "y=(kp=%.1f,ki=%.2f,kd=%.3f,predict=%.3f,lead=%.3f,psat=%.0f,lim=%d),"
                    "obs=(smooth_ms=%.0f,dead_time_s=%.3f,comp=%.2f,vel_tau_ms=%.0f,gate_px=%.0f),"
                    "k=(%.4f,%.4f)",
                    in.pid_x.kp, in.pid_x.ki, in.pid_x.kd, in.pid_x.predict_time_s,
                    in.pid_x.lead_time_s, in.pid_x.p_full_scale_px, in.pid_x.limit_counts,
                    in.pid_y.kp, in.pid_y.ki, in.pid_y.kd, in.pid_y.predict_time_s,
                    in.pid_y.lead_time_s, in.pid_y.p_full_scale_px, in.pid_y.limit_counts,
                    boss::kAnchorObserverSmoothMs, boss::kAimDeadTimeS,
                    boss::kAimDeadTimeCompScale, boss::kAnchorObserverVelTauMs,
                    boss::kAnchorObserverGatePx,
                    in.px_per_count_x, in.px_per_count_y);
                last_x = in.pid_x;
                last_y = in.pid_y;
                logged = true;
            }
        }
        in.crosshair_x = pivot.x;
        in.crosshair_y = pivot.y;
        in.fov_radius_x = fov_rx;
        in.fov_radius_y = fov_ry;
        in.image_size   = static_cast<double>(config_resolution);

        // dt = 两次 tick 之间的【真实墙钟间隔�? 与老实现一致�?
        // 不要改用 detection_interval_ms 的平滑�? dt 直接进微分项 (Δerror/dt)�?
        // 前馈学习 (ff_state += ff_error/dt * lr) �?ff_output (= ff_state*dt),
        // 换成平滑值会让微分与前馈的作用都偏离老手感。首�?tick �?1/60�?
        const auto now = std::chrono::steady_clock::now();
        double dt = 1.0 / 60.0;
        if (last_tick_ts != std::chrono::steady_clock::time_point::min())
            dt = std::chrono::duration<double>(now - last_tick_ts).count();
        last_tick_ts = now;

        const boss::EngineOutput out = engine.tick(in, dt);

        publish_boss_debug(engine);
        if (!out.have_target)
        {
            if (had_lock) { reset_trigger(mouseThread, trigger); mouseThread.clearQueuedMoves(); }
            had_lock = false;
            continue;
        }

        // ─── Drive mouse ───
        if (out.have_target)
        {
            had_lock = true;
            // 现役�?AVA 管线直接输出【鼠标计数�? 全程计数�? 不经过像�?>计数换算,
            // 因此不需�?mouse_pixels_per_count 标定, 也就不存在标定错导致的自激�?
            double drive_dx = out.dx;
            double drive_dy = out.dy;
            // 本帧扳机是否在命中区�?—�?扳机 FSM 在后面才�? 这里先给默认�?
            // 供本帧的全链路日志使�?日志在扳�?FSM 之前落盘)�?
            bool trigger_in_zone = false;
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
            // 只有【真换目标�?身份变了且瞄点跳�?=25px)才清在途指令。tracker 重锁同一个目�?
            // 也清的话, 每秒会清 8 �? 在途位移被反复丢弃, 压枪/跟枪都会被拖断�?
            if (out.target_switched)
                mouseThread.clearQueuedMoves();
            // 老链�? 输出已经是计�? 直接发送。限�?死区�?PIDF 后处理里已经做过�?
            const int64_t send_capture_ns = probed_frame_capture_ns
                - static_cast<int64_t>(config_snapshot->capture_age_offset_ms * 1e6);
            mouseThread.sendRawMove(static_cast<int>(std::lround(drive_dx)),
                static_cast<int>(std::lround(drive_dy)), send_capture_ns, aim_consume_ns);

            // 全链路日�? 本帧的完整现�?检�?找色枢轴/锚点/误差/控制器输�?整形�?
            // 位移/队列/扳机相位/延迟探针)。字段含义见 runtime/chain_log.h�?
            {
                const double pivot_xy[2] = {pivot.x, pivot.y};
                const double crosshair_xy[2] = {
                    static_cast<double>(in.crosshair_x),
                    static_cast<double>(in.crosshair_y)};
                const double anchor_xy[2] = {
                    static_cast<double>(out.anchor.x),
                    static_cast<double>(out.anchor.y)};
                chain_log_aim_frame(
                    static_cast<std::int64_t>(lastVersion),
                    static_cast<int>(precise_boxes.size()),
                    config_resolution, detection_interval_ms,
                    pivot_xy, profile_ptr != nullptr && profile_ptr->crosshair_detect_enabled,
                    crosshair_xy,
                    in.fov_radius_x, in.fov_radius_y,
                    out.current_track_id, anchor_xy,
                    static_cast<double>(out.bbox.width),
                    static_cast<double>(out.bbox.height),
                    out.class_id,
                    static_cast<double>(out.observed_bbox.x),
                    static_cast<double>(out.observed_bbox.y),
                    out.dx, out.dy, out.coasting, out.motion_suppressed,
                    static_cast<int>(std::lround(drive_dx)), static_cast<int>(std::lround(drive_dy)),
                    movement_feedback.backlog, movement_feedback.latency_ms,
                    movement_feedback.failed,
                    static_cast<int>(trigger.phase),
                    profile_ptr->trigger_enabled,
                    trigger_in_zone);

                // 控制器内部现�? 排查"抽一�?冲过�?停不�?时最关键的一段�?
                // k̂/v̂/前馈都只有在线估出来的�? 没有它们就只能靠猜�?
                // 字段含义:
                //   dt_ms    本拍真实 tick 间隔(掉帧/换目标跳帧会变大, 输出按它缩放)
                //   sup/switch/jump  身份变化 / 真换目标 / 瞄点跳了多少像素
                //   ex,ey    原始误差(像素)     eux,euy  加前馈后送给 P/I 的误�?
                //   ffx,ffy  前馈偏移(像素)     fsx/fsy  速度前馈噪声�?0=视为静止)
                //   pndx/pndy 在途自身位�?Smith 补偿, 像素, 已从误差里扣�?
                //   ipx/dpx  积分�?微分项的像素当量
                //   ix/iy    积分状�?像素*�?   carry    未发出的计数零头
                //   cmdx/y   取整前的浮点指令    lim      本拍生效的输出上�?
                //   kx/ky    每计数多少像�?k̂)  vx/vy    目标自身速度(像素/�?
                //   fits     观测器已接受的有效拟合窗口数(0 = 还没标定出来)
                runtime::chainlog::write(
                    runtime::chainlog::SecPid, 2,
                    ",dt_ms=%.2f,sup=%d,switch=%d,jump_px=%.1f,ff_ready=%d,fits=%d/%d,"
                    "ex=%.2f,ey=%.2f,eux=%.2f,euy=%.2f,ffx=%.2f,ffy=%.2f,"
                    "fsx=%.2f,fsy=%.2f,"
                    "pndx=%.2f,pndy=%.2f,"
                    "ipx=%.2f,ipy=%.2f,dpx=%.2f,dpy=%.2f,ix=%.2f,iy=%.2f,"
                    "carryx=%.2f,carryy=%.2f,cmdx=%.2f,cmdy=%.2f,lim=%d/%d,"
                    "kx=%.4f,ky=%.4f,vx=%.1f,vy=%.1f",
                    out.pid_dt_ms, out.motion_suppressed ? 1 : 0, out.target_switched ? 1 : 0,
                    out.target_anchor_jump_px, out.pid_ff_ready ? 1 : 0,
                    out.pid_fits_x, out.pid_fits_y,
                    out.pid_error_px_x, out.pid_error_px_y,
                    out.pid_used_error_px_x, out.pid_used_error_px_y,
                    out.pid_feedforward_px_x, out.pid_feedforward_px_y,
                    out.pid_ff_scale_x, out.pid_ff_scale_y,
                    out.pid_pending_px_x, out.pid_pending_px_y,
                    out.pid_i_px_x, out.pid_i_px_y, out.pid_d_px_x, out.pid_d_px_y,
                    out.pid_integral_x, out.pid_integral_y,
                    out.pid_carry_x, out.pid_carry_y,
                    out.pid_cmd_x, out.pid_cmd_y, out.pid_limit_x, out.pid_limit_y,
                    out.pid_px_per_count_x, out.pid_px_per_count_y,
                    out.pid_target_vel_x, out.pid_target_vel_y);

                // 选择层现�? 本帧所�?可瞄"候选的�?类别/置信�?+ 引擎最终选了哪一个�?
                // 用来判断"瞄点在两个目标之间来回跳"(抽搐) 到底是选择层在换目�? 还是
                // 同一个目标的框自己在动。最多记 8 个候�? 超过只记总数�?
                {
                    constexpr int kMaxLogged = 8;
                    char cand[300];
                    int used = 0;
                    const std::size_t total = std::min(precise_boxes.size(), classes.size());
                    const std::size_t shown = std::min<std::size_t>(total, kMaxLogged);
                    for (std::size_t i = 0; i < shown; ++i)
                    {
                        const auto& b = precise_boxes[i];
                        const float conf = (i < confidences.size()) ? confidences[i] : 0.0f;
                        const int n = std::snprintf(
                            cand + used, sizeof(cand) - static_cast<std::size_t>(used),
                            "%s%.0f,%.0f,%.0f,%.0f,%d,%.2f",
                            (i == 0 ? "" : ";"), static_cast<double>(b.x),
                            static_cast<double>(b.y), static_cast<double>(b.width),
                            static_cast<double>(b.height), classes[i],
                            static_cast<double>(conf));
                        if (n <= 0 || used + n >= static_cast<int>(sizeof(cand)) - 1)
                            break;
                        used += n;
                    }
                    cand[used] = '\0';
                    runtime::chainlog::write(
                        runtime::chainlog::SecSelect, 3,
                        ",total=%zu,sel_cls=%d,sel_box=%.0f,%.0f,%.0f,%.0f,sel_anchor=%.1f,%.1f,"
                        "cand=%s",
                        total, out.class_id, static_cast<double>(out.bbox.x),
                        static_cast<double>(out.bbox.y), static_cast<double>(out.bbox.width),
                        static_cast<double>(out.bbox.height),
                        static_cast<double>(out.anchor.x), static_cast<double>(out.anchor.y),
                        cand);
                }
            }

            // 扳机模式: 0 = 长按(按住不松�?, >0 = 连点(�?duration 后松�?�?
            const bool trigger_hold_mode = profile_ptr->trigger_fire_duration <= 0;

            if (out.coasting)
            {
                // 长按模式: 短暂滑行(检测丢一两帧, tracker 还在预测)不松�?—�?
                // 松了再按就是"连点"那种顿挫。这里冻结扳机状�? 等目标观�?
                // 回来再由 FSM �?准星是否还在命中�?决定是否松手。目标真�?
                // 丢了会走下面 !have_target 分支强制松开�?
                if (!trigger_hold_mode)
                    reset_trigger(mouseThread, trigger);
            }

            // ─── 扳机 FSM (命中盒约�? ───
            // 长按模式 (trigger_fire_duration == 0): 进命中区按下, 一直按�?
            // 只有准星离开命中区才松手。连点模�?(>0): 老行�? �?N ms 松手,
            // 冷却后再按�?
            if (!out.coasting && profile_ptr->trigger_enabled)
            {
                // ── 命中区: 单一【框内区间】, 用原始 pivot 判定 (2026-09-12) ──────
                // 旧实现是两条规则相与, 而且基准不同:
                //   ① 以【瞄点】为中心的容差带  |pivot - anchor| <= bbox*0.5
                //   ② 以【框顶】为基准的人体边界  pivot.y >= bbox.y - 0.12*bbox.h
                // 两条的几何中心差了 0.1*bbox.h, 实测(2615 个扳机帧)有 30% 的帧结论
                // 相反(8.6% 只有①过 / 21.3% 只有②过), 于是 in_zone 反复摆动:
                // 连续段长度 100% <= 2 帧(<=16ms) —— 表现就是"扳机点一下就松"。
                //
                // 现在照 AimMagic 的形状改成【一个区间判完】, 以框为基准:
                //   区间 = [box_c - bbox*s/2, box_c + bbox*s/2], s = trigger_y_percent/100
                // 好处: 命中区与瞄点【解耦】—— 换瞄点(胸口/头部)不再改变触发几何。
                //
                // 判定输入换成【原始】pivot: 原来是 StaticCrosshairRef 平滑过的
                // (限速 1500px/s + 一阶惯性 + 120ms 丢帧冻结), 那会让"准星是否真的
                // 在框里"变成对一个滞后量的判断。
                const double s = std::max(0.1, profile_ptr->trigger_y_percent / 100.0);
                const double half_x = out.bbox.width * s * 0.5;
                const double half_y = out.bbox.height * s * 0.5;
                const double box_cx = static_cast<double>(out.bbox.x) + out.bbox.width * 0.5;
                const double box_cy = static_cast<double>(out.bbox.y) + out.bbox.height * 0.5;

                const auto raw = crosshair_runtime::read();
                const double judge_x = raw.valid ? static_cast<double>(raw.x) : pivot.x;
                const double judge_y = raw.valid ? static_cast<double>(raw.y) : pivot.y;
                const bool x_inside = std::abs(judge_x - box_cx) <= half_x;
                const bool y_inside = std::abs(judge_y - box_cy) <= half_y;

                const bool in_zone = x_inside && y_inside;

                // 全链路日志: 保留 in_tol / in_body 两个字段名以免下游脚本失效,
                // 但现在它们分别是"X 在区间内"与"Y 在区间内"。
                trigger_in_zone = in_zone;
                runtime::chainlog::write(
                    runtime::chainlog::SecTrigger, 2,
                    ",phase=%d,in_zone=%d,in_tol=%d,in_body=%d,tx=%.1f,ty=%.1f,"
                    "anchor_x=%.1f,anchor_y=%.1f,pivot_x=%.1f,pivot_y=%.1f",
                    static_cast<int>(trigger.phase), in_zone ? 1 : 0,
                    x_inside ? 1 : 0, y_inside ? 1 : 0,
                    half_x, half_y,
                    static_cast<double>(out.anchor.x),
                    static_cast<double>(out.anchor.y), judge_x, judge_y);

                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                const auto begin_fire = [&] {
                    trigger.in_zone_since_ms = -1;
                    trigger.phase_time_ms = now_ms;
                    mouseThread.pressLeftButton();
                    trigger.phase = TriggerPhase::Pressed;
                    // 长按模式不设按住上限(phase_target_ms 只在连点模式使用)�?
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
                        // 零延迟直通模式：�?delay �?0 时直接击发，实现机械级瞬�?
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
                        // 长按模式: 只要还在命中区就一直按�? 不做时长循环�?
                        // 准星离开命中区才松手, 然后走一次冷却间�?—�?避免�?
                        // 判定边缘"�?�?�?变成连点�?
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
                        // 冷却结束瞬间若依然在受击框内，无缝立即衔接下一轮爆发连�?
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
            // match �?close-enough centre = same track).
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

    // 全链路日�? 一次会话结束时导出最�?N �?文件名带时间�? 落在 logs/ �?�?
    // 环型缓冲�?一直在�?�? 所以这里导出的是本次会话最后几秒的完整现场�?
    runtime::chainlog::end_frame();
    chain_log_auto_dump("session_end");
    mouseThread.clearQueuedMoves();
}
