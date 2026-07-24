#pragma once

#include <cstdint>

namespace cvm::recovered {

// Globals +96..+116 copied by sub_14004DAD0.  Defaults are assigned at
// 0x14004C96A..0x14004C97F and the exact rectangle test is sub_140049090.
struct TargetRegionConfig {
    bool enabled{};                         // byte_140BD1F70
    float width_scale{1.0f};                // dword_140BD1F74
    float height_scale{0.60000002f};        // qword_140BD1F78 low
    float horizontal_bias{};                // qword_140BD1F78 high
    float vertical_bias{};                  // qword_140BD1F80 low
    std::int32_t region_axis_mode{};         // qword_140BD1F80 high
};

struct TargetBoxF {
    float left{};
    float top{};
    float width{};
    float height{};
};

struct TargetRegionEvaluation {
    bool valid{};
    bool inside{};
    float normalized_width_scale{};
    float normalized_height_scale{};
    float normalized_horizontal_bias{};
    float normalized_vertical_bias{};
    float center_x{};
    float center_y{};
    float half_width{};
    float half_height{};
    float left{};
    float right{};
    float top{};
    float bottom{};
};

// sub_14004B870.
bool valid_target_box(TargetBoxF box) noexcept;

// sub_140049090, with all formerly global inputs made explicit.  Bounds are
// inclusive and float operation ordering follows 0x1400492D8..0x1400493AA.
TargetRegionEvaluation evaluate_target_region(
    const TargetRegionConfig& config,
    bool target_valid,
    TargetBoxF box,
    float current_x,
    float current_y) noexcept;

struct TargetRegionBoxSelection {
    bool preferred_path_enabled{};   // HIBYTE(word_140BD2070)
    bool preferred_target_valid{};   // low byte moved from v183
    TargetBoxF preferred{};          // v180[0..3]
    bool fallback_target_valid{};    // low byte of word_140BD48E0
    TargetBoxF fallback{};           // unk_140BD48CC/qword_140BD48D0/dword_140BD48D8
};

// 0x14005021B..0x1400502FA geometry source selection.
TargetRegionEvaluation evaluate_selected_target_region(
    const TargetRegionConfig& config,
    const TargetRegionBoxSelection& selection,
    float current_x,
    float current_y) noexcept;

} // namespace cvm::recovered
