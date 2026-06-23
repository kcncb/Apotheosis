#include "flashlight_detector.h"

#include <algorithm>
#include <cmath>

namespace crosshair
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;

// max(B,G,R) per pixel → one-channel "any-colour-tint" brightness map. A
// pure white halo and a colour-tinted halo (blue / yellow flashlight) both
// reach near 255 in at least one channel, so this is more permissive than
// luminance while still rejecting dim coloured surfaces.
cv::Mat to_max_channel(const cv::Mat& bgr)
{
    cv::Mat ch[3];
    cv::split(bgr, ch);
    cv::Mat out;
    cv::max(ch[0], ch[1], out);
    cv::max(out,    ch[2], out);
    return out;
}

// Shared core: walk every external contour in the brightness mask and emit
// a FlashlightCandidate carrying the measured geometry + an accept/reject
// verdict. The two public entry points (detectAll, detectVerbose) reuse
// this; detectAll throws away the rejects, detectVerbose keeps them so the
// preview can show the user why a halo didn't pass.
void walk_contours(const cv::Mat& bgrFrame,
                   const FlashlightDetectorSettings& settings,
                   std::vector<FlashlightCandidate>& out,
                   bool keep_rejects)
{
    out.clear();
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
        return;

    const int min_r = std::max(1, settings.min_radius);
    const int max_r = std::max(min_r, settings.max_radius);
    const float min_area = kPi * static_cast<float>(min_r) * static_cast<float>(min_r);
    const float max_area = kPi * static_cast<float>(max_r) * static_cast<float>(max_r);
    const float min_circ = std::clamp(settings.min_circularity, 0.0f, 1.0f);

    const cv::Mat bright = to_max_channel(bgrFrame);
    const int thr = std::clamp(settings.brightness_threshold, 0, 255);
    const int min_contrast = std::max(0, settings.min_local_contrast);
    cv::Mat mask;
    cv::threshold(bright, mask, static_cast<double>(thr), 255.0, cv::THRESH_BINARY);

    if (settings.open_radius > 0)
    {
        const int r = std::min(settings.open_radius, 9);
        const int k = 2 * r + 1;
        const cv::Mat ker = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::morphologyEx(mask, mask, cv::MORPH_OPEN, ker);
    }

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    if (contours.empty())
        return;

    out.reserve(contours.size());
    for (const auto& c : contours)
    {
        FlashlightCandidate cd;

        if (c.size() < 5)
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "tiny";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }

        const double area = cv::contourArea(c);
        const double perim = cv::arcLength(c, /*closed=*/true);
        if (!(perim > 0.0) || !(area > 0.0))
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "degenerate";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }

        // Isoperimetric ratio (Polsby–Popper): 1.0 = perfect circle.
        const float circ = static_cast<float>(4.0 * kPi * area / (perim * perim));

        // Centroid via image moments.
        const cv::Moments m = cv::moments(c);
        if (!(m.m00 > 0.0))
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "degenerate";
            cd.circularity = circ;
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }
        const cv::Point2f center(
            static_cast<float>(m.m10 / m.m00),
            static_cast<float>(m.m01 / m.m00));
        const float radius = std::sqrt(static_cast<float>(area) / kPi);

        cd.center     = center;
        cd.radius     = radius;
        cd.circularity = circ;

        if (area < static_cast<double>(min_area))
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "too small";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }
        if (area > static_cast<double>(max_area))
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "too large";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }
        if (circ < min_circ)
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "deformed";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }

        // Local-contrast gate: inner-mean vs ring-mean. A bright sky / cloud
        // patch has nearly identical brightness on both sides → small diff,
        // gets dropped. A flashlight halo sits on darker scene → big diff,
        // passes. Skipped when min_contrast=0 to preserve legacy behaviour.
        if (min_contrast > 0)
        {
            const cv::Rect inner_bbox = cv::boundingRect(c);
            const int margin = std::max(6, static_cast<int>(std::lround(radius * 0.6f)));
            cv::Rect outer_bbox(
                inner_bbox.x - margin,
                inner_bbox.y - margin,
                inner_bbox.width  + 2 * margin,
                inner_bbox.height + 2 * margin);
            outer_bbox &= cv::Rect(0, 0, bright.cols, bright.rows);
            if (outer_bbox.area() <= 0)
            {
                if (!keep_rejects) continue;
                cd.reject_reason = "low contrast";
                cd.accepted = false;
                out.push_back(cd);
                continue;
            }

            // Build a mask of the contour shifted into outer_bbox-local coords.
            cv::Mat inner_mask = cv::Mat::zeros(outer_bbox.size(), CV_8UC1);
            std::vector<std::vector<cv::Point>> shifted(1);
            shifted[0].reserve(c.size());
            for (const auto& p : c)
                shifted[0].emplace_back(p.x - outer_bbox.x, p.y - outer_bbox.y);
            cv::drawContours(inner_mask, shifted, 0, cv::Scalar(255), cv::FILLED);

            // Outer ring = outer_bbox minus the inner mask (one dilation worth
            // of margin around the contour, capturing the immediate scene).
            cv::Mat outer_mask;
            cv::bitwise_not(inner_mask, outer_mask);

            const cv::Mat bright_roi = bright(outer_bbox);
            const cv::Scalar mi = cv::mean(bright_roi, inner_mask);
            const cv::Scalar mo = cv::mean(bright_roi, outer_mask);
            const double contrast = mi[0] - mo[0];

            if (contrast < static_cast<double>(min_contrast))
            {
                if (!keep_rejects) continue;
                cd.reject_reason = "low contrast";
                cd.accepted = false;
                out.push_back(cd);
                continue;
            }
        }

        cd.accepted = true;
        cd.reject_reason = "";
        out.push_back(cd);
    }
}

} // namespace

