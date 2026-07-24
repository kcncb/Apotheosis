#include "pid_input_pipeline.hpp"

#include <algorithm>

namespace cvm::recovered {

TargetPoint select_pid_target(
    const ProcessHumanizationOutput& process_output,
    const QxCurveOutput& qx_output) noexcept {
    if (qx_output.valid)
        return {qx_output.target_x, qx_output.target_y};
    return {process_output.target_x, process_output.target_y};
}

PidfInput pack_pidf_input(
    TargetPoint target,
    float target_width,
    float target_height,
    bool frame_divisor_enabled,
    std::int32_t configured_frame_divisor,
    float current_x,
    float current_y) noexcept {
    PidfInput result{};
    result.valid = 1;
    result.target_x = static_cast<double>(target.x);
    result.target_y = static_cast<double>(target.y);
    result.target_radius_x = static_cast<double>(std::max(target_width, 0.0f));
    result.target_radius_y = static_cast<double>(std::max(target_height, 0.0f));
    const std::int32_t raw_divisor = frame_divisor_enabled ? configured_frame_divisor : 0;
    result.frame_divisor = std::max(raw_divisor, 0);
    result.current_x = static_cast<double>(current_x);
    result.current_y = static_cast<double>(current_y);
    return result;
}

} // namespace cvm::recovered
