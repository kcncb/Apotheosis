#include "target_selector_top_exact.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace cvm::recovered {
namespace {

float native_clamp_01(float value) noexcept {
    if (value <= 1.0f)
        return value >= 0.0f ? value : 0.0f;
    return 1.0f;
}

float candidate_iou_exact(
    std::array<float, 4> box,
    CandidateGeometryExact candidate) noexcept {
    const float box_width = std::fmax(box[2] - box[0], 0.0f);
    const float box_height = std::fmax(box[3] - box[1], 0.0f);
    const float candidate_width = std::fmax(
        candidate.right - candidate.left, 0.0f);
    const float candidate_height = std::fmax(
        candidate.bottom - candidate.top, 0.0f);
    const float intersection_width = std::fmax(
        std::fmin(candidate.right, box[2])
            - std::fmax(candidate.left, box[0]),
        0.0f);
    const float intersection_height = std::fmax(
        std::fmin(candidate.bottom, box[3])
            - std::fmax(candidate.top, box[1]),
        0.0f);
    const float intersection = intersection_width * intersection_height;
    return intersection
        / (candidate_width * candidate_height
           + box_width * box_height - intersection + 0.000001f);
}

bool contains_class(
    const std::vector<std::int32_t>& values,
    std::int32_t class_id) noexcept {
    return std::find(values.begin(), values.end(), class_id) != values.end();
}

std::int32_t select_primary_fusion_index(
    const TargetSelectorRuntimeConfig& config,
    std::span<const Detection72Abi> detections,
    const CandidateOverlapMatricesExact& overlap,
    std::int32_t anchor_index) noexcept {
    if (anchor_index < 0
        || static_cast<std::size_t>(anchor_index) >= detections.size()) {
        return anchor_index;
    }
    const auto reverse = config.classes_by_included_class.find(
        detections[static_cast<std::size_t>(anchor_index)].class_id);
    if (reverse == config.classes_by_included_class.end())
        return anchor_index;

    const std::size_t count = detections.size();
    const std::size_t row = static_cast<std::size_t>(anchor_index) * count;
    float best = -std::numeric_limits<float>::infinity();
    std::int32_t selected = anchor_index;
    for (std::size_t index = 0; index < count; ++index) {
        if (!contains_class(reverse->second, detections[index].class_id))
            continue;
        const float coverage = overlap.anchor_coverage[row + index];
        if (coverage > 0.5f && coverage > best) {
            best = coverage;
            selected = static_cast<std::int32_t>(index);
        }
    }
    return selected;
}

std::int32_t select_related_fusion_index(
    const TargetSelectorRuntimeConfig& config,
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    const CandidateOverlapMatricesExact& overlap,
    std::int32_t primary_index,
    std::array<float, 2> origin) noexcept {
    if (primary_index < 0
        || static_cast<std::size_t>(primary_index) >= detections.size()) {
        return -1;
    }
    const auto direct = config.include_by_class.find(
        detections[static_cast<std::size_t>(primary_index)].class_id);
    if (direct == config.include_by_class.end())
        return -1;

    const std::size_t count = detections.size();
    const std::size_t row = static_cast<std::size_t>(primary_index) * count;
    float best_distance_squared = std::numeric_limits<float>::infinity();
    std::int32_t selected = -1;
    for (std::size_t index = 0; index < count; ++index) {
        if (!contains_class(direct->second, detections[index].class_id)
            || !(overlap.anchor_coverage[row + index] > 0.5f)) {
            continue;
        }
        const double dx = geometry[index].center_x
            - static_cast<double>(origin[0]);
        const double dy = geometry[index].center_y
            - static_cast<double>(origin[1]);
        const float distance_squared = static_cast<float>(dy * dy + dx * dx);
        if (best_distance_squared > distance_squared) {
            best_distance_squared = distance_squared;
            selected = static_cast<std::int32_t>(index);
        }
    }
    return selected;
}

bool area_matches_previous_exact(
    CandidateGeometryExact candidate,
    std::array<float, 4> previous) noexcept {
    const float previous_area =
        std::fmax(1.0f, previous[3] - previous[1])
        * std::fmax(1.0f, previous[2] - previous[0]);
    const float candidate_area =
        std::fmax(0.0f, candidate.bottom - candidate.top)
        * std::fmax(0.0f, candidate.right - candidate.left);
    return !(previous_area * 0.5f > candidate_area
             || candidate_area > previous_area * 1.5f);
}

} // namespace

