#include "flashlight_tuning.h"

#include <algorithm>
#include <cmath>

namespace crosshair
{

namespace
{
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int   lerpi(float a, float b, float t) { return static_cast<int>(std::lround(lerpf(a, b, t))); }
} // namespace

FlashlightTuning flashlight_derive_tuning(int sensitivity,
                                          int reject_strength,
                                          int spot_size,
                                          cv::Size frame)
{
    const float s = clamp01(static_cast<float>(sensitivity)     / 100.0f); // 灵敏度
    const float r = clamp01(static_cast<float>(reject_strength) / 100.0f); // 抗误锁
    const float z = clamp01(static_cast<float>(spot_size)       / 100.0f); // 光斑大小
    const int   H = std::max(1, frame.height);

    FlashlightTuning t;

    // ---- 灵敏度 → 外观门 (brightness / circularity / contrast / accept bar) ----
    // Higher sensitivity = looser gates + lower accept bar = locks more eagerly.
    t.det.enabled              = true;
    t.det.brightness_threshold = lerpi(238.0f, 200.0f, s);
    t.det.min_circularity      = lerpf(0.72f, 0.48f, s);
    t.det.min_local_contrast   = lerpi(48.0f, 18.0f, s);
    t.det.open_radius          = 1;
    t.det.max_spots            = 5;   // internal cap; runtime ranks, loop picks one
    t.accept_conf_floor        = lerpf(0.50f, 0.24f, s);

    // ---- 光斑大小 → 半径档 (scaled to detection resolution) ----
    // Small floor always present so far/tiny lights still pass; the knob raises
    // the ceiling so up-close floods count too.
    t.det.min_radius = std::max(3, static_cast<int>(std::lround(H * 0.005f)));
    t.det.max_radius = std::max(t.det.min_radius + 1,
                                static_cast<int>(std::lround(H * lerpf(0.12f, 0.50f, z))));

    // ---- 抗误锁 → 深度门 ----
    if (r < 0.20f)      t.depth.mode = 0; // off (≈ old appearance-only behaviour)
    else if (r < 0.60f) t.depth.mode = 1; // soft penalty
    else                t.depth.mode = 2; // hard sky-reject (open-sky scenes)
    t.depth.far_level        = lerpi(22.0f, 55.0f, r);
    t.depth.sky_cluster_frac = 0.12f;
    t.depth.soft_penalty     = lerpf(0.10f, 0.40f, r);
    t.depth.top_band_penalty = (r >= 0.20f) ? lerpf(0.05f, 0.30f, r) : 0.0f;

    // ---- 抗误锁 → 时间门 ----
    t.temporal.confirm_frames  = lerpi(1.0f, 3.0f, r);
    t.temporal.max_jump_px     = std::max(16.0f, H * 0.06f);
    t.temporal.drop_after      = 6;
    t.temporal.onset_bonus     = 0.15f;
    t.temporal.confirmed_bonus = lerpf(0.0f, 0.12f, r);

    // ---- 抗误锁 → 联动 ----
    t.coloc.overlap_frac        = 0.10f;
    t.coloc.boost               = 0.25f;
    t.coloc.required_for_orphan = (r >= 0.30f);

    return t;
}

} // namespace crosshair
