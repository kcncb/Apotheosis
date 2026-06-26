#ifndef GLASS_TUNING_H
#define GLASS_TUNING_H

#include <opencv2/core.hpp>

namespace crosshair
{

// ===========================================================================
// 玻璃过滤 tuning: collapse the three raw sliders (环厚 / 命中率 / 最小框) behind
// ONE behaviour-level macro knob so the UI only exposes a single slider.
//
//   • 过滤强度 (strength) — 保守(只杀明显玻璃) ↔ 激进(薄膜也杀)
//
// The other two were never independent behaviour axes:
//   • 边缘环厚度  — a sampling internal; fixed at 0.15 (the old default).
//   • 最小框短边  — a resolution-dependent safety floor; auto-scaled to the
//                   detection frame here instead of being a manual px slider.
//
// `glass_derive_settings()` is the SINGLE source of truth mapping knob →
// internals. Mirrors crosshair/flashlight_tuning.h.
// ===========================================================================

// The three concrete params consumed by GlassFilter::check (colours + enabled
// are supplied separately from config/UI).
struct GlassDerived
{
    float edge_ring_frac;
    float coverage_threshold;
    int   min_box_short_side;
};

// Map the single 过滤强度 knob (0..100) + detection frame size to the concrete
// params. Higher strength = lower coverage threshold = rejects more boxes as
// glass. Pure function, no global state.
GlassDerived glass_derive_settings(int strength, cv::Size frame);

} // namespace crosshair

#endif // GLASS_TUNING_H
