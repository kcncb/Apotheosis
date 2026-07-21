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
    // 用户坐标:0=框底,1=框顶。新锁定时在 [min,max] 中抽一次并保持。
    float y_offset_min = 0.5f;
    float y_offset_max = 0.5f;
    float min_conf = 0.0f;
};

struct EngineInput
{
    const std::vector<cv::Rect>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    const std::vector<float>* confidences = nullptr;

    // 优先级 = 索引顺序 (0 最高)。空表示当前热键没有可瞄类别 → 不选目标。
    std::vector<TargetSlot> target_slots;

    bool  deadzone_enabled = false;
    float deadzone_percent = 0.0f;
    int   lost_target_cache_frames = 5;

    double crosshair_x = 0.0;
    double crosshair_y = 0.0;

    double fov_radius_x = 0.0;
    double fov_radius_y = 0.0;

    // 检测图边长(= detection_resolution)。疾风用它把 bbox 对角线归一成占比,
    // 驱动 box 大小自适应(大框压增益/小框抬增益)。微澜不使用。
    double image_size = 0.0;


    // Trajectory shaper. When mode == Linear the engine uses ART's
    // built-in proportional drive (legacy / default). Otherwise the
    // engine routes ART's filtered aim point through AimPathDriver so
    // the cursor traces the requested Bezier / freehand curve from the
    // current position to the aim point.
    AimPathDriver::Params path{};

    // 移动控制器选择 (见 movers.h)。Smooth = 走 ART/path 原路径,
    // Predictive = 引擎旁路 AimPathDriver,把 (ART filtered aim − crosshair) 喂给
    // 位置式 PID + 预测器直接出 dx/dy。
    mover::Kind             mover_kind = mover::Kind::Classic;
    mover::ClassicPidParams classic_params{};
    mover::YaoguangParams   yaoguang_params{};
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
    bool   coasting = false;          // track 在缓存期，但本帧没有真实检测
    bool   motion_suppressed = false; // 要求上层清空尚未发送的旧移动
};

class AimEngine
{
public:
    void reset();
    EngineOutput tick(const EngineInput& in, double dt);
    void applyMove(int dx, int dy);

    int  lockedTrackId() const { return current_id_; }
    const std::vector<Track>& tracks() const { return tracks_; }

private:
    static constexpr float kMatchRatio = 0.5f;
    static constexpr float kMatchMinPx = 28.0f;

    void assignDetections(const EngineInput& in);
    void purgeLost(int max_missed_frames);
    void resetMotionControllers();

    std::vector<Track> tracks_;
    int next_id_ = 1;
    int current_id_ = -1;
    int prev_current_id_ = -1;
    AimPathDriver path_;
    // 疾风 PID + 预测器,作为成员长期持有 (state 跨帧)。
    // 天枢 经典 PID + 动态 KP + 预测器。
    mover::ClassicPidMover classic_mover_{};
    mover::YaoguangMover   yaoguang_mover_{};

    // 共享死区采用进入/退出滞回，避免检测噪声在边界每帧开关。
    bool deadzone_latched_ = false;
    int  deadzone_track_id_ = -1;

    // 每次锁定只抽一次随机 Y，锁定期间保持，避免“随机”本身变成抖动。
    int   random_y_track_id_ = -1;
    float random_y_min_ = 0.5f;
    float random_y_max_ = 0.5f;
    float random_y_value_ = 0.5f;
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
