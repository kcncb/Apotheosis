#ifndef MOUSE_AIM_SCALE_H
#define MOUSE_AIM_SCALE_H

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace boss
{

// ── 距离尺度 (G6「近大远小」) ────────────────────────────────────────────────
//
// 本模块只回答一个问题: **这个目标在画面里比我调参时大了多少倍**, 并把它映射成
// 一个连续、有界、单调的乘数 s。它【不估计距离】, 也【不引入任何相机内参或
// 真实深度假设】。
//
// ██ 公式 ██
//
//     s = clamp( (h / H₀)^γ , s_min , s_max )        γ = 1.0
//
//   h  = 当前帧的框高(像素)
//   H₀ = 【基准框高】—— 「自动调参」页上按按钮设定的那个距离的框高中位数
//
//   等比关系: 高度减半 -> 倍率减半。h = H₀ 时 s 恒为 1.0(与没有这个机制逐位相同)。
//
// ██ ★★ 为什么不用"近处框高/远处框高"两个绝对值 ★★ ██
//
// 这是用户明确提出并纠正过的设计问题(2026-09-14)。原设计让用户填两个绝对像素
// 阈值(near=160 / far=45), 但:
//
//   1. 【用户根本测不出来】。"这个目标的框现在是多少像素" 在靶场里无法观测 ——
//      用户能看到目标, 但看不到检测框的数值。让他填一个他看不见的量, 这个参数
//      就只能靠猜。
//   2. 【它不是普适的】。换游戏(人形目标尺寸体系不同)、换分辨率、换目标类型
//      (人/车/靶), 同一段像素高度对应的"距离感"完全不同。写死两个数, 换一个
//      场景就废了。
//   3. 【用户真正需要的不是"远近", 而是"相对我调参的时候"】。用户在某个距离上
//      调出了一组满意的 Kp, 那组参数天然就是"在 H₀ 这个框高下表现最好"的。所以
//      唯一有意义的参照系就是【他自己调参时的那个框高】, 而不是任何绝对数值。
//
// 于是设计改成: **基准自动来自用户调参时的框高, 用户一个像素值都不用填。**
// 换游戏/换分辨率时用户本来就要重新调参, 基准随之自动更新, 不需要额外操作。
//
// ██ ★★ 为什么远处也真的降增益(与原设计相反, 已被用户纠正) ★★ ██
//
// 原实现把 s_min 硬钉在 1.0, 理由是"纯反馈的匀速跟随滞后 ≈ v/(Kp_eff·k̂),
// 降增益会让远处更跟不上"。
//
// ★ 这个论证是【错的】, 因为它隐含假设了像素速度 v 固定。但现实中
//
//     v_px ≈ f · v_world / d
//
// 远处目标在画面里【移动本来就慢】(同一个世界速度, 距离远则像素位移小), 所以
// 远处需要的像素域增益本来就应该小。s = h/H₀ 恰好跟随这个 1/d 关系下降, 它不是
// "朝错误方向使劲", 而是【跟随距离变化的正确缩放】。
//
// 用户的原话: "远处敌人看着移动会变慢啊 那肯定也是按公式下降"。
//
// 所以 s_min 放开, 允许 < 1.0。默认 1.0(不改变现有行为), 用户可以按需往下拧。
//
// ██ ⚠️ 必须避免的错误: 不要把深度算两遍 ██
//
// 屏幕速度【本身已经是】世界速度的投影。如果再乘一个与距离(或框高)成正比的
// 因子去做"几何距离补偿", 等于把 1/d 算了两遍。
//
//     ✗ 错: 提前量 = v_px · T · (1/s)      // s 正比于框高, 这就是又乘了一次 d
//     ✓ 对: 提前量 = v_px · T, 而 T 受 s 调制
//
// 这条错误在项目历史上犯过一次并纠错过, 依据见 `mouse/aim_predict.h` 顶部
// 「关于"目标越小提前量越大"」那一段。本模块因此只输出一个【乘数】, 由消费方
// 决定它作用在哪个环节(增益/上限/门限/平滑), 而绝不让它去改 v_px 的量纲。
//
// ██ 尺度线索: 只用检测框高度 ██
//
// 用户已确认 `bbox.height` 是本项目唯一的距离代理(单目 2D 检测框, 没有深度通道;
// `AimbotTarget::depth_at_pivot` 是恒为 -1 的占位, 不可使用)。
//
// 用高度而不是宽度: 人形目标的【高度】受姿态影响更小(蹲下/侧身主要改变宽度),
// 而且高度对"目标被地形部分遮挡"也更稳健 —— 遮挡通常从脚开始, 宽度先失真。
//
// ██ 为什么连续, 而不是"近距离/远距离"两档 ██
//
// 硬开关会在阈值附近产生【极限环】: 框高在阈值上下抖动 → 增益在两档之间来回切
// → 准星一顿一顿。本项目已经因为"死区"吃过一次这个亏(实测输出以 10.2 次/秒
// 在动/不动之间翻转)。所以用单调连续函数。
//
// ██ 为什么用幂函数而不是线性 ██
//
// 框高与距离成反比(h ≈ f·H_world/d), 所以 d 的等比变化对应 h 的等比变化。
// s = (h/H₀)^γ 让【距离的等比变化】对应【尺度的等比变化】: 距离减半 → 框高
// 翻倍 → s 翻倍(γ=1)。若用线性 s = a·h + b, 则"40m→30m"与"10m→9m"会得到
// 完全不同的尺度增量, 而它们视觉上是同一档。对数/幂映射是唯一与几何一致的形式。
struct AimScaleParams
{
    // ── 基准框高(像素) ──────────────────────────────────────────────────────
    //
    // ★ 这不是"要用户填的阈值", 而是【用户按按钮设定的观测值】: 站到平时交战的
    //   典型距离上对着目标停一两秒, 按「以当前距离设为基准」, 程序取最近约 7 秒
    //   框高的中位数填在这里(见 autotune::Runtime::set_baseline_from_recent)。
    //   ★ 与自动调参 agent 的开关无关 —— 不开 agent 也能设。
    //   h = H₀ 时 s = 1.0。
    //
    //   0 或负数 = 还没设置基准, 此时整条链路恒为中性 1.0(与关闭尺度逐位相同)。
    double base_h_px = 0.0;

    // 幂指数。1.0 = 等比(高度减半→倍率减半)。
    // <1 远处下降更慢(不那么钝), >1 下降更快。默认 1.0, 先不暴露给界面。
    double gamma = 1.0;

    // 远处下限。★ 允许 < 1.0 —— 见上面「为什么远处也真的降增益」。
    //   默认 1.0 = 不改变现有行为; 用户觉得远处该更稳就往下拧。
    double s_min = 1.00;
    // 近处上限。★ 受物理天花板约束: Kp_eff = Kp * s_max 必须仍在 g_crit 以下。
    //   生产 Kp=35、s_max=1.5 => 52.5, 与 g_crit 对应的约 52 基本相等 ——
    //   所以 1.5 是"贴着上限"的取值; 再大就要先降 Kp。
    double s_max = 1.50;

    // 框高自身的轻度平滑时间常数(秒)。0 = 不平滑。
    //
    // ★ 这不违反「全链路只有一道平滑」那条约束 —— 那条针对的是【位置】路径
    //   (位置被串联滤波会累积滞后, 参数还互相耦合)。框高是一条【独立的标量
    //   通道】, 它不进位置回路, 只调制几个乘数, 所以在这里平滑不会给位置引入
    //   任何滞后。
    //
    //   框高确实在抖(框在逐帧变化), 而尺度乘数会直接乘到增益上 —— 不过滤的话
    //   就是"增益被框高噪声调制", 表现是准星跟着框的抖动一起变频。取 120ms:
    //   明显长于拍间隔(8.3ms), 又远短于"目标靠近/远离"的时间尺度(几百毫秒以上)。
    double smooth_tau_s = 0.120;
};

class AimScale
{
public:
    // 没学到基准时的中性值。整个模块的"关闭态"就是这个值。
    static constexpr double kNeutralScale = 1.00;
    static constexpr double kDefaultSMin  = 1.00;
    static constexpr double kDefaultSMax  = 1.50;
    // 基准的合法范围。太小的框(噪声)或太大的框(畸形)不该被当成基准。
    static constexpr double kMinBaseHPx = 4.0;
    static constexpr double kMaxBaseHPx = 4000.0;

    using Params = AimScaleParams;

    AimScale() = default;

    // 每拍都可以调。只有【参数真的变了】才清状态 —— 引擎每拍都调 configure(),
    // 无条件 reset 会让平滑器永远停在初始化态(2026-09-12 在 AnchorObserver 上
    // 踩过这个坑: 延迟线永远攒不满, 速度恒为 0)。
    void configure(const AimScaleParams& p)
    {
        AimScaleParams q = p;

        if (!std::isfinite(q.base_h_px) || q.base_h_px < kMinBaseHPx
            || q.base_h_px > kMaxBaseHPx)
            q.base_h_px = 0.0;              // 非法基准 -> 视为"还没学到"

        if (!std::isfinite(q.gamma) || q.gamma <= 0.0)
            q.gamma = 1.0;

        if (!std::isfinite(q.s_min) || q.s_min <= 0.0)
            q.s_min = kDefaultSMin;
        if (!std::isfinite(q.s_max) || q.s_max <= 0.0)
            q.s_max = kDefaultSMax;
        // 端点顺序保护: 用户填反了就交换, 而不是让整条链路输出怪值。
        if (q.s_min > q.s_max)
            std::swap(q.s_min, q.s_max);

        if (!std::isfinite(q.smooth_tau_s) || q.smooth_tau_s < 0.0)
            q.smooth_tau_s = 0.0;

        const bool changed = !configured_
            || q.base_h_px != p_.base_h_px
            || q.gamma != p_.gamma
            || q.s_min != p_.s_min
            || q.s_max != p_.s_max
            || q.smooth_tau_s != p_.smooth_tau_s;
        p_ = q;
        configured_ = true;
        if (changed)
            reset();
    }

    void reset()
    {
        initialized_ = false;
        h_smoothed_ = 0.0;
        // ★ 复位到【中性 1.0】而不是 s_max。
        //   若复位到 s_max, "还没有任何框高信息"的头几拍会用一个近处的增益倍数,
        //   也就是"刚锁上时比平时快 50%" —— 那正是要避免的失效模式。中性值意味着
        //   在信息到位之前, 行为与"没有这个机制"逐位相同。
        s_ = kNeutralScale;
    }

    // 喂一拍框高(像素), 返回本拍的尺度 s ∈ [s_min, s_max]。
    //
    // 坏输入(非有限 / <=0)沿用上一次的平滑值; 从未有过有效输入时返回 1.0
    // (中性 = "不做尺度调制"), 这样链路在还没有框高信息时的行为与关掉尺度一致。
    double step(double bbox_height_px, double dt)
    {
        if (!std::isfinite(bbox_height_px) || bbox_height_px <= 0.0)
            return initialized_ ? s_ : kNeutralScale;

        if (!std::isfinite(dt) || dt <= 0.0)
            dt = 1.0 / 120.0;

        if (!initialized_)
        {
            // 首拍直接贴上, 不做"从某个初值慢慢爬" —— 那会让锁定后的头几拍
            // 用一个错误的尺度出力, 表现是"刚锁上时拉枪比平时慢/快一截"。
            initialized_ = true;
            h_smoothed_ = bbox_height_px;
        }
        else if (p_.smooth_tau_s > 0.0)
        {
            const double alpha = std::clamp(dt / (p_.smooth_tau_s + dt), 1e-4, 1.0);
            h_smoothed_ += (bbox_height_px - h_smoothed_) * alpha;
        }
        else
        {
            h_smoothed_ = bbox_height_px;
        }

        s_ = mapHeightToScale(h_smoothed_);
        return s_;
    }

    // 框高 → 尺度的纯映射(无状态, 供测试与遥测)。
    //
    //     s = clamp( (h/H₀)^γ , s_min , s_max )
    //
    // 性质(有回归): 连续、单调不减、有界。
    //   · 没有基准(H₀<=0) -> 恒为 1.0(与没有这个机制逐位相同)
    //   · h = H₀          -> 1.0
    //   · h > H₀(更近)    -> 上升, 最高 s_max
    //   · h < H₀(更远)    -> 下降, 最低 s_min
    double mapHeightToScale(double h) const
    {
        // 没有学到基准 -> 中性。这样"用户还没调过参"时行为与不做尺度逐位相同。
        if (!std::isfinite(p_.base_h_px) || p_.base_h_px <= 0.0)
            return kNeutralScale;
        // 无有效框高 -> 中性, 与"没有这个机制"逐位相同。
        if (!std::isfinite(h) || h <= 0.0)
            return kNeutralScale;

        const double ratio = h / p_.base_h_px;
        double s = std::pow(ratio, p_.gamma);
        if (!std::isfinite(s))
            return kNeutralScale;
        return std::clamp(s, p_.s_min, p_.s_max);
    }

    // ── 基准 H₀ 从哪来 ─────────────────────────────────────────────────────
    //
    // ★ 本类【不再自己采基准】。2026-09-14 之前这里有一套
    //   begin_baseline_sample/add_baseline_sample/finish_baseline_sample,
    //   与 autotune::Runtime 里那套重复 —— 两份实现迟早不一致, 已删除。
    //
    //   现在唯一的来源是 autotune::Runtime(见 autotune_runtime.h):
    //   界面上一行「以当前距离设为基准」按钮, 取最近几秒框高的中位数, 存进
    //   config 的 aim_scale_base_h, 再由 mouse_thread_loop 填进
    //   AimScaleParams::base_h_px。
    //
    //   本类只负责【给定基准之后怎么映射】, 不关心基准是怎么来的 —— 这样
    //   "谁负责采集"只有一处, 不会有两份真相。

    double scale() const { return s_; }
    double smoothedHeight() const { return h_smoothed_; }
    bool initialized() const { return initialized_; }
    const AimScaleParams& params() const { return p_; }

private:
    AimScaleParams p_{};
    bool configured_ = false;
    bool initialized_ = false;
    double h_smoothed_ = 0.0;
    double s_ = kNeutralScale;   // 中性起步
};

} // namespace boss

#endif // MOUSE_AIM_SCALE_H
