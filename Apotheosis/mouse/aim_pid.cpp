#include "aim_pid.h"

#include <algorithm>
#include <cmath>

namespace boss
{
namespace
{

// dt 允许区间。上界 1/2000 s: 重复 tick / 时间戳抖动不该把微分项吹爆;
// 下界 1/15 s: 掉帧超过 66 ms 就按 15fps 计算, 不让积分一步跳过头。
constexpr double kMinDt = 1.0 / 2000.0;
constexpr double kMaxDt = 1.0 / 15.0;

// 微分低通时间常数: 检测框本身有几个像素的抖动, 裸差分(再除以很小的 dt)会被
// 放大成每拍几十个计数的抖动电流, 一阶低通把它压回可用范围。
//
// ★ 2026-09-12: 0.006 -> 0.020。
//   原来的 6ms 比实际拍间隔(dt_ms p50 = 8.3ms)还小, 于是滤波系数
//   alpha = dt/(tau+dt) = 8.3/(6+8.3) = 0.58 —— "新值占 58%", 这个低通在运行
//   帧率下几乎不生效, 等于让未滤干净的框量化台阶直接进微分项。
//   一阶低通的等效时间常数要 >= 2~3 倍采样周期才有意义, 所以取 20ms(≈2.4 倍)。
//   实测签名: 小误差段 |dpy| 的 p50 = 0.00 / p90 = 0.15, 即"大部分帧是 0、
//   偶尔跳一下", 正是没滤干净的量化噪声。
constexpr double kDerivativeTau = 0.020;

// 内置输出上限(计数/拍)。用户没填"移动限幅"时用它兜底。
constexpr int kDefaultLimitCounts = 200;

// 取整零头的绝对值上限。lround 之后 |零头| <= 0.5, 这里是防御性夹取。
constexpr double kCarryClamp = 0.5;

// 积分"快速回吐"时间常数: 误差反向时用这个时间常数把积分衰减掉。
constexpr double kIntegralUnwindTau = 0.2;

// 前馈偏移的像素上限。观测器出错时, 前馈最多把误差挪这么多, 绝不会把指令顶飞。
// pending(在途自身位移)的上限要【放宽】: 一次大甩枪在 46ms 死区里发出去的位移可以到
//   k * 限幅 * (死区/拍间隔) = 0.593 * 200 * 5.6 ≈ 660px
// 夹得太小等于在最需要补偿的时候把它切掉(而那时正是过冲/极限环的来源)。
constexpr double kMaxPendingPx = 800.0;
constexpr double kMaxPredictPx = 120.0;
constexpr double kMaxLeadPx = 80.0;

// 前馈允许的最长预测时间(秒)。UI 上限比它小, 这里是防御性夹取。
constexpr double kMaxFeedforwardTimeS = 2.0;

// ── 速度前馈的"噪声门"(像素/秒) ──────────────────────────────────────────────
// 提前量/延迟预测这两项都是 `v̂ * 时间`, 所以 v̂ 的噪声会【按时间放大】成假误差。
// 检测框坐标一帧一变(实测每帧 1.1~1.9px 中位 / 8.8~9.1px 90分位), 除以 8.3ms 的拍间隔
// 就是 130~230px/s 的瞬时速度噪声。观测器对外报出的速度已经过 200ms 低通, 回归测试
// (aim_pid_test [15]②b, ±1.9px/6Hz + 每37帧一个9px尖峰) 实测静止目标的噪声量级:
//     p50 = 7, p95 = 11, p99 = 46, max = 52 px/s
// 所以门的下限设在 60(高于噪声上界): |v̂| < 60 时【一点前馈都不给】—— 静止目标不需要
// 提前量, 给了只是把噪声灌进误差回路(乘 0.125s 再乘 Kp=100 的 0.83 = 每拍几个计数
// 在瞄点上嗡嗡)。110px/s 以上一定是真运动(慢速横移也有 100px/s 量级), 满额给。
// 注意这只挡速度前馈, 不影响 P/I/D —— 静止目标的稳态误差照样靠积分磨到 0。
constexpr double kFeedforwardVelFloorPxS = 60.0;
constexpr double kFeedforwardVelFullPxS = 110.0;

} // namespace

void AimPid::configure(const AimPidParams& params)
{
    const AimPidParams fallback{};

    AimPidParams p = params;
    if (!std::isfinite(p.kp) || p.kp < 0.0)
        p.kp = fallback.kp;
    if (!std::isfinite(p.ki) || p.ki < 0.0)
        p.ki = fallback.ki;
    if (!std::isfinite(p.kd) || p.kd < 0.0)
        p.kd = fallback.kd;
    if (!std::isfinite(p.predict_time_s) || p.predict_time_s < 0.0)
        p.predict_time_s = 0.0;
    if (!std::isfinite(p.lead_time_s) || p.lead_time_s < 0.0)
        p.lead_time_s = 0.0;
    p.predict_time_s = std::min(p.predict_time_s, kMaxFeedforwardTimeS);
    p.lead_time_s = std::min(p.lead_time_s, kMaxFeedforwardTimeS);
    if (!std::isfinite(p.p_full_scale_px) || p.p_full_scale_px < 0.0)
        p.p_full_scale_px = fallback.p_full_scale_px;
    if (!std::isfinite(p.integral_window_px) || p.integral_window_px < 0.0)
        p.integral_window_px = fallback.integral_window_px;
    p.limit_counts = std::clamp(p.limit_counts, 0, 100000);

    params_ = p;
    configured_ = true;
}

void AimPid::reset()
{
    integral_ = 0.0;
    derivative_ = 0.0;
    prev_error_ = 0.0;
    carry_ = 0.0;
    first_ = true;
    last_p_ = 0.0;
    last_i_ = 0.0;
    last_d_ = 0.0;
    last_output_ = 0.0;
    last_used_error_ = 0.0;
    last_feedforward_ = 0.0;
}

int AimPid::outputLimit() const
{
    return params_.limit_counts > 0 ? params_.limit_counts : kDefaultLimitCounts;
}

double AimPid::predictTimeSeconds() const
{
    return params_.predict_time_s;
}

int AimPid::step(double error_px, double dt, const AimPidFeedback& feedback)
{
    if (!configured_)
        configure(AimPidParams{});

    if (!std::isfinite(error_px))
    {
        // 坏测量: 这一拍不发, 也不把它当成"误差 0"写进历史(否则下一拍会出现
        // 一个人造的导数尖峰)。状态原样保留。
        last_p_ = 0.0;
        last_i_ = 0.0;
        last_d_ = 0.0;
        last_output_ = 0.0;
        last_used_error_ = 0.0;
        last_feedforward_ = 0.0;
        return 0;
    }

    if (!std::isfinite(dt) || dt <= 0.0)
        dt = kMaxDt;
    // 第一版行为: 直接按真实 dt 缩放输出(不再夹到"典型拍间隔")。
    // 日志里 dt_ms 会原样记下真实间隔, 掉帧/换目标跳帧造成的放大可以在日志里直接看到。
    dt = std::clamp(dt, kMinDt, kMaxDt);
    last_dt_ = dt;

    // 复位后的第一拍没有"上一拍误差", 微分按 0 处理, 避免换目标瞬间的导数冲击。
    //
    // ★ 2026-09-12: 原来这里只把 prev_error_ 初始化, 没有清 derivative_。
    //   于是"开镜/换靶第一下"会把上一段遗留的微分状态直接放出来 —— 一段本不该
    //   属于这个目标的阻尼。AimMagic 的等价写法是在 frame_count==1 时把 dTerm
    //   整个置 0(反汇编 0x14005719B 的 XORPS XMM6,XMM6), 这里照做。
    //   注意顺序: 必须在下面算微分之前清, 否则这一拍的 alpha 混合会把它又带回来。
    if (first_)
    {
        prev_error_ = error_px;
        derivative_ = 0.0;
        first_ = false;
    }

    const double limit = static_cast<double>(outputLimit());

    // ── 微分项: 对【测量误差】求导(像素/秒), 一阶低通 ────────────────────────
    // 用测量误差而不是 e_used: 前馈本身变化很慢, 拿它求导只会把噪声引进来。
    //
    // 注: 试过"单拍跳变不喂微分"(想挡检测框整帧跳 20~45px 造成的假速度), 但甩枪/换靶
    // 时【误差本身】就会单拍跳几百像素 —— 那是真的, 冻掉它反而让到位过冲变差(实测
    // 静止目标尾段误差从 <2px 涨过门槛)。要区分"框跳"和"真的在动"只有观测器手里有信息
    // (它有自身指令模型, 能算出残差), 所以这件事留给观测器的野值门限去做。
    const double raw_derivative = (error_px - prev_error_) / dt;
    const double alpha = std::clamp(dt / (kDerivativeTau + dt), 0.0, 1.0);
    derivative_ += (raw_derivative - derivative_) * alpha;

    // ── 前馈(像素) ──────────────────────────────────────────────────────────
    double velocity = feedback.target_velocity_px_s;
    if (!std::isfinite(velocity))
        velocity = 0.0;
    double pending = feedback.pending_self_motion_px;
    if (!std::isfinite(pending))
        pending = 0.0;

    // 在途自身位移: 我们刚发出去的位移画面还没显示出来, 所以【真实误差比看到的小】,
    // 要减掉, 否则会在追赶过程中重复下令(过冲的经典成因)。
    pending = std::clamp(pending, -kMaxPendingPx, kMaxPendingPx);
    // 盲区里目标自己的位移: 目标会走, 提前补上。
    // ★ 速度前馈的"噪声门"(见 kFeedforwardVelFloorPxS 的注释): 只在目标【真的在动】
    //   时才提前。静止目标的 |v̂| 全是框抖动的噪声, 给了就是把噪声灌进误差回路。
    double ff_scale = 1.0;
    if (kFeedforwardVelFullPxS > kFeedforwardVelFloorPxS)
    {
        const double abs_v = std::abs(velocity);
        ff_scale = std::clamp((abs_v - kFeedforwardVelFloorPxS)
                                  / (kFeedforwardVelFullPxS - kFeedforwardVelFloorPxS),
                              0.0, 1.0);
    }
    const double predict_px = std::clamp(velocity * params_.predict_time_s * ff_scale,
                                         -kMaxPredictPx, kMaxPredictPx);
    // 主动提前量: 瞄目标的未来位置(打移动靶/弹道提前)。
    const double lead_px = std::clamp(velocity * params_.lead_time_s * ff_scale,
                                      -kMaxLeadPx, kMaxLeadPx);

    const double feedforward = -pending + predict_px + lead_px;
    const double used_error = error_px + feedforward;

    // ── 三个分项先折算到【像素】, 再统一乘 kp*dt ────────────────────────────
    //   e_used          像素
    //   ki * 积分状态   [1/s] * [像素*秒] = 像素
    //   kd * 微分       [秒] * [像素/秒]  = 像素
    // 括号里是"等效像素误差", 乘 kp[计数/(像素*秒)] * dt 得到本拍计数。三个增益都带
    // 时间量纲, 所以换帧率不用重调 —— 链路死区是按【秒】存在的(采集+推理+HID+游戏+
    // 显示), 老实现那种"每拍增益"在 60fps 和 240fps 下表现完全不同。
    const double rate_limit = limit / dt;  // 输出上限折算成计数/秒

    // ── 积分项: 积 e_used(像素·秒) ─────────────────────────────────────────
    // 积分速率对大误差【按 window/|e| 衰减】, 不做硬开关:
    //   · 甩枪阶段(误差几百像素)几乎不积, 否则会在 50ms 死区里攒出一大笔欠账, 到位后
    //     要几百毫秒才吐干净(实测: 硬开关会留下 50px 级摆动)。
    //   · 跟匀速目标时误差是 v/(k*Kp) 这个量级(几十像素), 速率只是略微衰减, 积分照常
    //     把滞后磨掉 —— 硬开关会把它整个关掉, 变成 P-only 回路, 匀速目标永远差一截。
    double integral_rate = 1.0;
    if (params_.integral_window_px > 0.0)
    {
        const double abs_error = std::abs(used_error);
        if (abs_error > params_.integral_window_px)
            integral_rate = params_.integral_window_px / abs_error;
    }

    // 误差反向时【快速回吐】: 此刻这份积分正推着指令朝错误方向走, 慢慢按 Ti 消退会
    // 在目标点附近留下一个几像素、持续一秒以上的残余偏移(实测甩枪后 8~9px, 正是老
    // 实现"落不到位"的同一个病)。回吐只在大误差阶段攒下的欠账上生效, 定了就不再动。
    if (params_.ki > 0.0 && integral_ * used_error < 0.0)
        integral_ *= std::exp(-dt / kIntegralUnwindTau);

    integral_ += used_error * integral_rate * dt;

    // "反算"式抗饱和: 积分项单独的贡献不超过输出上限; 输出顶到限幅时积分状态
    // 自己就停在边界上, 不会累积出一笔欠账。
    double integral_px = params_.ki * integral_;
    const double integral_cap = params_.kp > 0.0 ? rate_limit / params_.kp : 0.0;
    if (integral_cap > 0.0 && std::abs(integral_px) > integral_cap)
    {
        integral_px = std::copysign(integral_cap, integral_px);
        integral_ = params_.ki > 0.0 ? integral_px / params_.ki : 0.0;
    }

    // ── P 项: 连续饱和(取代死区) ────────────────────────────────────────────
    // 死区靠"误差小就不出力"防冲, 代价是在瞄点周围留一个永久盲区 —— 实测本机
    // 5px 死区下 47.9% 的帧 |ey|>5px, 而且输出以 10.2 次/秒 的频率在"动/不动"
    // 之间翻转(即所谓死区震荡: 停了 → 扰动把误差推出去 → 满增益放出来 → 过冲)。
    //
    // 这里改成: |e| <= p_full_scale_px 时原样透传(满增益, 一个像素都不丢);
    //          |e| >  p_full_scale_px 时按 p_full_scale_px/|e| 衰减。
    //
    // 关键在于这个函数【经过原点且连续】: 误差多小都会出力, 所以不存在"停了"这个
    // 状态, 也就不存在那个极限环。而大误差段被压成常数幅值, 甩枪/换靶那一下不再
    // 给出远超"走到目标所需"的输出 —— 这才是原来死区想解决的问题, 只是当时用错了工具。
    double p_px = used_error;
    if (params_.p_full_scale_px > 0.0)
    {
        const double abs_e = std::abs(used_error);
        if (abs_e > params_.p_full_scale_px)
            p_px = std::copysign(params_.p_full_scale_px, used_error);
    }

    const double d_px = params_.kd * derivative_;
    const double bracket = p_px + integral_px + d_px;

    double u = params_.kp * bracket * dt;  // 计数/拍
    const bool clipped = std::abs(u) > limit;
    u = std::clamp(u, -limit, limit);

    // ── 出口: 整条链路里唯一一次取整 ───────────────────────────────────────
    // lround(NaN/inf) 是未定义行为, 所以先确认要取整的数有限。Kp=0 而 Ki>0 这种
    // 组合下积分项没有上限可夹, 长时间跑可能溢出成 inf, 这里兜住。
    double want = u + carry_;
    if (!std::isfinite(want))
    {
        integral_ = 0.0;
        carry_ = 0.0;
        want = 0.0;
    }
    const int limit_int = outputLimit();
    long counts = std::lround(want);
    if (counts > limit_int)
    {
        counts = limit_int;
        carry_ = 0.0;
    }
    else if (counts < -limit_int)
    {
        counts = -limit_int;
        carry_ = 0.0;
    }
    else if (clipped)
    {
        // 被限幅截掉的位移不再攒成待补发的欠账。
        carry_ = 0.0;
    }
    else
    {
        carry_ = std::clamp(want - static_cast<double>(counts), -kCarryClamp, kCarryClamp);
    }

    last_p_ = p_px;
    last_i_ = integral_px;
    last_d_ = d_px;
    last_output_ = want;
    last_used_error_ = used_error;
    last_feedforward_ = feedforward;
    last_ff_scale_ = ff_scale;
    prev_error_ = error_px;

    return static_cast<int>(counts);
}

} // namespace boss
