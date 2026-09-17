#include "anchor.h"

#include <algorithm>
#include <cmath>

namespace control {

namespace {

// 简单的确定性哈希 —— 不用 <random>，因为：
//   1) 要可复现（单测不能因为随机而闪烁）
//   2) 不用全局状态（多实例互不干扰）
//   3) 不需要统计学质量，只要"看起来散开"就够
inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// 返回 [0,1) 的确定性伪随机数。
inline double unitRandom(uint64_t seed)
{
    return static_cast<double>(mix64(seed) >> 11) * (1.0 / 9007199254740992.0);
}

} // namespace

Vec2 anchorFromOffset(const Vec2& filteredCenter, const Box& box, double yOffset)
{
    // ★ 坐标约定：检测图像素，y 向下增长。
    //   yOffset = 1.0 ⇒ 框顶 ⇒ centerY − h/2
    //   yOffset = 0.5 ⇒ 中心 ⇒ centerY
    //   yOffset = 0.0 ⇒ 框底 ⇒ centerY + h/2
    //   即 anchorY = centerY + (0.5 − yOffset) × h
    return Vec2{
        filteredCenter.x,
        filteredCenter.y + (0.5 - yOffset) * box.h
    };
}

Vec2 computeAnchor(const Vec2& filteredCenter, const Box& box,
                   const AimPointConfig& cfg, uint64_t frameIndex)
{
    double lo = cfg.yOffset;
    double hi = cfg.yOffsetMax;
    if (hi < lo)
        std::swap(lo, hi);

    double offset = lo;
    if (hi > lo)
    {
        // 每帧一个独立的随机值。frameIndex 参与是为了让同一帧可复现
        // （同一帧重算两次结果相同 ⇒ 单测稳定）。
        const uint64_t seed = (cfg.randomSeed != 0 ? cfg.randomSeed : 0x9E3779B97F4A7C15ULL)
                            + frameIndex * 0xBF58476D1CE4E5B9ULL;
        offset = lo + (hi - lo) * unitRandom(seed);
    }

    return anchorFromOffset(filteredCenter, box, offset);
}

} // namespace control
