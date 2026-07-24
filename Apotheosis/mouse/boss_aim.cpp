#include "boss_aim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace boss
{
namespace
{

bool sameSlot(const TargetSlot& a, const TargetSlot& b) noexcept
{
    return a.class_id == b.class_id
        && a.y_offset_min == b.y_offset_min
        && a.y_offset_max == b.y_offset_max
        && a.min_conf == b.min_conf;
}

bool samePidf(const mover::PidfParams& a, const mover::PidfParams& b) noexcept
{
    return a.kp_x == b.kp_x && a.kp_y == b.kp_y
        && a.ki_x == b.ki_x && a.ki_y == b.ki_y
        && a.kd_x == b.kd_x && a.kd_y == b.kd_y
        && a.kf_x == b.kf_x && a.kf_y == b.kf_y
        && a.lr_x == b.lr_x && a.lr_y == b.lr_y
        && a.deadzone_x == b.deadzone_x
        && a.deadzone_y == b.deadzone_y
        && a.movement_limit_x == b.movement_limit_x
        && a.movement_limit_y == b.movement_limit_y;
}

ava::hotkey::AimKeyRuntimeView alwaysActiveRuntime()
{
    ava::hotkey::AimKeyRuntimeView runtime;
    // 使用 AVA 键名表中的规范名称；"Mouse1" 不在恢复的 123 项表中，
    // 会令管线每帧停在 no_active_profile。
    runtime.configured_aim_keys = {"LeftMouseButton"};
    runtime.key_selection_mode = 1;
    runtime.profile_enabled = [](std::string_view) { return true; };
    runtime.block_new_press = [](std::string_view) { return false; };
    return runtime;
}

ava::hotkey::InputEnvironment alwaysActiveInput()
{
    ava::hotkey::InputEnvironment input;
    input.get_async_key_state = [](int) -> std::uint16_t { return 0x8000u; };
    return input;
}

std::int64_t steadyNowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

AimEngine::AimEngine() = default;
AimEngine::~AimEngine() = default;

void AimEngine::reset()
{
    selector_.reset();
    pipeline_.reset();
    aimpoint_state_ = {};
    orchestration_ = {};
    orchestration_.primary_target = cvm::recovered::empty_aim_target_record();
    orchestration_.secondary_target = cvm::recovered::empty_aim_target_record();
    selector_slots_.clear();
    selector_lost_frames_ = -1;
    selector_normalizer_ = -1;
    pipeline_config_valid_ = false;
    current_id_ = -1;
    last_generation_ = -1;
    tracks_.clear();
}

bool AimEngine::selectorConfigChanged(const EngineInput& in) const
{
    if (!selector_ || selector_slots_.size() != in.target_slots.size()
        || selector_lost_frames_ != in.lost_target_cache_frames
        || selector_normalizer_ != static_cast<int>(std::lround(in.image_size)))
        return true;
    for (std::size_t i = 0; i < selector_slots_.size(); ++i)
        if (!sameSlot(selector_slots_[i], in.target_slots[i]))
            return true;
    return false;
}

void AimEngine::rebuildSelector(const EngineInput& in)
{
    cvm::recovered::TargetSelectorConfigInput config;
    config.class_priority_enabled = true;
    config.search_radius = 0.5f;
    config.acquire_center_weight = 0.7f;
    config.max_lost_frames = std::clamp(in.lost_target_cache_frames, 0, 240);
    config.normalizer_x = std::max(1, static_cast<int>(std::lround(in.image_size)));
    config.normalizer_y = config.normalizer_x;

    const float maximum_priority = static_cast<float>(in.target_slots.size());
    for (std::size_t i = 0; i < in.target_slots.size(); ++i)
    {
        const auto& slot = in.target_slots[i];
        if (slot.class_id < 0)
            continue;
        config.target_labels.push_back(slot.class_id);
        cvm::recovered::TargetClassPriorityInput rule;
        rule.class_id = slot.class_id;
        rule.priority = maximum_priority - static_cast<float>(i);
        config.class_priority.push_back(std::move(rule));
    }

    selector_ = std::make_unique<cvm::recovered::AimTargetSelectorExact>(
        cvm::recovered::normalize_target_selector_config_exact(config));
    selector_slots_ = in.target_slots;
    selector_lost_frames_ = in.lost_target_cache_frames;
    selector_normalizer_ = static_cast<int>(std::lround(in.image_size));
    aimpoint_state_ = {};
    orchestration_ = {};
    orchestration_.primary_target = cvm::recovered::empty_aim_target_record();
    orchestration_.secondary_target = cvm::recovered::empty_aim_target_record();
    current_id_ = -1;
    last_generation_ = -1;
}

bool AimEngine::pipelineConfigChanged(const EngineInput& in) const
{
    return !pipeline_ || !pipeline_config_valid_
        || !samePidf(pipeline_pidf_, in.pidf_params);
}

void AimEngine::rebuildPipeline(const EngineInput& in, double now_seconds)
{
    cvm::recovered::AimMovementPipelineConfig config;
    config.pidf_mode = cvm::recovered::NativePidfMode::mode1;
    config.pidf.kp_x = in.pidf_params.kp_x;
    config.pidf.kp_y = in.pidf_params.kp_y;
    // AVA 的配置装载和界面写回都把 Ki 固定为 0；四个可见参数依次是
    // Kp（瞄准速度）、Kd（过冲控制）、Kf（锁定强度）、LR（预测速度）。
    config.pidf.ki_x = 0.0;
    config.pidf.ki_y = 0.0;
    config.pidf.kd_x = in.pidf_params.kd_x;
    config.pidf.kd_y = in.pidf_params.kd_y;
    config.pidf.kf_x = in.pidf_params.kf_x;
    config.pidf.kf_y = in.pidf_params.kf_y;
    config.pidf.lr_x = in.pidf_params.lr_x;
    config.pidf.lr_y = in.pidf_params.lr_y;
    // 死区使用 AVA PIDF 原生后处理：瞄点（含稳定的随机 Y）先完整
    // 生成，PIDF 仅在最终误差进入死区且当帧移动不超过 1 px 时停下。
    config.pidf.deadzone_x = in.pidf_params.deadzone_x;
    config.pidf.deadzone_y = in.pidf_params.deadzone_y;
    config.pidf.movement_limit_x = in.pidf_params.movement_limit_x;
    config.pidf.movement_limit_y = in.pidf_params.movement_limit_y;

    // AimPath 在运行线程中作为 PIDF 后置整形；Process/QX 保持关闭。
    config.process.enabled = 0;
    config.qx.qx_curve_enabled = 0;
    config.movement.movement_mode = 0;
    config.movement.executor_present = true;
    config.movement.executor_ready = true;
    config.movement.executor = [](std::span<const cvm::recovered::RelativeMove>) {};

    pipeline_ = std::make_unique<cvm::recovered::AimMovementPipelineExact>(
        std::move(config), 5489u, 0x6d2b79f5u, now_seconds);
    pipeline_pidf_ = in.pidf_params;
    pipeline_config_valid_ = true;
    orchestration_ = {};
    orchestration_.primary_target = cvm::recovered::empty_aim_target_record();
    orchestration_.secondary_target = cvm::recovered::empty_aim_target_record();
}

cvm::recovered::ControllerOrchestrationHooks AimEngine::orchestrationHooks(
    double now_seconds, bool* released_pending)
{
    cvm::recovered::ControllerOrchestrationHooks hooks;
    hooks.executor_present = true;
    hooks.executor_ready = true;
    hooks.release_primary_output = [released_pending] {
        if (released_pending)
            *released_pending = true;
    };
    hooks.release_secondary_output = hooks.release_primary_output;
    hooks.reset_selected_pidf = [this, now_seconds] {
        if (pipeline_)
            pipeline_->reset_pidf_runtime(now_seconds);
    };
    hooks.reset_qx_runtime = [this] {
        if (pipeline_)
            pipeline_->reset_qx_runtime();
    };
    return hooks;
}

EngineOutput AimEngine::tick(const EngineInput& in, double dt)
{
    EngineOutput out;
    const std::int64_t now_ns = steadyNowNs();
    const double now_seconds = static_cast<double>(now_ns) * 1.0e-9;
    dt = std::clamp(dt > 0.0 ? dt : 1.0 / 60.0, 0.0, 0.1);

    if (selectorConfigChanged(in))
        rebuildSelector(in);
    if (pipelineConfigChanged(in))
        rebuildPipeline(in, now_seconds);

    std::vector<cvm::recovered::Detection72Abi> detections;
    if (in.boxes && in.classes)
    {
        const std::size_t count = std::min(in.boxes->size(), in.classes->size());
        detections.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const cv::Rect2f& box = (*in.boxes)[i];
            const int class_id = (*in.classes)[i];
            const float confidence = in.confidences && i < in.confidences->size()
                ? (*in.confidences)[i] : 0.0f;
            const auto slot = std::find_if(in.target_slots.begin(), in.target_slots.end(),
                [class_id](const TargetSlot& value) { return value.class_id == class_id; });
            if (slot == in.target_slots.end() || confidence < slot->min_conf
                || box.width <= 0 || box.height <= 0)
                continue;
            cvm::recovered::Detection72Abi detection;
            detection.left = static_cast<float>(box.x);
            detection.top = static_cast<float>(box.y);
            detection.width = static_cast<float>(box.width);
            detection.height = static_cast<float>(box.height);
            detection.class_id = class_id;
            detection.confidence = confidence;
            detections.push_back(detection);
        }
    }

    float radius = 0.0f;
    if (in.fov_radius_x > 0.0 && in.fov_radius_y > 0.0)
        radius = static_cast<float>(std::min(in.fov_radius_x, in.fov_radius_y));
    const std::array<float, 2> origin{
        static_cast<float>(in.crosshair_x), static_cast<float>(in.crosshair_y)};
    const auto* selected = selector_->select_aim_target(
        detections, origin, {0.0f, 0.0f}, radius, 1);

    tracks_.clear();
    if (!selected)
    {
        bool released_pending = false;
        cvm::recovered::handle_aim_target_loss_exact(
            orchestration_, orchestrationHooks(now_seconds, &released_pending));
        out.motion_suppressed = current_id_ != -1 || released_pending;
        current_id_ = -1;
        last_generation_ = -1;
        return out;
    }

    const int generation = selector_->target_generation();
    const bool generation_changed = generation != last_generation_;
    last_generation_ = generation;
    current_id_ = generation;

    const auto aim_target = cvm::recovered::make_aimpoint_target_record_exact(*selected);
    if (generation_changed)
    {
        cvm::recovered::NativeAimTargetRecord72Abi native_target;
        static_assert(sizeof(native_target.bytes) == sizeof(aim_target));
        std::memcpy(native_target.bytes.data(), &aim_target, sizeof(aim_target));
        bool released_pending = false;
        cvm::recovered::install_new_aim_target_record_exact(
            orchestration_, native_target,
            orchestrationHooks(now_seconds, &released_pending));
        out.motion_suppressed = true;
    }
    std::vector<cvm::recovered::ClassAimPointRuleExact> aim_rules;
    aim_rules.reserve(in.target_slots.size());
    for (const auto& slot : in.target_slots)
    {
        cvm::recovered::ClassAimPointRuleExact rule;
        rule.class_id = slot.class_id;
        rule.ratio_low = 1.0f - std::clamp(slot.y_offset_max, 0.0f, 1.0f);
        rule.ratio_high = 1.0f - std::clamp(slot.y_offset_min, 0.0f, 1.0f);
        aim_rules.push_back(rule);
    }
    cvm::recovered::AimPointConfigExact aim_config;
    aim_config.reference_x = static_cast<float>(in.crosshair_x);
    aim_config.reference_y = static_cast<float>(in.crosshair_y);
    aim_config.default_ratio_low = 0.5f;
    aim_config.default_ratio_high = 0.5f;
    aim_config.class_rules_enabled = true;
    aim_config.class_rules = aim_rules;
    (void)cvm::recovered::target_to_aimpoint_exact(
        aim_target, aimpoint_state_, aim_config);

    cvm::recovered::AimMovementFrameInput frame;
    frame.now_ns = now_ns;
    frame.now_seconds = now_seconds;
    frame.qx_elapsed_ms = static_cast<float>(dt * 1000.0);
    frame.current_x = static_cast<float>(in.crosshair_x);
    frame.current_y = static_cast<float>(in.crosshair_y);
    frame.qx_stage_selected = false;
    frame.target.valid = aimpoint_state_.output_valid != 0;
    // 与 CompleteMovementControllerExact 的 72-byte packing 边界保持一致。
    frame.target.qx_input_blocked = aim_target.target_flag != 0;
    frame.target.target_identity = generation;
    frame.target.fallback_identity = selected->class_id;
    frame.target.tracking_identity = generation;
    frame.target.predicted = aim_target.predicted_aim_valid != 0;
    frame.target.base_x = aimpoint_state_.aim_x;
    frame.target.base_y = aimpoint_state_.aim_y;
    frame.target.width = aimpoint_state_.box_width;
    frame.target.height = aimpoint_state_.box_height;

    const auto trace = pipeline_->step(
        alwaysActiveRuntime(), alwaysActiveInput(), frame);
    orchestration_.target_active = true;

    out.have_target = true;
    out.current_track_id = generation;
    out.anchor = {aimpoint_state_.aim_x, aimpoint_state_.aim_y};
    out.bbox = {selected->left, selected->top,
                selected->right - selected->left,
                selected->bottom - selected->top};
    out.coasting = selected->target_flag != 0;
    out.motion_suppressed = out.motion_suppressed
        || trace.stop_reason == cvm::recovered::AimMovementStopReason::region_policy_suppressed;
    if (trace.movement_dispatched)
    {
        out.dx = trace.movement.accepted_total.dx;
        out.dy = trace.movement.accepted_total.dy;
    }
    Track debug;
    debug.id = generation;
    debug.bbox = out.bbox;
    debug.anchor = out.anchor;
    debug.missed = selected->target_kind_or_age;
    debug.alive = true;
    debug.class_id = selected->class_id;
    debug.confidence = selected->confidence;
    debug.observed_this_frame = !out.coasting;
    tracks_.push_back(debug);
    return out;
}

} // namespace boss
