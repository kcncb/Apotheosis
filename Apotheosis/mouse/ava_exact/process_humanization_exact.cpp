#include "process_humanization_exact.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cvm::recovered {
namespace {
constexpr float kTwoPi = 6.2831855f;
constexpr float kInv24 = 0.000000059604645f;

float uniform24(ProcessMt19937& random, float low, float high) noexcept {
    const float unit = static_cast<float>(random.next() >> 8) * kInv24;
    return (high - low) * unit + low;
}

float distance(Vec2f a, Vec2f b) noexcept {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::int64_t schedule_next_transition(
    const ProcessHumanizationConfig& config,
    ProcessHumanizationState& state,
    std::int64_t now_ns) noexcept {
    const float delay_sample = uniform24(state.random, state.random_low, state.random_high)
        * static_cast<float>(config.settle_ms);
    const long rounded_delay = std::lround(delay_sample);
    const std::int64_t delay_ms = static_cast<std::int64_t>(config.correction_ms)
        + 3000 + static_cast<std::int64_t>(rounded_delay);
    return now_ns + 1000000LL * delay_ms;
}

float ease_in_out_quadratic(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t < 0.5f)
        return (t + t) * t;
    const float remaining = 1.0f - t;
    return 1.0f - (remaining + remaining) * remaining;
}
} // namespace

ProcessHumanizationConfig normalize_process_humanization_config(
    ProcessHumanizationConfig value) noexcept {
    auto finite_or = [](float input, float fallback) noexcept {
        return std::isfinite(input) ? input : fallback;
    };
    value.landing_radius_axis0 = std::clamp(
        finite_or(value.landing_radius_axis0, 2.0f), 0.0f, 256.0f);
    value.landing_radius_axis1 = std::clamp(
        finite_or(value.landing_radius_axis1, 8.0f), 0.0f, 256.0f);
    value.correction_ms = std::clamp(value.correction_ms, 0, 1000);
    value.settle_ms = std::clamp(value.settle_ms, 0, 2000);
    value.minimum_landing_separation = std::clamp(
        finite_or(value.minimum_landing_separation, 1.5f), 0.0f, 128.0f);
    value.radial_distribution_mode = std::clamp(value.radial_distribution_mode, 0, 2);
    value.separation_threshold = std::clamp(
        finite_or(value.separation_threshold, 1.0f), 0.0f, 256.0f);
    value.maximum_offset_delta = std::clamp(
        finite_or(value.maximum_offset_delta, 0.0f), 0.0f, 256.0f);
    value.edge_bias_pixels = std::clamp(
        finite_or(value.edge_bias_pixels, 0.0f), 0.0f, 128.0f);
    return value;
}

void ProcessMt19937::reseed(std::uint32_t seed) noexcept {
    words_[0] = seed;
    for (std::size_t i = 1; i < words_.size(); ++i)
        words_[i] = static_cast<std::uint32_t>(
            i + 1812433253u * (words_[i - 1] ^ (words_[i - 1] >> 30)));
    index_ = words_.size();
}

void ProcessMt19937::twist() noexcept {
    constexpr std::uint32_t matrix_a = 0x9908B0DFu;
    constexpr std::uint32_t upper = 0x80000000u;
    constexpr std::uint32_t lower = 0x7FFFFFFFu;
    for (std::size_t i = 0; i < words_.size(); ++i) {
        const std::uint32_t joined = (words_[i] & upper) | (words_[(i + 1) % 624] & lower);
        words_[i] = words_[(i + 397) % 624] ^ (joined >> 1)
            ^ ((joined & 1u) ? matrix_a : 0u);
    }
    index_ = 0;
}

std::uint32_t ProcessMt19937::next() noexcept {
    if (index_ >= words_.size())
        twist();
    std::uint32_t value = words_[index_++];
    value ^= value >> 11;
    value ^= (value << 7) & 0x9D2C5680u;
    value ^= (value << 15) & 0xEFC60000u;
    value ^= value >> 18;
    return value;
}

float process_target_scale(TargetGeometryScaleInput target) noexcept {
    float selected_height = target.height > 0.0f ? target.height : 0.0f;
    float selected_width = target.width > 0.0f ? target.width : 0.0f;
    float selected = std::min(selected_width, selected_height);
    if (selected <= 0.0f)
        selected = std::max(selected_width, selected_height);
    selected = std::max(selected, 0.0f);
    if (selected <= 0.0f)
        return 0.5f;
    return std::clamp(selected / 80.0f, 0.5f, 2.0f);
}

