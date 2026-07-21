#include "boss_aim.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace boss
{

void AimEngine::reset()
{
    tracks_.clear();
    next_id_ = 1;
    current_id_ = -1;
    prev_current_id_ = -1;
    path_.reset();
    classic_mover_.reset();
    yaoguang_mover_.reset();
    deadzone_latched_ = false;
    deadzone_track_id_ = -1;
    random_y_track_id_ = -1;
    random_y_min_ = random_y_max_ = random_y_value_ = 0.5f;
}

void AimEngine::resetMotionControllers()
{
    path_.reset();
    classic_mover_.reset();
    yaoguang_mover_.reset();
}

void AimEngine::applyMove(int dx, int dy)
{
    path_.applyMove(dx, dy);
    classic_mover_.applyMove(dx, dy);
    yaoguang_mover_.applyMove(dx, dy);
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
            for (const auto& s : in.target_slots)
                if (s.class_id == class_id) { slot_match = true; break; }
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
            if (t.class_id != class_id)
                continue;
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

void AimEngine::purgeLost(int max_missed_frames)
{
    const int keep = std::clamp(max_missed_frames, 0, 240);
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
            [keep](const Track& t) { return !t.alive || t.missed > keep; }),
        tracks_.end());
}

EngineOutput AimEngine::tick(const EngineInput& in, double dt)
{
    EngineOutput out;

    if (!(dt > 1e-6))
        dt = 1.0 / 120.0;
    if (dt > 0.25)
    {
        // 长停顿后历史已经不连续。把本帧作为首帧播种，避免人为截短 dt
        // 后把一大段位移除以 250ms 制造导数尖峰。
        resetMotionControllers();
        dt = 0.0;
    }

    // ─── Layer A: 目标管理 ───
    assignDetections(in);
    purgeLost(in.lost_target_cache_frames);

    // ─── 目标选择: 优先级优先 × 距离次之 (距离上限已由 FOV 椭圆前置裁掉) ───
    Track* selected = nullptr;
    int  best_pri  = 999;
    double best_dist = std::numeric_limits<double>::infinity();
    bool best_is_current = false;
    float sel_y_min = 0.5f, sel_y_max = 0.5f;

    for (auto& t : tracks_)
    {
        if (!t.alive) continue;

        int   pri = -1;
        float y_min = 0.5f, y_max = 0.5f;
        float min_conf = 0.0f;
        for (size_t si = 0; si < in.target_slots.size(); ++si)
        {
            if (in.target_slots[si].class_id == t.class_id)
            {
                pri      = static_cast<int>(si);
                y_min    = in.target_slots[si].y_offset_min;
                y_max    = in.target_slots[si].y_offset_max;
                min_conf = in.target_slots[si].min_conf;
                break;
            }
        }
        if (pri < 0) continue;
        if (min_conf > 0.0f && t.confidence < min_conf) continue;

        y_min = std::clamp(y_min, 0.0f, 1.0f);
        y_max = std::clamp(y_max, 0.0f, 1.0f);
        if (y_min > y_max) std::swap(y_min, y_max);

        const float bx = t.bbox.x + t.bbox.width * 0.5f;
        // 距离比较用随机区间中点；真正锁定后才抽样，避免候选评估消耗随机数。
        const float y_mid = (y_min + y_max) * 0.5f;
        const float aim_y = t.bbox.y + t.bbox.height * (1.0f - y_mid);

        const double dx = static_cast<double>(bx) - in.crosshair_x;
        const double dy = static_cast<double>(aim_y) - in.crosshair_y;
        const double dist = std::hypot(dx, dy);

        const bool is_current = (t.id == current_id_);
        if (pri < best_pri ||
            (pri == best_pri && is_current && !best_is_current) ||
            (pri == best_pri && is_current == best_is_current && dist < best_dist))
        {
            best_pri  = pri;
            best_dist = dist;
            best_is_current = is_current;
            selected  = &t;
            sel_y_min = y_min;
            sel_y_max = y_max;
        }
    }

    if (!selected)
    {
        if (current_id_ != -1)
            resetMotionControllers();
        current_id_ = -1;
        prev_current_id_ = -1;
        deadzone_latched_ = false;
        deadzone_track_id_ = -1;
        random_y_track_id_ = -1;
        return out;
    }

    // 新锁定/切换目标时在用户范围内只抽一次，锁定期间保持同一比例。
    // 范围参数被实时修改时也重抽一次，让 UI 调整立即生效。
    if (random_y_track_id_ != selected->id ||
        std::fabs(random_y_min_ - sel_y_min) > 1e-6f ||
        std::fabs(random_y_max_ - sel_y_max) > 1e-6f)
    {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> dist(sel_y_min, sel_y_max);
        random_y_track_id_ = selected->id;
        random_y_min_ = sel_y_min;
        random_y_max_ = sel_y_max;
        random_y_value_ = (sel_y_max - sel_y_min > 1e-6f)
            ? dist(rng) : sel_y_min;
    }

    const float bx = selected->bbox.x + selected->bbox.width * 0.5f;
    const float aim_y = selected->bbox.y
                      + selected->bbox.height * (1.0f - random_y_value_);
    selected->anchor = cv::Point2f(bx, aim_y);

    const bool target_changed = (current_id_ != selected->id);
    current_id_ = selected->id;
    if (target_changed)
    {
        deadzone_latched_ = false;
        deadzone_track_id_ = selected->id;
        out.motion_suppressed = true; // 上层先清掉上一目标尚未发送的队列
    }

    out.have_target      = true;
    out.current_track_id = selected->id;
    out.anchor           = selected->anchor;
    out.bbox             = selected->bbox;

    // Coasting frame: target is tracked but wasn't observed this tick.
    // Hold position — stale anchor would contaminate filters.
    if (selected->missed > 0)
    {
        // 缓存只保留 track 身份，不使用旧坐标继续控制/开火。每个 coasting
        // 帧都重置历史，重捕帧自然以零导数重新播种。
        resetMotionControllers();
        deadzone_latched_ = false;
        out.dx = 0;
        out.dy = 0;
        out.coasting = true;
        out.motion_suppressed = true;
        prev_current_id_ = selected->id;
        return out;
    }

    // ─── 共享死区 ───
    if (in.deadzone_enabled && selected->bbox.width > 0.f && selected->bbox.height > 0.f)
    {
        // UI 的 100% 表示完整 bbox，故这里用半宽/半高。退出阈值比进入
        // 阈值扩大 25%，避免噪声在边缘反复开关。
        const double enter_w = static_cast<double>(selected->bbox.width)
                             * in.deadzone_percent * 0.005;
        const double enter_h = static_cast<double>(selected->bbox.height)
                             * in.deadzone_percent * 0.005;
        const double scale = deadzone_latched_ ? 1.25 : 1.0;
        const bool inside =
            std::abs(in.crosshair_x - static_cast<double>(selected->anchor.x)) <= enter_w * scale &&
            std::abs(in.crosshair_y - static_cast<double>(selected->anchor.y)) <= enter_h * scale;
        if (inside && enter_w > 0.0 && enter_h > 0.0)
        {
            if (!deadzone_latched_)
                resetMotionControllers();
            deadzone_latched_ = true;
            deadzone_track_id_ = selected->id;
            out.dx = 0;
            out.dy = 0;
            out.motion_suppressed = true;
            prev_current_id_ = selected->id;
            return out;
        }
        if (deadzone_latched_)
        {
            deadzone_latched_ = false;
            resetMotionControllers();
        }
    }
    else if (deadzone_latched_)
    {
        deadzone_latched_ = false;
        resetMotionControllers();
    }

    const int bbox_w = static_cast<int>(std::lround(selected->bbox.width));
    const int bbox_h = static_cast<int>(std::lround(selected->bbox.height));

    // 每帧都 configure，确保 Bezier → Linear → Bezier 这类热切换会复位
    // 虚拟曲线游标，而不是复用上一次轨迹的 start/cursor。
    path_.configure(in.path);

    {
        mover::Move m;
        if (in.mover_kind == mover::Kind::Yaoguang)
        {
            yaoguang_mover_.configure(in.yaoguang_params);
            m = yaoguang_mover_.step(
                static_cast<double>(selected->anchor.x),
                static_cast<double>(selected->anchor.y),
                in.crosshair_x, in.crosshair_y,
                static_cast<double>(selected->bbox.width),
                static_cast<double>(selected->bbox.height),
                in.image_size, dt, selected->id);
        }
        else
        {
            classic_mover_.configure(in.classic_params);
            m = classic_mover_.step(
                static_cast<double>(selected->anchor.x),
                static_cast<double>(selected->anchor.y),
                in.crosshair_x, in.crosshair_y,
                static_cast<double>(selected->bbox.width),
                static_cast<double>(selected->bbox.height),
                in.image_size, dt, selected->id);
        }

        if (in.path.mode == AimPathDriver::Mode::Linear)
        {
            out.dx = m.dx;
            out.dy = m.dy;
        }
        else
        {
            // 非直线曲线:天枢的预测后 aim 作为 goal,曲线负责轨迹形状。
            path_.configure(in.path);
            AimPathDriver::Result pr = path_.step(
                m.aim_x, m.aim_y,
                in.crosshair_x, in.crosshair_y,
                dt, selected->id, m.dx, m.dy,
                in.mover_kind == mover::Kind::Yaoguang
                    ? std::max(4.0, std::hypot(static_cast<double>(bbox_w),
                                               static_cast<double>(bbox_h)) * 0.12)
                    : 0.0);
            out.dx = pr.move_x;
            out.dy = pr.move_y;
        }
        // 曲线只负责轨迹方向，不得放大控制器已经给出的安全步长。
        const double base_mag = std::hypot(static_cast<double>(m.dx), static_cast<double>(m.dy));
        const double shaped_mag = std::hypot(static_cast<double>(out.dx), static_cast<double>(out.dy));
        if (shaped_mag > base_mag && shaped_mag > 0.0)
        {
            const double scale = base_mag / shaped_mag;
            out.dx = static_cast<int>(std::trunc(static_cast<double>(out.dx) * scale));
            out.dy = static_cast<int>(std::trunc(static_cast<double>(out.dy) * scale));
        }
        out.cutoff_hz   = 0.0;
        out.consistency = 0.0;
        out.snapped     = false;
        prev_current_id_ = selected->id;
        return out;
    }

}

} // namespace boss
