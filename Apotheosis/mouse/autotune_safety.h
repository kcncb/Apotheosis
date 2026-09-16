#pragma once

// =============================================================================
// 调参 agent —— 安全边界
// =============================================================================
//
// 为什么边界必须【独立于 LLM】写死在代码里 (2026-09-14)
//   把"改多少"的决定权完全交给模型是不负责任的: 模型会给出 10 倍的变化、会
//   给出 NaN、会在发散时还继续加 Kp。所以这一层的职责是: 【无论模型说什么,
//   落在控制器上的参数一定在安全域内】。
//
//   ★ 三条硬规则:
//     ① 单次改动幅度上限 —— 防止一拍之内把 Kp 从 35 跳到 200。
//        调参是迭代的, 每次 20% 以内, 十几次也能跨一个数量级。
//     ② 绝对值域夹取 —— 每个参数的物理允许范围(与 UI 的 spinbox 范围一致)。
//     ③ 发散/抖动时强制回滚 —— 不"问模型怎么办", 直接退回上一组已知可用的
//        参数并冻结一轮。自激是会越调越坏的, 不能靠模型自己发现。
//
//   ★ 还有一条不对称规则(见 §4.3): 在途补偿 beta 补过头会正反馈发散,
//     所以 beta 的【上调】比下调更保守。
// =============================================================================

#include "mouse/aim_pid.h"
#include "mouse/aim_scale.h"
#include "mouse/autotune_metrics.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace boss::autotune {

// agent 能改的全部参数。刻意做成一个独立结构体(不是直接引用 config),
// 这样"agent 改了什么"永远是显式的、可审计的、可回滚的。
struct Knobs
{
    // ── PID 三增益 ─────────────────────────────────────────────────────────
    double kp = 35.0;          // 计数/(像素·秒)
    double ki = 1.0;           // 1/秒
    double kd = 0.0;           // 秒
    // ── 在途补偿 ───────────────────────────────────────────────────────────
    double beta = 1.6;         // 无量纲
    // ── P 项饱和 ───────────────────────────────────────────────────────────
    double psat_px = 0.0;      // 0 = 关闭
    // ── 尺度调度 ───────────────────────────────────────────────────────────
    // ★ 2026-09-14: 不再是"近处框高/远处框高"两个绝对阈值。用户测不出当前框高,
    //   换游戏/分辨率也失效。改成单一基准 base_h, 公式
    //       s = clamp((h/base_h)^γ, scale_min, scale_max)
    //   ★ base_h 不在这里 —— 它是用户在界面上按按钮设定的观测值(见
    //     Runtime::set_baseline_from_recent), 不是模型能调的参数。
    //     让模型去猜"用户当时在多远"是荒谬的。
    bool   scale_on = false;
    double scale_max = 1.50;
    double scale_min = 1.00;
    // ── 预测 ───────────────────────────────────────────────────────────────
    double predict_x = 0.0, predict_y = 0.0;
    int    predict_max_px = 12;
    int    predict_vel_floor = 60;

    std::string describe() const;
};

// ── 本轮改动幅度 (2026-09-15 新增) ─────────────────────────────────────────
//
// ★ 为什么需要这个, 以及为什么它必须同时作用于【提示词】和【硬夹取】:
//
//   用户实测反馈: "试了两次显示已应用但是体感没什么变化"。
//   查下来的原因是改动太小 —— 三轮加起来 Kp 只动了 10%、Ki 一动没动,
//   这种幅度体感上不可能有变化。而根因是两处叠加:
//     · 提示词里写着"幅度要克制 —— 宁可小步走稳, 不要大改";
//     · 硬夹取每轮最多 ±20%。
//   更麻烦的是: 当当前参数【明显偏离】合理区间时(比如 Ki=5.0 而默认是 1.0),
//   每轮 20% 的步子永远走不到那里。
//
//   ★ 所以这个旋钮必须【两处一起改】: 只改提示词不改夹取, 模型要 ±50% 也会
//     被砍回 ±20%, 用户还是看不到效果 —— 那正是这次的病。
//
//   ★ beta 仍然额外收紧(它补过头会正反馈发散, 与 Kp/Ki 不同性质)。
enum class StepStyle : int
{
    Conservative = 0,   // 保守: 单轮最多 ±10%
    Steady       = 1,   // 平稳: 单轮最多 ±20%(默认, 与原行为一致)
    Aggressive   = 2,   // 激进: 单轮最多 ±50%
};

inline const char* step_style_name(StepStyle s)
{
    switch (s)
    {
    case StepStyle::Conservative: return "保守";
    case StepStyle::Aggressive:   return "激进";
    case StepStyle::Steady:
    default:                      return "平稳";
    }
}

