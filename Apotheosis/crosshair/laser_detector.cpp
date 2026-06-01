#include "laser_detector.h"

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

// Laser ROI: freely positioned by an explicit centre point so the user can
// drag it over the beam (whose body usually sits below / off the crosshair).
// Clamped to the frame.
cv::Rect laser_roi(const cv::Size& frame, int rect_w, int rect_h,
                   int center_x, int center_y)
{
    const int w = std::max(4, rect_w);
    const int h = std::max(4, rect_h);
    const int x = center_x - w / 2;
    const int y = center_y - h / 2;
    return cv::Rect(x, y, w, h) & cv::Rect(0, 0, frame.width, frame.height);
}

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

// One beam candidate fitted from a connected component.
struct BeamFit
{
    bool   valid = false;
    cv::Point2f tip;     // aim end (nearer centre, upper)
    cv::Point2f muzzle;  // gun end  (lower)
    cv::Point2f axis;    // unit principal direction, oriented muzzle→tip
    float  elongation = 0.0f;
};

// Fit a line to the component's pixels (PCA), return its two end clusters with
// tip/muzzle assigned by the confirmed down→up orientation: the lower (larger
// y) end is the muzzle, the upper (smaller y) end is the tip. Rejects masses
// that aren't line-like enough.
BeamFit fit_beam(const std::vector<cv::Point>& pts, float min_elongation)
{
    BeamFit out;
    if (pts.size() < 4)
        return out;

    cv::Mat data(static_cast<int>(pts.size()), 2, CV_32F);
    for (size_t i = 0; i < pts.size(); ++i)
    {
        data.at<float>(static_cast<int>(i), 0) = static_cast<float>(pts[i].x);
        data.at<float>(static_cast<int>(i), 1) = static_cast<float>(pts[i].y);
    }

    cv::PCA pca(data, cv::Mat(), cv::PCA::DATA_AS_ROW);
    const cv::Point2f mean(pca.mean.at<float>(0, 0), pca.mean.at<float>(0, 1));
    cv::Point2f axis(pca.eigenvectors.at<float>(0, 0), pca.eigenvectors.at<float>(0, 1));
    const float axis_len = std::hypot(axis.x, axis.y);
    if (!(axis_len > 1e-3f))
        return out;
    axis.x /= axis_len;
    axis.y /= axis_len;

    // Elongation = ratio of principal to secondary std-dev. A line has a large
    // ratio; a blob approaches 1.
    const float ev0 = pca.eigenvalues.at<float>(0, 0);
    const float ev1 = pca.eigenvalues.at<float>(1, 0);
    const float elong = std::sqrt(std::max(ev0, 0.0f) / std::max(ev1, 1e-3f));
    if (elong < min_elongation)
        return out;

    // Project all pixels onto the axis; cluster-average each extreme so a stray
    // pixel can't define an end.
    float tmin = std::numeric_limits<float>::max();
    float tmax = -std::numeric_limits<float>::max();
    for (const auto& p : pts)
    {
        const float t = (p.x - mean.x) * axis.x + (p.y - mean.y) * axis.y;
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    const float span = std::max(1.0f, tmax - tmin);
    const float band = std::max(2.0f, 0.15f * span);

    cv::Point2f sumMin(0.f, 0.f), sumMax(0.f, 0.f);
    int nMin = 0, nMax = 0;
    for (const auto& p : pts)
    {
        const float t = (p.x - mean.x) * axis.x + (p.y - mean.y) * axis.y;
        if (t <= tmin + band) { sumMin += cv::Point2f((float)p.x, (float)p.y); ++nMin; }
        if (t >= tmax - band) { sumMax += cv::Point2f((float)p.x, (float)p.y); ++nMax; }
    }
    if (nMin == 0 || nMax == 0)
        return out;

    const cv::Point2f endA(sumMin.x / nMin, sumMin.y / nMin);
    const cv::Point2f endB(sumMax.x / nMax, sumMax.y / nMax);

    // Orientation: the beam runs up-from-the-gun, so the lower (larger y) end
    // is the muzzle and the upper end is the tip.
    if (endA.y >= endB.y) { out.muzzle = endA; out.tip = endB; }
    else                  { out.muzzle = endB; out.tip = endA; }
    // Direction = the (least-squares) PCA principal axis, oriented muzzle→tip.
    // Using the axis rather than (tip−muzzle) keeps direction stable when the
    // faint tip-cluster flickers, since the dense front segment dominates PCA.
    cv::Point2f ax = axis;
    if ((out.tip.x - out.muzzle.x) * ax.x + (out.tip.y - out.muzzle.y) * ax.y < 0.0f)
    {
        ax.x = -ax.x;
        ax.y = -ax.y;
    }
    out.axis = ax;
    out.elongation = elong;
    out.valid = true;
    return out;
}

} // namespace

