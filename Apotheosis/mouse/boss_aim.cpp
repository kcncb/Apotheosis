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

// EventSync 档: 跟踪器参数是否与上一拍相同(相同就不重新 configure,
// 免得每拍重建轨迹表、把速度采样窗清空)。
bool sameTrackerParams(const AimTrackerParams& a, const AimTrackerParams& b) noexcept
{
    return a.min_hits == b.min_hits
        && a.max_age == b.max_age
        && a.assoc_radius_px == b.assoc_radius_px
        && a.assoc_iou == b.assoc_iou
        && a.vel_window_s == b.vel_window_s
        && a.pred_factor_x == b.pred_factor_x
        && a.pred_factor_y == b.pred_factor_y
        && a.pred_min_w == b.pred_min_w
        && a.pred_max_w == b.pred_max_w
        && a.pred_max_lead_px == b.pred_max_lead_px
        && a.pred_vel_floor == b.pred_vel_floor;
}

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
    // 跟踪器与它持有的身份一起清掉(会话停止/重新开始)。
    esync_tracker_.reset();
    esync_locked_track_ = -1;
    esync_configured_ = false;
}

// ── ⑤ 在途账本登记 (AM 的发送环) ─────────────────────────────────────────────
//
// 由 mouse_thread_loop.cpp 在 sendRawMove 【之后】调用 —— 登的必须是真正发出去的
// 整数计数(不是 PID 输出里的零头, 那部分没发出去, 由 carry 攒到下一拍)。
void AimEngine::noteAimSend(int dx, int dy)
{
    esync_tracker_.noteSend(dx, dy);
}

