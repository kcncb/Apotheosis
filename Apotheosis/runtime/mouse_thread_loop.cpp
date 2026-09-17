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
#include "trigger_scope.h"
#include "auto_stop.h"
#include "boss_aim.h"
#include "capture.h"
#include "crosshair/crosshair_runtime.h"
#include "mouse.h"
#include "Apotheosis.h"
#include "runtime/aim_telemetry.h"
#include "runtime/latency_probe.h"
#include "runtime/chain_log.h"
#include "mouse/autotune_runtime.h"   // 调参 agent: 每帧喂样本
#include <ctime>    // chain log: 时间戳文件名
// 全链路排查日志(用法见 chain_log.h 头部说明)
#include <string>   // chain log: 文件路径拼接
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
//   3. BossAimEngine.tick()  → (dx, dy, fire)
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
// 找色【不参与 PID 公式】, 它只回答"这一帧准星在画面哪个位置", 用来替代那个
// 原本写死的画面几何中心(静态准星)。所以它必须以一个【静态常量】的姿态进来
// 控制器, 而不是一个每帧都在跳的活信号 —— 否则 pivot 的压缩噪声、选簇翻转）
// 直接变成误差阶跃, 被控制器放大成抖动或"一顿一顿"。
//
// 三条约束(都只作用在参考点上, 不改变控制器本身):
//   1) 限幅  : 参考点每拍位移不超过kMaxSpeedPxPerSec×dt, 几十像素的假命中/
//               跳簇被摊到几拍里, 不会变成一次20px 的大跳
//   2) 【2026-09-13 删除】惯性一阶低通(crosshair_smooth)。理由见下面 update() 里
//      的说明 —— 平滑现在统一由 PID 之前的 anchor_filter 负责, 参考点直接用原始值。
//   3) 丢帧保持: 命中断掉先原地保持kHoldMs, 而不是瞬间弹回几何中心; 超时后
//               再限速滑回中心 —— 避免"参考点跳回中心"这种几十像素的阶跃。
//
// 注意 1 与 3 【保留】: 它们不是"平滑", 是抗野值与抗阶跃。限速那一条尤其重要 ——
// 它挡的是"假命中/选簇翻转"这种几十像素的单帧跳变, 那是检测器的错, 不该让控制器
// 吃下去。这与"把正常信号平滑掉"是两回事。
struct StaticCrosshairRef
{
    double x = 0.0;
    double y = 0.0;
    bool   engaged = false;

    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point last_hit{};
    bool have_update = false;

    // 参考点的最大移动速度。真实后坐力抬枪(几百 px/s)完全不受限, 只有单帧突跳
    // 会被削平: 56px 的假跳在 ~5 帧(42ms)内吸收完了
    static constexpr double kMaxSpeedPxPerSec = 1500.0;
    static constexpr int    kHoldMs           = 120;

    void reset()
    {
        engaged = false;
        have_update = false;
    }

    void update(bool fresh, double tx, double ty,
                double centre,
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
                // 第一次拿到命中, 直接落位, 不做缓动(否则会从中心慢慢爬过来)。
                x = tx;
                y = ty;
                engaged = true;
                last_hit = now;
                return;
            }

            // 【2026-09-13 删除】这里原来是 crosshair_smooth 驱动的惯性(一阶低通):
            //     tau = kMinTauSec + (kMaxTauSec - kMinTauSec) * smooth
            //     x += (tx - x) * (1 - exp(-dt/tau))
            // 删掉的理由: 平滑现在统一由 PID 之前的 anchor_filter(α-β)承担。两道串联
            // 滤波各自引入滞后、参数互相耦合, 而能滤掉的东西一样, 没有理由留两道。
            // 锚点"跟不跟手"只应该有一个地方可调。
            //
            // 保留的只有限速(clamp_step): 它挡的是单帧几十像素的假命中, 是抗野值,
            // 不是平滑 —— 正常的目标移动速度远低于 kMaxSpeedPxPerSec, 不会被它削到。
            double nx = tx;
            double ny = ty;
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
    // 把(nx,ny) 约束到"离当前参考点最多kMaxSpeedPxPerSec×dt"。返回false
    // 表示目标就是当前点, 无位移。
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

    g_static_crosshair.update(fresh, snap.x, snap.y, centre, now);

    // 参考点对外发布一份, 预览画的就是这个, 和原始命中点分开的。
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
    // in_zone 起点时间戳: 进入 Idle 后一旦目标进入命中区就打stamp,
    // 用(now - in_zone_since_ms) ≥delay 立即触发,不再空转一帧。
    int64_t in_zone_since_ms = -1;
    // 上次开火 / 上次锁定时间target track id,用于检测转火并进入 SwitchCooldown。
    int last_fire_track_id = -1;
    // 本轮 phase 目标时长(delay/duration/interval)已经算入随机抖动,
    // 避免在同一 phase 里每帧重摇导致门槛漂移。
    int32_t phase_target_ms = 0;
    // 自动开镜(右键)状态机。放在这里是为了让所有 reset_trigger() 调用点
    // 都自动带上"收镜" —— 否则会话停了右键会卡在按下。
    boss::ScopeController scope;
    // 自动急停(开火时补一个反方向键)状态机。同上, 它只记"上一发还在按着没有",
    // 键本身由固件定时弹起(自清), 所以 reset() 不需要发任何东西。
    boss::AutoStopController auto_stop;
    // 本拍扳机是否真的按下了左键(begin_fire 成功那一拍), 供自动急停用。
    bool fired_this_tick = false;
    void reset() { *this = {}; }
};

void release_trigger_outputs(MouseThread& mouse, TriggerState& state)
{
    if (state.phase == TriggerPhase::Pressed)
        mouse.releaseLeftButton();
}

// 只把自动开镜欠下的抬指补上(不改变接敌状态)。
void flush_scope(MouseThread& mouse, TriggerState& state)
{
    const auto action = state.scope.flushUp();
    if (action.release_right)
        mouse.releaseRightButton();
}

