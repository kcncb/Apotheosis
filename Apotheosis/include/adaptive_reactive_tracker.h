#pragma once

#include <algorithm>
#include <cmath>

struct ArtSettings
{
    double speed_x = 0.6;
    double speed_y = 0.6;
    double dead_zone_px = 2.0;
};

struct ArtResult
{
    int move_x = 0;
    int move_y = 0;

    double aim_x = 0, aim_y = 0;
    double vel_x = 0, vel_y = 0;
    double cutoff_hz = 0;
    double consistency = 0;
    bool   snapped = false;
};

class AdaptiveReactiveTracker
{
public:
    void configure(const ArtSettings& s)
    {
        cfg_ = s;
    }

    void reset()
    {
        init_ = false;
        tid_  = -1;
        age_  = 0;
        rx_   = 0;
        ry_   = 0;
    }

    ArtResult step(double pivot_x, double pivot_y,
                   double cross_x, double cross_y,
                   double dt,
                   int    bbox_w, int bbox_h,
                   int    target_id = -1)
    {
        dt = clamp(dt, 0.001, 0.1);

        const double mx = pivot_x;
        const double my = pivot_y;

        ArtResult out{};

        // target switch -> clean reset
        if (target_id >= 0 && target_id != tid_) {
            tid_  = target_id;
            init_ = false;
        }

        // snap detection (bbox-diagonal adaptive threshold)
        if (init_) {
            const double thresh = kSnapMult * ((sd_ > 1.0) ? sd_ : kRefDiagPx);
            double ix = mx - fx_, iy = my - fy_;
            if (ix * ix + iy * iy > thresh * thresh) {
                init_ = false;
                out.snapped = true;
            }
        }

        // first frame: seed state
        if (!init_) {
            fx_ = mx;  fy_ = my;
            dsx_ = 0;  dsy_ = 0;
            dfx_ = 0;  dfy_ = 0;
            pmx_ = mx; pmy_ = my;
            ssp_ = 0;  sd_  = 0;
            age_ = 0;
            rx_  = 0;  ry_  = 0;
            init_ = true;

            out.aim_x = fx_;
            out.aim_y = fy_;
            return drive(out.aim_x, out.aim_y, cross_x, cross_y, out);
        }

        ++age_;

        // raw velocity (px/s)
        const double rv_x = (mx - pmx_) / dt;
        const double rv_y = (my - pmy_) / dt;
        pmx_ = mx;
        pmy_ = my;

        // duplicate-frame guard: dt < 3ms -> skip velocity update
        const bool real_frame = (dt >= 0.003);

        if (real_frame) {
            const double as = ema_alpha(kFcSlow, dt);
            dsx_ += as * (rv_x - dsx_);
            dsy_ += as * (rv_y - dsy_);

            const double af = ema_alpha(kFcFast, dt);
            dfx_ += af * (rv_x - dfx_);
            dfy_ += af * (rv_y - dfy_);

            ssp_ += as * (std::sqrt(dfx_ * dfx_ + dfy_ * dfy_) - ssp_);
        }

        // bbox-adaptive base cutoff
        const double raw_diag =
            std::sqrt(static_cast<double>(bbox_w * bbox_w + bbox_h * bbox_h));

        if (sd_ < 1.0)
            sd_ = raw_diag;
        else
            sd_ += ema_alpha(kFcSlow, dt) * (raw_diag - sd_);

        // Cutoff scales INVERSELY with bbox size.
        // Detection-center jitter is roughly proportional to bbox size, so
        // a big target on screen produces large absolute-pixel noise; we
        // want a LOWER cutoff there to reject it.  A small target produces
        // tiny noise but real motion looks "slow" through a sluggish filter,
        // so we want a HIGHER cutoff to keep lock.
        // Lower bound dropped 0.40 → 0.30: 大 bbox 时再多压一点平滑,
        // 抵消"bbox 大 → anchor 抖动幅度大 → 低通过滤不掉"造成的抖动。
        double dr = (sd_ > 1.0) ? kRefDiagPx / sd_ : 1.0;
        dr = clamp(dr, 0.30, 3.5);
        const double fc_min = kFcBase * dr;

        const double speed_slow = std::sqrt(dsx_ * dsx_ + dsy_ * dsy_);
        // Beta term (velocity-driven cutoff bump) is muted on large bboxes —
        // their speed_slow contains more noise per px/s, so we don't want
        // motion noise to pump cutoff right back into the jitter band.
        const double fc = fc_min + kBeta * dr * speed_slow;

        // filter position
        const double ap = ema_alpha(fc, dt);
        fx_ += ap * (mx - fx_);
        fy_ += ap * (my - fy_);

        // direction consistency + prediction
        const double speed_fast = std::sqrt(dfx_ * dfx_ + dfy_ * dfy_);

        double cons = 0.5;
        if (speed_fast > kConsThreshPx && speed_slow > kConsThreshPx) {
            double dot = dfx_ * dsx_ + dfy_ * dsy_;
            cons = 0.5 + 0.5 * dot / (speed_fast * speed_slow);
            cons = clamp(cons, 0.0, 1.0);
        } else if (speed_fast < kConsThreshPx) {
            cons = 0.0;
        }

        // Prediction gate: use speed_slow (trend velocity, 1.5 Hz EMA)
        // instead of speed_fast.  speed_slow has near-zero response to
        // zero-mean detection noise (bbox ±2 px jitter cancels out in
        // the low-pass), but rises quickly for real directional movement.
        //
        // 阈值 bbox 自适应: 原来固定 120 px/s 同时坑大目标和小目标 —
        //   · 小目标真实平移速度也只有几十 px/s,永远过不了 120 → prediction 永远 0
        //     → 鼠标只剩低通后的 fx_,跟不上目标。
        //   · 大目标 dsx_/dsy_ 因为 anchor 抖动幅度大,假速度轻松过 120
        //     → 抖动直接进 prediction 项,aim_x 在锚点周围抖。
        // 改成 max(80, sd_ * 1.5) — 小 bbox 阈值落到 80 容易触发预测,
        // 大 bbox 阈值上抬到 300+ 把噪声挡在门外。
        const double adaptive_thresh = std::max(
            80.0, (sd_ > 1.0 ? sd_ : kRefDiagPx) * 1.5);
        const double speed_gate = clamp(speed_slow / adaptive_thresh, 0.0, 1.0);

        // Proximity gate: fade prediction to zero as the crosshair
        // nears the filtered target.  Prevents overshoot / orbiting
        // caused by noisy dfx_/dfy_ pushing aim past a close target.
        // prox_r is capped in ABSOLUTE pixels so a huge bbox doesn't
        // keep prediction at full strength while still tens of pixels
        // from the anchor (the main source of large-target orbit jitter).
        const double raw_ex = fx_ - cross_x;
        const double raw_ey = fy_ - cross_y;
        const double raw_err = std::sqrt(raw_ex * raw_ex + raw_ey * raw_ey);
        const double prox_r = clamp((sd_ > 1.0 ? sd_ : kRefDiagPx) * kProxFrac,
                                    3.0, kProxRadiusMaxPx);
        const double prox_gate = clamp(raw_err / prox_r, 0.0, 1.0);

        const double warmup = clamp(age_ / 4.0, 0.0, 1.0);
        const double pred_sec =
            0.5 / (2.0 * kPi * fc) * cons * warmup * speed_gate * prox_gate;

        // Use slow velocity (dsx_/dsy_) for the prediction vector.
        // dfx_/dfy_ (10 Hz) amplifies frame-to-frame bbox noise into
        // ±hundreds of px/s; dsx_/dsy_ (1.5 Hz) cancels zero-mean
        // noise and tracks only the real trend direction.
        const double aim_x = fx_ + dsx_ * pred_sec;
        const double aim_y = fy_ + dsy_ * pred_sec;

        out.aim_x      = aim_x;
        out.aim_y      = aim_y;
        out.vel_x      = dfx_;
        out.vel_y      = dfy_;
        out.cutoff_hz  = fc;
        out.consistency = cons;

        return drive(aim_x, aim_y, cross_x, cross_y, out);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    static constexpr double kFcBase = 1.0;
    static constexpr double kBeta = 0.022;
    static constexpr double kFcSlow = 1.5;
    static constexpr double kFcFast = 10.0;

    static constexpr double kRefDiagPx = 70.0;
    static constexpr double kSnapMult = 1.15;
    static constexpr double kConsThreshPx = 80.0;

    // Prediction gate threshold (px/s, measured on speed_slow).
    static constexpr double kPredSpeedThresh = 120.0;

    // Proximity gate: within kProxFrac of bbox diagonal the prediction
    // fades to zero, preventing aim-point orbiting on small targets.
    static constexpr double kProxFrac = 0.15;
    // Absolute pixel cap on the proximity-fade radius — without this a
    // 250 px-diagonal target keeps prediction at 100% until the crosshair
    // is within ~38 px of anchor, which is the dominant cause of
    // large-target orbit / overshoot jitter.
    // 16 → 12: 再压一点,大目标更早(从 12 px 起)淡化预测,与上面 dr 下限
    // 收紧 + 阈值上抬合力把大目标抖动消干净。
    static constexpr double kProxRadiusMaxPx = 12.0;

    static constexpr double kDeadZoneFrac  = 0.030;
    static constexpr double kSoftZoneMult = 3.0;

    static double ema_alpha(double fc, double dt)
    {
        const double r = 2.0 * kPi * fc * dt;
        return r / (r + 1.0);
    }

    static double clamp(double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    ArtResult& drive(double tx, double ty,
                     double cx, double cy,
                     ArtResult& out)
    {
        double ex = tx - cx;
        double ey = ty - cy;

        const double err = std::sqrt(ex * ex + ey * ey);
        const double dz = std::max(cfg_.dead_zone_px,
                                   sd_ > 1.0 ? sd_ * kDeadZoneFrac : cfg_.dead_zone_px);
        const double soft = dz * kSoftZoneMult;
        if (err < dz + soft) {
            double fade;
            if (err < dz) {
                fade = 0.0;
            } else {
                double t = (err - dz) / soft;
                fade = t * t;
            }
            ex *= fade;
            ey *= fade;
            rx_ *= 0.3 + 0.7 * fade;
            ry_ *= 0.3 + 0.7 * fade;
        }

        double raw_x = cfg_.speed_x * ex + rx_;
        double raw_y = cfg_.speed_y * ey + ry_;

        out.move_x = static_cast<int>(std::trunc(raw_x));
        out.move_y = static_cast<int>(std::trunc(raw_y));
        rx_ = raw_x - out.move_x;
        ry_ = raw_y - out.move_y;

        return out;
    }

    bool init_ = false;
    int  tid_  = -1;
    int  age_  = 0;

    double fx_ = 0, fy_ = 0;
    double dsx_ = 0, dsy_ = 0;
    double dfx_ = 0, dfy_ = 0;
    double pmx_ = 0, pmy_ = 0;
    double ssp_ = 0;
    double sd_ = 0;
    double rx_ = 0, ry_ = 0;

    ArtSettings cfg_;
};
