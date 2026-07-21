#include "movers.h"

#include <algorithm>
#include <cmath>

namespace mover
{

double YaoguangMover::alphaFromHz(double hz, double dt)
{
    const double x = 2.0 * 3.14159265358979323846 * hz * dt;
    return x / (1.0 + x);
}

void YaoguangMover::reset()
{
    x_ = {}; y_ = {};
    last_track_id_ = -1;
    initialized_ = false;
    confidence_ = 0.0;
}

void YaoguangMover::configure(const YaoguangParams& p)
{
    params_.pull_speed_x = clampd(p.pull_speed_x, 0.0, 100.0);
    params_.pull_speed_y = clampd(p.pull_speed_y, 0.0, 100.0);
    params_.tracking = clampd(p.tracking, 0.0, 100.0);
    params_.prediction_ms = clampd(p.prediction_ms, 0.0, 100.0);
    params_.stability = clampd(p.stability, 0.0, 100.0);
}

void YaoguangMover::applyMove(int dx, int dy)
{
    x_.prev_output = static_cast<double>(dx);
    y_.prev_output = static_cast<double>(dy);
}

double YaoguangMover::stepAxis(AxisState& s, double measurement, double cross,
                               double bbox_axis, double image_size, double dt,
                               double pull_speed, double& goal_out)
{
    const double stable = params_.stability * 0.01;
    const double innovation = measurement - (s.position + s.velocity_slow * dt);
    const double teleport = std::max({24.0, bbox_axis * 2.5, image_size * 0.45});
    if (std::abs(innovation) > teleport || dt > 0.035)
    {
        s.position = measurement;
        s.velocity_fast = s.velocity_slow = 0.0;
        s.integral = 0.0;
        s.prev_error = measurement - cross;
        confidence_ = 0.0;
    }
    else
    {
        const double raw_v = clampd(innovation / std::max(dt, 1.0 / 300.0),
                                    -image_size * 16.0, image_size * 16.0);
        s.velocity_fast += (raw_v - s.velocity_fast) * alphaFromHz(18.0 - 8.0 * stable, dt);
        s.velocity_slow += (s.velocity_fast - s.velocity_slow) * alphaFromHz(7.0 - 4.0 * stable, dt);
        s.position += innovation * alphaFromHz(18.0 - 8.0 * stable, dt);
        confidence_ = clampd(confidence_ + dt * (raw_v * s.velocity_slow >= 0.0 ? 8.0 : -14.0), 0.0, 1.0);
    }

    const double horizon = params_.prediction_ms * 0.001 * confidence_;
    goal_out = s.position + s.velocity_slow * horizon;
    const double error = goal_out - cross;
    const double near_radius = std::max(3.0, bbox_axis * 0.18);
    const double far_mix = clampd(std::abs(error) / (near_radius * 3.0), 0.0, 1.0);
    const double pull = pull_speed * 0.01;
    const double track = params_.tracking * 0.01;
    const double kp_near = 0.10 + 0.65 * track * track;
    const double kp_far = 0.18 + 0.80 * pull * pull;
    const double kp = kp_near + (kp_far - kp_near) * far_mix;
    const double feedforward = s.velocity_slow * dt * (0.10 + 0.65 * track);

    if (std::abs(error) < near_radius && confidence_ > 0.5)
        s.integral = clampd(s.integral * std::exp(-3.0 * dt) + error * dt,
                            -near_radius * 0.08, near_radius * 0.08);
    else
        s.integral *= std::exp(-10.0 * dt);

    const double derivative = (error - s.prev_error) / std::max(dt, 1.0 / 300.0);
    const double kd = (0.0002 + 0.0018 * stable) * (1.0 - 0.65 * far_mix);
    double output = kp * error + feedforward + (0.04 * track) * s.integral - kd * derivative;
    const double safe = std::abs(error) * (0.72 + 0.23 * far_mix);
    output = clampd(output, -safe, safe);
    const double accel = image_size * (18.0 + 42.0 * pull) * dt;
    output = clampd(output, s.prev_output - accel, s.prev_output + accel);
    output = clampd(output, -image_size * 0.38, image_size * 0.38);
    s.prev_error = error;
    s.prev_output = output;
    const double raw = output + s.residual;
    const double whole = std::trunc(raw);
    s.residual = raw - whole;
    return whole;
}

Move YaoguangMover::step(double anchor_x, double anchor_y, double cross_x, double cross_y,
                         double bbox_w, double bbox_h, double image_size, double dt,
                         int track_id)
{
    Move out;
    dt = clampd(dt, 1.0 / 300.0, 0.1);
    image_size = std::max(64.0, image_size);
    if (!initialized_ || track_id != last_track_id_)
    {
        reset(); initialized_ = true; last_track_id_ = track_id;
        x_.position = anchor_x; y_.position = anchor_y;
        x_.prev_error = anchor_x - cross_x; y_.prev_error = anchor_y - cross_y;
    }
    out.dx = static_cast<int>(stepAxis(x_, anchor_x, cross_x, bbox_w, image_size, dt,
                                       params_.pull_speed_x, out.aim_x));
    out.dy = static_cast<int>(stepAxis(y_, anchor_y, cross_y, bbox_h, image_size, dt,
                                       params_.pull_speed_y, out.aim_y));
    return out;
}

} // namespace mover
