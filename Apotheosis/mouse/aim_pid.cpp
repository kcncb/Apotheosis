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

// dt 的有效下限, 用于换算"窗口占几拍"。避免 dt 极小导致 window_slots 溢出 int。
constexpr double kSlotDtFloor = 1.0 / 1000.0;

// 在途补偿强度 beta 的上限。★ 这不是"可用区间", 只是挡住填错量级的配置。
// 实测: beta=2.0 在 60fps 已经发散(尾段 56px), 2.5 同样发散, 默认 1.2 是选定值。
// 详见 aim_pid.h 里 inflight_beta 的扫描表。
constexpr double kMaxInflightBeta = 3.0;

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
    // 【已删除】predict_time_s / lead_time_s 不再参与控制。旧配置文件里即使有这两个
    // 键也直接忽略 —— 它们曾是与物理量脱钩的可调偏移, 会造成稳态瞄偏(见头文件说明)。
    if (!std::isfinite(p.p_full_scale_px) || p.p_full_scale_px < 0.0)
        p.p_full_scale_px = fallback.p_full_scale_px;
    if (!std::isfinite(p.integral_window_px) || p.integral_window_px < 0.0)
        p.integral_window_px = fallback.integral_window_px;
    p.limit_counts = std::clamp(p.limit_counts, 0, 100000);

    // 在途补偿的两个参数。坏值退回默认(0 = 关闭), 不会因为填错自激。
    if (!std::isfinite(p.inflight_beta) || p.inflight_beta < 0.0)
        p.inflight_beta = 0.0;
    // ★ 注意 beta 的语义是"每拍扣掉当前输出的 beta 倍"(见 .h 的说明), 所以
    //   默认值 1.2 是【合法且正常】的, 不能像早先那样夹在 [0,1] —— 那会把默认值
    //   悄悄改成 1.0。上限仍然要有, 但按实测的稳定性边界放到 3.0:
    //   beta=2.0 在 60fps 已经开始发散(尾段 56px), 2.5 同样发散, 所以 3.0 是
    //   "明显过头"的位置, 用来挡住填错量级的配置, 不是可用值。
    p.inflight_beta = std::clamp(p.inflight_beta, 0.0, kMaxInflightBeta);
    if (!std::isfinite(p.inflight_window_s) || p.inflight_window_s <= 0.0)
        p.inflight_window_s = fallback.inflight_window_s;
    // ★ 窗长上限: 不许超过实测链路死区(46ms)太多。
    //
    //   为什么是 46ms 而不是别的数: 从实机振荡周期反推 —— Kp=100 时误差以
    //   5.00Hz/200ms 摆动 = 24 拍, 离散"积分+纯延迟"环路的振荡周期是 2(2d+1) 拍
    //   => d = 5.5 拍 ≈ 46ms。用"软件 E2E ≈ 11ms"会漏掉 HID + 游戏帧 + 显示 +
    //   采集缓冲, 让速度估计混进 k*Δu/dt 的假速度。
    //
    //   这里给到 3 倍(138ms)的宽容度只是为了不让用户填错一个稍大的值就完全失效,
    //   但【默认就是 46ms】, 而且文档明确要求不要超过它。
    constexpr double kMaxInflightWindowS = 0.138;
    p.inflight_window_s = std::clamp(p.inflight_window_s, kMinDt, kMaxInflightWindowS);

    // 尺度下限保护: 允许 [0, 1]。
    if (!std::isfinite(p.scale_min_gain) || p.scale_min_gain < 0.0)
        p.scale_min_gain = fallback.scale_min_gain;
    p.scale_min_gain = std::clamp(p.scale_min_gain, 0.0, 1.0);

    // 亚量化保护的相对判据: 必须在 (0, 1] 内, 否则条件①永不成立或恒成立。
    if (!std::isfinite(p.subquantum_reverse_ratio)
        || p.subquantum_reverse_ratio <= 0.0 || p.subquantum_reverse_ratio > 1.0)
        p.subquantum_reverse_ratio = fallback.subquantum_reverse_ratio;

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
    last_raw_error_ = 0.0;
    last_inflight_px_ = 0.0;
    last_sq_held_ = false;
    // 在途记账一起清: 换了目标/重新开始跟踪时, 上一段"已发未生效"的计数不再
    // 属于当前误差, 留着会让控制器在新目标上错误地提前收手。
    for (double& v : in_flight_queue_)
        v = 0.0;
    in_flight_len_ = 0;
    in_flight_ = 0.0;
    // 注意: scale_ 【不】在这里复位。尺度是当前目标的观测量, 由引擎每拍注入;
    // 复位它只会造成"换目标后第一拍用 1.0 出力"的一次突跳, 而引擎紧接着就会
    // 注入新的尺度。保持上一次的值比清成 1.0 更平滑。
}

