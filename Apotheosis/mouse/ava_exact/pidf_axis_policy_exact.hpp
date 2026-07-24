#pragma once

#include "pidf_mode1_exact.hpp"
#include "pidf_mode2_exact.hpp"

#include <cstdint>

namespace cvm::recovered {

// 0x14005053A..0x1400505DE plus sub_1400517E0.  The property name feeding
// qword_140BD1F80+4 has not been guessed; region_axis_mode is its proven
// behavior: 0 suppresses both axes, 1 permits X, 2 permits Y while inside.
struct PidfAxisPolicyInput {
    bool target_inside_region{};        // r8b at 0x140050532/537
    std::int32_t region_axis_mode{};    // clamped to [0,2]
    bool external_y_block{};            // BYTE2(qword_140BD49D4)
};

struct PidfAxisPolicyDecision {
    std::int32_t normalized_mode{};
    bool telemetry_flag_x{};            // BYTE1(dword_140BD498C)
    bool telemetry_flag_y{};            // BYTE2(dword_140BD498C)
    bool block_x{};                      // object axis_blocked_x
    bool block_y{};                      // object axis_blocked_y
    bool suppress_pidf_update{};         // inside && normalized_mode == 0
};

PidfAxisPolicyDecision evaluate_pidf_axis_policy(
    PidfAxisPolicyInput input) noexcept;

// sub_1400517E0 state mutation for each concrete object.  Blocking an axis
// also clears that axis's fractional residual before the PIDF update.
void apply_pidf_axis_policy(PidfMode1State& state,
                            const PidfAxisPolicyDecision& decision) noexcept;
void apply_pidf_axis_policy(PidfMode2State& state,
                            const PidfAxisPolicyDecision& decision) noexcept;

} // namespace cvm::recovered
