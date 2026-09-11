#include "boss_aim.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace boss
{
namespace
{
bool sameSlot(const TargetSlot& a, const TargetSlot& b) noexcept
{
    return a.class_id == b.class_id && a.y_offset_min == b.y_offset_min
        && a.y_offset_max == b.y_offset_max && a.min_conf == b.min_conf;
}
int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
AimEngine::AimEngine(std::shared_ptr<motion::CommandJournal> journal)
    : journal_(journal ? std::move(journal) : std::make_shared<motion::CommandJournal>()), controller_(journal_) {}
AimEngine::~AimEngine() = default;
void AimEngine::reset()
{
    selector_.reset(); controller_.reset(); aimpoint_state_ = {};
    selector_slots_.clear(); selector_lost_frames_ = selector_normalizer_ = -1;
    current_id_ = last_generation_ = -1; tracks_.clear(); last_capture_ns_ = 0;
    observed_anchor_ = {}; observed_box_ = {}; last_observed_sequence_ = 0;
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
    // 滑行(coasting)窗口下限 1 帧。
    //
    // AVA 的 tracker 只要 lost_frames > max_lost_frames 就立刻 clear(), 而目标
    // 一旦判丢就会走 handle_aim_target_loss_exact() → reset_pidf(): 把 integral /
    // feed-forward / residual 全部清零。其中 residual 正是"每拍位移不足 1 个
    // count 时把零头攒到下一拍"的那份状态 —— 准星逼近锚点后每拍只有 0.2~0.6
    // count, 全靠它攒够 1 才发得出去。所以:
    //   · 单帧漏检(0.10 置信度检测器 + 快速横移目标的常态) → 整链重置 →
    //     连续两拍不发位移 = "一顿一顿";
    //   · residual 被反复清零 → 锚点附近永远差最后几个像素 = "在周围落不到身上"。
    // 配置里的 0 只当 1 帧(漏一帧走 tracker 预测分支, 不重置控制器), 想要更长
    // 的滑行窗口把"丢失目标缓存"调大即可。
    config.max_lost_frames = std::clamp(in.lost_target_cache_frames, 1, 240);
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
    current_id_ = -1;
    last_generation_ = -1;
}


EngineOutput AimEngine::tick(const EngineInput& in, double dt)
{
    EngineOutput out;
    const auto now = in.now_ns > 0 ? in.now_ns : steadyNowNs();
    motion::ControlConfig control;
    control.kp = {in.pidf_params.kp_x, in.pidf_params.kp_y};
    control.kd = {in.pidf_params.kd_x, in.pidf_params.kd_y};
    control.kf = {in.pidf_params.kf_x, in.pidf_params.kf_y};
    control.learning = {in.pidf_params.lr_x, in.pidf_params.lr_y};
    control.deadzone = {double(in.pidf_params.deadzone_x), double(in.pidf_params.deadzone_y)};
    control.calibration = in.calibration;
    control.period_ns = in.control_period_ns;
    control.max_age_ns = in.max_observation_age_ns;
    controller_.configure(control);
    if (selectorConfigChanged(in))
    {
        rebuildSelector(in); controller_.reset(); aimpoint_state_ = {};
        current_id_ = last_generation_ = -1; last_capture_ns_ = 0;
    }
    const auto captured = in.captured_ns;
    if (in.has_new_measurement && captured > 0 && captured <= now
        && now-captured <= control.max_age_ns && captured > last_capture_ns_)
    {
        const double measurement_dt = last_capture_ns_ ? (captured-last_capture_ns_)*1e-9 : dt;
        // Association may predict a box for matching, but the control observer
        // below only receives last_measurement at its actual capture time.
        const auto cameraMove = journal_->displacement(last_capture_ns_, captured, now);
        if (last_capture_ns_) selector_->shift_camera_origin(cameraMove.x, cameraMove.y);
        auto& tracker = selector_->tracker();
        const double frames = std::clamp(measurement_dt * 120.0, .1, 12.0);
        tracker.kalman.transition[2] = tracker.kalman.transition[7] = frames;
        last_capture_ns_ = captured;
        std::vector<cvm::recovered::Detection72Abi> detections;
        if (in.boxes && in.classes)
        {
            for (size_t i = 0; i < std::min(in.boxes->size(), in.classes->size()); ++i)
            {
                const auto& b = (*in.boxes)[i];
                const int cls = (*in.classes)[i];
                const float confidence = in.confidences && i < in.confidences->size() ? (*in.confidences)[i] : 0;
                const auto slot = std::find_if(in.target_slots.begin(), in.target_slots.end(),
                    [cls](const TargetSlot& s) { return s.class_id == cls; });
                if (slot == in.target_slots.end() || !std::isfinite(confidence) || confidence < slot->min_conf || b.width<=0 || b.height<=0
                    || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.width) || !std::isfinite(b.height)) continue;
                cvm::recovered::Detection72Abi d{};
                d.left=b.x;d.top=b.y;d.width=b.width;d.height=b.height;d.class_id=cls;d.confidence=confidence;
                detections.push_back(d);
            }
        }
        const float radius = float(std::min(in.fov_radius_x, in.fov_radius_y));
        const auto* selected = selector_->select_aim_target(detections,
            {float(in.crosshair_x),float(in.crosshair_y)}, {0,0}, std::max(0.0f,radius), 1);
        if (!selected)
        {
            controller_.reset(); current_id_=last_generation_=-1; tracks_.clear();
            out.motion_suppressed=true;
            return out;
        }
        const int generation=selector_->target_generation();
        if (generation!=last_generation_)
        {
            controller_.reset(); aimpoint_state_={}; out.motion_suppressed=true;
            last_generation_=current_id_=generation;
        }
        if (!selected->target_flag)
        {
            auto raw=selector_->tracker().last_measurement;
            if (class_id_ >= 0 && raw.class_id != class_id_)
            {
                controller_.reset(); aimpoint_state_ = {}; out.motion_suppressed = true;
            }
            raw.predicted_center_valid=0;
            const auto target=cvm::recovered::make_aimpoint_target_record_exact(raw);
            cvm::recovered::AimPointConfigExact aim;
            aim.reference_x=float(in.crosshair_x);aim.reference_y=float(in.crosshair_y);
            aim.default_ratio_low=aim.default_ratio_high=.5f;aim.class_rules_enabled=true;
            std::vector<cvm::recovered::ClassAimPointRuleExact> rules;
            for (const auto& slot:in.target_slots)
            {
                cvm::recovered::ClassAimPointRuleExact rule;
                rule.class_id=slot.class_id;rule.ratio_low=1-std::clamp(slot.y_offset_max,0.0f,1.0f);
                rule.ratio_high=1-std::clamp(slot.y_offset_min,0.0f,1.0f);rules.push_back(rule);
            }
            aim.class_rules=rules;
            cvm::recovered::target_to_aimpoint_exact(target,aimpoint_state_,aim);
            if (!std::isfinite(aimpoint_state_.aim_x) || !std::isfinite(aimpoint_state_.aim_y))
            {
                controller_.reset(); out.motion_suppressed=true; return out;
            }
            const double area=std::max(1.0f,(raw.right-raw.left)*(raw.bottom-raw.top));
            const double oldArea=std::max(1.0f,observed_box_.width*observed_box_.height);
            const double shapeNoise=std::clamp(std::abs(std::log(area/oldArea))*8.0,0.0,16.0);
            observed_anchor_={aimpoint_state_.aim_x,aimpoint_state_.aim_y};
            observed_box_={raw.left,raw.top,raw.right-raw.left,raw.bottom-raw.top};
            controller_.observe({in.sequence ? in.sequence : ++fallback_sequence_,captured,
                {observed_anchor_.x,observed_anchor_.y},1.0+shapeNoise},now);
            last_observed_sequence_=in.sequence;
            confidence_=raw.confidence; class_id_=raw.class_id;
        }
    }
    const auto command=controller_.advance(now,{in.crosshair_x,in.crosshair_y});
    out.have_target=command.valid;out.control_due=command.due;out.phase=command.phase;
    if (!command.valid) {tracks_.clear(); return out;}
    out.current_track_id=current_id_;
    out.anchor={float(command.predicted_anchor.x),float(command.predicted_anchor.y)};
    out.bbox=observed_box_;
    out.bbox.x+=out.anchor.x-observed_anchor_.x;out.bbox.y+=out.anchor.y-observed_anchor_.y;
    out.observed_bbox=observed_box_;
    out.class_id=class_id_;
    out.coasting=!in.has_new_measurement || last_observed_sequence_!=in.sequence;
    out.pixel_dx=command.pixels.x;out.pixel_dy=command.pixels.y;
    out.dx=int(std::lround(command.pixels.x));out.dy=int(std::lround(command.pixels.y));
    out.observed_at=command.observed_at;
    Track debug;debug.id=current_id_;debug.bbox=out.bbox;debug.anchor=out.anchor;
    debug.alive=true;debug.class_id=class_id_;debug.confidence=confidence_;debug.observed_this_frame=!out.coasting;
    tracks_={debug};
    return out;
}
}
