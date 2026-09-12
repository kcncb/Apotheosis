#include "pidf_postprocess_exact.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

int quantize_with_residual(double step, double& residual, bool blocked) {
    if (blocked) {
        step = 0.0;
        residual = 0.0;
    }

    residual += step;

    // 0x1400475B0 / 0x140047654 (mode 1)
    // 0x140048C12 / 0x140048CB5 (mode 2)
    // IAT 0x140679608 -> ucrtbase!rint.
    const double rounded = std::rint(residual);
    const int integer = static_cast<int>(rounded);
    if (integer != 0)
        residual -= static_cast<double>(integer);
    return integer;
}

int apply_deadzone(int move, double original_error, int deadzone) {
    // The binary only suppresses a quantized +/-1 at or inside the configured
    // error deadzone. Larger moves are deliberately retained.
    if (deadzone > 0 && static_cast<double>(deadzone) >= std::fabs(original_error)
        && std::abs(move) <= 1)
        return 0;
    return move;
}

int apply_movement_limit(int move, int limit) {
    return limit > 0 ? std::clamp(move, -limit, limit) : move;
}

} // namespace

IntegerMove postprocess_pidf_step(
    PidfPostState& state,
    const PidfPostConfig& config,
    double original_error_x,
    double original_error_y,
    double float_step_x,
    double float_step_y,
    bool block_x,
    bool block_y,
    const MoveTransform& transform) {
    IntegerMove result{
        quantize_with_residual(float_step_x, state.residual_x, block_x),
        quantize_with_residual(float_step_y, state.residual_y, block_y),
    };

    // mode 1: 0x14004769F..0x1400477B1
    // mode 2: 0x140048CF8..0x140048E04
    result.dx = apply_deadzone(result.dx, original_error_x, config.deadzone_x);
    result.dy = apply_deadzone(result.dy, original_error_y, config.deadzone_y);
    result.dx = apply_movement_limit(result.dx, config.movement_limit_x);
    result.dy = apply_movement_limit(result.dy, config.movement_limit_y);

    // std::function callback stored at mode 1 +0x2B8 / mode 2 +0x2C0.
    if (transform && (result.dx != 0 || result.dy != 0))
        result = transform(result.dx, result.dy);
    return result;
}

} // namespace cvm::recovered
