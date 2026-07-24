#include "flashlight_detector.h"

#include <algorithm>
#include <cmath>

namespace crosshair
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;

void make_channel_maps(const cv::Mat& bgr, cv::Mat& bright, cv::Mat& spread)
{
    cv::Mat ch[3];
    cv::split(bgr, ch);
    cv::Mat lo;
    cv::max(ch[0], ch[1], bright);
    cv::max(bright, ch[2], bright);
    cv::min(ch[0], ch[1], lo);
    cv::min(lo, ch[2], lo);
    cv::subtract(bright, lo, spread);
}

// Mean brightness over a disk / annulus around `c` on a CV_8UC1 image. When
// `r_in <= 0` it's a filled disk of radius `r_out`; otherwise the ring between
// the two radii. Returns -1 when the region has no in-frame pixels. Cost is
// O(r_out²); callers only invoke it on candidates that already passed the
// cheap shape gates, so the per-frame total stays small.
double ring_mean(const cv::Mat& bright, cv::Point2f c, float r_in, float r_out)
{
    if (r_out <= 0.0f)
        return -1.0;
    const int x0 = std::max(0, static_cast<int>(std::floor(c.x - r_out)));
    const int y0 = std::max(0, static_cast<int>(std::floor(c.y - r_out)));
    const int x1 = std::min(bright.cols, static_cast<int>(std::ceil(c.x + r_out)) + 1);
    const int y1 = std::min(bright.rows, static_cast<int>(std::ceil(c.y + r_out)) + 1);
    if (x1 <= x0 || y1 <= y0)
        return -1.0;

    const float ri2 = (r_in > 0.0f) ? r_in * r_in : -1.0f;
    const float ro2 = r_out * r_out;
    double sum = 0.0;
    long   cnt = 0;
    for (int y = y0; y < y1; ++y)
    {
        const uchar* row = bright.ptr<uchar>(y);
        const float dy = static_cast<float>(y) - c.y;
        const float dy2 = dy * dy;
        for (int x = x0; x < x1; ++x)
        {
            const float dx = static_cast<float>(x) - c.x;
            const float d2 = dx * dx + dy2;
            if (d2 <= ro2 && d2 >= ri2)
            {
                sum += row[x];
                ++cnt;
            }
        }
    }
    return (cnt > 0) ? (sum / static_cast<double>(cnt)) : -1.0;
}

// 检查光强是否从白核向各个方向一致衰减。只比较环平均值会让“一边是白块、
// 另一边恰好很暗”的 UI/灯具混过去；逐射线要求大多数方向都呈现光晕梯度。
float radial_consistency(const cv::Mat& bright, cv::Point2f c, float radius,
                         int min_contrast)
{
    constexpr int kSpokes = 16;
    int valid = 0;
    int falling = 0;
    const float inner_r = std::max(1.0f, radius * 0.35f);
    const float outer_r1 = std::max(inner_r + 2.0f, radius * 1.45f);
    const float outer_r2 = std::max(outer_r1 + 1.0f, radius * 2.10f);
    const float needed_drop = std::max(8.0f, static_cast<float>(min_contrast) * 0.45f);

    for (int i = 0; i < kSpokes; ++i)
    {
        const float a = (2.0f * kPi * static_cast<float>(i)) / kSpokes;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        auto sample = [&](float r, int& value) {
            const int x = static_cast<int>(std::lround(c.x + ca * r));
            const int y = static_cast<int>(std::lround(c.y + sa * r));
            if (x < 0 || y < 0 || x >= bright.cols || y >= bright.rows)
                return false;
            value = bright.at<uchar>(y, x);
            return true;
        };
        int inner = 0, outer1 = 0, outer2 = 0;
        if (!sample(inner_r, inner) || !sample(outer_r1, outer1) || !sample(outer_r2, outer2))
            continue;
        ++valid;
        const float outer = 0.5f * static_cast<float>(outer1 + outer2);
        if (static_cast<float>(inner) - outer >= needed_drop && outer1 + 8 >= outer2)
            ++falling;
    }
    return valid > 0 ? static_cast<float>(falling) / static_cast<float>(valid) : 0.0f;
}

