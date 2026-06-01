#ifndef LASER_DETECTOR_H
#define LASER_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <optional>
#include <vector>

#include "crosshair_detector.h" // CrosshairColorBand

namespace crosshair
{

// Independent laser-sight detector. Unlike CrosshairDetector (which reports the
// colour-mass centroid, correct for symmetric ring/dot/cross reticles), the
// laser is a long thin BEAM whose centroid sits uselessly mid-line. This
// detector instead isolates the beam as a line and reports its AIM END (the
// tip resting on the target near the frame centre), optionally extrapolated a
// few pixels further along the beam to recover the faint gradient end that the
// colour threshold can't see.
//
// It is deliberately background-robust: rather than trusting the colour mask
// alone (a bright sky / red wall / sign would all leak through), it scores the
// matched connected components by geometry — only a sufficiently ELONGATED
// component whose muzzle end sits BELOW its tip end (the beam always runs
// up-from-the-gun toward the centre) and whose tip is the end nearer the frame
// centre is accepted. Compact blobs and lines pointing the wrong way are
// rejected regardless of colour.
struct LaserDetectorSettings
{
    bool enabled = false;

    // Sampling rectangle (detection-image px): size + explicit centre point,
    // so the caller can freely position it over the beam (clamped to frame).
    int rect_w = 160;
    int rect_h = 240;
    int center_x = 160;
    int center_y = 200;

    // Colour bands (a hit in ANY enabled band counts). Shares the same palette
    // type as CrosshairDetector; the user typically configures the laser's red.
    std::vector<CrosshairColorBand> colors;

    // Minimum matched-pixel count for a component to be considered a beam.
    int min_pixel_count = 10;

    // MORPH_CLOSE radius (px) to bridge the dashed/gradient beam into one
    // component. 0 = off.
    int close_radius = 1;

    // Minimum elongation (principal/secondary axis std-dev ratio) for a
    // component to qualify as a line. Higher = stricter (rejects blobs).
    float min_elongation = 3.0f;

    // Target region (2nd box) near the static screen centre where the true
    // endpoint lies. The fitted line is projected into this box to estimate
    // the tip, instead of a fixed pixel extension — robust to the fuzzy /
    // flickering visible end. The reported tip is the point on the fitted line
    // nearest this box's centre, clamped inside the box (bounds over/under
    // extension). If the line misses the box entirely, the visible end is used.
    int target_center_x = 160;
    int target_rect_w = 60;
    int target_center_y = 160;
    int target_rect_h = 60;
};

// Full result of a laser fit, in detection-image coordinates. When `found`
// is false the points are unset. `tip` is the reported aim point (after the
// virtual extension); `visible_tip` is the beam's visible aim end before
// extension; `muzzle` is the gun end. The segment muzzle→tip lies along the
// detected beam and can be drawn as an overlay.
struct LaserResult
{
    bool found = false;
    cv::Point2f tip{};
    cv::Point2f visible_tip{};
    cv::Point2f muzzle{};
};

// Stateless detector.
class LaserDetector
{
public:
    // Full fit (line endpoints) for overlays / tuning.
    LaserResult detectLine(const cv::Mat& bgrFrame,
                           const LaserDetectorSettings& settings) const;

    // Convenience: just the reported aim tip, or std::nullopt when no
    // qualifying beam is found. Wraps detectLine().
    std::optional<cv::Point2f> detect(const cv::Mat& bgrFrame,
                                      const LaserDetectorSettings& settings) const;
};

} // namespace crosshair

#endif // LASER_DETECTOR_H
