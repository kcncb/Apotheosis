#include "boss_aim.h"

#include <algorithm>
#include <cmath>

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

// ── 运动观测器的常数�?boss_aim.h: kAnchorObserverSmoothMs / InputLagS / VelTauMs /
//    GatePx。它们不是用户旋�?—�?延迟预测/提前量那两个旋钮只进 PID 的前馈项�?─────────

} // namespace

AimEngine::AimEngine() = default;
AimEngine::~AimEngine() = default;

void AimEngine::reset()
{
    selector_.reset();
    aimpoint_state_ = {};
    selector_slots_.clear();
    selector_lost_frames_ = -1;
    selector_normalizer_ = -1;
    current_id_ = -1;
    last_generation_ = -1;
    tracks_.clear();
    // 控制器历史整个清�? 观测器保留已标定出的"每计数多少像�?(那是游戏属�?
    // 不是这一局/这个目标的属�?�?
    pid_x_.reset();
    pid_y_.reset();
    motion_x_.resetTargetMotion();
    motion_y_.resetTargetMotion();
    last_counts_x_ = 0;
    last_counts_y_ = 0;
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
    // AVA 的 tracker 只要 lost_frames > max_lost_frames 就立刻 clear(), 而目标一旦
    // 判丢就会把控制器的历史全部重置 —— residual(不足 1 个计数时攒到下一拍的零头)、
    // 速度/前馈状态都清零。所以配置里的 0 只当 1 帧(漏一帧走 tracker 预测分支, 不重置
    // 控制器); 想要更长的滑行窗口把"丢失目标缓存"调大即可。窗口太短会表现为
    // "一顿一顿"以及锚点附近落不到位。
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

    if (selectorConfigChanged(in))
        rebuildSelector(in);

    // ── 只保�?可瞄"的检�? 其余类别直接不进选择�?──────────────────────
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
        current_id_ = -1;
        last_generation_ = -1;
        // 丢目�? 清掉控制器历�?积分/微分/零头), 观测器只清速度与历史�?
        pid_x_.reset();
        pid_y_.reset();
        motion_x_.resetTargetMotion();
        motion_y_.resetTargetMotion();
        last_counts_x_ = 0;
        anchor_obs_x_.reset();
        anchor_obs_y_.reset();
        last_counts_y_ = 0;
        return out;
    }

    const int generation = selector_->target_generation();
    const bool generation_changed = generation != last_generation_;
    last_generation_ = generation;
    current_id_ = generation;

    const auto aim_target = cvm::recovered::make_aimpoint_target_record_exact(*selected);

    // 瞄点 = 框内按类别比例取的点(aim_classes)。这部分�?PID 无关, 保留�?
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
    (void)cvm::recovered::target_to_aimpoint_exact(aim_target, aimpoint_state_, aim_config);

    if (!std::isfinite(aimpoint_state_.aim_x) || !std::isfinite(aimpoint_state_.aim_y))
    {
        current_id_ = -1;
        last_generation_ = -1;
        pid_x_.reset();
        pid_y_.reset();
        motion_x_.resetTargetMotion();
        motion_y_.resetTargetMotion();
        last_counts_x_ = 0;
        anchor_obs_x_.reset();
        anchor_obs_y_.reset();
        last_counts_y_ = 0;
        return out;
    }

    out.have_target = true;
    out.current_track_id = generation;
    out.anchor = {aimpoint_state_.aim_x, aimpoint_state_.aim_y};
    out.bbox = {selected->left, selected->top,
                selected->right - selected->left,
                selected->bottom - selected->top};
    out.observed_bbox = out.bbox;
    out.class_id = selected->class_id;
    out.coasting = selected->target_flag != 0;
    out.motion_suppressed = generation_changed;

    // ══════════════════════════════════════════════════════════════════════�?
    //  �?PID: 误差是【检测图像素】的浮点�? 输出是【整数鼠标计数�?
    // ══════════════════════════════════════════════════════════════════════�?
    //  �?AVA PIDF 整条管线已删�? 现在�?mouse/aim_pid.h + mouse/aim_motion.h 取代:
    //
    //    err_x = out.anchor.x - in.crosshair_x          // 像素(浮点)
    //    err_y = out.anchor.y - in.crosshair_y
    //    out.dx / out.dy = 本拍要发的计�?
              // 整数, 只在这里取整一�?
    //
    //  控制器全程在像素域用 double �?连取整零头都攒到下一�?, 所以不存在"先把浮点误差
    //  取整再算"造成的死区。链路约束依旧成�? 输出就是计数, 外面直接 sendRawMove 发出,
    //  没有任何"每计数多少像�?的写死标定�?
    //
    //  前馈需�?每计数多少像�?(游戏灵敏�?和目标自身速度, 这两个量由观测器【在线估�?
    //  它只�?我们自己猛动、画面跟着猛变"的窗口来�? 估不出来就一直不 ready, 前馈自动
    //  关闭, 退化成普�?PID —�?绝不会因为一个写死的常数自激�?
    // ── �?真换目标", 并【只在真换目标时】复�?──────────────────────────────
    // 用【原始】瞄点判�?不受滤波影响), 再决定要不要复位。顺�? 先判 -> 复位 -> 滤波�?
    //
    // 为什么不�?身份一变就复位": tracker 经常只是把【同一个目标】重新锁一�?实测
    // 2282 次身份变化里 1256 次瞄点跳变不�?25px)。而复位会把积�?微分/零头/速度估计
    // 全部清零 —�?实机实测身份变化�?7% 的帧(�?14.5 帧一�? 即每�?8 �?, 于是:
    //   · 积分(Ti=1s)永远攒不起来 �?顶不住后坐力这种持续推力 �?压枪压不�?实测 38% �?
    //     �?|ey|>10px);
    //   · 速度估计 48% 的帧�?0 �?前馈也顶不住匀�?后坐力�?
    // 这跟"目标缓存"是两件独立的�? 缓存是在源头少丢�? 这里是让控制器不被重锁打断�?
    const cv::Point2f raw_anchor = out.anchor;
    const float anchor_jump = has_last_anchor_
        ? std::hypot(raw_anchor.x - last_anchor_.x, raw_anchor.y - last_anchor_.y)
        : 1e9f;
    out.target_switched = out.motion_suppressed && anchor_jump >= 25.0f;
    out.target_anchor_jump_px = anchor_jump;
    last_anchor_ = raw_anchor;
    has_last_anchor_ = true;

    if (out.target_switched)
    {
        pid_x_.reset();
        pid_y_.reset();
        motion_x_.resetTargetMotion();
        motion_y_.resetTargetMotion();
        anchor_obs_x_.reset();
        anchor_obs_y_.reset();
        last_counts_x_ = 0;
        last_counts_y_ = 0;
    }

    // ── 目标运动观测�?必须在算误差之前) ────────────────────────────────────
    // 分工(2026-09-12 �?: 位置【不参与】—�?τ=0 时它原样输出测量�? 也就是控制器吃的�?
    // 原始瞄点, "甩到瞄点"永远是一拍。观测器只提供两�? ①【速度�?只喂前馈, 不进位置
    // 回路) ②野值门�?>40px 整拍丢弃, 位置原地不动)。速度与位置解�? 所以哪怕速度估错
    // �? 准星也不会跟着抽�?
    //
    // 实机�?瞄到锚点后抽�?的两个真正来�? 都在这里修掉:
    //   �?`AnchorObserver::configure()` 以前无条�?reset(), 而引擎【每拍】都调它 —�?
    //      延迟线永远攒不满, 速度恒为 0(前馈整个是死�?; 更糟的是位置残差里只�?
    //      `-k*u_delayed`(拿本拍刚发的指令�?已经在画面里生效"), 每拍把瞄点估计推�?
    //      alpha*k*cmd —�?实测 α=0.295、k=0.593、cmd=50 时是 8.7px/�? 指令越大越明�?
    //      所�?Kp=100 抽搐、Kp=30 看着正常(cmd �?Kp)。现�?configure 只在参数真变�?
    //      才清状态�?
    //   �?输入延迟以前直接用用户的「延迟预测�?他设 75ms = 9 �?: 减掉的是【甩枪途中那批
    //      大指令�? 凭空造出 v̂ �?k*cmd/dt �?71*cmd px/s 的假速度, 再乘 (延迟预测+提前�?
    //      就是几十像素的假误差(正反馈发�? 回归测试 [15]④d 复现�?。现在是固定的物�?
    //      常数 kAnchorObserverInputLagS = 11ms �?一拍�?
    // pending_self_motion_px 恒为 0: 自身位移的扣除只由观测器这一处承�? 不能扣第二遍�?
    // 输入延迟 = 链路死区(�?kAimDeadTimeS): 画面�?已经生效"的那批计数是 d 秒前发的�?
    // �?这里以前写的�?11ms(软件 E2E), 漏掉�?HID+游戏+显示 —�?后果是速度估计里混�?
    //   k*(u_�?- u_�?/dt �?1000px/s 的假速度, 又被 0.125s 的前馈放大成上百像素假误�?
    //   (日志�?fsx=1.00/ffx=146.27 就是这样来的)。改成实测死区后 v̂ 才是干净的目标速度�?
    const bool observed = !out.coasting;
    anchor_obs_x_.configure(kAnchorObserverSmoothMs, in.px_per_count_x,
                            kAnchorObserverGatePx, kAimDeadTimeS,
                            kAnchorObserverVelTauMs);
    anchor_obs_y_.configure(kAnchorObserverSmoothMs, in.px_per_count_y,
                            kAnchorObserverGatePx, kAimDeadTimeS,
                            kAnchorObserverVelTauMs);
    {
        const double measured_x = static_cast<double>(out.anchor.x);
        const double measured_y = static_cast<double>(out.anchor.y);
        out.anchor = {
            static_cast<float>(anchor_obs_x_.step(measured_x, last_counts_x_, dt, observed)),
            static_cast<float>(anchor_obs_y_.step(measured_y, last_counts_y_, dt, observed))};
    }

    const double err_px_x = static_cast<double>(out.anchor.x) - in.crosshair_x;
    const double err_px_y = static_cast<double>(out.anchor.y) - in.crosshair_y;

    pid_x_.configure(in.pid_x);
    pid_y_.configure(in.pid_y);

    // 自动估算"每计数像�?的观测器只在用户没手填时才需�? 这里保持更新(它不参与输出)�?
    motion_x_.setManualPxPerCount(in.px_per_count_x);
    motion_y_.setManualPxPerCount(in.px_per_count_y);
    motion_x_.update(static_cast<double>(out.anchor.x), last_counts_x_, dt, observed);
    motion_y_.update(static_cast<double>(out.anchor.y), last_counts_y_, dt, observed);

    AimPidFeedback fb_x;
    // 目标自身速度一律由【运动观测器】给: 它是"扣掉自己位移之后"的速度, 只喂前馈
    // (提前�?延迟预测)。pending 一�?0: 自身位移的扣除已经由观测器的输入项承担�?
    //
    // �?前提是知�?每计数多少像�?: k=0(用户没填「每计数像素�?时观测器扣不掉自身位�?
    //   它的速度里就混着 k*u/dt �?71*cmd px/s 那一大块 —�?拿去前馈会变成几十像素的�?
    //   误差, 比不通前馈糟得多。所以这种情况【直接不发速度前馈�?退化成�?PID), 不猜�?
    //   不确定是"自动估算�?k̂"也一样危�?估偏 30% 就是几十 px/s 的假速度), 所以只�?
    //   用户手填的值。日志里 k=(x,y) �?vx/vy 能直接看出这一路是活的还是关的�?
    const bool ff_ok_x = in.px_per_count_x > 0.0;
    const bool ff_ok_y = in.px_per_count_y > 0.0;
    fb_x.target_velocity_px_s = ff_ok_x ? anchor_obs_x_.velocity() : 0.0;
    AimPidFeedback fb_y;
    fb_y.target_velocity_px_s = ff_ok_y ? anchor_obs_y_.velocity() : 0.0;

    // ── 在途自身位�?Smith 补偿) ─────────────────────────────────────────────
    // �?kAimDeadTimeS / kAimDeadTimeCompScale �?AnchorObserver::inFlightPx()�?
    // 这是"Kp=100 也不�?的关键一�? 它把"已经发出去、画面还没回�?的那批位移从看到�?
    // 误差里扣掉。没有它, 100 的增益在 46ms 死区下是 1.9 倍临�?-> 必然极限环�?
    fb_x.pending_self_motion_px = kAimDeadTimeCompScale * anchor_obs_x_.inFlightPx(kAimDeadTimeS);
    fb_y.pending_self_motion_px = kAimDeadTimeCompScale * anchor_obs_y_.inFlightPx(kAimDeadTimeS);

    out.dx = pid_x_.step(err_px_x, dt, fb_x);
    out.dy = pid_y_.step(err_px_y, dt, fb_y);
    last_counts_x_ = out.dx;
    last_counts_y_ = out.dy;

    // "速度前馈是否活着": 手填了「每计数像素」→ 观测器能把自身位移扣干净 �?速度可信,
    // 提前�?延迟预测才真的在起作用。以前这里报的是"在线 k̂ 标定是否 ready", 而在�?k̂
    // 在乱画面里经常一直不 ready(实测 1434 帧里只有 246 �?ready), 于是"同一组参数手�?
    // 完全不一�? —�?用户改用【手填每计数像素】就是为了绕开这个抖动, 日志也跟着改成
    // 报真正生效的那件事�?
    out.pid_ff_ready = ff_ok_x || ff_ok_y;
    out.pid_dt_ms = pid_x_.usedDt() * 1000.0;
    out.pid_error_px_x = err_px_x;
    out.pid_error_px_y = err_px_y;
    out.pid_used_error_px_x = pid_x_.usedErrorPx();
    out.pid_used_error_px_y = pid_y_.usedErrorPx();
    out.pid_cmd_x = pid_x_.outputCounts();
    out.pid_cmd_y = pid_y_.outputCounts();
    out.pid_feedforward_px_x = pid_x_.feedforwardPx();
    out.pid_feedforward_px_y = pid_y_.feedforwardPx();
    out.pid_ff_scale_x = pid_x_.feedforwardScale();
    out.pid_ff_scale_y = pid_y_.feedforwardScale();
    out.pid_pending_px_x = fb_x.pending_self_motion_px;
    out.pid_pending_px_y = fb_y.pending_self_motion_px;
    out.pid_px_per_count_x = motion_x_.pxPerCount();
    out.pid_px_per_count_y = motion_y_.pxPerCount();
    out.pid_target_vel_x = fb_x.target_velocity_px_s;
    out.pid_target_vel_y = fb_y.target_velocity_px_s;
    out.pid_i_px_x = pid_x_.iTerm();
    out.pid_i_px_y = pid_y_.iTerm();
    out.pid_d_px_x = pid_x_.dTerm();
    out.pid_d_px_y = pid_y_.dTerm();
    out.pid_integral_x = pid_x_.integralState();
    out.pid_integral_y = pid_y_.integralState();
    out.pid_carry_x = pid_x_.carryState();
    out.pid_carry_y = pid_y_.carryState();
    out.pid_limit_x = pid_x_.outputLimit();
    out.pid_limit_y = pid_y_.outputLimit();
    out.pid_fits_x = motion_x_.acceptedFits();
    out.pid_fits_y = motion_y_.acceptedFits();

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
