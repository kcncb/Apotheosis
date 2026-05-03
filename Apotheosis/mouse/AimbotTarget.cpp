#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "AimbotTarget.h"

AimbotTarget::AimbotTarget()
    : x(0), y(0), w(0), h(0), classId(0), pivotX(0.0), pivotY(0.0)
{
}

namespace
{
struct PivotResult
{
    double x;
    double y;
};

// Blend a user-supplied y_offset toward 0.5 (geometric center) as the bbox
// height grows from `low_frac` × screen_height up to `high_frac` × screen_h.
// At small bboxes (far targets) the user's offset is honoured exactly; at
// large bboxes (close targets) the pivot snaps to the centre to suppress
// pixel-domain jitter that would otherwise translate to large screen-space
// errors. Returns the original offset when disabled or when fractions are
// degenerate.
float apply_y_offset_size_decay(float user_offset,
                                float box_height,
                                int screen_height,
                                bool enabled,
                                float low_frac,
                                float high_frac)
{
    if (!enabled || screen_height <= 0 || box_height <= 0.0f)
        return user_offset;

    const float lo = std::clamp(low_frac, 0.0f, 1.0f);
    const float hi = std::clamp(high_frac, 0.0f, 1.0f);
    if (hi <= lo)
        return user_offset;

    const float h_frac = box_height / static_cast<float>(screen_height);
    const float alpha = std::clamp((h_frac - lo) / (hi - lo), 0.0f, 1.0f);

    constexpr float kCenter = 0.5f;
    return user_offset + alpha * (kCenter - user_offset);
}

// Default geometric pivot: horizontal center, y biased by y_offset.
PivotResult geometric_pivot(const cv::Rect2f& box, float y_offset)
{
    return {
        static_cast<double>(box.x) + static_cast<double>(box.width) * 0.5,
        static_cast<double>(box.y) + static_cast<double>(box.height) * y_offset
    };
}

// Pivot derived from the distribution of "visible" pixels (mask == 0) inside
// the bbox. Falls back to the geometric pivot when the mask is unusable or
// visibility is too sparse to be trusted. This exists so that a target with
// half its bbox occluded by a wall still has a pivot over the exposed half
// instead of on the wall.
PivotResult visibility_weighted_pivot(const cv::Rect2f& box,
                                      float y_offset,
                                      const cv::Mat& mask)
{
    const PivotResult fallback = geometric_pivot(box, y_offset);

    if (mask.empty() || mask.type() != CV_8UC1)
        return fallback;

    const cv::Rect img_rect(0, 0, mask.cols, mask.rows);
    const cv::Rect box_int(
        static_cast<int>(std::floor(box.x)),
        static_cast<int>(std::floor(box.y)),
        std::max(1, static_cast<int>(std::ceil(box.width))),
        std::max(1, static_cast<int>(std::ceil(box.height)))
    );
    const cv::Rect roi = box_int & img_rect;
    if (roi.area() <= 0)
        return fallback;

    const cv::Mat sub = mask(roi);

    long long sum_x = 0;
    int visible_count = 0;
    int min_y = roi.height;
    int max_y = -1;
    for (int y = 0; y < sub.rows; ++y)
    {
        const uint8_t* row = sub.ptr<uint8_t>(y);
        for (int x = 0; x < sub.cols; ++x)
        {
            if (row[x] == 0)
            {
                sum_x += x;
                ++visible_count;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    // Require a non-trivial chunk of visible pixels before trusting the
    // centroid. If the bbox is essentially all suppressed (false positive on
    // wall) or essentially all visible (no occlusion → geometric is fine
    // anyway), keep the geometric pivot.
    const int min_visible = std::max(16, roi.area() / 20); // ~5%
    const int max_visible = roi.area() - (roi.area() / 20);
    if (visible_count < min_visible || visible_count > max_visible)
        return fallback;

    const double mean_x_in_roi = static_cast<double>(sum_x) / visible_count;
    const double visible_top = static_cast<double>(roi.y + min_y);
    const double visible_bottom = static_cast<double>(roi.y + max_y);
    const double visible_height = visible_bottom - visible_top;

    return {
        static_cast<double>(roi.x) + mean_x_in_roi,
        visible_top + visible_height * static_cast<double>(y_offset)
    };
}
} // namespace

AimbotTarget::AimbotTarget(int x_, int y_, int w_, int h_, int cls, double px, double py)
    : x(x_), y(y_), w(w_), h(h_), classId(cls), pivotX(px), pivotY(py)
{
}

float MultiTargetTracker::iou(const cv::Rect2f& a, const cv::Rect2f& b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float w = std::max(0.0f, x2 - x1);
    const float h = std::max(0.0f, y2 - y1);
    const float inter = w * h;
    const float ua = a.width * a.height + b.width * b.height - inter;
    if (ua <= 1e-6f) return 0.0f;
    return inter / ua;
}

int MultiTargetTracker::findTrackIndexById(int id) const
{
    for (size_t i = 0; i < tracks_.size(); ++i)
        if (tracks_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

int MultiTargetTracker::allowedMissedFrames(const TrackState& t) const
{
    const int lockedBonus = (t.id == lockedTrackId_) ? 8 : 0;
    return maxMissedFrames_ + lockedBonus;
}

void MultiTargetTracker::pruneDeadTracks()
{
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(), [&](const TrackState& t) {
            return t.missed > allowedMissedFrames(t);
        }),
        tracks_.end());
}

namespace
{
// Production heuristic threat estimator. Returns a value in [0,1]; 1 = "this
// target is likely about to shoot you, drop them first". Combines explicit
// user-selected class tiers (head > body > other), motion direction toward the
// crosshair, and hit-frame count.
template <typename TrackT>
float compute_threat_score(const TrackT& track,
                            int   head_class_id,
                            int   body_class_id,
                            double crosshair_x,
                            double crosshair_y)
{
    // Class hint. Unselected ids (-1) do not participate in matching.
    float class_score = 0.30f;
    if (track.classId == head_class_id && head_class_id >= 0)
        class_score = 1.00f;
    else if (track.classId == body_class_id && body_class_id >= 0)
        class_score = 0.60f;

    // Motion-toward-crosshair score in [0,1]. Dot product of (crosshair -
    // pivot) with velocity, normalised. Stationary targets contribute 0.5
    // (neutral) — they're neither closing nor fleeing.
    const double dx = crosshair_x - track.pivotX;
    const double dy = crosshair_y - track.pivotY;
    const double pivot_dist = std::hypot(dx, dy);
    const double speed = std::hypot(static_cast<double>(track.velocity.x),
                                    static_cast<double>(track.velocity.y));
    float motion_score = 0.50f;
    if (pivot_dist > 1.0 && speed > 1.0)
    {
        const double dot = (dx * track.velocity.x + dy * track.velocity.y)
            / (pivot_dist * speed);
        motion_score = static_cast<float>(0.5 + 0.5 * std::clamp(dot, -1.0, 1.0));
    }

    // Confidence: hits saturate quickly so we don't penalise newly-acquired
    // but obviously-real tracks too hard.
    const float confidence = std::min(1.0f, static_cast<float>(track.hits) / 10.0f);

    // Weighted blend. Class is the strongest single signal because the user
    // explicitly told us which classes matter; motion is next; confidence
    // just gates the others.
    const float blended = (0.55f * class_score + 0.35f * motion_score + 0.10f * 1.0f) * (0.4f + 0.6f * confidence);
    return std::clamp(blended, 0.0f, 1.0f);
}
} // namespace

bool MultiTargetTracker::evaluateTrack(int trackIndex,
                                      const std::vector<int>& priority,
                                      int screenWidth,
                                      int screenHeight,
                                      bool threat_enabled,
                                      float threat_weight,
                                      int   threat_head_class_id,
                                      int   threat_body_class_id,
                                      int& outRank,
                                      double& outScore,
                                      float& outThreat,
                                      bool is_locked) const
{
    outThreat = 0.5f;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks_.size()))
        return false;

    const auto& t = tracks_[trackIndex];
    if (t.missed > allowedMissedFrames(t))
        return false;

    int rankFound = -1;
    for (size_t i = 0; i < priority.size(); ++i)
    {
        if (priority[i] == t.classId)
        {
            rankFound = static_cast<int>(i);
            break;
        }
    }
    if (rankFound < 0)
        return false;

    const double cx = screenWidth * 0.5;
    const double cy = screenHeight * 0.5;
    const double dx = t.pivotX - cx;
    const double dy = t.pivotY - cy;

    // Dynamic-FOV gate. Standard ellipse equation: (dx/rx)^2 + (dy/ry)^2 ≤ 1.
    // Skipped when either radius is non-positive (legacy / disabled).
    // The currently-locked track bypasses this gate: a tight dynamic-FOV
    // ellipse can briefly exclude the lock when the pivot oscillates near
    // the boundary, which would otherwise force the lock to hand off to a
    // different candidate even though the user clearly wants to stick.
    if (!is_locked && lastFovRadiusXpx_ > 0.0f && lastFovRadiusYpx_ > 0.0f)
    {
        const double nx = dx / lastFovRadiusXpx_;
        const double ny = dy / lastFovRadiusYpx_;
        if (nx * nx + ny * ny > 1.0)
            return false;
    }

    const double dist = std::hypot(dx, dy);
    const double hitBonus = std::min(5, t.hits) * 4.0;
    const double missPenalty = t.missed * 50.0;

    double score = dist + missPenalty - hitBonus;

    if (threat_enabled)
    {
        const float threat = compute_threat_score(t, threat_head_class_id, threat_body_class_id, cx, cy);
        outThreat = threat;

        // Map threat in [0,1] to a multiplicative score adjustment in
        // [1 - weight, 1 + weight]. threat=1 → score *= (1 - weight) (best);
        // threat=0 → score *= (1 + weight) (worst); threat=0.5 → no change.
        const double w = std::clamp(static_cast<double>(threat_weight), 0.0, 1.0);
        const double mul = 1.0 - w * (static_cast<double>(threat) - 0.5) * 2.0;
        score *= std::max(0.05, mul);
    }

    outRank = rankFound;
    outScore = score;
    return true;
}

int MultiTargetTracker::chooseBestTrack(const std::vector<int>& priority,
                                        int screenWidth,
                                        int screenHeight) const
{
    // Threat scoring is opt-in; chooseBestTrack itself is on the path used
    // both with and without an active aim hotkey. Read the cached threat
    // params we stashed in update().
    const bool threat_enabled = lastThreatEnabled_;
    const float threat_weight = lastThreatWeight_;
    const int   threat_head = lastThreatHeadClassId_;
    const int   threat_body = lastThreatBodyClassId_;

    if (tracks_.empty() || priority.empty())
        return -1;

    int bestIdx = -1;
    int bestRank = std::numeric_limits<int>::max();
    double bestScore = std::numeric_limits<double>::max();

    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        int rank = 0;
        double score = 0.0;
        float threat = 0.0f;
        if (!evaluateTrack(static_cast<int>(i), priority,
                           screenWidth, screenHeight,
                           threat_enabled, threat_weight, threat_head, threat_body,
                           rank, score, threat))
            continue;

        if (rank < bestRank || (rank == bestRank && score < bestScore))
        {
            bestRank = rank;
            bestScore = score;
            bestIdx = static_cast<int>(i);
        }
    }

