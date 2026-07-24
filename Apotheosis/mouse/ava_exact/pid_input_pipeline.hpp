#pragma once

#include <cstddef>
#include <cstdint>

namespace cvm::recovered {

// Stack object v175 at sub_14004F1B0+0xC59.  The layout is consumed by
// sub_140045310 and sub_140043D10.
struct QxCurveInput {
    std::uint8_t valid{};                 // +0
    std::uint8_t _pad01[3]{};
    std::int32_t target_identity{};       // +4
    float target_x{};                     // +8
    float target_y{};                     // +12
    float current_x{};                    // +16
    float current_y{};                    // +20
    float target_extent_x{};              // +24
    float target_extent_y{};              // +28
    float elapsed_ms{};                   // +32
};

// Stack object v171/v172 returned by sub_140045310.
struct QxCurveOutput {
    std::uint8_t valid{};                 // +0
    std::uint8_t _pad01[3]{};
    float target_x{};                     // +4
    float target_y{};                     // +8
};

// First 36 bytes written by sub_140024430.  Only the first two floats are
// selected as the PID target; the remaining fields are retained because the
// main loop copies them into telemetry/state globals.
struct ProcessHumanizationOutput {
    float target_x{};                     // +0
    float target_y{};                     // +4
    float offset_x{};                     // +8
    float offset_y{};                     // +12
    float scale_x{};                      // +16
    float scale_y{};                      // +20
    float auxiliary_x{};                  // +24
    float auxiliary_y{};                  // +28
    std::uint8_t active{};                // +32
    std::uint8_t _pad21[3]{};
};

// Exact ABI object packed at 0x14004FF14..0x14004FF9E and consumed by both
// PIDF implementations.  MSVC aligns the doubles at 8-byte boundaries.
struct PidfInput {
    std::uint8_t valid{};                 // +0
    std::uint8_t _pad01[7]{};
    double target_x{};                    // +8
    double target_y{};                    // +16
    double target_radius_x{};             // +24
    double target_radius_y{};             // +32
    std::int32_t frame_divisor{};         // +40
    std::uint8_t _pad2c[4]{};
    double current_x{};                   // +48
    double current_y{};                   // +56
};

struct TargetPoint {
    float x{};
    float y{};
};

// 0x14004FDC0..0x14004FEF6: QX output overrides process-humanization output
// only when its valid byte is set.
TargetPoint select_pid_target(
    const ProcessHumanizationOutput& process_output,
    const QxCurveOutput& qx_output) noexcept;

// 0x14004FF14..0x14004FF9E.
PidfInput pack_pidf_input(
    TargetPoint target,
    float target_width,
    float target_height,
    bool frame_divisor_enabled,
    std::int32_t configured_frame_divisor,
    float current_x,
    float current_y) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::QxCurveInput) == 36);
static_assert(offsetof(cvm::recovered::QxCurveInput, target_x) == 8);
static_assert(offsetof(cvm::recovered::QxCurveInput, elapsed_ms) == 32);
static_assert(sizeof(cvm::recovered::QxCurveOutput) == 12);
static_assert(sizeof(cvm::recovered::ProcessHumanizationOutput) == 36);
static_assert(sizeof(cvm::recovered::PidfInput) == 64);
static_assert(offsetof(cvm::recovered::PidfInput, target_x) == 8);
static_assert(offsetof(cvm::recovered::PidfInput, target_radius_x) == 24);
static_assert(offsetof(cvm::recovered::PidfInput, frame_divisor) == 40);
static_assert(offsetof(cvm::recovered::PidfInput, current_x) == 48);
