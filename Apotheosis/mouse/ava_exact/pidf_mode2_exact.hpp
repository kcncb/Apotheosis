#pragma once

#include "pidf_update_exact.hpp"

#include <cstddef>
#include <cstdint>

namespace cvm::recovered {

struct PidfNativeOutput {
    std::uint8_t initialized_this_frame{}; // +0
    std::uint8_t has_move{};               // +1
    std::uint8_t _pad02[2]{};
    std::int32_t dx{};                     // +4
    std::int32_t dy{};                     // +8
    std::uint8_t _pad0c[4]{};
    double original_error_x{};             // +16
    double original_error_y{};             // +24
    double corrected_error_x{};            // +32
    double corrected_error_y{};            // +40
};

// 96-byte constructor input copied verbatim by sub_1400482D0.
struct PidfMode2Config {
    double kp_x{};                         // +0
    double kp_y{};                         // +8
    double ki_x{};                         // +16
    double ki_y{};                         // +24
    double kd_x{};                         // +32
    double kd_y{};                         // +40
    double kf_x{};                         // +48
    double kf_y{};                         // +56
    double lr_x{};                         // +64
    double lr_y{};                         // +72
    std::int32_t deadzone_x{};             // +80
    std::int32_t deadzone_y{};             // +84
    std::int32_t movement_limit_x{};        // +88
    std::int32_t movement_limit_y{};        // +92
};

// Exact state layout used by sub_140048770.  Address-oriented scratch names
// are retained where the native helper deliberately stores intermediate
// values for telemetry/debugging.
struct PidfMode2State {
    PidfMode2Config config{};               // +0..+95
    double kd_x{};                          // +96
    double kd_y{};                          // +104
    double kp_x{};                          // +112
    double kp_y{};                          // +120
    double ki_x{};                          // +128
    double ki_y{};                          // +136
    std::uint8_t initialized{};             // +144
    std::uint8_t _pad091[7]{};

    double ff_state_x{};                    // +152
    double ff_state_y{};                    // +160
    double dynamic_lr_x{};                  // +168
    double dynamic_lr_y{};                  // +176
    double kf_high_x{};                     // +184
    double kf_high_y{};                     // +192
    double correction_accum_x{};            // +200
    double correction_accum_y{};            // +208
    double ff_previous_error_x{};            // +216
    double ff_previous_error_y{};            // +224
    double kf_low_x{};                      // +232
    double kf_low_y{};                      // +240
    double base_lr_x{};                     // +248
    double base_lr_y{};                     // +256
    double ff_output_x{};                   // +264
    double ff_output_y{};                   // +272
    double lr_exp_x{};                      // +280
    double lr_exp_y{};                      // +288
    double correction_target_x{};           // +296
    double correction_target_y{};           // +304
    double ff_error_x{};                    // +312
    double ff_error_y{};                    // +320
    double correction_delta_x{};            // +328
    double correction_delta_y{};            // +336
    double dynamic_weight_x{};              // +344
    double dynamic_weight_y{};              // +352
    double ff_derivative_x{};                // +360
    double ff_derivative_y{};                // +368
    double correction_ratio_x{};             // +376
    double correction_ratio_y{};             // +384
    double predicted_minus_move_x{};          // +392
    double predicted_minus_move_y{};          // +400
    std::uint8_t dynamic_active_x{};           // +408
    std::uint8_t dynamic_active_y{};           // +409
    std::uint8_t _pad19a[6]{};

    double residual_x{};                     // +416
    double residual_y{};                     // +424
    double previous_move_x{};                // +432
    double previous_move_y{};                // +440
    double integral_x{};                     // +448
    double integral_y{};                     // +456
    double absolute_error_x{};               // +464
    double absolute_error_y{};               // +472
    double previous_error_x{};               // +480
    double previous_error_y{};               // +488
    double integral_weight_x{};               // +496
    double integral_weight_y{};               // +504
    double previous_timestamp{};             // +512
    double attenuation_x{};                  // +520
    double attenuation_y{};                  // +528
    double rounded_x{};                      // +536
    double rounded_y{};                      // +544
    double raw_pid_x{};                      // +552
    double raw_pid_y{};                      // +560
    double scratch_x{};                      // +568
    double scratch_y{};                      // +576
    double corrected_error_x{};              // +584
    double corrected_error_y{};              // +592
    double radius_progress_x{};              // +600
    double radius_progress_y{};              // +608
    std::int32_t move_x{};                   // +616
    std::int32_t move_y{};                   // +620
    std::uint8_t move_nonzero_x{};            // +624
    std::uint8_t move_nonzero_y{};            // +625
    std::uint8_t ff_opposes_error_x{};         // +626
    std::uint8_t ff_opposes_error_y{};         // +627
    std::uint8_t far_outside_x{};              // +628
    std::uint8_t far_outside_y{};              // +629
    std::uint8_t damp_x{};                    // +630
    std::uint8_t damp_y{};                    // +631
    std::uint8_t integral_opposes_error_x{};   // +632
    std::uint8_t integral_opposes_error_y{};   // +633
    std::uint8_t inside_radius_x{};            // +634
    std::uint8_t inside_radius_y{};            // +635
    std::uint8_t _pad27c[4]{};
    std::uint8_t axis_blocked_x{};             // +640
    std::uint8_t axis_blocked_y{};             // +641
    std::uint8_t _pad282[6]{};
    std::uint8_t move_transform_storage[64]{};  // +648; std::function ABI
};

PidfMode2State construct_pidf_mode2(const PidfMode2Config& config,
                                    double now_seconds) noexcept;
void reset_pidf_mode2(PidfMode2State& state, double now_seconds) noexcept;

// Full sub_140048770 path when the optional std::function move transform is
// empty (the controller's normal callback is applied outside this class).
PidfNativeOutput update_pidf_mode2(PidfMode2State& state,
                                   const PidfInputExact& input,
                                   double now_seconds) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::PidfNativeOutput) == 48);
static_assert(sizeof(cvm::recovered::PidfMode2Config) == 96);
static_assert(sizeof(cvm::recovered::PidfMode2State) == 712);
static_assert(offsetof(cvm::recovered::PidfMode2State, ff_state_x) == 152);
static_assert(offsetof(cvm::recovered::PidfMode2State, residual_x) == 416);
static_assert(offsetof(cvm::recovered::PidfMode2State, corrected_error_x) == 584);
static_assert(offsetof(cvm::recovered::PidfMode2State, axis_blocked_x) == 640);
static_assert(offsetof(cvm::recovered::PidfMode2State, move_transform_storage) == 648);
