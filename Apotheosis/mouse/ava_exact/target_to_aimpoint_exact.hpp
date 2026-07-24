#pragma once

#include "target_selection_exact.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cvm::recovered {

// Exact 72-byte transient record assembled by ArmController at
// 0x14004F8AE..0x14004FA13 before calling 0x14004BC30.
struct AimPointTarget72Abi {
    float primary_left{};             // +0
    float primary_top{};              // +4
    float primary_width{};            // +8
    float primary_height{};           // +12
    std::int32_t primary_class{-1};    // +16
    float primary_confidence{};        // +20
    std::int32_t reserved24{-1};       // +24
    std::uint8_t target_flag{};        // +28
    std::array<std::uint8_t, 3> pad29{};
    float related_left{};             // +32
    float related_top{};              // +36
    float related_width{};            // +40
    float related_height{};           // +44
    std::uint8_t related_valid{};      // +48
    std::array<std::uint8_t, 3> pad49{};
    std::int32_t related_class{-1};    // +52
    float related_confidence{};        // +56
    float predicted_aim_x{};          // +60
    float predicted_aim_y{};          // +64
    std::uint8_t predicted_aim_valid{};// +68
    std::array<std::uint8_t, 3> pad69{};
};
static_assert(sizeof(AimPointTarget72Abi) == 72);
static_assert(offsetof(AimPointTarget72Abi, related_valid) == 48);
static_assert(offsetof(AimPointTarget72Abi, predicted_aim_valid) == 68);

// Per-class record stored in native xmmword_140BD2090's 20-byte vector.
struct ClassAimPointRuleExact {
    std::int32_t class_id{};
    float ratio_low{};
    float ratio_high{};
    float horizontal_offset{};
    float vertical_offset{};
};
static_assert(sizeof(ClassAimPointRuleExact) == 20);

// Explicit image of the globals consumed by 0x14004B540/0x14004BC30.
struct AimPointConfigExact {
    float reference_x{}; // xmmword_140BD1F10 +0
    float reference_y{}; // xmmword_140BD1F10 +4

    float default_ratio_low{};  // +8
    float default_ratio_high{}; // +12
    float default_horizontal_offset{}; // xmmword_140BD1F20 +0
    float default_vertical_offset{};   // xmmword_140BD1F20 +4

    float related_ratio_low{};  // xmmword_140BD1F20 +8, clamp [0,5]
    float related_ratio_high{}; // xmmword_140BD1F20 +12, clamp [0,5]
    float related_horizontal_offset{}; // xmmword_140BD1F30 +0
    float related_vertical_offset{};   // xmmword_140BD1F30 +4

    float fallback_ratio_low{};  // xmmword_140BD1F30 +8, clamp [0,1]
    float fallback_ratio_high{}; // xmmword_140BD1F30 +12, clamp [0,1]
    float fallback_horizontal_offset{}; // xmmword_140BD1F40 +0
    float fallback_vertical_offset{};   // xmmword_140BD1F40 +4
    float fallback_width_to_y{}; // xmmword_140BD1F40 +8; nonfinite -> 0.235

    bool related_mode_enabled{}; // HIBYTE(word_140BD2070)
    bool class_rules_enabled{};  // byte_140BD2072
    std::span<const ClassAimPointRuleExact> class_rules{};
};

// Exact 64-byte persistent result/state at dword_140BD48B8.
struct AimPointState64Abi {
    std::int32_t valid{};        // +0
    float aim_x{};               // +4
    float aim_y{};               // +8
    float aim_y_from_box_top{};  // +12
    float aim_y_normalized{};    // +16
    float box_left{};            // +20
    float box_top{};             // +24
    float box_width{};           // +28
    float box_height{};          // +32
    std::int32_t class_id{-1};   // +36
    std::uint8_t output_valid{}; // +40
    std::uint8_t used_related{}; // +41; also function return value
    std::uint8_t cache_valid{};  // +42
    std::uint8_t pad43{};
    float cached_ratio{};        // +44
    float cached_low{};          // +48
    float cached_high{};         // +52
    float cached_extra_y{};      // +56; related-mode helper only
    std::int32_t mode{};         // +60: 0=normal, 1=related, 2=fallback
};
static_assert(sizeof(AimPointState64Abi) == 64);
static_assert(offsetof(AimPointState64Abi, output_valid) == 40);
static_assert(offsetof(AimPointState64Abi, cached_ratio) == 44);
static_assert(offsetof(AimPointState64Abi, mode) == 60);

struct AimPointParametersExact {
    float ratio_low{};
    float ratio_high{};
    float horizontal_offset{};
    float vertical_offset{};
};

// Exact ArmController packing at 0x14004F8AE..0x14004FA13.
AimPointTarget72Abi make_aimpoint_target_record_exact(
    const SelectedTarget104Abi& selected) noexcept;

// 0x14004B540.
AimPointParametersExact resolve_aimpoint_parameters_exact(
    const AimPointTarget72Abi& target,
    const AimPointConfigExact& config) noexcept;

// 0x14004BC30. Mutates the same cache/result fields as native and returns the
// byte written at state+41 (whether the related target path was used).
bool target_to_aimpoint_exact(
    const AimPointTarget72Abi& target,
    AimPointState64Abi& state,
    const AimPointConfigExact& config) noexcept;

} // namespace cvm::recovered
