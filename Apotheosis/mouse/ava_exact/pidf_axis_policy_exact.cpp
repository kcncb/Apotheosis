#include "pidf_axis_policy_exact.hpp"

#include <algorithm>

namespace cvm::recovered {

PidfAxisPolicyDecision evaluate_pidf_axis_policy(
    PidfAxisPolicyInput input) noexcept {
    PidfAxisPolicyDecision out{};
    out.normalized_mode = std::clamp(input.region_axis_mode, 0, 2);

    // 0x14005056F uses TEST mode,0xFFFFFFFD: only 0 and 2 pass.
    out.telemetry_flag_x = input.target_inside_region
        && (out.normalized_mode == 0 || out.normalized_mode == 2);
    out.telemetry_flag_y = input.target_inside_region
        && out.normalized_mode <= 1;

    // sub_1400517E0 copies those flags into the active PIDF object's two
    // axis_blocked bytes; Y additionally ORs the prior global condition.
    out.block_x = out.telemetry_flag_x;
    out.block_y = input.external_y_block || out.telemetry_flag_y;
    out.suppress_pidf_update = input.target_inside_region
        && out.normalized_mode == 0;
    return out;
}

void apply_pidf_axis_policy(
    PidfMode1State& state,
    const PidfAxisPolicyDecision& decision) noexcept {
    state.axis_blocked_x = decision.block_x;
    state.axis_blocked_y = decision.block_y;
    if (decision.block_x)
        state.residual_x = 0.0;
    if (decision.block_y)
        state.residual_y = 0.0;
}

void apply_pidf_axis_policy(
    PidfMode2State& state,
    const PidfAxisPolicyDecision& decision) noexcept {
    state.axis_blocked_x = decision.block_x;
    state.axis_blocked_y = decision.block_y;
    if (decision.block_x)
        state.residual_x = 0.0;
    if (decision.block_y)
        state.residual_y = 0.0;
}

} // namespace cvm::recovered
