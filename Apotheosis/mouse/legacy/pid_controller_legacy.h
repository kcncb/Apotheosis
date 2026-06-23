#ifndef MOUSE_PID_CONTROLLER_H
#define MOUSE_PID_CONTROLLER_H

#include <algorithm>
#include <cmath>
#include <utility>

// =============================================================================
// 数字 PID 控制器(并联式,工业级实现) —— 从头重写
// -----------------------------------------------------------------------------
// 控制律仍是标准并联 PID:
//   u = Kp·e + Ki·∫e dt + Kd·d(e)/dt
// 在此之上做了三项工业级处理(都不改变它仍是 P/I/D 三参数 PID 这一事实):
//   1) 不完全微分(filtered derivative):对 D 项一阶低通,抑制小目标检测噪声被微分
//      放大成抽搐,并削弱目标跳变瞬间的"微分冲击(derivative kick)"。
//   2) 积分分离(conditional integration):误差过大(甩枪/跳变)时冻结积分累加,
//      避免积分在大误差段饱和、回到目标后产生超调。
//   3) 积分抗饱和钳位(anti-windup clamp):积分项硬上限,封死长期偏置导致的飘移。
// 误差(检测像素)→ u → 上层 lround 后送驱动。Kp/Ki/Kd 直接生效(无 ×0.1 缩放)。
//
// 与卡尔曼的分工:卡尔曼负责"目标在哪/将到哪"(平滑+提前量),PID 只负责把准星
// 平稳地驱到这个点。因此 PID 默认保守、靠 Kd 阻尼收敛,不做单帧限幅/死区。
// =============================================================================

namespace aim
{

struct PidGains
{
    double p = 0.5;
    double i = 0.0;
    double d = 0.1;

    // 不完全微分系数 [0,1):对 D 项输出做一阶低通。0 = 纯微分;越大滤波越强、
    // 相位滞后越多。常用 0.4~0.7。
    double d_filter = 0.55;

    // 积分分离阈值(|error| 绝对值,单位与输入一致)。|error| 超过该阈值时停止积分
    // 累加(保留已有值),回到阈值内再恢复。<=0 关闭分离。由上层按分辨率设置。
    double i_separation = 0.0;
};

// 积分项硬上限(抗饱和)。PID 输出单位是原始鼠标 counts,这相当于把积分项的稳态
// 贡献封顶在约 ±10k counts 的偏置。
inline constexpr double kPidIntegralClamp = 10000.0;

// 微分用的 dt 下限。控制步以可变节奏调用(检测事件 + 准星重推),偶尔两步间隔
// 只有几十微秒;若直接用真实 dt 做 (Δerr/dt),微分会被放大成天文数字,哪怕 D 极小
// 也会让鼠标瞬间飞走。把微分的 dt 钳到一个真实控制周期(≈1/200s)即可根除。
inline constexpr double kPidDerivativeMinDt = 1.0 / 200.0;
// 微分项单步输出硬上限(counts),最后一道安全阀。
inline constexpr double kPidDerivativeClamp = 4000.0;

class PidAxis
{
public:
    void reset() noexcept
    {
        integral_ = 0.0;
        prev_error_ = 0.0;
        d_state_ = 0.0;
        has_prev_ = false;
    }

    double step(double error, double dt, const PidGains& g) noexcept
    {
        if (!(dt > 1e-6)) dt = 1e-6;
        if (dt > 0.25)    dt = 0.25;     // 单帧时长封顶,防偶发大 dt 把微分/积分炸飞

        // ---- 比例 ----
        const double p_term = g.p * error;

        // ---- 积分(条件积分 + 抗饱和钳位)----
        const bool integrate = !(g.i_separation > 0.0) ||
                               std::fabs(error) < g.i_separation;
        if (integrate)
        {
            integral_ += error * dt;
            integral_ = std::clamp(integral_, -kPidIntegralClamp, kPidIntegralClamp);
        }
        const double i_term = g.i * integral_;

        // ---- 微分(受限 dt + 不完全微分低通 + 安全钳位)----
        // 用 max(dt, kPidDerivativeMinDt) 做微分,杜绝可变节奏下 dt 极小把微分炸飞。
        const double dtD = (dt < kPidDerivativeMinDt) ? kPidDerivativeMinDt : dt;
        const double derivative = has_prev_ ? (error - prev_error_) / dtD : 0.0;
        double raw_d = g.d * derivative;
        raw_d = std::clamp(raw_d, -kPidDerivativeClamp, kPidDerivativeClamp);
        const double beta = std::clamp(g.d_filter, 0.0, 0.99);
        d_state_ = beta * d_state_ + (1.0 - beta) * raw_d;

        prev_error_ = error;
        has_prev_ = true;

        const double u = p_term + i_term + d_state_;
        return std::isfinite(u) ? u : 0.0;
    }

private:
    double integral_ = 0.0;
    double prev_error_ = 0.0;
    double d_state_ = 0.0;     // 滤波后的微分项
    bool   has_prev_ = false;
};

class PidController2D
{
public:
    void setGains(const PidGains& gains) noexcept { gains_ = gains; }
    const PidGains& gains() const noexcept { return gains_; }

    void reset() noexcept
    {
        x_.reset();
        y_.reset();
    }

    std::pair<double, double> step(double errX, double errY, double dt) noexcept
    {
        return { x_.step(errX, dt, gains_), y_.step(errY, dt, gains_) };
    }

private:
    PidGains gains_{};
    PidAxis x_{};
    PidAxis y_{};
};

} // namespace aim

#endif // MOUSE_PID_CONTROLLER_H
