#ifndef ANCHOR_FILTER_H
#define ANCHOR_FILTER_H

#include <algorithm>
#include <cmath>

namespace boss
{

// ── 瞄点平滑器 (α-β) ─────────────────────────────────────────────────────────
//
// 职责只有一件事: **把检测框的抖动滤掉, 输出一个干净的位置给 PID。**
//
// ██ 为什么要它 ██
//
// 检测框每帧都在跳。实测本机框心逐帧变化中位 0.06px, 但偶发跳变有 1.9px ——
// 在 8.3ms 的拍间隔下, 那是 230px/s 的【假速度】。PID 的 P 项放大的是这个噪声,
// 不是真实误差。所以 Kp 一开大就开始抖 —— 抖的是检测噪声, 不是控制不稳。
//
// 把平滑器放在 PID【之前】, 喂进去的就是干净信号, Kp 才敢开大。
//
// ██ 为什么用 α-β 而不是一阶低通 ██
//
// 一阶低通(输出 = 上次*(1-α) + 测量*α)只知道"位置", 不知道"目标在动"。
// 于是目标匀速移动时, 它【会持续落后一个固定量】:
//
//     一阶低通追匀速目标的相位滞后 ≈ v * τ      (τ = 等效时间常数)
//
// 这个滞后只能靠把 Kp 开得更大去补 —— 但 Kp 越大, 残留噪声放大得越厉害,
// 于是"滤得狠 -> 滞后大 -> 要更大 Kp -> 又放大噪声"绕成一圈。
//
// α-β 带【速度状态】, 它是"预测 + 修正":
//
//     预测 = 上次位置 + 上次速度 * dt          // 它知道目标在动
//     位置 = 预测 + α * (测量 - 预测)
//     速度 = 上次速度 + (β/dt) * (测量 - 预测)
//
// 匀速目标下, 预测本来就准, 新息(测量-预测)接近 0, 所以【几乎不落后】;
// 而随机抖动互相抵消, 照样被滤掉。**同样的抗抖能力, 滞后远小于一阶低通。**
// 这就是本项目相对原神 AI 那套(疑似纯低通)的"青出于蓝"处。
//
// ██ β = 0 会退化成纯低通 ██
//
// β = 0 时速度恒为 0, 上式退化成一阶低通(α 就是低通系数)。留这条只是为了
// 回归测试能构造对照, 正常运行不要用。
//
// ██ 参数怎么定 ██
//
// α/β 由【平滑时间常数 tau_s】换算, 保持"帧率无关":
//
//     α = dt / (tau + dt)        // 与 anchor_observer 用的形式一致
//     β = α² / (2 - α)          // "不过冲"的经典配对(临界阻尼)
//
// ★ 这两个量【故意不暴露给界面】: 平滑强度和 Kp 是耦合的, 两个一起调极容易
//   调乱(滤狠了 Kp 要跟着大, 调完不知道是谁的问题)。用户只调 Kp/Ki/Kd 三个
//   增益 + 饱和阈值 + 输出限幅。
class AnchorFilter
{
public:
    AnchorFilter() = default;

    // tau_s: 平滑时间常数(秒)。<=0 = 不平滑(α=1, 直通), 只留给测试。
    // ★ 只有 tau 真的变化才清状态 —— 引擎每拍都会调 configure(), 无条件 reset
    //   会让滤波器永远停在初始化态(这是 2026-09-12 在别处踩过的坑)。
    void configure(double tau_s)
    {
        const double tau = (std::isfinite(tau_s) && tau_s > 0.0) ? tau_s : 0.0;
        if (configured_ && tau == tau_)
            return;
        tau_ = tau;
        configured_ = true;
        reset();
    }

    void reset()
    {
        initialized_ = false;
        pos_ = 0.0;
        vel_ = 0.0;
        samples_ = 0;
    }

    // 喂一拍测量值, 返回平滑后的位置。
    // dt <= 0 或非有限时按 1/120 处理(与别处一致), 不抛错也不卡死。
    double step(double measurement, double dt)
    {
        if (!std::isfinite(measurement))
            return initialized_ ? pos_ : 0.0;   // 测量不可用: 保持上次输出

        if (!std::isfinite(dt) || dt <= 0.0)
            dt = 1.0 / 120.0;

        // 首拍: 没有历史, 直接贴上去(不做任何"从 0 慢慢爬"的蠢事)。
        if (!initialized_)
        {
            initialized_ = true;
            pos_ = measurement;
            vel_ = 0.0;
            samples_ = 1;
            return pos_;
        }

        const double predicted = pos_ + vel_ * dt;
        const double residual = measurement - predicted;

        const double alpha = (tau_ > 0.0)
            ? std::clamp(dt / (tau_ + dt), 1e-4, 1.0)
            : 1.0;
        const double beta = alpha * alpha / (2.0 - alpha);

        pos_ = predicted + alpha * residual;
        vel_ += (beta / dt) * residual;
        if (samples_ < 1000) ++samples_;
        return pos_;
    }

    bool initialized() const { return initialized_; }
    // 平滑后的位置。未初始化时为 0。
    double position() const { return pos_; }

    // 当前速度估计 (像素/秒)。
    //
    // ★ 2026-09-13: 由"不外传"改为对外提供。理由 —— 它本来就是 α-β 的【状态】,
    //   "预测 + 修正" 里的预测项 pos_ + vel_*dt 用的就是它。把它拿出去喂瞄点预测,
    //   等于让滤波器内部已经在算的东西多一个消费者, 不引入新的噪声源, 也不需要
    //   另起一个速度估计器(照 AimMagic 的做法: 速度在跟踪器里按 track 维护)。
    //
    //   之前"不外传"的理由是"当时没有任何前馈消费目标速度"。现在有了(瞄点预测),
    //   所以这条注释随之更新 —— 不是推翻, 是过期。
    double velocity() const { return vel_; }

    // 速度估计是否可信(至少吃过两拍真测量; 只有一拍时 vel_ 恒为 0)。
    bool velocityValid() const { return initialized_ && samples_ >= 2; }

private:
    double tau_ = 0.0;
    bool configured_ = false;
    bool initialized_ = false;
    double pos_ = 0.0;
    double vel_ = 0.0;   // α-β 的速度状态(像素/秒)。位置预测与瞄点预测共用它。
    int samples_ = 0;    // 吃过几拍测量 —— velocityValid() 用它判断速度是否可信。
};

}  // namespace boss

#endif  // ANCHOR_FILTER_H
