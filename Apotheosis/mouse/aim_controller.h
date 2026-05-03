#ifndef MOUSE_AIM_CONTROLLER_H
#define MOUSE_AIM_CONTROLLER_H

#include <algorithm>
#include <cmath>
#include <utility>

namespace aim
{

// 3-knob non-linear P controller. Replaces the old PID + Flick/Track + boost
// stack. Per axis: move = speed * err * (1 + K * lock * w(r)). w(r) is a
// bell-shape that approaches 1 near the target (lock-snap) and 0 far away
// (pure speed control). No integrator, no derivative, no per-tick clamp —
// large recoil corrections must pass through unhindered for crosshair-color
// recoil compensation to work.
struct AimGains
{
    double speed_x = 0.6;       // far-distance proportional gain on X
    double speed_y = 0.6;       // far-distance proportional gain on Y
    double lock_strength = 0.0; // [0,1] near-target stickiness
    // Anchor radius in detection pixels. The bell w(r) = R²/(R²+err²) is 1
    // at err=0 and falls off around R, so this is effectively the "lock
    // range" knob from the tuning guide. 0 = use legacy 0.08*detection_width.
    double lock_radius_px = 25.0;
    // Per-tick attenuation applied to the effective lock multiplier. Used by
    // Path B (recoil-only re-push using stale target) so that crosshair
    // jitter from gun kick doesn't get amplified by the near-target lock
    // boost. 1.0 = no attenuation (Path A behaviour). Set <1 only for the
    // duration of a single step.
    double lock_attenuation = 1.0;
};

// K caps the additional gain at near-zero error to (1 + K * lock_strength).
inline constexpr double kAimLockFallbackRadiusFrac = 0.08;
inline constexpr double kAimLockMultiplier   = 4.0;

class AimController
{
public:
    void setGains(const AimGains& g) noexcept { gains_ = g; }
    const AimGains& gains() const noexcept { return gains_; }

    // Clears sub-pixel residual carry. Call on hotkey changes / lifts /
    // target loss so leftover fractions from the previous engagement do
    // not nudge the crosshair when a new one starts.
    void reset() noexcept { residual_x_ = 0.0; residual_y_ = 0.0; }

    // Return integer (moveX, moveY) given the error in detection pixels
    // and the detection-image side length (drives the anchor radius).
    // Sub-pixel residuals are carried across calls so persistent
    // sub-pixel error eventually produces a 1-px correction instead of
    // being silently truncated — that truncation was the implicit dead
    // zone that left the crosshair drifting near the anchor.
    std::pair<int, int> step(double errX,
                             double errY,
                             double detection_width_px) noexcept
    {
        // lock_strength == 0 short-circuits the entire dynamic-lock branch:
        // pure proportional speed control, no near-target boost. This is
        // the "动态锁死关闭" baseline state.
        double mult = 1.0;
        if (gains_.lock_strength > 1e-6)
        {
            double R = gains_.lock_radius_px;
            if (!(R > 0.0))
                R = kAimLockFallbackRadiusFrac * detection_width_px;
            R = std::max(1.0, R);
            const double r2 = errX * errX + errY * errY;
            const double w = (R * R) / (R * R + r2); // 1 at target, 0 far
            const double atten = std::clamp(gains_.lock_attenuation, 0.0, 1.0);
            mult = 1.0 + kAimLockMultiplier * gains_.lock_strength * atten * w;
        }

        const double rawX = gains_.speed_x * errX * mult + residual_x_;
        const double rawY = gains_.speed_y * errY * mult + residual_y_;

        const double mxd = std::trunc(rawX);
        const double myd = std::trunc(rawY);
        residual_x_ = rawX - mxd;
        residual_y_ = rawY - myd;

        return { static_cast<int>(mxd), static_cast<int>(myd) };
    }

private:
    AimGains gains_{};
    double residual_x_ = 0.0;
    double residual_y_ = 0.0;
};

} // namespace aim

#endif // MOUSE_AIM_CONTROLLER_H
