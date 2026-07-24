#include "pidf_mode2_exact.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cvm::recovered {
namespace {

double signum(double value) noexcept {
    return value > 0.0 ? 1.0 : (value < 0.0 ? -1.0 : 0.0);
}

void reset_feedforward(PidfMode2State& s) noexcept {
    s.ff_state_x = s.ff_state_y = 0.0;
    s.dynamic_lr_x = s.base_lr_x;
    s.dynamic_lr_y = s.base_lr_y;
    s.correction_accum_x = s.correction_accum_y = 0.0;
    s.ff_previous_error_x = s.ff_previous_error_y = 0.0;
    s.ff_output_x = s.ff_output_y = 0.0;
    s.lr_exp_x = s.lr_exp_y = 0.0;
    s.correction_target_x = s.correction_target_y = 0.0;
    s.ff_error_x = s.ff_error_y = 0.0;
    s.correction_delta_x = s.correction_delta_y = 0.0;
    s.dynamic_weight_x = s.dynamic_weight_y = 0.0;
    s.ff_derivative_x = s.ff_derivative_y = 0.0;
    s.correction_ratio_x = s.correction_ratio_y = 0.0;
    s.predicted_minus_move_x = s.predicted_minus_move_y = 0.0;
    s.dynamic_active_x = s.dynamic_active_y = 0;
}

void update_dynamic_lr(PidfMode2State& s,
                       double radius,
                       double dt) noexcept {
    const double denominator = -(radius + 0.000001);

    const double state_step_x = dt * s.ff_state_x;
    s.ff_output_x = state_step_x;
    s.dynamic_active_x = std::fabs(state_step_x) > 0.000001;
    const double z_x = signum(state_step_x) * s.corrected_error_x / denominator;
    s.lr_exp_x = 0.5 * std::exp(-(z_x * z_x));
    s.dynamic_weight_x = s.lr_exp_x - std::tanh(z_x) / 6.0 + 0.5;
    if (!s.dynamic_active_x)
        s.dynamic_weight_x = 1.0;
    s.dynamic_lr_x = s.dynamic_weight_x * s.base_lr_x;

    const double state_step_y = dt * s.ff_state_y;
    s.ff_output_y = state_step_y;
    s.dynamic_active_y = std::fabs(state_step_y) > 0.000001;
    const double z_y = signum(state_step_y) * s.corrected_error_y / denominator;
    s.lr_exp_y = 0.5 * std::exp(-(z_y * z_y));
    s.dynamic_weight_y = s.lr_exp_y - std::tanh(z_y) / 6.0 + 0.5;
    if (!s.dynamic_active_y)
        s.dynamic_weight_y = 1.0;
    s.dynamic_lr_y = s.dynamic_weight_y * s.base_lr_y;
}

void apply_high_kf_correction(PidfMode2State& s,
                              double dt,
                              double radius,
                              double smoothing_radius) noexcept {
    if (s.kf_high_x == 0.0 && s.kf_high_y == 0.0) {
        s.correction_delta_x = s.correction_delta_y = 0.0;
        s.correction_accum_x = s.correction_accum_y = 0.0;
        return;
    }

    const double state_step_x = s.ff_state_x * dt;
    const double state_step_y = s.ff_state_y * dt;
    s.ff_output_x = state_step_x;
    s.ff_output_y = state_step_y;

    s.correction_ratio_x = std::tanh(std::fabs(state_step_x) / smoothing_radius);
    s.correction_target_x = signum(s.ff_state_x) * radius;
    const double blended_x =
        (s.correction_target_x - state_step_x) * s.correction_ratio_x
        + state_step_x;
    s.correction_delta_x =
        (blended_x * s.kf_high_x - s.correction_accum_x) * s.dynamic_lr_x;
    s.correction_accum_x += s.correction_delta_x;

    s.correction_ratio_y = std::tanh(std::fabs(state_step_y) / smoothing_radius);
    s.correction_target_y = signum(s.ff_state_y) * radius;
    const double blended_y =
        (s.correction_target_y - state_step_y) * s.correction_ratio_y
        + state_step_y;
    s.correction_delta_y =
        (blended_y * s.kf_high_y - s.correction_accum_y) * s.dynamic_lr_y;
    s.correction_accum_y += s.correction_delta_y;
}

void update_low_kf_feedforward(PidfMode2State& s, double dt) noexcept {
    s.predicted_minus_move_x =
        s.ff_state_x * dt + s.ff_previous_error_x - s.previous_move_x;
    s.ff_error_x = s.corrected_error_x - s.predicted_minus_move_x;
    s.ff_derivative_x = s.ff_error_x / dt * s.dynamic_lr_x;
    s.ff_state_x += s.ff_derivative_x;
    s.ff_previous_error_x = s.corrected_error_x;
    s.ff_output_x = s.ff_state_x * dt * s.kf_low_x;

    s.predicted_minus_move_y =
        s.ff_state_y * dt + s.ff_previous_error_y - s.previous_move_y;
    s.ff_error_y = s.corrected_error_y - s.predicted_minus_move_y;
    s.ff_derivative_y = s.ff_error_y / dt * s.dynamic_lr_y;
    s.ff_state_y += s.ff_derivative_y;
    s.ff_previous_error_y = s.corrected_error_y;
    s.ff_output_y = s.ff_state_y * dt * s.kf_low_y;
}

std::int32_t quantize(double step,
                      double& residual,
                      double& rounded,
                      std::uint8_t& nonzero,
                      bool blocked) noexcept {
    if (blocked) {
        step = 0.0;
        residual = 0.0;
    }
    residual += step;
    rounded = std::rint(residual);
    const auto result = static_cast<std::int32_t>(rounded);
    nonzero = result != 0;
    if (result)
        residual -= static_cast<double>(result);
    return result;
}

void apply_post_limits(PidfMode2State& s,
                       double original_error_x,
                       double original_error_y) noexcept {
    if (s.config.deadzone_x > 0
        && static_cast<double>(s.config.deadzone_x) >= std::fabs(original_error_x)
        && std::abs(s.move_x) <= 1) {
        s.move_x = 0;
        s.move_nonzero_x = 0;
    }
    if (s.config.deadzone_y > 0
        && static_cast<double>(s.config.deadzone_y) >= std::fabs(original_error_y)
        && std::abs(s.move_y) <= 1) {
        s.move_y = 0;
        s.move_nonzero_y = 0;
    }
    if (s.config.movement_limit_x > 0) {
        s.move_x = std::clamp(
            s.move_x, -s.config.movement_limit_x, s.config.movement_limit_x);
        s.move_nonzero_x = s.move_x != 0;
    }
    if (s.config.movement_limit_y > 0) {
        s.move_y = std::clamp(
            s.move_y, -s.config.movement_limit_y, s.config.movement_limit_y);
        s.move_nonzero_y = s.move_y != 0;
    }
}

void update_post_frame_damping(PidfMode2State& s,
                               double radius,
                               double dt) noexcept {
    s.scratch_x = s.integral_x * s.corrected_error_x;
    s.integral_opposes_error_x = s.scratch_x < 0.0;
    s.scratch_y = s.integral_y * s.corrected_error_y;
    s.integral_opposes_error_y = s.scratch_y < 0.0;

    // sub_140048560 forwards the frame delta (not the target radius) to
    // sub_140047E60 when testing whether feed-forward opposes the error.
    s.ff_output_x = dt * s.ff_state_x;
    s.ff_output_y = dt * s.ff_state_y;
    s.scratch_x = s.ff_output_x * s.corrected_error_x;
    s.ff_opposes_error_x = s.scratch_x < 0.0;
    s.scratch_y = s.ff_output_y * s.corrected_error_y;
    s.ff_opposes_error_y = s.scratch_y < 0.0;
    s.damp_x = s.integral_opposes_error_x || s.ff_opposes_error_x;
    s.damp_y = s.integral_opposes_error_y || s.ff_opposes_error_y;

    s.attenuation_x = std::clamp(
        1.0 - std::tanh(s.absolute_error_x / (radius + 0.000001) * 0.5),
        0.0,
        1.0);
    s.attenuation_y = std::clamp(
        1.0 - std::tanh(s.absolute_error_y / (radius + 0.000001) * 0.5),
        0.0,
        1.0);
    if (s.damp_x) {
        s.integral_x *= s.attenuation_x;
        s.ff_state_x *= s.attenuation_x;
    }
    if (s.damp_y) {
        s.integral_y *= s.attenuation_y;
        s.ff_state_y *= s.attenuation_y;
    }
}

} // namespace

