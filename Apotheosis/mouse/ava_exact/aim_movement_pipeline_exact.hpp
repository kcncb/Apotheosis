#pragma once

#include "aim_key_selector.hpp"
#include "mouse_output_exact.hpp"
#include "pid_input_pipeline.hpp"
#include "pidf_axis_policy_exact.hpp"
#include "pidf_mode1_exact.hpp"
#include "pidf_mode2_exact.hpp"
#include "process_humanization_exact.hpp"
#include "qx_curve_config_exact.hpp"
#include "target_region_exact.hpp"

#include <cstdint>
#include <string>

namespace cvm::recovered {

// qword_140BD2750 low byte in ArmController_14004F1B0.
enum class NativePidfMode : std::uint8_t {
    disabled = 0,
    mode1 = 1,
    mode2 = 2,
};

// Configuration already selected by the profile/configuration layer.  The
// hotkey stage below determines whether that selected profile is active; the
// native program likewise materializes the selected profile into globals
// before ArmController consumes these values.
struct AimMovementPipelineConfig {
    ProcessHumanizationConfig process{};  // word_140BD2768
    QxSigmaConfig qx{};                   // state at 0x140BD3B70
    TargetRegionConfig target_region{};   // globals 0x140BD1F70..0x140BD1F83
    NativePidfMode pidf_mode{NativePidfMode::disabled};
    PidfMode2Config pidf{};                // common 96-byte constructor ABI

    bool frame_divisor_enabled{};          // byte_140BD3F10
    std::int32_t configured_frame_divisor{}; // dword_140BD3F80
    ControllerMovementConfig movement{};
};

struct AimMovementTargetFrame {
    bool valid{};
    bool qx_input_blocked{};  // BYTE12(v181); QX valid is its logical inverse
    std::int32_t target_identity{-1};
    std::int32_t fallback_identity{-1};
    std::int32_t tracking_identity{-1};
    bool predicted{};
    float base_x{};
    float base_y{};
    float width{};
    float height{};
};

struct AimMovementFrameInput {
    std::int64_t now_ns{};
    double now_seconds{};
    float qx_elapsed_ms{};        // v179; native clamps [0,50], then multiplies by .001
    float current_x{};
    float current_y{};
    bool qx_stage_selected{};     // low dword of qword_140BD1FE0 == 1

    // Geometry source selection at 0x14005021B..0x1400502FA.  The selected
    // box is evaluated locally rather than accepting a precomputed gate.
    TargetRegionBoxSelection target_region_boxes{};
    bool external_y_block{};
    AimMovementTargetFrame target{};
};

enum class AimMovementStopReason : std::uint8_t {
    none,
    no_active_profile,
    invalid_target,
    region_policy_suppressed,
    pidf_disabled,
    pidf_initialization_frame,
    zero_integer_move,
};

// Every stage is retained so a replay can compare the exact boundary objects
// rather than only the final mouse delta.
struct AimMovementFrameTrace {
    ava::hotkey::AimKeySelectionResult hotkey{};
    AimMovementStopReason stop_reason{AimMovementStopReason::none};
    ProcessHumanizationFrame process{};
    QxCurveInput qx_input{};
    QxCurveOutput qx_output{};
    TargetPoint selected_target{};
    PidfInputExact pidf_input{};
    TargetRegionEvaluation target_region{};
    PidfAxisPolicyDecision axis_policy{};
    PidfNativeOutput pidf_output{};
    ControllerMovementResult movement{};
    bool process_ran{};
    bool qx_ran{};
    bool pidf_ran{};
    bool movement_dispatched{};
};

class AimMovementPipelineExact {
public:
    AimMovementPipelineExact(AimMovementPipelineConfig config,
                             std::uint32_t process_seed,
                             std::uint32_t qx_seed,
                             double now_seconds = 0.0) noexcept;

    // Profile changes reconstruct the PIDF object just as the native
    // configuration boundary does.  QX's own apply routine retains the RNG
    // while resetting only its runtime portion when normalized values differ.
    void apply_config(AimMovementPipelineConfig config,
                      double now_seconds) noexcept;

    AimMovementFrameTrace step(
        const ava::hotkey::AimKeyRuntimeView& hotkey_runtime,
        const ava::hotkey::InputEnvironment& input_environment,
        const AimMovementFrameInput& frame);

    // Public orchestration seams corresponding to 0x140051860 and
    // 0x140043a80.  They let the recovered outer target-loss/reacquire state
    // machine reset the exact owned objects without reconstructing config.
    void reset_pidf_runtime(double now_seconds) noexcept;
    void reset_qx_runtime() noexcept;

    // Hotkey selection lives outside the native ArmController body.  The
    // complete controller uses this on target-loss/reacquire early returns.
    ava::hotkey::AimKeySelectionResult update_hotkey_only(
        const ava::hotkey::AimKeyRuntimeView& hotkey_runtime,
        const ava::hotkey::InputEnvironment& input_environment,
        std::int64_t now_ns);

    const AimMovementPipelineConfig& config() const noexcept { return config_; }
    const ava::hotkey::AimKeySelectionState& hotkey_state() const noexcept {
        return hotkey_state_;
    }
    const ProcessHumanizationState& process_state() const noexcept {
        return process_state_;
    }
    const QxSigmaState& qx_state() const noexcept { return qx_state_; }
    const PidfMode1State& mode1_state() const noexcept { return mode1_state_; }
    const PidfMode2State& mode2_state() const noexcept { return mode2_state_; }

private:
    void reset_selected_pidf(double now_seconds) noexcept;
    PidfNativeOutput update_selected_pidf(const PidfInputExact& input,
                                          double now_seconds) noexcept;

    AimMovementPipelineConfig config_{};
    ava::hotkey::AimKeySelectionState hotkey_state_{};
    ProcessHumanizationState process_state_{};
    QxSigmaState qx_state_{};
    PidfMode1State mode1_state_{};
    PidfMode2State mode2_state_{};
};

} // namespace cvm::recovered
