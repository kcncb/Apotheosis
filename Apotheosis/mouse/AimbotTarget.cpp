#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

// 模块C:body→head pivot 吸附。当 body 框够大(近距离)且其内部上半区存在一个面积
// 明显更小的 head 框时,把 pivot 移到该 head;否则保持原 pivot 不变。head 框集合由调用
// 方预先从本帧检测里收集(可不在 aim 类列表内)。
void maybe_snap_pivot_to_head(PivotResult& pivot,
                              const cv::Rect2f& bodyBox,
                              int bodyClassId,
                              float headInnerOffset,
                              const TrackerUpdate& in,
                              const std::vector<cv::Rect2f>& headBoxes)
{
    if (!in.close_range_head_aim_enabled || in.close_range_head_class_id < 0)
        return;
    if (bodyClassId == in.close_range_head_class_id) // 本身就是 head,无需吸附
        return;
    if (in.screen_height <= 0 || headBoxes.empty())
        return;
    if (bodyBox.height < in.close_range_trigger_height_frac * static_cast<float>(in.screen_height))
        return;

    const float bodyArea = bodyBox.width * bodyBox.height;
    if (bodyArea <= 0.0f)
        return;

    // 期望头部位置:body 框水平中心、靠上 12% 处。选离它最近的合规 head。
    const float expectX = bodyBox.x + bodyBox.width * 0.5f;
    const float expectY = bodyBox.y + bodyBox.height * 0.12f;

    const cv::Rect2f* best = nullptr;
    float bestD2 = std::numeric_limits<float>::max();
    for (const auto& hb : headBoxes)
    {
        const float hcx = hb.x + hb.width * 0.5f;
        const float hcy = hb.y + hb.height * 0.5f;
        // head 中心须落在 body 框水平范围内、上 55% 区域
        if (hcx < bodyBox.x || hcx > bodyBox.x + bodyBox.width)
            continue;
        if (hcy < bodyBox.y || hcy > bodyBox.y + bodyBox.height * 0.55f)
            continue;
        // head 面积须明显小于 body(滤掉重叠的另一个大框 / 同类大框)
        const float ha = hb.width * hb.height;
        if (ha <= 0.0f || ha > bodyArea * 0.6f)
            continue;
        const float dx = hcx - expectX;
        const float dy = hcy - expectY;
        const float dd = dx * dx + dy * dy;
        if (dd < bestD2)
        {
            bestD2 = dd;
            best = &hb;
        }
    }
    if (!best)
        return;

    pivot.x = static_cast<double>(best->x) + static_cast<double>(best->width) * 0.5;
    pivot.y = static_cast<double>(best->y) + static_cast<double>(best->height) * static_cast<double>(headInnerOffset);
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
// Sample a 5x5 window of normalized depth around (cx, cy) and return the
// median, normalized to [0,1] where 1 = closest. Returns -1.0f when the
// depth Mat is empty / wrong type or the sample window collapses to nothing.
float sample_depth_at(const cv::Mat& depth, double cx, double cy)
{
    if (depth.empty() || depth.type() != CV_8UC1)
        return -1.0f;
    const int x = static_cast<int>(std::lround(cx));
    const int y = static_cast<int>(std::lround(cy));
    const int x0 = std::clamp(x - 2, 0, depth.cols - 1);
    const int x1 = std::clamp(x + 2, 0, depth.cols - 1);
    const int y0 = std::clamp(y - 2, 0, depth.rows - 1);
    const int y1 = std::clamp(y + 2, 0, depth.rows - 1);
    if (x1 < x0 || y1 < y0)
        return -1.0f;

    std::array<uint8_t, 25> samples{};
    int n = 0;
    for (int yy = y0; yy <= y1 && n < static_cast<int>(samples.size()); ++yy)
    {
        const uint8_t* row = depth.ptr<uint8_t>(yy);
        for (int xx = x0; xx <= x1 && n < static_cast<int>(samples.size()); ++xx)
            samples[n++] = row[xx];
    }
    if (n == 0)
        return -1.0f;
    std::nth_element(samples.begin(), samples.begin() + n / 2, samples.begin() + n);
    return static_cast<float>(samples[n / 2]) / 255.0f;
}

// Threat score in [0,1] blended from two signals:
//   * depth_score — normalized depth at the track pivot (closer = higher)
//   * head_score  — head-class detection confidence (only when classId
//                   matches the user-selected head class)
// `ratio` linearly mixes the two: 0 = full depth, 1 = full head-conf.
// Falls back to neutral 0.5 for any unavailable signal so the threat term
// can't actively penalise a track when its inputs are missing.
template <typename TrackT>
float compute_threat_score(const TrackT& track,
                           int head_class_id,
                           float ratio,
                           const cv::Mat& depth_normalized)
{
    float depth_score = 0.5f;
    const float d = sample_depth_at(depth_normalized, track.pivotX, track.pivotY);
    if (d >= 0.0f)
        depth_score = std::clamp(d, 0.0f, 1.0f);

    float head_score = 0.5f;
    if (head_class_id >= 0)
        head_score = (track.classId == head_class_id)
                     ? std::clamp(track.confidence, 0.0f, 1.0f)
                     : 0.0f;

    const float r  = std::clamp(ratio, 0.0f, 1.0f);
    const float wd = 1.0f - r;
    const float wh = r;
    return std::clamp(wd * depth_score + wh * head_score, 0.0f, 1.0f);
}
} // namespace

