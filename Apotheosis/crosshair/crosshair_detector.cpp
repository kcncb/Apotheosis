#include "crosshair_detector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace crosshair
{

namespace
{

int clamp_byte(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

cv::Rect clipped_center_roi(const cv::Size& frame, int rect_w, int rect_h)
{
    const int w = std::max(4, rect_w);
    const int h = std::max(4, rect_h);
    int x = frame.width / 2 - w / 2;
    int y = frame.height / 2 - h / 2;
    cv::Rect roi(x, y, w, h);
    return roi & cv::Rect(0, 0, frame.width, frame.height);
}

// OR a single band into `out`. `out` must be allocated and CV_8UC1 the same
// size as `hsv`. `scratch` is reused across calls.
void accumulate_band(const cv::Mat& hsv,
                     const CrosshairColorBand& b,
                     cv::Mat& out,
                     cv::Mat& scratch)
{
    if (!b.enabled) return;

    const int h_lo = clamp_byte(b.h_low,  0, 179);
    const int h_hi = clamp_byte(b.h_high, 0, 179);
    const int s_lo = clamp_byte(b.s_min,  0, 255);
    const int s_hi = clamp_byte(b.s_max,  0, 255);
    const int v_lo = clamp_byte(b.v_min,  0, 255);
    const int v_hi = clamp_byte(b.v_max,  0, 255);

    cv::inRange(hsv,
                cv::Scalar(std::min(h_lo, h_hi), std::min(s_lo, s_hi), std::min(v_lo, v_hi)),
                cv::Scalar(std::max(h_lo, h_hi), std::max(s_lo, s_hi), std::max(v_lo, v_hi)),
                scratch);
    cv::bitwise_or(out, scratch, out);
}

} // namespace

std::vector<CrosshairColorBand> default_red_bands()
{
    CrosshairColorBand low;
    low.name = "Red-Low";
    low.h_low = 0;
    low.h_high = 10;

    CrosshairColorBand high;
    high.name = "Red-High";
    high.h_low = 160;
    high.h_high = 179;

    return { low, high };
}

std::optional<cv::Point2f> CrosshairDetector::detect(
    const cv::Mat& bgrFrame,
    const CrosshairDetectorSettings& settings) const
{
    if (!settings.enabled || bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
        return std::nullopt;

    // Empty / all-disabled color lists mean "nothing to match".
    bool any_enabled = false;
    for (const auto& c : settings.colors)
    {
        if (c.enabled) { any_enabled = true; break; }
    }
    if (!any_enabled)
        return std::nullopt;

    const cv::Rect roi = clipped_center_roi(bgrFrame.size(),
                                            settings.rect_w, settings.rect_h);
    if (roi.width < 4 || roi.height < 4)
        return std::nullopt;

    cv::Mat region = bgrFrame(roi);
    cv::Mat hsv;
    cv::cvtColor(region, hsv, cv::COLOR_BGR2HSV);

    cv::Mat mask = cv::Mat::zeros(hsv.size(), CV_8UC1);
    cv::Mat scratch;
    for (const auto& band : settings.colors)
        accumulate_band(hsv, band, mask, scratch);

    // Single-pixel noise removal — cheap and shape-agnostic.
    static const cv::Mat openKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, openKernel);

    // Optional CLOSE to bridge gradient-induced gaps (e.g. dot whose centre
    // fades to white doesn't pass HSV → ring is incomplete). Keep the
    // kernel small so we don't merge separate red noise into the crosshair.
    if (settings.close_radius > 0)
    {
        const int r = std::min(settings.close_radius, 7);
        const int k = 2 * r + 1;
        cv::Mat closeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, closeKernel);
    }

    // Density centroid of ALL red pixels in the ROI. Shape-agnostic: a
    // ring + centre dot, a single fading dot, and a thin cross all have
    // their pixel mass distributed symmetrically around the aim point, so
    // the moments centroid lands on it without any contour heuristics.
    const cv::Moments m = cv::moments(mask, true);
    if (m.m00 < static_cast<double>(std::max(1, settings.min_pixel_count)))
        return std::nullopt;

    const float local_x = static_cast<float>(m.m10 / m.m00);
    const float local_y = static_cast<float>(m.m01 / m.m00);
    return cv::Point2f(local_x + static_cast<float>(roi.x),
                       local_y + static_cast<float>(roi.y));
}

} // namespace crosshair
