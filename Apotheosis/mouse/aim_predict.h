#ifndef AIM_PREDICT_H
#define AIM_PREDICT_H

#include <algorithm>
#include <cmath>

namespace boss
{

// ── 瞄点预测 ────────────────────────────────────────────────────────────────
//
// 本模块是 **AimMagic 1.0.30 `FUN_14006e470`（预测状态机）的等价实现**，公式逐项
// 对齐逆向报告，不是自己推导的近似。依据：
//   `_amrev/AimMagic_RE/v1030/ANALYSIS_v1030.md` §4.2（1.0.30 落地，闭环 ✅）
//   `_amrev/AimMagic_RE/v108/ANALYSIS.md`        §6.2/§6.3（1.0.8，两条独立路径互证）
//   `_amrev/AimMagic_RE/v1030/qml_z/----_预测补偿...qml`（真实 UI，参数名与默认值）
//
// ██ 原式（报告原文，两处代码路径公式逐字相同） ██
//
//     sizeWeight = (maxW - w) / (maxW - minW)          // minW < w < maxW 之外为 0
//     predX = factor_x * platformMotionX * sizeWeight * velX
//     k     = (predX * predXPrev < 0) ? K_damp : 1.0   // 方向翻转 → 阻尼
//     smX   = (1 - k) * predXPrev + k * predX          // 低通
//     box.cx += smX                                     // ★ 直接改写框中心
//
// 参数（UI 名 → 本实现名 → 默认值）：
//     prediction_factor_x/y      预测系数X/Y      0.0      ← 用户调
//     prediction_min_width       最小预测宽度     20       ← 用户调
//     prediction_max_width       最大预测宽度     80       ← 用户调
//     prediction_enabled         启用预测         false
//
// ██ 关于"目标越小提前量越大" ██
//
// 这是 `sizeWeight` 的线性梯度：框宽 = minW 时权重 1（最强），= maxW 时权重 0
// （完全不补），区间外一律 0。
//
// ★ 需要纠正一个常见误读（包括本项目一度写错的版本）：**它不是几何上的 1/d 补偿**。
//   屏幕速度本身就是世界速度的投影（`sv = f·v/d`），直接乘时间是【已经正确】的，
//   再乘一个 `d` 相关量等于把深度算了两遍。
//   这条梯度真正在补偿的是**远距离下速度估计的不可靠**：目标越远，每帧在画面上的
//   位移越小（40m 外 5m/s 横移只有 ~0.7px/帧），检测框中心是整数量化的，这么小的
//   位移会被量化噪声淹没 —— 所以远处【要么不补（估成 0），要么需要更强的放大】。
//   区间设计正是这个用意：太小的框（< minW）根本不可信，直接不补；越大（越近）
//   速度越可信，补偿越弱；超过 maxW 视为近距离稳定目标，完全不需要补。
//
// ██ 速度来源 ██
//
// AM 用的是自己维护的固定系数速度低通（`ANALYSIS_v1030.md` §2.5 ③，稳态下与 α-β
// 等价）。本实现复用 `AnchorFilter` 的 α-β 速度状态 —— 它本来就是同一类量，且已经
// 在跑，不引入第二个估计器。
//
// ██ 与"在途补偿"的区别（这条重要） ██
//
// 本模块输出的是**对瞄点的修正**，让 PID 去追一个"目标将要到"的位置。
//   · 瞄点预测: 改【目标在哪】 —— 输出仍然是"误差 → PID → 计数";
//   · 在途补偿: 改【PID 怎么算】 —— 把未落地的计数从误差里扣掉。
// 前者是 AM/原神都在做的(原神靠高增益+输出整形达到类似效果), 后者是本项目自造的,
// 已删除。两者不叠加。
class AimPredict
{
public:
    AimPredict() = default;

