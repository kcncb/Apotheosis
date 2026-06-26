#ifndef FLASHLIGHT_TUNING_H
#define FLASHLIGHT_TUNING_H

#include <opencv2/core.hpp>

#include "flashlight_detector.h"

namespace crosshair
{

// ===========================================================================
// 寻光 tuning: collapse every hard-wired flashlight constant behind THREE
// behaviour-level macro knobs so the UI only ever exposes three sliders.
//
//   • 灵敏度     (sensitivity)     — 锁得勤 ↔ 锁得稳   (appearance axis)
//   • 抗误锁     (reject_strength) — 信外观 ↔ 信判别   (depth / time / colocation)
//   • 光斑大小   (spot_size)       — 可接受的光斑半径档(按分辨率自动)
//
// `flashlight_derive_tuning()` is the SINGLE source of truth: both the capture-
// thread runtime and the preview overlay derive their concrete settings from
// here, so there is exactly one place that maps knobs → internals. The old
// seven raw parameters (brightness / radius / circularity / open / contrast /
// max_spots) no longer exist as config; they are interpolated below.
// ===========================================================================

// Depth-gate behaviour (杀太阳/天空/远处泛光). Driven by 抗误锁.
struct FlashlightDepthGate
{
    int   mode             = 0;     // 0=off, 1=soft penalty, 2=hard sky-reject
    int   far_level        = 40;    // normalized-depth (0=farthest,255=nearest)
                                    // at/below which a spot counts as far/sky
    float sky_cluster_frac = 0.12f; // frame fraction at far_level to declare
                                    // "open sky present" (gates the hard reject)
    float soft_penalty     = 0.25f; // confidence subtracted in soft mode
    float top_band_penalty = 0.0f;  // positional fallback when no depth map:
                                    // penalize spots high in the frame (sky band)
};

// Temporal-tracker behaviour (杀水面/玻璃闪烁反光). Driven by 抗误锁.
struct FlashlightTemporal
{
    int   confirm_frames  = 1;      // K: consecutive frames a track must persist
    float max_jump_px     = 64.0f;  // association radius between frames
    int   drop_after      = 6;      // ticks unseen before a track is forgotten
    float onset_bonus     = 0.15f;  // reward a light that just turned on
    float confirmed_bonus = 0.0f;   // reward a stable confirmed track
};

// Colocation with model detections (与模型框联动). Driven by 抗误锁. Applied in
// the mouse loop where the model boxes live; carried here so all constants stay
// in one file.
struct FlashlightColocation
{
    float overlap_frac        = 0.10f; // halo∩box / halo-area to count colocated
    float boost               = 0.25f; // confidence bump when colocated
    bool  required_for_orphan = false; // when true a non-colocated spot must pass
                                       // depth+temporal to be aimable
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
