#ifndef FLASHLIGHT_DETECTOR_H
#define FLASHLIGHT_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <optional>
#include <vector>

namespace crosshair
{

// Whole-frame flashlight glare detector. Targets the bright over-exposed halo
// that an enemy's tactical flashlight burns into the player's view ("寻光" /
// 手电筒检测). Unlike the colour-based crosshair / laser detectors, this one
// is HUE-AGNOSTIC: the glare core is near-pure white in any colour space, so
// we threshold on brightness only and then keep the most CIRCULAR connected
// component within the user-selected radius range.
//
// Scanning the whole frame (not a ROI) is intentional — the flashlight halo
// can appear anywhere depending on where the enemy is standing. Cost is
// linear in detection-image area and the detector only runs when the user
// has enabled it.
struct FlashlightDetectorSettings
{
    bool enabled = false;

    // Pixel-brightness gate. The grey-level used is max(B,G,R) (so coloured
    // flashlight tints still saturate near the upper end). Default 220 keeps
    // only the over-exposed core of the glare.
    int  brightness_threshold = 220;

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

    // Local-contrast gate (key knob for sky / cloud rejection). For every
    // candidate contour we compare its mean inner brightness to the mean
    // brightness of an annular ring just outside it (1.5 × contour bbox).
    // A real flashlight halo sits on a darker scene → high contrast; a
    // bright sky region is uniformly bright → low contrast. The contour is
    // dropped when (inner_mean − outer_mean) is below this value.
    //   0   = disabled (legacy behaviour, contour shape gates only)
    //   30  = good default, kills most sky / cloud false positives
    //   80+ = strict, only halos against clearly darker backgrounds
    int  min_local_contrast = 30;
};

// One detected halo. `center` is the centroid (detection-image px); `radius`
// is the equivalent-area radius; `confidence` ∈ [0,1] mixes circularity and
// brightness — higher = more flashlight-like. Sorted descending by score.
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