Vec2f sample_process_landing(
    const ProcessHumanizationConfig& config,
    ProcessMt19937& random,
    TargetGeometryScaleInput target,
    float random_low,
    float random_high) noexcept {
    if (!config.enabled || !config.random_landing_enabled
        || (config.landing_radius_axis0 <= 0.0f && config.landing_radius_axis1 <= 0.0f))
        return {};

    const float angle = uniform24(random, random_low, random_high) * kTwoPi;
    const float radial_input = uniform24(random, random_low, random_high);
    float radial = std::pow(radial_input, 1.7f);
    if (config.radial_distribution_mode == 1)
        radial = std::sqrt(radial_input);
    else if (config.radial_distribution_mode == 2)
        radial = std::pow(radial_input, 0.35f);

    const float scale = process_target_scale(target);
    // The binary intentionally uses +8 for X and +4 for Y.
    const float radius_x = scale * std::max(config.landing_radius_axis1, 0.0f);
    const float radius_y = scale * std::max(config.landing_radius_axis0, 0.0f);
    const float maximum_radius = std::max(radius_x, radius_y);
    float edge_weight = 0.0f;
    if (maximum_radius > 0.0f) {
        edge_weight = std::clamp(
            scale * std::max(config.edge_bias_pixels, 0.0f) / maximum_radius,
            0.0f,
            1.0f);
    }
    const float blended_radius = (1.0f - edge_weight) * radial + edge_weight;
    return {
        std::cos(angle) * radius_x * blended_radius,
        std::sin(angle) * radius_y * blended_radius,
    };
}

Vec2f limit_process_offset_delta(
    Vec2f previous,
    Vec2f candidate,
    float maximum_offset_delta) noexcept {
    if (maximum_offset_delta <= 0.0f)
        return candidate;
    const float dx = candidate.x - previous.x;
    const float dy = candidate.y - previous.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (maximum_offset_delta >= length || length <= 0.001f)
        return candidate;
    const float ratio = maximum_offset_delta / length;
    return {previous.x + ratio * dx, previous.y + ratio * dy};
}

Vec2f choose_process_landing(
    const ProcessHumanizationConfig& config,
    ProcessMt19937& random,
    TargetGeometryScaleInput target,
    Vec2f previous,
    float random_low,
    float random_high) noexcept {
    float threshold = config.separation_threshold;
    if (config.maximum_offset_delta > 0.0f)
        threshold = std::min(config.maximum_offset_delta, threshold);
    if (threshold <= 0.0f) {
        return limit_process_offset_delta(
            previous,
            sample_process_landing(config, random, target, random_low, random_high),
            config.maximum_offset_delta);
    }

    float farthest_distance = -1.0f;
    Vec2f farthest{};
    for (int attempt = 0; attempt < 16; ++attempt) {
        const Vec2f candidate = limit_process_offset_delta(
            previous,
            sample_process_landing(config, random, target, random_low, random_high),
            config.maximum_offset_delta);
        const float candidate_distance = distance(candidate, previous);
        if (candidate_distance >= threshold)
            return candidate;
        if (candidate_distance > farthest_distance) {
            farthest_distance = candidate_distance;
            farthest = candidate;
        }
    }
    if (farthest_distance >= 0.0f)
        return farthest;
    return limit_process_offset_delta(
        previous,
        sample_process_landing(config, random, target, random_low, random_high),
        config.maximum_offset_delta);
}

Vec2f update_process_offset(
    const ProcessHumanizationConfig& config,
    ProcessHumanizationState& state,
    TargetGeometryScaleInput target,
    std::int64_t now_ns,
    float base_target_x,
    float base_target_y,
    float current_x,
    float current_y) noexcept {
    if (config.single_landing_on_reset)
        return state.current_offset;

    Vec2f interpolated{};
    if (config.correction_ms <= 0) {
        interpolated = state.transition_to;
    } else {
        const std::int64_t elapsed_ns = now_ns - state.transition_start_ns;
        const std::int64_t elapsed_ms = elapsed_ns / 1000000LL;
        const float progress = static_cast<float>(static_cast<std::int32_t>(elapsed_ms))
            / static_cast<float>(config.correction_ms);
        const float eased = ease_in_out_quadratic(progress);
        interpolated.x = state.transition_from.x
            + (state.transition_to.x - state.transition_from.x) * eased;
        interpolated.y = state.transition_from.y
            + (state.transition_to.y - state.transition_from.y) * eased;
    }
    state.current_offset = interpolated;

    bool replace = now_ns >= state.next_transition_ns;
    if (!replace) {
        const Vec2f aimed{base_target_x + interpolated.x, base_target_y + interpolated.y};
        const float distance_to_crosshair = distance(aimed, {current_x, current_y});
        const std::int64_t correction_end = state.transition_start_ns
            + 1000000LL * static_cast<std::int64_t>(config.correction_ms);
        replace = config.minimum_landing_separation >= distance_to_crosshair
            && now_ns >= correction_end;
    }
    if (replace) {
        state.transition_from = interpolated;
        state.transition_to = choose_process_landing(
            config, state.random, target, interpolated, state.random_low, state.random_high);
        state.transition_start_ns = now_ns;
        state.next_transition_ns = schedule_next_transition(config, state, now_ns);
    }
    return interpolated;
}

