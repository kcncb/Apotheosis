#ifndef MOUSE_MOVERS_H
#define MOUSE_MOVERS_H

// =============================================================================
// 鼠标移动控制器二选一 — 在 boss::AimEngine 输出 dx/dy 前接管。
//
//   Kind::Smooth      微澜 — 走 ART/path 原路径(EMA 自适应平滑),controller 不介入,
//                            engine 直接用 ArtResult/AimPathDriver 的 move。
//   Kind::Predictive  疾风 — 位置式 PID + 导数估计预测器。每帧:
//                              dx = Kp*err + Kd*(err - prev_err)/dt_norm
//                            自然 err→0 时 dx→0,不会绕目标转。预测器只贡献 fused 误差。
//
// 用户面只 4 个旋钮: 灵敏度 X (Kp_x) / 灵敏度 Y (Kp_y) / 阻尼 (Kd) / 预测权重。
// 渐入、输出上限、单像素死区都硬编默认值。
// =============================================================================

#include <algorithm>
#include <cmath>
#include <utility>

namespace mover
{

enum class Kind : int
{
    Smooth     = 0,
    Predictive = 1,
};

struct Move
{
    int dx = 0;
    int dy = 0;
};

// ---------------------------------------------------------------------------
// 疾风 — 暴露 4 项可调。
// ---------------------------------------------------------------------------
struct PredictiveParams
{
    double kp_x        = 0.6;
    double kp_y        = 0.6;
    double kd          = 0.10;
    double pred_weight = 0.5;   // 双轴共用,0 = 纯 PID,1 = 完整预测。
};

// ---------------------------------------------------------------------------
// 疾风内部: 导数估计预测器 (vel/acc 平滑 + 运动学外推)。
// ---------------------------------------------------------------------------
class DerivativePredictor
{
public:
    void reset() noexcept
    {
        last_error_x_ = last_error_y_ = 0.0;
        smooth_vel_x_ = smooth_vel_y_ = 0.0;
        smooth_acc_x_ = smooth_acc_y_ = 0.0;
        has_last_ = false;
    }

    std::pair<double, double> predict(double error_x, double error_y, double dt) noexcept
    {
        if (!has_last_)
        {
            last_error_x_ = error_x;
            last_error_y_ = error_y;
            has_last_ = true;
            return { 0.0, 0.0 };
        }

        dt = std::clamp(dt, 0.001, 0.05);

        // 注意: 不再用 prev_move 反推 (mouse counts vs detection pixels 单位错配会
        // 在高灵敏度时引入稳态偏置,锁偏的根因之一)。直接看 err 变化。
        double vel_x = std::clamp((error_x - last_error_x_) / dt, -3000.0, 3000.0);
        double vel_y = std::clamp((error_y - last_error_y_) / dt, -3000.0, 3000.0);

        // 过冲抑制: vel 与 err 反号时压速度。
        if (std::fabs(error_x) > 5.0 && vel_x * error_x > 0.0) vel_x *= 0.1;
        if (std::fabs(error_y) > 5.0 && vel_y * error_y > 0.0) vel_y *= 0.1;

        const double acc_x = std::clamp((vel_x - smooth_vel_x_) / dt, -5000.0, 5000.0);
        const double acc_y = std::clamp((vel_y - smooth_vel_y_) / dt, -5000.0, 5000.0);

        const double alpha_v = std::clamp(1.0 - std::pow(0.75, dt / 0.01), 0.05, 0.8);
        const double alpha_a = std::clamp(1.0 - std::pow(0.85, dt / 0.01), 0.05, 0.8);

        smooth_vel_x_ += (vel_x - smooth_vel_x_) * alpha_v;
        smooth_vel_y_ += (vel_y - smooth_vel_y_) * alpha_v;
        smooth_acc_x_ += (acc_x - smooth_acc_x_) * alpha_a;
        smooth_acc_y_ += (acc_y - smooth_acc_y_) * alpha_a;

        last_error_x_ = error_x;
        last_error_y_ = error_y;

        return {
            smooth_vel_x_ * dt + 0.5 * smooth_acc_x_ * dt * dt,
            smooth_vel_y_ * dt + 0.5 * smooth_acc_y_ * dt * dt
        };
    }

private:
    double last_error_x_ = 0.0, last_error_y_ = 0.0;
    double smooth_vel_x_ = 0.0, smooth_vel_y_ = 0.0;
    double smooth_acc_x_ = 0.0, smooth_acc_y_ = 0.0;
    bool   has_last_ = false;
};

class PredictiveMover
{
public:
    void reset();
    void configure(const PredictiveParams& p);
    Move step(double err_x, double err_y, double dt, int track_id);

private:
    PredictiveParams params_{};
    DerivativePredictor predictor_{};
    double prev_err_x_   = 0.0;
    double prev_err_y_   = 0.0;
    int    last_track_id_ = -1;
    double lock_age_sec_  = 0.0;
    double rx_ = 0.0, ry_ = 0.0;   // 亚像素残差。
};

} // namespace mover

#endif // MOUSE_MOVERS_H
