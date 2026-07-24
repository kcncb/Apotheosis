#include "pidf_update_exact.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {

SplitKf split_pidf_kf(double configured_kf) noexcept {
    return {
        std::min(configured_kf, 1.0),
        std::max(configured_kf - 1.0, 0.0),
    };
}

double dynamic_lr_weight(double feed_forward_state,
                         double dt,
                         double error,
                         double radius) noexcept {
    const double state_step = feed_forward_state * dt;
    if (std::fabs(state_step) <= 1.0e-6)
        return 1.0;

    // The native code applies the sign of state_step and then divides by the
    // negated target radius.  Do not simplify the signs: this is the exact
    // expression recovered from sub_140047990.
    const double z = std::copysign(1.0, state_step) * error
                   / -(radius + 1.0e-6);
    return 0.5 * std::exp(-(z * z)) - std::tanh(z) / 6.0 + 0.5;
}

void reset_pidf_update(PidfUpdateState& state, double now_seconds) noexcept {
    state = {};
    state.previous_timestamp = now_seconds;
}

PidfUpdateOutput update_pidf_exact(
    PidfUpdateState& state,
    const PidfUpdateConfig& config,
    const PidfInputExact& input,
    const PidfAdaptiveFrame& adaptive,
    double now_seconds,
    PidfMode mode,
    const MoveTransform& transform) {
    PidfUpdateOutput output{};

    if (!input.valid) {
        reset_pidf_update(state, now_seconds);
        return output;
    }

    output.original_error_x = input.target_x - input.current_x;
    output.original_error_y = input.target_y - input.current_y;
    output.corrected_error_x = adaptive.corrected_error_x;
    output.corrected_error_y = adaptive.corrected_error_y;

    if (!state.initialized) {
        state.initialized = true;
        state.previous_timestamp = now_seconds;
        state.previous_error_x = adaptive.corrected_error_x;
        state.previous_error_y = adaptive.corrected_error_y;
        output.initialized_this_frame = true;
        return output;
    }

    const double dt = now_seconds - state.previous_timestamp;

    // mode 1 gates each integrator; mode 2's adaptive stage supplies true in
    // normal operation and resets the corresponding integral on overshoot.
    if (adaptive.integral_gate_x)
        state.integral_x += dt * adaptive.corrected_error_x
                          * adaptive.integral_weight_x;
    if (adaptive.integral_gate_y)
        state.integral_y += dt * adaptive.corrected_error_y
                          * adaptive.integral_weight_y;

    state.derivative_x = config.x.kd
        * (adaptive.corrected_error_x - state.previous_error_x) / dt;
    state.derivative_y = config.y.kd
        * (adaptive.corrected_error_y - state.previous_error_y) / dt;

    state.raw_pid_x = config.x.kp * adaptive.corrected_error_x
                    + config.x.ki * state.integral_x
                    + state.derivative_x;
    state.raw_pid_y = config.y.kp * adaptive.corrected_error_y
                    + config.y.ki * state.integral_y
                    + state.derivative_y;

    const double feed_x = adaptive.feed_forward_x
                        * adaptive.integral_weight_x;
    const double feed_y = adaptive.feed_forward_y
                        * adaptive.integral_weight_y;

    double step_scale = 1.0;
    if (mode == PidfMode::mode1) {
        step_scale = 1.0 / std::max(
            static_cast<double>(input.frame_divisor) * 0.25, 1.0);
    }
    state.float_step_x = (state.raw_pid_x * 0.1 + feed_x) * step_scale;
    state.float_step_y = (state.raw_pid_y * 0.1 + feed_y) * step_scale;

    output.move = postprocess_pidf_step(
        state.post,
        config.post,
        output.original_error_x,
        output.original_error_y,
        state.float_step_x,
        state.float_step_y,
        adaptive.block_x,
        adaptive.block_y,
        transform);
    output.has_move = output.move.dx != 0 || output.move.dy != 0;

    state.previous_timestamp = now_seconds;
    state.previous_error_x = adaptive.corrected_error_x;
    state.previous_error_y = adaptive.corrected_error_y;
    return output;
}

} // namespace cvm::recovered
