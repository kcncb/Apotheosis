#ifndef MOUSE_PID_CONTROLLER_H
#define MOUSE_PID_CONTROLLER_H

#include <algorithm>
#include <cmath>
#include <utility>

namespace aim
{

struct PidGains
{
    double p = 0.5;
    double p_x = 0.5;
    double p_y = 0.5;
    double i = 0.0;
    double d = 0.1;
};

// Anti-windup limit for integral accumulation. PID output is measured in
// raw mouse counts, so this effectively caps the steady-state contribution
// of the integral term to ~10k counts of bias.
inline constexpr double kPidIntegralClamp = 10000.0;

class PidAxis
{
public:
    void reset() noexcept
    {
        integral_ = 0.0;
        prev_error_ = 0.0;
        has_prev_ = false;
    }

    double step(double error, double dt, const PidGains& gains) noexcept
    {
        if (!(dt > 1e-6))
            dt = 1e-6;

        integral_ += error * dt;
        if (integral_ > kPidIntegralClamp) integral_ = kPidIntegralClamp;
        else if (integral_ < -kPidIntegralClamp) integral_ = -kPidIntegralClamp;

        const double derivative = has_prev_ ? (error - prev_error_) / dt : 0.0;
        prev_error_ = error;
        has_prev_ = true;

        return gains.p * error + gains.i * integral_ + gains.d * derivative;
    }

private:
    double integral_ = 0.0;
    double prev_error_ = 0.0;
    bool has_prev_ = false;
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
        PidGains x_gains = gains_;
        PidGains y_gains = gains_;
        x_gains.p = gains_.p_x;
        y_gains.p = gains_.p_y;
        return { x_.step(errX, dt, x_gains), y_.step(errY, dt, y_gains) };
    }

private:
    PidGains gains_{};
    PidAxis x_{};
    PidAxis y_{};
};

} // namespace aim

#endif // MOUSE_PID_CONTROLLER_H