LaserResult LaserDetector::detectLine(
    const cv::Mat& bgrFrame,
    const LaserDetectorSettings& settings) const
{
    if (!settings.enabled || bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
        return LaserResult{};

    bool any_enabled = false;
    for (const auto& c : settings.colors)
        if (c.enabled) { any_enabled = true; break; }
    if (!any_enabled)
        return LaserResult{};

    const cv::Rect roi = laser_roi(bgrFrame.size(), settings.rect_w, settings.rect_h,
                                   settings.center_x, settings.center_y);
    if (roi.width < 4 || roi.height < 4)
        return LaserResult{};

    cv::Mat hsv;
    cv::cvtColor(bgrFrame(roi), hsv, cv::COLOR_BGR2HSV);

    cv::Mat mask = cv::Mat::zeros(hsv.size(), CV_8UC1);
    cv::Mat scratch;
    for (const auto& band : settings.colors)
        accumulate_band(hsv, band, mask, scratch);

    static const cv::Mat openKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, openKernel);
    if (settings.close_radius > 0)
    {
        const int r = std::min(settings.close_radius, 9);
        const int k = 2 * r + 1;
        cv::Mat closeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, closeKernel);
    }

    // Per-component analysis: a red sky / wall / sign leaks into the mask too,
    // so we never trust the colour blob as a whole. Score each component by
    // line geometry and keep the best qualifying beam.
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
    if (n <= 1)
        return LaserResult{};

    // Frame centre expressed in ROI-local coords (the ROI is biased downward,
    // so this is NOT the ROI centre). The tip should rest near here.
    const cv::Point2f center_local(bgrFrame.cols * 0.5f - static_cast<float>(roi.x),
                                   bgrFrame.rows * 0.5f - static_cast<float>(roi.y));
    const int min_pix = std::max(1, settings.min_pixel_count);

    BeamFit best;
    float bestScore = -std::numeric_limits<float>::max();

    std::vector<cv::Point> pts;
    for (int label = 1; label < n; ++label)
    {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area < min_pix)
            continue;

        // Collect this component's pixels.
        const int bx = stats.at<int>(label, cv::CC_STAT_LEFT);
        const int by = stats.at<int>(label, cv::CC_STAT_TOP);
        const int bw = stats.at<int>(label, cv::CC_STAT_WIDTH);
        const int bh = stats.at<int>(label, cv::CC_STAT_HEIGHT);
        pts.clear();
        pts.reserve(static_cast<size_t>(area));
        for (int yy = by; yy < by + bh; ++yy)
        {
            const int* lr = labels.ptr<int>(yy);
            for (int xx = bx; xx < bx + bw; ++xx)
                if (lr[xx] == label)
                    pts.emplace_back(xx, yy);
        }

        const BeamFit fit = fit_beam(pts, settings.min_elongation);
        if (!fit.valid)
            continue;

        // The tip must be the end nearer the frame centre; if the muzzle is
        // actually closer, this isn't a beam aimed at the centre — reject.
        const float dTip = std::hypot(fit.tip.x - center_local.x, fit.tip.y - center_local.y);
        const float dMuz = std::hypot(fit.muzzle.x - center_local.x, fit.muzzle.y - center_local.y);
        if (dTip > dMuz)
            continue;

        // Prefer the most line-like beam whose tip sits closest to the centre.
        const float score = fit.elongation - 0.05f * dTip;
        if (score > bestScore)
        {
            bestScore = score;
            best = fit;
        }
    }

    if (!best.valid)
        return LaserResult{};

    // Endpoints in global (detection-image) coords.
    const cv::Point2f off(static_cast<float>(roi.x), static_cast<float>(roi.y));
    const cv::Point2f muzzle_g = best.muzzle + off;
    const cv::Point2f vtip_g   = best.tip + off;

    // Estimate the true endpoint by projecting the fitted line into the target
    // box (the brown region near screen centre). The line direction comes from
    // the reliable, dense front segment (PCA over the whole component is
    // dominated by it), so a flickering fuzzy visible end no longer moves the
    // reported tip. Default to the visible end if anything degenerates.
    cv::Point2f tip_g = vtip_g;

    // Direction from the stable PCA axis (muzzle→tip), not the flickering tip.
    cv::Point2f d = best.axis;
    const float dlen = std::hypot(d.x, d.y);
    if (dlen > 1e-3f)
    {
        d.x /= dlen;
        d.y /= dlen;

        const float bw = std::max(2, settings.target_rect_w) * 0.5f;
        const float bh = std::max(2, settings.target_rect_h) * 0.5f;
        const cv::Point2f bc(static_cast<float>(settings.target_center_x),
                             static_cast<float>(settings.target_center_y));
        const float bx0 = bc.x - bw, bx1 = bc.x + bw;
        const float by0 = bc.y - bh, by1 = bc.y + bh;

        // Clip the infinite line muzzle + t*d to the box (slab method).
        float t0 = -1e9f, t1 = 1e9f;
        bool ok = true;
        // X slabs.
        if (std::abs(d.x) > 1e-6f)
        {
            float ta = (bx0 - muzzle_g.x) / d.x, tb = (bx1 - muzzle_g.x) / d.x;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        }
        else if (muzzle_g.x < bx0 || muzzle_g.x > bx1) ok = false; // parallel & outside
        // Y slabs.
        if (ok && std::abs(d.y) > 1e-6f)
        {
            float ta = (by0 - muzzle_g.y) / d.y, tb = (by1 - muzzle_g.y) / d.y;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        }
        else if (ok && (muzzle_g.y < by0 || muzzle_g.y > by1)) ok = false;

        if (ok && t0 <= t1)
        {
            // Point on the line nearest the box centre, clamped to the chord
            // inside the box → bounded against over/under-extension.
            const float tc = (bc.x - muzzle_g.x) * d.x + (bc.y - muzzle_g.y) * d.y;
            const float t  = std::clamp(tc, t0, t1);
            tip_g = muzzle_g + d * t;
        }
        // else: line misses the target box → keep visible end (no wild extend).
    }

    LaserResult out;
    out.found = true;
    out.muzzle = muzzle_g;
    out.visible_tip = vtip_g;
    out.tip = cv::Point2f(
        std::clamp(tip_g.x, 0.0f, static_cast<float>(bgrFrame.cols - 1)),
        std::clamp(tip_g.y, 0.0f, static_cast<float>(bgrFrame.rows - 1)));
    return out;
}

std::optional<cv::Point2f> LaserDetector::detect(
    const cv::Mat& bgrFrame,
    const LaserDetectorSettings& settings) const
{
    const LaserResult r = detectLine(bgrFrame, settings);
    if (!r.found)
        return std::nullopt;
    return r.tip;
}

} // namespace crosshair
