#include "qx_curve_config_exact.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

float finite_clamp(float value, float fallback, float low, float high) noexcept {
    if (!std::isfinite(value))
        value = fallback;
    return std::clamp(value, low, high);
}

} // namespace

QxSigmaConfig normalize_qx_sigma_config(QxSigmaConfig value) noexcept {
    value.phase_speed = finite_clamp(value.phase_speed, 1.0f, 0.05f, 8.0f);
    value.target_width_scale = finite_clamp(
        value.target_width_scale, 0.45f, 0.05f, 2.0f);
    value.target_width_min = finite_clamp(
        value.target_width_min, 6.0f, 1.0f, 256.0f);
    value.target_width_max = finite_clamp(
        value.target_width_max, 32.0f, 1.0f, 512.0f);
    if (value.target_width_min > value.target_width_max)
        std::swap(value.target_width_min, value.target_width_max);

    value.ou_sigma = finite_clamp(value.ou_sigma, 0.025f, 0.0f, 2.0f);
    value.tremor_amp = finite_clamp(value.tremor_amp, 0.35f, 0.0f, 8.0f);
    value.sdn_k = finite_clamp(value.sdn_k, 0.12f, 0.0f, 8.0f);
    value.curvature_scale = finite_clamp(
        value.curvature_scale, 0.008f, 0.0f, 0.2f);
    value.overshoot_probability = finite_clamp(
        value.overshoot_probability, 0.07f, 0.0f, 1.0f);
    value.endpoint_error_px = finite_clamp(
        value.endpoint_error_px, 1.2f, 0.0f, 8.0f);
    value.near_radius = finite_clamp(value.near_radius, -1.0f, -1.0f, 128.0f);
    value.max_offset_px = finite_clamp(
        value.max_offset_px, 48.0f, 0.0f, 128.0f);
    return value;
}

bool qx_sigma_config_equal(const QxSigmaConfig& a,
                           const QxSigmaConfig& b) noexcept {
    // sub_1400438D0 compares each logical field with ==, not padding bytes.
    return a.qx_curve_enabled == b.qx_curve_enabled
        && a.phase_speed == b.phase_speed
        && a.target_width_scale == b.target_width_scale
        && a.target_width_min == b.target_width_min
        && a.target_width_max == b.target_width_max
        && a.ou_sigma == b.ou_sigma
        && a.tremor_amp == b.tremor_amp
        && a.sdn_k == b.sdn_k
        && a.curvature_scale == b.curvature_scale
        && a.correction_bias_enabled == b.correction_bias_enabled
        && a.overshoot_probability == b.overshoot_probability
        && a.endpoint_error_enabled == b.endpoint_error_enabled
        && a.endpoint_error_px == b.endpoint_error_px
        && a.near_radius == b.near_radius
        && a.max_offset_px == b.max_offset_px;
}

void reset_qx_sigma_runtime(QxSigmaState& state) noexcept {
    // Preserve the configuration and xorshift state exactly as sub_140043A80.
    const QxSigmaConfig config = state.config;
    const std::uint32_t rng = state.rng_state;
    state = {};
    state.config = config;
    state.rng_state = rng;

    state.target_identity = -1;
    state.target_distance = 1.0f;
    state.longitudinal_distance = 1.0f;
    state.direction_x = 1.0f;
    state.orthogonal_y = 1.0f;
    state.progress_rate = 0.001f;
    state.progress_limit = 8.0f;
    state.primary_log_mean = -0.45f;
    state.primary_log_sigma = 0.32f;
    state.primary_mix = 1.0f;
    state.secondary_log_mean = -0.45f;
    state.secondary_log_sigma = 0.32f;
    state.secondary_mix = 1.0f;
    state.tertiary_log_mean = -0.45f;
    state.tertiary_log_sigma = 0.32f;
    state.tertiary_mix = 1.0f;
    state.f160 = 1.0f;
    state.f168 = 1.0f;
    state.f188 = 0.25f;
    for (auto& term : state.gaussian)
        term.width = 0.1f;
    state.phase_speed = 1.8f;
    state.phase_damping = 0.86f;
    state.phase_noise = 0.08f;
    state.oscillator_damping = 4.5f;
    state.noise_scale = 1.0f;
    state.unit0_x = 1.0f;
    state.unit1_x = 1.0f;
    state.unit_frequency = 9.0f;
}

QxSigmaState construct_qx_sigma_state(std::uint32_t random_seed) noexcept {
    QxSigmaState state{};
    state.config = QxSigmaConfig{};
    state.rng_state = random_seed ^ 0x9E3779B9u;
    reset_qx_sigma_runtime(state);
    return state;
}

bool apply_qx_sigma_config(QxSigmaState& state,
                           QxSigmaConfig config) noexcept {
    config = normalize_qx_sigma_config(config);
    if (qx_sigma_config_equal(state.config, config)) {
        state.config = config;
        return false;
    }
    state.config = config;
    reset_qx_sigma_runtime(state);
    return true;
}

} // namespace cvm::recovered
