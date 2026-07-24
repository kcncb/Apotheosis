#ifndef MOUSE_BOSS_AIM_H
#define MOUSE_BOSS_AIM_H

#include <memory>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ava_exact/aim_movement_pipeline_exact.hpp"
#include "ava_exact/controller_orchestration_exact.hpp"
#include "ava_exact/target_selector_top_exact.hpp"
#include "ava_exact/target_to_aimpoint_exact.hpp"
#include "movers.h"

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

    mover::PidfParams pidf_params{};
};

struct EngineOutput
{
    bool have_target = false;
    int current_track_id = -1;
    cv::Point2f anchor{};
    cv::Rect2f bbox{};
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
    AimEngine();
    ~AimEngine();

    void reset();
    EngineOutput tick(const EngineInput& in, double dt);
    int lockedTrackId() const { return current_id_; }
    const std::vector<Track>& tracks() const { return tracks_; }

private:
    bool selectorConfigChanged(const EngineInput& in) const;
    void rebuildSelector(const EngineInput& in);
    bool pipelineConfigChanged(const EngineInput& in) const;
    void rebuildPipeline(const EngineInput& in, double now_seconds);
    cvm::recovered::ControllerOrchestrationHooks orchestrationHooks(
        double now_seconds, bool* released_pending);

    std::unique_ptr<cvm::recovered::AimTargetSelectorExact> selector_;
    std::unique_ptr<cvm::recovered::AimMovementPipelineExact> pipeline_;
    cvm::recovered::AimPointState64Abi aimpoint_state_{};
    cvm::recovered::ControllerOrchestrationStateExact orchestration_{};
    std::vector<TargetSlot> selector_slots_;
    int selector_lost_frames_ = -1;
    int selector_normalizer_ = -1;

    mover::PidfParams pipeline_pidf_{};
    bool pipeline_config_valid_ = false;

    int current_id_ = -1;
    int last_generation_ = -1;
    std::vector<Track> tracks_;
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