std::vector<FlashlightSpot> FlashlightDetector::detectAll(
    const cv::Mat& bgrFrame,
    const FlashlightDetectorSettings& settings) const
{
    std::vector<FlashlightSpot> spots;
    if (!settings.enabled)
        return spots;

    std::vector<FlashlightCandidate> cands;
    walk_contours(bgrFrame, settings, cands, /*keep_rejects=*/false);

    const int thr = std::clamp(settings.brightness_threshold, 0, 255);
    const float bright_norm = std::clamp(static_cast<float>(thr) / 255.0f, 0.0f, 1.0f);
    spots.reserve(cands.size());
    for (const auto& cd : cands)
    {
        // Confidence: shape dominates (we trust circularity more than peak
        // brightness once we're past the brightness gate).
        const float conf = std::clamp(0.7f * cd.circularity + 0.3f * bright_norm, 0.0f, 1.0f);
        FlashlightSpot s;
        s.center     = cd.center;
        s.radius     = cd.radius;
        s.confidence = conf;
        spots.push_back(s);
    }

    std::sort(spots.begin(), spots.end(),
              [](const FlashlightSpot& a, const FlashlightSpot& b) {
                  if (a.confidence != b.confidence) return a.confidence > b.confidence;
                  return a.radius > b.radius;
              });
    return spots;
}

std::optional<cv::Point2f> FlashlightDetector::detect(
    const cv::Mat& bgrFrame,
    const FlashlightDetectorSettings& settings) const
{
    const auto all = detectAll(bgrFrame, settings);
    if (all.empty())
        return std::nullopt;
    return all.front().center;
}

std::vector<FlashlightCandidate> FlashlightDetector::detectVerbose(
    const cv::Mat& bgrFrame,
    const FlashlightDetectorSettings& settings) const
{
    std::vector<FlashlightCandidate> cands;
    // Diagnostic path bypasses `enabled` — when the user toggles "显示预览"
    // we want to see candidates even if they haven't wired the runtime
    // enable on a hotkey yet.
    FlashlightDetectorSettings active = settings;
    active.enabled = true;
    walk_contours(bgrFrame, active, cands, /*keep_rejects=*/true);
    return cands;
}

} // namespace crosshair
