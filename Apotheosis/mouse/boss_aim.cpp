#include "boss_aim.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace boss
{

void AimEngine::reset()
{
    tracks_.clear();
    next_id_ = 1;
    current_id_ = -1;
    prev_current_id_ = -1;
    lock_age_frames_ = 0;
    art_.reset();
    path_.reset();
    predictive_mover_.reset();
    last_mover_kind_ = mover::Kind::Smooth;
}

cv::Point2f AimEngine::compute_anchor(const cv::Rect& bbox,
                                      int class_id,
                                      const std::unordered_map<int, float>* y_offsets) const
{
    float y_frac = kDefaultHeadFrac;
    if (y_offsets)
    {
        auto it = y_offsets->find(class_id);
        if (it != y_offsets->end())
            y_frac = std::clamp(it->second, 0.0f, 1.0f);
    }
    return cv::Point2f(
        static_cast<float>(bbox.x) + static_cast<float>(bbox.width)  * 0.5f,
        static_cast<float>(bbox.y) + static_cast<float>(bbox.height) * y_frac);
}

void AimEngine::assignDetections(const EngineInput& in)
{
    for (auto& t : tracks_)
        t.observed_this_frame = false;

    if (!in.boxes || !in.classes)
    {
        for (auto& t : tracks_)
            ++t.missed;
        return;
    }

    const std::vector<cv::Rect>& boxes = *in.boxes;
    const std::vector<int>& classes = *in.classes;
    const std::vector<float>* confs = in.confidences;

    const double cx = in.crosshair_x;
    const double cy = in.crosshair_y;
    const bool   fov_gate = (in.fov_radius_x > 0.0 && in.fov_radius_y > 0.0);
    const double rxInv = fov_gate ? (1.0 / std::max(1.0, in.fov_radius_x)) : 0.0;
    const double ryInv = fov_gate ? (1.0 / std::max(1.0, in.fov_radius_y)) : 0.0;

    std::vector<char> track_matched(tracks_.size(), 0);

    const size_t n = std::min({ boxes.size(), classes.size(),
                                confs ? confs->size() : boxes.size() });

    for (size_t di = 0; di < n; ++di)
    {
        const cv::Rect& box = boxes[di];
        const int class_id = classes[di];
        if (in.eligible_classes
            && in.eligible_classes->find(class_id) == in.eligible_classes->end())
            continue;
        if (box.width <= 0 || box.height <= 0)
            continue;

        const double det_cx = box.x + box.width  * 0.5;
        const double det_cy = box.y + box.height * 0.5;
        if (fov_gate)
        {
            const double nx = (det_cx - cx) * rxInv;
            const double ny = (det_cy - cy) * ryInv;
            if (nx * nx + ny * ny > 1.0)
                continue;
        }

        int   best_idx  = -1;
        double best_d2  = std::numeric_limits<double>::infinity();
        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            if (track_matched[ti])
                continue;
            const Track& t = tracks_[ti];
            const double t_cx = t.bbox.x + t.bbox.width  * 0.5;
            const double t_cy = t.bbox.y + t.bbox.height * 0.5;
            const double dxv = t_cx - det_cx;
            const double dyv = t_cy - det_cy;
            const double d2  = dxv * dxv + dyv * dyv;
            if (d2 < best_d2)
            {
                best_d2  = d2;
                best_idx = static_cast<int>(ti);
            }
        }

        const double match_thresh = std::max(
            static_cast<double>(std::max(box.width, box.height)) * kMatchRatio,
            static_cast<double>(kMatchMinPx));
        const bool can_match = best_idx >= 0 && best_d2 < match_thresh * match_thresh;

        const cv::Point2f new_anchor = compute_anchor(box, class_id, in.y_offsets);
        const float conf = (confs && di < confs->size()) ? (*confs)[di] : 0.f;

        if (can_match)
        {
            Track& t = tracks_[static_cast<size_t>(best_idx)];
            t.bbox = cv::Rect2f(static_cast<float>(box.x), static_cast<float>(box.y),
                                static_cast<float>(box.width), static_cast<float>(box.height));
            t.anchor      = new_anchor;
            t.missed      = 0;
            t.class_id    = class_id;
            t.confidence  = conf;
            t.alive       = true;
            t.observed_this_frame = true;
            track_matched[static_cast<size_t>(best_idx)] = 1;
        }
        else
        {
            Track t;
            t.id          = next_id_++;
            t.bbox        = cv::Rect2f(static_cast<float>(box.x), static_cast<float>(box.y),
                                       static_cast<float>(box.width), static_cast<float>(box.height));
            t.anchor      = new_anchor;
            t.missed      = 0;
            t.alive       = true;
            t.class_id    = class_id;
            t.confidence  = conf;
            t.observed_this_frame = true;
            tracks_.push_back(std::move(t));
            track_matched.push_back(1);
        }
    }

    for (size_t ti = 0; ti < tracks_.size(); ++ti)
    {
        if (!track_matched[ti])
            ++tracks_[ti].missed;
    }
}

