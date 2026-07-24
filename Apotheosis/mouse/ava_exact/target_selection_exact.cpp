#include "target_selection_exact.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cvm::recovered {
namespace {

bool finite_point(float x, float y) noexcept {
    return std::isfinite(x) && std::isfinite(y);
}

bool finite_point(double x, double y) noexcept {
    return std::isfinite(x) && std::isfinite(y);
}

float native_clamp_01(float value) noexcept {
    // Exact branch ordering generated at 0x14006172D..0x14006176F.  In
    // particular an unordered NaN reaches the upper fallback (1.0f).
    if (value <= 1.0f)
        return value >= 0.0f ? value : 0.0f;
    return 1.0f;
}

// Squaring is intentionally performed in single precision before the result
// is promoted for comparison, matching MULSS in both native radius helpers.
double radius_squared(float radius) noexcept {
    const float squared = radius * radius;
    return static_cast<double>(squared);
}

double point_to_rect_distance_squared(
    double x,
    double y,
    double left,
    double top,
    double right,
    double bottom) noexcept {
    const double min_x = (std::min)(left, right);
    const double max_x = (std::max)(left, right);
    const double min_y = (std::min)(top, bottom);
    const double max_y = (std::max)(top, bottom);

    // This form preserves the native clamp branch ordering, including the
    // behavior of signed zero and unordered comparisons.
    const double nearest_x = min_x > x ? min_x : (x > max_x ? max_x : x);
    const double nearest_y = min_y > y ? min_y : (y > max_y ? max_y : y);
    const double dx = nearest_x - x;
    const double dy = nearest_y - y;
    return dy * dy + dx * dx;
}

bool radius_filter_impl(
    double center_x,
    double center_y,
    double left,
    double top,
    double right,
    double bottom,
    float origin_x,
    float origin_y,
    float radius,
    std::int32_t mode) noexcept {
    if (radius <= 0.0f)
        return true;
    if (!finite_point(origin_x, origin_y)
        || !finite_point(center_x, center_y))
        return false;

    const double limit = radius_squared(radius);
    if (mode != 1) {
        const double dx = center_x - static_cast<double>(origin_x);
        const double dy = center_y - static_cast<double>(origin_y);
        return limit >= dy * dy + dx * dx;
    }

    if (!std::isfinite(left) || !std::isfinite(top)
        || !std::isfinite(right) || !std::isfinite(bottom))
        return false;
    return limit >= point_to_rect_distance_squared(
        static_cast<double>(origin_x), static_cast<double>(origin_y),
        left, top, right, bottom);
}

TargetBoxF record_box(const SelectedTarget104Abi& target) noexcept {
    return TargetBoxF{
        target.left,
        target.top,
        target.right - target.left,
        target.bottom - target.top,
    };
}

} // namespace

CandidateGeometryExact prepare_candidate_geometry_exact(
    const Detection72Abi& detection) noexcept {
    CandidateGeometryExact out{};
    out.left = detection.left;
    out.top = detection.top;
    out.right = detection.left + detection.width;
    out.bottom = detection.top + detection.height;

    // Native converts each endpoint separately with CVTPS2PD, then ADDSD and
    // MULSD 0.5; it does not average in float.
    out.center_x = (static_cast<double>(out.right)
        + static_cast<double>(out.left)) * 0.5;
    out.center_y = (static_cast<double>(out.bottom)
        + static_cast<double>(out.top)) * 0.5;
    return out;
}

std::vector<CandidateGeometryExact> prepare_candidate_geometry_exact(
    std::span<const Detection72Abi> detections) {
    std::vector<CandidateGeometryExact> out;
    out.reserve(detections.size());
    for (const auto& detection : detections)
        out.push_back(prepare_candidate_geometry_exact(detection));
    return out;
}

