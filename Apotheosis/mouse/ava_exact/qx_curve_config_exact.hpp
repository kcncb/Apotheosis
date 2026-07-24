#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cvm::recovered {

struct QxCurveInput;
struct QxCurveOutput;

// Exact 60-byte configuration consumed by sub_140043460.  Field names are
// verified against the Qt property strings at 0x140A88EB8..0x140A890E0.
struct QxSigmaConfig {
    std::uint8_t qx_curve_enabled{};                 // +0
    std::uint8_t _pad01[3]{};
    float phase_speed{1.0f};                         // +4 qxSigmaPhaseSpeed
    float target_width_scale{0.45f};                 // +8 qxSigmaTargetWidthScale
    float target_width_min{6.0f};                    // +12 qxSigmaTargetWidthMin
    float target_width_max{32.0f};                   // +16 qxSigmaTargetWidthMax
    float ou_sigma{0.025f};                          // +20 qxSigmaOuSigma
    float tremor_amp{0.35f};                         // +24 qxSigmaTremorAmp
    float sdn_k{0.12f};                              // +28 qxSigmaSdnK
    float curvature_scale{0.008f};                   // +32 qxSigmaCurvatureScale
    std::uint8_t correction_bias_enabled{};          // +36
    std::uint8_t _pad25[3]{};
    float overshoot_probability{0.07f};              // +40 qxSigmaOvershootProb
    std::uint8_t endpoint_error_enabled{};            // +44
    std::uint8_t _pad2d[3]{};
    float endpoint_error_px{1.2f};                   // +48
    float near_radius{8.0f};                         // +52; -1 means auto
    float max_offset_px{48.0f};                      // +56
};

QxSigmaConfig normalize_qx_sigma_config(QxSigmaConfig value) noexcept;
bool qx_sigma_config_equal(const QxSigmaConfig& a,
                           const QxSigmaConfig& b) noexcept;

struct QxGaussianTerm {
    float center{};
    float width{0.1f};
    float tangent_amplitude{};
    float normal_amplitude{};
};

// Full native state layout.  Unknown internal names are address-oriented,
// rather than assigned speculative semantics.  This is exactly 352 bytes;
// offsets are asserted below and match the accesses in 0x140043A80,
// 0x140043D10 and 0x140045310.
struct QxSigmaState {
    QxSigmaConfig config{};                           // +0..+59
    std::uint32_t rng_state{};                       // +60
    std::uint8_t initialized{};                      // +64
    std::uint8_t _pad41[3]{};
    std::int32_t target_identity{-1};                // +68
    float target_distance{1.0f};                     // +72
    float longitudinal_distance{1.0f};               // +76
    float direction_x{1.0f};                         // +80
    float direction_y{};                             // +84
    float orthogonal_x{};                            // +88
    float orthogonal_y{1.0f};                        // +92
    float progress_rate{0.001f};                     // +96
    float progress_accumulator{};                    // +100
    float progress_velocity{};                       // +104
    float progress_limit{8.0f};                      // +108
    float primary_shift{};                           // +112
    float primary_log_mean{-0.45f};                  // +116
    float primary_log_sigma{0.32f};                  // +120
    float primary_mix{1.0f};                         // +124
    float secondary_shift{};                         // +128
    float secondary_log_mean{-0.45f};                // +132
    float secondary_log_sigma{0.32f};                // +136
    float secondary_mix{1.0f};                       // +140
    float tertiary_shift{};                          // +144
    float tertiary_log_mean{-0.45f};                 // +148
    float tertiary_log_sigma{0.32f};                 // +152
    float tertiary_mix{1.0f};                        // +156
    float f160{1.0f};
    float f164{};
    float f168{1.0f};
    float f172{};
    float f176{};
    float f180{};
    float f184{};
    float f188{0.25f};
    std::array<QxGaussianTerm, 4> gaussian{};         // +192..+255
    std::int32_t gaussian_count{};                   // +256
    float phase0{};                                  // +260
    float phase1{};                                  // +264
    float phase_speed{1.8f};                         // +268
    float phase_amplitude{};                         // +272
    float phase_skew{};                              // +276
    float phase_offset{};                            // +280
    float phase_damping{0.86f};                      // +284
    float phase_noise{0.08f};                        // +288
    float fixed_offset_x{};                          // +292
    float fixed_offset_y{};                          // +296
    float oscillator_x{};                            // +300
    float oscillator_y{};                            // +304
    float oscillator_damping{4.5f};                  // +308
    float noise_scale{1.0f};                         // +312
    float unit0_y{};                                 // +316
    float unit0_x{1.0f};                             // +320
    float unit1_y{};                                 // +324
    float unit1_x{1.0f};                             // +328
    float unit_frequency{9.0f};                      // +332
    float unit_rotation_speed{};                     // +336
    float previous_output_x{};                       // +340
    float previous_output_y{};                       // +344
    std::uint8_t previous_valid{};                   // +348
    std::uint8_t _pad15d[3]{};
};

// sub_140043A80.  Configuration and RNG are intentionally retained.
void reset_qx_sigma_runtime(QxSigmaState& state) noexcept;

// sub_140043200 with the system-random value made explicit for replay.
QxSigmaState construct_qx_sigma_state(std::uint32_t random_seed) noexcept;

// sub_1400438D0. Returns true when a changed normalized configuration caused
// the runtime portion to be reset.
bool apply_qx_sigma_config(QxSigmaState& state,
                           QxSigmaConfig config) noexcept;

// sub_140043D10: deterministic per-target state initialization.  Every
// random draw advances the native xorshift32 state in the original order.
void reset_qx_sigma_for_target(QxSigmaState& state,
                               const QxCurveInput& input,
                               float distance) noexcept;

// sub_140045310: complete per-frame Sigma curve stage.
QxCurveOutput step_qx_sigma(QxSigmaState& state,
                            const QxCurveInput& input) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::QxSigmaConfig) == 60);
static_assert(offsetof(cvm::recovered::QxSigmaConfig, correction_bias_enabled) == 36);
static_assert(offsetof(cvm::recovered::QxSigmaConfig, max_offset_px) == 56);
static_assert(sizeof(cvm::recovered::QxGaussianTerm) == 16);
static_assert(sizeof(cvm::recovered::QxSigmaState) == 352);
static_assert(offsetof(cvm::recovered::QxSigmaState, rng_state) == 60);
static_assert(offsetof(cvm::recovered::QxSigmaState, gaussian) == 192);
static_assert(offsetof(cvm::recovered::QxSigmaState, gaussian_count) == 256);
static_assert(offsetof(cvm::recovered::QxSigmaState, phase0) == 260);
static_assert(offsetof(cvm::recovered::QxSigmaState, previous_valid) == 348);
