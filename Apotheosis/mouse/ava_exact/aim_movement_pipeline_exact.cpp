#include "aim_movement_pipeline_exact.hpp"

#include <utility>

namespace cvm::recovered {
namespace {

ProcessHumanizationOutput to_process_output(
    const ProcessHumanizationFrame& frame) noexcept {
    ProcessHumanizationOutput output{};
    output.target_x = frame.target_x;
    output.target_y = frame.target_y;
    output.offset_x = frame.offset_x;
    output.offset_y = frame.offset_y;
    output.scale_x = frame.radius_x;
    output.scale_y = frame.radius_y;
    output.auxiliary_x = frame.weighted_radius_x;
    output.auxiliary_y = frame.weighted_radius_y;
    output.active = frame.active;
    return output;
}

PidfInputExact to_exact_pidf_input(const PidfInput& input) noexcept {
    PidfInputExact exact{};
    exact.valid = input.valid;
    exact.target_x = input.target_x;
    exact.target_y = input.target_y;
    exact.radius_x = input.target_radius_x;
    exact.radius_y = input.target_radius_y;
    exact.frame_divisor = input.frame_divisor;
    exact.current_x = input.current_x;
    exact.current_y = input.current_y;
    return exact;
}

} // namespace

AimMovementPipelineExact::AimMovementPipelineExact(
    AimMovementPipelineConfig config,
    std::uint32_t process_seed,
    std::uint32_t qx_seed,
    double now_seconds) noexcept
    : process_state_{},
      qx_state_(construct_qx_sigma_state(qx_seed)),
      mode1_state_(construct_pidf_mode1(config.pidf, now_seconds)),
      mode2_state_(construct_pidf_mode2(config.pidf, now_seconds)) {
    process_state_.random.reseed(process_seed);
    apply_config(std::move(config), now_seconds);
}

void AimMovementPipelineExact::apply_config(
    AimMovementPipelineConfig config,
    double now_seconds) noexcept {
    config.process = normalize_process_humanization_config(config.process);
    apply_qx_sigma_config(qx_state_, config.qx);

    // This method is the explicit native profile/configuration boundary, not
    // part of the per-frame ArmController path.  Reconstruct both possible
    // objects so changing qword_140BD2750 cannot expose stale gains/state.
    mode1_state_ = construct_pidf_mode1(config.pidf, now_seconds);
    mode2_state_ = construct_pidf_mode2(config.pidf, now_seconds);
    config.qx = qx_state_.config;
    config_ = std::move(config);
}

void AimMovementPipelineExact::reset_selected_pidf(
    double now_seconds) noexcept {
    if (config_.pidf_mode == NativePidfMode::mode1)
        reset_pidf_mode1(mode1_state_, now_seconds);
    else if (config_.pidf_mode == NativePidfMode::mode2)
        reset_pidf_mode2(mode2_state_, now_seconds);
}

PidfNativeOutput AimMovementPipelineExact::update_selected_pidf(
    const PidfInputExact& input,
    double now_seconds) noexcept {
    if (config_.pidf_mode == NativePidfMode::mode1)
        return update_pidf_mode1(mode1_state_, input, now_seconds);
    if (config_.pidf_mode == NativePidfMode::mode2)
        return update_pidf_mode2(mode2_state_, input, now_seconds);
    return {};
}

void AimMovementPipelineExact::reset_pidf_runtime(
    double now_seconds) noexcept {
    reset_selected_pidf(now_seconds);
}

void AimMovementPipelineExact::reset_qx_runtime() noexcept {
    reset_qx_sigma_runtime(qx_state_);
}

ava::hotkey::AimKeySelectionResult AimMovementPipelineExact::update_hotkey_only(
    const ava::hotkey::AimKeyRuntimeView& hotkey_runtime,
    const ava::hotkey::InputEnvironment& input_environment,
    std::int64_t now_ns) {
    return ava::hotkey::update_aim_key_selection(
        hotkey_state_, hotkey_runtime, input_environment, now_ns);
}

AimMovementFrameTrace AimMovementPipelineExact::step(
    const ava::hotkey::AimKeyRuntimeView& hotkey_runtime,
    const ava::hotkey::InputEnvironment& input_environment,
    const AimMovementFrameInput& frame) {
    AimMovementFrameTrace trace{};
    trace.hotkey = update_hotkey_only(
        hotkey_runtime, input_environment, frame.now_ns);

    const bool profile_resolved = trace.hotkey.has_active_profile
        && trace.hotkey.resolved_profile
        && !trace.hotkey.active_profile_key.empty();
    if (!profile_resolved) {
        reset_qx_sigma_runtime(qx_state_);
        reset_selected_pidf(frame.now_seconds);
        trace.stop_reason = AimMovementStopReason::no_active_profile;
        return trace;
    }
    if (!frame.target.valid) {
        reset_qx_sigma_runtime(qx_state_);
        reset_selected_pidf(frame.now_seconds);
        trace.stop_reason = AimMovementStopReason::invalid_target;
        return trace;
    }

    const ProcessTargetGeometry geometry{
        frame.target.width,
        frame.target.height,
        frame.target.fallback_identity,
        frame.target.tracking_identity,
        static_cast<std::uint8_t>(frame.target.predicted),
    };
    trace.process = process_humanized_target(
        config_.process,
        process_state_,
        geometry,
        frame.now_ns,
        frame.target.base_x,
        frame.target.base_y,
        frame.current_x,
        frame.current_y);
    trace.process_ran = true;

    const ProcessHumanizationOutput process_output =
        to_process_output(trace.process);
    if (frame.qx_stage_selected) {
        trace.qx_input.valid = !frame.target.qx_input_blocked;
        trace.qx_input.target_identity = frame.target.target_identity;
        trace.qx_input.target_x = trace.process.target_x;
        trace.qx_input.target_y = trace.process.target_y;
        trace.qx_input.current_x = frame.current_x;
        trace.qx_input.current_y = frame.current_y;
        trace.qx_input.target_extent_x = frame.target.width;
        trace.qx_input.target_extent_y = frame.target.height;
        trace.qx_input.elapsed_ms = frame.qx_elapsed_ms;
        trace.qx_output = step_qx_sigma(qx_state_, trace.qx_input);
        trace.qx_ran = true;
    } else {
        reset_qx_sigma_runtime(qx_state_);
    }

    trace.selected_target = select_pid_target(process_output, trace.qx_output);
    trace.pidf_input = to_exact_pidf_input(pack_pidf_input(
        trace.selected_target,
        frame.target.width,
        frame.target.height,
        config_.frame_divisor_enabled,
        config_.configured_frame_divisor,
        frame.current_x,
        frame.current_y));

    trace.target_region = evaluate_selected_target_region(
        config_.target_region,
        frame.target_region_boxes,
        frame.current_x,
        frame.current_y);
    trace.axis_policy = evaluate_pidf_axis_policy({
        trace.target_region.inside,
        config_.target_region.region_axis_mode,
        frame.external_y_block,
    });
    if (config_.pidf_mode == NativePidfMode::mode1)
        apply_pidf_axis_policy(mode1_state_, trace.axis_policy);
    else if (config_.pidf_mode == NativePidfMode::mode2)
        apply_pidf_axis_policy(mode2_state_, trace.axis_policy);

    if (trace.axis_policy.suppress_pidf_update) {
        reset_selected_pidf(frame.now_seconds);
        // reset_pidf_* clears initialized/integral/feed-forward/residual but
        // deliberately retains axis_blocked, matching the native object.
        if (config_.pidf_mode == NativePidfMode::mode1)
            apply_pidf_axis_policy(mode1_state_, trace.axis_policy);
        else if (config_.pidf_mode == NativePidfMode::mode2)
            apply_pidf_axis_policy(mode2_state_, trace.axis_policy);
        trace.pidf_input.valid = 0;
        trace.stop_reason = AimMovementStopReason::region_policy_suppressed;
        return trace;
    }
    if (config_.pidf_mode != NativePidfMode::mode1
        && config_.pidf_mode != NativePidfMode::mode2) {
        trace.pidf_input.valid = 0;
        trace.stop_reason = AimMovementStopReason::pidf_disabled;
        return trace;
    }

    trace.pidf_output = update_selected_pidf(
        trace.pidf_input, frame.now_seconds);
    trace.pidf_ran = true;
    if (trace.pidf_output.initialized_this_frame) {
        trace.stop_reason = AimMovementStopReason::pidf_initialization_frame;
        return trace;
    }
    if (!trace.pidf_output.has_move) {
        trace.stop_reason = AimMovementStopReason::zero_integer_move;
        return trace;
    }

    trace.movement = dispatch_controller_movement(
        config_.movement,
        trace.pidf_output.dx,
        trace.pidf_output.dy);
    trace.movement_dispatched = true;
    return trace;
}

} // namespace cvm::recovered