void AimPid::setScale(double s)
{
    if (!std::isfinite(s) || s <= 0.0)
    {
        scale_ = 1.0;   // 坏值 = 不做尺度调制, 而不是"增益变 0"
        return;
    }
    scale_ = s;
}

int AimPid::outputLimit() const
{
    return params_.limit_counts > 0 ? params_.limit_counts : kDefaultLimitCounts;
}

int AimPid::step(double error_px, double dt)
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
        last_raw_error_ = 0.0;
        last_inflight_px_ = 0.0;
        last_sq_held_ = false;
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

    // ── 等效增益: 距离尺度调制 (G6) ─────────────────────────────────────────
    //   Kp_eff = Kp · max(s, scale_min_gain)
    //
    // 近处(框大, s 大) -> 等效增益高 -> 拉枪/跟随更快;
    // 远处(框小, s 小) -> 等效增益低 -> 保守, 不把量化噪声放大成抖动。
    //
    // ★ scale_min_gain 是"远处也要能动"的兜底: s 再小也不许让等效增益塌到 0,
    //   否则准星在远处目标上会慢慢漂走(比"慢"更糟的失效模式)。
    //
    // ★ 尺度【只】作用在增益上, 不参与任何"提前量 = v·T·f(s)"的构造 ——
    //   屏幕速度本身已是 1/d 的投影, 那样做等于把深度算两遍
    //   (见 aim_scale.h 的 ⚠️ 与 docs/aiming-controller.md 的纠错记录)。
    const double s_eff = (params_.scale_min_gain > 0.0)
        ? std::max(scale_, params_.scale_min_gain)
        : scale_;
    const double kp_eff = params_.kp * s_eff;
    last_kp_eff_ = kp_eff;

    // ── 微分项: 对【测量误差】求导(像素/秒), 一阶低通 ────────────────────────
    // 用测量误差而不是 e_used: 在途补偿本身变化很快, 拿它求导只会把噪声引进来。
    //
    // 注: 试过"单拍跳变不喂微分"(想挡检测框整帧跳 20~45px 造成的假速度), 但甩枪/换靶
    // 时【误差本身】就会单拍跳几百像素 —— 那是真的, 冻掉它反而让到位过冲变差(实测
    // 静止目标尾段误差从 <2px 涨过门槛)。
    const double raw_derivative = (error_px - prev_error_) / dt;
    const double alpha = std::clamp(dt / (kDerivativeTau + dt), 0.0, 1.0);
    derivative_ += (raw_derivative - derivative_) * alpha;

    // ── 在途补偿 (Smith 类) ────────────────────────────────────────────────
    // 先算"已发出但尚未生效"的计数 N。补偿【在输出端按计数扣】, 不在像素域做。
    //
    //   u = kp_eff*dt*[e + ki*∫e + kd*ė]  -  inflight_beta * N / W   [计数]
    //
    //   N = 窗口内下发计数之和, W = 窗口折算成几拍。
    //   kp 与 k̂ 都不参与 —— 这是唯一不需要任何 counts<->px 换算的写法, 因此天然
    //   满足"回路里不得出现需要标定的 k̂"。
    //
    // ★★★ 三处都是实测踩坑后改定的, 每一处都别再改回去 ★★★
    //
    //   ① 【必须在计数域, 不能在像素域】
    //      老写法 e_used = e - beta*N/(kp*dt) 展开后 u = kp*e*dt - beta*N, 补偿
    //      本身恰好就是 -beta*N 个计数, 那个 (kp*dt) 除法纯属多余。但它用【增益 kp】
    //      去除【计数】, 等于隐含假设 kp = 1/k̂ —— 而 kp 是用户调的增益、k̂ 是游戏
    //      灵敏度, 毫不相干。实测(k̂=0.593, Kp=40, v=200px/s, N≈17):
    //        17/(40/120) = 51px 的补偿量, 而 17 个计数真实只对应 10.1px 位移, 过补 5 倍。
    //
    //   ② 【积分必须积原始 e, 不能积补偿后的 e_used】
    //      e_used 被补偿压到 0 之后积分就再也不积累了(实测稳态逐拍: e=+19.6px 而
    //      e_used=+0.00, int=+0.00), 于是纯 P 的固有滞后 v/(kp*k̂) 永远消不掉, 回路
    //      停在"e_used=0"这个假平衡点上 —— 表现为一个【随 beta 线性增长】的固定瞄偏:
    //          beta 0.1 -> 3.9px, 0.2 -> 9.3px, 0.4 -> 20.0px, 0.6 -> 33.0px
    //      这正是 §4.2 明令禁止的"稳态瞄偏随速度增长"。改成 ①+② 之后:
    //          静止 400px 阶跃 尾段 0.295px
    //          匀速 200px/s 跟随 尾段 0.822px   (改之前 577px, 完全失稳)
    //
    //   ③ 【必须除以窗口拍数 W 归一化】—— 帧率无关性的关键
    //      N 是"窗口内计数的【和】", 而每拍都要扣一次 beta*N。设窗口 W 拍、每拍发 c
    //      个计数, 稳态 N = W*c, 于是每拍扣 beta*W*c —— 而每拍实际只发 c, 补偿被
    //      放大了 W 倍! W 随帧率变(0.046s 在 60fps 是 3 拍、120fps 是 6 拍、1000fps
    //      是 46 拍), 所以同一个 beta 在不同帧率下强度差异极大:
    //          实测 60/120/240fps 尾段都是 0.30px 正常, 500fps 涨到 0.73px,
    //          1000fps 恶化到 8.30px —— 而 1000fps 下把窗口从 46 拍收到 10 拍就
    //          恢复到 0.295px, 证实了就是 W 的放大。
    //      除以 W 之后, 稳态每拍扣 beta*c(即"扣掉当前输出流的一部分"), 与帧率解耦。
    //
    //   ④ 补偿方向【不对称】: 补【不足】只是回到原来的延迟(安全); 补【过头】会把已
    //      生效的指令再扣一遍 → 正反馈发散。实测 beta 可用区间 0.2~0.6, 0.4 最优,
    //      ≥0.8 明显变差。窗口不许超过真实死区 46ms —— 用"软件 E2E ≈ 11ms"会漏掉
    //      HID + 游戏帧 + 显示 + 采集缓冲。
    //
    // 于是现在的分工是干净的:
    //   补偿项  -(beta/W)*N   只管"压住我自己制造的超调"(暂态)
    //   积分项  ki*∫e         只管"磨掉纯 P 的稳态滞后"(稳态)
    // 两者不再互相吃掉对方的作用。
    const bool inflight_on = (params_.inflight_beta > 0.0);

    // 窗口折算成"保留几拍"。至少 1 拍, 至多队容量。★ 补偿与记账必须用【同一个】
    // window_slots, 否则归一化除错。
    const double slot_dt = std::max(dt, kSlotDtFloor);
    int window_slots = static_cast<int>(std::lround(params_.inflight_window_s / slot_dt));
    window_slots = std::clamp(window_slots, 1, kInFlightCap);

    // N/W: 窗口内计数的【平均值】, 即"最近这批在途指令的典型每拍计数"。
    const double inflight_counts =
        inflight_on ? params_.inflight_beta * (in_flight_ / static_cast<double>(window_slots))
                    : 0.0;

    last_used_error_ = error_px;
    last_raw_error_ = error_px;
    last_inflight_px_ = 0.0;

    // ── 三个分项先折算到【像素】, 再统一乘 kp_eff*dt ─────────────────────────
    //   e               像素
    //   ki * 积分状态   [1/s] * [像素*秒] = 像素
    //   kd * 微分       [秒] * [像素/秒]  = 像素
    // 括号里是"等效像素误差", 乘 kp_eff[计数/(像素*秒)] * dt 得到本拍计数。三个增益
    // 都带时间量纲, 所以换帧率不用重调 —— 链路死区是按【秒】存在的。
    const double rate_limit = limit / dt;  // 输出上限折算成计数/秒

    // ── 积分项: 积【原始误差】e(像素·秒) ───────────────────────────────────
    // ★ 积的是 e, 不是"补偿后的 e_used" —— 理由见上面 ②, 这是稳态滞后能否被磨掉的
    //   关键。在途补偿改成输出端扣计数之后, 这里天然就只剩原始误差了。
    //
    // 积分速率对【大误差】按 window/|e| 衰减, 不做硬开关:
    //   · 甩枪阶段(误差几百像素)几乎不积, 否则会在 46ms 死区里攒出一大笔欠账,
    //     到位后要几百毫秒才吐干净(实测: 硬开关会留下 50px 级摆动)。
    //   · 跟匀速目标时误差是 v/(k*Kp) 这个量级(几十像素), 速率只是略微衰减, 积分
    //     照常把滞后磨掉 —— 硬开关会把它整个关掉, 变成 P-only 回路。
    double integral_rate = 1.0;
    if (params_.integral_window_px > 0.0)
    {
        const double abs_error = std::abs(error_px);
        if (abs_error > params_.integral_window_px)
            integral_rate = params_.integral_window_px / abs_error;
    }

    // 误差反向时【快速回吐】: 此刻这份积分正推着指令朝错误方向走, 慢慢按 Ti 消退会
    // 在目标点附近留下一个几像素、持续一秒以上的残余偏移(实测甩枪后 8~9px, 正是老
    // 实现"落不到位"的同一个病)。回吐只在大误差阶段攒下的欠账上生效。
    if (params_.ki > 0.0 && integral_ * error_px < 0.0)
        integral_ *= std::exp(-dt / kIntegralUnwindTau);

    integral_ += error_px * integral_rate * dt;

    // "反算"式抗饱和: 积分项单独的贡献不超过输出上限; 输出顶到限幅时积分状态
    // 自己就停在边界上, 不会累积出一笔欠账。
    //
    // ★ 用 kp_eff 而不是名义 kp: 积分项最终乘的是 kp_eff, 所以夹取也必须按它算,
    //   否则尺度小的时候积分会被允许攒到远超限额的大小。
    double integral_px = params_.ki * integral_;
    const double integral_cap = kp_eff > 0.0 ? rate_limit / kp_eff : 0.0;
    if (integral_cap > 0.0 && std::abs(integral_px) > integral_cap)
    {
        integral_px = std::copysign(integral_cap, integral_px);
        integral_ = params_.ki > 0.0 ? integral_px / params_.ki : 0.0;
    }

    // ── P 项: 连续饱和(取代死区) ────────────────────────────────────────────
    // |e| <= p_full_scale_px 时原样透传(满增益, 一个像素都不丢);
    // |e| >  p_full_scale_px 时按 p_full_scale_px/|e| 衰减。
    // 关键在于这个函数【经过原点且连续】: 误差多小都会出力, 所以不存在"停了"这个
    // 状态, 也就不存在那个极限环。
    double p_px = error_px;
    if (params_.p_full_scale_px > 0.0)
    {
        const double abs_e = std::abs(error_px);
        if (abs_e > params_.p_full_scale_px)
            p_px = std::copysign(params_.p_full_scale_px, error_px);
    }

    const double d_px = params_.kd * derivative_;
    const double bracket = p_px + integral_px + d_px;

    double u = kp_eff * bracket * dt;  // 计数/拍

    // ★ 在途补偿就在这里扣, 纯计数域, 不经过任何像素换算(见上面长注释 ①)。
    u -= inflight_counts;

    const bool clipped = std::abs(u) > limit;
    u = std::clamp(u, -limit, limit);

    // ── 亚量化保护: 补偿不许把指令顶反号 (无量纲, 不需 k̂) ──────────────────
    //
    // 【为什么需要】锁死后残差中位只有 0.26px, 而一个计数对应的画面位移是 k̂ 像素 ——
    // 也就是说稳态残差比"一个量子"还小。此时在途补偿(beta*N/W, N 是窗口内几个计数)
    // 足以把输出从"朝目标的小正数"顶成负数 → P 项反向 → 下一拍打对面 → 自持成
    // ±1 计数的 60Hz 抖动(日志实证: Kp=100 时 43~54% 的帧在下发 ±1、符号翻转 79%;
    // 同批帧 Kp=30 的 Y 轴只有 13%; 框自身逐帧变化中位仅 0.06px, 不是检测噪声)。
    //
    // 【判据】三条全部无量纲, 不需要知道"半个量子等于多少像素":
    //   ① 【本拍已经接近到位】: |kp_eff·e·dt| 小于一个计数。
    //      这是判据的核心 —— 亚量化状态的【定义】就是"这一拍想发的量不足一个计数"。
    //   ② 补偿【把指令顶成了反号】: 未补偿时朝目标(ucomp 与 error_px 同号),
    //      补偿后反了号(u 与 ucomp 异号)。
    //   ③ 反号后的幅度远小于未补偿时的幅度。
    // 三条都满足 => 认定这是亚量化翻号, 把输出夹到 0(不越过零点)。
    //
    // ★★ 判据①为什么是必需的(实测踩出来的) ★★
    //   少了①会【误伤甩枪段】。看实测的甩枪轨迹(Kp=100, 400px 阶跃, 逐拍打印):
    //       i=..6  c= -69  (窗口在灌满, inF=-866)
    //       i=..7  c=  -6  (窗口开始排空, inF=-672)
    //       i=..8  c= +41  <- 保护在这里把 +41 夹成了 0
    //   那一拍 e 还有 -162px(远未到位), 但补偿量因为【窗口排空】而从 -997 掉到 -672,
    //   这个"补偿量自身的变化"就足以把指令从 -6 顶成 +41。判据②③看到的就是这个,
    //   于是误判成抖动。加上①之后, |kp_eff·e·dt| = 100*162*0.0083 = 135 个计数,
    //   远大于 1, 保护直接不参与 —— 甩枪/跟枪逐拍输出因此【完全不变】。
    //
    // ★ 只夹【补偿造成的翻号】, 不碰 P/I/D 本身 —— 它们的算法一个字节都没变。
    // ★ 这个判据替代了老实现里基于 px_per_count 的"半个量子"检查, 后者需要 k̂。
    bool sq_comp_clamped = false;
    if (params_.subquantum_guard && inflight_on && inflight_counts != 0.0)
    {
        // ① 本拍"想发的量"不足一个计数 —— 亚量化状态的充要特征。
        const double uncomp = u + inflight_counts;   // 不做补偿时本拍会发的量
        const bool near_zero_demand = std::abs(uncomp) < 1.0;
        // ②③ 补偿把指令顶反了号, 且顶过去之后幅度很小。
        const bool comp_flipped = (u * uncomp < 0.0);
        const bool sub_quantum =
            std::abs(u) <= std::abs(uncomp) * params_.subquantum_reverse_ratio;
        if (near_zero_demand && comp_flipped && sub_quantum)
        {
            // 夹到"刚好抵消"为止(u == 0), 不越过零点。
            u = 0.0;
            sq_comp_clamped = true;
        }
    }

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

    // ── 亚量化保护 ②: 不足一个计数时停发, 并【冻结】零头 (无量纲) ───────────
    //
    // 【为什么】停发能立刻掐断 ±1 计数的自持翻号 —— 那个抖动的每一拍都只有
    // 1 个计数, 所以"本拍想发的量不足 1 个计数"正是它的特征签名。
    //
    // 【为什么零头要冻结而不是清零】(实测) 清零会把"20px/s 慢速横移所需的亚计数
    // 需求"一起丢掉, 滞回从 0.8px 涨到 1.2px。冻结则让那点需求继续攒着, 攒够
    // 一个计数自然会发出去。
    //
    // 【判据】|want| < 1(取整前不足一个计数)。配合上面的 sq_comp_clamped 使得这条
    // 保护只在"补偿已经被判定为亚量级翻号"时生效 —— 这样甩枪/跟枪的逐拍输出【完全
    // 不变】(回归断言位相同)。
    //
    // ★ 单独用 |want| < 1 是不够的: 大误差段刚开始减速时也会短暂出现 |want| < 1,
    //   那时停发会拖慢收敛。加上 sq_comp_clamped 之后, 保护只在误差已被压到亚计数
    //   级的稳态生效。
    const int limit_int = outputLimit();
    const bool sq_hold = params_.subquantum_guard && sq_comp_clamped && std::abs(want) < 1.0;
    last_sq_held_ = sq_hold;

    long counts;
    if (sq_hold)
    {
        // 本拍不发。★ 零头【冻结】(carry_ 原样保留), 不清零、也不累加 want ——
        // 因为 want 里含在途补偿的效果, 把它累进 carry_ 等于承认了那份补偿。
        counts = 0;
    }
    else
    {
        counts = std::lround(want);
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
    }

    // ── 在途记账推进 ────────────────────────────────────────────────────────
    // 把本拍【真正下发】的计数推进环, 同时把过了窗口的那一格移出。
    //
    // 用 counts(取整后的整数)而不是 want: 下游 sendRawMove 拿到的就是这个整数,
    // 未来会生效的位移就是它。carry_ 那点零头不在途(它还没发出去)。
    if (inflight_on)
    {
        // window_slots 已在上面算好(补偿的归一化与这里必须用同一个值)。
        // 1) 若已有 window_slots 拍, 弹出最老的一拍(队尾), 它视为已生效。
        if (in_flight_len_ >= window_slots)
        {
            in_flight_ -= in_flight_queue_[in_flight_len_ - 1];
            --in_flight_len_;
        }

        // 2) 本拍计数插入队首(整体后移一格)。窗口 <= 128 且每拍只做一次,
        //    128 次 double 拷贝在 240fps 下也远低于噪音水平。
        const int move = std::min(in_flight_len_, kInFlightCap - 1);
        for (int k = move; k > 0; --k)
            in_flight_queue_[k] = in_flight_queue_[k - 1];
        if (in_flight_len_ < kInFlightCap)
            ++in_flight_len_;

        const double pushed = std::clamp(static_cast<double>(counts),
                                         -static_cast<double>(limit_int),
                                         static_cast<double>(limit_int));
        in_flight_queue_[0] = pushed;
        in_flight_ += pushed;
    }

    last_p_ = p_px;
    last_i_ = integral_px;
    last_d_ = d_px;
    last_output_ = want;
    prev_error_ = error_px;

    return static_cast<int>(counts);
}

} // namespace boss