    return bestIdx;
}

void MultiTargetTracker::reset()
{
    tracks_.clear();
    nextId_ = 1;
    lockedTrackId_ = -1;
    challengerTrackId_ = -1;
    challengerStreak_ = 0;
    lockHoldRemaining_ = 0;
}

void MultiTargetTracker::update(const TrackerUpdate& in)
{
    const auto now = std::chrono::steady_clock::now();

    // Stash threat + dynamic-FOV gate so chooseBestTrack (also called from
    // elsewhere) applies the same filtering / weighting between updates.
    lastThreatEnabled_ = in.threat_priority_enabled;
    lastThreatWeight_ = in.threat_weight;
    lastThreatHeadClassId_ = in.threat_head_class_id;
    lastThreatBodyClassId_ = in.threat_body_class_id;
    lastFovRadiusXpx_ = in.fov_radius_x_px;
    lastFovRadiusYpx_ = in.fov_radius_y_px;

    for (auto& t : tracks_)
        t.observedThisFrame = false;

    if (!in.boxes || !in.classes || in.boxes->size() != in.classes->size())
    {
        pruneDeadTracks();
        return;
    }

    const cv::Mat empty_mask;
    const cv::Mat& mask_ref = (in.visibility_mask != nullptr) ? *in.visibility_mask : empty_mask;

    std::vector<DetectionCandidate> dets;
    dets.reserve(in.boxes->size());
    for (size_t i = 0; i < in.boxes->size(); ++i)
    {
        const int cls = (*in.classes)[i];
        if (in.aim_class_ids.find(cls) == in.aim_class_ids.end())
            continue;

        const cv::Rect& b = (*in.boxes)[i];
        DetectionCandidate d;
        d.box = cv::Rect2f(static_cast<float>(b.x), static_cast<float>(b.y),
                           static_cast<float>(b.width), static_cast<float>(b.height));
        d.classId = cls;
        auto it = in.y_offsets.find(cls);
        const float user_offset = (it != in.y_offsets.end())
            ? std::clamp(it->second, 0.0f, 1.0f)
            : 0.5f;
        const float offset = apply_y_offset_size_decay(
            user_offset, d.box.height, in.screen_height,
            in.y_offset_size_decay_enabled,
            in.y_offset_size_decay_low_frac,
            in.y_offset_size_decay_high_frac);
        const PivotResult pivot = visibility_weighted_pivot(d.box, offset, mask_ref);
        d.pivotX = pivot.x;
        d.pivotY = pivot.y;
        dets.push_back(d);
    }

    std::vector<int> detAssigned(dets.size(), -1);
    std::vector<int> trackAssigned(tracks_.size(), -1);

    auto computeMatchScore = [&](const TrackState& t, const DetectionCandidate& d, bool relaxedForLocked) -> double
    {
        if (d.classId != t.classId)
            return std::numeric_limits<double>::infinity();

        const double dt = std::clamp(
            std::chrono::duration<double>(now - t.lastUpdate).count(),
            1e-4, 0.25
        );

        const float predCx = t.box.x + t.box.width * 0.5f + t.velocity.x * static_cast<float>(dt);
        const float predCy = t.box.y + t.box.height * 0.5f + t.velocity.y * static_cast<float>(dt);
        cv::Rect2f predBox(predCx - t.box.width * 0.5f, predCy - t.box.height * 0.5f, t.box.width, t.box.height);

        const double detCx = d.box.x + d.box.width * 0.5;
        const double detCy = d.box.y + d.box.height * 0.5;
        const double dist = std::hypot(detCx - predCx, detCy - predCy);

        const double diag = std::hypot(static_cast<double>(t.box.width), static_cast<double>(t.box.height));
        const double speed = std::hypot(t.velocity.x, t.velocity.y);
        const double baseGate = std::max(24.0, diag * 1.15 + 10.0);
        const double speedGate = speed * dt * (1.8 + t.missed * 0.35);
        const double missGate = t.missed * std::max(14.0, diag * 0.18);
        double maxDist = baseGate + speedGate + missGate;
        if (relaxedForLocked)
            maxDist *= 1.10;

        if (dist > maxDist)
            return std::numeric_limits<double>::infinity();

        const double overlap = iou(predBox, d.box);
        const double missPenalty = t.missed * 0.025;
        const double hitBonus = std::min(6, t.hits) * 0.01;

        // Heavy IoU bias for the locked track. High-recoil / muzzle-flash
        // weapons spawn phantom same-class detections offset from the real
        // target; they are *close in distance* but *poorly overlap* the
        // predicted bbox. A 5x stronger overlap weight makes the matcher
        // prefer the real (well-overlapping) detection even when a phantom
        // sits a few pixels nearer the predicted centre.
        const bool is_locked_track = (t.id == lockedTrackId_);
        // Locked track: extreme IoU bias and a steep cliff when overlap is
        // poor. Phantom boxes from muzzle flash / smoke / heavy recoil
        // typically land near the predicted centre but with little overlap;
        // these penalties make them lose to a real detection that still has
        // any meaningful overlap, even when several pixels further away.
        const double overlapWeight = is_locked_track ? 4.00 : 0.30;
        double score = (dist / maxDist) + (1.0 - overlap) * overlapWeight + missPenalty - hitBonus;
        if (is_locked_track)
        {
            if (overlap < 0.20)
                score += 8.0;
            else if (overlap < 0.35)
                score += 2.0;
        }
        return score;
    };

    auto tryAssignTrack = [&](int trackIndex, bool relaxedForLocked) {
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks_.size()))
            return;
        if (trackAssigned[trackIndex] != -1)
            return;

        double bestScore = std::numeric_limits<double>::infinity();
        int bestDet = -1;
        const auto& track = tracks_[trackIndex];

        for (size_t di = 0; di < dets.size(); ++di)
        {
            if (detAssigned[di] != -1)
                continue;
            const double score = computeMatchScore(track, dets[di], relaxedForLocked);
            if (score < bestScore)
            {
                bestScore = score;
                bestDet = static_cast<int>(di);
            }
        }

        if (bestDet >= 0)
        {
            trackAssigned[trackIndex] = bestDet;
            detAssigned[bestDet] = trackIndex;
        }
    };

    if (lockedTrackId_ != -1)
    {
        const int lockedIdx = findTrackIndexById(lockedTrackId_);
        if (lockedIdx >= 0)
            tryAssignTrack(lockedIdx, true);
    }

    while (true)
    {
        double bestScore = std::numeric_limits<double>::max();
        int bestTi = -1;
        int bestDi = -1;

        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            if (trackAssigned[ti] != -1)
                continue;
            const auto& t = tracks_[ti];
            for (size_t di = 0; di < dets.size(); ++di)
            {
                if (detAssigned[di] != -1)
                    continue;
                const auto& d = dets[di];
                const double score = computeMatchScore(t, d, false);
                if (!std::isfinite(score))
                    continue;

                if (score < bestScore)
                {
                    bestScore = score;
                    bestTi = static_cast<int>(ti);
                    bestDi = static_cast<int>(di);
                }
            }
        }

        if (bestTi < 0 || bestDi < 0)
            break;

        trackAssigned[bestTi] = bestDi;
        detAssigned[bestDi] = bestTi;
    }

    for (size_t ti = 0; ti < tracks_.size(); ++ti)
    {
        auto& t = tracks_[ti];
        const int di = trackAssigned[ti];

        if (di >= 0)
        {
            const auto& d = dets[di];
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.2
            );

            const float oldCx = t.box.x + t.box.width * 0.5f;
            const float oldCy = t.box.y + t.box.height * 0.5f;
            const float newCx = d.box.x + d.box.width * 0.5f;
            const float newCy = d.box.y + d.box.height * 0.5f;
            cv::Point2f rawVel(
                static_cast<float>((newCx - oldCx) / dt),
                static_cast<float>((newCy - oldCy) / dt)
            );

            const double rawSpeed = std::hypot(rawVel.x, rawVel.y);
            const double maxReasonableSpeed = std::max(in.screen_width, in.screen_height) * 3.5;
            if (rawSpeed > maxReasonableSpeed && rawSpeed > 1e-4)
            {
                const float scale = static_cast<float>(maxReasonableSpeed / rawSpeed);
                rawVel *= scale;
            }

            const bool is_locked = (t.id == lockedTrackId_);

            // Locked-track jump-rejection. With high-recoil weapons the
            // matcher may briefly latch onto a phantom whose centre jumps
            // many bbox-widths in one frame. Compare the implied per-frame
            // displacement against the smoothed velocity; if it is far
            // larger than physically plausible, reject the match and let
            // the track coast on prediction this frame.
            if (is_locked && t.hits >= 2)
            {
                const double smoothSpeed = std::hypot(t.velocity.x, t.velocity.y);
                const double jumpDist = std::hypot(newCx - oldCx, newCy - oldCy);
                const double diag = std::hypot(t.box.width, t.box.height);
                // Tighter tolerance — only allow displacements that are
                // close to the smoothed velocity prediction plus a small
                // slack proportional to the bbox.
                const double jumpBudget = smoothSpeed * dt + 0.30 * diag + 8.0;
                if (jumpDist > jumpBudget * 1.30)
                {
                    // Treat this frame as missed — coast on prediction.
                    const double dt_coast = std::clamp(
                        std::chrono::duration<double>(now - t.lastUpdate).count(),
                        0.0, 0.2);
                    t.box.x += t.velocity.x * static_cast<float>(dt_coast);
                    t.box.y += t.velocity.y * static_cast<float>(dt_coast);
                    t.pivotX += t.velocity.x * dt_coast;
                    t.pivotY += t.velocity.y * dt_coast;
                    t.velocity *= 0.92f;
                    t.missed += 1;
                    t.observedThisFrame = false;
                    t.lastUpdate = now;
                    // Free the detection so other tracks (or new tracks)
                    // can pick it up.
                    detAssigned[di] = -1;
                    trackAssigned[ti] = -1;
                    continue;
                }
            }

            const float blend = is_locked ? 0.45f : 0.35f;
            t.velocity = t.velocity * (1.0f - blend) + rawVel * blend;

            // Locked-track box / pivot smoothing. Blend the new detection
            // toward the predicted position so a single noisy frame cannot
            // teleport the aim pivot. Non-locked tracks snap as before.
            if (is_locked)
            {
                const float predCx = oldCx + t.velocity.x * static_cast<float>(dt);
                const float predCy = oldCy + t.velocity.y * static_cast<float>(dt);
                const float predBoxX = predCx - d.box.width * 0.5f;
                const float predBoxY = predCy - d.box.height * 0.5f;
                const float boxBlend = 0.50f; // 50% detection, 50% prediction
                t.box.x = predBoxX * (1.0f - boxBlend) + d.box.x * boxBlend;
                t.box.y = predBoxY * (1.0f - boxBlend) + d.box.y * boxBlend;
                t.box.width = d.box.width;
                t.box.height = d.box.height;

                const double predPivotX = t.pivotX + t.velocity.x * dt;
                const double predPivotY = t.pivotY + t.velocity.y * dt;
                const double pivotBlend = 0.50;
                t.pivotX = predPivotX * (1.0 - pivotBlend) + d.pivotX * pivotBlend;
                t.pivotY = predPivotY * (1.0 - pivotBlend) + d.pivotY * pivotBlend;
            }
            else
            {
                t.box = d.box;
                t.pivotX = d.pivotX;
                t.pivotY = d.pivotY;
            }
            t.classId = d.classId;
            t.hits += 1;
            t.missed = 0;
            t.observedThisFrame = true;
            t.lastUpdate = now;
        }
        else
        {
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                0.0, 0.2
            );
            t.box.x += t.velocity.x * static_cast<float>(dt);
            t.box.y += t.velocity.y * static_cast<float>(dt);
            t.pivotX += t.velocity.x * dt;
            t.pivotY += t.velocity.y * dt;
            const float decay = (t.id == lockedTrackId_) ? 0.90f : 0.84f;
            t.velocity *= decay;
            t.missed += 1;
            t.observedThisFrame = false;
            t.lastUpdate = now;
        }
    }

    for (size_t di = 0; di < dets.size(); ++di)
    {
        if (detAssigned[di] != -1)
            continue;

        const auto& d = dets[di];
        TrackState t;
        t.id = nextId_++;
        t.box = d.box;
        t.classId = d.classId;
        t.hits = 1;
        t.missed = 0;
        t.observedThisFrame = true;
        t.pivotX = d.pivotX;
        t.pivotY = d.pivotY;
        t.lastUpdate = now;
        tracks_.push_back(t);
    }

    pruneDeadTracks();

    if (findTrackIndexById(lockedTrackId_) < 0)
    {
        lockedTrackId_ = -1;
        challengerTrackId_ = -1;
        challengerStreak_ = 0;
        lockHoldRemaining_ = 0;
    }

    // Helper that drops any in-flight challenger streak — used whenever the
    // lock changes hands or no challenge is in progress this frame.
    const auto clearChallenger = [&]() {
        challengerTrackId_ = -1;
        challengerStreak_ = 0;
    };

    // Helper to commit a new locked track id while seeding the minimum-hold
    // counter. Pass -1 to clear the lock.
    const auto setLockedTrack = [&](int newId) {
        if (newId != lockedTrackId_)
        {
            lockedTrackId_ = newId;
            lockHoldRemaining_ = (newId >= 0) ? std::max(0, in.lock_hold_min_frames) : 0;
        }
    };

    if (!in.keep_current_lock)
    {
        const int bestIdx = chooseBestTrack(in.priority_class_ids, in.screen_width, in.screen_height);
        setLockedTrack((bestIdx >= 0) ? tracks_[bestIdx].id : -1);
        clearChallenger();
        return;
    }

    if (lockedTrackId_ == -1)
    {
        const int bestIdx = chooseBestTrack(in.priority_class_ids, in.screen_width, in.screen_height);
        setLockedTrack((bestIdx >= 0) ? tracks_[bestIdx].id : -1);
        clearChallenger();
        return;
    }

    // Minimum-hold gate. If the user-configured lock_hold_min_frames says
    // "stay locked for N frames after acquisition", honour that here before
    // any switch logic runs. The counter ticks down once per update cycle
    // and is reset on every lock transition (see setLockedTrack).
    if (lockHoldRemaining_ > 0)
    {
        --lockHoldRemaining_;
        clearChallenger();
        return;
    }

    // -------------------------------------------------------------------
    // Lock is held AND aim hotkey is active — apply switch hysteresis so the
    // pivot doesn't ping-pong when two enemies overlap.
    // -------------------------------------------------------------------
    const int lockedIdx = findTrackIndexById(lockedTrackId_);
    const int bestIdx = chooseBestTrack(in.priority_class_ids, in.screen_width, in.screen_height);

    if (bestIdx < 0)
    {
        clearChallenger();
        return;
    }

    if (tracks_[bestIdx].id == lockedTrackId_)
    {
        clearChallenger();
        return;
    }

    int lockedRank = 0;
    double lockedScore = 0.0;
    float lockedThreat = 0.0f;
    const bool lockedEligible = evaluateTrack(
        lockedIdx, in.priority_class_ids,
        in.screen_width, in.screen_height,
        in.threat_priority_enabled, in.threat_weight, in.threat_head_class_id, in.threat_body_class_id,
        lockedRank, lockedScore, lockedThreat,
        /*is_locked=*/true);

    int bestRank = 0;
    double bestScore = 0.0;
    float bestThreat = 0.0f;
    if (!evaluateTrack(bestIdx, in.priority_class_ids,
                       in.screen_width, in.screen_height,
                       in.threat_priority_enabled, in.threat_weight, in.threat_head_class_id, in.threat_body_class_id,
                       bestRank, bestScore, bestThreat))
    {
        // Shouldn't happen — chooseBestTrack already filtered — but stay safe.
        clearChallenger();
        return;
    }

    // The locked track is no longer eligible (class removed from priority,
    // dropped out, etc.) — hand the lock over without waiting.
    if (!lockedEligible)
    {
        setLockedTrack(tracks_[bestIdx].id);
        clearChallenger();
        return;
    }

    // Higher-priority class always wins immediately.
    if (bestRank < lockedRank)
    {
        setLockedTrack(tracks_[bestIdx].id);
        clearChallenger();
        return;
    }

    // Same or lower rank — apply switch hysteresis. A challenger track must
    // beat the locked track's score by at least `lock_switch_score_margin`
    // (fraction of half-diagonal) for at least `lock_switch_min_frames`
    // consecutive frames before the lock transfers. Higher values = stickier
    // lock. Set margin large or frames high to make same-rank switching
    // effectively never happen.
    (void)lockedThreat;
    (void)bestThreat;

    const double scale = std::max(
        1.0,
        std::hypot(static_cast<double>(in.screen_width),
                   static_cast<double>(in.screen_height)) * 0.5);
    const double normalisedAdvantage = (lockedScore - bestScore) / scale;

    const float marginPct = std::max(0.0f, in.lock_switch_score_margin);
    const int   minFrames = std::max(1, in.lock_switch_min_frames);

    if (normalisedAdvantage <= static_cast<double>(marginPct))
    {
        // Challenger isn't decisively better — drop any in-flight streak.
        clearChallenger();
        return;
    }

    if (challengerTrackId_ == tracks_[bestIdx].id)
    {
        challengerStreak_ += 1;
    }
    else
    {
        challengerTrackId_ = tracks_[bestIdx].id;
        challengerStreak_ = 1;
    }

    if (challengerStreak_ >= minFrames)
    {
        setLockedTrack(tracks_[bestIdx].id);
        clearChallenger();
    }
}