TrackerCandidateGateMetricsExact compute_tracker_candidate_gate_exact(
    std::array<float, 4> tracked_box,
    std::array<double, 2> tracked_center,
    CandidateGeometryExact candidate,
    float search_radius_weight,
    float inverse_search_radius_weight) noexcept {
    TrackerCandidateGateMetricsExact out{};
    out.iou = candidate_iou_exact(tracked_box, candidate);
    const double dx = candidate.center_x - tracked_center[0];
    const double dy = candidate.center_y - tracked_center[1];
    out.distance_squared = static_cast<float>(dy * dy + dx * dx);
    const float tracked_height = std::fmax(
        tracked_box[3] - tracked_box[1], 1.0f);
    const float tracked_width = std::fmax(
        tracked_box[2] - tracked_box[0], 1.0f);
    const float scale = native_clamp_01(search_radius_weight)
        * (std::min)(tracked_height, tracked_width);
    out.radius_squared = scale * scale;

    if (search_radius_weight > 0.0f) {
        if (search_radius_weight < 1.0f) {
            out.enabled = out.iou >= inverse_search_radius_weight
                || out.radius_squared >= out.distance_squared;
        } else {
            out.enabled = out.radius_squared >= out.distance_squared;
        }
    } else {
        out.enabled = out.iou >= inverse_search_radius_weight;
    }
    return out;
}

bool tracker_candidate_gate_exact(
    std::array<float, 4> tracked_box,
    std::array<double, 2> tracked_center,
    CandidateGeometryExact candidate,
    float search_radius_weight,
    float inverse_search_radius_weight) noexcept {
    return compute_tracker_candidate_gate_exact(
        tracked_box, tracked_center, candidate,
        search_radius_weight, inverse_search_radius_weight).enabled;
}

float tracked_candidate_association_score_exact(
    const SelectedTarget104Abi& tracked,
    CandidateGeometryExact candidate) noexcept {
    const float normalizer = std::fmax(
        (std::min)(tracked.effective_width, tracked.effective_height), 1.0f);
    const float tracked_area = std::fmax(
        tracked.effective_height * tracked.effective_width, 1.0f);
    const double dx = candidate.center_x - tracked.effective_center_x;
    const double dy = candidate.center_y - tracked.effective_center_y;
    const float distance = static_cast<float>(std::sqrt(dy * dy + dx * dx));
    const float distance_ratio = native_clamp_01(distance / normalizer);
    const float candidate_area =
        (candidate.bottom - candidate.top)
        * (candidate.right - candidate.left);
    const float area_ratio = native_clamp_01(
        std::fabs(candidate_area - tracked_area) / tracked_area);
    return (1.0f - distance_ratio) * 0.69999999f
        + (1.0f - area_ratio) * 0.30000001f;
}

