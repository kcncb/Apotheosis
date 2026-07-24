#ifndef FLASHLIGHT_TUNING_H
#define FLASHLIGHT_TUNING_H

#include <opencv2/core.hpp>

#include "flashlight_detector.h"

namespace crosshair
{

// ===========================================================================
// 寻光 tuning。UI 只突出一个“检测倾向”主旋钮；抗误锁与核心大小保留在
// 折叠的高级设置中。所有具体阈值仍只在这里派生，运行时与预览不会各用
// 一套常量。
//
//   • 灵敏度     (sensitivity)     — 锁得勤 ↔ 锁得稳   (appearance axis)
//   • 抗误锁     (reject_strength) — 信外观 ↔ 信判别   (depth / time / colocation)
//   • 光斑大小   (spot_size)       — 可接受的光斑半径档(按分辨率自动)
//
// `flashlight_derive_tuning()` is the SINGLE source of truth for the inference-
// synchronized runtime, so there is exactly one place that maps knobs → internals. The old
// seven raw parameters (brightness / radius / circularity / open / contrast /
// max_spots) no longer exist as config; they are interpolated below.
// ===========================================================================

// Depth-gate behaviour (杀太阳/天空/远处泛光). Driven by 抗误锁.
struct FlashlightDepthGate
{
    int   mode             = 0;     // 0=off, 1=soft penalty, 2=hard far-reject
    int   far_level        = 40;    // normalized-depth (0=farthest,255=nearest)
                                    // at/below which a spot counts as far/sky
    float soft_penalty     = 0.25f; // confidence subtracted in soft mode
    float top_band_penalty = 0.0f;  // positional fallback when no depth map:
                                    // penalize spots high in the frame (sky band)
};

// Temporal-tracker behaviour (杀水面/玻璃闪烁反光). Driven by 抗误锁.
struct FlashlightTemporal
{
    int   confirm_frames  = 3;      // consecutive YOLO inference frames
    float max_jump_px     = 64.0f;  // association radius between frames
    int   drop_after      = 0;      // one miss drops the track immediately
    float onset_bonus     = 0.0f;
    float confirmed_bonus = 0.06f;
};

// Colocation with model detections (与模型框联动). Driven by 抗误锁. Applied in
// the mouse loop where the model boxes live; carried here so all constants stay
// in one file.
struct FlashlightColocation
{
    float max_box_widths = 0.50f; // 核心到人物框边缘最多半个框宽
};

// The full derived parameter set.
struct FlashlightTuning
{
    FlashlightDetectorSettings det;                 // appearance detector
    FlashlightDepthGate        depth;
    FlashlightTemporal         temporal;
    FlashlightColocation       coloc;
    float                      accept_conf_floor = 0.30f; // min adjusted conf to publish
};

// Map the three macro knobs (each 0..100) + the detection frame size to the
// full internal parameter set. Pure function, no global state.
FlashlightTuning flashlight_derive_tuning(int sensitivity,
                                          int reject_strength,
                                          int spot_size,
                                          cv::Size frame);

} // namespace crosshair

#endif // FLASHLIGHT_TUNING_H