// 接敌/会话结束: 保证按住的右键被还回去, 并且把"点按"重新武装给下一次接敌。
void release_scope(MouseThread& mouse, TriggerState& state)
{
    const auto action = state.scope.forceRelease();
    if (action.release_right)
        mouse.releaseRightButton();
    if (action.press_right)
        mouse.pressRightButton();
}

// keep_scope_state = true: 接敌【途中】的复位(丢目标/滑行/检测流断)。
// 点按模式在一次接敌里只点一下右键, 所以这种复位必须保住接敌状态 —— 否则每次
// 重新锁到目标都会又点一下, 在"切换开镜"的游戏里把镜来回切。欠下的抬指照样补发,
// 右键不会卡住。
// keep_scope_state = false (默认): 热键松开/换热键/会话结束 = 新的接敌, 重新武装。
void reset_trigger(MouseThread& mouse, TriggerState& state, bool keep_scope_state = false)
{
    release_trigger_outputs(mouse, state);
    if (keep_scope_state)
    {
        flush_scope(mouse, state);
        const boss::ScopeController kept = state.scope;
        // 自动急停的计时状态也要保住: 它记着"上一发还在按着没有"。丢掉的话,
        // 丢目标->再开火会在 60ms 窗口内重发一次, 固件那边的计时被重置,
        // 反方向键实际按住的时间就翻倍(玩家会被推着倒走一段)。
        const boss::AutoStopController kept_stop = state.auto_stop;
        state.reset();
        state.scope = kept;
        state.auto_stop = kept_stop;
        return;
    }
    release_scope(mouse, state);
    state.reset();
}

// 对基础延迟加±jitter 抖动,结果不小于0。thread_local RNG 避免锁竞争。
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
// 主循环里只调用下面这些函数, 每个调用点一次 —— 详细字段在这里统一拼装, 免得把
// 大段格式化代码散落到延迟敏感的主循环里。字段含义见 runtime/chain_log.h。
namespace chain_detail
{
inline std::string dump_path(const char* tag)
{
    // 落在 exe 同级下logs/ 里, 文件名带时间戳与标签, 便于事后认领。
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

// 有目标、且本帧真的走完了控制与下发流程时调用。
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

// "这一帧为什么没打/为什么松手" —— 排查时最常问的问题, 所以单独记成关键事件。
inline void chain_log_skip(const char* reason)
{
    runtime::chainlog::event(",reason=%s", reason);
}

// 自动导出最近N 帧(环形缓冲), 文件名带时间戳, 失败时不抛异常, 只记一条事件。
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

    // 本会话是否真的持有过锁定。重构时丢掉的老代码守卫(ev_last_track_id != -1)
    // 表达的就是这件事: 缓存超时只应该在"锁定过又断流"时清掉, 不该在本来就没
    // 锁定时反而reset。
    bool had_lock = false;
    // (last_source_age_ms 已随 capture_age_offset_ms 一起删除 2026-09-13)
    // 上一次把检测喂给 PIDF 的墙钟时刻 —— 用它算 dt, 与老实现一致。
    auto last_tick_ts = std::chrono::steady_clock::time_point::min();
    // dt 的一阶低通状态(见下面主循环里的说明)。0 = 尚未初始化。
    double dt_smoothed = 0.0;

    // 【2026-09-13 删除】last_calib_log_ts —— 那是给「每计数像素」测量的诊断日志
    // 做 0.5s 节流用的。标定链路整体移除, 不再需要。

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
                // 探针: 记录"看到了一批新检测"。与 markAimConsume(只统计被消费的
                // 分开, 这样日志才能区分"瞄准键没按下"和"采集/推理真的停更了"。
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

        // 只用硬件工作线程已完成的位移更新控制器测量状态。
        // 队列中尚未发送的命令不再被误当成真实鼠标位移。
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

        // ── 【2026-09-13 删除】「每计数像素」测量块 ─────────────────────
        // 这里原来是 ~110 行: 处理界面的测量请求/取消、每 0.5s 往链路日志打一条 verdict 诊断、
        // 取走测量结果、以及把"自动估算的 k̂"发布给界面。全部删除 —— 前馈删除后
        // 控制器不再消费 k̂, 整条标定链路失去消费者。详见 docs/aimmagic-comparison.md §6.8。
        // 界面的「测量」按钮同步移除。


        // 【2026-09-13 删除】原来这里有一段: 当 capture_age_offset_ms 变化时重置整个
        // 控制状态(engine / aim_path / trigger / 队列 / 锁定标志), 因为那个手填值会改变
        // 延迟遥测的时间戳基准, 混用新旧基准没有意义。
        //
        // 那个旋钮已删除(控制器是纯反馈, 不吃任何延迟估计), 所以"基准变了要重置"这个
        // 前提消失, 整段跟着删掉。真实延迟仍由 latency_probe 逐帧实测。
        // 顺带一提: 那个重置本身也很粗暴 —— 用户改一下帧龄估计就会丢掉锁定状态。

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
                // 与老实现一样: 热键/分辨率切换后第一拍dt 退回1/60, 不用跨越切换的间隔。
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
            // Hotkey transitions already cancel once above; never flood the
            // device with a cancellation packet on every idle poll.
            continue;
        }

