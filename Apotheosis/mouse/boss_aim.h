#ifndef MOUSE_BOSS_AIM_H
#define MOUSE_BOSS_AIM_H

#include <memory>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ava_exact/target_selector_top_exact.hpp"
#include "ava_exact/target_to_aimpoint_exact.hpp"
#include "movers.h"
#include "control/predictive_controller.h"

namespace boss
{

struct Track
{
    int id = -1;
    cv::Rect2f bbox{};
    cv::Point2f anchor{};
    int missed = 0;
    bool alive = true;
    int class_id = -1;
    float confidence = 0.0f;
    bool observed_this_frame = false;
};

struct TargetSlot
{
    int class_id = -1;
    // 用户坐标：0=框底，1=框顶。
    float y_offset_min = 0.5f;
    float y_offset_max = 0.5f;
    float min_conf = 0.0f;
};

struct EngineInput
{
    const std::vector<cv::Rect2f>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    const std::vector<float>* confidences = nullptr;
    std::vector<TargetSlot> target_slots;

    int lost_target_cache_frames = 5;

    double crosshair_x = 0.0;
    double crosshair_y = 0.0;
    double fov_radius_x = 0.0;
    double fov_radius_y = 0.0;
    double image_size = 0.0;
    bool has_new_measurement = true;
    uint64_t sequence = 0;
    int64_t captured_ns = 0, now_ns = 0;
    int64_t control_period_ns = 8'333'333, max_observation_age_ns = 120'000'000;
    motion::Calibration calibration;
    mover::PidfParams pidf_params{};
};

struct EngineOutput
{
    bool have_target = false;
    bool control_due = false;
    double pixel_dx = 0, pixel_dy = 0;
    int64_t observed_at = 0;
    motion::Phase phase = motion::Phase::acquire;
    int current_track_id = -1;
    cv::Point2f anchor{};
    cv::Rect2f bbox{};
    // 本帧的原始观测框(未随预测锚点平移)与锁定类别, 供诊断日志区分
    // "框自己在跳"和"锚点比例随类别切换"。bbox 的宽高与原始框相同。
    cv::Rect2f observed_bbox{};
    int class_id = -1;
    int dx = 0;
    int dy = 0;
    double cutoff_hz = 0.0;
    double consistency = 0.0;
    bool snapped = false;
    bool coasting = false;
    bool motion_suppressed = false;
};

class AimEngine
{
public:
    explicit AimEngine(std::shared_ptr<motion::CommandJournal> journal = nullptr);
    ~AimEngine();

    void reset();
    EngineOutput tick(const EngineInput& in, double dt);
    int lockedTrackId() const { return current_id_; }
    const std::vector<Track>& tracks() const { return tracks_; }

private:
    bool selectorConfigChanged(const EngineInput& in) const;
    void rebuildSelector(const EngineInput& in);
    std::unique_ptr<cvm::recovered::AimTargetSelectorExact> selector_;
    std::shared_ptr<motion::CommandJournal> journal_;
    motion::PredictiveController controller_;
    cvm::recovered::AimPointState64Abi aimpoint_state_{};
    int64_t last_capture_ns_ = 0;
    uint64_t fallback_sequence_ = 0, last_observed_sequence_ = 0;
    cv::Point2f observed_anchor_{};
    cv::Rect2f observed_box_{};
    float confidence_ = 0;
    int class_id_ = -1;
    std::vector<TargetSlot> selector_slots_;
    int selector_lost_frames_ = -1;
    int selector_normalizer_ = -1;


    int current_id_ = -1;
    int last_generation_ = -1;
    std::vector<Track> tracks_;
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
