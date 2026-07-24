#include "target_to_aimpoint_exact.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

constexpr float kCacheEpsilon = 0.000099999997f;

float ordered_clamp(float value, float low, float high) noexcept {
    // Mirrors the two COMISS/CMOVBE pairs. In particular, NaN survives.
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

float finite_clamp_or(float value, float low, float high,
                      float fallback) noexcept {
    return std::isfinite(value) ? ordered_clamp(value, low, high) : fallback;
}

bool valid_box(float left, float top, float width, float height) noexcept {
    return std::isfinite(left) && std::isfinite(top)
        && std::isfinite(width) && std::isfinite(height)
        && width > 0.0f && height > 0.0f;
}

void clear_cache(AimPointState64Abi& state) noexcept {
    state.cache_valid = 0;
    state.cached_ratio = 0.0f;
    state.cached_low = 0.0f;
    state.cached_high = 0.0f;
    state.cached_extra_y = 0.0f;
}

bool stabilize_ratio_exact(
    AimPointState64Abi& state,
    std::int32_t mode,
    std::int32_t class_id,
    float top,
    float height,
    float ratio_a,
    float ratio_b,
    float reference_y,
    float& output_y) noexcept {
    if (std::fabs(ratio_a - ratio_b) <= kCacheEpsilon || height <= 0.0f) {
        clear_cache(state);
        return false;
    }

    const float low = std::fmin(ratio_b, ratio_a);
    const float high = std::fmax(ratio_b, ratio_a);
    const bool target_changed = state.output_valid && state.class_id != class_id;
    const bool mode_changed = state.output_valid && state.mode != mode;
    const bool range_changed = !state.cache_valid
        || std::fabs(state.cached_low - low) > kCacheEpsilon
        || std::fabs(state.cached_high - high) > kCacheEpsilon;
    if (target_changed || mode_changed || range_changed) {
        state.cached_low = low;
        state.cached_high = high;
        state.cache_valid = 1;
        state.cached_ratio = ordered_clamp(
            (reference_y - top) / height, low, high);
    }
    output_y = height * state.cached_ratio + top;
    return true;
}

bool stabilize_ratio_with_extra_exact(
    AimPointState64Abi& state,
    std::int32_t mode,
    std::int32_t class_id,
    float top,
    float height,
    float ratio_a,
    float ratio_b,
    float extra_y,
    float reference_y,
    float& output_y) noexcept {
    if (std::fabs(ratio_a - ratio_b) <= kCacheEpsilon || height <= 0.0f) {
        clear_cache(state);
        return false;
    }

    const float low = std::fmin(ratio_b, ratio_a);
    const float high = std::fmax(ratio_b, ratio_a);
    const bool target_changed = state.output_valid && state.class_id != class_id;
    const bool mode_changed = state.output_valid && state.mode != mode;
    const bool parameters_changed = !state.cache_valid
        || std::fabs(state.cached_low - low) > kCacheEpsilon
        || std::fabs(state.cached_high - high) > kCacheEpsilon
        || std::fabs(state.cached_extra_y - extra_y) > kCacheEpsilon;
    if (target_changed || mode_changed || parameters_changed) {
        const float absolute_low = low * height + top + extra_y;
        const float absolute_high = high * height + top + extra_y;
        const float desired = ordered_clamp(reference_y,
            std::fmin(absolute_low, absolute_high),
            std::fmax(absolute_low, absolute_high));
        float ratio = (desired - top - extra_y) / height;
        ratio = ordered_clamp(ratio, low, high);
        state.cached_low = low;
        state.cached_high = high;
        state.cached_extra_y = extra_y;
        state.cached_ratio = ratio;
        state.cache_valid = 1;
    }
    output_y = height * state.cached_ratio + top + extra_y;
    return true;
}

} // namespace

AimPointTarget72Abi make_aimpoint_target_record_exact(
    const SelectedTarget104Abi& selected) noexcept {
    AimPointTarget72Abi target{};
    target.primary_left = selected.left;
    target.primary_top = selected.top;
    target.primary_width = std::fmax(selected.right - selected.left, 0.0f);
    target.primary_height = std::fmax(selected.bottom - selected.top, 0.0f);
    target.primary_class = selected.class_id;
    target.primary_confidence = selected.confidence;
    target.reserved24 = -1;
    target.target_flag = selected.target_flag;
    target.related_class = -1;
    if (selected.related_box_valid) {
        target.related_valid = 1;
        target.related_left = selected.related_left;
        target.related_top = selected.related_top;
        target.related_width = std::fmax(
            selected.related_right - selected.related_left, 0.0f);
        target.related_height = std::fmax(
            selected.related_bottom - selected.related_top, 0.0f);
        target.related_class = selected.related_class_id;
        target.related_confidence = selected.related_confidence;
    }
    if (selected.predicted_center_valid) {
        target.predicted_aim_valid = 1;
        target.predicted_aim_x = static_cast<float>(selected.primary_center_x);
        target.predicted_aim_y = static_cast<float>(selected.primary_center_y);
    }
    return target;
}