bool MultiTargetTracker::evaluateTrack(int trackIndex,
                                      const std::vector<int>& priority,
                                      int screenWidth,
                                      int screenHeight,
                                      bool threat_enabled,
                                      float threat_weight,
                                      int   threat_head_class_id,
                                      float threat_depth_head_ratio,
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

    // Distance reference is the aim origin: the crosshair-colour pivot when
    // that feature is live (where the reticle actually sits after recoil),
    // otherwise the detection-image centre (sentinel < 0 from the loop).
    const double cx = (lastAimOriginX_ >= 0.0) ? lastAimOriginX_ : screenWidth * 0.5;
    const double cy = (lastAimOriginY_ >= 0.0) ? lastAimOriginY_ : screenHeight * 0.5;
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

    // Selection score = pure distance from the aim origin to the target pivot.
    // Within a priority tier the nearest target to the crosshair always wins;
    // switch stickiness is handled separately by the lock hysteresis, and
    // coasting phantoms are kept out of contention by the observed-this-frame
    // guards in update(). (Threat / hit / miss terms intentionally dropped.)
    (void)threat_enabled;
    (void)threat_weight;
    (void)threat_head_class_id;
    (void)threat_depth_head_ratio;

    outRank = rankFound;
    outScore = std::hypot(dx, dy);
    return true;
}

