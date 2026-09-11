#pragma once
#include "mouse/control/predictive_controller.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace tracking_test
{
using namespace motion;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kNames[] = {"constant", "smooth_speed", "stop", "reverse", "acceleration",
                                "jump", "circle", "stationary", "position_step"};
struct Environment
{
    int output_hz = 120, measurement_ms = 16;
    double physical_gain = 1;
    int camera_hz = 120;
    double noise = .2;
    bool jitter = false;
    double model_gain = 1;
    int response_offset_ms = 0;
    double speed_scale = 1;
};
struct Metrics
{
    double p95 = 0, peak = 0, within_three_percent = 0, error_integral = 0;
    double max_command = 0;
    int commands = 0;
};
inline Vec target(int scenario, double t)
{
    if (scenario == 7) return {};
    if (t <= 1) return {300*t, 0};
    const double s = t-1;
    switch (scenario)
    {
    case 0: return {300*t, 0};
    case 1: return {300+300*s+300/(4*kPi)*(1-std::cos(4*kPi*s)), 0};
    case 2: return {300, 0};
    case 3: return {300-300*s, 0};
    case 4: return {s < .3 ? 300+300*s+1000*s*s : 480+900*(s-.3), 0};
    case 5: return {300, s < .5 ? 400*s-800*s*s : 0};
    case 6: return {300+300/(4*kPi)*std::sin(4*kPi*s), 300/(4*kPi)*(1-std::cos(4*kPi*s))};
    case 8: return {300*t+25, 0};
    default: return {};
    }
}
inline Metrics simulate(int scenario, Environment env = Environment{})
{
    struct Frame { Time capture, due; Vec anchor; uint64_t sequence; };
    struct PhysicalMove { Time effect; Vec pixels; };
    auto journal = std::make_shared<CommandJournal>();
    PredictiveController controller(journal);
    ControlConfig config;
    config.period_ns = 1'000'000'000LL / env.output_hz;
    config.calibration = {env.model_gain, env.model_gain, 5'000'000};
    controller.configure(config);
    OutputMapper mapper;
    const Time start = 1'000'000'000;
    Time next_frame = start;
    uint64_t sequence = 0;
    Vec camera;
    std::deque<Frame> frames;
    std::deque<PhysicalMove> physical_moves;
    std::vector<double> errors;
    Metrics result;
    for (int tick = 0; tick < 6000; ++tick)
    {
        const double seconds = tick * .0005;
        const Time now = start + tick * 500'000LL;
        // Physical truth has its own actuator queue, scale and timing. Never
        // derive ground truth from the controller's CommandJournal prediction.
        while (!physical_moves.empty() && physical_moves.front().effect <= now)
        {
            camera.x += physical_moves.front().pixels.x;
            camera.y += physical_moves.front().pixels.y;
            physical_moves.pop_front();
        }
        Vec world = target(scenario, seconds);
        world.x *= env.speed_scale; world.y *= env.speed_scale;
        const Vec error{world.x-camera.x, world.y-camera.y};
        if (now >= next_frame)
        {
            ++sequence;
            const double nx = env.noise * std::sin(sequence * 2.3999632297);
            const double ny = env.noise * std::cos(sequence * 1.61803398875);
            const Time jitter = env.jitter ? (int(sequence%5)-2) * 500'000LL : 0;
            // High-quality continuous detections: small noise and one missing
            // observation per 120 frames, including near maneuver onset.
            if (sequence % 120)
                frames.push_back({now, now+env.measurement_ms*1'000'000LL+jitter,
                                  {error.x+nx, error.y+ny}, sequence});
            next_frame += 1'000'000'000LL / env.camera_hz;
        }
        while (!frames.empty() && frames.front().due <= now)
        {
            const auto f = frames.front(); frames.pop_front();
            controller.observe({f.sequence, f.capture, f.anchor, std::max(1.0,env.noise*env.noise)}, now);
        }
        const auto output = controller.advance(now, {});
        if (output.valid && output.due)
        {
            const auto counts = mapper.map(output.pixels, config.calibration, 0, 0);
            if (counts[0] || counts[1])
            {
                const auto id = journal->queued(counts[0], counts[1], config.calibration, now);
                journal->complete(id, true, now);
                physical_moves.push_back({now+(5+env.response_offset_ms)*1'000'000LL,
                                          {counts[0]*env.physical_gain, counts[1]*env.physical_gain}});
                if (seconds >= 1 && seconds < 1.5)
                {
                    ++result.commands;
                    result.max_command = std::max(result.max_command, std::hypot(double(counts[0]),double(counts[1])));
                }
            }
        }
        // Score the maneuver itself, including its first transient. This is
        // deliberately not a tail-only settle test after the target stops.
        if (seconds >= 1 && seconds < 1.5)
        {
            const double e = std::hypot(error.x,error.y);
            errors.push_back(e);
            result.error_integral += e*.5;
        }
    }
    std::sort(errors.begin(),errors.end());
    result.p95 = errors[errors.size()*95/100];
    result.peak = errors.back();
    result.within_three_percent = 100.0 * (std::upper_bound(errors.begin(),errors.end(),3.)-errors.begin()) / errors.size();
    return result;
}
}
