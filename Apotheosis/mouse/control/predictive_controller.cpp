#include "predictive_controller.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace motion
{
namespace
{
double finiteClamp(double v, double low, double high, double fallback)
{ return std::isfinite(v) ? std::clamp(v, low, high) : fallback; }
constexpr double kJerkVariance = 1e10;
constexpr double kManeuverInnovationSquared = 9.0;
constexpr double kPositionJumpInnovationSquared = 100.0;
constexpr double kManeuverHoldSec = .025;
constexpr double kVelocityLimit = 3000.0;
// 连续多少次新息变号才认定"检测框在抖"。真实换向/急停只会变一次号(随后同号),
// 抖动则连续变号; 取 2 可以把"换向那一帧"留给原逻辑, 避免把真实换向当抖动平滑掉。
constexpr int kJitterFlipRun = 2;
constexpr double kAccelerationLimit = 20000.0;
constexpr double kCovarianceFloor = 1e-9;

bool finite(Vec v) { return std::isfinite(v.x) && std::isfinite(v.y); }
Calibration sanitize(Calibration c)
{
    c.pixels_per_count_x = finiteClamp(c.pixels_per_count_x, .01, 100, 1);
    c.pixels_per_count_y = finiteClamp(c.pixels_per_count_y, .01, 100, 1);
    c.effect_delay_ns = std::clamp<Time>(c.effect_delay_ns, 0, 100'000'000);
    c.effect_uncertainty_ns = std::clamp<Time>(c.effect_uncertainty_ns, 0, 50'000'000);
    return c;
}
}
uint64_t CommandJournal::queued(int dx, int dy, Calibration c, Time now)
{
    c = sanitize(c);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = ++next_id_;
    commands_.push_back({id, now, 0, 0, c.effect_delay_ns, c.effect_uncertainty_ns,
        {dx * c.pixels_per_count_x, dy * c.pixels_per_count_y}, dx, dy, Delivery::queued});
    // Longer than every supported observation/actuator delay, bounded in memory.
    while (commands_.size() > 2048 || (!commands_.empty() && commands_.front().state != Delivery::queued
           && now - commands_.front().issued > 1'000'000'000)) commands_.pop_front();
    return id;
}
void CommandJournal::complete(uint64_t id, bool sent, Time now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : commands_) if (c.id == id)
    {
        // A write that won a race with cancellation must still be accounted for.
        c.state = sent ? Delivery::sent : Delivery::failed;
        c.sent = now; c.effect = now + c.delay;
        break;
    }
}
void CommandJournal::cancel(uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : commands_) if (c.id == id && c.state == Delivery::queued)
        c.state = Delivery::cancelled;
}
void CommandJournal::cancelQueued()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : commands_) if (c.state == Delivery::queued) c.state = Delivery::cancelled;
}
Vec CommandJournal::displacement(Time from, Time to, Time now, bool include_queued) const
{
    Vec sum;
    if (to <= from) return sum;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : commands_)
    {
        Time effect = c.effect;
        if (c.state == Delivery::queued && include_queued)
            effect = std::max(c.issued, now) + c.delay;
        else if (c.state != Delivery::sent) continue;
        if (effect > from && effect <= to) { sum.x += c.pixels.x; sum.y += c.pixels.y; }
    }
    return sum;
}
Vec CommandJournal::timingVariance(Time from, Time to) const
{
    Vec variance;
    if (to <= from) return variance;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : commands_)
    {
        if (c.state != Delivery::sent || c.uncertainty <= 0) continue;
        if (std::abs(c.effect-from) <= c.uncertainty || std::abs(c.effect-to) <= c.uncertainty)
        {
            // At a sample boundary, the camera may include all or none of this
            // command. Do not teach that timing ambiguity as target acceleration.
            variance.x += .25 * c.pixels.x * c.pixels.x;
            variance.y += .25 * c.pixels.y * c.pixels.y;
        }
    }
    return variance;
}
std::deque<Command> CommandJournal::snapshot() const
{ std::lock_guard<std::mutex> lock(mutex_); return commands_; }