// 单轮相对幅度上限
inline double step_style_rel(StepStyle s)
{
    switch (s)
    {
    case StepStyle::Conservative: return 0.10;
    case StepStyle::Aggressive:   return 0.50;
    case StepStyle::Steady:
    default:                      return 0.20;
    }
}

// beta 的【上调】上限 —— 三档都额外收紧。实测 >2.4 开始变差, >3 必发散,
// 所以补过头的代价比补不足大得多, 方向不对称。
inline double step_style_beta_up(StepStyle s)
{
    switch (s)
    {
    case StepStyle::Conservative: return 0.05;
    case StepStyle::Aggressive:   return 0.20;
    case StepStyle::Steady:
    default:                      return 0.10;
    }
}

// 一个参数的允许范围。min/max 与 UI spinbox 保持一致,
// 免得"agent 设了一个界面上显示不出来的值"这种诡异状态。
struct Range { double lo, hi; };

// 数值参数表 —— 加新参数只需在这里加一行, 夹取/描述/回滚全都自动跟上。
struct Limits
{
    // 硬边界(绝对值域)
    static Range kp()          { return {1.0, 300.0}; }     // UI 上限
    static Range ki()          { return {0.0, 20.0}; }
    static Range kd()          { return {0.0, 0.30}; }
    static Range beta()        { return {0.0, 3.0}; }       // 实测 >3 必发散
    static Range psat()        { return {0.0, 1000.0}; }
    static Range scale_max()   { return {1.0, 2.0}; }
    // ★ 远处下限放开到 < 1.0(2026-09-14): 远处目标的像素速度本来就小, 按框高
    //   等比缩放是正确的距离补偿。下界与 config 的夹取范围保持一致。
    static Range scale_min()   { return {0.30, 1.0}; }
    static Range predict()     { return {-0.2, 0.2}; }      // §4.2 约束
    static Range predict_cap() { return {0.0, 64.0}; }      // 绝对天花板
    static Range vel_floor()   { return {0.0, 1000.0}; }

    // 单次改动幅度(相对值上限)。beta 的【上调】额外收紧, 见 apply()。
    static constexpr double kMaxRelativeStep = 0.20;   // 单次最多 ±20%
    static constexpr double kMaxBetaUpStep   = 0.10;   // beta 上调最多 ±10%
};

inline double clamp_to(double v, Range r)
{
    if (!std::isfinite(v)) return r.lo;      // NaN/Inf 一律回落到下界
    return std::min(std::max(v, r.lo), r.hi);
}

// 单步夹取: 先限相对幅度, 再限绝对值域。
// prev 是"上一组已知可用的值" —— 相对幅度是相对它算的, 这样不管模型
// 从哪个起点出发, 都不会一拍跳太远。
inline double step_clamp(double prev, double want, Range r, double rel_step)
{
    if (!std::isfinite(want)) return prev;            // 模型给了垃圾值 -> 不动
    const double span = std::max(1e-9, std::abs(prev) * rel_step);
    double v = want;
    if (v > prev + span) v = prev + span;
    if (v < prev - span) v = prev - span;
    return clamp_to(v, r);
}

// ── 夹取结果 ────────────────────────────────────────────────────────────────
struct ClampReport
{
    Knobs applied;                  // 真正会生效的参数
    std::string notes;              // 哪些字段被夹了(给 UI/日志看)
    bool any_clamped = false;
};

