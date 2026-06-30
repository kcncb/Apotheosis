#include "boss_aim.h"

#include <algorithm>
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
    art_.reset();
    path_.reset();
    predictive_mover_.reset();
    classic_mover_.reset();
    last_mover_kind_ = mover::Kind::Smooth;
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
        {
            bool slot_match = false;
            for (int si = 0; si < 3; ++si)
                if (in.target_slots[si].class_id == class_id) { slot_match = true; break; }
            if (!slot_match) continue;
        }
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

        const cv::Point2f new_anchor(
            static_cast<float>(box.x) + static_cast<float>(box.width)  * 0.5f,
            static_cast<float>(box.y) + static_cast<float>(box.height) * 0.5f);
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

    // ─── 目标选择: 3 槽位 × 优先级优先 × 距离次之 ───
    Track* selected = nullptr;
    int  best_pri  = 999;
    double best_dist = static_cast<double>(in.target_aim_range) + 1.0;
    float sel_y_top = 0.0f, sel_y_bot = 1.0f;

    for (auto& t : tracks_)
    {
        if (!t.alive) continue;

        int   pri = -1;
        float y_top = 0.0f, y_bot = 1.0f;
        float min_conf = 0.0f;
        for (int si = 0; si < 3; ++si)
        {
            if (in.target_slots[si].class_id == t.class_id)
            {
                pri      = si;
                y_top    = in.target_slots[si].y_top;
                y_bot    = in.target_slots[si].y_bot;
                min_conf = in.target_slots[si].min_conf;
                break;
            }
        }
        if (pri < 0) continue;
        if (min_conf > 0.0f && t.confidence < min_conf) continue;

        const float bx  = t.bbox.x + t.bbox.width * 0.5f;
        const float ya  = t.bbox.y + t.bbox.height * y_top;
        const float yb  = t.bbox.y + t.bbox.height * y_bot;
        float aim_y     = (ya + yb) * 0.5f;
        const float cy  = static_cast<float>(in.crosshair_y);
        if (cy >= ya && cy <= yb) aim_y = cy;

        const double dx = static_cast<double>(bx) - in.crosshair_x;
        const double dy = static_cast<double>(aim_y) - in.crosshair_y;
        const double dist = std::hypot(dx, dy);

        if (dist > static_cast<double>(in.target_aim_range)) continue;

        if (pri < best_pri || (pri == best_pri && dist < best_dist))
        {
            best_pri  = pri;
            best_dist = dist;
            selected  = &t;
            sel_y_top = y_top;
            sel_y_bot = y_bot;
        }
    }

    if (!selected)
    {
        current_id_ = -1;
        prev_current_id_ = -1;
        return out;
    }

    // Recompute anchor with Y-zone clamping for the chosen target.
    {
        const float bx  = selected->bbox.x + selected->bbox.width * 0.5f;
        const float ya  = selected->bbox.y + selected->bbox.height * sel_y_top;
        const float yb  = selected->bbox.y + selected->bbox.height * sel_y_bot;
        float aim_y     = (ya + yb) * 0.5f;
        const float cy  = static_cast<float>(in.crosshair_y);
        if (cy >= ya && cy <= yb) aim_y = cy;
        selected->anchor = cv::Point2f(bx, aim_y);
    }

    current_id_ = selected->id;

    out.have_target      = true;
    out.current_track_id = selected->id;
    out.anchor           = selected->anchor;
    out.bbox             = selected->bbox;

    // Coasting frame: target is tracked but wasn't observed this tick.
    // Hold position — stale anchor would contaminate filters.
    if (selected->missed > 0)
    {
        out.dx = 0;
        out.dy = 0;
        prev_current_id_ = selected->id;
        return out;
    }

    // ─── 共享死区 ───
    if (in.deadzone_enabled && selected->bbox.width > 0.f && selected->bbox.height > 0.f)
    {
        const double dz_w = static_cast<double>(selected->bbox.width)  * in.deadzone_percent * 0.01;
        const double dz_h = static_cast<double>(selected->bbox.height) * in.deadzone_percent * 0.01;
        if (std::abs(in.crosshair_x - static_cast<double>(selected->anchor.x)) < dz_w &&
            std::abs(in.crosshair_y - static_cast<double>(selected->anchor.y)) < dz_h)
        {
            out.dx = 0;
            out.dy = 0;
            prev_current_id_ = selected->id;
            return out;
        }
    }

    const int bbox_w = static_cast<int>(std::lround(selected->bbox.width));
    const int bbox_h = static_cast<int>(std::lround(selected->bbox.height));

    // 移动控制器路由 — 微澜与疾风是两套独立的 Layer B,共享上面的 Layer A 选目标。
    // 切换 mover 时复位被切入那一套的内部状态,避免上一套的残留窜进来。
    const bool mover_changed = (in.mover_kind != last_mover_kind_);
    last_mover_kind_ = in.mover_kind;

    if (in.mover_kind == mover::Kind::Predictive)
    {
        // ── 疾风:完全独立于 ART 的 bbox 自适应「带预测 PID」。直接吃原始
        //    anchor + bbox,自己估速 / 预测领先 / 位置式 P+D。ART 这条路彻底
        //    旁路(根本不调用 art_.step),所以 ART 的任何改动都不影响疾风。 ──
        if (mover_changed)
            predictive_mover_.reset();
        predictive_mover_.configure(in.predictive_params);
        const mover::Move m = predictive_mover_.step(
            static_cast<double>(selected->anchor.x),
            static_cast<double>(selected->anchor.y),
            in.crosshair_x, in.crosshair_y,
            static_cast<double>(selected->bbox.width),
            static_cast<double>(selected->bbox.height),
            in.image_size, dt, selected->id);
        out.dx = m.dx;
        out.dy = m.dy;
        // 疾风没有 ART 诊断量 → 留中性,不覆盖面板旧值。
        out.cutoff_hz   = 0.0;
        out.consistency = 0.0;
        out.snapped     = false;
        prev_current_id_ = selected->id;
        return out;
    }

    if (in.mover_kind == mover::Kind::Classic)
    {
        // ── 天枢:经典全参 PID + 动态 KP + EMA/Kalman 预测。旁路 ART。 ──
        if (mover_changed)
            classic_mover_.reset();
        classic_mover_.configure(in.classic_params);
        const mover::Move m = classic_mover_.step(
            static_cast<double>(selected->anchor.x),
            static_cast<double>(selected->anchor.y),
            in.crosshair_x, in.crosshair_y,
            static_cast<double>(selected->bbox.width),
            static_cast<double>(selected->bbox.height),
            in.image_size, dt, selected->id);
        out.dx = m.dx;
        out.dy = m.dy;
        out.cutoff_hz   = 0.0;
        out.consistency = 0.0;
        out.snapped     = false;
        prev_current_id_ = selected->id;
        return out;
    }

    // ── 微澜:ART 路径(Linear 直驱 / Bezier-Custom 经 AimPathDriver)。 ──
    if (mover_changed)
        art_.reset();   // 从疾风切回时清 ART 残留,下一帧重新播种 fx_

    art_.configure(in.aim);

    ArtResult r = art_.step(
        static_cast<double>(selected->anchor.x),
        static_cast<double>(selected->anchor.y),
        in.crosshair_x, in.crosshair_y,
        dt, bbox_w, bbox_h, selected->id);

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
            dt, selected->id);
        out.dx = pr.move_x;
        out.dy = pr.move_y;
    }
    out.cutoff_hz = r.cutoff_hz;
    out.consistency = r.consistency;
    out.snapped = r.snapped;

    prev_current_id_ = selected->id;

    return out;
}

} // namespace boss
