#include "qx_curve_config_exact.hpp"

#include "humanization_math_exact.hpp"
#include "pid_input_pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

constexpr float kTwoPi = 6.2831855f;
constexpr float kTinyLengthSquared = 0.0000000099999999f;

void limit_vector(float& x, float& y, float limit) noexcept {
    if (limit <= 0.0f) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    const float length_squared = x * x + y * y;
    if (length_squared > limit * limit && length_squared > kTinyLengthSquared) {
        const float scale = limit / std::sqrt(length_squared);
        x *= scale;
        y *= scale;
    }
}

} // namespace

QxCurveOutput step_qx_sigma(QxSigmaState& state,
                            const QxCurveInput& input) noexcept {
    QxCurveOutput output{};
    output.target_x = input.target_x;
    output.target_y = input.target_y;

    const float input_dx = input.target_x - input.current_x;
    const float input_dy = input.target_y - input.current_y;
    const float input_distance_squared = input_dx * input_dx + input_dy * input_dy;
    const float input_distance = std::sqrt(input_distance_squared);
    if (!state.config.qx_curve_enabled || !input.valid
        || input_distance <= 0.000099999997f || !std::isfinite(input_distance)) {
        reset_qx_sigma_runtime(state);
        return output;
    }

    if (!state.initialized || input.target_identity != state.target_identity)
        reset_qx_sigma_for_target(state, input, input_distance);

    const float elapsed_ms = std::clamp(
        finite_or(input.elapsed_ms, 0.0f), 0.0f, 50.0f);
    const float dt = elapsed_ms * 0.001f;
    const float nonnegative_dt = std::max(0.0f, dt);
    const float sqrt_dt = std::sqrt(nonnegative_dt);

    state.progress_velocity = std::clamp(
        state.progress_velocity
            + random_normal(state.rng_state, 0.0f, sqrt_dt * 0.035f)
            - state.progress_velocity * 10.0f * dt,
        -0.18f,
        0.18f);
    const float speed_multiplier = std::clamp(
        state.progress_accumulator + 1.0f + state.progress_velocity,
        0.74f,
        1.26f);

    const float longitudinal_projection =
        input_dx * state.direction_x + input_dy * state.direction_y;
    const float progress = std::clamp(
        (longitudinal_projection - state.longitudinal_distance)
            / std::max(state.target_distance - state.longitudinal_distance, 1.0f),
        0.0f,
        1.0f);
    state.progress_rate = std::clamp(
        state.progress_rate
            + nonnegative_dt * state.config.phase_speed * speed_multiplier,
        0.001f,
        8.0f);

    const float trajectory_strength =
        smoothstep(0.0f, 0.85f, progress) * 0.65f + 0.35f;
    const float primary_cdf = shifted_lognormal_cdf(
        state.progress_rate,
        state.primary_shift,
        state.primary_log_mean,
        state.primary_log_sigma);
    const float primary_pdf = shifted_lognormal_pdf(
        state.progress_rate,
        state.primary_shift,
        state.primary_log_mean,
        state.primary_log_sigma);

    const float secondary_cdf = state.f172 > 0.0f
        ? shifted_lognormal_cdf(
            state.progress_rate,
            state.secondary_shift,
            state.secondary_log_mean,
            state.secondary_log_sigma)
        : 0.0f;
    const float secondary_pdf = state.f172 > 0.0f
        ? shifted_lognormal_pdf(
            state.progress_rate,
            state.secondary_shift,
            state.secondary_log_mean,
            state.secondary_log_sigma)
        : 0.0f;
    const float tertiary_cdf = state.f176 > 0.0f
        ? shifted_lognormal_cdf(
            state.progress_rate,
            state.tertiary_shift,
            state.tertiary_log_mean,
            state.tertiary_log_sigma)
        : 0.0f;
    const float tertiary_pdf = state.f176 > 0.0f
        ? shifted_lognormal_pdf(
            state.progress_rate,
            state.tertiary_shift,
            state.tertiary_log_mean,
            state.tertiary_log_sigma)
        : 0.0f;

    const float correction_cdf = std::clamp(
        secondary_cdf * state.f172 + tertiary_cdf * state.f176,
        0.0f,
        1.0f);
    const float blended_cdf = std::clamp(
        state.primary_mix * primary_cdf
            + (1.0f - state.primary_mix) * correction_cdf,
        0.0f,
        1.0f);
    const float time_scale = normalized_dt(state.config.ou_sigma);
    const float raw_time_scale = std::clamp(
        finite_or(state.config.ou_sigma, 0.025f) / 0.2f,
        0.0f,
        10.0f);
    const float progress_complement = std::clamp(1.0f - progress, 0.0f, 1.0f);
    const float filtered_cdf = std::clamp(
        blended_cdf
            + (progress_complement - blended_cdf) * (time_scale * 0.08f),
        0.0f,
        1.0f);
    const float primary_bell = std::clamp(
        blended_cdf * 4.0f * (1.0f - blended_cdf), 0.0f, 1.0f);
    const float filtered_bell = std::clamp(filtered_cdf, 0.0f, 1.0f);

    const float profile_impulse = std::min(
        filtered_bell * 28.935184f * filtered_bell
            * (1.0f - filtered_bell) * (1.0f - filtered_bell)
            * (1.0f - filtered_bell),
        1.0f) * state.f164 * trajectory_strength;

    float gaussian_tangent = 0.0f;
    float gaussian_normal = 0.0f;
    for (int i = 0; i < state.gaussian_count; ++i) {
        const QxGaussianTerm& term = state.gaussian[static_cast<std::size_t>(i)];
        const float z = (filtered_cdf - term.center) / std::max(0.001f, term.width);
        const float weight = std::exp((-0.5f * z) * z);
        gaussian_normal += weight * term.normal_amplitude;
        gaussian_tangent += weight * term.tangent_amplitude;
    }

    const float phase_dt = std::min(0.02f, dt);
    const float angular_speed = state.phase_speed * kTwoPi;
    state.phase0 += phase_dt * angular_speed;
    state.phase1 += phase_dt * angular_speed * 0.53f;
    const float phase_envelope =
        smoothstep(0.84f, 0.98f, blended_cdf)
        * smoothstep(0.08f, 0.24f, blended_cdf)
        * state.phase_amplitude;
    const float phase_wave =
        std::sin(state.phase0) * 0.7f + std::sin(state.phase1) * 0.3f;
    const float phase_component = phase_envelope * phase_wave;

    const float secondary_unit = std::clamp(secondary_cdf, 0.0f, 1.0f);
    const float tertiary_unit = std::clamp(tertiary_cdf, 0.0f, 1.0f);
    const float correction_curve = (
        secondary_unit * 4.0f * (1.0f - secondary_unit) * state.f180
        + tertiary_unit * 4.0f * (1.0f - tertiary_unit) * state.f184)
        * trajectory_strength;
    const float longitudinal_correction = (
        (1.0f - state.f168) * correction_cdf
        + (state.f168 - 1.0f) * primary_cdf)
        * (state.f188 * state.target_distance)
        * trajectory_strength;

    const float oscillator_sigma =
        state.config.tremor_amp * state.noise_scale * sqrt_dt;
    state.oscillator_x += random_normal(
        state.rng_state, 0.0f, oscillator_sigma)
        - state.oscillator_x * state.oscillator_damping * dt;
    state.oscillator_y += random_normal(
        state.rng_state, 0.0f, oscillator_sigma)
        - state.oscillator_y * state.oscillator_damping * dt;

    float unit_angle_step = state.unit_frequency * kTwoPi * phase_dt;
    rotate_unit_vector(unit_angle_step, state.unit0_y, state.unit0_x);
    rotate_unit_vector(unit_angle_step, state.unit1_y, state.unit1_x);
    const float unit_decay = 1.0f - primary_bell * 0.78f;
    const float unit_x = state.unit_rotation_speed * state.unit0_y * unit_decay;
    const float unit_y = state.unit_rotation_speed * state.unit1_y * unit_decay;

    const float curvature_sigma =
        std::max(
            (secondary_pdf * state.f172 + tertiary_pdf * state.f176)
                    * (1.0f - state.primary_mix)
                + state.primary_mix * primary_pdf,
            0.0f)
        * state.target_distance
        * std::max(speed_multiplier * state.config.phase_speed, 0.0f)
        * 0.001f
        * state.config.curvature_scale;
    const float curvature_x = random_normal(state.rng_state, 0.0f, curvature_sigma);
    const float curvature_y = random_normal(state.rng_state, 0.0f, curvature_sigma);

    const float near_radius = state.config.near_radius >= 0.0f
        ? std::max(state.config.near_radius, 0.0f)
        : std::clamp(state.progress_limit * 0.6f, 4.0f, 18.0f);
    const float far_scale = near_radius <= 0.0f
        ? 1.0f
        : smoothstep(near_radius, near_radius + near_radius, input_distance);
    const float near_decay = 1.0f - far_scale;

    float offset_x =
        state.orthogonal_x * gaussian_tangent * trajectory_strength
        + state.orthogonal_x * profile_impulse
        + state.orthogonal_x * correction_curve
        + state.direction_x * longitudinal_correction
        + state.direction_x * (phase_component + gaussian_normal) * trajectory_strength
        + state.oscillator_x * trajectory_strength
        + unit_x * trajectory_strength
        + curvature_x * trajectory_strength;
    float offset_y =
        state.orthogonal_y * gaussian_tangent * trajectory_strength
        + state.orthogonal_y * profile_impulse
        + state.orthogonal_y * correction_curve
        + state.direction_y * longitudinal_correction
        + state.direction_y * (phase_component + gaussian_normal) * trajectory_strength
        + state.oscillator_y * trajectory_strength
        + unit_y * trajectory_strength
        + curvature_y * trajectory_strength;

    const float near_time_factor = near_radius > 0.0f
        ? smoothstep(0.0f, near_radius, input_distance) * (time_scale * 0.08f)
        : 0.0f;
    const float distance_factor = std::max(near_time_factor, far_scale);
    offset_x *= distance_factor;
    offset_y *= distance_factor;

    const float phase_position = std::clamp(correction_cdf, 0.0f, 1.0f);
    const float phase_z =
        (phase_position - state.phase_damping) / std::max(0.001f, state.phase_noise);
    const float phase_gaussian = std::exp((-0.5f * phase_z) * phase_z);
    const float near_phase_scale = near_radius <= 0.0f
        ? 1.0f
        : near_decay * 0.65f + 0.35f;
    const float phase_tangent = phase_gaussian * state.phase_skew * near_phase_scale;
    const float phase_normal = phase_gaussian * state.phase_offset * near_phase_scale;
    offset_x += state.orthogonal_x * phase_tangent
              + state.direction_x * phase_normal;
    offset_y += state.orthogonal_y * phase_tangent
              + state.direction_y * phase_normal;

    const float dynamic_limit = std::max(
        input_distance * std::clamp(
            std::sqrt(std::max(raw_time_scale, 0.0f)) * 0.08f + 0.12f,
            0.12f,
            0.38f),
        0.75f);
    limit_vector(offset_x, offset_y, dynamic_limit);

    if (state.config.endpoint_error_enabled && state.config.endpoint_error_px > 0.0f) {
        offset_x += near_decay * state.fixed_offset_x;
        offset_y += near_decay * state.fixed_offset_y;
    }

    const float max_offset = std::max(state.config.max_offset_px, 0.0f);
    limit_vector(offset_x, offset_y, max_offset);

    if (max_offset <= 0.0f) {
        state.previous_valid = 0;
        offset_x = 0.0f;
        offset_y = 0.0f;
    } else if (state.previous_valid) {
        float delta_x = offset_x - state.previous_output_x;
        float delta_y = offset_y - state.previous_output_y;
        const float reference_limit = std::min(
            std::max(dynamic_limit, state.config.endpoint_error_px),
            max_offset);
        const float max_delta = std::max(
            reference_limit * (time_scale * 4.0f + 4.0f)
                * std::max(0.001f, dt)
                * (near_decay * 2.25f + 1.0f),
            time_scale * 0.55f + 0.25f);
        limit_vector(delta_x, delta_y, max_delta);
        offset_x = state.previous_output_x + delta_x;
        offset_y = state.previous_output_y + delta_y;
        limit_vector(offset_x, offset_y, max_offset);
    } else {
        state.previous_valid = 1;
    }

    state.previous_output_x = offset_x;
    state.previous_output_y = offset_y;
    output.valid = 1;
    output.target_x = input.target_x + offset_x;
    output.target_y = input.target_y + offset_y;
    return output;
}

} // namespace cvm::recovered
