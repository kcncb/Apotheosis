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

    // One morphological open to reject single-pixel noise.
    static const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty())
        return std::nullopt;

    const int minArea = std::max(1, settings.min_area);
    const int maxArea = std::max(minArea, settings.max_area);

    const cv::Point2f roiCenter(roi.width * 0.5f, roi.height * 0.5f);
    std::optional<cv::Point2f> bestLocal;
    float bestDist2 = std::numeric_limits<float>::max();

    for (const auto& c : contours)
    {
        const double area = cv::contourArea(c);
        if (area < minArea || area > maxArea)
            continue;
        cv::Moments mu = cv::moments(c);
        if (mu.m00 <= 0.0)
            continue;
        const cv::Point2f local(static_cast<float>(mu.m10 / mu.m00),
                                static_cast<float>(mu.m01 / mu.m00));
        const float dx = local.x - roiCenter.x;
        const float dy = local.y - roiCenter.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestDist2)
        {
            bestDist2 = d2;
            bestLocal = local;
        }
    }

    if (!bestLocal)
        return std::nullopt;

    return cv::Point2f(bestLocal->x + static_cast<float>(roi.x),
                       bestLocal->y + static_cast<float>(roi.y));
}

} // namespace crosshair
