#include "target_selector_config_exact.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace cvm::recovered {
namespace {

float native_clamp_01(float value) noexcept {
    if (value <= 1.0f)
        return value >= 0.0f ? value : 0.0f;
    return 1.0f;
}

template <class T>
void append_unique(std::vector<T>& values, T value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

} // namespace

TargetSelectorRuntimeConfig normalize_target_selector_config_exact(
    const TargetSelectorConfigInput& input) {
    TargetSelectorRuntimeConfig out{};

    // sub_14004A9D0: reject negative class IDs and non-finite priorities;
    // include IDs are nonnegative and deduplicated while retaining first order.
    for (const auto& rule : input.class_priority) {
        if (rule.class_id < 0 || !std::isfinite(rule.priority))
            continue;
        TargetClassPriorityRuntime normalized{
            rule.class_id, rule.priority, {}};
        for (const auto included : rule.include) {
            if (included >= 0)
                append_unique(normalized.include, included);
        }
        out.rules.push_back(std::move(normalized));
    }

    // If every explicit rule was rejected, native synthesizes priority 1.0
    // entries from the target-label vector.
    if (out.rules.empty()) {
        for (const auto class_id : input.target_labels) {
            if (class_id >= 0)
                out.rules.push_back(TargetClassPriorityRuntime{
                    class_id, 1.0f, {}});
        }
    }

    // sub_14004AB90: an explicitly nonempty valid target-label list wins.
    for (const auto class_id : input.target_labels) {
        if (class_id >= 0)
            append_unique(out.active_classes, class_id);
    }
    if (out.active_classes.empty()) {
        // 0x14005D993..0x14005DC62 uses a std::set only in the no-explicit-
        // target-label branch.  Consequently rule classes and their direct
        // include classes become a sorted, unique active list.  An explicit
        // target-label list, handled above, wins and is not expanded.
        std::set<std::int32_t> closure;
        std::map<std::int32_t, std::vector<std::int32_t>> final_includes;
        for (const auto& rule : out.rules) {
            closure.insert(rule.class_id);
            final_includes[rule.class_id] = rule.include;
        }
        if (input.head_body_fusion_enabled) {
            // The forward include map is overwrite-on-duplicate.  Expansion
            // therefore uses only the final include vector for each rule key;
            // the reverse map built later intentionally still remembers every
            // accepted duplicate rule.
            for (const auto& [class_id, includes] : final_includes) {
                (void)class_id;
                closure.insert(includes.begin(), includes.end());
            }
        }
        out.active_classes.assign(closure.begin(), closure.end());
    }

    out.class_priority_enabled = input.class_priority_enabled;
    out.head_body_fusion_enabled = input.head_body_fusion_enabled;
    out.search_radius = native_clamp_01(input.search_radius);
    // 0x14005D4C3 calls _fdclass before clamping this field; every non-finite
    // value is replaced with the built-in 0.7 default.  search_radius does
    // not have this precheck and therefore keeps its unordered-upper fallback.
    out.acquire_center_weight = native_clamp_01(
        std::isfinite(input.acquire_center_weight)
            ? input.acquire_center_weight : 0.69999999f);
    out.max_lost_frames = (std::max)(input.max_lost_frames, 0);
    out.min_hold_frames = (std::max)(input.min_hold_frames, 0);
    out.normalizer_x = (std::max)(input.normalizer_x, 0);
    out.normalizer_y = (std::max)(input.normalizer_y, 0);

    // sub_14005D440 repeats normalization and creates tracker-side values.
    out.search_radius_weight = out.search_radius;
    out.inverse_search_radius_weight = 1.0f - out.search_radius;
    out.tracker_loss_limit = out.max_lost_frames;
    out.tracker_search_radius = std::fmax(out.search_radius, 0.0f);

    for (const auto& rule : out.rules) {
        out.priority_by_class[rule.class_id] = rule.priority;
        if (out.head_body_fusion_enabled) {
            out.include_by_class[rule.class_id] = rule.include;
            for (const auto included : rule.include)
                out.classes_by_included_class[included].push_back(rule.class_id);
        }
    }

    // 0x14005DC67..0x14005DD7D finds a single dense class-table extent from
    // rule keys, include keys/values, and the active list.
    std::int32_t maximum_class = -1;
    for (const auto& rule : out.rules) {
        maximum_class = (std::max)(maximum_class, rule.class_id);
        if (out.head_body_fusion_enabled) {
            for (const auto included : rule.include)
                maximum_class = (std::max)(maximum_class, included);
        }
    }
    for (const auto class_id : out.active_classes)
        maximum_class = (std::max)(maximum_class, class_id);
    out.class_count = maximum_class >= 0 ? maximum_class + 1 : 0;
    out.active_class_mask.assign(
        static_cast<std::size_t>(out.class_count), std::uint8_t{0});
    out.effective_priority_by_class.assign(
        static_cast<std::size_t>(out.class_count),
        -std::numeric_limits<float>::infinity());
    for (const auto class_id : out.active_classes) {
        if (class_id >= 0 && class_id < out.class_count)
            out.active_class_mask[static_cast<std::size_t>(class_id)] = 1;
    }

    // All accepted rules are finite.  Native initializes every active class
    // to the minimum configured priority, then overwrites explicit rules.
    float fallback_priority = std::numeric_limits<float>::infinity();
    for (const auto& [class_id, priority] : out.priority_by_class) {
        (void)class_id;
        fallback_priority = std::fmin(fallback_priority, priority);
    }
    if (!std::isfinite(fallback_priority))
        fallback_priority = -std::numeric_limits<float>::infinity();
    for (const auto class_id : out.active_classes) {
        if (class_id >= 0 && class_id < out.class_count)
            out.effective_priority_by_class[static_cast<std::size_t>(class_id)] =
                fallback_priority;
    }
    for (const auto& [class_id, priority] : out.priority_by_class) {
        if (class_id >= 0 && class_id < out.class_count
            && out.active_class_mask[static_cast<std::size_t>(class_id)]) {
            out.effective_priority_by_class[static_cast<std::size_t>(class_id)] =
                priority;
        }
    }

    // An active included class without its own priority rule receives ten
    // percent of the greatest priority of the rules which include it.
    for (const auto& [included, parents] : out.classes_by_included_class) {
        if (included < 0 || included >= out.class_count
            || !out.active_class_mask[static_cast<std::size_t>(included)]
            || out.priority_by_class.contains(included)) {
            continue;
        }
        float parent_max = -std::numeric_limits<float>::infinity();
        for (const auto parent : parents) {
            const auto found = out.priority_by_class.find(parent);
            if (found != out.priority_by_class.end())
                parent_max = std::fmax(parent_max, found->second);
        }
        if (std::isfinite(parent_max)) {
            out.effective_priority_by_class[static_cast<std::size_t>(included)] =
                parent_max * 0.1f;
        }
    }

    for (const auto class_id : out.active_classes) {
        std::vector<std::int32_t> allowed;
        append_unique(allowed, class_id);
        if (const auto direct = out.include_by_class.find(class_id);
            direct != out.include_by_class.end()) {
            for (const auto value : direct->second)
                append_unique(allowed, value);
        }
        if (const auto reverse = out.classes_by_included_class.find(class_id);
            reverse != out.classes_by_included_class.end()) {
            for (const auto value : reverse->second)
                append_unique(allowed, value);
        }
        out.association_classes_by_class.emplace(class_id, std::move(allowed));
    }
    return out;
}

} // namespace cvm::recovered
