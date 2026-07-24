#include "target_region_exact.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

bool valid_target_box(TargetBoxF box) noexcept {
    return std::isfinite(box.left)
        && std::isfinite(box.top)
        && std::isfinite(box.width)
        && std::isfinite(box.height)
        && box.width > 0.0f
        && box.height > 0.0f;
}

TargetRegionEvaluation evaluate_target_region(
    const TargetRegionConfig& config,
    bool target_valid,
    TargetBoxF box,
    float current_x,
    float current_y) noexcept {
    TargetRegionEvaluation out{};
    if (!config.enabled || !target_valid || !valid_target_box(box))
        return out;

    out.normalized_width_scale = std::clamp(
        finite_or(config.width_scale, 1.0f), 0.0f, 1.0f);
    out.normalized_height_scale = std::clamp(
        finite_or(config.height_scale, 0.60000002f), 0.0f, 1.0f);
    if (out.normalized_width_scale <= 0.0f
        || out.normalized_height_scale <= 0.0f)
        return out;

    const float horizontal_limit = std::fmax(
        (1.0f - out.normalized_width_scale) * 0.5f, 0.0f);
    const float vertical_limit = std::fmax(
        (1.0f - out.normalized_height_scale) * 0.5f, 0.0f);
    out.normalized_horizontal_bias = std::clamp(
        finite_or(config.horizontal_bias, 0.0f),
        -horizontal_limit,
        horizontal_limit);
    out.normalized_vertical_bias = std::clamp(
        finite_or(config.vertical_bias, 0.0f),
        -vertical_limit,
        vertical_limit);

    // Preserve the native single-precision grouping.
    const float scaled_width = out.normalized_width_scale * box.width;
    const float scaled_height = out.normalized_height_scale * box.height;
    out.half_width = scaled_width * 0.5f;
    out.half_height = scaled_height * 0.5f;
    out.center_x = (out.normalized_horizontal_bias + 0.5f) * box.width
        + box.left;
    out.center_y = (out.normalized_vertical_bias + 0.5f) * box.height
        + box.top;
    out.left = out.center_x - out.half_width;
    out.right = out.center_x + out.half_width;
    out.top = out.center_y - out.half_height;
    out.bottom = out.center_y + out.half_height;

    if (!std::isfinite(current_x) || !std::isfinite(current_y)
        || !std::isfinite(out.center_x) || !std::isfinite(out.center_y))
        return out;

    out.valid = true;
    out.inside = current_x >= out.left
        && out.right >= current_x
        && current_y >= out.top
        && out.bottom >= current_y;
    return out;
}

TargetRegionEvaluation evaluate_selected_target_region(
    const TargetRegionConfig& config,
    const TargetRegionBoxSelection& selection,
    float current_x,
    float current_y) noexcept {
    if (selection.preferred_path_enabled
        && selection.preferred_target_valid
        && valid_target_box(selection.preferred)) {
        return evaluate_target_region(
            config, true, selection.preferred, current_x, current_y);
    }
    return evaluate_target_region(
        config,
        selection.fallback_target_valid,
        selection.fallback,
        current_x,
        current_y);
}

} // namespace cvm::recovered
