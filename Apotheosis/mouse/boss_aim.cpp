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

// ── 【2026-09-13 删除】运动观测器常数 ────────────────────────────────────────
// kAnchorObserverSmoothMs / VelTauMs / GatePx / InputLagS 全部随观测器一起删除。
// 见 boss_aim.h 顶部的架构说明。

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
    // 控制器历史整个清掉。框平滑器也一起复位 —— 换了目标/重新开始, 上一段的
    // 位置速度估计对新目标没有意义。
    pid_x_.reset();
    pid_y_.reset();
    anchor_filter_x_.reset();
    anchor_filter_y_.reset();
    // 距离尺度也复位: 新目标在画面里的框高和旧目标无关, 沿用上一段的平滑值会让
    // 重新锁定后的头几拍用一个属于旧目标的尺度出力(表现是"刚锁上时快/慢一截")。
    // ★ 注意 AimPid::reset() 【故意不清 scale_】(它由引擎每拍注入), 所以复位
    //   尺度源是引擎的责任, 不能指望 PID 的 reset。
    aim_scale_.reset();
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

// ── 【2026-09-13 删除】beginCalibrationMeasure / cancelCalibrationMeasure ────
// 这两个函数驱动 AnchorMotionEstimator 的测量会话, 用于标定 k̂(每计数像素)。
// k̂ 已不再被任何控制逻辑消费(前馈删除), 测量功能连同界面按钮一起移除。