int MultiTargetTracker::chooseBestTrack(const std::vector<int>& priority,
                                        int screenWidth,
                                        int screenHeight,
                                        bool observedOnly) const
{
    // Threat scoring is opt-in; chooseBestTrack itself is on the path used
    // both with and without an active aim hotkey. Read the cached threat
    // params we stashed in update().
    const bool  threat_enabled = lastThreatEnabled_;
    const float threat_weight  = lastThreatWeight_;
    const int   threat_head    = lastThreatHeadClassId_;
    const float threat_ratio   = lastThreatDepthHeadRatio_;

    if (tracks_.empty() || priority.empty())
        return -1;

    int bestIdx = -1;
    int bestRank = std::numeric_limits<int>::max();
    double bestScore = std::numeric_limits<double>::max();

    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        if (observedOnly && !tracks_[i].observedThisFrame)
            continue;

        int rank = 0;
        double score = 0.0;
        float threat = 0.0f;
        if (!evaluateTrack(static_cast<int>(i), priority,
                           screenWidth, screenHeight,
                           threat_enabled, threat_weight, threat_head, threat_ratio,
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
    killSuspectedTrackId_ = -1;
    killGraceRemaining_ = 0;
}

void MultiTargetTracker::update(const TrackerUpdate& in)
{
    const auto now = std::chrono::steady_clock::now();

    // Stash threat + dynamic-FOV gate so chooseBestTrack (also called from
    // elsewhere) applies the same filtering / weighting between updates.
    // The depth Mat is held as a refcounted shallow copy so chooseBestTrack
    // can sample it inside / between update() calls without dangling.
    lastThreatEnabled_ = in.threat_priority_enabled;
    lastThreatWeight_ = in.threat_weight;
    lastThreatHeadClassId_ = in.threat_head_class_id;
    lastThreatDepthHeadRatio_ = in.threat_depth_head_ratio;
    if (in.depth_normalized && !in.depth_normalized->empty())
        lastDepthNormalized_ = *in.depth_normalized;
    else
        lastDepthNormalized_.release();
    lastFovRadiusXpx_ = in.fov_radius_x_px;
    lastFovRadiusYpx_ = in.fov_radius_y_px;
    lastAimOriginX_ = in.aim_origin_x;
    lastAimOriginY_ = in.aim_origin_y;

    for (auto& t : tracks_)
        t.observedThisFrame = false;

    if (!in.boxes || !in.classes || in.boxes->size() != in.classes->size())
    {
        pruneDeadTracks();
        return;
    }

    // 模块C:先收集本帧所有 head 框(body→head pivot 吸附用)。head 类不必是 aim 类,
    // 所以从原始检测里收集,不经 aim_class_ids 过滤。
    std::vector<cv::Rect2f> headBoxes;
    if (in.close_range_head_aim_enabled && in.close_range_head_class_id >= 0)
    {
        for (size_t i = 0; i < in.boxes->size(); ++i)
        {
            if ((*in.classes)[i] != in.close_range_head_class_id)
                continue;
            const cv::Rect& b = (*in.boxes)[i];
            headBoxes.emplace_back(static_cast<float>(b.x), static_cast<float>(b.y),
                                   static_cast<float>(b.width), static_cast<float>(b.height));
        }
    }

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
        d.confidence = (in.confidences && i < in.confidences->size())
                           ? std::clamp((*in.confidences)[i], 0.0f, 1.0f)
                           : 0.0f;
        auto it = in.y_offsets.find(cls);
        const float user_offset = (it != in.y_offsets.end())
            ? std::clamp(it->second, 0.0f, 1.0f)
            : 0.5f;
        const float offset = apply_y_offset_size_decay(
            user_offset, d.box.height, in.screen_height,
            in.y_offset_size_decay_enabled,
            in.y_offset_size_decay_low_frac,
            in.y_offset_size_decay_high_frac);
        PivotResult pivot = geometric_pivot(d.box, offset);

        // 模块C:近距离大 body 框 → 把瞄点吸附到框内上半区的 head 框(用原始 user_offset
        // 在 head 框内取点,避免 size_decay 把它拉向中心)。
        maybe_snap_pivot_to_head(pivot, d.box, cls, user_offset, in, headBoxes);

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
            t.confidence = d.confidence;
            t.hits += 1;
            t.missed = 0;
            t.observedThisFrame = true;
            t.lastUpdate = now;

            // 击杀检测：维护"上次观测时 bbox 面积 / 当前面积"比例。塌缩到阈值以下且
            // 紧接着失观，就是击杀征兆。
            const float newArea = d.box.width * d.box.height;
            if (t.lastObservedArea > 1.0f)
                t.lastAreaRatio = newArea / t.lastObservedArea;
            else
                t.lastAreaRatio = 1.0f;
            t.lastObservedArea = newArea;
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
        t.confidence = d.confidence;
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

    // -------------------------------------------------------------------
    // 击杀检测 / 多人锁切。
    //
    //   触发条件 (AND)：
    //     1) kill_detect_enabled = true（HotkeyProfile / TrackerUpdate 配置）
    //     2) trigger 在最近 kill_trigger_fresh_ms 内击发过（ms_since_last_fire 由调用方维护）
    //     3) 锁定目标失观 ≥ kill_suspicion_missed_frames 帧
    //     4) bbox 在最后一次观测时面积已塌缩到 ≤ kill_suspicion_bbox_shrink（或塌缩判定关闭）
    //
    //   命中击杀时：清空 lock_hold 计时、强制切到本帧可见的最优次目标，并为后续 N 帧
    //   开启"次目标快速接管" grace（killGraceRemaining_ > 0 时 same-rank hysteresis 被绕过）。
    //
    //   killGraceRemaining_ 每帧递减一次，确保仅在击杀瞬间打开"零阻力切换"窗口。
    if (killGraceRemaining_ > 0)
        --killGraceRemaining_;

    if (in.kill_detect_enabled)
    {
        const int li = findTrackIndexById(lockedTrackId_);
        if (li >= 0 && !tracks_[li].observedThisFrame
            && tracks_[li].missed >= std::max(1, in.kill_suspicion_missed_frames)
            && in.ms_since_last_fire <= std::max(0, in.kill_trigger_fresh_ms))
        {
            const bool shrinkOK = (in.kill_suspicion_bbox_shrink <= 0.0f)
                || (tracks_[li].lastAreaRatio <= in.kill_suspicion_bbox_shrink);
            if (shrinkOK)
            {
                const int obsBest = chooseBestTrack(in.priority_class_ids,
                                                    in.screen_width, in.screen_height,
                                                    /*observedOnly=*/true);
                if (obsBest >= 0 && tracks_[obsBest].id != lockedTrackId_)
                {
                    killSuspectedTrackId_ = lockedTrackId_;
                    killGraceRemaining_ = std::max(0, in.kill_followup_grace_frames);
                    setLockedTrack(tracks_[obsBest].id);
                    lockHoldRemaining_ = 0;  // 击杀场景下不让 lock_hold 拦腰拦下
                    clearChallenger();
                    return;
                }
            }
        }
    }

    // Coasting-lock fast handoff. If the locked target wasn't observed this
    // frame (it dropped out) and has now coasted past a short grace, hand the
    // lock to the best target that is genuinely on screen RIGHT NOW. Without
    // this, a departed target keeps the lock for its full missed-frame
    // allowance (and behind the minimum-hold gate below) while a replacement
    // is already visible — the "hesitates instead of switching to the next
    // target" symptom. The grace preserves single-/double-frame recoil
    // dropout survival, where abandoning the lock would be wrong.
    {
        const int coastGrace = std::max(1, in.coast_grace_frames);
        const int li = findTrackIndexById(lockedTrackId_);
        if (li >= 0 && !tracks_[li].observedThisFrame
            && tracks_[li].missed > coastGrace)
        {
            const int obsBest = chooseBestTrack(in.priority_class_ids,
                                                in.screen_width, in.screen_height,
                                                /*observedOnly=*/true);
            if (obsBest >= 0 && tracks_[obsBest].id != lockedTrackId_)
            {
                setLockedTrack(tracks_[obsBest].id);
                clearChallenger();
                return;
            }
        }
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

    // Never hand a live (observed-this-frame) lock over to a coasting phantom.
    // A just-departed target lingers as a velocity-extrapolated candidate for
    // several frames; if it scores better than the freshly-acquired, on-screen
    // lock it would repeatedly challenge it and make the pivot jitter until the
    // phantom is pruned — the "wobbles for a moment after acquiring the next
    // target, but only with multiple targets" symptom. Keep the observed lock.
    if (lockedIdx >= 0
        && tracks_[lockedIdx].observedThisFrame
        && !tracks_[bestIdx].observedThisFrame)
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
        in.threat_priority_enabled, in.threat_weight,
        in.threat_head_class_id, in.threat_depth_head_ratio,
        lockedRank, lockedScore, lockedThreat,
        /*is_locked=*/true);

    int bestRank = 0;
    double bestScore = 0.0;
    float bestThreat = 0.0f;
    if (!evaluateTrack(bestIdx, in.priority_class_ids,
                       in.screen_width, in.screen_height,
                       in.threat_priority_enabled, in.threat_weight,
                       in.threat_head_class_id, in.threat_depth_head_ratio,
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

    // Locked track outranks the best in-FOV challenger. This is reachable
    // only when the lock sits momentarily outside the dynamic-FOV ellipse
    // (chooseBestTrack honours the gate; the locked-track evaluation above
    // bypasses it via is_locked=true), so chooseBestTrack returns a
    // lower-priority candidate. Priority must dominate symmetrically: never
    // hand a higher-priority lock to a lower-priority challenger on score
    // alone. If the locked target is genuinely gone it becomes ineligible
    // (handled above) or gets pruned, and the lock re-acquires then.
    if (bestRank > lockedRank)
    {
        clearChallenger();
        return;
    }

    // Same rank — apply switch hysteresis. A challenger track must
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

    // 击杀 grace：刚连杀完，新挑战者立即夺锁，不必等 hysteresis 累计帧数。
    if (killGraceRemaining_ > 0)
    {
        setLockedTrack(tracks_[bestIdx].id);
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
        d.confidence = t.confidence;
        d.depth_at_pivot = sample_depth_at(lastDepthNormalized_, t.pivotX, t.pivotY);
        d.threat = compute_threat_score(t,
                                        lastThreatHeadClassId_,
                                        lastThreatDepthHeadRatio_,
                                        lastDepthNormalized_);
        out.push_back(d);
    }

    return out;
}