        // 正常缓存按“空检测帧”递增 missed；若采集/推理完全停更，就没有新
        // version 可递增。用最近检测节奏把缓存帧数换算成超时，避免旧锁定
        // 永久挂住，同时仍不拿陈旧 anchor 驱动 mover。
        //
        // 超时下限 3 个采集周期: 这个分支的目的只是"发现检测流真的断了", 不该
        // 被正常的发布抖动(8.2ms 的节奏偶尔9~10ms)触发 —— engine.reset() 会连
        // PIDF 和 residual 一起清掉, 那正是逼近锚点后停住不再收敛的直接原因。
        if (!hasNewDetection)
        {
            runtime::chainlog::event(",reason=no_new_detection,age_ms=%.2f,"
                                     "interval_ms=%.2f", detection_age_ms,
                                     detection_interval_ms);
            const double cadence_ms = (detection_interval_ms > 0.0)
                ? std::clamp(detection_interval_ms, 1.0, 1000.0)
                : (1000.0 / 60.0);
            const double cache_timeout_ms = cadence_ms
                * 3.0;  // 缓存已删掉, 固定 3 个采集周期就是"检测流断了"
            if (had_lock && detection_age_ms > cache_timeout_ms)
            {
                engine.reset();
                publish_boss_debug(engine);
                mouseThread.clearQueuedMoves();
                reset_trigger(mouseThread, trigger, /*keep_scope_state=*/true);
                g_pid_last_err_px.store(0.0f);
                had_lock = false;
            }
            runtime::chainlog::end_frame();
            // ── 控制节拍: 没有新检测就跳过, 与老实现一致 ──────────────────────
            // 老 AVA 链路(以及它的 PIDF)是【每个新检测 tick 一次】。重构把它改成
            // "独立控制时钟", 去掉了这里的 continue, 于是 PIDF 每轮鼠标循环都被调用,
            // 频率约为检测率的 2.5 倍 —— 但 dt 仍按检测间隔计算。这会同时改变:
            //   · D 项 (Δerror/dt): 相邻两次误差几乎没变 -> 微分作用被稀释
            //   · 前馈学习率 (ff_state += ff_error/dt * lr): 学习速度被改变
            //   · 分数余量 (residual += step): 累积节奏被改变
            // 结果就是"同一个参数手感完全不同"。所以这里必须保持 continue。
            //
            // ★ PID-EventSync (本档唯一链路): 帧间【不许】外推补拍 —— 这是档位语义
            //   的一部分。EventSync 的定义就是"每次推理只消费一次"; AimMagic 的
            //   EventSync 档在两次推理之间不发任何位移(它等条件变量睡到下一帧),
            //   帧间插值是它另一个档(kalman/FrameSync)的行为。两条同时开会叠成两层
            //   外推, 提前量被算两遍 —— 同源于 aim_pid.h 里"同一个物理量不许扣两遍"
            //   的教训。所以这里直接等下一帧真实观测, 不做任何预测补拍。
            //   (旧的 use_prediction_tick / prediction_tick_* 三个键已随之删除。)
            continue;
        }

        if (hasNewDetection && (probed_frame_capture_ns <= 0 || (frameContext.width>0 &&
            (frameContext.width!=config_resolution || frameContext.height!=config_resolution))))
        {
            engine.reset();
            if (had_lock) { reset_trigger(mouseThread, trigger, /*keep_scope_state=*/true); mouseThread.clearQueuedMoves(); }
            had_lock = false;
            continue;
        }

        // 延迟探针 T3: 控制环此刻真正拿到了一批【新鲜】检测。到这里的路径
        // 已经排除了无新帧与陈旧缓存的 continue, 所以结算出来的是真实消耗
        // 延迟, 不会被"读旧数据"污染。T4 由携带本帧戳的指令发送完成后结算。
        const int64_t aim_consume_ns = hasNewDetection
            ? runtime::latency::markAimConsume(probed_frame_capture_ns, probed_publish_ns)
            : runtime::latency::nowNs();

        // 全链路日志: 开一帧。本帧后续所有记录都会带上同一个frame= 便于串联。
        runtime::chainlog::begin_frame(static_cast<std::int64_t>(lastVersion));

