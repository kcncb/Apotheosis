#pragma once

#include "target_region_exact.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cvm::recovered {

// Detector result consumed by 0x14005E440.  The native vector stride is 72
// bytes; only the first 24 bytes are used by the target-selection preparation
// stage.  The remaining bytes are detector/tracker metadata and are preserved.
struct Detection72Abi {
    float left{};                       // +0
    float top{};                        // +4
    float width{};                      // +8
    float height{};                     // +12
    std::int32_t class_id{};            // +16
    float confidence{};                 // +20
    std::array<std::uint8_t, 48> metadata{}; // +24
};
static_assert(sizeof(Detection72Abi) == 72);
static_assert(offsetof(Detection72Abi, class_id) == 16);

struct CandidateGeometryExact {
    float left{};
    float top{};
    float right{};
    float bottom{};
    double center_x{};
    double center_y{};
};

// First 104 bytes of the record returned by 0x1400605E0/0x1400612A0 and
// consumed by ArmController at 0x14004F85F..0x14004F8A6.
struct SelectedTarget104Abi {
    std::int32_t class_id{};             // +0
    float confidence{};                  // +4
    float left{};                        // +8
    float top{};                         // +12
    float right{};                       // +16
    float bottom{};                      // +20
    std::uint8_t related_box_valid{};    // +24
    std::array<std::uint8_t, 3> pad25{};
    std::int32_t related_class_id{-1};   // +28
    float related_confidence{};          // +32
    float related_left{};                // +36
    float related_top{};                 // +40
    float related_right{};               // +44
    float related_bottom{};              // +48
    std::uint8_t target_flag{};          // +52
    std::array<std::uint8_t, 3> pad53{};
    std::int32_t target_kind_or_age{};   // +56
    std::uint8_t predicted_center_valid{}; // +60
    std::array<std::uint8_t, 3> pad61{};
    double primary_center_x{};           // +64
    double primary_center_y{};           // +72
    double effective_center_x{};         // +80
    double effective_center_y{};         // +88
    float effective_width{};             // +96
    float effective_height{};            // +100
};
static_assert(sizeof(SelectedTarget104Abi) == 104);
static_assert(offsetof(SelectedTarget104Abi, left) == 8);
static_assert(offsetof(SelectedTarget104Abi, target_flag) == 52);
static_assert(offsetof(SelectedTarget104Abi, effective_center_x) == 80);
static_assert(offsetof(SelectedTarget104Abi, effective_width) == 96);

// Full transient record written by 0x14005F730 before it is copied into the
// tracker's compact state. The extension retains source boxes used by later
// association passes.
struct BuiltSelectedTarget144Abi {
    SelectedTarget104Abi selected;       // +0..+103
    float source_left{};                 // +104
    float source_top{};                  // +108
    float source_right{};                // +112
    float source_bottom{};               // +116
    std::uint8_t related_source_valid{}; // +120
    std::array<std::uint8_t, 3> pad121{};
    float related_source_left{};         // +124
    float related_source_top{};          // +128
    float related_source_right{};        // +132
    float related_source_bottom{};       // +136
    std::int32_t related_source_tag{};   // +140
};
static_assert(sizeof(BuiltSelectedTarget144Abi) == 144);
static_assert(offsetof(BuiltSelectedTarget144Abi, source_left) == 104);
static_assert(offsetof(BuiltSelectedTarget144Abi, related_source_valid) == 120);

// Record construction core of 0x14005F730. primary_index is the index after
// 0x14005F4C0's overlap substitution. related_index < 0 means no related box.
BuiltSelectedTarget144Abi build_selected_target_record_exact(
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    std::int32_t primary_index,
    std::int32_t related_index = -1) noexcept;

// 0x14005E440's input-geometry preparation loop.
CandidateGeometryExact prepare_candidate_geometry_exact(
    const Detection72Abi& detection) noexcept;

std::vector<CandidateGeometryExact> prepare_candidate_geometry_exact(
    std::span<const Detection72Abi> detections);

// 0x14005E7F0. Matrices are row-major [anchor * count + candidate].  The
// normalized overlap denominator is the anchor area (not union area).
struct CandidateOverlapMatricesExact {
    std::vector<float> widths;
    std::vector<float> heights;
    std::vector<float> areas;
    std::vector<float> intersection_left;
    std::vector<float> intersection_top;
    std::vector<float> intersection_right;
    std::vector<float> intersection_bottom;
    std::vector<float> intersection_areas;
    std::vector<float> anchor_coverage;
};

CandidateOverlapMatricesExact compute_candidate_overlap_matrices_exact(
    std::span<const CandidateGeometryExact> candidates);

// The pure scoring core of 0x14005F4C0 after its class relationship mask has
// been resolved. Only coverage > 0.5 participates; strict comparisons retain
// the first equal candidate. If none qualifies, anchor_index is returned.
std::int32_t select_related_by_anchor_coverage_exact(
    std::int32_t anchor_index,
    std::span<const std::uint8_t> class_allowed,
    std::span<const float> anchor_coverage_row) noexcept;

// Per-candidate priority at 0x140061698..0x140061777. distance_weight is the
// direct native value at selector +56; it is not clamped in this function.
// normalizer_a/b are native selector +68/+72 and are each floored to one
// before their minimum is taken.
float compute_candidate_priority_score_exact(
    CandidateGeometryExact candidate,
    float origin_x,
    float origin_y,
    float distance_weight,
    std::int32_t normalizer_a,
    std::int32_t normalizer_b) noexcept;

// 0x14005FF50. radius <= 0 disables the filter before any validity checks.
// mode == 1 tests circle/rectangle intersection; every other mode tests the
// candidate center only.
bool candidate_passes_radius_filter_exact(
    CandidateGeometryExact candidate,
    float origin_x,
    float origin_y,
    float radius,
    std::int32_t mode) noexcept;

// 0x14004B030, operating on the returned 104-byte selection record.
bool selected_target_passes_radius_filter_exact(
    const SelectedTarget104Abi& target,
    float origin_x,
    float origin_y,
    float radius,
    std::int32_t mode) noexcept;

// 0x140060470. Only enabled entries participate; comparison is strict, so the
// first equal score wins. A first enabled NaN is retained exactly as native.
std::int32_t select_best_masked_score_exact(
    std::span<const std::uint8_t> enabled,
    std::span<const float> scores) noexcept;

// 0x1400493F0 and 0x14004A810.
float target_box_intersection_area_exact(
    TargetBoxF first,
    TargetBoxF second) noexcept;

bool target_records_match_exact(
    const SelectedTarget104Abi& first,
    const SelectedTarget104Abi& second) noexcept;

} // namespace cvm::recovered