bool AimEngine::selectorConfigChanged(const EngineInput& in) const{
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
        // 丢目标: 【不清控制器状态】。
        //   理由正是 AimMagic §3.4 的"状态跨帧持有": 单帧漏检(遮挡/闪一下)由跟踪器
        //   的 max_age 滑行窗口吸收, 而"漏一帧就把积分/零头全清掉"会让积分永远攒不
        //   起来(实测身份变化占 7% 的帧 → 每秒清 8 次)。
        //   轨迹真的死掉(max_age 到期)时, 跟踪器会换 id → 走下面的"真换目标"复位,
        //   所以不存在"拿着旧状态追新目标"的风险。
        //
        // 跟踪器仍然要推进一帧(空观测), 让漏帧计数增长、超龄轨迹被淘汰。
        esync_tracker_.beginFrame(dt);
        esync_tracker_.endFrame();
        out.esync_active = true;
        out.esync_track_id = esync_tracker_.lockedId();
        out.esync_track_count = static_cast<int>(esync_tracker_.tracks().size());
        const auto* lt = esync_tracker_.locked();
        if (lt)
        {
            out.esync_track_hits = lt->hits;
            out.esync_track_age = lt->age;
            out.esync_track_confirmed = lt->confirmed;
            out.esync_vel_x = lt->vel_x;
            out.esync_vel_y = lt->vel_y;
            out.esync_vel_valid = lt->vel_valid;
            out.esync_pred_k_x = lt->pred_k_x;
            out.esync_pred_k_y = lt->pred_k_y;
        }
        else
        {
            // 轨迹真的死了: 这才复位, 与"身份变化"同一条路径。
            pid_x_.reset();
            pid_y_.reset();
            anchor_filter_x_.reset();
            anchor_filter_y_.reset();
            last_counts_x_ = 0;
            last_counts_y_ = 0;
            esync_locked_track_ = -1;
        }
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

    // ── 跟踪器接管【身份】 (移植 AimMagic 1.0.30 PID-EventSync) ───────────────
    //
    // 只做一件事: 给这一拍的选中框一个【跨帧稳定的身份】。
    // selector 的 generation 在目标短暂漏检/重锁时会换号(实机实测身份变化占 7% 的
    // 帧), 而每次换号都可能触发复位。跟踪器把同一目标的连续观测关联到同一条轨迹
    // 上, 于是:
    //   · 身份只在【真的换了一个目标】时变化 → 复位不再被重锁打断;
    //   · 速度跨帧持有(采样窗), 且每轨一份预测状态(系数/平滑),
    //     两个目标交替出现时不会互相串状态。
    // ★ 这里【不】改瞄点、不改误差、不改 PID —— 那些仍在下面同一条路径里。
    {
        // ★ configure() 只在参数真的变了才重建跟踪器状态 —— 与 AnchorObserver
        //   当年"每拍无条件 reset 导致状态永远攒不满"的教训同源(见本文件下方注释)。
        //   用户改 min_hits/max_age 时参数变化 → 重新装一次并保留已有轨迹。
        if (!esync_configured_ || !sameTrackerParams(esync_params_, in.esync))
        {
            esync_tracker_.configure(in.esync);
            esync_params_ = in.esync;
            esync_configured_ = true;
        }
        esync_tracker_.beginFrame(dt);
        // 只看【本帧真的有观测】的框: coasting(selector 自己在滑行)的框不喂跟踪器,
        // 否则会把预测出来的假位移当成观测喂进速度采样窗 —— 那是正反馈。
        if (!out.coasting)
        {
            TrackBox tbl{};
            tbl.x = out.observed_bbox.x;
            tbl.y = out.observed_bbox.y;
            tbl.w = out.observed_bbox.width;
            tbl.h = out.observed_bbox.height;
            esync_tracker_.offer(tbl, out.observed_bbox.x + out.observed_bbox.width * 0.5,
                                 out.observed_bbox.y + out.observed_bbox.height * 0.5);
        }
        esync_tracker_.endFrame();

        const auto* lt = esync_tracker_.locked();
        out.esync_active = true;
        out.esync_track_count = static_cast<int>(esync_tracker_.tracks().size());
        if (lt)
        {
            out.esync_track_id = lt->id;
            out.esync_track_hits = lt->hits;
            out.esync_track_age = lt->age;
            out.esync_track_confirmed = lt->confirmed;
            out.esync_vel_x = lt->vel_x;
            out.esync_vel_y = lt->vel_y;
            out.esync_vel_valid = lt->vel_valid;
            out.esync_pred_k_x = lt->pred_k_x;
            out.esync_pred_k_y = lt->pred_k_y;
            // 身份切换判据: 跟踪器身份变化【就是】换目标。跟踪器的 id 有粘滞性,
            // 它换号只发生在真换了目标(旧轨迹死了)。★ 不需要旧的"瞄点跳 >=25px"闸,
            // 而且少了它, "小跳变真换目标"就不再漏判(实测 2282 次里 1256 次跳 <25px)。
            out.motion_suppressed = (esync_locked_track_ != -1 && lt->id != esync_locked_track_);
            out.current_track_id = lt->id;
            out.coasting = (lt->age > 0);
        }
        else
        {
            // 跟踪器手里一条已锁定的轨迹都没有: 本拍没有可瞄身份, 直接交还"丢目标"。
            // ★ 这里【不清控制器状态】(见上面丢目标分支的说明): 目标可能只是被遮挡了
            //   一两帧, 下一拍关联回同一条轨迹时积分/零头还在, 不该白清一遍。
            out.have_target = false;
            out.current_track_id = -1;
            return out;
        }
        esync_locked_track_ = out.current_track_id;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    //  控制器: 误差是【检测图像素】的浮点量, 输出是【整数鼠标计数】
    // ═══════════════════════════════════════════════════════════════════════════
    //  旧AVA PIDF 与旧的观测器/前馈整条管线已删除, 现在由 mouse/aim_pid.h 取代
    //  (aim_motion.h / anchor_observer.h 已于 2026-09-14 随在线 k̂ 估计一起删除):
    //
    //    err_x = out.anchor.x - in.crosshair_x          // 像素(浮点)
    //    err_y = out.anchor.y - in.crosshair_y
    //    out.dx / out.dy = 本拍要发的计数              // 整数, 只在这里取整一次
    //
    //  控制器全程在像素域用 double 算, 连取整零头都攒到下一拍, 所以不存在"先把浮点误差
    //  取整再算"造成的死区。链路约束依旧成立: 输出就是计数, 外面直接 sendRawMove 发出,
    //  没有任何"每计数多少像素"的写死标定。
    // ★ 唯一的例外是跟踪器的在途换算链(⑤): 它按 AM 的形态用【用户手填的 k̂】把已发
    //   计数换回像素, 且默认关闭; 开关一关, 全链路就不含任何 k̂。

    // ── 框平滑 (α-β) —— 在算误差【之前】, 且是进 PID 的唯一预处理 ──────────────
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
    // ★ 速度状态的去处只有一处: 喂给跟踪器的【自身瞄准速度】(AM 的系数涨落条件
    //   "自己没在动时系数只落不涨"要用它, 见下面预测补偿段)。位置路径不受它影响。

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

    // ── 预测补偿: 每轨预测状态机 (AimTracker::predictionLead) ────────────────
    //
    // 位置很关键: **在算误差之前**, 把"目标将要到哪"的偏移直接加到瞄点上。
    // AM 原文就是这么干的 (`*pfVar14 = fVar19 + *pfVar14;  // 预测偏移直接加到框中心`),
    // 所以后面 PID 吃的都是预测后的位置。
    //
    // ★ 状态住在【每条轨迹】上(不是全局一份): 每条轨迹一份系数/平滑状态, 速度来自
    //   轨迹自己的采样窗(AM §4.3)。换目标就换到另一条轨迹的状态, 不串味; 而且目标
    //   短暂漏检时轨迹还在, 系数不会每次重锁都从 0 重爬。
    //
    // ★ 先喂【自身瞄准速度】—— AM 的系数涨落有第二个条件(FUN_14006e470 行 135):
    //   "自己没在动时系数只落不涨"。不喂它 = 自己永远算没在动 = 预测永远不启动。
    //   自身速度用锚点滤波器的速度。
    {
        const double self_vx = anchor_filter_x_.velocity();
        const double self_vy = anchor_filter_y_.velocity();
        const bool ok = anchor_filter_x_.velocityValid()
                     && anchor_filter_y_.velocityValid();
        esync_tracker_.setAimVelocity(ok ? self_vx : 0.0, ok ? self_vy : 0.0);
    }

    double lead_x = 0.0;
    double lead_y = 0.0;
    const bool active = esync_tracker_.predictionLead(lead_x, lead_y);

    // 预测关(系数 0)时 predictionLead 恒返回 (0,0) —— 与"不做预测"逐位相同。
    last_predict_width_ = static_cast<float>(out.bbox.width);
    last_predict_lead_x_ = static_cast<float>(lead_x);
    last_predict_lead_y_ = static_cast<float>(lead_y);

    out.predict_lead_x = last_predict_lead_x_;
    out.predict_lead_y = last_predict_lead_y_;
    out.predict_size_weight =
        static_cast<float>(esync_tracker_.sizeWeight(static_cast<double>(out.bbox.width)));
    out.predict_active = active;

    out.anchor.x = static_cast<float>(static_cast<double>(out.anchor.x) + lead_x);
    out.anchor.y = static_cast<float>(static_cast<double>(out.anchor.y) + lead_y);

    // 遥测: 系数在本拍推进后的值(排查"预测到底有没有爬上去"看这两个)。
    if (const auto* lt = esync_tracker_.locked())
    {
        out.esync_pred_k_x = lt->pred_k_x;
        out.esync_pred_k_y = lt->pred_k_y;
    }

    double err_px_x = static_cast<double>(out.anchor.x) - in.crosshair_x;
    double err_px_y = static_cast<double>(out.anchor.y) - in.crosshair_y;

    // ─ ⑤ 在途自身位移补偿 (AM 的发送环 ÷ k̂) ────────────────────────────────
    //
    // AM: FUN_140067000 行 1172-1209 —— 把"已经发出去、还没生效"的计数求和, 再
    // ÷ counts_per_pixel 换回【像素】, 从误差里扣掉。
    //
    // ★★ 与 aim_pid.h 的【计数域】在途补偿(u -= beta*N/W)的关系: 两者是同一条 Smith
    //    预测器的两种做法, 严格【二选一】—— 同开会把同一批在途指令扣两次 =
    //    过补偿 = 正反馈发散(见 aim_pid.h 的 inflight_beta 要点①)。
    //    怎么选(见下面的 am_inflight): 换算窗 > 0 时用 AM 这一种, 否则用计数域那种。
    //   ★ 窗口 0(默认)时 inflightPixels() 恒返回 0 —— 这里就是恒等变换。
    {
        double if_x = 0.0;
        double if_y = 0.0;
        esync_tracker_.inflightPixels(if_x, if_y);
        out.esync_inflight_x = static_cast<float>(if_x);
        out.esync_inflight_y = static_cast<float>(if_y);
        err_px_x -= if_x;
        err_px_y -= if_y;
    }

    // 控制器参数直接来自配置, 只有【在途补偿落在哪一域】这一件事要在这里裁决。
    //
    // ★★ 为什么判据是"换算窗 > 0"而不是"档位" ★★
    //   用户把「在途换算窗」打开 = 他要用 AM 的像素域做法, 此时必须把计数域那项
    //   置 0, 否则同一批在途指令被扣两次, 必定发散。
    //   而窗口是 0(默认)时 AM 那条链根本没有在跑(inflightPixels() 恒 0), 这时
    //   计数域那项就是【唯一的在途补偿】, 绝不能顺手把它关掉 —— 实测证据:
    //   beta = 0 不是"关掉一个可选优化", 而是【拆掉主刹车】(400px 甩枪后尾段
    //   几百像素的自持极限环, 60fps 直接发散; 生产点 1.6 在 60/120/240/1000fps
    //   尾段都是 0.295px)。见 CLAUDE.md 的"在途自身位移补偿"要点。
    const bool am_inflight = AimTracker::amTakesOverInflight(in.esync);
    AimPidParams pid_x_cfg = in.pid_x;
    AimPidParams pid_y_cfg = in.pid_y;
    if (am_inflight)
    {
        pid_x_cfg.inflight_beta = 0.0;
        pid_y_cfg.inflight_beta = 0.0;
    }
    pid_x_.configure(pid_x_cfg);
    pid_y_.configure(pid_y_cfg);

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

    // 控制器出口: step() 只吃 (误差, dt), 整数计数只在这里出现一次。
    // 在途自身位移补偿不在这里 —— 它已在上面的误差入口按 AM 的形态(像素域)扣掉了。
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
    // ★ 这里报【实际生效】的值: 换到 AM 的像素域时计数域被判为 0(见上), 报配置里的
    //   原值会让人以为计数域补偿也在跑。
    out.pid_inflight_beta_x = pid_x_cfg.inflight_beta;
    out.pid_inflight_beta_y = pid_y_cfg.inflight_beta;

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