void reset_pidf_mode2(PidfMode2State& s, double now_seconds) noexcept {
    s.previous_timestamp = now_seconds;
    s.previous_error_x = s.previous_error_y = 0.0;
    s.integral_x = s.integral_y = 0.0;
    reset_feedforward(s);
    s.integral_weight_x = s.integral_weight_y = 0.0;
    s.residual_x = s.residual_y = 0.0;
    s.previous_move_x = s.previous_move_y = 0.0;
    s.initialized = 0;
}

PidfMode2State construct_pidf_mode2(const PidfMode2Config& config,
                                    double now_seconds) noexcept {
    PidfMode2State s{};
    s.config = config;
    s.kd_x = config.kd_x;
    s.kd_y = config.kd_y;
    s.kp_x = config.kp_x;
    s.kp_y = config.kp_y;
    s.ki_x = config.ki_x;
    s.ki_y = config.ki_y;
    s.base_lr_x = config.lr_x;
    s.base_lr_y = config.lr_y;
    s.dynamic_lr_x = config.lr_x;
    s.dynamic_lr_y = config.lr_y;
    s.kf_low_x = std::min(config.kf_x, 1.0);
    s.kf_low_y = std::min(config.kf_y, 1.0);
    s.kf_high_x = std::max(config.kf_x - 1.0, 0.0);
    s.kf_high_y = std::max(config.kf_y - 1.0, 0.0);
    reset_pidf_mode2(s, now_seconds);
    return s;
}