AimPointParametersExact resolve_aimpoint_parameters_exact(
    const AimPointTarget72Abi& target,
    const AimPointConfigExact& config) noexcept {
    AimPointParametersExact result{
        ordered_clamp(config.default_ratio_low, 0.0f, 1.0f),
        ordered_clamp(config.default_ratio_high, 0.0f, 1.0f),
        finite_clamp_or(config.default_horizontal_offset, -5.0f, 5.0f, 0.0f),
        finite_clamp_or(config.default_vertical_offset, -5.0f, 5.0f, 0.0f)};

    if (!config.class_rules_enabled)
        return result;
    std::int32_t class_id = target.primary_class;
    if (target.related_valid && target.related_class >= 0)
        class_id = target.related_class;
    if (class_id < 0)
        return result;
    for (const auto& rule : config.class_rules) {
        if (rule.class_id != class_id)
            continue;
        result.ratio_low = ordered_clamp(rule.ratio_low, 0.0f, 1.0f);
        result.ratio_high = ordered_clamp(rule.ratio_high, 0.0f, 1.0f);
        result.horizontal_offset = finite_clamp_or(
            rule.horizontal_offset, -5.0f, 5.0f, 0.0f);
        result.vertical_offset = finite_clamp_or(
            rule.vertical_offset, -5.0f, 5.0f, 0.0f);
        break;
    }
    return result;
}

bool target_to_aimpoint_exact(
    const AimPointTarget72Abi& target,
    AimPointState64Abi& state,
    const AimPointConfigExact& config) noexcept {
    std::int32_t mode = 0;
    if (config.related_mode_enabled) {
        mode = target.related_valid
                && valid_box(target.related_left, target.related_top,
                             target.related_width, target.related_height)
            ? 1 : 2;
    }
    if (state.output_valid && state.mode != mode)
        clear_cache(state);

    float left{};
    float top{};
    float width{};
    float height{};
    std::int32_t class_id{-1};
    float aim_x{};
    float aim_y{};
    bool used_related{};

    if (mode == 1) {
        const float ratio_low = ordered_clamp(
            config.related_ratio_low, 0.0f, 5.0f);
        const float ratio_high = ordered_clamp(
            config.related_ratio_high, 0.0f, 5.0f);
        const float horizontal_offset = finite_clamp_or(
            config.related_horizontal_offset, -5.0f, 5.0f, 0.0f);
        const float vertical_offset = finite_clamp_or(
            config.related_vertical_offset, -5.0f, 5.0f, 0.0f);

        left = target.related_left;
        top = target.related_top;
        width = target.related_width;
        height = std::fmax(target.related_height, 1.0f);
        class_id = target.related_class < 0
            ? target.primary_class : target.related_class;
        const float extra_y = height * vertical_offset;
        aim_y = top + height * ratio_low + extra_y;
        stabilize_ratio_with_extra_exact(state, 1, class_id, top, height,
            ratio_low, ratio_high, extra_y, config.reference_y, aim_y);
        aim_x = left + (horizontal_offset + 0.5f) * width;
        used_related = true;
    } else if (mode == 2) {
        const float ratio_low = ordered_clamp(
            config.fallback_ratio_low, 0.0f, 1.0f);
        const float ratio_high = ordered_clamp(
            config.fallback_ratio_high, 0.0f, 1.0f);
        const float horizontal_offset = finite_clamp_or(
            config.fallback_horizontal_offset, -5.0f, 5.0f, 0.0f);
        const float vertical_offset = finite_clamp_or(
            config.fallback_vertical_offset, -5.0f, 5.0f, 0.0f);
        const float width_to_y = finite_clamp_or(
            config.fallback_width_to_y, 0.0f, 5.0f, 0.235f);

        left = target.primary_left;
        top = target.primary_top;
        width = target.primary_width;
        height = target.primary_height;
        class_id = target.primary_class;
        aim_y = top + ratio_low * height;
        stabilize_ratio_exact(state, 2, class_id, top, height,
            ratio_low, ratio_high, config.reference_y, aim_y);
        aim_y += vertical_offset * height;
        aim_y += width_to_y * width;
        aim_x = left + (horizontal_offset + 0.5f) * width;
        used_related = false;
    } else {
        used_related = target.related_valid != 0;
        if (used_related) {
            left = target.related_left;
            top = target.related_top;
            width = target.related_width;
            height = target.related_height;
        } else {
            left = target.primary_left;
            top = target.primary_top;
            width = target.primary_width;
            height = target.primary_height;
        }
        const AimPointParametersExact p =
            resolve_aimpoint_parameters_exact(target, config);
        if (target.predicted_aim_valid) {
            aim_x = target.predicted_aim_x;
            aim_y = target.predicted_aim_y;
        } else {
            aim_y = top + p.ratio_low * height;
            aim_x = left + (p.horizontal_offset + 0.5f) * width;
        }
        class_id = used_related && target.related_class >= 0
            ? target.related_class : target.primary_class;
        stabilize_ratio_exact(state, 0, class_id, top, height,
            p.ratio_low, p.ratio_high, config.reference_y, aim_y);
        if (!target.predicted_aim_valid)
            aim_y += p.vertical_offset * height;
    }

    const float relative_y = aim_y - top;
    float normalized_y = 0.0f;
    if (height > 0.0f)
        normalized_y = relative_y / height;

    state.valid = 1;
    state.aim_x = aim_x;
    state.aim_y = aim_y;
    state.aim_y_from_box_top = relative_y;
    state.aim_y_normalized = normalized_y;
    state.box_left = left;
    state.box_top = top;
    state.box_width = width;
    state.box_height = height;
    state.class_id = class_id;
    state.output_valid = 1;
    state.used_related = static_cast<std::uint8_t>(used_related);
    state.mode = mode;
    return used_related;
}

} // namespace cvm::recovered
