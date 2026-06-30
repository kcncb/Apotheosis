#ifndef MOUSE_BOSS_AIM_H
#define MOUSE_BOSS_AIM_H

// =============================================================================
// Boss AI aiming engine — Layer A (target management) + Layer B (movers).
//
//   Layer A  目标管理   — 贪心就近关联帧 / 3 槽位优先级选目标 / 丢检容忍 / 共享死区
//   Layer B  移动控制   — 0 微澜 (ART) / 1 疾风 (PID) / 2 天枢 (Classic PID)
// =============================================================================

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

struct TargetSlot
{
    int   class_id = -1;
    float y_top = 0.0f;
    float y_bot = 1.0f;
    float min_conf = 0.0f;
};

struct EngineInput
{
    const std::vector<cv::Rect>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    const std::vector<float>* confidences = nullptr;

    TargetSlot target_slots[3];
    int target_aim_range = 150;

    bool  deadzone_enabled = false;
    float deadzone_percent = 0.0f;

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
    mover::ClassicPidParams classic_params{};
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
    static constexpr int   kMDrop      = 5;
    static constexpr float kMatchRatio = 0.5f;
    static constexpr float kMatchMinPx = 28.0f;

    void assignDetections(const EngineInput& in);
    void purgeLost();

    std::vector<Track> tracks_;
    int next_id_ = 1;
    int current_id_ = -1;
    int prev_current_id_ = -1;
    AdaptiveReactiveTracker art_;
    AimPathDriver path_;
    // 疾风 PID + 预测器,作为成员长期持有 (state 跨帧)。
    mover::PredictiveMover predictive_mover_{};
    // 天枢 经典 PID + 动态 KP + 预测器。
    mover::ClassicPidMover classic_mover_{};
    mover::Kind            last_mover_kind_ = mover::Kind::Smooth;
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
