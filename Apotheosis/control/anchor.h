#pragma once

// ④ 瞄点计算 + xy 中轴
//
//   anchor = 滤波后的中心点 + y_offset × 框高      (px, 检测像素)
//   y_offset ∈ [y_offset, y_offset_max]            框内相对位置，L0 逐游戏配置
//
// ★ y_offset 的语义是【框内相对位置】（1=框顶 / 0.5=中心 / 0=框底），
//   不是像素 —— 所以它跨游戏时比像素偏移稳。
// ⚠️ 但方案此前"人形目标胸口在任何游戏里都是 0.55"那句过于乐观：
//   它依赖"框把什么框住了"（全身 / 上半身 / 头），同一个 0.55 指向的位置完全不同。
//   ⇒ y_offset 的通用性依赖"框的语义一致"，这是 L0 的责任，不是白捡的。

#include "types.h"

namespace control {

struct AimPointConfig
{
    // 框内相对位置。1.0 = 框顶，0.5 = 中心，0.0 = 框底。
    double yOffset = 0.5;
    // 随机区间上限。> yOffset 时每帧在 [yOffset, yOffsetMax] 之间随机取值。
    // ★ 目的：避免总是打同一个点（某些游戏对固定点位的判定不利）。
    //   等于 yOffset 时表示不随机。
    double yOffsetMax = 0.5;
    // 随机种子注入点：为 0 表示用内部计数器。
    // ★ 抽出来是为了让单测可复现（否则测试会因为随机而闪烁）。
    uint64_t randomSeed = 0;
};

// 由（滤波后的）中心点与框尺寸算出瞄点。
// ★ 注意传入的应该是【滤波后的中心点】，但框尺寸用稳定器的（未滤波）——
//   滤波对象只有中心点（D7 / §3.2.2）。
Vec2 computeAnchor(const Vec2& filteredCenter, const Box& box,
                   const AimPointConfig& cfg, uint64_t frameIndex);

// 由 yOffset 与框算瞄点（不涉及随机，给单测用）。
Vec2 anchorFromOffset(const Vec2& filteredCenter, const Box& box, double yOffset);

} // namespace control