    // factor_x/y  : 预测系数 X/Y。0 = 该轴不预测。UI 默认 0.0。
    //               ★ 2026-09-14 起范围收紧到 ±0.2: 合理量级是 0.05~0.2, 旧范围
    //               ±100 会让准星偏出几百到上万像素(违反任务书 §4.2)。
    // min_w/max_w : 尺寸权重区间(像素宽)。max_w <= min_w 时整体关闭。
    // damp        : 方向翻转阻尼系数 K_damp(0..1)。1 = 不阻尼, 越小越"滑"。
    //               AM 里是一个内部常数, 这里做成可调但给保守默认。
    // max_lead_px : 【提前量硬上限】(像素)。0 或负 => 用内置默认 kDefaultMaxLeadPx。
    //               ★ 这是本类最重要的安全阀, 见 compute() 里的说明。
    // vel_floor   : 【速度噪声门】(像素/秒)。|v̂| 低于此值时提前量归零。
    //               实测静止目标的 v̂ 噪声 p99=46 / max=52, 默认 60 能挡住噪声。
    void configure(double factor_x, double factor_y,
                   double min_w, double max_w, double damp,
                   double max_lead_px = 0.0, double vel_floor = kDefaultVelFloorPxS)
    {
        factor_x_ = finiteOr(factor_x, 0.0);
        factor_y_ = finiteOr(factor_y, 0.0);
        min_w_ = (std::isfinite(min_w) && min_w > 0.0) ? min_w : 0.0;
        max_w_ = finiteOr(max_w, 0.0);
        // AM: maxW = max(minW + 1, cfg->maxW)
        if (max_w_ <= min_w_) max_w_ = min_w_ + 1.0;
        damp_ = std::clamp(finiteOr(damp, 1.0), 0.0, 1.0);
        // 硬上限: 坏值/未设 => 内置默认。绝不允许"不设上限"。
        max_lead_px_ = (std::isfinite(max_lead_px) && max_lead_px > 0.0)
            ? std::min(max_lead_px, kAbsMaxLeadPx) : kDefaultMaxLeadPx;
        vel_floor_ = (std::isfinite(vel_floor) && vel_floor >= 0.0)
            ? vel_floor : kDefaultVelFloorPxS;
    }

    void reset()
    {
        vel_x_ = vel_y_ = 0.0;
        pred_x_ = pred_y_ = 0.0;
        prev_x_ = prev_y_ = 0.0;
        have_ = false;
    }

    // factor 非零 且 尺寸区间有效 才认为启用。
    bool enabled() const
    {
        return (factor_x_ != 0.0 || factor_y_ != 0.0) && max_w_ > min_w_;
    }

    // 尺寸权重: 目标越小(越远)权重越大, 线性梯度。区间外为 0。
    // 与 AM 的 `sizeWeight = (maxW - w)/(maxW - minW)` 逐字等价。
    double sizeWeight(double width_px) const
    {
        if (!(std::isfinite(width_px))) return 0.0;
        if (!(width_px > min_w_ && width_px < max_w_)) return 0.0;
        return (max_w_ - width_px) / (max_w_ - min_w_);
    }

    struct Offset { double x = 0.0; double y = 0.0; };

    // 算这一拍要【加到框中心】的偏移(像素)。
    //
    // motion_x/y : 目标的位移速度(像素/秒), 即 AM 的 "platformMotion"。
    //              AM 用固定系数低通维护; 本实现由 AnchorFilter 的 α-β 速度提供。
    // width_px   : 检测框宽(像素)。尺寸权重的输入。
    // vel_ok     : 速度是否可信。不可信 => 零偏移, 不猜。
    //
    // 注意: AM 还有一层 per-track 的 velX/velY 自适应系数(0..1, 按"目标偏离上次
    // 瞄点的距离是否超过 框宽×比例"来增长/衰减)。那一层的目的是"目标没怎么动就别
    // 补"，对一个已经稳定的速度估计是冗余的 —— 本实现不复制它, 改为直接要求
    // vel_ok(α-β 至少两拍真测量), 语义等价而少两个魔数。
    Offset compute(double motion_x, double motion_y, double width_px,
                   bool vel_ok) const
    {
        if (!enabled() || !vel_ok
            || !std::isfinite(motion_x) || !std::isfinite(motion_y))
            return {};

        const double sw = sizeWeight(width_px);
        if (sw <= 0.0) return {};

        // ── 速度噪声门 (任务书 §4.2 第③条, 2026-09-14 新增) ────────────────
        // 静止目标的 v̂ 是噪声: 实测 p99=46px/s、max=52px/s。不设门的话这些噪声
        // 会被 factor 乘成几像素的假提前量, 在 Kp=35 下就是瞄点上嗡嗡抖。
        // ★ 用【连续斜坡】而不是硬开关: 门限处直接跳变会在目标速度刚好跨过门限时
        //   产生一拍几像素的突变(看起来是"准星抽一下")。斜坡是 C0 连续的。
        const double speed = std::hypot(motion_x, motion_y);
        double gate = 1.0;
        if (vel_floor_ > 0.0)
        {
            if (speed <= vel_floor_) return {};               // 完全不给
            // 门限到 2x 门限之间线性爬满, 保证过门限时偏移从 0 连续长起来。
            gate = std::min(1.0, (speed - vel_floor_) / vel_floor_);
        }

        double want_x = factor_x_ * motion_x * sw * gate;
        double want_y = factor_y_ * motion_y * sw * gate;

        // ── 提前量硬上限 (任务书 §4.2 第②条, 2026-09-14 新增) ──────────────
        // ★ 这是本类最关键的安全阀。没有它, 稳态瞄偏 = factor×sw×v, 随速度【线性
        //   增长】—— 正是 §4.2 明令禁止的形态, 也是历史上"准星稳定停在目标下一拍
        //   位置"的病根。有了它, 提前量被钉在一个与物理量同量级的小数(默认 12px),
        //   速度再高也不会把准星甩出去。
        //   夹取用整体模长而不是分轴, 避免斜向运动时实际幅度超过上限(√2 倍)。
        const double mag = std::hypot(want_x, want_y);
        if (mag > max_lead_px_)
        {
            const double s = max_lead_px_ / mag;
            want_x *= s;
            want_y *= s;
        }

        // 方向翻转阻尼 (AM 原文: k = (predX*predXPrev < 0) ? K_damp : 1.0)
        //   同号 => k = 1.0 => 直接取新值(无平滑)
        //   反号 => k = damp => 只挪过去一部分, 下一拍继续 —— "滑过去"而不是"跳过去"
        // 未初始化时直接采纳, 不从 0 慢慢爬。
        const double kx = (!have_ || want_x * prev_x_ >= 0.0) ? 1.0 : damp_;
        const double ky = (!have_ || want_y * prev_y_ >= 0.0) ? 1.0 : damp_;
        Offset out{ (1.0 - kx) * prev_x_ + kx * want_x,
                    (1.0 - ky) * prev_y_ + ky * want_y };

        // ★ 阻尼后的结果也要再夹一次: 方向翻转那几拍 out 可能由 prev 主导, 而 prev
        //   本身是上一拍的合法值, 所以理论上不会超; 但 prev 是阻尼混合的结果, 若
        //   上限被中途调小就会残留超限值。补一次, 保证"任何时刻都不超上限"。
        const double omag = std::hypot(out.x, out.y);
        if (omag > max_lead_px_)
        {
            const double s = max_lead_px_ / omag;
            out.x *= s;
            out.y *= s;
        }
        return out;
    }

