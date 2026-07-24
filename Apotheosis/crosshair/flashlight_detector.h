#ifndef FLASHLIGHT_DETECTOR_H
#define FLASHLIGHT_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <optional>
#include <vector>

namespace crosshair
{

// Whole-frame flashlight glare detector. Targets the bright over-exposed halo
// that an enemy's tactical flashlight burns into the player's view ("寻光" /
// 手电筒检测). 与准星/激光找色不同，它要求“高亮且近白”的过曝核心，再验证
// 四周是否存在一致的径向衰减，以减少彩色 UI、灯牌和单侧高光误锁。
//
// DISTANCE INVARIANCE is the hard part. The in-game glare is a RADIAL gradient
// (bright core fading outward), and its on-screen size swings wildly with
// range: a far enemy is a tiny white dot, a point-blank one floods the whole
// view. A single brightness threshold turns that gradient into a blob whose
// SIZE is entangled with distance, so tuning the radius band for "far dot"
// misses "near flood" and vice-versa — and loosening the band to catch the
// flood lets far-away scene clutter (sky, specular glints, UI) flood in.
//
// The fix is to gate on SCALE-INVARIANT structure instead of pixel size:
//   • circularity of the bright contour (round, not an elongated streak), and
//   • a radius-proportional RADIAL FALLOFF — the core must be brighter than a
//     ring sampled at ~1.5× its radius, and brightness must decrease outward.
// A real halo passes at any range; a flat bright patch (sky/UI) fails the
// falloff test regardless of size. Detections are then deduped (one glare can
// fragment into core + halo) and capped to `max_spots` so a noisy frame can
// never spawn a swarm of candidates. 无法定位核心的大面积白屏直接拒绝。
//
// Scanning the whole frame (not a ROI) is intentional — the flashlight halo
// can appear anywhere depending on where the enemy is standing. Cost is
// linear in detection-image area and the detector only runs when the user
// has enabled it.
struct FlashlightDetectorSettings
{
    bool enabled = false;

    // Pixel-brightness gate for the over-exposed core.
    int  brightness_threshold = 220;

    // A flashlight core is close to white even when its outer bloom is tinted.
    // Reject pixels whose max/min channel spread is larger than this. This is
    // the main defence against coloured HUD icons and emissive signs.
    int  max_channel_spread = 48;

    // Acceptable spot radius range in detection-image pixels. Small = near
    // distant lights; large = up-close flashes. A halo outside this band is
    // ignored, so the muzzle flash / specular highlights don't get picked.
    int  min_radius = 5;
    int  max_radius = 100;

    // Minimum 4πA / P² ratio (1.0 = perfect circle, ~0.78 = square,
    // ~0.6 = generous halo). Reject elongated bright streaks (UI bars,
    // sun-on-rail glints) by raising; relax for partly-occluded halos.
    float min_circularity = 0.60f;

    // MORPH_OPEN radius (px) before connected-components, to drop hot pixels /
    // single-row specular streaks. 0 = off.
    int  open_radius = 1;

    // Radial-falloff / local-contrast gate (key knob for sky / cloud rejection
    // AND the thing that makes distance-invariance work). For every candidate
    // we sample mean brightness in a core disk (~0.35 × radius) and an outer
    // ring (~1.25–1.8 × radius), both PROPORTIONAL to the detected radius so
    // the test means the same thing for a 6 px dot and a 120 px flood. A real
    // halo: bright core, darker ring, brightness decreasing outward → passes.
    // A flat bright patch (sky / cloud / UI): core ≈ ring, no falloff → dropped
    // even when it's large. The contour is dropped when (core − ring) is below
    // this value or the profile isn't monotonically decreasing.
    //   0   = disabled (contour shape gates only)
    //   30  = good default, kills most sky / cloud false positives
    //   80+ = strict, only halos against clearly darker backgrounds
    int  min_local_contrast = 30;

    // Fraction of radial spokes that must get darker from core to halo edge.
    // Unlike a single annulus mean, this rejects a bright patch that happens to
    // be dark on only one side. 0 disables the gate.
    float min_radial_consistency = 0.70f;

    // Hard cap on how many halos a single frame may report. After the shape /
    // falloff gates, surviving spots are sorted by confidence, deduped (a glare
    // that fragments into core + halo collapses to one), and truncated to this
    // many. Bounds the "tuned for near flood → found a swarm at distance"
    // failure so the aim pipeline never sees a cloud of phantom targets.
    int  max_spots = 3;
};

// One detected halo. `center` is the centroid (detection-image px); `radius`
// is the equivalent-area radius; `confidence` ∈ [0,1] mixes circularity and
// brightness / contrast / radial consistency — higher = more flashlight-like.
struct FlashlightSpot
{
    cv::Point2f center{};
    float       radius     = 0.0f;
    float       confidence = 0.0f;
};

// Diagnostic record for every contour that survived the brightness mask,
// regardless of whether it ultimately passed the circularity / radius gates.
// `accepted` is true when the spot would have been emitted by detectAll().
// When false, `reject_reason` is one of: "too small", "too large",
// "deformed" (circ < min), "degenerate" (zero perimeter or moments).
struct FlashlightCandidate
{
    cv::Point2f center{};
    float       radius      = 0.0f;
    float       circularity = 0.0f;
    float       contrast     = 0.0f; // core_mean − outer_ring_mean (radial falloff)
    float       radial_consistency = 0.0f;
    bool        accepted    = false;
    const char* reject_reason = "";
};

// Stateless detector.
class FlashlightDetector
{
public:
    // Full scan: every qualifying spot, sorted by descending confidence.
    std::vector<FlashlightSpot> detectAll(
        const cv::Mat& bgrFrame,
        const FlashlightDetectorSettings& settings) const;

    // Convenience: just the best spot's centre, or std::nullopt when no
    // qualifying glare is found this frame. Wraps detectAll().
    std::optional<cv::Point2f> detect(
        const cv::Mat& bgrFrame,
        const FlashlightDetectorSettings& settings) const;

    // Verbose pass: returns one entry per bright contour, accepted or not,
    // with the measured radius / circularity and (when rejected) the reason.
    // Used by the preview overlay so the user can SEE why a halo that's
    // obviously there isn't being picked up. Pure diagnostic — runs the same
    // pipeline as detectAll().
    std::vector<FlashlightCandidate> detectVerbose(
        const cv::Mat& bgrFrame,
        const FlashlightDetectorSettings& settings) const;
};

} // namespace crosshair

#endif // FLASHLIGHT_DETECTOR_H