bool MultiTargetTracker::getLockedTarget(LockedTargetInfo& out) const
{
    const int idx = findTrackIndexById(lockedTrackId_);
    if (idx < 0)
        return false;

    const auto& t = tracks_[idx];
    if (t.missed > allowedMissedFrames(t))
        return false;

    out.trackId = t.id;
    out.observedThisFrame = t.observedThisFrame;
    out.missedFrames = t.missed;
    out.target = AimbotTarget(
        static_cast<int>(std::lround(t.box.x)),
        static_cast<int>(std::lround(t.box.y)),
        static_cast<int>(std::lround(t.box.width)),
        static_cast<int>(std::lround(t.box.height)),
        t.classId,
        t.pivotX,
        t.pivotY
    );
    return true;
}

std::vector<TrackDebugInfo> MultiTargetTracker::getDebugTracks() const
{
    std::vector<TrackDebugInfo> out;
    out.reserve(tracks_.size());

    for (const auto& t : tracks_)
    {
        if (t.missed > allowedMissedFrames(t))
            continue;

        TrackDebugInfo d;
        d.trackId = t.id;
        d.classId = t.classId;
        d.box = cv::Rect(
            static_cast<int>(std::lround(t.box.x)),
            static_cast<int>(std::lround(t.box.y)),
            static_cast<int>(std::lround(t.box.width)),
            static_cast<int>(std::lround(t.box.height))
        );
        d.pivotX = t.pivotX;
        d.pivotY = t.pivotY;
        d.observedThisFrame = t.observedThisFrame;
        d.missedFrames = t.missed;
        d.isLocked = (t.id == lockedTrackId_);
        out.push_back(d);
    }

    return out;
}
