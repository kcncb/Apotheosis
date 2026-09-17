#pragma once

// ⑤⑥ PID 控制 + 出口量化
//
// ★★★ 公式（docs/generic-controller-layer.md §4.3.2）：
//
//   ── 每个方向 d ∈ {x, y} 各算一遍 ──
//
//   e_d    = anchor_d − cross_d                        (px, double)
//   P_d    = Kp_d · e_d
//   I_d   += Ki_d · e_d · dt   → clamp(±I_max_d)
//            if (e_d · e_prev_d < 0)  I_d *= exp(−dt / τ_unwind)   ★ 按分量判反向
//   D_d    = Kd_d · LP(de_d) / dt                      ★ D 项单独轻低通
//   u_d    = dt · Kp_d · ( e_d + I_d + Kd_d·de_d/dt )
//   counts_d = lround(u_d + carry_d)                   唯一一次量化
//   carry_d  = (u_d + carry_d) − counts_d              余量【分方向】结转
//
// ★★ 为什么是 FK 结构（三项同乘 Kp）：改 Kp 时三项【同比例缩放】，
//    调参时不会互相打架 —— 这是 §3.2.2"参数耦合，两个一起调必乱"的直接应对。
//
// ★★ 纯反馈，无前馈、无预测、无动力学模型（§4.1）。不要"顺手"加外推。

#include "types.h"

namespace control {

// ── 六个增益 + 出口参数 ─────────────────────────────────────────────────
// ★ 默认值全部等价于【历史行为】（Kp=35 / 其余 0 / 死区 0），
//   这样"从单套改成六套"这个动作本身不改变任何行为，是安全的起点。
struct PidConfig
{
    // 追踪增益（计数/(像素·秒)）
    double kpX = 35.0;
    double kpY = 35.0;
    // 积分增益（1/秒）。0 = 关闭积分。
    double kiX = 0.0;
    double kiY = 0.0;
    // 微分增益（秒）。0 = 关闭微分。
    double kdX = 0.0;
    double kdY = 0.0;

    // 积分回吐时间常数（秒）。
    // ★★ 历史值 0.2s 在 D2/AD 急停场景下【太慢】：AD 急停 2-3Hz ⇒ 变向间隔
    //    330-500ms，而回吐要 200ms ⇒ 积分几乎永远处在"没吐干净"的状态。
    //    收紧到 30ms 起步。⚠️ 这个值【没有实测依据】（方案 §7 第 7 条）。
    double tauUnwindSec = 0.030;

    // D 项低通时间常数（秒）。★ 无实测依据（方案 §7 第 8 条）。
    //   零惯性下目标速度是阶跃，阶跃的微分是冲激 ⇒ 急停那帧 de 有巨大尖峰，
    //   Kd 会把准星朝【目标原来的方向】猛推 —— 这正是 AD 急停超调的成因。
    //   <= 0 表示不做低通（直通）。
    double tauDerivSec = 0.020;

    // 积分上限（像素·秒 的等效量）。
    // ★ 用 clamp（非反算式）：反算式需要整定 Tt，而 Tt 没有实测依据。
    //   <= 0 表示用 maxOutputCounts 作为上限。
    double iMax = 0.0;

    // ★★ 这里【故意没有】移动死区参数。
    //    死区已被实测证伪（5px ⇒ 10.2 次/秒的"动/不动"翻转抖动），
    //    而它想解决的问题（末段不冲过头）已由 pFullScalePx 连续饱和负责。
    //    ★ 保留一个"能调但调了就坏"的旋钮比没有更糟 —— 别加回来。
    //    依据见 docs/generic-controller-layer.md §5.1。

    // 移动限幅（计数/拍）。0 = 内置 200。
    int maxOutputCounts = 200;

    // P 项连续饱和（检测像素）。误差超过这个值时 P 项不再增大。
    // ★ 与死区的本质区别：连续且经过原点，所以不存在"停了"这个状态。
    //   <= 0 表示不饱和。
    double pFullScalePx = 0.0;
};

// ── 单方向的状态 ────────────────────────────────────────────────────────
struct AxisState
{
    double integral = 0.0;      // 积分累加器（内部已乘过 Ki）
    double carry = 0.0;         // 量化余量 ★ 必须分方向，否则跨轴串扰
    double prevError = 0.0;
    double derivLp = 0.0;       // D 项低通的内部状态
    bool hasPrev = false;

    void reset()
    {
        integral = 0.0;
        carry = 0.0;
        prevError = 0.0;
        derivLp = 0.0;
        hasPrev = false;
    }
};

// ── 调试读数（调参时用；不参与控制） ────────────────────────────────────
struct AxisTelemetry
{
    double error = 0.0;
    double p = 0.0;             // 比例项贡献（未乘 dt·Kp）
    double i = 0.0;             // 积分项贡献（未乘 dt·Kp）
    double d = 0.0;             // 微分项贡献（未乘 dt·Kp）
    double u = 0.0;             // 未量化的输出（计数）
    int counts = 0;             // 量化后的输出
    double carry = 0.0;
    // 稳定线读数：Kp·dt·k̂ / g_crit。> 1.0 表示越过稳定线。
    // ★ k̂ 用内部常量（方案 §5.2：不做成配置项）。
    double stabilityRatio = 0.0;
};

struct ControlTelemetry
{
    AxisTelemetry x;
    AxisTelemetry y;
    // 是否发生了积分回吐（本拍）。
    bool unwoundX = false;
    bool unwoundY = false;
};

// ── 控制器 ──────────────────────────────────────────────────────────────
class PidController
{
public:
    PidController() = default;
    explicit PidController(const PidConfig& cfg) : cfg_(cfg) {}

    void setConfig(const PidConfig& cfg) { cfg_ = cfg; }
    const PidConfig& config() const { return cfg_; }

    // 一拍控制。
    //   anchor : 瞄点（检测像素）
    //   cross  : 准星（检测像素）
    //   dtSec  : 与上一拍的间隔（秒）
    // ★ dtSec <= 0 时本拍不输出（返回 {0,0} 且不改状态）—— 避免除零与假速度。
    Counts update(const Vec2& anchor, const Vec2& cross, double dtSec);

    // ★ 换目标 / 突变时必须调用（消除积分残留与微分历史）。
    //   不调用的话，新目标的控制会带着旧目标攒下的积分 —— 表现为"换目标时冲一下"。
    void reset();

    const ControlTelemetry& telemetry() const { return telemetry_; }

private:
    double stepAxis(Axis axis, double error, double dtSec,
                    AxisState& st, AxisTelemetry& tm, bool& unwound);

    PidConfig cfg_;
    AxisState stateX_;
    AxisState stateY_;
    ControlTelemetry telemetry_;
};

// ★ 稳定线常数（docs/generic-controller-layer.md §4.2）：
//   d = 46ms（实测死区，不是估算）；120fps ⇒ d = 5.5 拍
//   g_crit = 2·sin(π/(2(2d+1))) = 0.2602
//   k̂ ≈ 0.593（本机实测）
// ★★ 不许用"软件 E2E ≈ 11ms"去算 —— 它漏掉了 HID 固件、游戏帧更新、
//    显示输出、采集卡缓冲，会在速度估计里混进假速度。
inline constexpr double kLoopDeadTimeMs = 46.0;
inline constexpr double kCriticalGain = 0.2602;
inline constexpr double kCountsPerPixel = 0.593;

// 计算稳定线读数：Kp · dt · k̂ / g_crit。> 1.0 = 越过稳定线。
double stabilityRatio(double kp, double dtSec);

} // namespace control
