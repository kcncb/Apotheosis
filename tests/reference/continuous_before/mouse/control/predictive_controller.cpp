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
bool finite(Vec v) { return std::isfinite(v.x) && std::isfinite(v.y); }
Calibration sanitize(Calibration c)
{
    c.pixels_per_count_x = finiteClamp(c.pixels_per_count_x, .01, 100, 1);
    c.pixels_per_count_y = finiteClamp(c.pixels_per_count_y, .01, 100, 1);
    c.effect_delay_ns = std::clamp<Time>(c.effect_delay_ns, 0, 100'000'000);
    return c;
}
}
uint64_t CommandJournal::queued(int dx, int dy, Calibration c, Time now)
{
    c = sanitize(c);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = ++next_id_;
    commands_.push_back({id, now, 0, 0, c.effect_delay_ns,
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
    const double accelerationVariance = 4'000'000.0 * ratio * ratio;
    a.p += a.v * dt - moved;
    const double dt2 = dt * dt;
    double p00 = a.p00 + 2 * dt * a.p01 + dt2 * a.p11 + accelerationVariance * dt2 * dt2 / 4;
    double p01 = a.p01 + dt * a.p11 + accelerationVariance * dt2 * dt / 2;
    double p11 = a.p11 + accelerationVariance * dt2;
    const double innovation = z - a.p;
    const double variance = p00 + noise;
    const double k0 = p00 / variance, k1 = p01 / variance;
    a.p += k0 * innovation;
    a.v = std::clamp(a.v + k1 * innovation, -3000.0, 3000.0);
    a.p00 = std::max(1e-9, (1 - k0) * p00);
    a.p01 = (1 - k0) * p01;
    a.p11 = std::max(1e-9, p11 - k1 * p01);
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
        axes_[0].p = o.anchor.x; axes_[1].p = o.anchor.y;
        axes_[0].p00 = axes_[1].p00 = noise;
        last_control_ = next_control_ = 0; phase_ = Phase::acquire;
    }
    else
    {
        const double dt = (o.captured - observed_at_) * 1e-9;
        const Vec moved = journal_->displacement(observed_at_, o.captured, now);
        updateAxis(axes_[0], o.anchor.x, moved.x, dt, noise, config_.learning.x);
        updateAxis(axes_[1], o.anchor.y, moved.y, dt, noise, config_.learning.y);
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
    const Vec moved = journal_->displacement(observed_at_, effective, now, true);
    out.predicted_anchor = {axes_[0].p + axes_[0].v * horizon - moved.x,
                            axes_[1].p + axes_[1].v * horizon - moved.y};
    out.velocity = velocity();
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
    const Vec executed = journal_->displacement(now - config_.period_ns, now, now);
    const double phaseRate = phase_ == Phase::acquire ? 50.0 : phase_ == Phase::settle ? 28.0 : 16.0;
    auto axis = [&](double error, double v, double move, double kp, double kd, double kf, double deadzone) {
        const double correction = error * (1.0 - std::exp(-phaseRate * (kp / 2.0) * dt));
        const double relative = v - move / dt;
        double damping = relative * dt * (kd * 4.0);
        damping = correction * damping < 0 ? std::clamp(damping, -std::abs(correction)*.5, std::abs(correction)*.5) : 0;
        if (std::abs(error) <= deadzone && std::abs(v) < 2.0) return 0.0;
        // Fade predictions only when observations grow old, not merely on entry
        // into the dead zone. Moving targets still require velocity feedforward.
        const double freshness = std::clamp(double(config_.max_age_ns - (now-arrived_at_)) /
                                           double(config_.max_age_ns / 2), 0.0, 1.0);
        return std::clamp((v * dt * kf + correction + damping) * freshness, -30000.0, 30000.0);
    };
    out.pixels = {axis(e.x, axes_[0].v, executed.x, config_.kp.x, config_.kd.x, config_.kf.x, config_.deadzone.x),
                  axis(e.y, axes_[1].v, executed.y, config_.kp.y, config_.kd.y, config_.kf.y, config_.deadzone.y)};
    return out;
}
}
