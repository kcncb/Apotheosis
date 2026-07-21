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

    // CLOSE to re-bridge a single physical light that the threshold split into
    // multiple connected components. This happens when a foreground silhouette
    // (an enemy body, a door frame, window mullions, blinds) cuts across a
    // bright background — RETR_EXTERNAL then yields N small contours sitting
    // around the silhouette instead of one big halo. Each fragment scores low
    // on circularity and lands a slot in the candidate list, polluting both
    // the cap and the eventual aim point.
    //
    // Kernel sized to ¼ of max_radius so it can span the typical silhouette
    // gap (≈10–30 px at 1080p detection) without merging genuinely separate
    // distant lights (those are caught by NMS later). Capped at 15 px so a
    // huge max_radius can't pull the cost off a cliff (OpenCV CPU morph is
    // O(k²) per pixel). Floored at 3 so it always does *something*.
    const int close_r = std::clamp(max_r / 4, 3, 15);
    const int close_k = 2 * close_r + 1;
    const cv::Mat close_ker = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(close_k, close_k));
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, close_ker);

    // Near-field flood: a point-blank flashlight washes out most of the view,
    // leaving no localizable contour (it's a screen-filling gradient, not a
    // blob). Detect that by sheer coverage and report a single centre spot —
    // when you're being blinded at touching distance the enemy is dead ahead,
    // so frame centre is the best aim point. Threshold is deliberately high
    // (≈two-thirds of the frame) so a merely bright sky never trips it.
    const int frame_px = mask.rows * mask.cols;
    if (frame_px > 0 && cv::countNonZero(mask) > static_cast<int>(0.65 * frame_px))
    {
        FlashlightCandidate flood;
        flood.center      = cv::Point2f(mask.cols * 0.5f, mask.rows * 0.5f);
        flood.radius      = 0.25f * static_cast<float>(std::min(mask.cols, mask.rows));
        flood.circularity = 1.0f;
        flood.contrast    = 255.0f;
        flood.accepted    = true;
        out.push_back(flood);
        return;
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
        // Confidence blends three scale-invariant cues: roundness, radial
        // falloff strength (the flashlight signature), and the brightness gate.
        const float contrast_norm = std::clamp(cd.contrast / 255.0f, 0.0f, 1.0f);
        const float conf = std::clamp(
            0.5f * cd.circularity + 0.3f * contrast_norm + 0.2f * bright_norm,
            0.0f, 1.0f);
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
    const float bright_norm = std::clamp(static_cast<float>(thr) / 255.0f, 0.0f, 1.0f);
    const int cap         = std::max(1, settings.max_spots);

    struct Ref { size_t idx; float conf; float radius; cv::Point2f c; };
    std::vector<Ref> accepted;
    accepted.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++i)
    {
        if (!cands[i].accepted) continue;
        const float contrast_norm = std::clamp(cands[i].contrast / 255.0f, 0.0f, 1.0f);
        const float conf = std::clamp(
            0.5f * cands[i].circularity + 0.3f * contrast_norm + 0.2f * bright_norm,
            0.0f, 1.0f);
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