void AimEngine::purgeLost()
{
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
            [](const Track& t) { return !t.alive || t.missed > kMDrop; }),
        tracks_.end());
}

EngineOutput AimEngine::tick(const EngineInput& in, double dt)
{
    EngineOutput out;

    if (!(dt > 1e-6))
        dt = 1.0 / 120.0;
    if (dt > 0.25)
        dt = 0.25;

    // ─── Layer A: 目标管理 ───
    assignDetections(in);
    purgeLost();

    // Snapshot the lock at tick entry so we can correctly age / reset
    // lock_age_frames_ at the end (after any rank / hysteresis switch).
    const int locked_before = current_id_;

    auto rank_of = [&](int class_id) -> int {
        if (!in.class_priority) return 0;            // no priority info -> all equal
        auto it = in.class_priority->find(class_id);
        return (it != in.class_priority->end()) ? it->second : INT_MAX;
    };

    // Best (lowest) rank present among ALL alive tracks — we keep coasting
    // tracks (missed > 0 but <= kMDrop) in the pool so a head briefly missed
    // for 1–2 frames doesn't lose lock to a body that's still observed.
    int best_rank = INT_MAX;
    for (const auto& t : tracks_)
    {
        if (!t.alive) continue;
        const int r = rank_of(t.class_id);
        if (r < best_rank) best_rank = r;
    }

    if (best_rank == INT_MAX)
    {
        current_id_ = -1;
        prev_current_id_ = -1;
        lock_age_frames_ = 0;
        return out;
    }

    // Lock pool = alive tracks at the best rank. Within the pool we prefer
    // tracks observed THIS frame for distance / hysteresis decisions; only
    // when no priority-rank track is observed do we fall back to coasting
    // ones (keeps the lock identity stable through brief detection drops).
    std::vector<Track*> pool;
    std::vector<Track*> observed_pool;
    pool.reserve(tracks_.size());
    observed_pool.reserve(tracks_.size());
    for (auto& t : tracks_)
    {
        if (!t.alive) continue;
        if (rank_of(t.class_id) != best_rank) continue;
        pool.push_back(&t);
        if (t.missed == 0) observed_pool.push_back(&t);
    }

    if (pool.empty())
    {
        current_id_ = -1;
        prev_current_id_ = -1;
        lock_age_frames_ = 0;
        return out;
    }

    // Base pool: prefer fresh observations; fall back to coasting if none.
    std::vector<Track*> selection_pool = observed_pool.empty() ? pool : observed_pool;

    // Splice in the current lock when it's alive but missed THIS frame.
    // Without this, a 1-frame detection drop on the locked target sends
    // control through the no-hysteresis "current == nullptr" branch, where
    // any nearby observed track silently steals the lock — exactly the
    // far-distance "I just looked away and it switched" failure mode.
    if (current_id_ >= 0)
    {
        bool already_in = false;
        for (auto* t : selection_pool)
            if (t->id == current_id_) { already_in = true; break; }
        if (!already_in)
        {
            for (auto* t : pool)
                if (t->id == current_id_) { selection_pool.push_back(t); break; }
        }
    }

    auto dist_to_cross = [&](const Track* t) -> double {
        const double dx = static_cast<double>(t->anchor.x) - in.crosshair_x;
        const double dy = static_cast<double>(t->anchor.y) - in.crosshair_y;
        return std::hypot(dx, dy);
    };

    // Confidence-weighted "effective" distance.  Used for ALL lock
    // decisions inside the pool (initial pick, contender vs current).
    // A higher-confidence detection looks closer; a flickering 0.2-conf
    // ghost at the model's range cannot out-bid a solid 0.6-conf lock
    // just by being a handful of px nearer the crosshair.
    auto eff_dist = [&](const Track* t) -> double {
        const double d = dist_to_cross(t);
        const double w = std::max(static_cast<double>(t->confidence),
                                  static_cast<double>(kMinConfWeight));
        return d / w;
    };

    // Locate the previously-locked track inside the current pool.  If the
    // old lock died entirely (purged after kMDrop misses, or its class
    // got demoted by a higher-priority appearance) we drop it and
    // re-pick — that's the head-priority cross-rank fast path.
    Track* current = nullptr;
    for (Track* t : selection_pool)
        if (t->id == current_id_) { current = t; break; }

    if (!current)
    {
        double best = std::numeric_limits<double>::infinity();
        for (Track* t : selection_pool)
        {
            const double d = eff_dist(t);
            if (d < best) { best = d; current = t; }
        }
        current_id_ = current->id;
    }
    else
    {
        Track* contender = nullptr;
        double best = std::numeric_limits<double>::infinity();
        for (Track* t : selection_pool)
        {
            if (t == current) continue;
            const double d = eff_dist(t);
            if (d < best) { best = d; contender = t; }
        }
        if (contender && lock_age_frames_ >= kMinFramesLocked)
        {
            // Min-frames gate: just-locked targets get a few frames of
            // grace.  Combined with kAlphaHyst (contender must be 60%
            // closer in effective distance) and the splice-in above
            // (coasting current is always a hysteresis baseline, never a
            // free-for-all), this kills the lock_aggression=0 leak.
            const double cur_d = eff_dist(current);
            if (best < static_cast<double>(kAlphaHyst) * cur_d)
            {
                current = contender;
                current_id_ = current->id;
            }
        }
    }

    // Age the lock counter exactly once per tick.  Reset whenever the lock
    // identity moved this tick (forced re-pick from empty `current`, cross-
    // rank promotion, or hysteresis-driven swap).
    if (current_id_ == locked_before)
        ++lock_age_frames_;
    else
        lock_age_frames_ = 0;

    // ─── Layer B: Adaptive Reactive Tracker ───
    out.have_target = true;
    out.current_track_id = current->id;
    out.anchor = current->anchor;
    out.bbox   = current->bbox;

    // Coasting frame: the locked track wasn't observed this tick (we kept
    // it as `current` via the splice-in to defend against contender
    // hijack).  Its anchor is STALE — pushing it through ART would (a)
    // contaminate the position filter with a duplicate sample, (b) drive
    // the cursor toward the last-known spot, and (c) cause a snap-back when
    // the target reappears a few pixels off.  All three show up as jitter
    // even in Linear mode, which is exactly the symptom: hold cursor still
    // until a fresh observation arrives, lock identity preserved.
    if (current->missed > 0)
    {
        out.dx = 0;
        out.dy = 0;
        prev_current_id_ = current->id;
        return out;
    }

    art_.configure(in.aim);

    const int bbox_w = static_cast<int>(std::lround(current->bbox.width));
    const int bbox_h = static_cast<int>(std::lround(current->bbox.height));

    ArtResult r = art_.step(
        static_cast<double>(current->anchor.x),
        static_cast<double>(current->anchor.y),
        in.crosshair_x, in.crosshair_y,
        dt, bbox_w, bbox_h, current->id);

    // 移动控制器路由:
    //   微澜 (Smooth) → 走 ART/path 原路径,沿用 aim_path_mode 选 Linear/Bezier/Custom。
    //   疾风/流光     → 旁路 AimPathDriver,把 (filtered aim − crosshair) 喂给 PID。
    if (in.mover_kind == mover::Kind::Smooth)
    {
        if (in.path.mode == AimPathDriver::Mode::Linear)
        {
            out.dx = r.move_x;
            out.dy = r.move_y;
        }
        else
        {
            path_.configure(in.path);
            AimPathDriver::Result pr = path_.step(
                r.aim_x, r.aim_y,
                in.crosshair_x, in.crosshair_y,
                dt, current->id);
            out.dx = pr.move_x;
            out.dy = pr.move_y;
        }
    }
    else
    {
        // 切到/回到 Predictive: 清状态避免上一段微澜阶段的残留进新算法。
        if (in.mover_kind != last_mover_kind_)
        {
            predictive_mover_.reset();
            last_mover_kind_ = in.mover_kind;
        }

        const double err_x = r.aim_x - in.crosshair_x;
        const double err_y = r.aim_y - in.crosshair_y;
        predictive_mover_.configure(in.predictive_params);
        const mover::Move m = predictive_mover_.step(err_x, err_y, dt, current->id);
        out.dx = m.dx;
        out.dy = m.dy;
    }
    out.cutoff_hz = r.cutoff_hz;
    out.consistency = r.consistency;
    out.snapped = r.snapped;

    prev_current_id_ = current->id;

    return out;
}

} // namespace boss
