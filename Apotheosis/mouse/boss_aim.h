#ifndef MOUSE_BOSS_AIM_H
#define MOUSE_BOSS_AIM_H

// =============================================================================
// Boss AI aiming engine — Layer A (target management) + ART aim tracker.
//
//   Layer A  目标管理   — 贪心就近关联帧 / 滞回选目标 / 丢检容忍
//   Layer B  ART        — Adaptive Reactive Tracker (modified 1€ Filter)
//                         dual-rate velocity, bbox-adaptive cutoff,
//                         direction-consistency prediction, sub-pixel residual
//   开火                — |aim - anchor| < FIRE_RADIUS_RATIO × min(w,h) 时 fire
//
// 暴露给用户: speed_x, speed_y, dead_zone_px
// =============================================================================

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <opencv2/opencv.hpp>
#include "adaptive_reactive_tracker.h"
#include "aim_path.h"
#include "movers.h"

namespace boss
{

struct Track
{
    int id = -1;
    cv::Rect2f bbox{};
    cv::Point2f anchor{ 0.f, 0.f };
    int missed = 0;
    bool alive = true;
    int class_id = -1;
    float confidence = 0.f;
    bool observed_this_frame = false;
};

struct EngineInput
{
    const std::vector<cv::Rect>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    const std::vector<float>* confidences = nullptr;

    const std::unordered_set<int>* eligible_classes = nullptr;

    const std::unordered_map<int, float>* y_offsets = nullptr;

    // class_id -> priority rank (smaller = higher priority, 0 = top).
    // Built from HotkeyProfile::aim_classes ordering. When non-null, the
    // engine locks the highest-priority class that has any alive track,
    // and only applies distance/hysteresis WITHIN that rank.  This is what
    // makes a head/body split model actually obey "head first": without it
    // body bboxes always win on raw distance because they're bigger.
    const std::unordered_map<int, int>* class_priority = nullptr;

    double crosshair_x = 0.0;
    double crosshair_y = 0.0;

    double fov_radius_x = 0.0;
    double fov_radius_y = 0.0;

    // 检测图边长(= detection_resolution)。疾风用它把 bbox 对角线归一成占比,
    // 驱动 box 大小自适应(大框压增益/小框抬增益)。微澜不使用。
    double image_size = 0.0;

    ArtSettings aim{};

    // Trajectory shaper. When mode == Linear the engine uses ART's
    // built-in proportional drive (legacy / default). Otherwise the
    // engine routes ART's filtered aim point through AimPathDriver so
    // the cursor traces the requested Bezier / freehand curve from the
    // current position to the aim point.
    AimPathDriver::Params path{};

    // 移动控制器选择 (见 movers.h)。Smooth = 走 ART/path 原路径,
    // Predictive = 引擎旁路 AimPathDriver,把 (ART filtered aim − crosshair) 喂给
    // 位置式 PID + 预测器直接出 dx/dy。
    mover::Kind             mover_kind = mover::Kind::Smooth;
    mover::PredictiveParams predictive_params{};
};

struct EngineOutput
{
    bool have_target = false;
    int  current_track_id = -1;

    cv::Point2f anchor{ 0.f, 0.f };
    cv::Rect2f  bbox{};

    int dx = 0;
    int dy = 0;

    // ART diagnostics
    double cutoff_hz = 0;
    double consistency = 0;
    bool   snapped = false;
};

class AimEngine
{
public:
    void reset();
    EngineOutput tick(const EngineInput& in, double dt);

    int  lockedTrackId() const { return current_id_; }
    const std::vector<Track>& tracks() const { return tracks_; }

private:
    static constexpr int   kMDrop          = 5;
    // 同 rank 内切换迟滞:对手必须比当前锁近 (1 - kAlphaHyst) 才能抢锁。
    // 0.4 = 必须近 60%(原 0.7 时只需近 30%,bbox 一抖就乒乓)。
    static constexpr float kAlphaHyst      = 0.4f;
    // 同 rank 切换冷却:锁上不到 kMinFramesLocked 帧时,任何距离对手都不夺锁。
    // 阻断单帧检测抖动 / 重叠人头瞬时排序翻转造成的乱切。跨 rank(类别优先级
    // 升级,例:body→head)不受此限制,头部一出现仍立即吸附。
    static constexpr int   kMinFramesLocked = 4;
    static constexpr float kMatchRatio     = 0.5f;
    // Absolute-pixel floor for the track-association radius. Without this
    // a far-distance 20 px bbox uses match_thresh = 10 px, but real
    // detection-center jitter on tiny boxes routinely exceeds 10 px →
    // the same enemy gets a fresh track ID every frame, which then trips
    // the engine's "old lock died, contender out of nowhere" path with
    // no hysteresis. Floor keeps the same enemy on the same track at
    // long range.
    static constexpr float kMatchMinPx     = 28.0f;
    // Confidence-weighted distance: effective_d = d / max(conf, kMinConfWeight).
    // Higher-confidence detections look "closer" to the engine, so a
    // flickering low-confidence detection at the edge of the model's range
    // can't out-bid a solidly-held lock just by being a few px closer.
    // Floor stops a zero-conf detection from looking infinitely close.
    static constexpr float kMinConfWeight  = 0.15f;
    static constexpr float kDefaultHeadFrac = 0.15f;

    cv::Point2f compute_anchor(const cv::Rect& bbox, int class_id,
                               const std::unordered_map<int, float>* y_offsets) const;

    void assignDetections(const EngineInput& in);
    void purgeLost();

    std::vector<Track> tracks_;
    int next_id_ = 1;
    int current_id_ = -1;
    int prev_current_id_ = -1;
    // 当前锁定 track 已经连续锁了多少帧 (0 = 这帧才锁上)。同 rank 内切换
    // 至少要等 kMinFramesLocked 帧才允许触发。reset()/换锁 都清零。
    int lock_age_frames_ = 0;
    AdaptiveReactiveTracker art_;
    AimPathDriver path_;
    // 疾风 PID + 预测器,作为成员长期持有 (state 跨帧)。
    mover::PredictiveMover predictive_mover_{};
    mover::Kind            last_mover_kind_ = mover::Kind::Smooth;
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
