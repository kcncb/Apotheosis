#ifndef CROSSHAIR_DETECTOR_H
#define CROSSHAIR_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <optional>
#include <string>
#include <vector>

namespace crosshair
{

// One HSV color band. Red wraps at H=0/180, so a red preset is expressed as
// two bands (0..10 and 160..179). Green / purple / cyan are a single band.
// All values in OpenCV HSV space: H in [0,179], S/V in [0,255].
struct CrosshairColorBand
{
    std::string name = "Red-Low";
    bool enabled = true;
    int h_low  = 0;
    int h_high = 10;
    int s_min  = 120;
    int s_max  = 255;
    int v_min  = 120;
    int v_max  = 255;
};

// Per-call detector configuration. `enabled` is toggled by the caller (the
// mouse loop) based on the currently-active hotkey; shape / color list /
// area filters come from Config and are shared across hotkeys.
struct CrosshairDetectorSettings
{
    bool enabled = false;

    // Sampling rectangle in detection-image pixels, centered on the frame.
    int rect_w = 40;
    int rect_h = 40;

    // Active color bands: a hit in ANY enabled band counts as a candidate.
    std::vector<CrosshairColorBand> colors;

    // Minimum count of red-mask pixels in the ROI required to call it a
    // detection. Below this we treat the frame as "no crosshair seen" —
    // prevents random single-pixel noise from defining a pivot.
    int min_pixel_count = 4;

    // Optional MORPH_CLOSE radius (px). 0 disables. Useful for crosshair
    // styles where the white-to-red gradient leaves the red mask with
    // small gaps; closing fills them so the density centroid is stable.
    // Bigger radius bridges further but risks merging nearby red noise
    // (blood, hit markers) into the crosshair blob — keep small.
    int close_radius = 1;
};

// Default color list applied when none is configured: the two bands that
// cover red across the H wraparound.
std::vector<CrosshairColorBand> default_red_bands();

// Stateless detector: given a full BGR frame in detection-image space, returns
// the centroid (x, y) in the same coordinate space of the reticle pixel blob
// that sits closest to the frame center, or std::nullopt if nothing passes
// the filter.
class CrosshairDetector
{
public:
    std::optional<cv::Point2f> detect(const cv::Mat& bgrFrame,
                                      const CrosshairDetectorSettings& settings) const;
};

} // namespace crosshair

#endif // CROSSHAIR_DETECTOR_H
