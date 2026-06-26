#include "glass_tuning.h"

#include <algorithm>
#include <cmath>

namespace crosshair
{

namespace
{
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

GlassDerived glass_derive_settings(int strength, cv::Size frame)
{
    const float s = clamp01(static_cast<float>(strength) / 100.0f); // 过滤强度
    const int   H = std::max(1, frame.height);

    GlassDerived d;

    // Sampling ring fixed at 15% of the box short side — empirically the sweet
    // spot between "too thin = noise-dominated" and "too thick = eats the
    // player and dilutes coverage".
    d.edge_ring_frac = 0.15f;

    // 过滤强度 → 命中率阈值 (inverted): 0 = 保守 (0.70, 只杀环内几乎全是玻璃色的框),
    // 100 = 激进 (0.25, 薄淡玻璃膜也判). 50 ≈ 0.475，约等于旧的 0.45 默认值。
    d.coverage_threshold = lerpf(0.70f, 0.25f, s);

    // Min box short side auto-scaled to detection resolution (~6%, floored) so
    // it stops being a resolution-dependent footgun. 320 → ~19, 640 → ~38.
    d.min_box_short_side = std::max(12, static_cast<int>(std::lround(H * 0.06f)));

    return d;
}

} // namespace crosshair
