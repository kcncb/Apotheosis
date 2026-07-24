#include "flashlight_runtime.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "Apotheosis.h"
#include "config.h"
#include "runtime/config_snapshot.h"
#include "depth/depth_mask.h"
#include "flashlight_detector.h"
#include "flashlight_tracker.h"
#include "flashlight_tuning.h"
#include "runtime/active_hotkey.h"
#include "runtime/event_orchestrator.h"

// Defined here; declared in Apotheosis.h. Tells the capture thread to produce the
// normalized depth map while the 寻光 depth-gate is active (depth.mode > 0).
std::atomic<bool> g_flashlight_depth_required{false};

namespace flashlight_runtime
{

namespace
{
std::mutex                    g_mtx;
Snapshot                      g_snap{};
crosshair::FlashlightDetector g_detector;
crosshair::FlashlightTracker  g_tracker; // mouse inference consumer only
int                           g_selected_track_id = -1;

bool associated_with_box(cv::Point2f p, const std::vector<cv::Rect>& boxes,
                         float max_box_widths)
{
    for (const cv::Rect& raw_box : boxes)
    {
        if (raw_box.area() <= 0) continue;
        const float left   = static_cast<float>(raw_box.x);
        const float right  = static_cast<float>(raw_box.x + raw_box.width);
        const float top    = static_cast<float>(raw_box.y);
        const float bottom = static_cast<float>(raw_box.y + raw_box.height);
        const float dx = std::max({left - p.x, 0.0f, p.x - right});
        const float dy = std::max({top - p.y, 0.0f, p.y - bottom});
        const float allowed = std::max(2.0f, raw_box.width * max_box_widths);
        if (dx * dx + dy * dy <= allowed * allowed)
            return true;
    }
    return false;
}

void reset_and_clear()
{
    g_tracker.reset();
    g_selected_track_id = -1;
    publish(Snapshot{});
}
} // namespace

Snapshot read()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void publish(const Snapshot& snap)
{
    // 事件编排:发布完整上升/下降边沿，供“持续期间循环”可靠启停。
    bool rising = false;
    bool falling = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const bool prev_valid = g_snap.valid && !g_snap.spots.empty();
        const bool now_valid  = snap.valid   && !snap.spots.empty();
        rising = now_valid && !prev_valid;
        falling = !now_valid && prev_valid;
        g_snap = snap;
    }
    if (rising) event_orch::publish(event_orch::EventType::FlashlightHit);
    if (falling) event_orch::publish(event_orch::EventType::FlashlightLost);
}

