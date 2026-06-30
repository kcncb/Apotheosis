// =============================================================================
// 天枢 (ClassicPidMover) — 从 zimumodule 原样移植的经典 PID + 动态 KP + 预测。
// 算法逻辑、参数语义、默认值均与 zimumodule.cpp 保持一致。
// =============================================================================

#include "movers.h"
#include <cmath>
#include <algorithm>

namespace mover
{

void ClassicPidMover::reset()
{
    integral_x_ = integral_y_ = 0;
    prev_err_x_ = prev_err_y_ = 0;
    velocity_ema_ = 0;
    first_frame_ = true;

    ema_vx_ = ema_vy_ = 0;
    prev_target_x_ = prev_target_y_ = 0;
    ema_init_ = false;

    kf_x_ = kf_y_ = 0;
    kf_vx_ = kf_vy_ = 0;
    kf_px_ = kf_py_ = 1;
    kf_pvx_ = kf_pvy_ = 1;
    kf_init_ = false;
    accum_x_ = accum_y_ = 0;

    elapsed_sec_ = 0;
    aim_timing_ = false;
    prev_track_id_ = -1;
}

void ClassicPidMover::configure(const ClassicPidParams& p)
{
    params_ = p;
}

// ── 动态 KP: 按距离衰减 (pow 曲线) ──
static double CalcDistanceKP(double dist, double kp_min, double kp_max,
                             double factor, double img_w, double img_h)
{
    double diag = std::sqrt(img_w * img_w + img_h * img_h);
    double norm = dist / diag;
    if (norm > 1.0) norm = 1.0;
    double kp = std::pow(1.0 - norm, factor) * (kp_max - kp_min) + kp_min;
    return std::clamp(kp, kp_min, kp_max);
}

// ── 动态 KP: 按时间线性渐变 ──
static double CalcTimeKP(double kp_min, double kp_max, int duration_ms,
                         double elapsed_ms)
{
    if (duration_ms <= 0) return kp_max;
    double t = elapsed_ms / static_cast<double>(duration_ms);
    if (t > 1.0) t = 1.0;
    return t * (kp_max - kp_min) + kp_min;
}

Move ClassicPidMover::step(
    double anchor_x, double anchor_y,
    double cross_x,  double cross_y,
    double bbox_w,   double bbox_h,
    double image_size, double dt, int track_id)
{
    Move out{0, 0};

    // 锁切 / 首次锁定 → 重置所有状态
    if (track_id != prev_track_id_)
    {
        integral_x_ = integral_y_ = 0;
        prev_err_x_ = prev_err_y_ = 0;
        velocity_ema_ = 0;
        first_frame_ = true;

        ema_vx_ = ema_vy_ = 0;
        prev_target_x_ = prev_target_y_ = 0;
        ema_init_ = false;

        kf_x_ = kf_y_ = 0;
        kf_vx_ = kf_vy_ = 0;
        kf_px_ = kf_py_ = 1;
        kf_pvx_ = kf_pvy_ = 1;
        kf_init_ = false;
        accum_x_ = accum_y_ = 0;

        elapsed_sec_ = 0;
        aim_timing_ = false;
        prev_track_id_ = track_id;
    }

    // 瞄准计时
    if (!aim_timing_)
    {
        elapsed_sec_ = 0;
        aim_timing_ = true;
    }
    else
    {
        elapsed_sec_ += dt;
    }
    const double elapsed_ms = elapsed_sec_ * 1000.0;

    double aim_x = anchor_x;
    double aim_y = anchor_y;

    // image_size 既做宽也做高(正方形检测图)
    const double img_w = image_size;
    const double img_h = image_size;

    // ── 预测 ──
    int pred_mode = params_.prediction_mode;
    if (pred_mode == 0 && params_.kalman_q_pos != 1.0)
    {
        // 兼容: kalman 参数被改过但 prediction_mode 为 0 → 不强制开启
    }

    if (pred_mode == 1)
    {
        // EMA 速度预测
        if (!ema_init_)
        {
            ema_vx_ = 0;
            ema_vy_ = 0;
            ema_init_ = true;
        }
        else
        {
            ema_vx_ = (aim_x - prev_target_x_) * 0.3 + ema_vx_ * 0.7;
            ema_vy_ = (aim_y - prev_target_y_) * 0.3 + ema_vy_ * 0.7;
        }
        prev_target_x_ = aim_x;
        prev_target_y_ = aim_y;

        aim_x += ema_vx_ * params_.velocity_lead_frames + 0.5;
        if (!params_.independent_y)
            aim_y += ema_vy_ * params_.velocity_lead_frames + 0.5;
    }
    else if (pred_mode == 2)
    {
        // Kalman 滤波预测
        double q_pos = params_.kalman_q_pos;
        double q_vel = params_.kalman_q_vel;
        double r_obs = params_.kalman_r_obs;
        double lookahead = params_.kalman_lookahead;

        if (!kf_init_)
        {
            kf_x_ = aim_x;
            kf_y_ = aim_y;
            kf_vx_ = 0; kf_vy_ = 0;
            kf_px_ = 1; kf_py_ = 1;
            kf_pvx_ = 1; kf_pvy_ = 1;
            kf_init_ = true;
            accum_x_ = 0; accum_y_ = 0;
        }
        else
        {
            double kdt = dt;
            if (kdt <= 0 || kdt > 1.0) kdt = 0.016;

            // Predict
            double pred_x  = kf_x_ + kf_vx_ * kdt;
            double pred_y  = kf_y_ + kf_vy_ * kdt;
            double pred_px  = kf_px_ + q_pos;
            double pred_py  = kf_py_ + q_pos;
            double pred_pvx = kf_pvx_ + q_vel;
            double pred_pvy = kf_pvy_ + q_vel;

            // Observation (raw target + accumulated offset)
            double obs_x = aim_x + accum_x_;
            double obs_y = aim_y + accum_y_;

            // Update X
            double kx = pred_px / (pred_px + r_obs);
            kf_x_ = pred_x + kx * (obs_x - pred_x);
            kf_px_ = (1.0 - kx) * pred_px;
            double kvx = pred_pvx / (pred_pvx + r_obs);
            kf_vx_ = kf_vx_ + kvx * (obs_x - pred_x) / kdt;
            kf_pvx_ = (1.0 - kvx) * pred_pvx;

            // Update Y
            double ky = pred_py / (pred_py + r_obs);
            kf_y_ = pred_y + ky * (obs_y - pred_y);
            kf_py_ = (1.0 - ky) * pred_py;
            double kvy = pred_pvy / (pred_pvy + r_obs);
            kf_vy_ = kf_vy_ + kvy * (obs_y - pred_y) / kdt;
            kf_pvy_ = (1.0 - kvy) * pred_pvy;

            // Lookahead
            double la = lookahead / 1000.0;
            aim_x = kf_x_ + kf_vx_ * la - accum_x_;
            aim_y = kf_y_ + kf_vy_ * la - accum_y_;
        }
    }

    // ── 计算 KP / KI / KD ──
    double kp_x, kp_y;
    double ki_x, ki_y, kd_x, kd_y;
    double imax_x = 0, imax_y = 0;

    double dist = std::sqrt((aim_x - cross_x) * (aim_x - cross_x) +
                            (aim_y - cross_y) * (aim_y - cross_y));

    if (params_.aim_mode == 0)
    {
        // 简单模式: 对称 KP
        double kp;
        if (params_.simple_transition_ms > 0 && aim_timing_)
        {
            kp = CalcTimeKP(params_.simple_start_speed, params_.simple_end_speed,
                            params_.simple_transition_ms, elapsed_ms);
        }
        else
        {
            kp = params_.simple_end_speed;
        }
        kp_x = kp_y = kp;
        ki_x = ki_y = params_.simple_ki;
        kd_x = kd_y = params_.simple_kd;
    }
    else
    {
        // 高级模式: 独立 XY
        if (params_.adv_time_dynamic_x && params_.adv_time_x > 0 && aim_timing_)
        {
            kp_x = CalcTimeKP(params_.adv_kpmin_x, params_.adv_kpmax_x,
                              params_.adv_time_x, elapsed_ms);
        }
        else
        {
            kp_x = CalcDistanceKP(dist, params_.adv_kpmin_x, params_.adv_kpmax_x,
                                  params_.adv_pfactor_x, img_w, img_h);
        }

        if (params_.adv_time_dynamic_y && params_.adv_time_y > 0 && aim_timing_)
        {
            kp_y = CalcTimeKP(params_.adv_kpmin_y, params_.adv_kpmax_y,
                              params_.adv_time_y, elapsed_ms);
        }
        else
        {
            kp_y = CalcDistanceKP(dist, params_.adv_kpmin_y, params_.adv_kpmax_y,
                                  params_.adv_pfactor_y, img_w, img_h);
        }

        ki_x = params_.adv_ki_x;  ki_y = params_.adv_ki_y;
        kd_x = params_.adv_kd_x;  kd_y = params_.adv_kd_y;
        imax_x = params_.adv_imax_x; imax_y = params_.adv_imax_y;
    }

    // ── PID 计算 ──
    double raw_dx = aim_x - cross_x;
    double raw_dy = aim_y - cross_y;

    // 积分
    integral_x_ += raw_dx;
    integral_y_ += raw_dy;
    if (imax_x > 0) integral_x_ = clampd(integral_x_, -imax_x, imax_x);
    if (imax_y > 0) integral_y_ = clampd(integral_y_, -imax_y, imax_y);

    // 微分
    double accel_x = 0, accel_y = 0;
    if (!first_frame_)
    {
        accel_x = raw_dx - prev_err_x_;
        accel_y = raw_dy - prev_err_y_;
        double spd = std::sqrt(accel_x * accel_x + accel_y * accel_y);
        velocity_ema_ = spd * 0.3 + velocity_ema_ * 0.7;
    }
    first_frame_ = false;
    prev_err_x_ = raw_dx;
    prev_err_y_ = raw_dy;

    // PID 输出
    double ox = kp_x * raw_dx + ki_x * integral_x_ + kd_x * accel_x;
    double oy = kp_y * raw_dy + ki_y * integral_y_ + kd_y * accel_y;

    out.dx = static_cast<int>(ox + (ox >= 0 ? 0.5 : -0.5));
    out.dy = static_cast<int>(oy + (oy >= 0 ? 0.5 : -0.5));

    // 更新 Kalman 累计量
    if (pred_mode == 2)
    {
        accum_x_ += static_cast<double>(out.dx);
        accum_y_ += static_cast<double>(out.dy);
    }

    return out;
}

} // namespace mover
