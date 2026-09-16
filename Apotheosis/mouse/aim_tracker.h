#ifndef MOUSE_AIM_TRACKER_H
#define MOUSE_AIM_TRACKER_H

// ── 目标跟踪器 (PID-EventSync 档专用, mouse/aim_tracker.h) ────────────────────
//
// 这是 **AimMagic 1.0.30 跟踪器 + 预测状态机(FUN_14006e470 / FUN_1400824e0 选靶)
// 的移植**, 对照语料: Downloads\AimMagic_RE_extracted\AimMagic_RE\v1030\。
//
// ★ 它只在 aim_mode == 1 (EventSync 档) 被接线; aim_mode == 0 (默认档) 的链路
//   【一行都不会经过这里】, 行为与本文件存在之前逐位相同。
//
// ── 移植了 AM 的哪六件事 ─────────────────────────────────────────────────────
//   ① 关联: trackId 优先 → 最近邻(平方距离, 不含置信度) → 新建轨迹
//      (AM: FUN_1400824e0 行 680-720 的三级粘滞; 本实现只取前两级 + 新建)
//   ② 生命周期: min_hits 才确认 / max_age 漏帧后删除
//      (AM: FUN_140089ac0 行 448-450, min_hits=3, max_age=5 —— 都是【帧数】,
//       语义无单位歧义, 是 docs/aimmagic-port-spec.md §0.1 里"可以直接照搬"的那类)
//   ③ 速度: 每个 track 维护带【采样窗】的位移/时间累积器 —— 目标速度跨帧持有,
//      而不是每帧从两个框心重建(AM §4.3 的"采样窗"速度)
//   ④ 预测状态机: 系数按 0.1/帧 爬升、-0.2/帧 回落、目标【速度】超过
//      max(30px, 0.75×框宽)×(1+1.5×系数)÷帧间隔 门限才涨 —— 输出 = 系数 × 框心
//      速度 × 尺寸权重, 就地加到瞄点上(AM FUN_14006e470, 常数从脱壳镜像逐个读出:
//      DAT_1401f461c=0.1 / DAT_1401f9a08=0.2 / DAT_1401f4634=1.0 /
//      DAT_1401f9a20=1.5 / DAT_1401f9a0c=0.75 / DAT_1401f8110=30.0)
//   ⑤ **在途自身位移补偿(AM 的发送环窗口)** —— 把"已经发出去、还没生效"的计数
//      求和, 再 ÷ counts_per_pixel 换回像素从误差里扣掉
//      (AM FUN_140067000 行 1172-1209)。见下面 k̂ 的说明。
//   ⑥ **自运动预测项**(AM: 提前量与自身瞄准速度成正比) —— 见下。
//
// ── ★★ 关于 k̂ (每计数像素) —— 2026-09-15 更正 ★★ ──────────────────────────
// 本文件早期版本写着"k̂ 测不准, 所以 ⑤⑥ 不移植"。那个结论**一半是错的**:
//
//   · AM 在 EventSync 档用的 k̂ = `kalman_counts_per_pixel_x/y`, 它是
//     【用户在设置里手填的常量】(默认 1.0, 范围 [0.001, 10000],
//     见 ANALYSIS_v1030.md 行 143-144), **不是测量值**。代码里它只被当作
//     一个带下限的除数用(FUN_140067000 行 1198-1209)。AM **没有任何在线标定**。
//   · 真正测不出来的是**甩枪档**的 `pid_calib`(发已知计数→读光标→反算),
//     那条路依赖单机架构, 与本档无关(comparison §6.7)。
//
// 所以正确做法是**照 AM 一样让用户手填**: `kp_hat_x/y`, 默认 1.0(不做换算)。
// 填错的风险由用户承担 —— 这与 AM 完全一致, 且比"程序猜一个"安全得多。
//
// 全部逻辑是头文件内联(与 trigger_scope.h / aim_scale.h 同风格), 不依赖 Qt/OpenCV
// 的重型部分 —— 逻辑测试可以直接编译它。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace boss
{

// ── 常数: AM 1.0.30 脱壳镜像读出的预测状态机常数 ────────────────────────────
// 系数每帧爬升步长(目标位移超门限时)。AM: DAT_1401f461c = 0.1。
inline constexpr double kPredFactorRiseStep = 0.1;
// 系数每帧回落步长(目标位移不足门限时)。AM: DAT_1401f9a08 = 0.2 (1/0.2=5 帧
// 就能从满值落回 0 —— 回落比爬升快, 目标一停预测立刻收手)。
inline constexpr double kPredFactorFallStep = 0.2;
// 系数上限。AM: DAT_1401f4634 = 1.0 (float 1.0f)。
inline constexpr double kPredFactorMax = 1.0;
// 位移门限的尺寸系数。AM: DAT_1401f9a20 = 1.5 / DAT_1401f9a0c = 0.75。
// ★ 判据(改成【速度域】, 见 predictionLead): 目标速度 >
//   max(kPredGateMinPx, kPredGateSizeRatioAlt × 框宽) × (1 + kPredGateSizeRatio × 系数)
//   ÷ 帧间隔。AM 原文吃的是【本帧位移】(像素), 那个形式只在它的帧率下成立;
//   换成速度域之后, 判据在任何帧率下都还原 AM 的原意(见 docs/eventsync-mode.md §4.3)。
inline constexpr double kPredGateSizeRatio = 1.5;
inline constexpr double kPredGateSizeRatioAlt = 0.75;
// 门限的绝对下限(像素)。AM: DAT_1401f8110 = 30.0 —— 帧率无关的量, 直接照搬。
inline constexpr double kPredGateMinPx = 30.0;
// 计数→像素换算率(k̂)的默认值。AM: kalman_counts_per_pixel_x/y 默认 1.0。
// 1.0 的语义是【不做换算】(1 计数 = 1 像素), 也就是"用户没填就等于不生效"。
inline constexpr double kCountsPerPixelDefault = 1.0;
// k̂ 的合法下限(防除零)。AM: 用 max(k̂, DAT_1401f8000) 夹一次再除
// (FUN_140067000 行 1199-1205)。这里用同一个量级: 0.001。
inline constexpr double kCountsPerPixelMin = 0.001;
inline constexpr double kCountsPerPixelMax = 10000.0;
// 预测生效的目标速度门(像素/秒)。AM: DAT_1401f9aa0 读出 ≈3.51 —— 很低, 只挡
// 完全静止。本方保持同一量级, 但允许配置收高(量化噪声在 60px/s 以下, 见
// aim_predict.h 的实测: 静止目标 v̂ 噪声 p99=46px/s —— 所以默认取 60,
// 与 aim_predict 的默认门一致; 想要 AM 的激进手感可以调回 4)。
inline constexpr double kPredVelFloorDefaultPxS = 60.0;
// 自运动补偿的最大标定增益。AM 吃 runtime+0xC04(自身瞄准速度)乘一个用户系数。
// ★ 这一项在本项目历史上是【删过一次的雷】(predictive_controller, 1a5a792):
//   把一个写死的像素/计数标定增益放进反馈回路内部, 标定不准就在锚点附近
//   每拍反向极限环。所以移植时保留 AM 的形状但加三道夹取(见 selfMotionLead):
//   ① 默认 0(不生效);  ② 幅度上限 ±kSelfMotionMaxGain;  ③ 输出仍走同一个
//   硬像素上限。用户明确要用才开, 且开多大他自己负责。
inline constexpr double kSelfMotionMaxGain = 1.0;
// 自运动项的速度门: "自己是不是真的在甩"。AM: DAT_1401f9aa0 ≈ 3.5057,
// 单位是【像素/帧】—— 所以必须除以帧间隔换算到速度域(帧率无关)。
// ★ 常数照搬 AM(3.5057), 只做量纲转换。
inline constexpr double kPredSelfSpeedGatePxPerFrame = 3.5057;
// 在途窗口的硬上限 = 真实链路死区(46ms, 见 mouse/aim_pid.h 的 kAimDeadTimeS)。
// ★ 这里重复写一遍常数是为了让本头文件【不依赖 aim_pid.h】(逻辑测试只编它一个);
//   两处必须一致, 改一处就要改另一处 —— 有回归盯着(aim_eventsync_test §[6])。
inline constexpr double kInflightWindowMaxS = 0.046;

struct TrackBox
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct AimTrackerParams
{
    // 连续命中多少帧才算"确认的轨迹"(未确认轨迹不作为锁定目标)。
    // AM: FUN_140089ac0 行 448-450 的 min_hits = 3。
    int min_hits = 3;
    // 漏帧多少帧后删除轨迹。AM: 同处 max_age = 5。
    // ★ 本项目检测流本身就是 120fps 的 8.3ms 拍, 5 帧 ≈ 42ms 的滑行窗口。
    int max_age = 5;
    // 关联的最近邻门限(像素, 中心距)。超过视为"不是同一个目标"。
    // AM 的最近邻没有显式门限(总是取最近的), 但 AM 有一层"FOV 内才参与选靶"
    // 的上游过滤; 本实现把这个门限放在关联层, 语义等价且可测。
    double assoc_radius_px = 80.0;
    // IoU 门限: 与锁定轨迹的 IoU >= 此值才算"trackId 优先"命中。
    double assoc_iou = 0.20;
    // 速度采样窗(秒)。窗内位移/窗时间 = 速度(AM §4.3 的采样窗速度)。
    // 0 = 每帧重算(不推荐: 量化噪声在 8.3ms 里就是几百 px/s 的假速度)。
    double vel_window_s = 0.10;
    // 关联的 IoU 门限见下; 预测状态机的"目标在动"判据用 pred_vel_floor。
    // ── 预测状态机参数 (AM 的 QML: prediction_factor_x/y / min_w / max_w) ──
    double pred_factor_x = 0.0;       // 0 = 该轴不预测(AM UI 默认 0)
    double pred_factor_y = 0.0;
    double pred_min_w = 20.0;         // 尺寸权重区间(框宽, 像素)
    double pred_max_w = 80.0;
    double pred_max_lead_px = 12.0;   // 【硬像素上限】(任务书 §4.2)
    double pred_vel_floor = kPredVelFloorDefaultPxS;  // 速度噪声门(px/s)

    // ── ⑤ 计数→像素换算率(k̂)。AM: kalman_counts_per_pixel_x/y, 默认 1.0 ──────
    // ★ 用户手填的常量(**不是测量值** —— AM 也没有在线标定)。
    //   1.0 = 不做换算, 也就是"没填 = 不生效"。填成自己游戏的真实灵敏度即可。
    //   用途只有一处: 把"已发出还没生效的计数"换回像素(见 inflightPixels)。
    double counts_per_pixel_x = kCountsPerPixelDefault;
    double counts_per_pixel_y = kCountsPerPixelDefault;
    // 在途补偿的窗口长度(秒)。AM: mouse_effect_delay_ms, 默认 8ms。
    // ★ 它必须 ≤ 真实链路死区(kAimDeadTimeS = 46ms), 否则会把早已生效的指令
    //   再扣一次 → 正反馈发散(本项目在途补偿的实测结论, 见 aim_pid.h)。
    double inflight_window_s = 0.008;
    // 在途补偿增益(beta)。AM 这里是"计数求和后除以窗口拍数再换算", 没有额外增益;
    // 本实现保留一个倍率, 默认 1.0 = 严格照 AM。开大相当于更激进地扣。
    double inflight_beta = 1.0;

    // ── ⑥ 自运动补偿(AM 的"提前量与自身瞄准速度成正比") ──────────────────────
    // ★ 默认 0 = 关闭。这是本项目删过一次的那类项(输出侧标定增益进反馈回路),
    //   所以默认必须关, 由用户明确开启并承担风险。见 kSelfMotionMaxGain 的说明。
    double self_motion_gain = 0.0;
};

// ── 每个 track 的状态 ────────────────────────────────────────────────────────
struct TrackState
{
    int id = -1;
    TrackBox box{};
    int hits = 0;          // 连续命中数(min_hits 用)
    int age = 0;           // 连续漏帧数(max_age 用)
    bool confirmed = false;

    // 速度采样窗(AM §4.3): 窗内 (位移, 时间) 累积。
    double vel_win_x = 0.0;   // 窗内 X 位移累计(像素)
    double vel_win_y = 0.0;
    double vel_win_t = 0.0;   // 窗内时间累计(秒)
    double vel_x = 0.0;       // 估计的框心速度(像素/秒, 窗排空时结算)
    double vel_y = 0.0;
    bool vel_valid = false;   // 至少结算过一次且时间足够

    // 预测系数 FSM(每 track 一份, AM: track+0x18/0x1C)。
    double pred_k_x = 0.0;    // [0, 1] 爬坡系数
    double pred_k_y = 0.0;
    // 上拍平滑后的预测偏移(方向翻转阻尼用, AM: track+0x20/0x24)。
    double pred_smooth_x = 0.0;
    double pred_smooth_y = 0.0;

    // 本帧是否已被某个候选关联(一帧最多一个观测 —— 关联时用它挡重复)。
    bool observed_this_frame_ = false;
};

// ── 跟踪器 ───────────────────────────────────────────────────────────────────
//
// 用法(每推理帧一次, 即 EventSync 的"事件"):
//   tr.configure(params);
//   tr.beginFrame(dt);
//   for each detection: tr.offer(box, cx, cy);      // 任意顺序
//   tr.endFrame();
//   const TrackState* t = tr.locked();              // 当前锁定轨迹(可空)
//   float lead_x, lead_y; bool active = tr.predictionLead(lead_x, lead_y);
class AimTracker
{
public:
    AimTracker() = default;

    // ⑤ 在途账本里的一格: 那一拍发出去的计数。
    struct SendRec { int dx = 0; int dy = 0; };

    void configure(const AimTrackerParams& p)
    {
        // AM 语义: max(min_hits+1, max_age) 的保护在 AM 源里就有
        // (FUN_14006e470 行 95-101: iVar16 = max(min_hits+1, max_age))。
        params_ = p;
        params_.min_hits = std::max(1, p.min_hits);
        params_.max_age = std::max(params_.min_hits + 1, p.max_age);
        params_.vel_window_s = (p.vel_window_s > 0.0) ? p.vel_window_s : 0.0;
        params_.pred_min_w = (p.pred_min_w > 0.0) ? p.pred_min_w : 0.0;
        // AM: maxW = max(minW + 1, maxW) —— max <= min 时整条预测按"区间外"处理。
        params_.pred_max_w = (p.pred_max_w > params_.pred_min_w)
            ? p.pred_max_w : params_.pred_min_w + 1.0;
        params_.pred_factor_x = clampHalf(p.pred_factor_x);
        params_.pred_factor_y = clampHalf(p.pred_factor_y);
        params_.pred_max_lead_px = (p.pred_max_lead_px > 0.0)
            ? std::min(p.pred_max_lead_px, kAbsMaxLeadPx) : kDefaultMaxLeadPx;
        params_.pred_vel_floor = (p.pred_vel_floor >= 0.0)
            ? p.pred_vel_floor : kPredVelFloorDefaultPxS;
        // k̂: 非法值(<=0 / NaN)一律回落到默认 1.0(= 不换算), 并夹到 AM 的域。
        params_.counts_per_pixel_x = clampK(p.counts_per_pixel_x);
        params_.counts_per_pixel_y = clampK(p.counts_per_pixel_y);
        // 在途窗口: ★ 上限必须是真实链路死区 —— 超过它会把早已生效的指令再扣一次,
        // 正反馈发散(本项目在途补偿的实测结论)。AM 默认 8ms 远在安全区内。
        params_.inflight_window_s = std::clamp(p.inflight_window_s, 0.0, kInflightWindowMaxS);
        params_.inflight_beta = (std::isfinite(p.inflight_beta) && p.inflight_beta >= 0.0)
            ? p.inflight_beta : 1.0;
        // 自运动补偿: AM 那一项没有上限, 本实现给硬夹取(见 kSelfMotionMaxGain)。
        params_.self_motion_gain = (std::isfinite(p.self_motion_gain))
            ? std::clamp(p.self_motion_gain, -kSelfMotionMaxGain, kSelfMotionMaxGain)
            : 0.0;
    }

    void reset()
    {
        tracks_.clear();
        next_id_ = 1;
        locked_id_ = -1;
        dt_ = 0.0;
        lock_taken_this_frame_ = false;
        aim_vx_ = 0.0;
        aim_vy_ = 0.0;
        clearInflight();
    }

    void beginFrame(double dt)
    {
        dt_ = (dt > 0.0 && dt < 0.5) ? dt : (1.0 / 120.0);
        lock_taken_this_frame_ = false;
    }

    // 喂一个候选检测。重复调用表示多个候选; offer 内部做关联。
    // cx/cy 是框心(调用方算好, 避免这里重复算)。
    // 每帧每个 track 只吃第一个关联成功的候选(一帧双观测会把速度窗灌错)。
    void offer(const TrackBox& box, double cx, double cy)
    {
        // ① trackId 优先: 与当前锁定轨迹 IoU >= 阈值 的候选直接继承。
        //   ★ 本帧已经发生过接管时不再改锁 —— 见 ③ 的说明。
        if (locked_id_ >= 0 && !lock_taken_this_frame_)
        {
            TrackState* t = find(locked_id_);
            if (t)
            {
                if (!t->observed_this_frame_
                    && iou(t->box, box) >= params_.assoc_iou)
                {
                    attach(*t, box);
                    locked_id_ = t->id;
                    return;
                }
            }
            else
            {
                locked_id_ = -1;
            }
        }

        // ② 最近邻(中心平方距离, 不含置信度 —— AM 的判据)。
        TrackState* best = nullptr;
        double best_d2 = params_.assoc_radius_px * params_.assoc_radius_px;
        for (TrackState& t : tracks_)
        {
            if (t.observed_this_frame_)
                continue;   // 本帧已经吃过一个观测
            const double dx = box_center_x(t.box) - cx;
            const double dy = box_center_y(t.box) - cy;
            const double d2 = dx * dx + dy * dy;
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best = &t;
            }
        }
        if (best)
        {
            attach(*best, box);
            // ★ 同理: 本帧已经接管过就不再改锁。
            if (!lock_taken_this_frame_)
            {
                locked_id_ = best->id;
                lock_taken_this_frame_ = true;
            }
            return;
        }

        // ③ 新建轨迹。
        //
        // ★ 当前锁定的轨迹与这个候选【关联不上】时, 说明上游选中的是另一个目标
        //   (或旧目标已死), 身份必须换 —— 否则跟踪器会把两个不同目标串成同一个 id,
        //   而"换目标要复位控制器"就永远不会触发。
        //   关联门限 80px 在 120fps 下相当于 9600px/s, 正常横穿不可能误触发。
        TrackState t;
        t.id = next_id_++;
        t.box = box;
        t.hits = 1;
        t.age = 0;
        t.observed_this_frame_ = true;
        t.confirmed = (params_.min_hits <= 1);
        const int new_id = t.id;
        tracks_.push_back(t);
        // ★ 不因为 min_hits 而把锁定推迟 3 帧 —— 上游 selector 已经判定"这就是该瞄的
        //   目标", 跟踪器没有资格再推迟这个决定(docs/aimmagic-port-spec.md §2.3
        //   实测过"加了 min_hits 之后目标刚露头 3 帧内完全锁不上")。
        //   min_hits 只用于 (a) 遥测上的 confirmed 标记, (b) 锁定轨迹死掉之后能不能
        //   自动转移到别的轨迹(见 endFrame)。
        //
        // ★★ 一帧只允许【一次】接管: 上游每帧通常只送一个候选(它就是"该瞄谁"的
        //    决定者), 但接口允许一帧多个。若每个"关联不上"的候选都去抢锁, 结果是
        //    【本帧最后喂进来的那个】赢 —— 而调用方的意图恰恰是"先喂的优先级高"。
        //    实测症状: 两个远隔目标交替出现时, 锁定会莫名其妙地在两者之间跳。
        if (!lock_taken_this_frame_)
        {
            locked_id_ = new_id;
            lock_taken_this_frame_ = true;
        }
    }

    void endFrame()
    {
        // 未被本帧观测命中的轨迹: age+1, 超龄删除; 命中的已在 attach 里清零。
        for (std::size_t i = 0; i < tracks_.size();)
        {
            TrackState& t = tracks_[i];
            if (!t.observed_this_frame_)
            {
                ++t.age;
                t.hits = 0;   // AM 的 min_hits 是"连续命中", 断了就重来
                if (t.age > params_.max_age)
                {
                    if (t.id == locked_id_)
                        locked_id_ = -1;
                    tracks_.erase(tracks_.begin() + static_cast<long>(i));
                    continue;
                }
            }
            ++i;
        }
        for (TrackState& t : tracks_)
            t.observed_this_frame_ = false;

        // 锁定目标转移: 若锁定轨迹被删, 取"最老且已确认"的轨迹续上(不另起炉灶)。
        if (locked_id_ < 0)
        {
            for (const TrackState& t : tracks_)
            {
                if (t.confirmed)
                {
                    locked_id_ = t.id;
                    break;
                }
            }
        }
    }

    // ── ⑤ 在途自身位移补偿 (AM 的发送环窗口, FUN_140067000 行 1172-1209) ──────
    //
    // 调用方每下发一次位移就登一次账; 需要扣的时候调 inflightPixels()。
    //   AM 的做法: 把 `now − mouse_effect_delay_ms` 之后发出的计数求和,
    //   再 ÷ counts_per_pixel 换回【像素】, 从误差里扣掉。
    //   本实现的窗口不用时间戳, 而用【定长环形缓冲】—— 与 aim_pid.h 的计数域
    //   在途补偿同一套做法(窗口 = 固定拍数), 可复现、不依赖时钟抖动。
    void noteSend(int dx, int dy)
    {
        const std::size_t w = inflightWindowTicks();
        if (w == 0)
        {
            // 整条换算链关闭: 不记账, 在途量恒为 0。
            clearInflight();
            return;
        }
        if (send_ring_.size() != w)
            send_ring_.assign(w, SendRec{});
        send_ring_[send_head_] = SendRec{dx, dy};
        send_head_ = (send_head_ + 1) % w;
        if (send_filled_ < w) ++send_filled_;
    }

    // 窗口内"已发出、还没生效"的位移, 单位【像素】(已经 ÷ k̂)。
    // ★ k̂ 默认 1.0 → 这个函数返回的就是"计数当像素"(=不换算), 与关掉本项等价。
    void inflightPixels(double& px_x, double& px_y) const
    {
        px_x = 0.0;
        px_y = 0.0;
        const std::size_t ticks = inflightWindowTicks();
        if (ticks == 0 || send_filled_ == 0)
            return;   // 整条链关闭 / 没发过东西 —— 绝不是除零
        double sx = 0.0, sy = 0.0;
        for (std::size_t i = 0; i < send_filled_; ++i)
        {
            sx += static_cast<double>(send_ring_[i].dx);
            sy += static_cast<double>(send_ring_[i].dy);
        }
        // AM: max(k̂, 下限) 再除(行 1199-1205 的夹取), 防止除零/放大。
        const double kx = std::max(params_.counts_per_pixel_x, kCountsPerPixelMin);
        const double ky = std::max(params_.counts_per_pixel_y, kCountsPerPixelMin);
        // AM 把窗口内计数【求和】后直接除以 k̂ —— 但那份和是"一个延迟窗口内发出去的
        // 总量", 而我们要扣的是"本拍该扣多少"。所以除以窗口拍数, 与 aim_pid.h 的
        // 计数域补偿(u -= beta*N/W)完全同构。★ 不除会导致同一批计数被重复扣 W 次
        // (实测 1000fps 尾段 8.30px, 见 CLAUDE.md 的在途补偿要点②)。
        const double w = static_cast<double>(ticks);
        const double b = params_.inflight_beta;
        px_x = b * (sx / kx) / w;
        px_y = b * (sy / ky) / w;
    }

    // ── ⑥ 自运动补偿 (AM: 提前量与自身瞄准速度成正比) ─────────────────────────
    //
    // aim_vx/aim_vy 是【自己】的瞄准速度(像素/秒, 由调用方从锚点估算)。
    // ★★ 这一项在本项目是【删过一次的雷】(predictive_controller, 1a5a792):
    //    它把标定增益放进反馈回路内部, 标定不准就在锚点附近每拍反向极限环。
    //    所以: 默认 self_motion_gain = 0(不生效); 开了之后仍然走同一个硬像素上限。
    void selfMotionLead(double aim_vx, double aim_vy, double& lead_x, double& lead_y) const
    {
        lead_x = 0.0;
        lead_y = 0.0;
        if (params_.self_motion_gain == 0.0)
            return;
        if (!std::isfinite(aim_vx) || !std::isfinite(aim_vy))
            return;
        // 与目标速度预测共用同一个噪声门: 自身速度也是量化噪声的重灾区
        // (准星抖动在 8.3ms 里就是几百 px/s 的假速度)。
        const double cap = params_.pred_max_lead_px;
        const double g = params_.self_motion_gain;
        lead_x = std::clamp(g * aim_vx, -cap, cap);
        lead_y = std::clamp(g * aim_vy, -cap, cap);
    }

    // 在途窗口对应的拍数。
    // ★ 窗口 <= 0 时返回 0 —— 语义是【整条换算链关闭】(在途量恒为 0)。
    //   这与"窗口至少 1 拍"不冲突: 1 拍 = 只扣本拍, 那是"窗口极小"; 0 拍 =
    //   "这一项不参与"。两者必须能分开, 否则"全关时逐位一致"这条回归就不成立。
    std::size_t inflightWindowTicks() const
    {
        if (!(params_.inflight_window_s > 0.0))
            return 0;
        const double ticks = params_.inflight_window_s / std::max(dt_, 1e-6);
        const auto n = static_cast<std::size_t>(std::max(1.0, std::floor(ticks + 0.5)));
        return std::min<std::size_t>(n, 512);
    }

    // 清空在途账本(换目标/复位时必须调 —— 旧目标的欠账不该算到新目标头上)。
    void clearInflight()
    {
        send_ring_.clear();
        send_head_ = 0;
        send_filled_ = 0;
    }

    // 当前锁定轨迹(可空)。
    //
    // ★ 未确认(hits < min_hits)的锁定轨迹【照样返回】—— docs/aimmagic-port-spec.md
    //   §2.3 的教训: min_hits 若挡住"已锁定轨迹的延续", 会出现"目标刚露头 3 帧内
    //   完全锁不上"的反效果。AM 的三级选靶里 trackId 优先也不看确认数。确认数只
    //   用来决定"锁定目标死了以后, 能不能自动转移到另一条轨迹"(见 endFrame)。
    const TrackState* locked() const
    {
        return find(locked_id_);
    }

    int lockedId() const { return locked_id_; }
    const std::vector<TrackState>& tracks() const { return tracks_; }

    // ── 预测状态机 (AM FUN_14006e470 的移植) ────────────────────────────────
    //
    // 每帧调用一次(必须在锁定轨迹存在时)。返回本拍要加到瞄点上的偏移(像素)。
    //
    // AM 原文的行为(逐项对齐):
    //   sizeWeight = (maxW - w) / (maxW - minW)        区间外为 0
    //   门限       = size * 1.5 + 1  (位移超过它才算"真在动")
    //                否则 size * 0.75 + 1  (保守分支, 门限更低 —— AM 的两个常量
    //                对应的是"先看大门限, 不足再看小门限"的两段式)
    //   超门限     : factor 每帧 +0.1, 到 1.0 为止
    //   未超       : factor 每帧 -0.2, 到 0 为止
    //   偏移       = factor * frameFactor * sizeWeight * v   (frameFactor 本项目
    //                用 factor_x/y 承担 —— AM 的 factor 是用户系数)
    //   方向翻转   : 新偏移与上一拍异号 -> 输出乘 0.02 后走低通平滑(0.02→1)
    //
    // 本实现补的两道安全阀(AM 没有但本项目必须有):
    //   · 硬像素上限 max_lead_px(任务书 §4.2: 提前量必须被钳住);
    //   · 速度噪声门 vel_floor(|v| 低于门限 -> 该轴偏移为 0, 不让量化噪声
    //     被乘成假提前量 —— 实测静止目标 v̂ 噪声 p99=46px/s)。
    bool predictionLead(double& lead_x, double& lead_y)
    {
        lead_x = 0.0;
        lead_y = 0.0;
        TrackState* t = find(locked_id_);
        // ★ 不要求 confirmed —— 与 locked() 同一条理由(port-spec §2.3): min_hits 是
        //   "连续命中数", 它不该成为"目标刚露头 3 帧内预测不工作"的原因。AM 的预测
        //   状态机本身也没有 confirmed 这个概念(它每帧都推进系数)。
        if (!t)
            return false;

        const double w = static_cast<double>(t->box.w);
        const double size_w = sizeWeight(w);
        if (size_w <= 0.0)
        {
            // 区间外: 目标速度那一项不补(权重为 0 ⇒ 偏移恒为 0), 但 FSM 仍然推进到
            // "回落"(AM 的行为: 系数照样按帧演化)。
            stepFactor(t->pred_k_x, false);
            stepFactor(t->pred_k_y, false);
            t->pred_smooth_x = 0.0;
            t->pred_smooth_y = 0.0;
            // ★ 但【自运动项不吃 sizeWeight】—— AM 的自运动项是独立的加法项,
            //   与框大小无关(它预测的是"自己甩枪时画面跟不上")。所以这里不能直接
            //   返回, 否则框一大就自运动补偿也一起没了。
            double sm_x = 0.0, sm_y = 0.0;
            selfMotionLead(aim_vx_, aim_vy_, sm_x, sm_y);
            lead_x = sm_x;
            lead_y = sm_y;
            return (lead_x != 0.0 || lead_y != 0.0);
        }

        // 系数涨还是落 —— "该不该加大预测"。
        //
        // ★★ 逐字对照 AM 的原文(脱壳 FUN_14006e470 行 119-146), 原文是一条 if/else:
        //
        //   条件(涨) =  位移 > max(30, 0.75×size) × (系数×1.5 + 1)      ← ① 目标动得够快
        //            || |自身瞄准速度| <= fVar9(≈3.5 px/帧)              ← ② 自己没在动
        //
        // ★★ 注意 ② 是【或】, 而且是【落】那一支的条件!
        //    原文: if (① <= 位移 || ② <= 自身速度) { 系数 -= 0.2 } else { 系数 += 0.1 }
        //    也就是说: 只有【目标位移够大 且 自己也在动】时系数才涨; 目标没动、
        //    或者自己没在动, 都回落。
        //    这就是它为什么是"自摇领先量" —— 它预测的是【自己甩枪时画面跟不上】的那部分。
        //
        // ★★ 判据改成【速度域】(2026-09-15, 用户要求) ★★
        // 原文的 ① 吃的是【本帧位移】(像素)。那个形式只在 AM 自己的帧率下成立 ——
        // 它隐含假设"相邻推理帧之间的位移"与框宽同量级。本项目 120fps 下 300px/s 的
        // 目标每帧只走 2.5px, 而 60px 宽的框门限是 45~112px, 差 20~45 倍, 门限恒不
        // 成立 ⇒ 系数永远爬不上去 ⇒ 预测被**静默关掉**。
        // 换成速度域后同一个判据在任何帧率下都还原 AM 的原意: 把"每帧位移"换成
        // "每秒位移"(÷帧间隔), 三个常数(30 / 0.75 / 1.5)一个都没改。
        const double dt_safe = std::max(dt_, 1e-6);
        const double size_ref = std::max(kPredGateMinPx, kPredGateSizeRatioAlt * w);
        // ① 目标速度 > max(30, 0.75×w) × (1 + 1.5×系数) ÷ 帧间隔
        const double gate_v = size_ref * (1.0 + kPredGateSizeRatio * t->pred_k_x) / dt_safe;
        const double speed = std::hypot(t->vel_x, t->vel_y);
        const bool target_moving = t->vel_valid && speed > gate_v;
        // ② 自身瞄准速度 <= fVar9(换算到速度域: kPredSelfSpeedGatePxPerFrame ÷ dt)
        //    ★ 没接自运动项(aim_vx/vy = 0)时, ②恒成立 ⇒ 系数只落不涨。
        //      这正是 AM 的行为, 也是为什么【必须】把自身速度接进来才有预测 ——
        //      只给目标速度而不给自身速度, 这一档的预测在当前实现下永远不会启动。
        const double self_speed = std::hypot(aim_vx_, aim_vy_);
        const bool self_still = self_speed <= kPredSelfSpeedGatePxPerFrame / dt_safe;
        const bool rising = target_moving && !self_still;

        stepFactor(t->pred_k_x, rising);
        stepFactor(t->pred_k_y, rising);

        // 每轴: 偏移 = 系数 × 用户系数 × sizeWeight × v, 带噪声门 + 硬上限。
        lead_x = axisLead(t->pred_k_x, t->pred_smooth_x, params_.pred_factor_x,
                          t->vel_x, params_.pred_vel_floor,
                          params_.pred_max_lead_px * size_w);
        lead_y = axisLead(t->pred_k_y, t->pred_smooth_y, params_.pred_factor_y,
                          t->vel_y, params_.pred_vel_floor,
                          params_.pred_max_lead_px * size_w);

        // ⑥ 自运动项: AM 的预测偏移里还有一项吃【自身瞄准速度】(runtime+0xC04)。
        //    ★ 它按【自身】速度定标, 与上面按【目标】速度的那项相加。
        //    ★ 默认 gain = 0 ⇒ 这一项恒为 0, 也就是不生效 —— 见 kSelfMotionMaxGain 的
        //      说明(这类项在本项目被删过一次, 必须由用户明确开启)。
        double sm_x = 0.0, sm_y = 0.0;
        selfMotionLead(aim_vx_, aim_vy_, sm_x, sm_y);
        // 硬上限必须【两项相加之后】再夹一次 —— 否则同时开两项可以突破上限,
        // 违反任务书 §4.2 ②。
        lead_x = std::clamp(lead_x + sm_x, -params_.pred_max_lead_px, params_.pred_max_lead_px);
        lead_y = std::clamp(lead_y + sm_y, -params_.pred_max_lead_px, params_.pred_max_lead_px);
        return (lead_x != 0.0 || lead_y != 0.0);
    }

    // ⑥ 每拍告诉跟踪器"自己现在瞄多快"(像素/秒)。用于自运动项与它的涨落门限。
    //
    // ★ 必须每拍调(即使自运动项关着) —— 因为 AM 的系数涨落【第二个条件】就是
    //   自身速度: 自己没在动时, 系数只落不涨。不喂它 = 自己永远算"没在动"
    //   = 预测永远不启动(见 predictionLead 里的 ②)。
    void setAimVelocity(double vx, double vy)
    {
        aim_vx_ = std::isfinite(vx) ? vx : 0.0;
        aim_vy_ = std::isfinite(vy) ? vy : 0.0;
    }

    double sizeWeight(double box_w) const
    {
        const double lo = params_.pred_min_w;
        const double hi = params_.pred_max_w;
        if (hi <= lo)
            return 0.0;
        if (box_w <= lo || box_w >= hi)
            return 0.0;
        return (hi - box_w) / (hi - lo);
    }

    int nextTrackId() const { return next_id_; }

private:
    static double clampHalf(double v)
    {
        if (!std::isfinite(v)) return 0.0;
        return std::clamp(v, -0.2, 0.2);   // 与 aim_predict 同范围(§4.2 ①)
    }

    // k̂ 的夹取: 非有限 / 非正值一律回落到默认 1.0(= 不做换算)。
    static double clampK(double v)
    {
        if (!std::isfinite(v) || v <= 0.0) return kCountsPerPixelDefault;
        return std::clamp(v, kCountsPerPixelMin, kCountsPerPixelMax);
    }

    static double box_center_x(const TrackBox& b) { return b.x + b.w * 0.5; }
    static double box_center_y(const TrackBox& b) { return b.y + b.h * 0.5; }

    static double iou(const TrackBox& a, const TrackBox& b)
    {
        const double x1 = std::max<double>(a.x, b.x);
        const double y1 = std::max<double>(a.y, b.y);
        const double x2 = std::min<double>(a.x + a.w, b.x + b.w);
        const double y2 = std::min<double>(a.y + a.h, b.y + b.h);
        const double iw = std::max(0.0, x2 - x1);
        const double ih = std::max(0.0, y2 - y1);
        const double inter = iw * ih;
        const double uni = static_cast<double>(a.w) * a.h
                         + static_cast<double>(b.w) * b.h - inter;
        return uni > 0.0 ? inter / uni : 0.0;
    }

    TrackState* find(int id)
    {
        for (TrackState& t : tracks_)
            if (t.id == id) return &t;
        return nullptr;
    }
    const TrackState* find(int id) const
    {
        for (const TrackState& t : tracks_)
            if (t.id == id) return &t;
        return nullptr;
    }

    // 把一个候选检测接到已有轨迹上: 更新框、清 age、hits+1、推进速度窗。
    void attach(TrackState& t, const TrackBox& box)
    {
        const double old_cx = box_center_x(t.box);
        const double old_cy = box_center_y(t.box);
        t.box = box;
        t.age = 0;
        t.observed_this_frame_ = true;
        if (!t.confirmed)
        {
            ++t.hits;
            if (t.hits >= params_.min_hits)
                t.confirmed = true;
        }

        if (params_.vel_window_s > 0.0)
        {
            // 采样窗速度: 窗内累计位移与时间, 排空时结算一次平均速度。
            // (AM §4.3: 速度跨帧持有, 不是每帧从相邻两框心重建。)
            t.vel_win_x += (box_center_x(box) - old_cx);
            t.vel_win_y += (box_center_y(box) - old_cy);
            t.vel_win_t += dt_;
            if (t.vel_win_t >= params_.vel_window_s)
            {
                t.vel_x = t.vel_win_x / t.vel_win_t;
                t.vel_y = t.vel_win_y / t.vel_win_t;
                t.vel_win_x = 0.0;
                t.vel_win_y = 0.0;
                t.vel_win_t = 0.0;
                t.vel_valid = true;
            }
        }
        else
        {
            // 每帧重算(dt=0 时退化为 0 速度 —— 不产生 NaN)。
            t.vel_x = (dt_ > 0.0) ? (box_center_x(box) - old_cx) / dt_ : 0.0;
            t.vel_y = (dt_ > 0.0) ? (box_center_y(box) - old_cy) / dt_ : 0.0;
            t.vel_valid = (dt_ > 0.0);
        }
    }

    static void stepFactor(double& k, bool rising)
    {
        if (rising)
            k = std::min(kPredFactorMax, k + kPredFactorRiseStep);
        else
            k = std::max(0.0, k - kPredFactorFallStep);
    }

    // 单轴偏移计算: 系数 × 用户系数 × v, 噪声门 + 方向翻转平滑 + 硬上限。
    static double axisLead(double fsm_k, double& smooth, double user_factor,
                           double vel, double vel_floor, double max_lead)
    {
        double lead = fsm_k * user_factor * vel;
        // 速度噪声门: |v| 太低时这一轴不补(挡量化噪声)。
        if (std::abs(vel) < vel_floor)
            lead = 0.0;
        // 方向翻转阻尼(AM: track+0x20 的低通, 翻转时 0.02):
        const double blend = (lead * smooth < 0.0 && smooth != 0.0) ? 0.02 : 1.0;
        smooth = (1.0 - blend) * smooth + blend * lead;
        // 硬像素上限(任务书 §4.2 ②)。
        return std::clamp(smooth, -max_lead, max_lead);
    }

    AimTrackerParams params_{};
    std::vector<TrackState> tracks_;
    int next_id_ = 1;
    int locked_id_ = -1;
    double dt_ = 0.0;
    // 本帧是否已经定过锁定身份(一帧只认第一次, 见 offer ③)。
    bool lock_taken_this_frame_ = false;

    // ⑤ 在途账本: 定长环形缓冲, 每格是"那一拍发出去的计数"。
    std::vector<SendRec> send_ring_{};
    std::size_t send_head_ = 0;
    std::size_t send_filled_ = 0;

    // ⑥ 自身瞄准速度(像素/秒), 由 setAimVelocity 每拍喂。
    double aim_vx_ = 0.0;
    double aim_vy_ = 0.0;

public:
    // 提前量硬上限的绝对天花板与默认值 —— 与 aim_predict.h 保持一致。
    static constexpr double kDefaultMaxLeadPx = 12.0;
    static constexpr double kAbsMaxLeadPx = 64.0;
};

}  // namespace boss

#endif  // MOUSE_AIM_TRACKER_H
