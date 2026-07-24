#pragma once

#include "pidf_postprocess_exact.hpp"

#include <cstdint>

namespace cvm::recovered {

// Common input ABI passed to sub_140047060 / sub_140048770.
struct PidfInputExact {
    std::uint8_t valid{};          // +0
    std::uint8_t _pad01[7]{};
    double target_x{};             // +8
    double target_y{};             // +16
    double radius_x{};             // +24
    double radius_y{};             // +32
    std::int32_t frame_divisor{};  // +40
    std::uint8_t _pad2c[4]{};
    double current_x{};            // +48
    double current_y{};            // +56
};

struct PidfAxisGains {
    double kp{};
    double ki{};
    double kd{};
};

struct PidfUpdateConfig {
    PidfAxisGains x{};
    PidfAxisGains y{};
    PidfPostConfig post{};
};

// Values produced by the mode-specific Kf/Lr adaptive stage immediately
// before the common PID arithmetic.  Keeping this boundary explicit avoids
// hiding the still separate mode-1/mode-2 adaptive filters in a generic PID.
struct PidfAdaptiveFrame {
    double corrected_error_x{};
    double corrected_error_y{};
    double integral_weight_x{1.0};
    double integral_weight_y{1.0};
    double feed_forward_x{};
    double feed_forward_y{};
    bool integral_gate_x{true};
    bool integral_gate_y{true};
    bool block_x{};
    bool block_y{};
};

struct PidfUpdateState {
    bool initialized{};
    double previous_timestamp{};
    double previous_error_x{};
    double previous_error_y{};
    double integral_x{};
    double integral_y{};
    double derivative_x{};
    double derivative_y{};
    double raw_pid_x{};
    double raw_pid_y{};
    double float_step_x{};
    double float_step_y{};
    PidfPostState post{};
};

struct PidfUpdateOutput {
    bool initialized_this_frame{};
    bool has_move{};
    IntegerMove move{};
    double original_error_x{};
    double original_error_y{};
    double corrected_error_x{};
    double corrected_error_y{};
};

enum class PidfMode : std::uint8_t {
    mode1 = 1,
    mode2 = 2,
};

// Exact Kf normalization performed by sub_140046B30/sub_1400482D0.
struct SplitKf {
    double low{};
    double high{};
};
SplitKf split_pidf_kf(double configured_kf) noexcept;

// Exact mode-2 Lr weight from sub_140047990.
double dynamic_lr_weight(double feed_forward_state,
                         double dt,
                         double error,
                         double radius) noexcept;

// Recovered arithmetic from 0x140047474..0x1400477B1 (mode 1) and
// 0x140048AD3..0x140048E04 (mode 2), including residual accumulation,
// rint integerization, deadzone and per-frame movement limit.
PidfUpdateOutput update_pidf_exact(
    PidfUpdateState& state,
    const PidfUpdateConfig& config,
    const PidfInputExact& input,
    const PidfAdaptiveFrame& adaptive,
    double now_seconds,
    PidfMode mode,
    const MoveTransform& transform = {});

void reset_pidf_update(PidfUpdateState& state, double now_seconds) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::PidfInputExact) == 64);
static_assert(offsetof(cvm::recovered::PidfInputExact, frame_divisor) == 40);
static_assert(offsetof(cvm::recovered::PidfInputExact, current_x) == 48);