        const auto pivot = resolve_crosshair_pivot(
            profile_ptr, config_resolution,
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
        // 目标槽位 = 该热键的 aim_classes 列表 (顺序即优先级)；
        // 用户面范围语义: 1 = 框顶,0 = 框底。引擎在每次新锁定时
        // 从范围内抽一次并保持，切换/重新锁定才重新抽样。
        in.target_slots.clear();
        in.target_slots.reserve(profile_ptr->aim_classes.size());
        // min_conf 语义: >0 视作该类别的私有阈值 ==0 视作 "跟随全局",
        // 由AI 页的 config.confidence_threshold 顶上, 从而和 GPU/CPU 的
        // 和NMS/后处理阈值保持一致(不会低于全局设置)。
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
        // 新PID 参数(单位见mouse/aim_pid.h):
        //   Kp  计数/(像素*秒)   Ki  1/秒  Kd  秒
        //   延迟预测/提前量 秒  P 项饱和阈值 像素   限幅 计数/拍
        // 配置字段沿用 AVA 时代留下的pidf_* 槽位, 语义已换成上面这套(界面标题同步改了)。
        // 旧配置里这些值是按每拍增益/前馈强度"写的, 范围完全不同: 超范围的一律按新默认
        // 处理, 否则老配置文件会给出一个荒唐的回路增益。
        auto pid_params = [](const HotkeyProfile& hp, bool x_axis) {
            boss::AimPidParams p;
            const double kp = static_cast<double>(x_axis ? hp.pidf_kp_x : hp.pidf_kp_y);
            const double ki = static_cast<double>(x_axis ? hp.pidf_ki_x : hp.pidf_ki_y);
            const double kd = static_cast<double>(x_axis ? hp.pidf_kd_x : hp.pidf_kd_y);
            // ★ 2026-09-13: pidf_lr_*(延迟预测) 与 pidf_kf_*(提前量) 这两个槽位【已停用】。
            //   它们是"目标速度 × 可调时间"的独立偏移, 稳态下就是 v̂*时间 的固定瞄偏 ——
            //   用户实测准星会稳定停在目标的下一拍位置而不是目标身上。
            //   现在链路延迟补偿改由 AimPidFeedback::dead_time_s 用【实测死区】配对完成
            //   (扣自己的位移 + 补敌人的位移, 稳态抵消), 不再需要也不允许这两个旋钮。
            //   配置键保留读取只为兼容老配置文件, 值被忽略。
            const int psat = x_axis ? hp.pidf_psat_x : hp.pidf_psat_y;
            const int limit = x_axis ? hp.pidf_limit_x : hp.pidf_limit_y;
            const boss::AimPidParams fallback{};
            p.kp = (kp > 0.0 && kp <= 400.0) ? kp : fallback.kp;
            p.ki = (ki > 0.0 && ki <= 60.0) ? ki : fallback.ki;
            p.kd = (kd >= 0.0 && kd <= 0.3) ? kd : fallback.kd;
            // ── P 项饱和阈值 (2026-09-14 由「移动死区」改名) ─────────────────
            // 死区语义已整段删除(它会让误差在瞄点周围永久丢精度, 并制造 10 次/秒的
            // 输出抖动)。这个槽位现在正式表示: |e| <= psat 时 P 项满增益, 超过则按
            // psat/|e| 连续衰减(经过原点, 无硬切换)。
            //
            // ★ 0 = 关闭(生产默认)。实测(400px 阶跃): 饱和对过冲的影响【取决于 Kp】——
            //   Kp 15/25 时饱和让过冲变差(12.1→20.4 / 11.0→21.6), Kp 35+ 时变好
            //   (43.6→28.2 / 145.6→50.1), 转折点在 Kp≈30。生产 Kp=35 压在转折点右侧,
            //   为了防止"调过头", 默认关闭(少一个非线性环节)。
            //
            // ★ 老配置兼容: v6 迁移已把 <=20 的旧【死区】值丢弃(那种值当饱和阈值用
            //   会把回路静默变软), 所以这里不必再判一次。
            p.p_full_scale_px = static_cast<double>(psat);
            p.limit_counts = std::max(0, limit);

            // ── 在途自身位移补偿 (Smith 类, 2026-09-13 重做并【重新启用】) ──────
            //
            // 它曾被停用, 理由是"没解决用户的问题"。重做后它成了本方案的主力:
            // 纯反馈在 46ms 死区下的增益天花板是 g_crit = 0.2602, 对应本机
            // k̂≈0.593 时 Kp ≈ 52 —— 这就是"Kp 拉高会晃、拉低拉枪慢"的物理根源。
            // 在途补偿把"已发出、游戏里已生效、画面还没回来"的那批计数扣掉, 环路
            // 退化成每拍收缩 1-g 倍, 于是 Kp 可以开到 35 而尾段稳定在 0.295px。
            //
            // ★ 表达式里【没有 k̂】: 补偿直接在【计数域】扣 (u -= beta*N/W),
            //   N 是本类自己记账的下发计数, W 是窗口拍数。全程不做 counts<->px 换算,
            //   所以不存在"标定不准就自激"的老毛病(那正是旧 predictive_controller
            //   被删除的原因)。
            //
            // pidf_inflight_x/y 槽位语义已换成这个无量纲的 beta(见 config.h)。
            // 实测默认 1.6(见 mouse/aim_pid.h 的扫描表): 60/120/240/1000fps 四个
            // 帧率上尾段一致 0.295px, 且离 60fps 的发散点(beta=2.0)有一档裕度。
            constexpr double kDefaultInflightBeta = 1.6;
            const double inflight =
                static_cast<double>(x_axis ? hp.pidf_inflight_x : hp.pidf_inflight_y);
            // 0 是合法值(= 关闭), 所以不能用 "> 0" 判断"是否填了"; 用负值/非数表示
            // "没填", 回落到默认。> kMaxInflightBeta 的值交给 AimPid::configure 夹取。
            p.inflight_beta = (std::isfinite(inflight) && inflight >= 0.0)
                ? inflight
                : kDefaultInflightBeta;

            // 窗口 = 【真实链路死区】46ms。★ 不许用"软件 E2E ≈ 11ms": 它漏掉了
            // HID + 游戏帧 + 显示 + 采集缓冲。窗口小于真实死区 = 补不足(安全,
            // 只是回到原来的延迟); 大于真实死区 = 把已生效的位移当在途重复扣除
            // → 正反馈发散。所以这里直接引用那个实测常数, 不让用户随便填大。
            p.inflight_window_s = boss::kAimDeadTimeS;
            return p;
        };
        in.pid_x = pid_params(*profile_ptr, true);
        in.pid_y = pid_params(*profile_ptr, false);

        // ─ PID-EventSync: 跟踪器 + 每轨预测状态机 (移植 AimMagic 1.0.30 全链路) ─
        // ★ 键名与 AM 一一对应(2026-09-16 重建): 跟踪器四个 + 预测四个。
        //   此前那一堆(关联半径 / 换算窗 / k̂ / 在途 beta / 自运动增益)在 AM 里
        //   **没有对应键**, 已整条删除 —— 见 docs/aimmagic-ground-truth.md §2.5。
        {
            boss::AimTrackerParams tp;
            tp.enable_tracking = true;   // 本档即跟踪器链路, 没有单独的开关
            tp.min_hits = profile_ptr->esync_min_hits;
            tp.max_age = profile_ptr->esync_max_age;
            tp.assoc_iou = static_cast<double>(profile_ptr->esync_assoc_iou);
            tp.vel_sample_ms = static_cast<double>(profile_ptr->esync_vel_sample_ms);
            // 预测补偿 (AM 的 AimKey 作用域)。
            tp.pred_factor_x = static_cast<double>(profile_ptr->esync_pred_factor_x);
            tp.pred_factor_y = static_cast<double>(profile_ptr->esync_pred_factor_y);
            tp.pred_min_w = static_cast<double>(profile_ptr->esync_pred_min_w);
            tp.pred_max_w = static_cast<double>(profile_ptr->esync_pred_max_w);
            // ★ 本项目自加的两道安全阀: 0 = 关闭 = 与 AM 逐位一致。
            //   沿用 pidf_predict_* 那组槽位(它们在 config.h 里仍存在)。
            tp.pred_max_lead_px = static_cast<double>(profile_ptr->pidf_predict_max_px);
            tp.pred_vel_floor = static_cast<double>(profile_ptr->pidf_predict_vel_floor);
            in.esync = tp;
        }

        // ── 尺度增益调度 s(bbox.height) —— 2026-09-14 新增, 同日改为"单基准" ──
        // 在这里把界面上的三个量填进引擎。s 同时作用在 X/Y 两个轴上(共用一个
        // AimScale 实例), 因为"目标多大"是目标本身的属性, 不分轴。
        //
        // ★ 2026-09-14 改版要点: 不再有"近处框高/远处框高"两个绝对阈值。用户明确
        //   指出他【测不出当前框高】, 换游戏/换分辨率也失效。改成单一基准(用户在
        //   某个距离上整定参数时, 那一段的框高中位数), 公式
        //       s = clamp((h/基准)^γ, s_min, s_max)
        //   基准 = 0(还没学到)时整条链路恒为中性 1.0。
        //
        // ★ 关闭时把两端都填成 1.0 = "全区间中性"的确切表达, 于是"关掉"与"还没有
        //   基准"两条路径得到的是【同一个】s=1.0, 也就逐位一致。
        {
            boss::AimScaleParams sp;
            const bool enabled = (profile_ptr->aim_scale_enabled != 0);
            sp.base_h_px = enabled
                ? static_cast<double>(profile_ptr->aim_scale_base_h) : 0.0;
            sp.s_max = enabled
                ? static_cast<double>(profile_ptr->aim_scale_max) : 1.0;
            sp.s_min = enabled
                ? static_cast<double>(profile_ptr->aim_scale_min) : 1.0;
            in.aim_scale = sp;
        }

        // 【2026-09-13 删除】in.px_per_count_x/y —— 「每计数像素」已不再是控制器输入
        // (前馈删除)。界面上对应的输入框与测量按钮也一并移除。
        in.lost_target_cache_frames = 0;  // 缓存功能已按需求删除: 丢了就立刻释放, 不留滑行窗口
        // 瞄点平滑现在由 mouse/anchor_filter.h 的 α-β 滤波器负责, 在 boss_aim 里
        // 紧挨着 PID 之前。参数是编译期常数(kAnchorFilterTauMs), 不是用户旋钮。
        // 生效参数一变就记一行。排查"手感突然不一样", 第一件要确认的事就是
        // "现在到底哪一组参数在生效" —— 之前有过一次"配置文件里是 100, 实际在跑 20"
        // 的情况, 就是因为看不到这一行才查了半天。
        {
            static boss::AimPidParams last_x{};
            static boss::AimPidParams last_y{};
            static bool logged = false;
            // 预测参数也要参与变化检测 —— 它们同样决定手感, 改动时必须留痕。
            static double last_pfx = 1e30, last_pfy = 1e30;
            static double last_pmn = -1.0, last_pmx = -1.0;
            const auto same = [](const boss::AimPidParams& a, const boss::AimPidParams& b) {
                return a.kp == b.kp && a.ki == b.ki && a.kd == b.kd
                    && a.p_full_scale_px == b.p_full_scale_px && a.limit_counts == b.limit_counts;
            };
            const boss::AimTrackerParams& trp = in.esync;
            if (!logged || !same(in.pid_x, last_x) || !same(in.pid_y, last_y)
                || trp.pred_factor_x != last_pfx || trp.pred_factor_y != last_pfy
                || trp.pred_min_w != last_pmn || trp.pred_max_w != last_pmx)
            {
                runtime::chainlog::event(
                    ",reason=pid_params,"
                    "x=(kp=%.1f,ki=%.2f,kd=%.3f,psat=%.0f,lim=%d),"
                    "y=(kp=%.1f,ki=%.2f,kd=%.3f,psat=%.0f,lim=%d),"
                    "predict=(fx=%.3f,fy=%.3f,minw=%.0f,maxw=%.0f,max_px=%.0f,vel_floor=%.0f),"
                    "filter_tau_ms=%.0f",
                    in.pid_x.kp, in.pid_x.ki, in.pid_x.kd,
                    in.pid_x.p_full_scale_px, in.pid_x.limit_counts,
                    in.pid_y.kp, in.pid_y.ki, in.pid_y.kd,
                    in.pid_y.p_full_scale_px, in.pid_y.limit_counts,
                    trp.pred_factor_x, trp.pred_factor_y,
                    trp.pred_min_w, trp.pred_max_w,
                    trp.pred_max_lead_px, trp.pred_vel_floor,
                    boss::kAnchorFilterTauMs);
                last_x = in.pid_x;
                last_y = in.pid_y;
                last_pfx = trp.pred_factor_x; last_pfy = trp.pred_factor_y;
                last_pmn = trp.pred_min_w; last_pmx = trp.pred_max_w;
                logged = true;
            }
        }
        in.crosshair_x = pivot.x;
        in.crosshair_y = pivot.y;
        in.fov_radius_x = fov_rx;
        in.fov_radius_y = fov_ry;
        in.image_size   = static_cast<double>(config_resolution);
        // 跟踪器的速度采样窗要一个【单调时钟】: AM 的 tracking_velocity_sample_ms
        // 是"两次观测的真实间隔超过 20ms 就重算", 用"拍数×dt"累加也能近似, 但窗的
        // 边界会随 dt 抖动。steady_clock 与 dt 同源(下面 dt 就是它算的), 所以一致。
        in.now_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // dt = 两次 tick 之间的墙钟间隔(秒)。首个 tick 用 1/120 兜底。
        //
        // ── 【2026-09-13 改】不再把裸间隔直接喂给控制器 ─────────────────────────
        // 原来这里写的是"用真实间隔, 不要平滑", 理由是前馈学习率(ff_state +=
        // ff_err/dt*lr)会因平滑而偏离手感。**那个理由随前馈一起删除了**, 现在
        // step() 只吃 (误差, dt), 没有任何按 1/dt 放大的状态。
        //
        // 而实测日志(chain_live.log, 41245 拍)里裸间隔是这样的:
        //     p10=7.45ms  p50=8.55ms  p90=11.64ms  max=23.22ms
        // 相对均值偏差: 一半的拍偏 15%, 最差偏 65% —— 也就是说等效增益一直在
        // 忽大忽小。独立仿真对比"均匀 1/120"与"这条实测分布":
        //     Kp=60  到位 0.83s -> 2.83s, 抖动 0.25px -> 4.25px, 跟踪滞后 52 -> 92px
        //     Kp=150 到位 0.43s -> 0.95s, 抖动 0.25px -> 0.50px, 跟踪滞后 21 -> 37px
        // 即: 抖动的 dt 让拉枪慢 2.2 倍、跟踪滞后大 80%, **与 Kp 调多少无关** ——
        // 用户怎么调都会被它拖累。所以必须在这里治理。
        //
        // 治理方式: 一阶低通(时间常数 kDtSmoothTau), 只滤【高频抖动】, 保留【趋势】。
        //   · 掉帧(真的要变慢)会被跟随: 低通的时间常数远小于掉帧持续的时间。
        //   · 采样抖动(±几毫秒)被压掉: 那正是等效增益抖动的来源。
        // 注意 dt 仍然会被 step() 夹到 [1/2000, 1/15], 所以异常值不会传进去。
        const auto now = std::chrono::steady_clock::now();
        double dt = 1.0 / 120.0;
        if (last_tick_ts != std::chrono::steady_clock::time_point::min())
            dt = std::chrono::duration<double>(now - last_tick_ts).count();
        last_tick_ts = now;
        // 低通平滑: alpha = dt/(tau+dt)。tau=25ms 约为典型拍间隔(8.5ms)的 3 倍,
        // 足以压掉逐拍抖动, 又能在几十毫秒内跟上真实的掉帧。
        {
            constexpr double kDtSmoothTau = 0.025;
            if (!(dt > 0.0) || dt > 0.5)
                dt = dt_smoothed > 0.0 ? dt_smoothed : (1.0 / 120.0);
            const double alpha = dt / (kDtSmoothTau + dt);
            dt_smoothed = (dt_smoothed > 0.0)
                ? dt_smoothed + (dt - dt_smoothed) * alpha
                : dt;
            dt = dt_smoothed;
        }

        const boss::EngineOutput out = engine.tick(in, dt);

        publish_boss_debug(engine);
        if (!out.have_target)
        {
            if (had_lock) { reset_trigger(mouseThread, trigger, /*keep_scope_state=*/true); mouseThread.clearQueuedMoves(); }
            had_lock = false;
            continue;
        }

        // ─── Drive mouse ───
        if (out.have_target)
        {
            had_lock = true;
            // 现役的AVA 管线直接输出【鼠标计数】, 全程计数域, 不经过像素>计数换算,
            // 因此不需要mouse_pixels_per_count 标定, 也就不存在标定错导致的自激。
            double drive_dx = out.dx;
            double drive_dy = out.dy;
            // 本帧扳机是否在命中区内 —— 扳机 FSM 在后面才跑, 这里先给默认值
            // 供本帧的全链路日志使用(日志在扳机FSM 之前落盘)。
            bool trigger_in_zone = false;
            boss::AimPathDriver::Params path;
            path.mode = static_cast<boss::AimPathDriver::Mode>(
                std::clamp(profile_ptr->aim_path_mode, 0, 3));
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
            // WindMouse 曲线 (mode=3): AM 的 wind_mouse_G0/W0/M0/D0 + curve_threshold。
            path.wind_gravity = static_cast<double>(profile_ptr->aim_path_wind_gravity);
            path.wind_wind = static_cast<double>(profile_ptr->aim_path_wind_wind);
            path.wind_step = static_cast<double>(profile_ptr->aim_path_wind_step);
            path.wind_distance = static_cast<double>(profile_ptr->aim_path_wind_distance);
            path.wind_threshold_px = static_cast<double>(profile_ptr->aim_path_wind_threshold);
            aim_path_driver.configure(path);
            const auto shaped = aim_path_driver.step(
                static_cast<double>(out.anchor.x),
                static_cast<double>(out.anchor.y),
                pivot.x, pivot.y, dt, out.current_track_id,
                drive_dx, drive_dy);
            drive_dx = shaped.move_x;
            drive_dy = shaped.move_y;
            // 只有【真换目标】(跟踪器身份变了)才清在途指令。跟踪器在滑行窗口内重锁
            // 同一个目标也清的话, 每秒会清 8 次, 在途位移被反复丢弃, 压枪/跟枪都会被拖断。
            if (out.target_switched)
                mouseThread.clearQueuedMoves();
            // 老链路: 输出已经是计数, 直接发送。限幅/死区在PIDF 后处理里已经做过了
            // 采集戳原样用(帧龄估计旋钮已删)。
            const int64_t send_capture_ns = probed_frame_capture_ns;
            const int send_dx = static_cast<int>(std::lround(drive_dx));
            const int send_dy = static_cast<int>(std::lround(drive_dy));
            mouseThread.sendRawMove(send_dx, send_dy, send_capture_ns, aim_consume_ns);

            // ── 【2026-09-16 删除】noteAimSend(send_dx, send_dy) ────────────────
            // 它是 AM 发送环的登记入口(供像素域在途补偿 ÷ k̂ 用)。整条像素域在途
            // 补偿已随 k̂ 一起删除 —— AM 里那条链由 FrameSync/EventSync 档消费, 而
            // k̂ 在双机架构下无法测量。在途补偿现在只有计数域一种落点
            // (AimPidParams::inflight_beta, 生产 1.6)。见 ground-truth §6/§8。

            // 全链路日志: 本帧的完整现场(检测/找色枢轴/锚点/误差/控制器输出/整形/
            // 位移/队列/扳机相位/延迟探针)。字段含义见 runtime/chain_log.h。
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

                // 控制器内部现场, 排查"抽一下/冲过头/停不住"时最关键的一段。
                // k̂/v̂/前馈都只有在线估出来的量, 没有它们就只能靠猜。
                // 字段含义:
                //   dt_ms    本拍真实 tick 间隔(掉帧/换目标跳帧会变大, 输出按它缩放)
                //   sup/switch/jump  身份变化 / 真换目标 / 瞄点跳了多少像素
                //   ex,ey    原始误差(像素)     eux,euy  加前馈后送给 P/I 的误差
                //   ffx,ffy  前馈偏移(像素)     fsx/fsy  速度前馈噪声门(0=视为静止)
                //   pndx/pndy 在途自身位移(Smith 补偿, 像素), 已从误差里扣掉
                //   ipx/dpx  积分项/微分项的像素当量
                //   ix/iy    积分状态(像素*秒)   carry    未发出的计数零头
                //   cmdx/y   取整前的浮点指令    lim      本拍生效的输出上限
                //   kx/ky    每计数多少像素(k̂)  vx/vy    目标自身速度(像素/秒)
                //   fits     观测器已接受的有效拟合窗口数(0 = 还没标定出来)
                //   bbh      本拍检测框高度(像素)。调参 agent 用它学【基准框高】
                //            (尺度调度的参照点), 见 mouse/aim_scale.h。
                //   ─ PID-EventSync (本档唯一链路) ──
                //   tid      跟踪器身份(与 gen 不同源, 跨帧粘滞)
                //   th/tm    锁定轨迹的连续命中数 / 连续漏帧数(tm>0 = 本拍在滑行)
                //   tvx/tvy  跟踪器采样窗给出的目标速度(像素/秒)
                //   tkx/tky  每轨预测系数的当前值 [0,1](排查"预测到底爬没爬上去")
                runtime::chainlog::write(
                    runtime::chainlog::SecPid, 2,
                    ",dt_ms=%.2f,sup=%d,switch=%d,jump_px=%.1f,"
                    "ex=%.2f,ey=%.2f,eux=%.2f,euy=%.2f,"
                    "ipx=%.2f,ipy=%.2f,dpx=%.2f,dpy=%.2f,ix=%.2f,iy=%.2f,"
                    "carryx=%.2f,carryy=%.2f,cmdx=%.2f,cmdy=%.2f,lim=%d/%d,bbh=%.1f,"
                    "tid=%d,th=%d,tm=%d,tvx=%.1f,tvy=%.1f,tkx=%.2f,tky=%.2f",
                    out.pid_dt_ms, out.motion_suppressed ? 1 : 0, out.target_switched ? 1 : 0,
                    out.target_anchor_jump_px,
                    out.pid_error_px_x, out.pid_error_px_y,
                    out.pid_used_error_px_x, out.pid_used_error_px_y,
                    out.pid_i_px_x, out.pid_i_px_y, out.pid_d_px_x, out.pid_d_px_y,
                    out.pid_integral_x, out.pid_integral_y,
                    out.pid_carry_x, out.pid_carry_y,
                    out.pid_cmd_x, out.pid_cmd_y,
                    out.pid_limit_x, out.pid_limit_y,
                    static_cast<double>(out.bbox.height),
                    out.esync_track_id, out.esync_track_hits,
                    out.esync_track_age, out.esync_vel_x, out.esync_vel_y,
                    out.esync_pred_k_x, out.esync_pred_k_y);

                // 调参 agent 的数据源 (2026-09-14)。
                // ★ 用【拉】而不是推: 这里只调用一次很轻的 diff, agent 没开时
                //   函数第一行就返回, 热路径上只多一次 atomic 读。
                //   真正做解析的是 agent 那一侧, 且只在开启时才解析。
                boss::autotune::Runtime::instance().feed_from_chainlog();

                // 选择层现场: 本帧所有"可瞄"候选的(类别/置信度) + 引擎最终选了哪一个。
                // 用来判断"瞄点在两个目标之间来回跳"(抽搐) 到底是选择层在换目标, 还是
                // 同一个目标的框自己在动。最多记 8 个候选, 超过只记总数。
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

            // 扳机模式: 0 = 长按(按住不松手), >0 = 连点(等duration 后松手)。
            const bool trigger_hold_mode = profile_ptr->trigger_fire_duration <= 0;

            if (out.coasting)
            {
                // 长按模式: 短暂滑行(检测丢一两帧, tracker 还在预测)不松手 ——
                // 松了再按就是"连点"那种顿挫。这里冻结扳机状态, 等目标观测
                // 回来再由 FSM 用准星是否还在命中区决定是否松手。目标真的
                // 丢了会走下面 !have_target 分支强制松开。
                if (!trigger_hold_mode)
                    reset_trigger(mouseThread, trigger, /*keep_scope_state=*/true);
            }

            // ─── 扳机 FSM (命中盒约束) ───
            // 长按模式 (trigger_fire_duration == 0): 进命中区按下, 一直按着。
            // 只有准星离开命中区才松手。连点模式(>0): 老行为是 等N ms 松手,
            // 冷却后再按。
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

                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                // ── 自动开镜 (仿 AimMagic 的「开火方式」) ────────────────────
                // ① 热键自己就绑了右键 -> 用户本来就在按右键(靠它开镜在瞄),
                //    再自动开镜只会把镜切回去, 所以直接不适用。
                // ② 开镜跟着 in_zone 走: 在命中区就开, 离开就收。这样连点模式在
                //    冷却期间镜不会反复开合。
                // ③ 开镜是【左键之前】的事: ready() 当闸门, 保证第一颗子弹是
                //    开着镜打出去的(AM 是同一拍先左键再右键, 那第一枪其实没开镜)。
                const bool auto_scope_allowed = std::none_of(
                    profile_ptr->keys.begin(), profile_ptr->keys.end(),
                    [](const std::string& key) { return key == "RightMouseButton"; });
                const int auto_scope_mode = std::clamp(profile_ptr->trigger_auto_scope, 0, 2);
                const int auto_scope_delay = std::max(0, profile_ptr->trigger_scope_delay_ms);
                {
                    const auto scope_action = trigger.scope.tick(
                        in_zone, auto_scope_allowed, auto_scope_mode,
                        auto_scope_delay, now_ms);
                    // ★ 顺序: 先抬后按。同一拍里既有抬又有按(模式被改、重新起一次)
                    // 时必须先 up 再 down, 否则会变成"按着不放"。
                    if (scope_action.release_right)
                        mouseThread.releaseRightButton();
                    if (scope_action.press_right)
                        mouseThread.pressRightButton();
                }
                const bool scope_ready = trigger.scope.ready(
                    auto_scope_allowed, auto_scope_mode, auto_scope_delay, now_ms);

                runtime::chainlog::write(
                    runtime::chainlog::SecTrigger, 2,
                    ",phase=%d,in_zone=%d,in_tol=%d,in_body=%d,tx=%.1f,ty=%.1f,"
                    "anchor_x=%.1f,anchor_y=%.1f,pivot_x=%.1f,pivot_y=%.1f,"
                    "scope=%d,scope_ok=%d",
                    static_cast<int>(trigger.phase), in_zone ? 1 : 0,
                    x_inside ? 1 : 0, y_inside ? 1 : 0,
                    half_x, half_y,
                    static_cast<double>(out.anchor.x),
                    static_cast<double>(out.anchor.y), judge_x, judge_y,
                    trigger.scope.engaged() ? 1 : 0, scope_ready ? 1 : 0);

                const auto begin_fire = [&] {
                    // 镜还没开好(或还没等够 scope_delay_ms)就不按左键 —— 保持
                    // 当前 phase, 下一拍再试。
                    if (!scope_ready)
                        return;
                    trigger.in_zone_since_ms = -1;
                    trigger.phase_time_ms = now_ms;
                    mouseThread.pressLeftButton();
                    trigger.phase = TriggerPhase::Pressed;
                    trigger.fired_this_tick = true;
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
                        // 零延迟直通模式：当delay 为0 时直接击发，实现机械级瞬发。
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
                        // 长按模式: 只要还在命中区就一直按着, 不做时长循环。
                        // 准星离开命中区才松手, 然后走一次冷却间隔 —— 避免在
                        // 判定边缘"踩空"了变成连点。
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
                        // 冷却结束瞬间若依然在受击框内，无缝立即衔接下一轮爆发连发。
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

                // ── 自动急停 (mouse/auto_stop.h) ────────────────────────────
                // 开火那一拍, 玩家按着 WASD 就往盒子补一个反方向键的短按:
                // 游戏里 W+S 同时存在 = 抵消 = 立刻停住, 这一枪才是站定打的。
                // ★ 只有 MAKCUNEW 有键盘通道(0x22 KEY_TAP); 其它输入方式直接跳过。
                //   不在每一拍都读键 —— 只在真的开火那一拍读, 省掉每秒上百次
                //   GetAsyncKeyState。
                if (trigger.fired_this_tick)
                {
                    const bool stop_enabled =
                        profile_ptr->trigger_auto_stop > 0
                        && config_snapshot->input_method == "MAKCUNEW";
                    if (stop_enabled)
                    {
                        boss::AutoStopController::Keys keys;
                        keys.forward = (GetAsyncKeyState('W') & 0x8000) != 0;
                        keys.back    = (GetAsyncKeyState('S') & 0x8000) != 0;
                        keys.left    = (GetAsyncKeyState('A') & 0x8000) != 0;
                        keys.right   = (GetAsyncKeyState('D') & 0x8000) != 0;

                        const auto stop_action = trigger.auto_stop.tick(
                            true, keys, true,
                            std::clamp(profile_ptr->trigger_stop_ms, 20, 300), now_ms);
                        if (stop_action.tap)
                        {
                            const bool sent = mouseThread.tapKey(
                                stop_action.hid_key,
                                std::clamp(profile_ptr->trigger_stop_ms, 20, 300));
                            runtime::chainlog::write(
                                runtime::chainlog::SecTrigger, 1,
                                ",auto_stop=%s,key=%s,ms=%d,sent=%d",
                                keys.any() ? "tap" : "none",
                                stop_action.name,
                                std::clamp(profile_ptr->trigger_stop_ms, 20, 300),
                                sent ? 1 : 0);
                        }
                    }
                    else if (profile_ptr->trigger_auto_stop > 0)
                    {
                        // 配了但当前输入方式不支持(老 MAKCU 没有键盘通道) —— 记一次,
                        // 免得用户以为它坏了。
                        runtime::chainlog::write(
                            runtime::chainlog::SecTrigger, 1,
                            ",auto_stop=unsupported,input=%s",
                            config_snapshot->input_method.c_str());
                    }
                    trigger.fired_this_tick = false;
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
            reset_trigger(mouseThread, trigger, /*keep_scope_state=*/true);
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
            // match (close-enough centre = same track).
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

    // 全链路日志: 一次会话结束时导出最近N 帧(文件名带时间戳), 落在 logs/ 里。
    // 环型缓冲里一直在跑, 所以这里导出的是本次会话最后几秒的完整现场。
    runtime::chainlog::end_frame();
    chain_log_auto_dump("session_end");
    mouseThread.clearQueuedMoves();
}