void process_inference_frame(const cv::Mat& bgrFrame,
                             const std::vector<cv::Rect>& modelBoxes)
{
    // Default: don't request depth. Set true below once we know the gate engages.
    ::g_flashlight_depth_required.store(false);

    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        reset_and_clear();
        return;
    }

    // Active-hotkey gate (per-hotkey opt-in). Clear + reset tracks when off so
    // stale tracks never bleed across an enable/disable.
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0)
    {
        reset_and_clear();
        return;
    }

    int sensitivity = 50;
    int reject      = 50;
    int spot_size   = 50;
    {
        const auto snapshot = runtime_config::read();
        const auto& cfg = *snapshot;
        if (active_idx >= static_cast<int>(cfg.hotkeys.size()))
        {
            reset_and_clear();
            return;
        }
        const auto& hk = cfg.hotkeys[active_idx];
        if (!hk.flashlight_detect_enabled)
        {
            reset_and_clear();
            return;
        }
        sensitivity = cfg.flashlight_sensitivity;
        reject      = cfg.flashlight_reject_strength;
        spot_size   = cfg.flashlight_spot_size;
    }

    const crosshair::FlashlightTuning tuning =
        crosshair::flashlight_derive_tuning(sensitivity, reject, spot_size, bgrFrame.size());

    // Request normalized depth from the capture thread iff the depth-gate engages.
    ::g_flashlight_depth_required.store(tuning.depth.mode > 0);

    const auto raw = g_detector.detectAll(bgrFrame, tuning.det);
    if (raw.empty())
    {
        // YOLO 时钟上的一次完整 miss：立即释放，不跨帧保留旧锁点。
        g_tracker.update({}, {}, tuning.temporal);
        g_selected_track_id = -1;
        publish(Snapshot{});
        return;
    }

    // ---- Depth-gate prep (杀太阳/天空/远处泛光) ----
    cv::Mat depthN;
    bool    depth_map_ok = false;
    if (tuning.depth.mode > 0)
    {
        depthN = depth_anything::GetDepthMaskGenerator().getDepthNormalized();
        depth_map_ok = !depthN.empty() && depthN.type() == CV_8UC1 &&
                       depthN.size() == bgrFrame.size();
    }

    // ---- Temporal tracker (杀水面/玻璃闪烁反光) ----
    std::vector<cv::Point2f> centers;
    std::vector<float>       radii;
    centers.reserve(raw.size());
    radii.reserve(raw.size());
    for (const auto& s : raw)
    {
        centers.push_back(s.center);
        radii.push_back(s.radius);
    }
    const auto verdicts = g_tracker.update(centers, radii, tuning.temporal);

    // ---- Score + gate each spot ----
    const cv::Rect frame_rect(0, 0, bgrFrame.cols, bgrFrame.rows);
    std::vector<Spot> spots;
    spots.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const auto& s = raw[i];
        const crosshair::FlashlightTrackVerdict v =
            (i < verdicts.size()) ? verdicts[i] : crosshair::FlashlightTrackVerdict{};

        bool  passed_depth = true;
        float penalty      = 0.0f;
        if (tuning.depth.mode > 0)
        {
            if (depth_map_ok)
            {
                const int cx = std::clamp(static_cast<int>(std::lround(s.center.x)), 0, depthN.cols - 1);
                const int cy = std::clamp(static_cast<int>(std::lround(s.center.y)), 0, depthN.rows - 1);
                const bool is_far = depthN.at<uint8_t>(cy, cx) <= tuning.depth.far_level;
                if (is_far)
                {
                    if (tuning.depth.mode == 2)
                        passed_depth = false; // 严格模式下孤立远景光直接拒绝
                    else
                        penalty += tuning.depth.soft_penalty;
                }
            }
            else if (tuning.depth.top_band_penalty > 0.0f)
            {
                // No usable depth map → positional fallback: sun/sky sit high in
                // the frame, so penalize the top band.
                if (s.center.y < bgrFrame.rows * 0.30f)
                {
                    penalty += tuning.depth.top_band_penalty;
                    if (tuning.depth.mode == 2)
                        passed_depth = false;
                }
            }
        }

        float conf = s.confidence - penalty;
        if (v.onset)     conf += tuning.temporal.onset_bonus;
        if (v.confirmed) conf += tuning.temporal.confirmed_bonus;
        conf = std::clamp(conf, 0.0f, 1.0f);

        if (conf < tuning.accept_conf_floor)
            continue;

        // Use the tracker's EMA-smoothed centre/radius (kills 1-3 px per-frame
        // contour-moment jitter that the mover's velocity-adaptive one-euro
        // would otherwise pass straight to the aim output during camera
        // rotation — "疾风" buzz when a halo is the active target). Onset
        // frames have smoothed == raw so the first lock is instant.
        const cv::Point2f sc = v.smoothed_center;
        const float       sr = (v.smoothed_radius > 0.0f) ? v.smoothed_radius : s.radius;

        const int diameter = std::max(2, static_cast<int>(std::lround(sr * 2.0f)));
        const int bx = static_cast<int>(std::lround(sc.x)) - diameter / 2;
        const int by = static_cast<int>(std::lround(sc.y)) - diameter / 2;
        const cv::Rect clipped = cv::Rect(bx, by, diameter, diameter) & frame_rect;
        if (clipped.area() <= 0)
            continue;

        Spot out;
        out.box          = clipped;
        out.center       = sc;
        out.radius       = sr;
        out.confidence   = conf;
        out.track_id     = v.track_id;
        out.passed_depth = passed_depth;
        out.confirmed    = v.confirmed;
        out.onset        = v.onset;
        out.associated_with_model = associated_with_box(
            sc, modelBoxes, tuning.coloc.max_box_widths);
        out.independent_aimable = !out.associated_with_model && passed_depth && v.confirmed;

        // 有人物框时第一帧即可认定“这是人物携带的手电”，但不注入光斑；
        // 无人物框必须通过深度和连续三次确认才成为独立目标。
        if (!out.associated_with_model && !out.independent_aimable)
            continue;
        spots.push_back(out);
    }

    if (spots.empty())
    {
        g_selected_track_id = -1;
        publish(Snapshot{});
        return;
    }

    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) {
        if (a.associated_with_model != b.associated_with_model)
            return a.associated_with_model; // 有模型框时最终应由模型框瞄准
        return a.confidence > b.confidence;
    });

    auto selected = spots.end();
    if (g_selected_track_id >= 0)
    {
        selected = std::find_if(spots.begin(), spots.end(), [](const Spot& s) {
            return s.track_id == g_selected_track_id;
        });
        if (selected == spots.end())
        {
            // 原目标本轮消失：先明确丢失，不在同一帧无缝跳到另一盏灯。
            g_selected_track_id = -1;
            publish(Snapshot{});
            return;
        }
    }
    else
    {
        selected = spots.begin();
        g_selected_track_id = selected->track_id;
    }

    Snapshot snap;
    snap.valid = true;
    snap.spots.push_back(*selected); // 对外永远只有稳定选中的一个结果
    snap.ts    = std::chrono::steady_clock::now();
    publish(snap);
}

} // namespace flashlight_runtime
