#include "controller_orchestration_exact.hpp"

#include <cstring>

namespace cvm::recovered {
namespace {

template <class T>
void put(std::array<std::byte, 72>& bytes,
         std::size_t offset,
         T value) noexcept {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void event(const ControllerOrchestrationHooks& hooks,
           ControllerOrchestrationEvent value) {
    if (hooks.trace)
        hooks.trace(value);
}

void reset_selection_metadata(
    ControllerOrchestrationStateExact& state) noexcept {
    state.selection_metadata.fill(std::byte{});
    const std::int32_t minus_one = -1;
    std::memcpy(state.selection_metadata.data() + 0,
                &minus_one, sizeof(minus_one));
    std::memcpy(state.selection_metadata.data() + 36,
                &minus_one, sizeof(minus_one));
}

void reset_target_latch(
    ControllerOrchestrationStateExact& state) noexcept {
    state.target_latch_valid = false;
    state.target_latch_identity_a = -1;
    state.target_latch_identity_b = -1;
    state.target_latch_predicted = false;
    state.target_latch_x = 0.0f;
    state.target_latch_y = 0.0f;
    state.per_target_history.fill(0);
    state.per_target_history_valid = false;
}

void reset_records_and_frame_values(
    ControllerOrchestrationStateExact& state) noexcept {
    state.target_active = false;
    state.frame_values.fill(0);
    reset_selection_metadata(state);
    state.primary_target = empty_aim_target_record();
    state.primary_target_valid = false;
    state.secondary_target = empty_aim_target_record();
    state.secondary_target_valid = false;
    state.auxiliary_target_valid = false;
    state.auxiliary_values.fill(0);
    state.state_10992 = 0;
    state.telemetry = {};
}

void reset_qx(const ControllerOrchestrationHooks& hooks) {
    event(hooks, ControllerOrchestrationEvent::reset_qx);
    if (hooks.reset_qx_runtime)
        hooks.reset_qx_runtime();
}

void reset_pidf(const ControllerOrchestrationHooks& hooks) {
    event(hooks, ControllerOrchestrationEvent::reset_pidf);
    if (hooks.reset_selected_pidf)
        hooks.reset_selected_pidf();
}

void publish(const ControllerOrchestrationHooks& hooks) {
    event(hooks, ControllerOrchestrationEvent::publish_snapshot);
    if (hooks.publish_snapshot)
        hooks.publish_snapshot();
}

void notify_secondary(const ControllerOrchestrationStateExact& state,
                      const ControllerOrchestrationHooks& hooks) {
    event(hooks, ControllerOrchestrationEvent::notify_secondary_target);
    if (hooks.notify_secondary_target)
        hooks.notify_secondary_target(state.secondary_target, false);
}

} // namespace

NativeAimTargetRecord72Abi empty_aim_target_record() noexcept {
    NativeAimTargetRecord72Abi result{};
    put<std::int32_t>(result.bytes, 16, -1);
    put<std::int32_t>(result.bytes, 24, -1);
    put<std::int32_t>(result.bytes, 52, -1);
    return result;
}

void release_latched_controller_outputs(
    ControllerOrchestrationStateExact& state,
    const ControllerOrchestrationHooks& hooks) {
    const bool primary = state.latched_primary_output;
    const bool secondary = state.latched_secondary_output;
    state.latched_primary_output = false;
    state.latched_secondary_output = false;
    state.latched_output_time = 0;

    if (!hooks.executor_present || (!primary && !secondary)
        || !hooks.executor_ready) {
        return;
    }
    if (primary) {
        event(hooks, ControllerOrchestrationEvent::release_primary_output);
        if (hooks.release_primary_output)
            hooks.release_primary_output();
    }
    if (secondary) {
        event(hooks, ControllerOrchestrationEvent::release_secondary_output);
        if (hooks.release_secondary_output)
            hooks.release_secondary_output();
    }
}

void reset_active_aim_target_controller_exact(
    ControllerOrchestrationStateExact& state,
    const ControllerOrchestrationHooks& hooks) {
    release_latched_controller_outputs(state, hooks);
    // Native clears telemetry under +11168 before resetting PIDF/QX.
    state.telemetry = {};
    reset_pidf(hooks);
    reset_target_latch(state);
    reset_qx(hooks);
    reset_records_and_frame_values(state);
    state.auxiliary_mode = 0;
    state.state_10951 = false;
    state.state_10952 = 0;
    state.state_10968 = 0;
    publish(hooks);
}

void handle_aim_target_loss_exact(
    ControllerOrchestrationStateExact& state,
    const ControllerOrchestrationHooks& hooks) {
    release_latched_controller_outputs(state, hooks);
    event(hooks, ControllerOrchestrationEvent::radius_inactive);
    if (hooks.update_radius_active)
        hooks.update_radius_active(false);

    const bool was_active = state.target_active;
    reset_pidf(hooks);
    if (was_active) {
        // This apparent duplication is native behavior: 0x1400508c8 calls
        // 0x14004ef30, whose first operations are release and PID reset again.
        reset_active_aim_target_controller_exact(state, hooks);
    } else {
        reset_qx(hooks);
        reset_records_and_frame_values(state);
        state.auxiliary_mode = 0;
        publish(hooks);
    }
    notify_secondary(state, hooks);
}

void install_new_aim_target_record_exact(
    ControllerOrchestrationStateExact& state,
    const NativeAimTargetRecord72Abi& target,
    const ControllerOrchestrationHooks& hooks) {
    event(hooks, ControllerOrchestrationEvent::radius_inactive);
    if (hooks.update_radius_active)
        hooks.update_radius_active(false);
    reset_pidf(hooks);
    reset_target_latch(state);
    reset_qx(hooks);

    reset_records_and_frame_values(state);
    state.primary_target = target;
    state.primary_target_valid = true;
    state.auxiliary_mode = 0;
    state.state_10951 = false;
    state.state_10952 = 0;
    // 0x140050b50 does not clear +10968 and does not release output latches.
    publish(hooks);
    notify_secondary(state, hooks);
}

void publish_target_error_without_move_exact(
    ControllerOrchestrationStateExact& state,
    float target_x,
    float target_y,
    float current_x,
    float current_y,
    const ControllerOrchestrationHooks& hooks) {
    reset_pidf(hooks);
    const float error_x = target_x - current_x;
    const float error_y = target_y - current_y;
    std::memcpy(&state.frame_values[3], &error_x, sizeof(error_x));
    std::memcpy(&state.frame_values[4], &error_y, sizeof(error_y));
    state.frame_values[5] = 0;
    state.frame_values[6] = 0;
    state.telemetry = {};
    publish(hooks);
}

} // namespace cvm::recovered