std::vector<std::uint8_t> filter_candidate_subset_exact(
    const TargetSelectorRuntimeConfig& config,
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    std::span<const std::int32_t> candidate_indices,
    std::array<float, 2> origin,
    float radius,
    std::int32_t radius_mode) {
    std::vector<std::uint8_t> enabled(detections.size(), 0);
    if (config.class_priority_enabled) {
        float maximum = -std::numeric_limits<float>::infinity();
        for (const auto candidate_index : candidate_indices) {
            if (candidate_index < 0
                || static_cast<std::size_t>(candidate_index) >= detections.size()
                || static_cast<std::size_t>(candidate_index) >= geometry.size()) {
                continue;
            }
            const auto class_id = detections[static_cast<std::size_t>(candidate_index)].class_id;
            if (class_id < 0 || class_id >= config.class_count
                || !candidate_passes_radius_filter_exact(
                    geometry[static_cast<std::size_t>(candidate_index)],
                    origin[0], origin[1], radius, radius_mode)) {
                continue;
            }
            const float priority = config.effective_priority_by_class[
                static_cast<std::size_t>(class_id)];
            if (std::isfinite(priority))
                maximum = std::fmax(priority, maximum);
        }
        if (!std::isfinite(maximum))
            return enabled;
        for (const auto candidate_index : candidate_indices) {
            if (candidate_index < 0
                || static_cast<std::size_t>(candidate_index) >= detections.size()
                || static_cast<std::size_t>(candidate_index) >= geometry.size()) {
                continue;
            }
            const auto class_id = detections[static_cast<std::size_t>(candidate_index)].class_id;
            if (class_id >= 0 && class_id < config.class_count
                && candidate_passes_radius_filter_exact(
                    geometry[static_cast<std::size_t>(candidate_index)],
                    origin[0], origin[1], radius, radius_mode)
                && config.effective_priority_by_class[
                    static_cast<std::size_t>(class_id)] == maximum) {
                enabled[static_cast<std::size_t>(candidate_index)] = 1;
            }
        }
        return enabled;
    }

    for (const auto candidate_index : candidate_indices) {
        if (candidate_index < 0
            || static_cast<std::size_t>(candidate_index) >= detections.size()
            || static_cast<std::size_t>(candidate_index) >= geometry.size()) {
            continue;
        }
        const auto class_id = detections[static_cast<std::size_t>(candidate_index)].class_id;
        if (class_id >= 0 && class_id < config.class_count
            && config.active_class_mask[static_cast<std::size_t>(class_id)]
            && candidate_passes_radius_filter_exact(
                geometry[static_cast<std::size_t>(candidate_index)],
                origin[0], origin[1], radius, radius_mode)) {
            enabled[static_cast<std::size_t>(candidate_index)] = 1;
        }
    }
    return enabled;
}

BuiltSelectedTarget144Abi build_fused_target_record_exact(
    const TargetSelectorRuntimeConfig& config,
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    const CandidateOverlapMatricesExact& overlap,
    std::int32_t anchor_index,
    std::array<float, 2> relation_origin) noexcept {
    const auto primary = select_primary_fusion_index(
        config, detections, overlap, anchor_index);
    const auto related = select_related_fusion_index(
        config, detections, geometry, overlap, primary, relation_origin);
    return build_selected_target_record_exact(
        detections, geometry, primary, related);
}

void TargetDetectionHoldStabilizerExact::clear() noexcept {
    previous_.clear();
    ages_.clear();
}

std::vector<std::uint8_t> TargetDetectionHoldStabilizerExact::update(
    std::span<const Detection72Abi> detections,
    std::int32_t min_hold_frames,
    float search_radius_weight,
    float inverse_search_radius_weight) {
    std::vector<std::uint8_t> stable(detections.size(), 0);
    if (min_hold_frames <= 0) {
        clear();
        return stable;
    }
    if (detections.empty()) {
        clear();
        return stable;
    }

    const auto previous_geometry = prepare_candidate_geometry_exact(previous_);
    std::vector<std::uint8_t> previous_used(previous_.size(), 0);
    std::vector<Detection72Abi> next_previous;
    std::vector<std::int32_t> next_ages;
    next_previous.reserve(detections.size());
    next_ages.reserve(detections.size());

    for (std::size_t current_index = 0;
         current_index < detections.size(); ++current_index) {
        const auto current_geometry =
            prepare_candidate_geometry_exact(detections[current_index]);
        const std::array<float, 4> current_box{
            current_geometry.left, current_geometry.top,
            current_geometry.right, current_geometry.bottom};
        const std::array<double, 2> current_center{
            current_geometry.center_x, current_geometry.center_y};
        float best_iou = -std::numeric_limits<float>::infinity();
        std::int32_t best_previous = -1;
        for (std::size_t previous_index = 0;
             previous_index < previous_.size(); ++previous_index) {
            if (previous_used[previous_index]
                || previous_[previous_index].class_id
                    != detections[current_index].class_id
                || !tracker_candidate_gate_exact(
                    current_box, current_center, previous_geometry[previous_index],
                    search_radius_weight, inverse_search_radius_weight)) {
                continue;
            }
            const float iou = candidate_iou_exact(
                current_box, previous_geometry[previous_index]);
            if (iou > best_iou) {
                best_iou = iou;
                best_previous = static_cast<std::int32_t>(previous_index);
            }
        }

        std::int32_t age = 0;
        if (best_previous >= 0) {
            const auto index = static_cast<std::size_t>(best_previous);
            previous_used[index] = 1;
            age = ages_[index] + 1;
        }
        next_previous.push_back(detections[current_index]);
        next_ages.push_back(age);
        if (age >= min_hold_frames)
            stable[current_index] = 1;
    }
    previous_ = std::move(next_previous);
    ages_ = std::move(next_ages);
    return stable;
}