BuiltSelectedTarget144Abi build_selected_target_record_exact(
    std::span<const Detection72Abi> detections,
    std::span<const CandidateGeometryExact> geometry,
    std::int32_t primary_index,
    std::int32_t related_index) noexcept {
    BuiltSelectedTarget144Abi out{};
    out.selected.related_class_id = -1;
    if (primary_index < 0
        || static_cast<std::size_t>(primary_index) >= detections.size()
        || static_cast<std::size_t>(primary_index) >= geometry.size())
        return out;

    const auto& source = detections[static_cast<std::size_t>(primary_index)];
    const auto& box = geometry[static_cast<std::size_t>(primary_index)];
    out.selected.class_id = source.class_id;
    out.selected.confidence = source.confidence;
    out.selected.left = box.left;
    out.selected.top = box.top;
    out.selected.right = box.right;
    out.selected.bottom = box.bottom;
    out.selected.primary_center_x = box.center_x;
    out.selected.primary_center_y = box.center_y;
    out.selected.effective_center_x = box.center_x;
    out.selected.effective_center_y = box.center_y;
    out.selected.effective_width = box.right - box.left;
    out.selected.effective_height = box.bottom - box.top;
    out.source_left = box.left;
    out.source_top = box.top;
    out.source_right = box.right;
    out.source_bottom = box.bottom;

    if (related_index < 0
        || static_cast<std::size_t>(related_index) >= detections.size()
        || static_cast<std::size_t>(related_index) >= geometry.size())
        return out;

    const auto& related_source = detections[static_cast<std::size_t>(related_index)];
    const auto& related = geometry[static_cast<std::size_t>(related_index)];
    out.selected.related_box_valid = 1;
    out.selected.related_class_id = related_source.class_id;
    out.selected.related_confidence = related_source.confidence;
    out.selected.related_left = related.left;
    out.selected.related_top = related.top;
    out.selected.related_right = related.right;
    out.selected.related_bottom = related.bottom;

    // 0x14005FD51..0x14005FDBB uses ordered scalar comparisons rather than
    // MINSS/MAXSS.  On an unordered comparison the related operand is chosen
    // for all four coordinates, which differs from std::min/std::max when the
    // existing primary coordinate is NaN.
    out.selected.left = out.selected.left <= related.left
        ? out.selected.left : related.left;
    out.selected.top = out.selected.top <= related.top
        ? out.selected.top : related.top;
    out.selected.right = related.right <= out.selected.right
        ? out.selected.right : related.right;
    out.selected.bottom = related.bottom <= out.selected.bottom
        ? out.selected.bottom : related.bottom;
    out.selected.effective_width = out.selected.right - out.selected.left;
    out.selected.effective_height = out.selected.bottom - out.selected.top;
    out.selected.effective_center_x =
        (static_cast<double>(out.selected.right)
            + static_cast<double>(out.selected.left)) * 0.5;
    out.selected.effective_center_y =
        (static_cast<double>(out.selected.bottom)
            + static_cast<double>(out.selected.top)) * 0.5;
    out.selected.primary_center_x = out.selected.effective_center_x;
    out.selected.primary_center_y = out.selected.effective_center_y;
    out.selected.predicted_center_valid = 0;

    out.related_source_valid = 1;
    out.related_source_left = related.left;
    out.related_source_top = related.top;
    out.related_source_right = related.right;
    out.related_source_bottom = related.bottom;
    return out;
}

CandidateOverlapMatricesExact compute_candidate_overlap_matrices_exact(
    std::span<const CandidateGeometryExact> candidates) {
    CandidateOverlapMatricesExact out;
    const std::size_t count = candidates.size();
    out.widths.resize(count);
    out.heights.resize(count);
    out.areas.resize(count);
    const std::size_t matrix_size = count * count;
    out.intersection_left.resize(matrix_size);
    out.intersection_top.resize(matrix_size);
    out.intersection_right.resize(matrix_size);
    out.intersection_bottom.resize(matrix_size);
    out.intersection_areas.resize(matrix_size);
    out.anchor_coverage.resize(matrix_size);

    for (std::size_t i = 0; i < count; ++i) {
        out.widths[i] = candidates[i].right - candidates[i].left;
        out.heights[i] = candidates[i].bottom - candidates[i].top;
        out.areas[i] = out.heights[i] * out.widths[i];
    }

    for (std::size_t anchor = 0; anchor < count; ++anchor) {
        for (std::size_t candidate = 0; candidate < count; ++candidate) {
            const std::size_t at = anchor * count + candidate;
            out.intersection_left[at] = (std::max)(
                candidates[anchor].left, candidates[candidate].left);
            out.intersection_top[at] = (std::max)(
                candidates[anchor].top, candidates[candidate].top);
            out.intersection_right[at] = (std::min)(
                candidates[anchor].right, candidates[candidate].right);
            out.intersection_bottom[at] = (std::min)(
                candidates[anchor].bottom, candidates[candidate].bottom);
            const float overlap_height = std::fmax(
                out.intersection_bottom[at] - out.intersection_top[at], 0.0f);
            const float overlap_width = std::fmax(
                out.intersection_right[at] - out.intersection_left[at], 0.0f);
            out.intersection_areas[at] = overlap_height * overlap_width;
            out.anchor_coverage[at] = out.intersection_areas[at]
                / (out.areas[anchor] + 0.000001f);
        }
    }
    return out;
}

std::int32_t select_related_by_anchor_coverage_exact(
    std::int32_t anchor_index,
    std::span<const std::uint8_t> class_allowed,
    std::span<const float> anchor_coverage_row) noexcept {
    const std::size_t count = (std::min)(
        class_allowed.size(), anchor_coverage_row.size());
    float best = -std::numeric_limits<float>::infinity();
    std::int32_t selected = anchor_index;
    for (std::size_t i = 0; i < count; ++i) {
        const float coverage = anchor_coverage_row[i];
        if (class_allowed[i] && coverage > 0.5f && coverage > best) {
            best = coverage;
            selected = static_cast<std::int32_t>(i);
        }
    }
    return selected;
}

