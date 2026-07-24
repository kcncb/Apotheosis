#pragma once

#include "mouse_output_exact.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace cvm::recovered {

// Exact 72-byte record copied by 0x140050cb8..0x140050ce8.  Several fields
// are detector/tracker union variants, so the ABI form intentionally keeps
// raw bytes instead of assigning one misleading C++ interpretation.
struct NativeAimTargetRecord72Abi {
    std::array<std::byte, 72> bytes{};
    friend bool operator==(const NativeAimTargetRecord72Abi&,
                           const NativeAimTargetRecord72Abi&) = default;
};

// Native default assembled repeatedly by 0x14004ef30, 0x140050870 and
// 0x140050b50.  Sentinel dwords are at +16, +24 and +52.
NativeAimTargetRecord72Abi empty_aim_target_record() noexcept;

// Movement-bearing portion of the controller state around native
// +10632..+10999, plus the small per-target latch reset by the same routines.
// Axis blocks are explicit because the native reset functions deliberately do
// not clear +10877/+10878.
struct ControllerOrchestrationStateExact {
    bool target_active{};                       // +10632
    std::array<std::uint32_t, 7> frame_values{};// +10636..+10663
    std::array<std::byte, 64> selection_metadata{}; // +10664..+10727
    NativeAimTargetRecord72Abi primary_target{};    // +10728
    bool primary_target_valid{};                     // +10800
    NativeAimTargetRecord72Abi secondary_target{};  // +10804
    bool secondary_target_valid{};                   // +10876
    bool axis_block_x{};                             // +10877, retained
    bool axis_block_y{};                             // +10878, retained
    bool latched_primary_output{};                   // +10879
    bool latched_secondary_output{};                 // +10880
    std::int64_t latched_output_time{};              // +10888
    bool auxiliary_target_valid{};                   // +10896
    std::array<std::uint32_t, 8> auxiliary_values{}; // +10900..+10931
    std::int32_t auxiliary_mode{};                   // +10936
    MovementTelemetry telemetry{};                  // +10940..+10949 semantic
    bool state_10951{};
    std::int32_t state_10952{};
    std::uint64_t state_10968{};
    std::uint64_t state_10992{};

    // +2184/+2200 and +7216..+7256 are reset on a full reset/new target but
    // retained by the inactive partial-loss branch.
    bool target_latch_valid{};
    std::int32_t target_latch_identity_a{-1};
    std::int32_t target_latch_identity_b{-1};
    bool target_latch_predicted{};
    float target_latch_x{};
    float target_latch_y{};
    std::array<std::uint64_t, 5> per_target_history{};
    bool per_target_history_valid{};
};

enum class ControllerOrchestrationEvent {
    release_primary_output,
    release_secondary_output,
    radius_inactive,
    reset_pidf,
    reset_qx,
    publish_snapshot,
    notify_secondary_target,
};

struct ControllerOrchestrationHooks {
    bool executor_present{};
    bool executor_ready{};
    std::function<void()> release_primary_output{};   // executor vtable +0x138
    std::function<void()> release_secondary_output{}; // executor vtable +0x140
    std::function<void(bool)> update_radius_active{}; // 0x140051c60
    std::function<void()> reset_selected_pidf{};      // 0x140051860
    std::function<void()> reset_qx_runtime{};         // 0x140043a80
    std::function<void()> publish_snapshot{};         // 0x140052050
    std::function<void(const NativeAimTargetRecord72Abi&, bool)>
        notify_secondary_target{};
    std::function<void(ControllerOrchestrationEvent)> trace{};
};

// 0x1400518e0. Latches are cleared before readiness is tested; failed or
// absent executors therefore do not leave a release pending for retry.
void release_latched_controller_outputs(
    ControllerOrchestrationStateExact& state,
    const ControllerOrchestrationHooks& hooks);

// 0x14004ef30.  This is the full active-target reset.  It does not touch the
// radius smoother and deliberately retains axis_block_x/axis_block_y.
void reset_active_aim_target_controller_exact(
    ControllerOrchestrationStateExact& state,
    const ControllerOrchestrationHooks& hooks);

// 0x140050870.  The active branch invokes the full reset (and therefore calls
// release/PID reset a second time); the inactive branch performs the smaller
// record/telemetry reset.  Both finish with the secondary-target callback.
void handle_aim_target_loss_exact(
    ControllerOrchestrationStateExact& state,
    const ControllerOrchestrationHooks& hooks);

// 0x140050b50.  Installs a fresh primary 72-byte record, clears the secondary
// record and per-target runtime, but preserves latched output and axis flags.
void install_new_aim_target_record_exact(
    ControllerOrchestrationStateExact& state,
    const NativeAimTargetRecord72Abi& target,
    const ControllerOrchestrationHooks& hooks);

// 0x140050e70: publish target error while resetting PIDF and forcing a zero
// integer move.  target/current are the absolute coordinates passed natively.
void publish_target_error_without_move_exact(
    ControllerOrchestrationStateExact& state,
    float target_x,
    float target_y,
    float current_x,
    float current_y,
    const ControllerOrchestrationHooks& hooks);

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::NativeAimTargetRecord72Abi) == 72);
