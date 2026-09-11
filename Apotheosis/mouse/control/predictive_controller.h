#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace motion
{
using Time = int64_t; // steady clock nanoseconds throughout
struct Vec { double x = 0, y = 0; };
struct Calibration
{
    double pixels_per_count_x = 1.0, pixels_per_count_y = 1.0;
    Time effect_delay_ns = 8'333'333; // estimate, not a hardware acknowledgement
    Time effect_uncertainty_ns = 2'000'000; // uncertainty around that estimate
};
enum class Delivery { queued, sent, failed, cancelled };
struct Command
{
    uint64_t id = 0;
    Time issued = 0, sent = 0, effect = 0, delay = 0, uncertainty = 0;
    Vec pixels;
    int dx = 0, dy = 0;
    Delivery state = Delivery::queued;
};

// The actuator owns this journal. Stored pixel deltas are the FINAL quantized
// commands, after path shaping and calibration, never the controller's wishes.
class CommandJournal
{
public:
    uint64_t queued(int dx, int dy, Calibration calibration, Time now);
    void complete(uint64_t id, bool sent, Time now);
    void cancel(uint64_t id);
    void cancelQueued();
    Vec displacement(Time from, Time to, Time now, bool include_queued = false) const;
    std::deque<Command> snapshot() const;
    Vec timingVariance(Time from, Time to) const;
private:
    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    uint64_t next_id_ = 0;
};

// One rounding boundary, immediately before the actuator accepts a command.
class OutputMapper
{
public:
    std::array<int, 2> map(Vec pixels, Calibration calibration, int limit_x, int limit_y);
    void reset() { residual_ = {}; }
private:
    Vec residual_;
};

struct Observation
{
    uint64_t sequence = 0;
    Time captured = 0;
    Vec anchor;
    double variance = 1.0;
};
struct ControlConfig
{
    Vec kp{2, 2}, kd{.05, .05}, kf{1, 1};
    Vec deadzone;
    Vec learning{.08,.08};
    Time period_ns = 8'333'333;
    Time max_age_ns = 120'000'000;
    Calibration calibration;
};
enum class Phase { acquire, settle, track };
struct ControlOutput
{
    bool valid = false, due = false;
    Vec pixels, predicted_anchor, velocity;
    Phase phase = Phase::acquire;
    Time observed_at = 0;
};

class PredictiveController
{
public:
    explicit PredictiveController(std::shared_ptr<CommandJournal> journal = nullptr);
    void reset();
    void configure(ControlConfig config);
    bool observe(const Observation& observation, Time now);
    ControlOutput advance(Time now, Vec reference);
    bool valid(Time now) const;
    Vec velocity() const { return {axes_[0].v, axes_[1].v}; }
private:
    struct Axis
    {
        double p = 0, v = 0, acceleration = 0;
        double last_observation = 0, maneuver_hold_sec = 0;
        // Position / velocity / acceleration covariance at the capture time.
        std::array<double, 9> covariance{1,0,0, 0,1e5,0, 0,0,1e8};
        // 抖动判别(见 updateAxis): prev_innovation 记上一帧新息, flip_run 记
        // "新息连续变号"的次数。检测框位置抖动会连续变号, 真实换向只变一次。
        // reset() 会随 axes_ 一起清零。
        double prev_innovation = 0.0;
        int flip_run = 0;
    };
    void updateAxis(Axis& a, double observed, double movement, double dt, double noise, double learning);
    std::shared_ptr<CommandJournal> journal_;
    ControlConfig config_;
    std::array<Axis, 2> axes_{};
    Time observed_at_ = 0, arrived_at_ = 0, last_control_ = 0, next_control_ = 0;
    uint64_t sequence_ = 0;
    Phase phase_ = Phase::acquire;
};
}
