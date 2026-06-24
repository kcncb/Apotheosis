#include "movers.h"

#include <algorithm>
#include <cmath>

namespace mover
{

// =============================================================================
// 疾风 — 独立 bbox 自适应「带预测 PID」(实现见 movers.h 顶部说明)。
//
// 关键不变量:
//   · 从不调用 ART;只吃 Layer A 给的原始 anchor + bbox。
//   · 大框单帧 |u| ≤ |err|(overshoot=1.0)→ 结构性保证不冲过目标。
//   · 锁切 / teleport / 掉帧重捕 → 重播种,prev_err 对齐当前,vel 清零,
//     避免微分瞬冲与多帧位移除以单帧 dt 产生的速度尖刺。
// =============================================================================

void PredictiveMover::reset()
{
    last_track_id_ = -1;
    has_prev_      = false;
    prev_ax_ = prev_ay_ = 0.0;
    vel_x_   = vel_y_   = 0.0;
    prev_err_x_ = prev_err_y_ = 0.0;
    rx_ = ry_ = 0.0;
    lock_age_sec_ = 0.0;
}

void PredictiveMover::configure(const PredictiveParams& p)
{
    params_ = p;
}

Move PredictiveMover::step(double anchor_x, double anchor_y,
                           double cross_x,  double cross_y,
                           double bbox_w,   double bbox_h,
                           double image_size, double dt, int track_id)
{
    Move out;

    if (!(dt > 1e-6)) dt = 1e-6;
    if (dt > 0.25)    dt = 0.25;
    if (!(image_size > 1.0)) image_size = 1.0;

    const double diag = std::sqrt(bbox_w * bbox_w + bbox_h * bbox_h);

    // ── 锁切:清状态,本帧作为新序列重播种 ──
    bool reseed = false;
    if (track_id != last_track_id_)
    {
        last_track_id_ = track_id;
        has_prev_      = false;
        vel_x_ = vel_y_ = 0.0;
        rx_ = ry_ = 0.0;
        lock_age_sec_ = 0.0;
        reseed = true;
    }

    // ── teleport / 掉帧重捕:anchor 跳变过大 → 速度清零、本帧不预测 ──
    if (has_prev_ && !reseed)
    {
        const double jump = std::hypot(anchor_x - prev_ax_, anchor_y - prev_ay_);
        const double snapThresh =
            std::max(kSnapDiagMul * diag, kSnapImgFrac * image_size);
        if (jump > snapThresh)
        {
            vel_x_ = vel_y_ = 0.0;
            reseed = true;
        }
    }

    // ── 速度估计(目标位移 EMA);重播种帧只对齐 prev,不产生速度 ──
    if (!has_prev_ || reseed)
    {
        prev_ax_ = anchor_x;
        prev_ay_ = anchor_y;
        has_prev_ = true;
    }
    else
    {
        double rvx = (anchor_x - prev_ax_) / dt;
        double rvy = (anchor_y - prev_ay_) / dt;
        rvx = clampd(rvx, -kVelClampPx, kVelClampPx);
        rvy = clampd(rvy, -kVelClampPx, kVelClampPx);
        const double a = ema_alpha(kVelCutHz, dt);
        vel_x_ += a * (rvx - vel_x_);
        vel_y_ += a * (rvy - vel_y_);
        prev_ax_ = anchor_x;
        prev_ay_ = anchor_y;
    }

    lock_age_sec_ += dt;

    // ── bbox 尺寸因子 t_small(1 = 远小框,0 = 近大框)──
    const double sz = diag / image_size;
    const double t_small =
        clampd((kSizeFracHi - sz) / (kSizeFracHi - kSizeFracLo), 0.0, 1.0);

    // ── 自适应系数 ──
    const double kpx       = params_.kp_x * lerp(kGainBig, kGainSmall, t_small);
    const double kpy       = params_.kp_y * lerp(kGainBig, kGainSmall, t_small);
    const double kd        = params_.kd   * lerp(kDampBig, kDampSmall, t_small);
    const double lead      = params_.pred_weight * kLeadBaseSec
                             * lerp(kLeadBig, kLeadSmall, t_small);
    const double overshoot = lerp(kOvershootBig, kOvershootSmall, t_small);
    const double dzFrac    = lerp(kDzFracBig, kDzFracSmall, t_small);

    // ── 预测领先点 → led-err ──
    const double led_x = anchor_x + vel_x_ * lead;
    const double led_y = anchor_y + vel_y_ * lead;
    const double err_x = led_x - cross_x;
    const double err_y = led_y - cross_y;

    // 重播种帧 prev_err 对齐当前,微分项不瞬冲。
    if (reseed)
    {
        prev_err_x_ = err_x;
        prev_err_y_ = err_y;
    }

    // ── 死区(基于领先误差幅值):settled 时停手、清残差 ──
    const double errMag = std::hypot(err_x, err_y);
    const double dz = std::max(kDzMinPx, diag * dzFrac);
    if (errMag < dz)
    {
        rx_ = ry_ = 0.0;
        prev_err_x_ = err_x;
        prev_err_y_ = err_y;
        return out;  // {0, 0}
    }

    // ── 位置式 P + D(微分作用于 led-err)──
    const double dt_eff = std::max(dt, kDtNorm);
    const double dErrX  = (err_x - prev_err_x_) / dt_eff;
    const double dErrY  = (err_y - prev_err_y_) / dt_eff;

    double ux = kpx * err_x + kd * dErrX * kDtNorm;
    double uy = kpy * err_y + kd * dErrY * kDtNorm;

    // ── 不过冲硬钳位:单帧 |u| ≤ |err|·overshoot(大框=1.0 → 绝不冲过目标)──
    ux = clampd(ux, -std::fabs(err_x) * overshoot, std::fabs(err_x) * overshoot);
    uy = clampd(uy, -std::fabs(err_y) * overshoot, std::fabs(err_y) * overshoot);

    // ── 渐入:锁后 kRampSec 内 scale 从 kInitScale smoothstep 到 1.0 ──
    const double prog   = clampd(lock_age_sec_ / kRampSec, 0.0, 1.0);
    const double smooth = prog * prog * (3.0 - 2.0 * prog);
    const double scale  = kInitScale + (1.0 - kInitScale) * smooth;
    ux *= scale;
    uy *= scale;

    // ── 输出上限,防单帧暴走 ──
    ux = clampd(ux, -kOutMaxPx, kOutMaxPx);
    uy = clampd(uy, -kOutMaxPx, kOutMaxPx);

    // ── 亚像素残差累加,trunc 取整 ──
    const double rawx = ux + rx_;
    const double rawy = uy + ry_;
    out.dx = static_cast<int>(std::trunc(rawx));
    out.dy = static_cast<int>(std::trunc(rawy));
    rx_ = rawx - out.dx;
    ry_ = rawy - out.dy;

    prev_err_x_ = err_x;
    prev_err_y_ = err_y;
    return out;
}

} // namespace mover