EngineOutput AimEngine::tick(const EngineInput& in, double dt)
{
    EngineOutput out;

    if (selectorConfigChanged(in))
        rebuildSelector(in);

    // ── 只保留"可瞄"的检测, 其余类别直接不进选择；──────────────────────
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
        // 丢目标: 控制器历史(积分/微分/零头)与框平滑器状态一起清掉 —— 换了目标,
        // 上一段的位置/速度估计对新目标没有意义, 留着只会让它"预测"到旧方向去。
        pid_x_.reset();
        pid_y_.reset();
        anchor_filter_x_.reset();
        anchor_filter_y_.reset();
        last_counts_x_ = 0;
        last_counts_y_ = 0;
        return out;
    }

    const int generation = selector_->target_generation();
    const bool generation_changed = generation != last_generation_;
    last_generation_ = generation;
    current_id_ = generation;

    const auto aim_target = cvm::recovered::make_aimpoint_target_record_exact(*selected);

    // 瞄点 = 框内按类别比例取的点(aim_classes)。这部分与PID 无关, 保留它
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
        anchor_filter_x_.reset();
        anchor_filter_y_.reset();
        last_counts_x_ = 0;
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

    // ═══════════════════════════════════════════════════════════════════════
    //  新PID: 误差是【检测图像素】的浮点量 输出是【整数鼠标计数】
    // ═══════════════════════════════════════════════════════════════════════
    //  旧AVA PIDF 整条管线已删除, 现在由 mouse/aim_pid.h 取代
    //  (aim_motion.h / anchor_observer.h 已于 2026-09-14 随在线 k̂ 估计一起删除):
    //
    //    err_x = out.anchor.x - in.crosshair_x          // 像素(浮点)
    //    err_y = out.anchor.y - in.crosshair_y
    //    out.dx / out.dy = 本拍要发的计数              // 整数, 只在这里取整一次
    //
    //  控制器全程在像素域用 double 算, 连取整零头都攒到下一拍, 所以不存在"先把浮点误差
    //  取整再算"造成的死区。链路约束依旧成立: 输出就是计数, 外面直接 sendRawMove 发出,
    //  没有任何"每计数多少像素"的写死标定。
    //
    //  前馈需要每计数多少像素(游戏灵敏度和目标自身速度), 这两个量由观测器【在线估出】
    //  它只靠"我们自己猛动、画面跟着猛变"的窗口来定, 估不出来就一直不 ready, 前馈自动
    //  关闭, 退化成普通PID —— 绝不会因为一个写死的常数自激。
    // ── "真换目标", 并【只在真换目标时】复位──────────────────────────────
    // 用【原始】瞄点判断(不受滤波影响), 再决定要不要复位。顺序: 先判 -> 复位 -> 滤波。
    //
    // 为什么不能"身份一变就复位": tracker 经常只是把【同一个目标】重新锁一次, 实测
    // 2282 次身份变化里 1256 次瞄点跳变不到25px)。而复位会把积分/微分/零头/速度估计
    // 全部清零 —— 实机实测身份变化占7% 的帧(约14.5 帧一次, 即每秒8 次), 于是:
    //   · 积分(Ti=1s)永远攒不起来 → 顶不住后坐力这种持续推力 → 压枪压不住(实测 38% 的
    //     帧有|ey|>10px);
    //   · 速度估计 48% 的帧为0 → 前馈也顶不住匀速后坐力。
    // 这跟"目标缓存"是两件独立的事: 缓存是在源头少丢帧, 这里是让控制器不被重锁打断。
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
        anchor_filter_x_.reset();
        anchor_filter_y_.reset();
        last_counts_x_ = 0;
        last_counts_y_ = 0;
    }

    // ── 目标运动观测器必须在算误差之前) ────────────────────────────────────
    // 分工(2026-09-12 ）: 位置【不参与】—，τ=0 时它原样输出测量值 也就是控制器吃的量
    // 原始瞄点, "甩到瞄点"永远是一拍。观测器只提供两件 ①【速度）只喂前馈, 不进位置
    // 回路) ②野值门限>40px 整拍丢弃, 位置原地不动)。速度与位置解耦 所以哪怕速度估错
    // 错 准星也不会跟着抽搐
    //
    // 实机上瞄到锚点后抽搐的两个真正来源 都在这里修掉:
    //   件`AnchorObserver::configure()` 以前无条件reset(), 而引擎【每拍】都调它 —，
    //      延迟线永远攒不满, 速度恒为 0(前馈整个是死剩; 更糟的是位置残差里只剩
    //      `-k*u_delayed`(拿本拍刚发的指令，已经在画面里生效"), 每拍把瞄点估计推偏
    //      alpha*k*cmd —，实测 α=0.295、k=0.593、cmd=50 时是 8.7px/拍 指令越大越明显
    //      所以Kp=100 抽搐、Kp=30 看着正常(cmd ∝Kp)。现在configure 只在参数真变时
    //      才清状态。
    //   件输入延迟以前直接用用户的「延迟预测）他设 75ms = 9 拍: 减掉的是【甩枪途中那批
    //      大指令， 凭空造出 v̂ 是k*cmd/dt ≈71*cmd px/s 的假速度, 再乘 (延迟预测+提前量
    //      就是几十像素的假误差(正反馈发散 回归测试 [15]④d 复现了。现在是固定的物理
    //      常数 kAnchorObserverInputLagS = 11ms 是一拍半
    // pending_self_motion_px 恒为 0: 自身位移的扣除只由观测器这一处承担 不能扣第二遍。
    // 输入延迟 = 链路死区(用kAimDeadTimeS): 画面里已经生效"的那批计数是 d 秒前发的。
    // 但这里以前写的是11ms(软件 E2E), 漏掉了HID+游戏+显示 —，后果是速度估计里混。
    //   k*(u_（- u_）/dt ≈1000px/s 的假速度, 又被 0.125s 的前馈放大成上百像素假误差
    //   (日志里fsx=1.00/ffx=146.27 就是这样来的)。改成实测死区后 v̂ 才是干净的目标速度；
    // ── 框平滑 (α-β) — 在算误差【之前】, 且是进 PID 的唯一预处理 ──────────────
    //
    // 位置: 只做一件事 —— 滤掉检测框的抖动。
    //   实测框心逐帧变化中位 0.06px, 但偶发跳变 1.9px; 在 8.3ms 拍间隔下那是
    //   230px/s 的【假速度】。PID 的 P 项放大的是这个噪声, 不是真实误差 ——
    //   所以 Kp 一开大就开始抖。把平滑放在 PID 之前, 喂进去的信号干净了, Kp 才敢开大。
    //
    // 为什么用 α-β 而不是一阶低通: α-β 带速度状态, 是"预测 + 修正"。匀速目标下
    // 预测本来就准, 新息接近 0, 所以【几乎不落后】; 而随机抖动互相抵消, 照样被滤掉。
    // 一阶低通只知道位置, 追匀速目标会持续落后一个固定量, 得靠更大的 Kp 去补 ——
    // 而 Kp 越大残留噪声放大得越厉害, 绕成一圈。同一份抗抖能力, α-β 的滞后小得多。
    //
    // 速度【不外传】: 它只是滤波器内部的状态。现在没有任何前馈消费目标速度。
    {
        const double tau_s = kAnchorFilterTauMs / 1000.0;
        anchor_filter_x_.configure(tau_s);
        anchor_filter_y_.configure(tau_s);
        out.anchor = {
            static_cast<float>(anchor_filter_x_.step(
                static_cast<double>(out.anchor.x), dt)),
            static_cast<float>(anchor_filter_y_.step(
                static_cast<double>(out.anchor.y), dt))};
    }

    // ── 预测补偿 (2026-09-13, 对齐 AimMagic 1.0.30「预测补偿」) ──────────────
    //
    // 位置很关键: **在算误差之前**, 把"目标将要到哪"的偏移直接加到瞄点上。
    // AM 原文就是这么干的 (`*pfVar14 = fVar19 + *pfVar14;  // 预测偏移直接加到框中心`),
    // 所以后面选靶、扳机、PID 吃的都是预测后的位置。本实现的选靶在更早阶段完成
    // (selector_ 在 tick 开头就跑了), 所以这里只影响【误差 -> PID】这一段;
    // 这对"准星追得上移动目标"这个目标来说是等价的, 而且改动面小得多。
    //
    // 速度来源: AnchorFilter 的 α-β 速度状态 —— 它本来就是"预测 + 修正"里的预测项
    // 所用的同一个量(AM 也是复用它自己维护的速度低通, 见 aim_predict.h 顶部)。
    // 尺寸权重输入: 当前检测框的宽度。
    {
        predict_.configure(in.predict_factor_x, in.predict_factor_y,
                           in.predict_min_width, in.predict_max_width,
                           in.predict_damp,
                           in.predict_max_px, in.predict_vel_floor);

        const double vx = anchor_filter_x_.velocity();
        const double vy = anchor_filter_y_.velocity();
        const bool vel_ok = anchor_filter_x_.velocityValid()
                         && anchor_filter_y_.velocityValid();
        const double box_w = static_cast<double>(out.bbox.width);

        const AimPredict::Offset lead = predict_.compute(vx, vy, box_w, vel_ok);
        predict_.commit(lead);

        last_predict_width_ = static_cast<float>(box_w);
        last_predict_lead_x_ = static_cast<float>(lead.x);
        last_predict_lead_y_ = static_cast<float>(lead.y);

        out.predict_lead_x = last_predict_lead_x_;
        out.predict_lead_y = last_predict_lead_y_;
        out.predict_size_weight = static_cast<float>(predict_.sizeWeight(box_w));
        out.predict_active = predict_.enabled() && (lead.x != 0.0 || lead.y != 0.0);

        // 静止目标 / 关预测 / 速度不可信 => lead 恒为 (0,0), 这一步是恒等变换,
        // 与不做预测逐位相同。这是本模块最重要的安全性质。
        out.anchor.x = static_cast<float>(static_cast<double>(out.anchor.x) + lead.x);
        out.anchor.y = static_cast<float>(static_cast<double>(out.anchor.y) + lead.y);
    }

    const double err_px_x = static_cast<double>(out.anchor.x) - in.crosshair_x;
    const double err_px_y = static_cast<double>(out.anchor.y) - in.crosshair_y;

    // 控制器参数直接来自配置 —— 不再有任何在线估算出来的量要注入。
    pid_x_.configure(in.pid_x);
    pid_y_.configure(in.pid_y);

    // ── 距离尺度 (2026-09-13, 见 mouse/aim_scale.h) ──────────────────────────
    //
    // 【要解决什么】同样 200px/s 的世界速度, 近处目标在画面里走得快、远处走得慢
    //   (v_px ≈ f·v_world/d)。所以"跟得上"所需的等效增益和距离有关: 近处该更快,
    //   远处该更慢 —— 否则要么近处冲过头, 要么远处拖沓。
    //
    // 【为什么用框高】它是本项目【唯一】可用的距离代理。双机架构下拿不到真实距离:
    //   AimbotTarget::depth_at_pivot 是恒 -1 的占位常量, 相机的内参也没有标定。
    //   而框高随距离近似成反比, 单调、连续、每帧都有。
    //
    // 【框高取自哪里, 为什么】取【原始观测框】(out.bbox, 未经 AnchorFilter 平滑)。
    //   理由: AnchorFilter 平滑的是【瞄点位置】, 不是框尺寸; 而框高是一条独立的
    //   标量通道, 有自己的平滑(AimScale 内部 smooth_tau_s=120ms)。让两个滤波器
    //   串在同一条数据上, 会让"框高"的等效带宽取决于锚点滤波器的参数 —— 那正是
    //   项目明令禁止的"多个时间滤波器串联"。所以这里取原始框高, 平滑交给 AimScale。
    //   ★ 注意: 取的是 out.bbox(观测框)而不是预测后的瞄点, 所以预测补偿的
    //     lead 偏移不会污染尺度。
    //
    // 【尺度作用在哪些环节】本实现只作用在【等效增益】这一条上, 也就是
    //   kp_eff = kp * s (由 AimPid::setScale 注入)。
    //   为什么是这一条而不是别的:
    //     · "提前量上限"和"速度可信度门限"这两条 —— 本项目当前【没有提前量】在跑
    //       (predict_factor 默认 0, 见 config.h), 没有可调的对象。等真要开预测
    //       的时候再接, 现在接上去就是无人消费的死代码。
    //     · "平滑强度" —— 把它做成尺度的函数会让远处目标的锚点滞后变大, 那和 G5
    //       "不许有可见滞后"直接冲突, 所以故意不接。
    //     · 增益这一条就够用且可验证: 近处等效增益高(拉枪快), 远处等效增益低
    //       (不会把量化噪声放大成抖动)。
    //
    // ★ 尺度【不】用于"几何投影补偿": 屏幕速度本身就是世界速度的投影, 再乘一个
    //   与距离成正比的因子就是重复计算(见 aim_scale.h 顶部的 ⚠️)。
    //
    // 关闭态(aim_scale 参数全默认但 s_min==s_max, 或调用方要求关闭): s 恒为 1.0,
    // setScale(1.0) 是恒等变换, 整条链路与没有尺度逐位相同。
    {
        aim_scale_.configure(in.aim_scale);
        const double bbox_h = static_cast<double>(out.bbox.height);
        // ★ 只在【真的有目标】时喂: 丢目标时框高是 0, 喂进去会让尺度瞬间跳到远处值,
        //   下一拍重新锁定时又要爬回来 —— 表现为"重新锁定后的头几拍明显发软"。
        //   丢目标时保持上一拍的尺度, 与"位置输出不变"的降级要求一致。
        if (out.have_target && bbox_h > 0.0)
            out.aim_scale = aim_scale_.step(bbox_h, dt);
        else
            out.aim_scale = aim_scale_.scale();
        out.aim_scale_height_px = aim_scale_.smoothedHeight();
        out.aim_scale_active = aim_scale_.initialized();

        pid_x_.setScale(out.aim_scale);
        pid_y_.setScale(out.aim_scale);
    }

    // ── 【2026-09-13 删除】全部前馈构造 ──────────────────────────────────────
    // 这里原来构造两个 AimPidFeedback, 填三样东西:
    //   · target_velocity_px_s   目标自身速度   (需要观测器)
    //   · pending_self_motion_px Smith 补偿     (需要 k̂)
    //   · dead_time_s            配对补偿时间窗 (需要 k̂)
    // 全部删除 —— 前馈依赖 k̂, 而 k̂ 在双机架构下测不准。详见 aim_pid.h 的删除说明。
    // 现在 step() 只吃 (误差, dt), 控制器就是教科书 PID。
    // (在途自身位移补偿没有消失: 它搬进了 AimPid 内部, 用【计数域】实现, 不含 k̂。
    //  见 aim_pid.h 的 inflight_beta。)
    out.dx = pid_x_.step(err_px_x, dt);
    out.dy = pid_y_.step(err_px_y, dt);
    last_counts_x_ = out.dx;
    last_counts_y_ = out.dy;

    // 遥测: 只报真正还在生效的量。
    out.pid_dt_ms = pid_x_.usedDt() * 1000.0;
    out.pid_error_px_x = err_px_x;
    out.pid_error_px_y = err_px_y;
    out.pid_used_error_px_x = pid_x_.usedErrorPx();
    out.pid_used_error_px_y = pid_y_.usedErrorPx();
    out.pid_cmd_x = pid_x_.outputCounts();
    out.pid_cmd_y = pid_y_.outputCounts();
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
    // ★ 在途补偿遥测: 排查"冲过头/贴不死"最先看这两个 —— 补偿是不是吃掉了稳态误差。
    out.pid_inflight_x = static_cast<int>(std::lround(pid_x_.inFlightCounts()));
    out.pid_inflight_y = static_cast<int>(std::lround(pid_y_.inFlightCounts()));
    out.pid_inflight_beta_x = in.pid_x.inflight_beta;
    out.pid_inflight_beta_y = in.pid_y.inflight_beta;

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
