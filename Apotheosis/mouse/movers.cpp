#include "movers.h"

#include <algorithm>
#include <cmath>

namespace mover
{

// =============================================================================
// 疾风 PredictiveMover — 位置式 PID + 预测器。
//
// 关键设计:
//   - 位置式 dx = (Kp*fused + Kd*d_fused/dt_norm) * scale,err→0 时 dx 自然→0。
//   - 预测器只贡献 "fused = err + pred*weight",pred 跟随 |err| 线性缩放。
//   - 1 px 死区 hardcoded;锁住后 80ms smoothstep 渐入。
//   - 残差累加保证亚像素移动不丢失。
// =============================================================================

void PredictiveMover::reset()
{
    predictor_.reset();
    prev_err_x_ = prev_err_y_ = 0.0;
    last_track_id_ = -1;
    lock_age_sec_  = 0.0;
    rx_ = ry_ = 0.0;
}

void PredictiveMover::configure(const PredictiveParams& p)
{
    params_ = p;
}

Move PredictiveMover::step(double err_x, double err_y, double dt, int track_id)
{
    Move out;

    // 锁切换: 全部清零,prev_err 对齐当前 err 避免微分项瞬冲。
    if (track_id >= 0 && track_id != last_track_id_)
    {
        predictor_.reset();
        prev_err_x_ = err_x;
        prev_err_y_ = err_y;
        rx_ = ry_ = 0.0;
        lock_age_sec_ = 0.0;
        last_track_id_ = track_id;
    }

    if (!(dt > 1e-6)) dt = 1e-6;
    if (dt > 0.25)    dt = 0.25;
    lock_age_sec_ += dt;

    // 渐入: 锁住后 80ms 内 scale 从 0.4 smoothstep 到 1.0。
    constexpr double kInitScale = 0.4;
    constexpr double kRampSec   = 0.08;
    const double prog       = std::clamp(lock_age_sec_ / kRampSec, 0.0, 1.0);
    const double smoothstep = prog * prog * (3.0 - 2.0 * prog);
    const double scale      = kInitScale + (1.0 - kInitScale) * smoothstep;

    // 预测器 (只看 err 变化,不再喂 prev_move,避免单位错配)。
    auto [pred_x, pred_y] = predictor_.predict(err_x, err_y, dt);

    // 预测幅度跟随当前 |err| 缩放,上限 60。err 越小,预测能扰动越少。
    const double pred_lim_x = std::min(std::fabs(err_x), 60.0);
    const double pred_lim_y = std::min(std::fabs(err_y), 60.0);
    pred_x = std::clamp(pred_x, -pred_lim_x, pred_lim_x);
    pred_y = std::clamp(pred_y, -pred_lim_y, pred_lim_y);

    if (std::fabs(pred_x) > 100.0 || std::fabs(pred_y) > 100.0)
    {
        predictor_.reset();
        pred_x = pred_y = 0.0;
    }

    const double fused_x = err_x + pred_x * params_.pred_weight;
    const double fused_y = err_y + pred_y * params_.pred_weight;

    // 单像素死区: 误差<1 px 直接 0 输出,清残差,prev_err 对齐。
    constexpr double kDeadZonePx = 1.0;
    if (std::fabs(fused_x) < kDeadZonePx && std::fabs(fused_y) < kDeadZonePx)
    {
        rx_ = ry_ = 0.0;
        prev_err_x_ = err_x;
        prev_err_y_ = err_y;
        return out;
    }

    // 位置式 PID: dx = (Kp*fused + Kd*d_fused/dt_norm) * scale。
    // dt_norm 用 1/240 当 baseline,让 Kd 在 240 FPS 时刚好等效,
    // 帧率变化时按 dt 比例缩放,避免低帧率微分爆掉。
    constexpr double kDtNorm = 1.0 / 240.0;
    const double dt_eff = std::max(dt, kDtNorm);
    const double d_fused_x = (fused_x - prev_err_x_) / dt_eff;
    const double d_fused_y = (fused_y - prev_err_y_) / dt_eff;

    double u_x = (params_.kp_x * fused_x + params_.kd * d_fused_x * kDtNorm) * scale;
    double u_y = (params_.kp_y * fused_y + params_.kd * d_fused_y * kDtNorm) * scale;

    // 输出绝对值上限 100 px/帧,防一帧暴走。
    constexpr double kOutputMax = 100.0;
    u_x = std::clamp(u_x, -kOutputMax, kOutputMax);
    u_y = std::clamp(u_y, -kOutputMax, kOutputMax);

    // 亚像素残差累加,trunc 取整。
    const double raw_x = u_x + rx_;
    const double raw_y = u_y + ry_;
    out.dx = static_cast<int>(std::trunc(raw_x));
    out.dy = static_cast<int>(std::trunc(raw_y));
    rx_ = raw_x - out.dx;
    ry_ = raw_y - out.dy;

    prev_err_x_ = err_x;
    prev_err_y_ = err_y;

    return out;
}

} // namespace mover
