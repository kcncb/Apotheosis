#pragma once

#include "target_selector_config_exact.hpp"
#include "target_tracker_exact.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace cvm::recovered {

struct SavedTargetSourceBoxExact {
    bool valid{};
    std::array<float, 4> box{}; // left, top, right, bottom
    std::int32_t class_id{-1};
};

// Persistent detector hold filter at selector +2136 (0x140064120).  A fresh
// detection starts at age zero; therefore min_hold_frames == 1 first admits a
// same-class detection on the second consecutively associated frame.
class TargetDetectionHoldStabilizerExact {
public:
    std::vector<std::uint8_t> update(
        std::span<const Detection72Abi> detections,
        std::int32_t min_hold_frames,
        float search_radius_weight,
        float inverse_search_radius_weight);

    void clear() noexcept;
    const std::vector<std::int32_t>& ages() const noexcept { return ages_; }

private:
    std::vector<Detection72Abi> previous_{};
    std::vector<std::int32_t> ages_{};
};

// Pure cores used by 0x140060220, 0x1400637D0/0x140063C10 and 0x140060830.
std::vector<std::uint8_t> filter_candidate_subset_exact(
    const TargetSelectorRuntimeConfig& config,
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    std::span<const std::int32_t> candidate_indices,
    std::array<float, 2> origin,
    float radius,
    std::int32_t radius_mode);

struct TrackerCandidateGateMetricsExact {
    float iou{};
    float distance_squared{};
    float radius_squared{};
    bool enabled{};
};

TrackerCandidateGateMetricsExact compute_tracker_candidate_gate_exact(
    std::array<float, 4> tracked_box,
    std::array<double, 2> tracked_center,
    CandidateGeometryExact candidate,
    float search_radius_weight,
    float inverse_search_radius_weight) noexcept;

bool tracker_candidate_gate_exact(
    std::array<float, 4> tracked_box,
    std::array<double, 2> tracked_center,
    CandidateGeometryExact candidate,
    float search_radius_weight,
    float inverse_search_radius_weight) noexcept;

float tracked_candidate_association_score_exact(
    const SelectedTarget104Abi& tracked,
    CandidateGeometryExact candidate) noexcept;

BuiltSelectedTarget144Abi build_fused_target_record_exact(
    const TargetSelectorRuntimeConfig& config,
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    const CandidateOverlapMatricesExact& overlap,
    std::int32_t anchor_index,
    std::array<float, 2> relation_origin) noexcept;

// Portable, stateful rewrite of both native selector entry points:
//   0x1400612A0 select_aim_target
//   0x1400605E0 select_tracked_detection_index
// It retains the native status/reset/generation counters and source-box
// association state in addition to the Kalman tracker itself.
class AimTargetSelectorExact {
public:
    explicit AimTargetSelectorExact(TargetSelectorRuntimeConfig config);

    void set_config(TargetSelectorRuntimeConfig config);

    const SelectedTarget104Abi* select_aim_target(
        std::span<const Detection72Abi> detections,
        std::array<float, 2> relation_and_radius_origin,
        std::array<float, 2> output_origin_offset,
        float radius,
        std::int32_t radius_mode);

    const SelectedTarget104Abi* select_tracked_detection_index(
        std::span<const Detection72Abi> detections,
        std::int32_t detection_index,
        std::array<float, 2> relation_origin,
        std::array<float, 2> output_origin_offset);

    const TargetSelectorRuntimeConfig& config() const noexcept { return config_; }
    const TargetTrackerExact& tracker() const noexcept { return tracker_; }
    TargetTrackerExact& tracker() noexcept { return tracker_; }
    const TargetDetectionHoldStabilizerExact& hold_stabilizer() const noexcept {
        return hold_;
    }
    const SavedTargetSourceBoxExact& previous_primary() const noexcept {
        return previous_primary_;
    }
    const SavedTargetSourceBoxExact& previous_related() const noexcept {
        return previous_related_;
    }

    std::int32_t selection_status() const noexcept { return selection_status_; }
    std::int32_t tracker_reset_flag() const noexcept { return tracker_reset_flag_; }
    std::int32_t consecutive_output_count() const noexcept {
        return consecutive_output_count_;
    }
    std::int32_t target_generation() const noexcept { return target_generation_; }

private:
    bool class_is_active(std::int32_t class_id) const noexcept;
    bool association_class_allowed(
        std::int32_t tracked_class,
        std::int32_t candidate_class) const noexcept;
    void clear_previous_boxes() noexcept;
    void clear_tracker_and_previous_boxes() noexcept;
    void save_previous_boxes(const BuiltSelectedTarget144Abi& built) noexcept;
    void increment_generation_native() noexcept;
    const SelectedTarget104Abi* finish_output(
        std::array<float, 2> output_origin_offset);
    const SelectedTarget104Abi* associate_tracked_target(
        std::span<const Detection72Abi> detections,
        std::span<const CandidateGeometryExact> geometry,
        const CandidateOverlapMatricesExact& overlap,
        std::array<float, 2> relation_origin,
        std::array<float, 2> output_origin_offset,
        float radius,
        std::int32_t radius_mode,
        bool& allow_fallback);

    TargetSelectorRuntimeConfig config_{};
    TargetTrackerExact tracker_{};
    TargetDetectionHoldStabilizerExact hold_{};
    SavedTargetSourceBoxExact previous_primary_{};
    SavedTargetSourceBoxExact previous_related_{};
    std::int32_t selection_status_{};       // selector +88
    std::int32_t tracker_reset_flag_{};     // selector +92
    std::int32_t consecutive_output_count_{}; // selector +96
    std::int32_t target_generation_{};      // selector +100
};

} // namespace cvm::recovered
