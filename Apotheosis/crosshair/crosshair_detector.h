#ifndef CROSSHAIR_DETECTOR_H
#define CROSSHAIR_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <optional>
#include <string>
#include <vector>

namespace crosshair
{

// Color band definition with support for both classical HSV and adaptive color saliency
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

// Per-call detector configuration.
struct CrosshairDetectorSettings
{
    bool enabled = false;

    // Sampling rectangle in detection-image pixels, centered on the frame.
    int rect_w = 64;
    int rect_h = 64;

    // Active color bands
    std::vector<CrosshairColorBand> colors;

    // Minimum count of reticle energy/pixels in the ROI required for detection
    int min_pixel_count = 4;

    // Morphological closing radius (0 to 7)
    int close_radius = 1;
};

std::vector<CrosshairColorBand> default_red_bands();

// Universal Robust Crosshair Detector
// Handles arbitrary capture card compression artifacts (MJPEG, NV12, YUY2, RGB24),
// lighting shifts, muzzle flash noise, and low quality streaming feeds.
class CrosshairDetector
{
public:
    std::optional<cv::Point2f> detect(const cv::Mat& bgrFrame,
                                      const CrosshairDetectorSettings& settings) const;
};

} // namespace crosshair

#endif // CROSSHAIR_DETECTOR_H