ProcessHumanizationFrame process_humanized_target(
    const ProcessHumanizationConfig& config,
    ProcessHumanizationState& state,
    const ProcessTargetGeometry& target,
    std::int64_t now_ns,
    float base_target_x,
    float base_target_y,
    float current_x,
    float current_y) noexcept {
    bool same_target = false;
    if (state.initialized) {
        if (state.last_tracking_identity >= 0 && target.tracking_identity >= 0) {
            same_target = state.last_tracking_identity == target.tracking_identity;
        } else if (state.last_fallback_identity == target.fallback_identity) {
            const float identity_radius = std::max(
                std::max(target.width, target.height) * 0.25f, 12.0f);
            const float dx = base_target_x - state.last_base_x;
            const float dy = base_target_y - state.last_base_y;
            same_target = dx * dx + dy * dy <= identity_radius * identity_radius;
        }
    }

    bool reset = state.initialized && !same_target;
    if (same_target && state.last_predicted && !target.predicted)
        reset = true;

    const bool eligible = config.enabled && config.random_landing_enabled
        && (config.landing_radius_axis0 > 0.0f || config.landing_radius_axis1 > 0.0f)
        && (!config.skip_predicted_target || !target.predicted);

    if (!config.enabled || !config.random_landing_enabled
        || (config.landing_radius_axis0 <= 0.0f && config.landing_radius_axis1 <= 0.0f)) {
        state.current_offset = {};
        state.transition_from = {};
        state.transition_to = {};
        state.transition_start_ns = 0;
        state.next_transition_ns = 0;
        state.active = false;
        state.initialized = true;
        state.last_tracking_identity = target.tracking_identity;
        state.last_fallback_identity = target.fallback_identity;
        state.last_predicted = target.predicted != 0;
        state.last_base_x = base_target_x;
        state.last_base_y = base_target_y;
        return {base_target_x, base_target_y, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};
    }

    if (reset) {
        state.current_offset = {};
        state.transition_from = {};
        state.transition_to = {};
        state.transition_start_ns = 0;
        state.next_transition_ns = 0;
        state.active = false;
    }

    const TargetGeometryScaleInput scale_input{target.width, target.height};
    if ((!state.initialized || reset || !state.active) && eligible) {
        state.active = true;
        if (config.single_landing_on_reset) {
            const Vec2f landing = sample_process_landing(
                config, state.random, scale_input, state.random_low, state.random_high);
            state.current_offset = landing;
            state.transition_from = landing;
            state.transition_to = landing;
            state.transition_start_ns = now_ns;
            state.next_transition_ns = (std::numeric_limits<std::int64_t>::max)();
        } else {
            state.transition_from = state.current_offset;
            state.transition_to = choose_process_landing(
                config,
                state.random,
                scale_input,
                state.current_offset,
                state.random_low,
                state.random_high);
            state.transition_start_ns = now_ns;
            state.next_transition_ns = schedule_next_transition(config, state, now_ns);
        }
    }

    state.initialized = true;
    state.last_tracking_identity = target.tracking_identity;
    state.last_fallback_identity = target.fallback_identity;
    state.last_predicted = target.predicted != 0;
    state.last_base_x = base_target_x;
    state.last_base_y = base_target_y;

    if (!state.active)
        return {base_target_x, base_target_y, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};

    const Vec2f offset = update_process_offset(
        config,
        state,
        scale_input,
        now_ns,
        base_target_x,
        base_target_y,
        current_x,
        current_y);
    const float scale = process_target_scale(scale_input);
    const float radius_x = scale * std::max(config.landing_radius_axis1, 0.0f);
    const float radius_y = scale * std::max(config.landing_radius_axis0, 0.0f);
    const float maximum_radius = std::max(radius_x, radius_y);
    float edge_weight = 0.0f;
    if (maximum_radius > 0.0f) {
        edge_weight = std::clamp(
            scale * std::max(config.edge_bias_pixels, 0.0f) / maximum_radius,
            0.0f,
            1.0f);
    }
    return {
        base_target_x + offset.x,
        base_target_y + offset.y,
        offset.x,
        offset.y,
        radius_x,
        radius_y,
        edge_weight * radius_x,
        edge_weight * radius_y,
        true,
    };
}

} // namespace cvm::recovered
