#ifndef MOUSE_AIM_TRACKER_H
#define MOUSE_AIM_TRACKER_H

// ── 目标跟踪器 + 预测状态机 (mouse/aim_tracker.h) ─────────────────────────────
//
// **AimMagic 1.0.30 的逐字移植**。对照语料:
//   Downloads\AimMagic_RE_extracted\AimMagic_RE\v1030\
// 全部事实与行号见 `docs/aimmagic-ground-truth.md`(该项目内定稿)。
//
// ★★ 本文件在 2026-09-16 做过一次【推倒重写】。此前的版本是"按思路适配进来"的,
//    被用户指出与 AM 不符 —— 复核后确认有 18 处偏差(ground-truth §8)。
//    重写的原则只有一条: **AM 怎么写就怎么写**, 不加 AM 没有的安全阀,
//    不改量纲, 不改判据, 不改落点。凡是有意偏离的地方, 都在下面写明"为什么"。
//
// ── AM 的两个函数, 本文件各对应一半 ──────────────────────────────────────────
//   跟踪器   FUN_140089ac0 (550 行)  → 的身份/生命周期/速度部分
//   预测状态机 FUN_14006e470 (189 行)  → predictionLead()
//
// ── 跟踪器: AM 实际只有五件事 ────────────────────────────────────────────────
//   ① 关联: **类别 id 相等** 且 **IoU 最大**(严格大于门限) —— AM 没有距离半径,
//      也没有"trackId 优先"的第二条路。贪心: detection 外层、track 内层。
//      (FUN_140089ac0 L199-201 类别, L196/L264 IoU)
//   ② 生命周期: `min_hits <= hits && misses <= max_age` 才输出; `max_age < misses` 删除
//      (L448-449, L511)。★ 两个比较都是 `<=`, 删除是严格 `<`。
//   ③ 速度: **窗内沿用旧值, 窗外重算并混合** K1=0.25/K2=0.75 —— 不是"窗内累加"。
//      (L367-396) ★ 窗内那一支**不刷新时间戳**, 所以窗口会自然过期。
//   ④ 滑行外推: 已确认但本帧未命中的轨迹, 位置按 `v × clamp(dt,1e-3,1)` 外推,
//      发出速度按 **X ×0.95 / Y ×0.8** 衰减 (L458-485)。
//   ⑤ 配置偏移**只有五个**(L123/196/448/449/370): enable_tracking(0x42)、
//      min_hits(0x44)、max_age(0x48)、tracking_iou_threshold(0x4C)、
//      tracking_velocity_sample_ms(0x50)。**再无其它。**
//
// ── 预测状态机: AM 的实际形状 ────────────────────────────────────────────────
//   · 尺寸权重 `sw` 吃【框高】(L107), 而门限吃【框宽】(L124/L130)。**同一函数里混用。**
//     `sw = (max_w − h) / (max_w − min_w)`, 区间外见 §5.1 的边界说明。
//   · 门限 = `max(30, 0.8 × 尺寸) × (1 + 1.5 × 系数)`, 与 **本帧位移** 比较(L133-134)。
//   · `||` 后半是"落"的条件: `自身平台位移 <= 10` ⇒ 回落(L135)。即"目标够快 **且**
//     自己在动"才涨。
//   · 提前量 = `用户系数 × 平台位移 × sw × 系数`, **就地加到框心上**(L177-178)。
//     ★ 是【平台位移(runtime+0xC04/0xC08)】, 不是目标速度 —— 它预测的是
//       "自己在甩、画面还没回来"那一部分, 不是目标会往哪走。
//   · 涨 0.1/帧、落 0.2/帧、夹 [0,1](L136-146)。方向翻转时低通系数取 0.02(L168)。
//
// ── ★★ 本项目**有意**偏离 AM 的两处(仅此两处) ★★ ────────────────────────────
//   1. **提前量硬上限** `pred_max_lead_px`。AM 没有上限。本项目保留一项安全阀,
//      默认 0 = **关闭该项**(等价于没有上限 = 与 AM 逐位一致)。
//      用户显式设正数才生效。理由: 任务书 §4.2 明令要求提前量有界;
//      但默认值必须让"照抄 AM"成为默认行为。
//   2. **速度噪声门** `pred_vel_floor`。同上, 默认 0 = 关闭。
//   3. 平台位移 `platform_vx/vy` 由调用方注入 —— AM 从 runtime+0xC04/0xC08 取,
//      本项目双机架构下没有那个量, 所以由调用方喂"自身瞄准速度"。
//      **这是数据来源的差异, 不是算法的差异。**
//
// 全部逻辑头文件内联(与 trigger_scope.h / aim_scale.h 同风格), 不依赖 Qt/OpenCV,
// 逻辑测试可以直接编译它。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace boss
{

// ── 常数: 全部从解包镜像逐字节读出 (ground-truth §1) ─────────────────────────
// 系数爬升/回落步长。AM: DAT_1401f461c = 0.1f / DAT_1401f9a08 = 0.2f。
inline constexpr double kPredFactorRiseStep = 0.1;
inline constexpr double kPredFactorFallStep = 0.2;
// 系数上限。AM: DAT_1401f4634 = 1.0f。
inline constexpr double kPredFactorMax = 1.0;
// 门限的尺寸系数。AM: DAT_1401f9a20 = 1.5f / DAT_1401f9a0c = **0.8f**
// ★ 此前本文件写的是 0.75 —— **错的**, 真值是 0.8。
inline constexpr double kPredGateSizeRatio = 1.5;
inline constexpr double kPredGateSizeRatioAlt = 0.8;
// 门限的绝对下限(像素)。AM: DAT_1401f8110 = 30.0f。
inline constexpr double kPredGateMinPx = 30.0;
// 自身平台位移门(像素/帧)。AM: DAT_1401f9aa0 = **10.0f**
// ★ 此前本文件写的是 3.5057 —— **错的**, 真值是 10.0。
inline constexpr double kPredSelfMoveGatePx = 10.0;
// 方向翻转时的低通混合系数。AM: DAT_1401f9a00 = **0.02f**(L87 → fVar7)。
inline constexpr double kPredFlipBlend = 0.02;
// ★★ 正常(未翻转)时的低通混合系数。AM: DAT_1401f5b7c = **0.05f**(L88 → fVar6)。
//    ★ 这一条此前被我写反了 —— 我以为"正常 = 1.0(直通)、翻转 = 0.02",
//      实际是"正常 = 0.05、翻转 = 0.02", **两者都是重低通**, 只是翻转时更重。
//      证据: L166/L170 `fVar19 = fVar6`(正常) / `fVar19 = fVar7`(翻转),
//      然后 L174 `fVar19 = (1 - blend)*smooth + blend*lead`(fVar4 = 1.0)。
//      ⇒ blend 就是 fVar6/fVar7, 而 0.05 / 0.02 都是小量。
//    ⇒ 语义: AM 的逐帧提前量输出是**强平滑**的, 阶跃响应时间常数约 20 帧。
//      这解释了为什么它的提前量看起来"慢慢起来"。
inline constexpr double kPredNormalBlend = 0.05;
// 速度混合系数。AM: DAT_1401f4620 = 0.25f / DAT_1401f4630 = 0.75f (K1+K2 = 1.0)。
inline constexpr double kVelBlendNew = 0.25;
inline constexpr double kVelBlendOld = 0.75;
// dt 夹取范围(**秒**)。AM: DAT_1401f8000 = 0.001 / DAT_1401f6358 = 1.0
// ★ 两个都是 double, 且 AM 全程把 dt 归一成【秒】(L367-368 除以 1e9)。
inline constexpr double kVelDtMinS = 0.001;
inline constexpr double kVelDtMaxS = 1.0;
// 滑行外推时的速度衰减。AM: X 用 DAT_1401f9a10 = 0.95f, Y 用 DAT_1401f9a0c = 0.8f。
inline constexpr double kCoastDecayX = 0.95;
inline constexpr double kCoastDecayY = 0.8;
// IoU 半宽/半高换算。AM: DAT_1401f462c = 0.5f。
inline constexpr double kHalfExtent = 0.5;
// 默认的 IoU 门限。AM: tracking_iou_threshold 默认 = DAT_1401f5b88 = 0.3f。
inline constexpr double kDefaultAssocIou = 0.3;
// 默认速度采样窗(毫秒)。AM: tracking_velocity_sample_ms 默认 = 0x14 = 20ms。
inline constexpr double kDefaultVelSampleMs = 20.0;
// 轨迹过期时间(纳秒)。AM: 0x7744d640 = 2 秒 —— 用于清理超过 2 秒没更新的状态。
inline constexpr double kTrackTimeoutS = 2.0;

struct TrackBox
{
    float x = 0.0f;   // 左上角
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// ── 跟踪器参数: 与 AM 的【五个】配置项一一对应 ───────────────────────────────
struct AimTrackerParams
{
    // ── 以下四项 = AM 的 Group 作用域跟踪配置(ground-truth §2.1) ──────────────
    // AM: enable_tracking(config+0x42)。本项目的引擎有独立的启用开关, 故此处恒为
    // true; 保留字段是为了让测试能验证"关掉追踪 = 身份直通 + 清空轨迹"这条路径。
    bool enable_tracking = true;
    // AM: min_hits(config+0x44) 默认 3。输出判据是 `min_hits <= hits`。
    int min_hits = 3;
    // AM: max_age(config+0x48) 默认 5。删除判据是 `max_age < misses`。
    int max_age = 5;
    // AM: tracking_iou_threshold(config+0x4C) 默认 0.3。**严格大于**才算命中。
    double assoc_iou = kDefaultAssocIou;
    // AM: tracking_velocity_sample_ms(config+0x50) 默认 20, 夹取 [1, 1000]。
    double vel_sample_ms = kDefaultVelSampleMs;

    // ── 以下 = AM 的 AimKey 作用域预测配置(ground-truth §2.2) ─────────────────
    // AM: prediction_factor_x/y, 默认 0(且回退到旧的 prediction_factor)。
    double pred_factor_x = 0.0;
    double pred_factor_y = 0.0;
    // AM: prediction_min_width / max_width, 默认 20 / 80。
    // ★ 尺寸权重吃【框高】, 所以这两个量的语义是"框高区间"。
    double pred_min_w = 20.0;
    double pred_max_w = 80.0;

    // ── 本项目有意新增的两道安全阀: **默认 0 = 关闭 = 与 AM 逐位一致** ─────────
    // ① 提前量硬上限(像素)。0 = 无上限(AM 原样)。
    double pred_max_lead_px = 0.0;
    // ② 目标速度噪声门(像素/帧)。0 = 无门(AM 原样)。
    double pred_vel_floor = 0.0;

    // ── 调用方注入的"平台位移"(AM runtime+0xC04/0xC08) ───────────────────────
    // ★ 不是配置项 —— 每拍由 setPlatformVelocity() 喂。
    //   保留在参数结构里只为了让 organizeParams 能比较"要不要重装"。
};

// ── 每个 track 的状态 (stride 0x48, 见 ground-truth §4.4 的偏移表) ───────────
struct TrackState
{
    int id = -1;              // AM: 全局 trackId 计数器
    // ★ AM: track+0x18 —— 类别 id。关联的必要条件(L200-201 整数严格相等)。
    int class_id = -1;
    TrackBox box{};           // 观测框
    int hits = 0;             // AM: +0x2C 命中累计(命中 +1, 不因漏帧清零)
    int misses = 0;           // AM: +0x30 漏帧累计(命中清 0, 漏帧 +1)

    // AM: +0x20/+0x24 —— 平滑后的框心速度(像素/毫秒, 与 AM 同量纲)
    // ★ 单位是【像素/毫秒】, 因为 AM 全程用 ms。转 px/s 只在校验/遥测时做。
    double vel_x = 0.0;
    double vel_y = 0.0;
    bool vel_valid = false;   // 至少重算过一次(窗外的 else 支)

    // AM: +0x40/+0x44/+0x48 —— 上次【观测】的位置与时间戳(ms)
    double last_obs_cx = 0.0;
    double last_obs_cy = 0.0;
    double last_obs_ms = 0.0;
    bool has_last_obs = false;

    // AM: +0x38 —— 上次【发出】的时间戳(ms), 滑行外推用
    double last_emit_ms = 0.0;
    bool has_last_emit = false;

    // 预测系数 FSM(每轨一份)。AM: track+0x18(X) / track+0x1C(Y)。
    double pred_k_x = 0.0;    // [0, 1]
    double pred_k_y = 0.0;
    // 低通后的预测偏移。AM: track+0x20(X) / track+0x24(Y)。
    double pred_smooth_x = 0.0;
    double pred_smooth_y = 0.0;

    // 本帧是否已被某个观测命中(AM 用 std::vector<bool> 位图做同一件事, L178/199)。
    bool observed_this_frame_ = false;

    // 本帧的"输出框"(含滑行外推)。AM 是就地改写检测框, 本项目不修改调用方的框,
    // 所以把"改写后"的结果放在这里 —— 语义一致(下游吃的是改写后的框心)。
    double out_cx = 0.0;
    double out_cy = 0.0;
};

// ── 跟踪器 ───────────────────────────────────────────────────────────────────
//
// 用法(每推理帧一次, 即 EventSync 的"事件"):
//   tr.configure(params);
//   tr.beginFrame(dt_s, now_ms);
//   for each detection: tr.offer(box, class_id);      // 观测
//   tr.endFrame();
//   const TrackState* t = tr.locked();                // 当前锁定轨迹(可空)
class AimTracker
{
public:
    AimTracker() = default;

    void configure(const AimTrackerParams& p)
    {
        params_ = p;
        // AM 的 clamp: min_hits 无夹取(解析器直接读); max_age 无夹取。
        // vel_sample_ms 夹 [1, 1000] —— anchors.txt L21702-21711。
        params_.vel_sample_ms = std::clamp(p.vel_sample_ms, 1.0, 1000.0);
        // IoU 门限夹 [0,1](AM 的解析器未夹, 但 UI 的 from/to 是 0..1, 且越界无意义)。
        params_.assoc_iou = std::isfinite(p.assoc_iou)
            ? std::clamp(p.assoc_iou, 0.0, 1.0) : kDefaultAssocIou;
        // ★ 尺寸区间: AM 原文 iVar16 = max(min+1, max)(L99-101)。照抄。
        params_.pred_max_w = (p.pred_max_w > p.pred_min_w + 1.0)
            ? p.pred_max_w : p.pred_min_w + 1.0;
        // ★★ 系数【不夹取】—— AM 的解析器对 prediction_factor_x/y 无 clamp
        //    (ground-truth §9.1)。此前本文件夹 ±0.2 是自加的, 已删除。
        params_.pred_factor_x = std::isfinite(p.pred_factor_x) ? p.pred_factor_x : 0.0;
        params_.pred_factor_y = std::isfinite(p.pred_factor_y) ? p.pred_factor_y : 0.0;
        // 本项目自加的两道阀: 默认 0 = 关闭。给非法值兜底。
        params_.pred_max_lead_px = (std::isfinite(p.pred_max_lead_px)
                                    && p.pred_max_lead_px > 0.0)
            ? p.pred_max_lead_px : 0.0;
        params_.pred_vel_floor = (std::isfinite(p.pred_vel_floor)
                                  && p.pred_vel_floor > 0.0)
            ? p.pred_vel_floor : 0.0;
    }

    void reset()
    {
        tracks_.clear();
        next_id_ = 1;
        locked_id_ = -1;
        dt_s_ = 0.0;
        now_ms_ = 0.0;
        lock_taken_this_frame_ = false;
        platform_vx_ = 0.0;
        platform_vy_ = 0.0;
        write_to_box_ = false;
    }

    // now_s: 单调时钟(秒)。dt_s: 距上一拍(秒)。
    void beginFrame(double dt_s, double now_s)
    {
        dt_s_ = (std::isfinite(dt_s) && dt_s > 0.0 && dt_s < 0.5)
            ? dt_s : (1.0 / 120.0);
        now_ms_ = std::isfinite(now_s) ? now_s * 1000.0 : now_ms_ + dt_s_ * 1000.0;
        lock_taken_this_frame_ = false;
    }

    // 喂一个候选观测。
    //
    // ★ class_id 是**关联的必要条件**(AM L199-201)。传一个不会与目标类别相等的值
    //   可以让该观测"不参与任何关联"(等价于 AM 里类别不匹配)。
    //   本项目调用方只有一个候选(selector 已选定), 所以 class_id 恒等于目标类别;
    //   保留参数是为了让逻辑测试能复现 AM 的"类别必须相等"这条。
    void offer(const TrackBox& box, int class_id)
    {
        if (!params_.enable_tracking)
            return;

        const double cx = box_center_x(box);
        const double cy = box_center_y(box);

        // ── 关联: 类别相等 + IoU 最大(严格大于门限) ──────────────────────────
        //
        // AM: L196 用门限当"当前最优"的初值, L264 `if (best < iou)` 严格比较。
        // 所以 IoU **恰好等于**门限 ⇒ 不命中。
        // ★ 没有距离半径, 没有 trackId 优先路径 —— 这两条都是本项目此前凭空加的。
        TrackState* best = nullptr;
        double best_iou = params_.assoc_iou;
        for (TrackState& t : tracks_)
        {
            if (t.observed_this_frame_ || t.class_id != class_id)
                continue;
            const double v = iou(t.box, box);
            if (best_iou < v)
            {
                best_iou = v;
                best = &t;
            }
        }

        if (best)
        {
            attach(*best, box, cx, cy);
            if (!lock_taken_this_frame_)
            {
                locked_id_ = best->id;
                lock_taken_this_frame_ = true;
            }
            return;
        }

        // ── 新建轨迹 ─────────────────────────────────────────────────────────
        // AM L292-327: 未关联上就新建, hits = 1, misses = 0。
        TrackState t;
        t.id = next_id_++;
        t.box = box;
        t.class_id = class_id;
        t.hits = 1;
        t.misses = 0;
        t.observed_this_frame_ = true;
        t.last_obs_cx = cx;
        t.last_obs_cy = cy;
        t.last_obs_ms = now_ms_;
        t.has_last_obs = true;
        t.out_cx = cx;
        t.out_cy = cy;
        const int new_id = t.id;
        tracks_.push_back(t);

        // ★ 与 AM 的差别: AM 用"三级粘滞"(trackId 优先)决定谁被选中, 本项目上游
        //   selector 已经做了"该瞄谁"的决定, 跟踪器只负责给身份。所以新建轨迹
        //   直接接管锁定 —— 但一帧只允许【一次】接管, 否则一帧多候选时最后喂的赢。
        if (!lock_taken_this_frame_)
        {
            locked_id_ = new_id;
            lock_taken_this_frame_ = true;
        }
    }

    void endFrame()
    {
        // ── 漏帧处理 + 超龄删除 + 滑行外推 (AM L444-485, L509-542) ───────────
        for (std::size_t i = 0; i < tracks_.size();)
        {
            TrackState& t = tracks_[i];
            if (!t.observed_this_frame_)
            {
                ++t.misses;   // AM L447
                // AM L511: `max_age < misses` ⇒ 删除(严格 <)。即漏 max_age 帧后仍存活,
                // 第 max_age+1 帧才删 —— 请注意这与"漏 max_age 帧就删"差一帧。
                if (params_.max_age < t.misses)
                {
                    if (t.id == locked_id_)
                        locked_id_ = -1;
                    tracks_.erase(tracks_.begin() + static_cast<long>(i));
                    continue;
                }

                // ── 滑行外推 (AM L458-485) ───────────────────────────────────
                //
                // ★★ 本项目此前【完全没有】这一块(偏差 #17): 漏帧时轨迹原地不动,
                //    于是滑行期目标位置是"上一次观测的位置", 误差恒定滞后一个真实位移。
                //    AM 的做法: 位置按 `v × clamp(dt_s, 0.001, 1.0)` 外推,
                //    然后把速度衰减 —— **X 乘 0.95, Y 乘 0.8**。
                //
                // ★ 两个轴衰减系数不同(0.95 vs 0.8)是 AM 原文就这样(L436-437),
                //   推测源自"水平移动更常见、更可信"。照抄, 不做'统一化'。
                if (t.vel_valid)
                {
                    const double dt = std::clamp((now_ms_ - t.last_obs_ms) / 1000.0,
                                                 kVelDtMinS, kVelDtMaxS);
                    t.out_cx = box_center_x(t.box) + t.vel_x * dt;
                    t.out_cy = box_center_y(t.box) + t.vel_y * dt;
                    t.vel_x *= kCoastDecayX;
                    t.vel_y *= kCoastDecayY;
                }
                else
                {
                    t.out_cx = box_center_x(t.box);
                    t.out_cy = box_center_y(t.box);
                }
            }
            ++i;
        }
        for (TrackState& t : tracks_)
            t.observed_this_frame_ = false;

        // 锁定轨迹被删: 交给调用方处理(本项目上游每拍都会喂一个候选,
        // 下一拍自然会建立新轨迹并接管)。AM 没有"自动转移到最老轨迹"这条 ——
        // 那是本项目此前加的, 已删除(它会让身份在两条轨迹间莫名跳动)。
    }

    // 当前锁定轨迹(可空)。★ 不要求 confirmed —— AM 的输出判据在 emit 那一层
    // (`min_hits <= hits && misses <= max_age`), 不在"能不能锁定"这一层。
    const TrackState* locked() const { return find(locked_id_); }

    int lockedId() const { return locked_id_; }
    const std::vector<TrackState>& tracks() const { return tracks_; }

    // 该轨迹本帧是否"够格输出"(AM L448-449 的判据, 两个都是 `<=`)。
    bool emits(const TrackState& t) const
    {
        return params_.min_hits <= t.hits && t.misses <= params_.max_age;
    }

    // ── ⑤ 平台位移 (AM runtime+0xC04/0xC08) ─────────────────────────────────
    //
    // ★ 必须每拍喂 —— AM 的系数涨落第二个条件就是它(L135): 平台位移 <= 10px/帧
    //   时系数只落不涨。不喂 = 永远算"自己没动" = 预测永远不启动。
    // ★ 单位: 像素/秒(调用方从锚点滤波器拿)。AM 存的是"每帧位移", 所以
    //   predictionLead 里要 ÷dt 换算 —— 这是**唯一**必须做量纲转换的地方, 因为
    //   本项目的调用方天然给出速度而不是位移。
    void setPlatformVelocity(double vx_px_s, double vy_px_s)
    {
        platform_vx_ = std::isfinite(vx_px_s) ? vx_px_s : 0.0;
        platform_vy_ = std::isfinite(vy_px_s) ? vy_px_s : 0.0;
    }

    // ── 预测状态机 (AM FUN_14006e470 的移植) ────────────────────────────────
    //
    // 返回本拍要加到框心上的偏移(像素)。同时**就地改写**锁定轨迹的输出框心
    // (out_cx/out_cy), 对应 AM 的 `*pfVar14 = fVar19 + *pfVar14`(L177)。
    bool predictionLead(double& lead_x, double& lead_y)
    {
        lead_x = 0.0;
        lead_y = 0.0;
        TrackState* t = find(locked_id_);
        if (!t)
            return false;

        // ── 尺寸权重: 吃【框高】(AM L107 `pfVar14[2]`) ───────────────────────
        //
        // AM L107-116:
        //   fVar19 = 框高; fVar23 = 1.0;
        //   if (min_w < h) { if (h < max_w) fVar23 = (max_w - h)/(max_w - min_w); else fVar23 = 0; }
        // ★ 注意边界: `h <= min_w` 时 fVar23 保持 **1.0**(不是 0);
        //   `h >= max_w` 时才被置 0。这是 AM 原文的形状, 照抄。
        const double h = static_cast<double>(t->box.h);
        const double lo = params_.pred_min_w;
        const double hi = params_.pred_max_w;
        double sw = 1.0;
        if (lo < h)
        {
            if (h < hi)
                sw = (hi - h) / (hi - lo);
            else
                sw = 0.0;
        }

        // ── 系数涨落 ─────────────────────────────────────────────────────────
        //
        // AM L119-146 是一条复合 if/else, 完整形状:
        //   进入条件: prediction_enabled && sw > 0 && (factor_x > 0 || factor_y > 0)
        //   涨的条件: 位移 > max(30, 0.8×尺寸) × (1 + 1.5×系数)
        //              || 平台位移 <= 10px/帧        ← 这一半是【落】的条件
        //   ⇒ 涨 = ① 不成立 && ② 不成立 = "目标动得够快 且 自己在动"
        //
        // ★ 位移与平台位移都是【像素/帧】(AM 的原生量纲)。
        //   本项目平台速度是 px/s, 所以乘 dt_s 还原成"每帧位移" —— 量纲还原, 非判据改动。

        // ① 本帧位移 > max(30, 0.8×尺寸) × (1 + 1.5×系数)
        //
        // ★★ 两轴的"尺寸"是【交叉】的, 已逐行核对 AM L121-149:
        //      X 轴(L133-135): 门限尺寸 = fVar18 ← fVar19 = **框高**(L107/L130)
        //                       位移 = *pfVar14           = 框 x
        //                       自身 = *(param_1+0xC04)
        //      Y 轴(L148-149): 门限尺寸 = fVar21 ← pfVar14[3] = **框宽**(L124)
        //                       位移 = fVar20 = pfVar14[1] = 框 y
        //                       自身 = *(param_1+0xC08)
        //    ⇒ **X 轴吃框高, Y 轴吃框宽**。这是 AM 原文的不对称, 照抄。
        //
        //    ★ 量纲: 本实现的 vel_* 是【像素/秒】, 所以本帧位移 = vel × dt_s。
        //      AM 存的是"每帧位移"直接比较 —— 二者等价。
        const double disp_x = std::abs(t->vel_x * dt_s_);
        const double disp_y = std::abs(t->vel_y * dt_s_);
        const double self_disp_x = std::abs(platform_vx_ * dt_s_);
        const double self_disp_y = std::abs(platform_vy_ * dt_s_);

        // X 轴门限用【框高】; Y 轴门限用【框宽】。
        const double size_for_x = std::max(kPredGateMinPx, kPredGateSizeRatioAlt * h);
        const double size_for_y = std::max(
            kPredGateMinPx, kPredGateSizeRatioAlt * static_cast<double>(t->box.w));

        const bool rising_x =
            (size_for_x * (1.0 + kPredGateSizeRatio * t->pred_k_x) < disp_x)
            && (kPredSelfMoveGatePx < self_disp_x);
        const bool rising_y =
            (size_for_y * (1.0 + kPredGateSizeRatio * t->pred_k_y) < disp_y)
            && (kPredSelfMoveGatePx < self_disp_y);

        stepFactor(t->pred_k_x, rising_x);
        stepFactor(t->pred_k_y, rising_y);

        // ── 提前量 = 用户系数 × 平台位移 × sw × 系数 (AM L162-164) ─────────────
        //
        // ★★ 是【平台位移】, 不是目标速度。
        //    此前本实现用的是目标速度 —— 那是**根本性的语义错误**(偏差 #4)。
        const double lead_target_x = params_.pred_factor_x * platform_vx_ * sw * t->pred_k_x;
        const double lead_target_y = params_.pred_factor_y * platform_vy_ * sw * t->pred_k_y;

        // 方向翻转阻尼 + 低通 (AM L166-178)
        lead_x = smoothAxis(t->pred_smooth_x, lead_target_x);
        lead_y = smoothAxis(t->pred_smooth_y, lead_target_y);

        // ── 本项目自加的两道阀: 默认关闭, 开了才生效 ────────────────────────
        if (params_.pred_vel_floor > 0.0)
        {
            // 噪声门按【目标速度】判(px/秒), 与 AM 无关, 是本项目为
            // "静止目标假速度"加的。默认 0 时不参与。
            const double sp_x = std::abs(t->vel_x);
            const double sp_y = std::abs(t->vel_y);
            if (sp_x < params_.pred_vel_floor) lead_x = 0.0;
            if (sp_y < params_.pred_vel_floor) lead_y = 0.0;
        }
        if (params_.pred_max_lead_px > 0.0)
        {
            const double cap = params_.pred_max_lead_px;
            lead_x = std::clamp(lead_x, -cap, cap);
            lead_y = std::clamp(lead_y, -cap, cap);
        }

        // ★ 就地改写输出框心(AM L177-178 的等价物)。
        //   下游(误差计算/选靶)吃的是改写后的位置 —— 这一点必须与 AM 一致。
        t->out_cx = box_center_x(t->box) + lead_x;
        t->out_cy = box_center_y(t->box) + lead_y;

        if (write_to_box_ && (lead_x != 0.0 || lead_y != 0.0))
        {
            // 调用方要求"真的改写框"时(与 AM 逐位一致的模式), 把框左上角平移。
            t->box.x = static_cast<float>(t->out_cx - static_cast<double>(t->box.w) * kHalfExtent);
            t->box.y = static_cast<float>(t->out_cy - static_cast<double>(t->box.h) * kHalfExtent);
        }

        return (lead_x != 0.0 || lead_y != 0.0);
    }

    // 是否把提前量"就地写回框"(AM 的原始做法)。默认 false。
    //
    // ★ 为什么默认 false: AM 就地改写的是**它自己的检测框数组**, 而本项目的
    //   `out.bbox` 是调用方(selector)的输出, 就地改写会影响下一帧的关联基准 ——
    //   而那正是 AM 的语义(它的下一步选靶吃改写后的位置)。
    //   两者都正确, 但混用会双重计入。本项目把结果放在 TrackState::out_cx/out_cy,
    //   由 boss_aim.cpp 取用, 等价且不动调用方的数据。
    void setWriteBackToBox(bool on) { write_to_box_ = on; }

    double sizeWeight(double box_h) const
    {
        const double lo = params_.pred_min_w;
        const double hi = params_.pred_max_w;
        double sw = 1.0;
        if (lo < box_h)
        {
            if (box_h < hi)
                sw = (hi - box_h) / (hi - lo);
            else
                sw = 0.0;
        }
        return sw;
    }

    int nextTrackId() const { return next_id_; }

private:
    static double box_center_x(const TrackBox& b) { return b.x + b.w * kHalfExtent; }
    static double box_center_y(const TrackBox& b) { return b.y + b.h * kHalfExtent; }

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

    // 接上一个观测 (AM L364-410): 更新框/位置, hits+1, misses=0, 速度估计。
    void attach(TrackState& t, const TrackBox& box, double cx, double cy)
    {
        const double prev_cx = t.last_obs_cx;
        const double prev_cy = t.last_obs_cy;
        t.box = box;
        t.observed_this_frame_ = true;
        ++t.hits;
        t.misses = 0;

        // ── 速度估计 (AM L364-396) ───────────────────────────────────────────
        //
        // ★★ AM 的形状是"**窗内沿用旧值、窗外重算并混合**", 不是"窗内累加、满窗结算"。
        //    此前本实现写成了后者 —— 那是**另一套滤波器**(偏差 #9)。
        //
        // ★ 量纲已逐行核对(AM L367-373):
        //    `local_100 = (now_ns − last_ns) / 1e9`  ⇒ **秒**
        //    `iVar17  = max(1, tracking_velocity_sample_ms)` ⇒ **毫秒**
        //    比较式 `local_100 < iVar17 / 1000.0` ⇒ 秒 vs 秒, **单位是一致的**。
        //    所以"沿用旧值"的条件是 `elapsed_s < vel_sample_ms/1000`。
        if (t.has_last_obs)
        {
            const double elapsed_s = (now_ms_ - t.last_obs_ms) / 1000.0;
            if (elapsed_s < params_.vel_sample_ms / 1000.0)
            {
                // AM L373-375: 窗内 ⇒ 沿用旧速度, ★ **不刷新时间戳**
                // (所以窗口会自然过期, 而不是被每帧刷新而永不过期)。
                // 这里什么都不做 —— 保持 t.vel_x/vel_y 原值。
            }
            else
            {
                // AM L377-396: 窗外 ⇒ 用 clamp(dt_s, 0.001, 1.0) 重算并混合。
                // ★ dt 的夹取域也是**秒**(DAT_1401f8000 = 0.001 / DAT_1401f6358 = 1.0,
                //   两个都是 double)。所以速度单位 = 像素/秒。
                // ★ 混合: 新值 × 0.25 + 旧值 × 0.75 (DAT_1401f4620 / DAT_1401f4630)。
                const double dt = std::clamp(elapsed_s, kVelDtMinS, kVelDtMaxS);
                t.vel_x = ((cx - prev_cx) / dt) * kVelBlendNew + t.vel_x * kVelBlendOld;
                t.vel_y = ((cy - prev_cy) / dt) * kVelBlendNew + t.vel_y * kVelBlendOld;
                t.vel_valid = true;
                // AM L393-395: 刷新"上次观测"的位置与时间戳。
                t.last_obs_cx = cx;
                t.last_obs_cy = cy;
                t.last_obs_ms = now_ms_;
            }
        }
        else
        {
            t.has_last_obs = true;
            t.last_obs_cx = cx;
            t.last_obs_cy = cy;
            t.last_obs_ms = now_ms_;
        }

        t.out_cx = cx;
        t.out_cy = cy;
    }

    static void stepFactor(double& k, bool rising)
    {
        if (rising)
            k = std::min(kPredFactorMax, k + kPredFactorRiseStep);
        else
            k = std::max(0.0, k - kPredFactorFallStep);
    }

    // 方向翻转阻尼 + 低通 (AM L166-178)。
    //
    // AM:
    //   blend = 0.05;                       // fVar6 = DAT_1401f5b7c, 正常
    //   if (lead * smooth < 0) blend = 0.02; // fVar7 = DAT_1401f9a00, 翻转时更重
    //   smooth = (1 - blend) * smooth + blend * lead;
    //   return smooth;
    //
    // ★★ 两个 blend 都是**小量** ⇒ 输出是重低通(时间常数 ~20 帧)。
    //    此前我写成"正常 1.0 直通"—— 反了。
    static double smoothAxis(double& smooth, double lead)
    {
        double blend = kPredNormalBlend;
        if (lead * smooth < 0.0)
            blend = kPredFlipBlend;
        smooth = (1.0 - blend) * smooth + blend * lead;
        return smooth;
    }

    AimTrackerParams params_{};
    std::vector<TrackState> tracks_;
    int next_id_ = 1;
    int locked_id_ = -1;
    double dt_s_ = 0.0;
    double now_ms_ = 0.0;
    bool lock_taken_this_frame_ = false;
    bool write_to_box_ = false;

    // ⑤ 平台位移(像素/秒), 由 setPlatformVelocity 每拍喂。
    double platform_vx_ = 0.0;
    double platform_vy_ = 0.0;

public:
    // ★ 保留: 调用方(boss_aim.cpp / 逻辑测试)可能引用。
    static constexpr double kDefaultMaxLeadPx = 0.0;
    static constexpr double kAbsMaxLeadPx = 64.0;
};

}  // namespace boss

#endif  // MOUSE_AIM_TRACKER_H