AimTargetSelectorExact::AimTargetSelectorExact(
    TargetSelectorRuntimeConfig config) {
    set_config(std::move(config));
}

void AimTargetSelectorExact::set_config(TargetSelectorRuntimeConfig config) {
    config_ = std::move(config);
    // 0x14005D560 and 0x14005D5C6 reset both persistent trackers whenever
    // normalized selector parameters are applied.
    tracker_.clear();
    hold_.clear();
    tracker_.max_lost_frames = config_.tracker_loss_limit;
    tracker_.search_radius = config_.tracker_search_radius;
}

bool AimTargetSelectorExact::class_is_active(
    std::int32_t class_id) const noexcept {
    return class_id >= 0 && class_id < config_.class_count
        && config_.active_class_mask[static_cast<std::size_t>(class_id)];
}

bool AimTargetSelectorExact::association_class_allowed(
    std::int32_t tracked_class,
    std::int32_t candidate_class) const noexcept {
    const auto found = config_.association_classes_by_class.find(tracked_class);
    if (found == config_.association_classes_by_class.end())
        return candidate_class == tracked_class;
    return contains_class(found->second, candidate_class);
}

void AimTargetSelectorExact::clear_previous_boxes() noexcept {
    previous_primary_ = {};
    previous_primary_.class_id = -1;
    previous_related_ = {};
    previous_related_.class_id = -1;
}

void AimTargetSelectorExact::clear_tracker_and_previous_boxes() noexcept {
    tracker_.clear();
    clear_previous_boxes();
}

void AimTargetSelectorExact::save_previous_boxes(
    const BuiltSelectedTarget144Abi& built) noexcept {
    previous_primary_.valid = true;
    previous_primary_.box = {
        built.source_left, built.source_top,
        built.source_right, built.source_bottom};
    previous_primary_.class_id = built.selected.class_id;
    if (built.related_source_valid && built.selected.related_box_valid) {
        previous_related_.valid = true;
        previous_related_.box = {
            built.related_source_left, built.related_source_top,
            built.related_source_right, built.related_source_bottom};
        previous_related_.class_id = built.selected.related_class_id;
    } else {
        previous_related_ = {};
        previous_related_.class_id = -1;
    }
}

void AimTargetSelectorExact::increment_generation_native() noexcept {
    // Native does not clamp forever: once the counter reaches INT_MAX-1 it
    // restarts at one (0x140061855..0x140061870).
    target_generation_ = target_generation_ < 2'147'483'646
        ? target_generation_ + 1 : 1;
}

const SelectedTarget104Abi* AimTargetSelectorExact::finish_output(
    std::array<float, 2> output_origin_offset) {
    const auto* output = tracker_.build_output(&output_origin_offset);
    if (!output) {
        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }
    selection_status_ = (output->target_kind_or_age > 0 ? 1 : 0) + 1;
    ++consecutive_output_count_;
    return output;
}