cv::Point2f weighted_core_center(const cv::Mat& bright, const cv::Mat& mask,
                                 const cv::Rect& bounds, int threshold,
                                 cv::Point2f fallback)
{
    double sx = 0.0, sy = 0.0, sw = 0.0;
    const cv::Rect clipped = bounds & cv::Rect(0, 0, bright.cols, bright.rows);
    for (int y = clipped.y; y < clipped.y + clipped.height; ++y)
    {
        const uchar* bp = bright.ptr<uchar>(y);
        const uchar* mp = mask.ptr<uchar>(y);
        for (int x = clipped.x; x < clipped.x + clipped.width; ++x)
        {
            if (!mp[x]) continue;
            const double w = std::max(1, static_cast<int>(bp[x]) - threshold + 1);
            sx += w * x;
            sy += w * y;
            sw += w;
        }
    }
    return sw > 0.0 ? cv::Point2f(static_cast<float>(sx / sw), static_cast<float>(sy / sw))
                    : fallback;
}

float candidate_confidence(const FlashlightCandidate& cd, int threshold)
{
    const float contrast = std::clamp(cd.contrast / 255.0f, 0.0f, 1.0f);
    const float brightness = std::clamp(static_cast<float>(threshold) / 255.0f, 0.0f, 1.0f);
    return std::clamp(0.22f * cd.circularity + 0.28f * contrast
                    + 0.35f * cd.radial_consistency + 0.15f * brightness,
                      0.0f, 1.0f);
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
    const float min_area = std::max(4.0f, 0.65f * kPi * static_cast<float>(min_r) * static_cast<float>(min_r));
    const float max_area = kPi * static_cast<float>(max_r) * static_cast<float>(max_r);
    const float min_circ = std::clamp(settings.min_circularity, 0.0f, 1.0f);

    cv::Mat bright, spread;
    make_channel_maps(bgrFrame, bright, spread);
    const int thr = std::clamp(settings.brightness_threshold, 0, 255);
    const int min_contrast = std::max(0, settings.min_local_contrast);
    cv::Mat mask, neutral;
    cv::threshold(bright, mask, static_cast<double>(thr), 255.0, cv::THRESH_BINARY);
    cv::threshold(spread, neutral,
                  static_cast<double>(std::clamp(settings.max_channel_spread, 0, 255)),
                  255.0, cv::THRESH_BINARY_INV);
    cv::bitwise_and(mask, neutral, mask);

    if (settings.open_radius > 0)
    {
        const int r = std::min(settings.open_radius, 9);
        const int k = 2 * r + 1;
        const cv::Mat ker = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        cv::morphologyEx(mask, mask, cv::MORPH_OPEN, ker);
    }

    // 大面积白屏、菜单或天空没有可定位的“核心”。旧逻辑会把覆盖率超过 65%
    // 的画面直接当成屏幕中心手电，造成灾难性误锁；严格策略下整帧丢弃。
    const int frame_px = mask.rows * mask.cols;
    if (frame_px > 0 && cv::countNonZero(mask) > static_cast<int>(0.30 * frame_px))
        return;

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
        const cv::Point2f contour_center(
            static_cast<float>(m.m10 / m.m00),
            static_cast<float>(m.m01 / m.m00));
        const cv::Rect bounds = cv::boundingRect(c);
        const cv::Point2f center = weighted_core_center(bright, mask, bounds, thr, contour_center);
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
        const float aspect = static_cast<float>(std::max(bounds.width, bounds.height)) /
                             static_cast<float>(std::max(1, std::min(bounds.width, bounds.height)));
        const bool touches_border = bounds.x <= 1 || bounds.y <= 1 ||
            bounds.x + bounds.width >= mask.cols - 1 ||
            bounds.y + bounds.height >= mask.rows - 1;
        if (touches_border)
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "border";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }
        const float required_circ = (radius < 5.0f) ? min_circ * 0.72f : min_circ;
        if (circ < required_circ || aspect > 1.65f)
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "deformed";
            cd.accepted = false;
            out.push_back(cd);
            continue;
        }

        // Radial falloff (scale-invariant): sample a core disk, a mid annulus
        // and an outer ring, ALL proportional to the detected radius — so the
        // test means the same thing for a 6 px far dot and a 120 px near halo.
        // A real glare is brightest at the core and fades outward; a flat bright
        // patch (sky / cloud / UI) reads nearly the same at every radius. We
        // store (core − outer) as the candidate's contrast for confidence
        // scoring and, when the gate is on, require both a minimum drop and a
        // monotonically-decreasing profile. This is what lets the radius band
        // stay wide (to admit near floods) without far-field clutter leaking in.
        const double core_mean  = ring_mean(bright, center, 0.0f,          std::max(1.0f, radius * 0.35f));
        const double mid_mean   = ring_mean(bright, center, radius * 0.50f, radius * 0.90f);
        const double outer_mean = ring_mean(bright, center, radius * 1.25f, radius * 1.80f);
        cd.contrast = (core_mean >= 0.0 && outer_mean >= 0.0)
                          ? static_cast<float>(core_mean - outer_mean)
                          : 0.0f;

        if (min_contrast > 0)
        {
            constexpr double kSlack = 6.0; // tolerate a saturated core plateau
            const double inner = (mid_mean >= 0.0) ? mid_mean : core_mean;
            const bool monotonic =
                (mid_mean   < 0.0 || core_mean + kSlack >= mid_mean) &&
                (outer_mean < 0.0 || inner     + kSlack >= outer_mean);
            const bool strong = (cd.contrast >= static_cast<float>(min_contrast));
            if (!(strong && monotonic))
            {
                if (!keep_rejects) continue;
                cd.reject_reason = "low contrast";
                cd.accepted = false;
                out.push_back(cd);
                continue;
            }
        }

        cd.radial_consistency = radial_consistency(bright, center, radius, min_contrast);
        if (settings.min_radial_consistency > 0.0f &&
            cd.radial_consistency < std::clamp(settings.min_radial_consistency, 0.0f, 1.0f))
        {
            if (!keep_rejects) continue;
            cd.reject_reason = "not radial";
            cd.accepted = false;
            out.push_back(cd);
            continue;
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
    spots.reserve(cands.size());
    for (const auto& cd : cands)
    {
        const float conf = candidate_confidence(cd, thr);
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

    // Dedup + cap. One physical glare can split into a saturated core plus a
    // softer halo ring (two contours, near-identical centres); collapse those
    // so they don't each eat a slot. Greedy NMS over the confidence-sorted list
    // keeps the strongest and drops any later spot whose centre falls within
    // 1.5× the larger of the two radii. The 1.5× (vs strict 1×) is the
    // residual-merge step after the CLOSE morph above: when a single physical
    // light leaves multiple fragments at 30–50 px apart (small ring + small
    // core post-split), 1×r-radius NMS misses them but 1.5×r catches them
    // without eating genuinely distant lights — those sit well past 2×r.
    // Then truncate to max_spots so a noisy frame can never hand the aim
    // pipeline a swarm of phantom lights.
    constexpr float kMergeScale = 1.5f;
    const int cap = std::max(1, settings.max_spots);
    std::vector<FlashlightSpot> kept;
    kept.reserve(std::min(spots.size(), static_cast<size_t>(cap)));
    for (const auto& s : spots)
    {
        bool dup = false;
        for (const auto& k : kept)
        {
            const float dx = s.center.x - k.center.x;
            const float dy = s.center.y - k.center.y;
            const float merge = kMergeScale * std::max(s.radius, k.radius);
            if (dx * dx + dy * dy < merge * merge) { dup = true; break; }
        }
        if (dup) continue;
        kept.push_back(s);
        if (static_cast<int>(kept.size()) >= cap) break;
    }
    return kept;
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

    // Replay detectAll's post-pipeline (confidence sort → NMS → max_spots cap)
    // on the shape-gate survivors and mark the losers so the preview matches
    // exactly what the aim loop sees. Without this the user sees N>max_spots
    // green circles and thinks the cap isn't working — the cap IS working in
    // detectAll, but detectVerbose was emitting raw contours.
    //
    // Visual contract for the overlay:
    //   accepted=true                     → green  (final survivors, ≤ max_spots)
    //   accepted=false, reason="merged"   → yellow (killed by NMS dedup)
    //   accepted=false, reason="capped"   → yellow (lost the max_spots race)
    //   accepted=false, any other reason  → red    (failed the shape gate)
    const int thr         = std::clamp(settings.brightness_threshold, 0, 255);
    const int cap         = std::max(1, settings.max_spots);

    struct Ref { size_t idx; float conf; float radius; cv::Point2f c; };
    std::vector<Ref> accepted;
    accepted.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i)
    {
        if (!cands[i].accepted) continue;
        const float conf = candidate_confidence(cands[i], thr);
        accepted.push_back({i, conf, cands[i].radius, cands[i].center});
    }
    std::sort(accepted.begin(), accepted.end(),
              [](const Ref& a, const Ref& b) {
                  if (a.conf != b.conf)   return a.conf > b.conf;
                  return a.radius > b.radius;
              });

    constexpr float kMergeScale = 1.5f;
    std::vector<Ref> kept;
    kept.reserve(std::min(accepted.size(), static_cast<size_t>(cap)));
    for (const auto& r : accepted)
    {
        bool dup = false;
        for (const auto& k : kept)
        {
            const float dx = r.c.x - k.c.x;
            const float dy = r.c.y - k.c.y;
            const float merge = kMergeScale * std::max(r.radius, k.radius);
            if (dx * dx + dy * dy < merge * merge) { dup = true; break; }
        }
        if (dup)
        {
            cands[r.idx].accepted      = false;
            cands[r.idx].reject_reason = "merged";
            continue;
        }
        if (static_cast<int>(kept.size()) >= cap)
        {
            cands[r.idx].accepted      = false;
            cands[r.idx].reject_reason = "capped";
            continue;
        }
        kept.push_back(r);
    }

    // Final TOTAL cap: limit the preview to max_spots markers altogether
    // (green + yellow + red), not max_spots green plus an unbounded swarm of
    // red. Without this, a bright-cluttered frame paints a forest of red
    // rejected circles even though the aim pipeline only ever sees ≤ cap
    // accepted spots — user has no way to read the picture.
    //
    // Priority: accepted (green) first — those drive the aim and must all be
    // visible. Remaining slots filled by the most flashlight-looking rejects
    // (highest circularity × radius), so the user still sees the strongest
    // near-misses for tuning circularity / contrast / radius bands. Drop the
    // rest entirely.
    std::vector<size_t> order;
    order.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) {
                  const auto& ca = cands[a];
                  const auto& cb = cands[b];
                  if (ca.accepted != cb.accepted) return ca.accepted; // green first
                  const float sa = ca.circularity * std::max(1.0f, ca.radius);
                  const float sb = cb.circularity * std::max(1.0f, cb.radius);
                  return sa > sb;
              });
    if (static_cast<int>(order.size()) > cap) order.resize(cap);

    std::vector<FlashlightCandidate> trimmed;
    trimmed.reserve(order.size());
    for (size_t idx : order) trimmed.push_back(cands[idx]);
    return trimmed;
}

} // namespace crosshair
