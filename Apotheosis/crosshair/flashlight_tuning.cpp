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
    t.det.brightness_threshold   = lerpi(246.0f, 214.0f, s);
    t.det.max_channel_spread     = lerpi(24.0f, 58.0f, s);
    t.det.min_circularity        = lerpf(0.76f, 0.54f, s);
    t.det.min_local_contrast     = lerpi(58.0f, 28.0f, s);
    t.det.min_radial_consistency = lerpf(0.82f, 0.66f, s);
    t.det.open_radius          = 0;
    t.det.max_spots            = 5;   // internal cap; runtime ranks, loop picks one
    t.accept_conf_floor        = lerpf(0.72f, 0.58f, s);

    // ---- 光斑大小 → 半径档 (scaled to detection resolution) ----
    // 旧版固定 15px 下限会在 320 检测分辨率直接吃掉远处手电。现在按分辨率
    // 缩放到 2~4px，并靠白核、径向一致性与三帧确认抑制小反光。
    t.det.min_radius = std::max(2, static_cast<int>(std::lround(
        H * lerpf(0.010f, 0.005f, s))));
    t.det.max_radius = std::max(t.det.min_radius + 1,
                                static_cast<int>(std::lround(H * lerpf(0.08f, 0.34f, z))));

    // ---- 抗误锁 → 深度门 ----
    if (r < 0.20f)      t.depth.mode = 0;
    else if (r < 0.50f) t.depth.mode = 1;
    else                t.depth.mode = 2; // 严格模式：孤立远景光直接拒绝
    t.depth.far_level        = lerpi(20.0f, 52.0f, r);
    t.depth.soft_penalty     = lerpf(0.10f, 0.40f, r);
    t.depth.top_band_penalty = (r >= 0.20f) ? lerpf(0.05f, 0.30f, r) : 0.0f;

    // ---- 抗误锁 → 时间门 ----
    t.temporal.confirm_frames  = 3; // 与用户约定：连续三个 YOLO 推理帧
    t.temporal.max_jump_px     = std::max(16.0f, H * 0.10f);
    t.temporal.drop_after      = 0; // 任一推理帧丢失就立即撤销
    t.temporal.onset_bonus     = 0.0f;
    t.temporal.confirmed_bonus = 0.06f;

    // ---- 抗误锁 → 联动 ----
    t.coloc.max_box_widths      = 0.50f;

    return t;
}

} // namespace crosshair
