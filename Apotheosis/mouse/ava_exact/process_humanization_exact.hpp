#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cvm::recovered {

struct Vec2f {
    float x{};
    float y{};
};

// First 48 bytes of the state rooted at 0x140BD2768.  Names which are not
// exposed directly by behavior are deliberately address-oriented rather than
// guessed.  The normalizer is sub_140024110.
struct ProcessHumanizationConfig {
    std::uint8_t enabled{};                    // +0
    std::uint8_t random_landing_enabled{};     // +1
    std::uint8_t _pad02[2]{};
    float landing_radius_axis0{};              // +4, clamped [0,256]
    float landing_radius_axis1{};              // +8, clamped [0,256]
    std::uint8_t single_landing_on_reset{};    // +12; one landing, expiry=INT64_MAX
    std::uint8_t _pad0d[3]{};
    std::int32_t correction_ms{};              // +16, clamped [0,1000]
    std::int32_t settle_ms{};                  // +20, clamped [0,2000]
    float minimum_landing_separation{};        // +24, clamped [0,128]
    std::int32_t radial_distribution_mode{};   // +28, clamped [0,2]
    float separation_threshold{};              // +32, clamped [0,256]
    float maximum_offset_delta{};              // +36, clamped [0,256]
    float edge_bias_pixels{};                  // +40, clamped [0,128]
    std::uint8_t skip_predicted_target{};       // +44
    std::uint8_t _pad2d[3]{};
};

ProcessHumanizationConfig normalize_process_humanization_config(
    ProcessHumanizationConfig value) noexcept;

// Exact MT19937 sequence used by sub_1400252B0.  The binary keeps two 624-word
// banks to avoid modulo operations; the sequence is standard MT19937.
class ProcessMt19937 {
public:
    explicit ProcessMt19937(std::uint32_t seed = 5489u) noexcept { reseed(seed); }
    void reseed(std::uint32_t seed) noexcept;
    std::uint32_t next() noexcept;

private:
    void twist() noexcept;
    std::array<std::uint32_t, 624> words_{};
    std::size_t index_{624};
};

struct TargetGeometryScaleInput {
    float width{};   // target geometry +8
    float height{};  // target geometry +12
};

struct ProcessTargetGeometry {
    float width{};                            // source target +8
    float height{};                           // source target +12
    std::int32_t fallback_identity{};         // source target +16
    std::int32_t tracking_identity{};         // source target +24
    std::uint8_t predicted{};                 // source target +28
};

struct ProcessHumanizationState {
    bool initialized{};                       // binary +48
    std::int32_t last_tracking_identity{-1};  // +52
    std::int32_t last_fallback_identity{-1};  // +56
    bool last_predicted{};                    // +60
    float last_base_x{};                      // +64
    float last_base_y{};                      // +68
    ProcessMt19937 random{};                  // binary +72..+5068
    float random_low{};                       // +5072
    float random_high{1.0f};                  // +5076
    Vec2f current_offset{};                   // +5080
    Vec2f transition_from{};                  // +5088
    Vec2f transition_to{};                    // +5096
    std::int64_t transition_start_ns{};       // +5104
    std::int64_t next_transition_ns{};        // +5112
    bool active{};                            // +5120
};

struct ProcessHumanizationFrame {
    float target_x{};                         // output +0
    float target_y{};                         // +4
    float offset_x{};                         // +8
    float offset_y{};                         // +12
    float radius_x{};                         // +16
    float radius_y{};                         // +20
    float weighted_radius_x{};                // +24
    float weighted_radius_y{};                // +28
    bool active{};                            // +32
};

// sub_140024060.
float process_target_scale(TargetGeometryScaleInput target) noexcept;

// sub_140024C40. The process state keeps the random interval [0,1]; explicit
// bounds are accepted here so the recovered helper can also replay snapshots.
Vec2f sample_process_landing(
    const ProcessHumanizationConfig& config,
    ProcessMt19937& random,
    TargetGeometryScaleInput target,
    float random_low = 0.0f,
    float random_high = 1.0f) noexcept;

// sub_140025140: limit the candidate's displacement from the previous offset
// to maximum_offset_delta.
Vec2f limit_process_offset_delta(
    Vec2f previous,
    Vec2f candidate,
    float maximum_offset_delta) noexcept;

// sub_140024E70: draw up to 16 candidates and select the first one far enough
// from the previous point; otherwise retain the farthest candidate.
Vec2f choose_process_landing(
    const ProcessHumanizationConfig& config,
    ProcessMt19937& random,
    TargetGeometryScaleInput target,
    Vec2f previous,
    float random_low = 0.0f,
    float random_high = 1.0f) noexcept;

// sub_140024910: interpolate the offset and schedule a replacement landing.
Vec2f update_process_offset(
    const ProcessHumanizationConfig& config,
    ProcessHumanizationState& state,
    TargetGeometryScaleInput target,
    std::int64_t now_ns,
    float base_target_x,
    float base_target_y,
    float current_x,
    float current_y) noexcept;

// sub_140024430: complete process-humanization stage used immediately before
// QX-curve shaping and PID input packing.
ProcessHumanizationFrame process_humanized_target(
    const ProcessHumanizationConfig& config,
    ProcessHumanizationState& state,
    const ProcessTargetGeometry& target,
    std::int64_t now_ns,
    float base_target_x,
    float base_target_y,
    float current_x,
    float current_y) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::ProcessHumanizationConfig) == 48);
static_assert(offsetof(cvm::recovered::ProcessHumanizationConfig, correction_ms) == 16);
static_assert(offsetof(cvm::recovered::ProcessHumanizationConfig, skip_predicted_target) == 44);