const SelectedTarget104Abi* AimTargetSelectorExact::associate_tracked_target(
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    const CandidateOverlapMatricesExact& overlap,
    std::array<float, 2> relation_origin,
    std::array<float, 2> output_origin_offset,
    float radius,
    std::int32_t radius_mode,
    bool& allow_fallback) {
    allow_fallback = true;
    if (!tracker_.active)
        return nullptr;
    if (!class_is_active(tracker_.target.class_id)) {
        clear_tracker_and_previous_boxes();
        return nullptr;
    }

    const std::size_t count = detections.size();
    std::vector<std::uint8_t> base_gate(count, 0);
    const std::array<float, 4> tracked_box{
        tracker_.target.left, tracker_.target.top,
        tracker_.target.right, tracker_.target.bottom};
    const std::array<double, 2> tracked_center{
        tracker_.target.effective_center_x,
        tracker_.target.effective_center_y};
    bool any_base = false;
    for (std::size_t index = 0; index < count; ++index) {
        const bool enabled = association_class_allowed(
            tracker_.target.class_id, detections[index].class_id)
            && tracker_candidate_gate_exact(
                tracked_box, tracked_center, geometry[index],
                config_.search_radius_weight,
                config_.inverse_search_radius_weight);
        base_gate[index] = enabled;
        any_base = any_base || enabled;
    }

    std::int32_t remembered_related_class = previous_related_.class_id;
    if (remembered_related_class < 0) {
        remembered_related_class = tracker_.target.related_box_valid
            ? tracker_.target.related_class_id : -1;
    }

    std::vector<std::uint8_t> candidates(count, 0);
    if (any_base) {
        if (previous_primary_.valid) {
            for (std::size_t index = 0; index < count; ++index) {
                if (base_gate[index]
                    && detections[index].class_id == tracker_.target.class_id
                    && area_matches_previous_exact(
                        geometry[index], previous_primary_.box)) {
                    candidates[index] = 1;
                }
            }
        }
        if (remembered_related_class >= 0 && previous_related_.valid) {
            for (std::size_t index = 0; index < count; ++index) {
                if (base_gate[index]
                    && detections[index].class_id == remembered_related_class
                    && area_matches_previous_exact(
                        geometry[index], previous_related_.box)) {
                    candidates[index] = 1;
                }
            }
        }

        if (std::find(candidates.begin(), candidates.end(), std::uint8_t{1})
            == candidates.end()) {
            for (std::size_t index = 0; index < count; ++index) {
                if (!base_gate[index]
                    || detections[index].class_id == tracker_.target.class_id
                    || (remembered_related_class >= 0
                        && detections[index].class_id
                            == remembered_related_class)) {
                    continue;
                }
                if (!candidate_passes_radius_filter_exact(
                        geometry[index], relation_origin[0], relation_origin[1],
                        radius, radius_mode)) {
                    continue;
                }
                const std::array<float, 2> candidate_origin{
                    static_cast<float>(geometry[index].center_x),
                    static_cast<float>(geometry[index].center_y)};
                const auto built = build_fused_target_record_exact(
                    config_, detections, geometry, overlap,
                    static_cast<std::int32_t>(index), candidate_origin);
                const auto candidate_class = detections[index].class_id;
                if (built.selected.class_id == candidate_class
                    || (built.selected.related_box_valid
                        && built.selected.related_class_id == candidate_class)) {
                    candidates[index] = 1;
                }
            }
        }
    }

    std::vector<float> scores(count,
        -std::numeric_limits<float>::infinity());
    for (std::size_t index = 0; index < count; ++index) {
        if (candidates[index]) {
            scores[index] = tracked_candidate_association_score_exact(
                tracker_.target, geometry[index]);
        }
    }
    const auto selected = select_best_masked_score_exact(candidates, scores);
    if (selected < 0) {
        tracker_.update(nullptr);
        allow_fallback = false;
        if (!tracker_.active) {
            clear_previous_boxes();
            return nullptr;
        }
        return finish_output(output_origin_offset);
    }

    const std::int32_t old_lost_frames = tracker_.lost_frames;
    const auto built = build_fused_target_record_exact(
        config_, detections, geometry, overlap, selected, relation_origin);
    save_previous_boxes(built);
    if (old_lost_frames <= 0) {
        tracker_.update(&built.selected);
    } else {
        increment_generation_native();
        tracker_.initialize(built.selected);
    }
    allow_fallback = false;
    return finish_output(output_origin_offset);
}

