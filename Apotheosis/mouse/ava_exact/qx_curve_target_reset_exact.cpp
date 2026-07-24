#include "qx_curve_config_exact.hpp"

#include "humanization_math_exact.hpp"
#include "pid_input_pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

constexpr float kTwoPi = 6.2831855f;

float uniform01(QxSigmaState& state) noexcept {
    return qx_uniform24(qx_next_u32(state.rng_state));
}

float uniform(QxSigmaState& state, float low, float high) noexcept {
    return low + uniform01(state) * (high - low);
}

float random_sign(QxSigmaState& state) noexcept {
    return uniform01(state) >= 0.5f ? 1.0f : -1.0f;
}

float clamp_length_component(float value, float limit) noexcept {
    return std::clamp(value, -limit, limit);
}

} // namespace

void reset_qx_sigma_for_target(QxSigmaState& state,
                               const QxCurveInput& input,
                               float distance) noexcept {
    const float target_distance = std::max(1.0f, distance);
    state.initialized = 1;
    state.target_identity = input.target_identity;
    state.target_distance = target_distance;

    const float extent_x = std::max(finite_or(input.target_extent_x, 0.0f), 0.0f);
    const float extent_y = std::max(finite_or(input.target_extent_y, 0.0f), 0.0f);
    float target_width;
    if (extent_x > 0.0f && extent_y > 0.0f)
        target_width = std::min(extent_x, extent_y);
    else
        target_width = std::max(std::max(extent_x, extent_y), 1.0f);

    target_width = std::clamp(
        target_width * state.config.target_width_scale,
        state.config.target_width_min,
        state.config.target_width_max);
    state.progress_limit = target_width;
    state.longitudinal_distance = std::clamp(
        target_width * 0.35f,
        1.0f,
        std::max(1.0f, target_distance * 0.45f));

    const float dx = input.target_x - input.current_x;
    const float dy = input.target_y - input.current_y;
    const float input_distance = std::sqrt(dx * dx + dy * dy);
    float direction_x;
    float direction_y;
    if (input_distance <= 0.000099999997f) {
        direction_x = 1.0f;
        direction_y = 0.0f;
    } else {
        direction_x = dx / input_distance;
        direction_y = dy / input_distance;
        if (std::fabs(direction_y) > std::fabs(direction_x) * 3.0f) {
            direction_x = 0.0f;
            direction_y = direction_y < 0.0f ? -1.0f : 1.0f;
        }
    }
    state.direction_x = direction_x;
    state.direction_y = direction_y;

    const float orthogonal_sign = random_sign(state);
    state.orthogonal_x = -direction_y * orthogonal_sign;
    state.orthogonal_y = direction_x * orthogonal_sign;

    const float absolute_sum = std::max(
        std::fabs(direction_x) + std::fabs(direction_y), 0.000099999997f);
    state.f160 = std::clamp(
        1.0f + (std::fabs(direction_y) / absolute_sum) * 0.15f,
        1.0f,
        1.15f);

    const float width_scale = std::clamp(
        10.0f / std::max(target_width, 4.0f), 0.6f, 1.45f);
    const float log_distance = std::log2(
        target_distance / std::max(target_width, 1.0f) + 1.0f);
    const float time_scale = normalized_dt(state.config.ou_sigma);

    state.progress_accumulator = uniform(state, -0.06f, 0.06f);
    state.progress_velocity = uniform(state, -0.055f, 0.055f);
    state.f168 = 1.0f;
    state.f172 = 0.0f;
    state.f176 = 0.0f;
    state.primary_shift = 0.0f;

    const float distance_sigma = target_distance * state.config.ou_sigma * state.f160;
    state.f164 = distance_sigma * std::clamp(
        random_normal(state.rng_state, 0.0f, 1.0f), -2.5f, 2.5f);
    state.f188 = std::clamp(log_distance * 0.045f + 0.22f, 0.2f, 0.46f);

    state.primary_log_sigma = uniform(state, 0.16f, 0.31f);
    const float primary_sigma = std::max(state.primary_log_sigma, 0.08f);
    const float primary_scale = std::clamp(
        log_distance * 0.18f + 0.7f, 0.7f, 1.6f);
    state.primary_log_mean = std::log(std::max(
        uniform(state, 0.28f, 0.42f) * primary_scale, 0.035f))
        + primary_sigma * primary_sigma;
    state.primary_mix = 1.0f;

    state.secondary_shift = 0.0f;
    state.secondary_log_mean = -0.45f;
    state.secondary_log_sigma = 0.32f;
    state.secondary_mix = 1.0f;
    state.tertiary_shift = 0.0f;
    state.tertiary_log_mean = -0.45f;
    state.tertiary_log_sigma = 0.32f;
    state.tertiary_mix = 1.0f;

    if (state.config.correction_bias_enabled) {
        float correction_ratio;
        if (state.config.overshoot_probability <= uniform01(state))
            correction_ratio = uniform(state, 0.92f, 0.97f);
        else
            correction_ratio = uniform(state, 1.02f, 1.08f);
        state.f168 = correction_ratio;

        const float correction_probability = std::clamp(
            log_distance * 0.12f + 0.3f + width_scale * 0.12f,
            0.35f,
            1.0f);
        float secondary_weight;
        float tertiary_weight;
        if (uniform01(state) >= correction_probability * 0.28f) {
            state.primary_mix = uniform(state, 0.80f, 0.88f);
            secondary_weight = 1.0f;
            tertiary_weight = 0.0f;
        } else {
            state.primary_mix = uniform(state, 0.72f, 0.80f);
            secondary_weight = uniform(state, 0.52f, 0.68f);
            tertiary_weight = 1.0f - secondary_weight;
        }
        state.f172 = secondary_weight;
        state.f176 = tertiary_weight;

        state.secondary_shift = correction_ratio <= 1.0f
            ? uniform(state, 0.44f, 0.58f)
            : uniform(state, 0.54f, 0.66f);
        state.secondary_log_sigma = uniform(state, 0.12f, 0.20f);
        const float secondary_sigma = std::max(state.secondary_log_sigma, 0.08f);
        state.secondary_log_mean = std::log(std::max(
            uniform(state, 0.74f, 0.88f) - state.secondary_shift,
            0.035f)) + secondary_sigma * secondary_sigma;
        state.secondary_mix = secondary_weight;

        state.tertiary_shift = uniform(state, 0.66f, 0.82f);
        state.tertiary_log_sigma = uniform(state, 0.12f, 0.20f);
        const float tertiary_sigma = std::max(state.tertiary_log_sigma, 0.08f);
        state.tertiary_log_mean = std::log(std::max(
            uniform(state, 0.90f, 1.10f) - state.tertiary_shift,
            0.035f)) + tertiary_sigma * tertiary_sigma;
        state.tertiary_mix = tertiary_weight;

        const float curvature_limit = std::max(
            (time_scale * 0.045f + 0.012f) * target_distance, 2.0f);
        const float curvature_sigma = distance_sigma
            * std::clamp(width_scale, 0.75f, 1.35f)
            * (time_scale * 0.85f + 0.35f);
        state.f180 = clamp_length_component(
            random_normal(state.rng_state, 0.0f, curvature_sigma),
            curvature_limit);
        state.f184 = tertiary_weight > 0.0f
            ? clamp_length_component(
                random_normal(state.rng_state, 0.0f, curvature_sigma * 0.65f),
                curvature_limit * 0.70f)
            : 0.0f;
    }

    state.phase_amplitude = 0.0f;
    state.phase_skew = 0.0f;
    state.phase_offset = 0.0f;
    state.gaussian_count = 0;
    for (auto& term : state.gaussian)
        term = QxGaussianTerm{};

    if (time_scale > 0.001f && state.config.ou_sigma > 0.0f) {
        const float gaussian_base = state.config.ou_sigma
            * target_distance * state.f160;
        const float component_limit = std::max(
            (time_scale * 0.022f + 0.006f) * target_distance,
            0.8f);
        state.gaussian_count = std::clamp(
            static_cast<int>(uniform01(state) * 2.9990001f) + 2,
            2,
            4);
        const float initial_sign = random_sign(state);
        const float amplitude = std::max(
            gaussian_base * 0.35f, std::fabs(state.f164));
        const float interval = 0.64f / static_cast<float>(state.gaussian_count);
        const float normal_limit = std::clamp(
            (time_scale * 0.0045f + 0.0015f) * target_distance,
            0.15f,
            4.0f);

        for (int i = 0; i < state.gaussian_count; ++i) {
            float sign = initial_sign;
            if (uniform01(state) < 0.35f)
                sign = -initial_sign;
            const float signed_amplitude = sign * amplitude;
            const float center_low = 0.18f + static_cast<float>(i) * interval;
            QxGaussianTerm& term = state.gaussian[static_cast<std::size_t>(i)];
            term.center = uniform(state, center_low, center_low + interval);
            term.width = uniform(state, 0.055f, 0.145f);
            term.tangent_amplitude = std::clamp(
                uniform(state, 0.15f, 0.45f) * signed_amplitude,
                -component_limit,
                component_limit);
            term.normal_amplitude = uniform(state, -1.0f, 1.0f) * normal_limit;
        }

        state.phase_amplitude = std::clamp(
            uniform(state, 0.55f, 1.15f)
                * ((time_scale * 0.008f + 0.002f) * target_distance),
            0.2f,
            5.0f);
        state.phase_speed = uniform(state, 1.0f, 2.6f);
        state.phase0 = uniform(state, 0.0f, kTwoPi);
        state.phase1 = uniform(state, 0.0f, kTwoPi);
        const float phase_strength = uniform(state, 0.3f, 2.0f)
            * (time_scale * 0.65f + 0.35f);
        state.phase_skew = random_sign(state) * phase_strength;
        state.phase_offset = uniform(state, -0.45f, 0.45f) * phase_strength;
        state.phase_damping = uniform(state, 0.74f, 0.92f);
        state.phase_noise = uniform(state, 0.04f, 0.095f);
    }

    state.fixed_offset_x = 0.0f;
    state.fixed_offset_y = 0.0f;
    if (state.config.endpoint_error_enabled && state.config.endpoint_error_px > 0.0f) {
        const float radius = uniform(state, 0.0f, state.config.endpoint_error_px);
        const float angle = uniform(state, 0.0f, kTwoPi);
        state.fixed_offset_x = std::cos(angle) * radius;
        state.fixed_offset_y = std::sin(angle) * radius;
    }

    state.oscillator_x = 0.0f;
    state.oscillator_y = 0.0f;
    state.progress_rate = uniform(state, 0.001f, 0.035f);
    state.oscillator_damping = uniform(state, 3.2f, 5.8f);
    state.noise_scale = uniform(state, 0.85f, 1.45f);

    const float unit0_angle = uniform(state, 0.0f, kTwoPi);
    const float unit1_angle = uniform(state, 0.0f, kTwoPi);
    state.unit0_y = std::sin(unit0_angle);
    state.unit0_x = std::cos(unit0_angle);
    state.unit1_y = std::sin(unit1_angle);
    state.unit1_x = std::cos(unit1_angle);
    state.unit_frequency = uniform(state, 8.0f, 12.0f);
    state.unit_rotation_speed = state.config.sdn_k > 0.0f
        ? uniform(state, state.config.sdn_k * 0.27f, state.config.sdn_k)
        : 0.0f;
    state.previous_output_x = 0.0f;
    state.previous_output_y = 0.0f;
    state.previous_valid = 0;
}

} // namespace cvm::recovered