// 把模型想要的参数夹到安全域内。prev = 当前正在用的(已知可用的)参数。
//
// ★ style = 用户选的"本轮改动幅度"。默认 Steady(±20%) = 原行为, 所以已有的
//   两参数调用点不用改。
inline ClampReport apply_limits(const Knobs& prev, const Knobs& want,
                                StepStyle style = StepStyle::Steady)
{
    ClampReport rep;
    Knobs& a = rep.applied;
    a = prev;

    const double rel = step_style_rel(style);
    const double betaUp = step_style_beta_up(style);

    auto note = [&](const char* name, double from, double to) {
        if (std::abs(from - to) < 1e-9) return;
        rep.any_clamped = true;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %.3f->%.3f; ", name, from, to);
        rep.notes += buf;
    };

    a.kp = step_clamp(prev.kp, want.kp, Limits::kp(), rel);
    note("kp", want.kp, a.kp);
    a.ki = step_clamp(prev.ki, want.ki, Limits::ki(), rel);
    note("ki", want.ki, a.ki);
    a.kd = step_clamp(prev.kd, want.kd, Limits::kd(), rel);
    note("kd", want.kd, a.kd);

    // ★ beta 不对称: 补过头会正反馈发散, 所以上调更保守。
    {
        const Range r = Limits::beta();
        const bool up = want.beta > prev.beta;
        a.beta = step_clamp(prev.beta, want.beta, r, up ? betaUp : rel);
        note("beta", want.beta, a.beta);
    }

    a.psat_px = step_clamp(prev.psat_px, want.psat_px, Limits::psat(), rel);
    note("psat", want.psat_px, a.psat_px);

    // 尺度: 开关不夹取(离散), 数值走夹取。
    // ★ 额外一条硬约束: Kp * scale_max 是等效增益, 有稳定性天花板(见 CLAUDE.md)。
    //   这里【不】替用户压 s_max, 而是在报告里标注出来, 让调用方决定 ——
    //   因为天花板依赖 k̂, 而 k̂ 恰恰是本项目测不准的量。
    a.scale_on = want.scale_on;
    a.scale_max = step_clamp(prev.scale_max, want.scale_max,
                             Limits::scale_max(), rel);
    note("s_max", want.scale_max, a.scale_max);
    // ★ s_min 也允许 < 1.0 了(远处真的降增益)。原实现把它钉死在 1.0, 是建立在
    //   "降增益必然让远处更跟不上"这个【错误前提】上的 —— 那个论证假设了像素
    //   速度固定, 而远处目标的像素速度本来就小。
    a.scale_min = step_clamp(prev.scale_min, want.scale_min,
                             Limits::scale_min(), rel);
    note("s_min", want.scale_min, a.scale_min);

    a.predict_x = step_clamp(prev.predict_x, want.predict_x, Limits::predict(),
                             rel);
    note("pred_x", want.predict_x, a.predict_x);
    a.predict_y = step_clamp(prev.predict_y, want.predict_y, Limits::predict(),
                             rel);
    note("pred_y", want.predict_y, a.predict_y);
    a.predict_max_px = static_cast<int>(std::lround(
        step_clamp(prev.predict_max_px, want.predict_max_px,
                   Limits::predict_cap(), rel)));
    note("pred_cap", want.predict_max_px, a.predict_max_px);
    a.predict_vel_floor = static_cast<int>(std::lround(
        step_clamp(prev.predict_vel_floor, want.predict_vel_floor,
                   Limits::vel_floor(), rel)));
    note("vel_floor", want.predict_vel_floor, a.predict_vel_floor);

    if (!rep.any_clamped) rep.notes = "(无夹取)";
    return rep;
}

// ── 该不该回滚 ──────────────────────────────────────────────────────────────
// 输入端只有"当前这一轮的指标" —— 决策完全在本函数内, 不问模型。
// 触发条件(任一):
//   ① 判为发散
//   ② 误差中位/p90 比上一轮【明显变差】(30% 以上)
//   ③ 翻转率过高(自持抖动)
// ★ 刻意要求"明显变差"才回滚: 实机数据噪声大, 一点点波动就回滚会让 agent
//   永远在原地打转。
struct RollbackDecision
{
    bool rollback = false;
    std::string reason;
};

inline RollbackDecision should_rollback(const Metrics& now, const Metrics& prev,
                                        bool have_prev)
{
    RollbackDecision d;
    if (now.divergent)
    {
        d.rollback = true;
        d.reason = "误差不收敛(判为发散)";
        return d;
    }
    // 自持抖动: 下发足够多且几乎每拍反号。
    if (now.nonzero_out >= 40 && now.flip_ratio >= 0.85)
    {
        d.rollback = true;
        d.reason = "指令符号每拍翻转(自持抖动)";
        return d;
    }
    if (have_prev && prev.n >= 16 && now.n >= 16)
    {
        if (now.err_p90 > prev.err_p90 * 1.30 && now.err_p90 > 1.0)
        {
            d.rollback = true;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "误差 p90 明显变差 %.2f->%.2f px", prev.err_p90, now.err_p90);
            d.reason = buf;
            return d;
        }
    }
    return d;
}

inline std::string Knobs::describe() const
{
    char buf[384];
    std::snprintf(buf, sizeof(buf),
        "Kp=%.1f Ki=%.2f Kd=%.4f beta=%.2f | psat=%.0fpx | "
        "尺度%s s_min=%.2f s_max=%.2f | 预测x=%.3f y=%.3f 上限=%dpx 门限=%dpx/s",
        kp, ki, kd, beta, psat_px,
        scale_on ? "开" : "关", scale_min, scale_max,
        predict_x, predict_y, predict_max_px, predict_vel_floor);
    return std::string(buf);
}

} // namespace boss::autotune
