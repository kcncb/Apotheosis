#pragma once

#include <functional>

namespace cvm::recovered {

// Recovered from sub_140047060 and sub_140048770.
struct PidfPostConfig {
    int deadzone_x{};       // PID state +0x50
    int deadzone_y{};       // PID state +0x54
    int movement_limit_x{}; // PID state +0x58
    int movement_limit_y{}; // PID state +0x5C
};

struct PidfPostState {
    double residual_x{}; // mode 1: +0x1D8; mode 2: +0x1A0
    double residual_y{}; // mode 1: +0x1E0; mode 2: +0x1A8
};

struct IntegerMove {
    int dx{};
    int dy{};
};

using MoveTransform = std::function<IntegerMove(int, int)>;

// Bit-for-bit algorithmic reconstruction of the shared post-PID stage.
// rint() obeys the active floating-point rounding mode, matching ucrtbase!rint.
IntegerMove postprocess_pidf_step(
    PidfPostState& state,
    const PidfPostConfig& config,
    double original_error_x,
    double original_error_y,
    double float_step_x,
    double float_step_y,
    bool block_x,
    bool block_y,
    const MoveTransform& transform = {});

} // namespace cvm::recovered