std::array<int, 2> OutputMapper::map(Vec p, Calibration c, int lx, int ly)
{
    if (!finite(p)) { reset(); return {}; }
    c = sanitize(c);
    auto axis = [](double pixels, double gain, double& residual, int limit) {
        const double requested = std::clamp(pixels / gain, -30000.0, 30000.0) + residual;
        const double rounded = std::nearbyint(requested);
        const int cap = limit > 0 ? std::min(limit, 30000) : 30000;
        const int accepted = static_cast<int>(std::clamp(rounded, -double(cap), double(cap)));
        // Saturation is not an output debt: feedback replans from actual motion.
        residual = accepted == rounded ? requested - rounded : 0.0;
        return accepted;
    };
    return {axis(p.x, c.pixels_per_count_x, residual_.x, lx),
            axis(p.y, c.pixels_per_count_y, residual_.y, ly)};
}

PredictiveController::PredictiveController(std::shared_ptr<CommandJournal> j)
    : journal_(j ? std::move(j) : std::make_shared<CommandJournal>()) {}
void PredictiveController::reset()
{ journal_->cancelQueued(); axes_ = {}; observed_at_ = arrived_at_ = last_control_ = next_control_ = 0; sequence_ = 0; phase_ = Phase::acquire; }
void PredictiveController::configure(ControlConfig c)
{
    c.calibration = sanitize(c.calibration);
    c.period_ns = std::clamp<Time>(c.period_ns, 4'000'000, 33'333'333);
    c.max_age_ns = std::clamp<Time>(c.max_age_ns, 20'000'000, 200'000'000);
    c.kp = {finiteClamp(c.kp.x, 0, 8, 2), finiteClamp(c.kp.y, 0, 8, 2)};
    c.kd = {finiteClamp(c.kd.x, 0, .25, .05), finiteClamp(c.kd.y, 0, .25, .05)};
    c.kf = {finiteClamp(c.kf.x, 0, 2, 1), finiteClamp(c.kf.y, 0, 2, 1)};
    c.deadzone = {finiteClamp(c.deadzone.x, 0, 1000, 0), finiteClamp(c.deadzone.y, 0, 1000, 0)};
    config_ = c;
}
bool PredictiveController::valid(Time now) const
{ return observed_at_ > 0 && now >= observed_at_ && now - observed_at_ <= config_.max_age_ns; }
void PredictiveController::updateAxis(Axis& a, double z, double moved, double dt, double noise, double learning)
{
    const double ratio = finiteClamp(learning / .08, .1, 4.0, 1.0);
    const double dt2 = dt * dt;
    const std::array<double, 9> transition{1,dt,.5*dt2, 0,1,dt, 0,0,1};
    const std::array<double, 3> jerk{dt*dt2/6, .5*dt2, dt};
    std::array<double, 9> prior{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
        {
            for (int k = 0; k < 3; ++k)
                for (int l = 0; l < 3; ++l)
                    prior[row*3+col] += transition[row*3+k]
                        * a.covariance[k*3+l] * transition[col*3+l];
            prior[row*3+col] += kJerkVariance * ratio * ratio * jerk[row] * jerk[col];
        }

    a.p += a.v * dt + .5 * a.acceleration * dt2 - moved;
    a.v += a.acceleration * dt;
    const double innovation = z - a.p;
    // 检测框"位置在跳、面积不变"时的抖动特征: 新息逐帧变号。真实换向/急停只会
    // 变一次号然后持续同号, 所以统计"连续变号次数"就能把两者分开。
    // 注意[不要]用新息幅度去抬高观测方差: 目标真实的急停/换向同样产生大幅度新息,
    // 抬高方差会把本该做的位置吸附一起平滑掉 —— 实测那样改会让
    // continuous_tracking_test 的 reverse 场景 p95 从 4.35px 退化到 14.75px。
    const double prior_innovation = a.prev_innovation;
    a.prev_innovation = innovation;
    if (prior_innovation != 0.0 && innovation * prior_innovation < 0.0) ++a.flip_run;
    else a.flip_run = 0;
    const bool jitter_like = a.flip_run >= kJitterFlipRun;
    const double variance = prior[0] + noise;
    // Correct the interval velocity for our own camera motion. A significant
    // model break gets a causal velocity estimate immediately; carrying the old
    // acceleration through a stop/reversal would create a second overshoot.
    const double measured_velocity = std::clamp(
        (z - a.last_observation + moved) / dt, -kVelocityLimit, kVelocityLimit);
    a.last_observation = z;
    if (innovation * innovation > kPositionJumpInnovationSquared * variance)
    {
        // A discontinuous anchor/camera displacement is not evidence of a
        // sustained extreme velocity. Correct position without extrapolating
        // the one-frame displacement into a large second movement.
        a.p = z;
        a.v = std::clamp(a.v, -kVelocityLimit, kVelocityLimit);
        a.acceleration = 0;
        a.maneuver_hold_sec = kManeuverHoldSec;
        a.covariance = {noise,0,0, 0,std::min(1e8,prior[4]),0, 0,0,1e8};
        return;
    }
    if (innovation * innovation > kManeuverInnovationSquared * variance)
    {
        a.p = z;
        // 机动分支的速度处理见上面的 jitter_like 判别: 连续变号(检测框在抖)时
        // 速度归零且不放大速度协方差; 只变一次号时按原逻辑取因果速度估计,
        // 所以真实换向/急停的行为与改动前一致。
        if (jitter_like)
        {
            // 连续变号 = 检测框在抖。这里既不把单帧位移当成速度, 也不能放大速度
            // 协方差: 协方差一旦被放大, 随后正常路径的速度增益会变成约 1/dt
            // (实测 gain[1]≈120), 把 ±2.5px 的抖动直接灌成几百 px/s 的速度, 而且
            // 每帧重来一次 —— 那正是"锚点附近永不收敛"的极限环来源。
            // 速度归零后 |v| < sqrt(cov) 仍成立, 所以抖动也不会被外推出去。
            a.v = 0.0;
        }
        else
        {
            // 只变一次号: 按原逻辑当成真实换向/急停的因果速度估计。
            a.v = measured_velocity;
        }
        a.acceleration = 0;
        a.maneuver_hold_sec = kManeuverHoldSec;
        const double velocity_variance = jitter_like
            ? std::min(1e8, noise)
            : std::min(1e8, 2 * noise / dt2);
        a.covariance = {noise,0,0, 0,velocity_variance,0, 0,0,1e8};
        return;
    }

    const std::array<double, 3> gain{prior[0]/variance, prior[3]/variance, prior[6]/variance};
    a.p += gain[0] * innovation;
    a.v = std::clamp(a.v + gain[1]*innovation, -kVelocityLimit, kVelocityLimit);
    a.acceleration = std::clamp(a.acceleration + gain[2]*innovation,
                                -kAccelerationLimit, kAccelerationLimit);
    if (a.maneuver_hold_sec > 0)
    {
        a.acceleration = 0;
        a.maneuver_hold_sec = std::max(0.0, a.maneuver_hold_sec - dt);
    }

    // Joseph form preserves a positive covariance through small/noisy time
    // intervals. That covariance also decides whether stationary motion is real.
    std::array<double, 9> correction{1,0,0, 0,1,0, 0,0,1};
    for (int row = 0; row < 3; ++row) correction[row*3] -= gain[row];
    std::array<double, 9> posterior{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
        {
            for (int k = 0; k < 3; ++k)
                for (int l = 0; l < 3; ++l)
                    posterior[row*3+col] += correction[row*3+k] * prior[k*3+l] * correction[col*3+l];
            posterior[row*3+col] += noise * gain[row] * gain[col];
        }
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            a.covariance[row*3+col] = row == col
                ? std::max(kCovarianceFloor, posterior[row*3+col])
                : .5 * (posterior[row*3+col] + posterior[col*3+row]);

}
bool PredictiveController::observe(const Observation& o, Time now)
{
    if (!finite(o.anchor) || o.captured <= 0 || o.captured > now
        || now - o.captured > config_.max_age_ns || o.captured <= observed_at_
        || (sequence_ && o.sequence <= sequence_)) return false;
    const double noise = finiteClamp(o.variance, .25, 100, 1);
    if (!observed_at_ || o.captured - observed_at_ > config_.max_age_ns)
    {
        axes_ = {};
        axes_[0].p = axes_[0].last_observation = o.anchor.x;
        axes_[1].p = axes_[1].last_observation = o.anchor.y;
        axes_[0].covariance[0] = axes_[1].covariance[0] = noise;
        last_control_ = next_control_ = 0; phase_ = Phase::acquire;
    }
    else
    {
        const double dt = (o.captured - observed_at_) * 1e-9;
        const Vec moved = journal_->displacement(observed_at_, o.captured, now);
        const Vec timing_noise = journal_->timingVariance(observed_at_, o.captured);
        updateAxis(axes_[0], o.anchor.x, moved.x, dt, noise+timing_noise.x, config_.learning.x);
        updateAxis(axes_[1], o.anchor.y, moved.y, dt, noise+timing_noise.y, config_.learning.y);
    }
    observed_at_ = o.captured; arrived_at_ = now; sequence_ = o.sequence;
    return true;
}
ControlOutput PredictiveController::advance(Time now, Vec reference)
{
    ControlOutput out;
    if (!valid(now) || !finite(reference)) return out;
    out.valid = true; out.observed_at = observed_at_;
    const Time effective = now + config_.calibration.effect_delay_ns;
    const double horizon = (effective - observed_at_) * 1e-9;
    const auto supportedMotion = [](const Axis& a) {
        const bool stationary = std::abs(a.v) < std::sqrt(a.covariance[4])
            && std::abs(a.acceleration) < std::sqrt(a.covariance[8]);
        return stationary ? std::array<double, 2>{0,0}
                          : std::array<double, 2>{a.v,a.acceleration};
    };
    const auto motion_x = supportedMotion(axes_[0]);
    const auto motion_y = supportedMotion(axes_[1]);
    const Vec moved = journal_->displacement(observed_at_, effective, now, true);
    out.predicted_anchor = {
        axes_[0].p + motion_x[0]*horizon + .5*motion_x[1]*horizon*horizon - moved.x,
        axes_[1].p + motion_y[0]*horizon + .5*motion_y[1]*horizon*horizon - moved.y};
    out.velocity = {motion_x[0] + motion_x[1]*horizon, motion_y[0] + motion_y[1]*horizon};
    if (next_control_ && now < next_control_) return out;
    const double dt = (last_control_ ? std::clamp(now-last_control_, config_.period_ns/2, config_.period_ns*2)
                                    : config_.period_ns) * 1e-9;
    next_control_ = next_control_ ? next_control_ + config_.period_ns : now + config_.period_ns;
    if (next_control_ <= now) next_control_ = now + config_.period_ns;
    last_control_ = now; out.due = true;
    const Vec e{out.predicted_anchor.x - reference.x, out.predicted_anchor.y - reference.y};
    const double distance = std::hypot(e.x, e.y);
    if (distance > 24) phase_ = Phase::acquire;
    else if (phase_ == Phase::acquire || (phase_ == Phase::track && distance > 5)) phase_ = Phase::settle;
    else if (distance < 2.5) phase_ = Phase::track;
    out.phase = phase_;
    const double phase_weight = phase_ == Phase::acquire ? 4.0 : phase_ == Phase::settle ? 3.0 : 2.0;
    const double freshness = std::clamp(double(config_.max_age_ns - (now-arrived_at_)) /
                                       double(config_.max_age_ns / 2), 0.0, 1.0);
    const Vec extra_pending = journal_->displacement(
        effective, effective + static_cast<Time>(dt * .5e9), now, true);
    auto axis = [&](double error, double velocity, double acceleration, double pending,
                    double kp, double kd, double kf, double deadzone, double pixel_step) {
        // Below half a physical count, correcting an unsupported stationary
        // estimate only creates a quantization limit cycle. Slow genuine motion
        // is still observed and will cross this smallest actionable error band.
        const double rest_band = std::max(deadzone, .5 * pixel_step);
        if (velocity == 0 && acceleration == 0 && std::abs(error) <= rest_band) return 0.0;
        const double position_weight = kp * phase_weight;
        const double gain = position_weight / (position_weight + 1.0 + 20.0*kd);
        const double feedforward = (velocity*dt + .5*acceleration*dt*dt) * kf;
        // Minimize position error in the middle of the upcoming sample-and-hold
        // interval, with a penalty for departing from target velocity. This
        // replaces full-cycle prepayment + slow correction of its constant bias.
        const double midpoint_error = error + (velocity*.5*dt + acceleration*.125*dt*dt)*kf - pending;
        const double pixels = gain * midpoint_error + (1.0-gain) * feedforward;
        return std::clamp(pixels * freshness, -30000.0, 30000.0);
    };
    out.pixels = {
        axis(e.x, out.velocity.x, motion_x[1], extra_pending.x, config_.kp.x, config_.kd.x,
             config_.kf.x, config_.deadzone.x, config_.calibration.pixels_per_count_x),
        axis(e.y, out.velocity.y, motion_y[1], extra_pending.y, config_.kp.y, config_.kd.y,
             config_.kf.y, config_.deadzone.y, config_.calibration.pixels_per_count_y)};
    return out;
}
}