const SelectedTarget104Abi* AimTargetSelectorExact::select_aim_target(
    std::span<const Detection72Abi> detections,
    std::array<float, 2> relation_and_radius_origin,
    std::array<float, 2> output_origin_offset,
    float radius,
    std::int32_t radius_mode) {
    tracker_reset_flag_ = 0;
    const auto geometry = prepare_candidate_geometry_exact(detections);
    const auto overlap = compute_candidate_overlap_matrices_exact(geometry);

    std::vector<std::uint8_t> stable_mask;
    if (config_.min_hold_frames > 0) {
        stable_mask = hold_.update(
            detections, config_.min_hold_frames,
            config_.search_radius_weight,
            config_.inverse_search_radius_weight);
    } else {
        hold_.clear();
    }

    if (config_.active_classes.empty() || config_.class_count <= 0) {
        clear_tracker_and_previous_boxes();
        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }

    if (!detections.empty()) {
        if (tracker_.active) {
            bool allow_fallback = true;
            if (const auto* output = associate_tracked_target(
                    detections, geometry, overlap,
                    relation_and_radius_origin, output_origin_offset,
                    radius, radius_mode, allow_fallback)) {
                return output;
            }
            if (!allow_fallback) {
                if (!tracker_.active)
                    tracker_reset_flag_ = 1;
                selection_status_ = 0;
                consecutive_output_count_ = 0;
                return nullptr;
            }
        }
        if (tracker_.active) {
            selection_status_ = 0;
            consecutive_output_count_ = 0;
            return nullptr;
        }

        std::vector<std::int32_t> candidate_indices;
        candidate_indices.reserve(detections.size());
        for (std::size_t index = 0; index < detections.size(); ++index) {
            if (config_.min_hold_frames <= 0 || stable_mask[index])
                candidate_indices.push_back(static_cast<std::int32_t>(index));
        }
        const auto enabled = filter_candidate_subset_exact(
            config_, detections, geometry, candidate_indices,
            relation_and_radius_origin, radius, radius_mode);
        std::vector<float> scores(detections.size(),
            -std::numeric_limits<float>::infinity());
        for (std::size_t index = 0; index < detections.size(); ++index) {
            if (enabled[index]) {
                scores[index] = compute_candidate_priority_score_exact(
                    geometry[index],
                    relation_and_radius_origin[0],
                    relation_and_radius_origin[1],
                    config_.acquire_center_weight,
                    config_.normalizer_x,
                    config_.normalizer_y);
            }
        }
        const auto selected = select_best_masked_score_exact(enabled, scores);
        if (selected >= 0) {
            const auto built = build_fused_target_record_exact(
                config_, detections, geometry, overlap, selected,
                relation_and_radius_origin);
            save_previous_boxes(built);
            increment_generation_native();
            tracker_.initialize(built.selected);
            return finish_output(output_origin_offset);
        }

        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }

    if (!tracker_.active) {
        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }
    if (!class_is_active(tracker_.target.class_id)) {
        clear_tracker_and_previous_boxes();
        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }
    tracker_.update(nullptr);
    if (!tracker_.active) {
        clear_previous_boxes();
        tracker_reset_flag_ = 1;
        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }
    return finish_output(output_origin_offset);
}

const SelectedTarget104Abi*
AimTargetSelectorExact::select_tracked_detection_index(
    std::span<const Detection72Abi> detections,
    std::int32_t detection_index,
    std::array<float, 2> relation_origin,
    std::array<float, 2> output_origin_offset) {
    tracker_reset_flag_ = 0;
    const auto geometry = prepare_candidate_geometry_exact(detections);
    const auto overlap = compute_candidate_overlap_matrices_exact(geometry);
    if (detection_index < 0
        || static_cast<std::size_t>(detection_index) >= detections.size()) {
        if (tracker_.active) {
            tracker_.update(nullptr);
            if (tracker_.active)
                return finish_output(output_origin_offset);
            clear_previous_boxes();
            tracker_reset_flag_ = 1;
        }
        selection_status_ = 0;
        consecutive_output_count_ = 0;
        return nullptr;
    }

    const bool was_active = tracker_.active;
    const bool was_lost = was_active && tracker_.lost_frames > 0;
    const auto built = build_fused_target_record_exact(
        config_, detections, geometry, overlap,
        detection_index, relation_origin);
    save_previous_boxes(built);
    if (!was_active || was_lost) {
        increment_generation_native();
        tracker_.initialize(built.selected);
    } else {
        tracker_.update(&built.selected);
    }
    return finish_output(output_origin_offset);
}

} // namespace cvm::recovered