float compute_candidate_priority_score_exact(
    CandidateGeometryExact candidate,
    float origin_x,
    float origin_y,
    float distance_weight,
    std::int32_t normalizer_a,
    std::int32_t normalizer_b) noexcept {
    const std::int32_t floor_a = normalizer_a >= 1 ? normalizer_a : 1;
    const std::int32_t floor_b = normalizer_b >= 1 ? normalizer_b : 1;
    const std::int32_t normalization_i = (std::min)(floor_a, floor_b);
    const float normalization = static_cast<float>(normalization_i);

    const double dx = candidate.center_x - static_cast<double>(origin_x);
    const double dy = candidate.center_y - static_cast<double>(origin_y);
    const double distance_squared = dy * dy + dx * dx;
    const float distance = static_cast<float>(std::sqrt(distance_squared));
    const float normalized_distance = native_clamp_01(distance / normalization);

    const float width = candidate.right - candidate.left;
    const float height = candidate.bottom - candidate.top;
    const float normalized_size = native_clamp_01(
        (std::min)(width, height) / normalization);
    const float proximity = 1.0f - normalized_distance;
    const float size_weight = 1.0f - distance_weight;
    return proximity * distance_weight + size_weight * normalized_size;
}

bool candidate_passes_radius_filter_exact(
    CandidateGeometryExact candidate,
    float origin_x,
    float origin_y,
    float radius,
    std::int32_t mode) noexcept {
    return radius_filter_impl(
        candidate.center_x, candidate.center_y,
        candidate.left, candidate.top, candidate.right, candidate.bottom,
        origin_x, origin_y, radius, mode);
}

bool selected_target_passes_radius_filter_exact(
    const SelectedTarget104Abi& target,
    float origin_x,
    float origin_y,
    float radius,
    std::int32_t mode) noexcept {
    return radius_filter_impl(
        target.effective_center_x, target.effective_center_y,
        target.left, target.top, target.right, target.bottom,
        origin_x, origin_y, radius, mode);
}

std::int32_t select_best_masked_score_exact(
    std::span<const std::uint8_t> enabled,
    std::span<const float> scores) noexcept {
    const std::size_t count = (std::min)(enabled.size(), scores.size());
    float best = -std::numeric_limits<float>::infinity();
    std::int32_t selected = -1;
    for (std::size_t i = 0; i < count; ++i) {
        if (enabled[i] && (selected < 0 || scores[i] > best)) {
            best = scores[i];
            selected = static_cast<std::int32_t>(i);
        }
    }
    return selected;
}

float target_box_intersection_area_exact(
    TargetBoxF first,
    TargetBoxF second) noexcept {
    if (!valid_target_box(first) || !valid_target_box(second))
        return 0.0f;
    const float overlap_width = std::fmax(
        std::fmin(second.left + second.width, first.left + first.width)
            - (std::max)(second.left, first.left),
        0.0f);
    const float overlap_height = std::fmax(
        std::fmin(second.top + second.height, first.top + first.height)
            - (std::max)(second.top, first.top),
        0.0f);
    return overlap_width * overlap_height;
}

bool target_records_match_exact(
    const SelectedTarget104Abi& first,
    const SelectedTarget104Abi& second) noexcept {
    if (first.class_id != second.class_id)
        return false;
    const TargetBoxF first_box = record_box(first);
    const TargetBoxF second_box = record_box(second);
    if (!valid_target_box(first_box) || !valid_target_box(second_box))
        return false;

    const float intersection = target_box_intersection_area_exact(
        first_box, second_box);
    if (intersection > 0.0f) {
        const float first_area = std::fmax(first_box.height, 0.0f)
            * std::fmax(first_box.width, 0.0f);
        const float second_area = std::fmax(second_box.height, 0.0f)
            * std::fmax(second_box.width, 0.0f);
        const float union_area = (second_area + first_area) - intersection;
        if (union_area > 0.0f
            && intersection / union_area >= 0.15000001f)
            return true;
    }

    const float first_center_x = first.left + first_box.width * 0.5f;
    const float second_center_x = second.left + second_box.width * 0.5f;
    const float dx = first_center_x - second_center_x;
    const float first_center_y = first.top + first_box.height * 0.5f;
    const float second_center_y = second.top + second_box.height * 0.5f;
    const float dy = first_center_y - second_center_y;
    const float max_width = (std::max)(first_box.width, second_box.width);
    const float max_height = (std::max)(first_box.height, second_box.height);
    const float scale = std::fmax(
        std::fmax((std::min)(max_width, max_height), 1.0f) * 0.75f,
        6.0f);
    return scale * scale >= dy * dy + dx * dx;
}

} // namespace cvm::recovered
