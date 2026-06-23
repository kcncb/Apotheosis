#ifndef MOUSE_AIM_PATH_H
#define MOUSE_AIM_PATH_H

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

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
//   Custom  — a 32-sample piecewise-linear deviation curve drawn by the
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

    static constexpr int kCustomSamples = 32;

    struct Params
    {
        Mode  mode = Mode::Linear;
        double speed_x      = 0.6;
        double speed_y      = 0.6;
        double dead_zone_px = 2.0;

        // Bezier control points (X∈[0,1], Y∈[-1,1]).
        double cx1 = 0.30, cy1 = 0.00;
        double cx2 = 0.70, cy2 = 0.00;

        // Custom curve samples (Y at X = 0..1 in uniform N steps).
        std::array<float, kCustomSamples> custom_samples{};

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

    void configure(const Params& p) { p_ = p; }

    void reset()
    {
        engaged_ = false;
        last_id_ = -1;
        rx_ = ry_ = 0.0;
    }

    // dt is unused in Linear mode (matches ART); other modes use it only
    // as a soft hint for very slow frames — progress is otherwise driven
    // by cursor projection.
    Result step(double aim_x, double aim_y,
                double cur_x, double cur_y,
                double /*dt*/, int target_id)
    {
        Result out;

        // Linear: replicate ART::drive() byte-for-byte semantics so that
        // toggling between modes feels neutral on the "straight" preset.
        if (p_.mode == Mode::Linear)
        {
            return drive_linear(aim_x - cur_x, aim_y - cur_y);
        }

        // Re-anchor on lock change, first call, or large goal drift.
        const bool id_changed = (target_id >= 0 && target_id != last_id_);
        const double dx_goal = aim_x - goal_x_;
        const double dy_goal = aim_y - goal_y_;
        const bool goal_drifted = (engaged_) &&
            (std::sqrt(dx_goal * dx_goal + dy_goal * dy_goal) > p_.reanchor_px);

        if (!engaged_ || id_changed || goal_drifted)
        {
            start_x_ = cur_x;  start_y_ = cur_y;
            goal_x_  = aim_x;  goal_y_  = aim_y;
            engaged_ = true;
            last_id_ = target_id;
            rx_ = ry_ = 0.0;
            // path length cached so we don't sqrt every frame
            const double lx = goal_x_ - start_x_;
            const double ly = goal_y_ - start_y_;
            length_ = std::sqrt(lx * lx + ly * ly);
        }
        else
        {
            // Soft update of the goal so a slowly-moving target doesn't
            // freeze the path. We re-extend rather than re-anchor.
            goal_x_ = aim_x;
            goal_y_ = aim_y;
            const double lx = goal_x_ - start_x_;
            const double ly = goal_y_ - start_y_;
            length_ = std::sqrt(lx * lx + ly * ly);
        }
        last_id_ = target_id;

        // Degenerate paths: fall back to linear so the cursor can still
        // close very small errors without numerical noise from the curve.
        if (length_ < 1.0)
        {
            return drive_linear(aim_x - cur_x, aim_y - cur_y);
        }

        // Axis (start→goal) and perpendicular (left-hand 90°).
        const double ax = (goal_x_ - start_x_) / length_;
        const double ay = (goal_y_ - start_y_) / length_;
        const double px = -ay;
        const double py =  ax;

        // Progress = cursor projection on axis (clamped 0..1).
        const double rel_x = cur_x - start_x_;
        const double rel_y = cur_y - start_y_;
        double t = (rel_x * ax + rel_y * ay) / length_;
        t = clamp01(t);

        // Step forward along progress by (speed * err_along_axis) / length.
        // This is the analogue of "speed * err" in linear, but expressed
        // in path-progress units. Y is mixed in via speed_y on the
        // perpendicular axis so the user's speed_x/y still apply naturally.
        const double axis_err   = (1.0 - t) * length_;
        const double t_step     = clamp01(p_.speed_x * axis_err / length_);
        const double t_next     = clamp01(t + t_step);

        // Sample curve at t_next.
        const double y_norm = curve_y(t_next);
        // Desired cursor position at the next sub-step.
        const double desired_x = start_x_ + t_next * length_ * ax
                                 + y_norm  * length_ * px;
        const double desired_y = start_y_ + t_next * length_ * ay
                                 + y_norm  * length_ * py;

        // Per-axis error with speed scaling on perpendicular component.
        const double dxv = desired_x - cur_x;
        const double dyv = desired_y - cur_y;

        // Dead zone on the REMAINING goal distance — once we're within
        // dead_zone, freeze (path is done). Otherwise apply speed_y to
        // the perpendicular component to honour the user's vertical-gain
        // preference.
        const double goal_dx = goal_x_ - cur_x;
        const double goal_dy = goal_y_ - cur_y;
        const double goal_err = std::sqrt(goal_dx * goal_dx + goal_dy * goal_dy);
        if (goal_err < p_.dead_zone_px)
        {
            rx_ = ry_ = 0.0;
            return out;
        }

        // Decompose desired step into axis vs perpendicular components,
        // apply speed_y to perpendicular, then recompose. This preserves
        // the curve shape but lets the user damp vertical motion.
        const double step_axis  = dxv * ax + dyv * ay;
        const double step_perp  = dxv * px + dyv * py;
        const double scaled_axis = step_axis;
        const double scaled_perp = step_perp * (p_.speed_y / std::max(p_.speed_x, 1e-3));
        const double raw_x = scaled_axis * ax + scaled_perp * px + rx_;
        const double raw_y = scaled_axis * ay + scaled_perp * py + ry_;

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
            const int N = static_cast<int>(p_.custom_samples.size());
            if (N < 2) return 0.0;
            const double pos = t * (N - 1);
            int i0 = static_cast<int>(std::floor(pos));
            int i1 = i0 + 1;
            if (i0 < 0)      { i0 = 0;     i1 = 1; }
            if (i1 > N - 1)  { i1 = N - 1; i0 = i1 - 1; }
            const double f = pos - i0;
            const double y0 = static_cast<double>(p_.custom_samples[i0]);
            const double y1 = static_cast<double>(p_.custom_samples[i1]);
            return y0 + (y1 - y0) * f;
        }
        return 0.0;
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
    double start_x_ = 0.0, start_y_ = 0.0;
    double goal_x_  = 0.0, goal_y_  = 0.0;
    double length_  = 0.0;
    double rx_ = 0.0, ry_ = 0.0;
};

} // namespace boss

#endif // MOUSE_AIM_PATH_H
