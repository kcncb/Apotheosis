#include "alpha_beta_filter.h"

#include <algorithm>
#include <cmath>

namespace control {

namespace {

// 把秒换算成"拍数"后夹到一个安全区间。
// ★ 目的：dt 异常（首帧、卡顿、时钟跳变）时不让 α/β 冲出 [0,1]。
//   α > 1 会让滤波器过冲（比观测还激进），β > 1 会让速度估计发散。
double clampDtTicks(double tauSeconds, double dtSeconds, double& ticksOut)
{
    if (tauSeconds <= 0.0 || dtSeconds <= 0.0)
    {
        ticksOut = 1.0;
        return 1.0;
    }
    ticksOut = tauSeconds / dtSeconds;
    return ticksOut;
}

} // namespace

AlphaBetaFilter::AlphaBetaFilter(const AlphaBetaParams& params)
    : params_(params)
{
}

double AlphaBetaFilter::alphaForTau(double tauSeconds, double dtSeconds)
{
    // α-β 的标准形式（"fading memory" / 指数平滑等价）：
    //   α = 1 − exp(−dt/τ)
    // 它的性质：dt ≪ τ 时 α ≈ dt/τ（平滑强）；dt ≫ τ 时 α → 1（等于直通观测）。
    // ★ 用 exp 而不是 dt/τ 的线性近似 —— 后者在 dt 接近 τ 时会给出 >1 的值。
    if (tauSeconds <= 0.0)
        return 1.0;
    if (dtSeconds <= 0.0)
        return 0.0;
    const double a = 1.0 - std::exp(-dtSeconds / tauSeconds);
    return std::clamp(a, 0.0, 1.0);
}

double AlphaBetaFilter::betaForTau(double tauSeconds, double dtSeconds)
{
    // β 必须与 α 相容：稳态下 α-β 要对匀速目标无偏（新息趋 0）。
    // 标准配比 β = α²/(2−α)，它在小 α 时 ≈ α²/2，是"临界阻尼"的选择。
    // ★ 这样就【不需要】第二个旋钮 —— β 由 α 决定，符合"只留一个可调点"。
    const double a = alphaForTau(tauSeconds, dtSeconds);
    const double denom = 2.0 - a;
    if (denom <= 0.0)
        return 1.0;
    return std::clamp((a * a) / denom, 0.0, 1.0);
}

void AlphaBetaFilter::observe(const Vec2& center, double dtSeconds)
{
    // 首帧或刚复位：直接采纳观测，速度置 0。
    // ★ 速度置 0 是刻意的 —— 用一个"猜的"初始速度会让前几拍被错误外推，
    //   而 reset 的语义就是"对旧目标的运动一无所知"。
    if (!initialized_)
    {
        estimate_ = center;
        velocity_ = Vec2{ 0.0, 0.0 };
        initialized_ = true;
        return;
    }

    // dt 异常时按"只平滑不预测"处理：不做速度外推，避免放大。
    double ticks = 0.0;
    const double tauSeconds = params_.tauMs * 0.001;
    if (dtSeconds <= 0.0)
    {
        // 同一时刻重复观测：只做一次位置修正，不预测。
        const double a = 0.5;
        estimate_ = estimate_ + (center - estimate_) * a;
        return;
    }
    clampDtTicks(tauSeconds, dtSeconds, ticks);

    // ── α-β 更新 ────────────────────────────────────────────────────────
    // 1) 先按上一步的速度外推（预测）
    const Vec2 predicted = estimate_ + velocity_ * dtSeconds;

    // 2) 用观测修正预测（更新）
    const double a = alphaForTau(tauSeconds, dtSeconds);
    const double b = betaForTau(tauSeconds, dtSeconds);
    const Vec2 residual = center - predicted;

    estimate_ = predicted + residual * a;
    velocity_ = velocity_ + residual * (b / dtSeconds);
}

Vec2 AlphaBetaFilter::position() const
{
    return estimate_;
}

void AlphaBetaFilter::reset()
{
    initialized_ = false;
    estimate_ = Vec2{ 0.0, 0.0 };
    velocity_ = Vec2{ 0.0, 0.0 };
}

} // namespace control
