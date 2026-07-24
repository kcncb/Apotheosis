#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace cvm::recovered {

// arm_config.json mbzz.class_priority element. The native transient record is
// 32 bytes: class_id, priority, and an MSVC vector<int32_t> include.
struct TargetClassPriorityInput {
    std::int32_t class_id{-1};
    float priority{1.0f};
    std::vector<std::int32_t> include;
};

struct TargetSelectorConfigInput {
    std::vector<std::int32_t> target_labels;
    std::vector<TargetClassPriorityInput> class_priority;
    bool class_priority_enabled{true};
    bool head_body_fusion_enabled{true};
    float search_radius{0.5f};
    float acquire_center_weight{0.69999999f};
    std::int32_t max_lost_frames{3};
    std::int32_t min_hold_frames{};
    std::int32_t normalizer_x{};
    std::int32_t normalizer_y{};
};

struct TargetClassPriorityRuntime {
    std::int32_t class_id{};
    float priority{};
    std::vector<std::int32_t> include;
};

struct TargetSelectorRuntimeConfig {
    std::vector<TargetClassPriorityRuntime> rules;
    std::vector<std::int32_t> active_classes;

    bool class_priority_enabled{};       // selector +48
    bool head_body_fusion_enabled{};     // selector +49
    float search_radius{};               // selector +52
    float acquire_center_weight{};       // selector +56
    std::int32_t max_lost_frames{};      // selector +60
    std::int32_t min_hold_frames{};      // selector +64
    std::int32_t normalizer_x{};         // selector +68
    std::int32_t normalizer_y{};         // selector +72

    // Derived at 0x14005D440.
    float search_radius_weight{};        // selector +80
    float inverse_search_radius_weight{};// selector +84
    std::int32_t tracker_loss_limit{};   // selector +684
    float tracker_search_radius{};       // selector +688

    // Rebuilt at 0x14005D5F0. Duplicate class rules overwrite forward values
    // in input order, while reverse include lists append in input order.
    std::map<std::int32_t, float> priority_by_class;
    std::map<std::int32_t, std::vector<std::int32_t>> include_by_class;
    std::map<std::int32_t, std::vector<std::int32_t>> classes_by_included_class;

    // Dense tables rebuilt by 0x14005D930.  These are deliberately kept in
    // addition to the three source maps above: classes which are active but
    // have no explicit rule receive a finite fallback priority, whereas every
    // inactive slot remains -infinity.  association_classes_by_class is the
    // native undirected one-hop mask: self + direct include + reverse include.
    std::int32_t class_count{};          // selector +560
    std::vector<std::uint8_t> active_class_mask; // selector +512
    std::vector<float> effective_priority_by_class; // selector +536
    std::map<std::int32_t, std::vector<std::int32_t>>
        association_classes_by_class;   // selector +424 cache
};

// 0x14004A9D0, 0x14004AB90, 0x14004B300, 0x14005D440 and
// 0x14005D5F0 expressed without MSVC container internals.
TargetSelectorRuntimeConfig normalize_target_selector_config_exact(
    const TargetSelectorConfigInput& input);

} // namespace cvm::recovered