PidfNativeOutput update_pidf_mode2(PidfMode2State& s,
                                   const PidfInputExact& input,
                                   double now_seconds) noexcept {
    PidfNativeOutput out{};
    const double dt = now_seconds - s.previous_timestamp;
    if (!input.valid) {
        reset_pidf_mode2(s, now_seconds);
        return out;
    }

    out.original_error_x = input.target_x - input.current_x;
    out.original_error_y = input.target_y - input.current_y;
    s.corrected_error_x = out.original_error_x;
    s.corrected_error_y = out.original_error_y;
    const double radius = std::min(input.radius_x, input.radius_y);

    update_dynamic_lr(s, radius, dt);
    apply_high_kf_correction(s, dt, radius, radius * 0.05 + 0.000001);
    s.corrected_error_x += s.correction_accum_x;
    s.corrected_error_y += s.correction_accum_y;
    out.corrected_error_x = s.corrected_error_x;
    out.corrected_error_y = s.corrected_error_y;

    if (!s.initialized) {
        s.previous_error_x = s.corrected_error_x;
        s.previous_error_y = s.corrected_error_y;
        s.previous_timestamp = now_seconds;
        s.ff_previous_error_x = s.corrected_error_x;
        s.ff_previous_error_y = s.corrected_error_y;
        s.initialized = 1;
        out.initialized_this_frame = 1;
        return out;
    }

    s.absolute_error_x = std::fabs(s.corrected_error_x);
    s.radius_progress_x = std::clamp(
        1.0 - s.absolute_error_x / (radius + 0.000001), 0.0, 1.0);
    s.inside_radius_x = radius >= s.absolute_error_x;
    if (s.inside_radius_x)
        s.integral_weight_x = std::max(s.integral_weight_x, s.radius_progress_x);
    s.far_outside_x = s.absolute_error_x > radius * 1.5;
    if (s.far_outside_x) {
        s.integral_weight_x = 0.0;
        s.integral_x = 0.0;
        s.ff_state_x = 0.0;
    }

    s.absolute_error_y = std::fabs(s.corrected_error_y);
    s.radius_progress_y = std::clamp(
        1.0 - s.absolute_error_y / (radius + 0.000001), 0.0, 1.0);
    s.inside_radius_y = radius >= s.absolute_error_y;
    if (s.inside_radius_y)
        s.integral_weight_y = std::max(s.integral_weight_y, s.radius_progress_y);
    s.far_outside_y = s.absolute_error_y > radius * 1.5;
    if (s.far_outside_y) {
        s.integral_weight_y = 0.0;
        s.integral_y = 0.0;
        s.ff_state_y = 0.0;
    }

    s.integral_x += s.corrected_error_x * dt * s.integral_weight_x;
    s.integral_y += s.corrected_error_y * dt * s.integral_weight_y;
    s.scratch_x = s.kd_x
        * (s.corrected_error_x - s.previous_error_x) / dt;
    s.raw_pid_x = s.kp_x * s.corrected_error_x
                + s.ki_x * s.integral_x + s.scratch_x;
    s.scratch_y = s.kd_y
        * (s.corrected_error_y - s.previous_error_y) / dt;
    s.raw_pid_y = s.kp_y * s.corrected_error_y
                + s.ki_y * s.integral_y + s.scratch_y;

    update_low_kf_feedforward(s, dt);
    double float_step_x = s.raw_pid_x * 0.1
                        + s.ff_output_x * s.integral_weight_x;
    double float_step_y = s.raw_pid_y * 0.1
                        + s.ff_output_y * s.integral_weight_y;
    s.raw_pid_x = float_step_x;
    s.raw_pid_y = float_step_y;
    s.move_x = quantize(
        float_step_x, s.residual_x, s.rounded_x,
        s.move_nonzero_x, s.axis_blocked_x != 0);
    s.move_y = quantize(
        float_step_y, s.residual_y, s.rounded_y,
        s.move_nonzero_y, s.axis_blocked_y != 0);
    apply_post_limits(s, out.original_error_x, out.original_error_y);

    out.dx = s.move_x;
    out.dy = s.move_y;
    out.has_move = out.dx != 0 || out.dy != 0;

    update_post_frame_damping(s, radius, dt);
    s.previous_timestamp = now_seconds;
    s.previous_error_x = s.corrected_error_x;
    s.previous_error_y = s.corrected_error_y;
    s.previous_move_x = static_cast<double>(out.dx);
    s.previous_move_y = static_cast<double>(out.dy);
    return out;
}

} // namespace cvm::recovered
