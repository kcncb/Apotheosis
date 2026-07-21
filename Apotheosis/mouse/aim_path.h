#ifndef MOUSE_AIM_PATH_H
#define MOUSE_AIM_PATH_H

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace boss
{

// Per-frame trajectory shaper.
//
// Three modes:
//   Linear  — direct proportional move, identical to ART::drive(). Each
//             frame the cursor steps `speed * err` toward the aim point.
//             No anchoring, no arc; this is the legacy / default behaviour.
//   Bezier  — cubic Bezier from (0,0) to (1,0) with two control points
//             cx1,cy1,cx2,cy2.  X = travel progress along the start→goal
//             chord, Y = perpendicular deviation in units of chord length.
//   Custom  — a high-resolution piecewise-linear deviation curve drawn by the
//             user in the UI. Endpoints are pinned to zero so the path
//             still ends on the goal.
//
// On lock or large goal drift the driver re-anchors: start = current cursor,
// goal = current aim. Travel progress is then driven by cursor PROJECTION
// onto the start→goal axis (not wall clock), so a moving target slowing
// you down doesn't break the shape; the cursor just takes more frames to
// arrive. Residual pixels from sub-integer moves accumulate the same way
// ART::drive does, so micro-motion still adds up correctly over time.
class AimPathDriver
{
public:
    enum class Mode : int
    {
        Linear = 0,
        Bezier = 1,
        Custom = 2,
    };

    static constexpr int kCustomSamples = 32768;

    struct Params
    {
        Mode  mode = Mode::Linear;
        double speed_x      = 0.6;
        double speed_y      = 0.6;
        double dead_zone_px = 2.0;
        double strength = 1.0;

        // Bezier control points (X∈[0,1], Y∈[-1,1]).
        double cx1 = 0.30, cy1 = 0.00;
        double cx2 = 0.70, cy2 = 0.00;

        // Custom curve samples (Y at X = 0..1 in uniform N steps).
        std::shared_ptr<const std::vector<float>> custom_samples;
        bool neural_enabled = false;
        std::array<float, 25> neural_weights{};

        // Re-anchor when the goal drifts more than this many pixels from
        // the current path's endpoint. Below this we keep the existing
        // shape (target jitter shouldn't restart the path).
        double reanchor_px = 40.0;
    };

    struct Result
    {
        int move_x = 0;
        int move_y = 0;
    };

    void configure(const Params& p)
    {
        const bool shape_changed =
            p.mode != p_.mode ||
            p.cx1 != p_.cx1 || p.cy1 != p_.cy1 ||
            p.cx2 != p_.cx2 || p.cy2 != p_.cy2 ||
            p.custom_samples != p_.custom_samples ||
            p.neural_enabled != p_.neural_enabled ||
            p.neural_weights != p_.neural_weights;
        if (shape_changed)
            reset();
        p_ = p;
    }

    void reset()
    {
        engaged_ = false;
        last_id_ = -1;
        progress_ = 0.0;
        reference_length_ = 1.0;
        axis_x_ = 1.0;
        axis_y_ = 0.0;
        blend_entry_tangent_ = false;
        entry_slope_ = 0.0;
        rx_ = ry_ = 0.0;
    }

    // 由上层在最终 Y 力度缩放之后回报真正发送的位移。step() 只计算，
    // 不提前假设原始输出一定被采用。
    void applyMove(int dx, int dy)
    {
        (void)dx;
        (void)dy;
    }

    // dt is unused in Linear mode (matches ART); other modes use it only
    // as a soft hint for very slow frames — progress is otherwise driven
    // by cursor projection.
    Result step(double aim_x, double aim_y,
                double cur_x, double cur_y,
                double /*dt*/, int target_id,
                int base_dx, int base_dy,
                double settle_radius_px = 0.0)
    {
        Result out;

        // 直线模式只透传 mover 已经算好的输出。这样 PDFLr / PID / ART 的
        // 增益、阻尼、死区在所有轨迹模式下都保持同一套语义。
        if (p_.mode == Mode::Linear)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double screen_err_x = aim_x - cur_x;
        const double screen_err_y = aim_y - cur_y;
        const double screen_err_mag = std::hypot(screen_err_x, screen_err_y);

        // 每次锁定只固定一次轨迹坐标系。检测框的微小抖动不应每帧旋转
        // 曲线的法向量，否则 1~2 px 的目标噪声会被放大成交替侧向位移。
        const bool id_changed = (target_id >= 0 && target_id != last_id_);
        if (!engaged_ || id_changed)
        {
            engaged_ = true;
            last_id_ = target_id;
            progress_ = 0.0;
            reference_length_ = std::max(1.0, screen_err_mag);
            if (screen_err_mag > 1e-6)
            {
                axis_x_ = screen_err_x / screen_err_mag;
                axis_y_ = screen_err_y / screen_err_mag;
            }
            rx_ = ry_ = 0.0;
        }
        last_id_ = target_id;

        // mover 输出为 0 就必须停。旧实现会绕过 mover 自己的死区/阻尼，
        // 导致已经收敛时曲线仍继续发位移。
        const double base_mag = std::hypot(static_cast<double>(base_dx),
                                           static_cast<double>(base_dy));
        if (base_mag < 0.5)
        {
            rx_ = ry_ = 0.0;
            return out;
        }

        // 极短路径或控制器正在反向制动时直接采用原输出。
        if (screen_err_mag < 1.0)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        // 近目标时必须逐渐退回控制器原始方向。否则曲线切线会在目标已经
        // 很近时继续制造侧向分量，移动目标反复开新段后表现为绕目标画圈。
        // settle_radius 由上层按 bbox 大小提供，0 保持旧行为。
        const double settle_radius = std::max(0.0, settle_radius_px);
        if (settle_radius > 0.0 && screen_err_mag <= settle_radius)
        {
            rx_ = ry_ = 0.0;
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        const double forward = static_cast<double>(base_dx) * screen_err_x
                             + static_cast<double>(base_dy) * screen_err_y;
        if (forward <= 0.0)
        {
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        // 上一段已到达曲线终点，但移动目标仍让原控制器产生有效
        // 前进输出时，以当前误差开启下一段。否则 progress 永远停在 1，
        // 尾部切线为零的曲线会在第一段之后全部退化成直线。
        if (progress_ >= 1.0 - 1e-9)
        {
            entry_slope_ = curve_derivative(1.0);
            blend_entry_tangent_ = true;
            progress_ = 0.0;
            reference_length_ = std::max(1.0, screen_err_mag);
            if (screen_err_mag > 1e-6)
            {
                axis_x_ = screen_err_x / screen_err_mag;
                axis_y_ = screen_err_y / screen_err_mag;
            }
            rx_ = ry_ = 0.0;
        }

        // 进度是显式单调状态，不再将虚拟游标投影到每帧变化的目标轴上。
        double dy_dt = curve_derivative(progress_);
        dy_dt *= std::clamp(p_.strength, 0.0, 1.0);
        if (blend_entry_tangent_)
        {
            constexpr double kBlendSpan = 0.15;
            const double u = clamp01(progress_ / kBlendSpan);
            const double smooth = u * u * (3.0 - 2.0 * u);
            dy_dt = entry_slope_ + (dy_dt - entry_slope_) * smooth;
            if (u >= 1.0) blend_entry_tangent_ = false;
        }
        if (settle_radius > 0.0)
        {
            const double fade_end = settle_radius * 2.5;
            const double u = clamp01((screen_err_mag - settle_radius) /
                                     std::max(1.0, fade_end - settle_radius));
            const double smooth = u * u * (3.0 - 2.0 * u);
            dy_dt *= smooth;
        }
        const double local_scale = std::sqrt(1.0 + dy_dt * dy_dt);
        progress_ = clamp01(progress_ + base_mag /
            std::max(1.0, reference_length_ * local_scale));

        if (std::abs(dy_dt) < 1e-9)
        {
            rx_ = ry_ = 0.0;
            out.move_x = base_dx;
            out.move_y = base_dy;
            return out;
        }

        // 在锁定时的稳定坐标系内，只按曲线局部切线旋转原 mover
        // 输出，不改变幅值。平坦曲线 dy/dt=0 时必须与直线模式逐像素一致。
        const double perp_x = -axis_y_;
        const double perp_y =  axis_x_;
        const double local_x = static_cast<double>(base_dx) * axis_x_
                             + static_cast<double>(base_dy) * axis_y_;
        const double local_y = static_cast<double>(base_dx) * perp_x
                             + static_cast<double>(base_dy) * perp_y;
        const double inv_scale = 1.0 / local_scale;
        const double cos_a = inv_scale;
        const double sin_a = dy_dt * inv_scale;
        const double shaped_local_x = local_x * cos_a - local_y * sin_a;
        const double shaped_local_y = local_x * sin_a + local_y * cos_a;
        const double step_x = shaped_local_x * axis_x_ + shaped_local_y * perp_x;
        const double step_y = shaped_local_x * axis_y_ + shaped_local_y * perp_y;

        const double raw_x = step_x + rx_;
        const double raw_y = step_y + ry_;

        out.move_x = static_cast<int>(std::trunc(raw_x));
        out.move_y = static_cast<int>(std::trunc(raw_y));
        rx_ = raw_x - out.move_x;
        ry_ = raw_y - out.move_y;
        return out;
    }

private:
    static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    Result drive_linear(double ex, double ey)
    {
        Result out;
        const double err = std::sqrt(ex * ex + ey * ey);
        if (err < p_.dead_zone_px)
        {
            rx_ = ry_ = 0.0;
            return out;
        }
        const double raw_x = p_.speed_x * ex + rx_;
        const double raw_y = p_.speed_y * ey + ry_;
        out.move_x = static_cast<int>(std::trunc(raw_x));
        out.move_y = static_cast<int>(std::trunc(raw_y));
        rx_ = raw_x - out.move_x;
        ry_ = raw_y - out.move_y;
        return out;
    }

    // Evaluate the deviation curve Y at progress t∈[0,1].
    double curve_y(double t) const
    {
        if (p_.mode == Mode::Bezier)
        {
            // Solve cubic Bezier in X for parameter u s.t. X(u)=t, then
            // return Y(u). Iterative Newton (6 iters is enough for px work).
            double u = t;
            for (int i = 0; i < 6; ++i)
            {
                const double x  = bezier_axis(u, 0.0, p_.cx1, p_.cx2, 1.0);
                const double dx = bezier_deriv(u, 0.0, p_.cx1, p_.cx2, 1.0);
                if (std::abs(dx) < 1e-6) break;
                u -= (x - t) / dx;
                u  = clamp01(u);
            }
            return bezier_axis(u, 0.0, p_.cy1, p_.cy2, 0.0);
        }
        if (p_.mode == Mode::Custom)
        {
            if (p_.neural_enabled)
            {
                const double x = t * 2.0 - 1.0;
                double raw = static_cast<double>(p_.neural_weights[24]);
                for (int h = 0; h < 8; ++h)
                {
                    const double activation = std::tanh(
                        static_cast<double>(p_.neural_weights[h]) * x +
                        static_cast<double>(p_.neural_weights[8 + h]));
                    raw += static_cast<double>(p_.neural_weights[16 + h]) * activation;
                }
                return std::tanh(raw) * 4.0 * t * (1.0 - t);
            }
            if (!p_.custom_samples) return 0.0;
            const auto& samples = *p_.custom_samples;
            const int N = static_cast<int>(samples.size());
            if (N < 2) return 0.0;
            const double pos = t * (N - 1);
            int i0 = static_cast<int>(std::floor(pos));
            int i1 = i0 + 1;
            if (i0 < 0)      { i0 = 0;     i1 = 1; }
            if (i1 > N - 1)  { i1 = N - 1; i0 = i1 - 1; }
            const double f = pos - i0;
            const double y0 = static_cast<double>(samples[i0]);
            const double y1 = static_cast<double>(samples[i1]);
            return y0 + (y1 - y0) * f;
        }
        return 0.0;
    }

    double curve_derivative(double t) const
    {
        constexpr double eps = 1e-4;
        const double ta = std::max(0.0, t - eps);
        const double tb = std::min(1.0, t + eps);
        return (tb > ta) ? (curve_y(tb) - curve_y(ta)) / (tb - ta) : 0.0;
    }

    static double bezier_axis(double u, double a, double b, double c, double d)
    {
        const double m = 1.0 - u;
        return m*m*m*a + 3.0*m*m*u*b + 3.0*m*u*u*c + u*u*u*d;
    }
    static double bezier_deriv(double u, double a, double b, double c, double d)
    {
        const double m = 1.0 - u;
        return 3.0*m*m*(b - a) + 6.0*m*u*(c - b) + 3.0*u*u*(d - c);
    }

    Params p_{};
    bool engaged_ = false;
    int  last_id_ = -1;
    double progress_ = 0.0;
    double reference_length_ = 1.0;
    double axis_x_ = 1.0, axis_y_ = 0.0;
    bool blend_entry_tangent_ = false;
    double entry_slope_ = 0.0;
    double rx_ = 0.0, ry_ = 0.0;
};

} // namespace boss

#endif // MOUSE_AIM_PATH_H
