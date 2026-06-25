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
//   · 反馈项单帧 |fb| ≤ |err|·overshoot(大框=1.0)→ 反馈绝不冲过目标;
//     前馈项 ff=pred·vel·dt 另算(匹配目标速度,稳态不构成过冲)。
//   · 速度估计在世界系(屏幕系 Δanchor + 上帧自身平移),否则跟上后估速塌成 0。
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
    fa_x_ = fa_y_ = 0.0;
    has_anchor_filt_ = false;
    prev_raw_x_ = prev_raw_y_ = 0.0;
    for (int i = 0; i < kHistN; ++i) { out_hist_x_[i] = 0.0; out_hist_y_[i] = 0.0; }
    hist_w_ = 0;
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
        last_track_id_   = track_id;
        has_prev_        = false;
        has_anchor_filt_ = false;
        vel_x_ = vel_y_  = 0.0;
        rx_ = ry_        = 0.0;
        for (int i = 0; i < kHistN; ++i) { out_hist_x_[i] = 0.0; out_hist_y_[i] = 0.0; }
        hist_w_          = 0;
        lock_age_sec_    = 0.0;
        reseed = true;
    }

    // ── teleport / 掉帧重捕:用【原始】anchor 判跳变(滤波会糊掉真实瞬移)──
    if (has_anchor_filt_ && !reseed)
    {
        const double jump = std::hypot(anchor_x - prev_raw_x_, anchor_y - prev_raw_y_);
        const double snapThresh =
            std::max(kSnapDiagMul * diag, kSnapImgFrac * image_size);
        if (jump > snapThresh)
        {
            vel_x_ = vel_y_ = 0.0;
            for (int i = 0; i < kHistN; ++i) { out_hist_x_[i] = 0.0; out_hist_y_[i] = 0.0; }
            hist_w_ = 0;
            reseed = true;
        }
    }
    prev_raw_x_ = anchor_x;
    prev_raw_y_ = anchor_y;

    // ── one-euro anchor 滤波:静止重平滑、运动低延迟(疾风原本零滤波)──
    if (!has_anchor_filt_ || reseed)
    {
        fa_x_ = anchor_x;
        fa_y_ = anchor_y;
        has_anchor_filt_ = true;
    }
    else
    {
        const double spd = std::hypot(anchor_x - fa_x_, anchor_y - fa_y_) / dt;
        const double fc  = kEuroFcMin + kEuroBeta * spd;
        const double a   = ema_alpha(fc, dt);
        fa_x_ += a * (anchor_x - fa_x_);
        fa_y_ += a * (anchor_y - fa_y_);
    }
    anchor_x = fa_x_;
    anchor_y = fa_y_;

    // ── 速度估计(世界系)= 屏幕系 Δanchor + 「已生效」的自身平移 ──
    // 已生效平移 = 自身输出延迟 kAimLatencySec 后的值,从输出历史环取(用【已生效】
    // 而非【上一帧】输出,才能切断 输出→估速→前馈→输出 的正反馈)。重播种帧只对齐
    // prev,不产生速度。
    if (!has_prev_ || reseed)
    {
        prev_ax_ = anchor_x;
        prev_ay_ = anchor_y;
        has_prev_ = true;
    }
    else
    {
        int lat = static_cast<int>(std::lround(kAimLatencySec / dt));
        if (lat < 0)         lat = 0;
        if (lat > kHistN - 1) lat = kHistN - 1;
        // hist_w_ 指向下一个写入位;最近一帧输出在 (hist_w_-1)。lat 帧前 = -1-lat。
        const int idx = ((hist_w_ - 1 - lat) % kHistN + kHistN) % kHistN;
        const double applied_x = out_hist_x_[idx];
        const double applied_y = out_hist_y_[idx];

        double rvx = (anchor_x - prev_ax_) / dt + applied_x / dt;
        double rvy = (anchor_y - prev_ay_) / dt + applied_y / dt;
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
    const double overshoot = lerp(kOvershootBig, kOvershootSmall, t_small);
    const double dzFrac    = lerp(kDzFracBig, kDzFracSmall, t_small);
    const double ffScale   = lerp(kFFBig, kFFSmall, t_small);  // 大框关前馈 → 保不过冲

    // ── 位置误差(无 setpoint 领先;目标速度由前馈处理)──
    const double err_x = anchor_x - cross_x;
    const double err_y = anchor_y - cross_y;

    // 重播种帧 prev_err 对齐当前,微分项不瞬冲。
    if (reseed)
    {
        prev_err_x_ = err_x;
        prev_err_y_ = err_y;
    }

    // ── 死区(基于误差幅值):settled 时只关反馈,前馈仍可带动(目标在动时)──
    const double errMag = std::hypot(err_x, err_y);
    const double dz = std::max(kDzMinPx, diag * dzFrac);
    const bool in_dead = (errMag < dz);

    // ── 反馈 P + D:钳到 |err|·overshoot(大框=1.0 → 绝不冲过目标)──
    double fb_x = 0.0, fb_y = 0.0;
    if (!in_dead)
    {
        const double dt_eff = std::max(dt, kDtNorm);
        const double dErrX  = (err_x - prev_err_x_) / dt_eff;
        const double dErrY  = (err_y - prev_err_y_) / dt_eff;
        fb_x = kpx * err_x + kd * dErrX * kDtNorm;
        fb_y = kpy * err_y + kd * dErrY * kDtNorm;
        fb_x = clampd(fb_x, -std::fabs(err_x) * overshoot, std::fabs(err_x) * overshoot);
        fb_y = clampd(fb_y, -std::fabs(err_y) * overshoot, std::fabs(err_y) * overshoot);
    }

    // ── 速度前馈:跟着目标【世界系】速度走,与 err 无关(pred_weight = 增益;
    //    大框 ffScale=0 → 不前馈,保留近框不过冲)──
    const double ff_x = params_.pred_weight * ffScale * vel_x_ * dt;
    const double ff_y = params_.pred_weight * ffScale * vel_y_ * dt;

    double ux = fb_x + ff_x;
    double uy = fb_y + ff_y;

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

    // 输出入历史环(供后续帧按假定延迟还原已生效平移)。
    out_hist_x_[hist_w_] = static_cast<double>(out.dx);
    out_hist_y_[hist_w_] = static_cast<double>(out.dy);
    hist_w_ = (hist_w_ + 1) % kHistN;
    return out;
}

} // namespace mover
