#pragma once

#include "pidf_mode2_exact.hpp"

#include <cstddef>
#include <cstdint>

namespace cvm::recovered {

// Mode 1 consumes the same 96-byte gain/deadzone/limit configuration and
// returns the same 48-byte result ABI as Mode 2, but owns a different
// 704-byte adaptive-controller state.
using PidfMode1Config = PidfMode2Config;

struct PidfMode1State {
    PidfMode1Config config{};               // +0..+95
    double kd_x{};                          // +96
    double kd_y{};                          // +104
    double kp_x{};                          // +112
    double kp_y{};                          // +120
    double ki_x{};                          // +128
    double ki_y{};                          // +136
    std::uint8_t initialized{};             // +144
    std::uint8_t _pad091[7]{};

    // sub_140046570/sub_1400467F0/sub_140046A50 adaptive substate
    double dynamic_lr_x{};                  // +152
    double dynamic_lr_y{};                  // +160
    double ff_state_x{};                    // +168
    double ff_state_y{};                    // +176
    double kf_high_x{};                     // +184
    double kf_high_y{};                     // +192
    double correction_accum_x{};            // +200
    double correction_accum_y{};            // +208
    double base_lr_x{};                     // +216
    double base_lr_y{};                     // +224
    double kf_low_x{};                      // +232
    double kf_low_y{};                      // +240
    double ff_previous_error_x{};           // +248
    double ff_previous_error_y{};           // +256
    double ff_output_x{};                   // +264
    double ff_output_y{};                   // +272
    double correction_target_x{};           // +280
    double correction_target_y{};           // +288
    double ff_error_x{};                    // +296
    double ff_error_y{};                    // +304
    double correction_output_x{};           // +312
    double correction_output_y{};           // +320
    double correction_delta_x{};            // +328
    double correction_delta_y{};            // +336
    double correction_ratio_x{};            // +344
    double correction_ratio_y{};            // +352
    double predicted_minus_move_x{};         // +360
    double predicted_minus_move_y{};         // +368
    double ff_derivative_x{};                // +376
    double ff_derivative_y{};                // +384

    double integral_weight_x{};              // +392
    double integral_weight_y{};              // +400
    double previous_move_x{};                // +408
    double previous_move_y{};                // +416
    double integral_x{};                     // +424
    double integral_y{};                     // +432
    std::uint8_t _pad1b8[16]{};              // +440..+455
    double previous_error_x{};               // +456
    double previous_error_y{};               // +464
    double residual_x{};                     // +472
    double residual_y{};                     // +480
    double previous_timestamp{};             // +488
    double gaussian_weight_x{};              // +496
    double gaussian_weight_y{};              // +504
    double absolute_error_x{};               // +512
    double absolute_error_y{};               // +520
    double raw_pid_x{};                      // +528
    double raw_pid_y{};                      // +536
    double rounded_x{};                      // +544
    double rounded_y{};                      // +552
    double scratch_x{};                      // +560
    double scratch_y{};                      // +568
    double corrected_error_x{};              // +576
    double corrected_error_y{};              // +584
    double adaptive_radius_x{};               // +592
    double adaptive_radius_y{};               // +600
    std::int32_t move_x{};                   // +608
    std::int32_t move_y{};                   // +612
    std::uint8_t damp_x{};                   // +616
    std::uint8_t damp_y{};                   // +617
    std::uint8_t integral_enabled_x{};        // +618
    std::uint8_t integral_enabled_y{};        // +619
    std::uint8_t move_nonzero_x{};            // +620
    std::uint8_t move_nonzero_y{};            // +621
    std::uint8_t high_kf_enabled_x{};          // +622
    std::uint8_t high_kf_enabled_y{};          // +623
    std::uint8_t integral_opposes_error_x{};   // +624
    std::uint8_t integral_opposes_error_y{};   // +625
    std::uint8_t ff_opposes_error_x{};         // +626
    std::uint8_t ff_opposes_error_y{};         // +627
    std::uint8_t _pad274[4]{};                // +628..+631
    std::uint8_t axis_blocked_x{};             // +632
    std::uint8_t axis_blocked_y{};             // +633
    std::uint8_t _pad27a[6]{};
    std::uint8_t move_transform_storage[64]{};  // +640; std::function ABI
};

PidfMode1State construct_pidf_mode1(const PidfMode1Config& config,
                                    double now_seconds) noexcept;
void reset_pidf_mode1(PidfMode1State& state, double now_seconds) noexcept;
PidfNativeOutput update_pidf_mode1(PidfMode1State& state,
                                   const PidfInputExact& input,
                                   double now_seconds) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::PidfMode1State) == 704);
static_assert(offsetof(cvm::recovered::PidfMode1State, dynamic_lr_x) == 152);
static_assert(offsetof(cvm::recovered::PidfMode1State, integral_weight_x) == 392);
static_assert(offsetof(cvm::recovered::PidfMode1State, corrected_error_x) == 576);
static_assert(offsetof(cvm::recovered::PidfMode1State, axis_blocked_x) == 632);
static_assert(offsetof(cvm::recovered::PidfMode1State, move_transform_storage) == 640);