    // 落盘本拍结果, 供下一拍做方向翻转判定。引擎每拍调一次。
    // (AM 把平滑结果存回 track+0x20/+0x24, 语义相同)
    void commit(const Offset& o)
    {
        prev_x_ = std::isfinite(o.x) ? o.x : 0.0;
        prev_y_ = std::isfinite(o.y) ? o.y : 0.0;
        pred_x_ = prev_x_;
        pred_y_ = prev_y_;
        have_ = true;
    }

    double lastLeadX() const { return pred_x_; }
    double lastLeadY() const { return pred_y_; }
    double factorX() const { return factor_x_; }
    double factorY() const { return factor_y_; }
    // 提前量硬上限与速度门(供 UI 遥测/回归断言读取)
    double maxLeadPx() const { return max_lead_px_; }
    double velFloorPxS() const { return vel_floor_; }

    // ★ 提前量硬上限的默认值(像素)。
    // 12px 的依据: 46ms 死区内, 一个 300px/s 的目标走过 13.8px —— 也就是"补上一个
    // 链路死区"所需的最大提前量就在这个量级。再大就是主动瞄未来, 属于 §4.2 禁止的
    // "稳态瞄偏随速度线性增长"。
    static constexpr double kDefaultMaxLeadPx = 12.0;
    // 绝对天花板: 即使配置填得更大也不允许越过(UI 范围是 0..64)。
    static constexpr double kAbsMaxLeadPx = 64.0;
    // 速度噪声门默认值: 实测静止目标 v̂ 噪声 p99=46 / max=52。
    static constexpr double kDefaultVelFloorPxS = 60.0;

private:
    static double finiteOr(double v, double d)
    {
        return std::isfinite(v) ? v : d;
    }

    double factor_x_ = 0.0;
    double factor_y_ = 0.0;
    double min_w_ = 0.0;
    double max_w_ = 1.0;
    double damp_ = 1.0;
    double max_lead_px_ = kDefaultMaxLeadPx;
    double vel_floor_ = kDefaultVelFloorPxS;

    // 速度估计状态(目前只用于透传, 保留以便将来需要时切换成内置估计器)
    double vel_x_ = 0.0;
    double vel_y_ = 0.0;
    // 平滑状态 (AM: track+0x20/+0x24)
    double pred_x_ = 0.0;
    double pred_y_ = 0.0;
    double prev_x_ = 0.0;
    double prev_y_ = 0.0;
    bool have_ = false;
};

}  // namespace boss

#endif  // AIM_PREDICT_H
