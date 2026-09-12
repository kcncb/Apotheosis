#ifndef MOUSE_ANCHOR_OBSERVER_H
#define MOUSE_ANCHOR_OBSERVER_H

// 目标运动观测器 [单轴一个实例, 无依赖, 可单测] —— 旧 AVA 那套做法。
//
// ── 模型 ────────────────────────────────────────────────────────────────────
//   画面里的瞄点 = 目标自己的位置  -  k * (我们累计发出的计数)
//   其中 k = "每计数多少像素"(游戏灵敏度, 用户手填或用估计值)。
//   「我们自己的位移」是一个【已知输入】, 不该被滤波(那会把甩枪拖慢), 该被【扣除】;
//   扣掉之后剩下的才是目标自身运动。这就是旧 AVA 文档里写的:
//   "独立的二维位置/速度观测器……扣除观测区间内预计生效的自身指令后, 再学习目标运动"。
//
//   预测:  a_pred = a + v*dt - k*u_delayed      (u_delayed = 画面里已经生效的那批计数)
//   校正:  res = a_meas - a_pred
//          a += alpha * res
//          v += (beta/dt) * res
//
// ── 分工(2026-09-12 改定, 这是本文件最重要的约定) ───────────────────────────
//   位置: 【不给控制器用】。控制器吃的是原始瞄点(τ=0 → α=1 → pos == measurement),
//         这样"甩到瞄点"永远是一拍的事, 增益一高也不会因为滤波滞后而晃。
//   速度: 【给前馈用】。它是"扣掉自己位移之后"的目标速度, 只走 predict/lead 两项,
//         而且对外再做一阶低通 —— 因为它不进位置回路, 所以估错了也不会让准星抽。
//
//   ★ 血的教训: 输入延迟(input_lag)必须是【真实链路盲区】(≈11ms ≈ 一拍), 绝不能用
//     用户的「延迟预测」旋钮(他设了 75ms = 9 拍)。用错的表现: 减去的是 9 拍前那批
//     【甩枪途中的大指令】, 于是 v̂ = (Δm + k*u)/dt 里混进 k*u/dt ≈ 71*cmd px/s 的
//     假速度, 再乘上 predict+lead 变成几十像素的假误差 -> 准星在瞄点上按指令大小抽搐
//     (Kp=100 明显、Kp=30 看不出来, 因为 cmd ∝ Kp)。
//
// ── 两个安全阀 ──────────────────────────────────────────────────────────────
//   · 野值门限: |残差| 超过阈值就整拍丢弃, 位置原地不动(实测有 170px 的野值)。真实
//     换目标由调用方 reset(), 不靠这个门限放行。
//   · 计数延迟线: 输入用的计数必须延迟到"已经在画面里生效"的那一批, 否则甩枪时预测会
//     被自己刚发的指令带飞(残差巨大 -> 滤波器被自己的输入主导)。
#include <algorithm>
#include <cmath>

namespace boss
{

class AnchorObserver
{
public:
    // tau_ms     : 位置平滑时间常数(毫秒)。<=0 = 【不平滑】: α=1, 输出位置一拍就贴到
    //              测量值(也就是"一帧瞬时移动到瞄点")。注意 0 不是"关掉观测器" ——
    //              速度估计和"扣除自身指令"照做。要最快响应就用 0。
    // k_px_per_count: "每计数多少像素"; <=0 = 不做自身位移扣除 —— 这时速度里会混进我们自己
    //              的指令(k*u/dt ≈ 71*cmd px/s), 所以调用方必须同时禁掉速度前馈
    //              (boss_aim.cpp 就是这么做的: px_per_count<=0 时直接把速度喂 0)。
    // gate_px    : 野值门限(像素), <=0 = 不设门限。
    // input_lag_s: 计数延迟(秒) —— 画面里已经生效的那批计数。必须接近【真实链路盲区】
    //              (实测死区 ≈46ms: HID+游戏帧+显示+采集缓冲+推理, 不是软件 E2E 的 11ms),
    //              见 boss_aim.h 的 kAimDeadTimeS。估大了 == 把"还没落地的自身位移"当成
    //              已发生 == 变成正反馈, 大增益时会晃。
    // vel_tau_ms : 只作用于【对外报出的速度】的一阶低通(毫秒)。速度只喂前馈, 而"框坐标
    //              一帧一变"算出来的瞬时速度噪声极大(实测框每帧抖 1.9px / 8.3ms ≈ 230px/s,
    //              乘 0.05s 的提前量就是十几像素的假误差) —— 不压住就会把噪声灌进误差。
    //              <=0 = 不额外低通(只留给测试用)。
    void configure(double tau_ms, double k_px_per_count, double gate_px = 40.0,
                   double input_lag_s = 0.011, double vel_tau_ms = 200.0)
    {
        // 注意: 这里不再把 tau<=0 当成"整个关闭"。0 = 不平滑(α=1), 输入扣除照做。
        const double tau = (std::isfinite(tau_ms) && tau_ms > 0.0) ? tau_ms / 1000.0 : 0.0;
        const double k = (std::isfinite(k_px_per_count) && k_px_per_count > 0.0)
            ? k_px_per_count : 0.0;
        const double gate = (std::isfinite(gate_px) && gate_px > 0.0) ? gate_px : 0.0;
        const double lag = (std::isfinite(input_lag_s) && input_lag_s >= 0.0)
            ? input_lag_s : 0.011;
        const double vel_tau = (std::isfinite(vel_tau_ms) && vel_tau_ms > 0.0)
            ? vel_tau_ms / 1000.0 : 0.0;

        // ★★ 引擎【每拍】都会调这个函数(参数来自配置快照), 所以只有参数真的变了才清状态。
        //    原来这里无条件 reset() —— 那等于每拍把延迟线清空: pushed_ 永远是 1, 永远在
        //    warm-up 里, 速度恒为 0, 前馈(提前量/延迟预测)整个是死的; 更糟的是位置路径
        //    的残差里只剩 -k*u_delayed 这一项(拿"本拍刚发的指令"当"已经在画面里生效"),
        //    每拍把瞄点估计推偏 alpha*k*cmd —— 实测 α=0.295、k=0.593、cmd=50 时是 8.7px/拍,
        //    这就是"Kp=100 瞄到锚点后抽搐、Kp=30 却看着正常(因为 cmd ∝ Kp)"的真正来源。
        //    现在 τ=0(位置不过滤)让位置路径对这一项天然免疫, 这里再把状态保住, 速度才活。
        if (configured_ && tau == tau_ && k == k_ && gate == gate_
            && lag == lag_ && vel_tau == vel_tau_)
            return;
        tau_ = tau;
        k_ = k;
        gate_ = gate;
        lag_ = lag;
        vel_tau_ = vel_tau;
        configured_ = true;
        reset();
    }

    // keep_velocity: 换目标时清位置与速度(false); 只是想清野值时保留速度(true)。
    void reset(bool keep_velocity = false)
    {
        pos_ = 0.0;
        if (!keep_velocity)
        {
            vel_ = 0.0;
            vel_out_ = 0.0;
        }
        initialized_ = false;
        lag_head_ = 0;
        pushed_ = 0;
        for (double& v : lag_ring_)
            v = 0.0;
        last_gated_ = false;
        last_dt_ = 0.0;
    }

    bool enabled() const { return tau_ > 0.0; }   // 位置是否做平滑(0 = 不平滑)
    double position() const { return pos_; }
    double velocity() const
    {
        // 延迟线攒够之前不报速度: 那几拍还没法把"我们自己的位移"扣干净, 报出去会让
        // 前馈用一个错误的速度推一把(实机上就是"一按瞄准键先窜一下")。
        // 报的是低通之后的 vel_out_(见 configure 的 vel_tau_ms)。
        if (!initialized_ || pushed_ < 3)
            return 0.0;
        return vel_out_;
    }
    double rawVelocity() const { return vel_; }   // 未低通的瞬时速度(日志/诊断用)
    bool lastGated() const { return last_gated_; }

    // ── 在途自身位移(像素) —— Smith 补偿的核心 ────────────────────────────────
    // 「最近 window_s 秒内发出去的计数」× k。这批计数【已经发出去、游戏里也已经生效,
    // 只是检测画面还没回来】(链路死区, 实测 ≈46ms), 所以"我看到的误差"比"真实误差"
    // 正好大这么多 —— 控制器应该当成它们已经落地来算:
    //     e_used = e_meas - inFlightPx(死区) + v̂*死区
    // 这样"自身指令 -> 误差"这条通路就没有延迟了, 环路退化成 y_n = y_{n-1} - g*y_{n-1}:
    // 每拍收缩 (1-g) 倍, Kp=100 时 g=0.494 —— 一拍走一半, 而且不振荡。
    // 没有它, Kp=100 在 46ms 死区下是 1.9 倍临界增益, 必然极限环(实机 ±150px @5Hz)。
    // 注意: 窗口【不能超过】真实死区, 否则会把已经生效的指令再扣一遍 -> 正反馈发散。
    double inFlightPx(double window_s) const
    {
        if (k_ <= 0.0 || !std::isfinite(window_s) || window_s <= 0.0 || pushed_ <= 0)
            return 0.0;
        const double step = (last_dt_ > 0.0) ? last_dt_ : 1.0 / 120.0;
        constexpr int kRing = 64;
        const int n = std::clamp(static_cast<int>(window_s / step + 0.5), 1, kRing - 1);
        const int cnt = std::min(n, pushed_);
        double sum = 0.0;
        for (int i = 0; i < cnt; ++i)
            sum += lag_ring_[(lag_head_ - 1 - i + 2 * kRing) % kRing];
        return k_ * sum;
    }

    // measurement: 本拍瞄点(检测图像素)。counts_sent: 本拍实际下发的计数。
    // 返回本拍给控制器用的瞄点(τ=0 时 == measurement)。
    double step(double measurement, int counts_sent, double dt, bool use_measurement = true)
    {
        if (!std::isfinite(measurement))
            return initialized_ ? pos_ : measurement;
        if (!std::isfinite(dt) || dt <= 0.0)
            dt = 1.0 / 120.0;

        // 计数延迟线: 只保留最近 lag_ 秒的计数, 顺便维护累计和。
        lag_ring_[lag_head_] = static_cast<double>(counts_sent);
        lag_head_ = (lag_head_ + 1) % 64;
        ++pushed_;
        const double u_effective = delayed_counts(dt);

        // ★ 这里【不能】提前把 pos_ 写成 measurement。位置由下面的 α-β 校正负责:
        //   τ=0 时 α=1, 校正结果本来就是 pos_ = measurement(一拍贴上去)。
        //   如果在这里先赋值, 下一行的预测就变成 predicted = 测量值 + ... , 残差里
        //   的 Δm 被自己消掉, 速度就退化成 v̂ = k*u/dt —— 也就是把我们自己的指令
        //   当成了目标速度(实测 1424px/s 的假速度, 直接让前馈把准星顶飞)。
        //   (2026-09-12 实测: 这样接上去 PID 后, 静止目标的指令每秒反向 18 次。)

        // 延迟线还没攒够: 这几拍我们不知道"自己的位移什么时候生效", 任何估计都会把这个
        // 不确定性误判成目标在动。所以这段直接跟随测量, 不进 α-β 校正(也不报速度)。
        const int warmup = std::clamp(static_cast<int>(lag_ / dt + 0.5), 1, 63) + 1;
        if (pushed_ < warmup)
        {
            last_gated_ = true;   // 对外视为"本拍不可信"
            pos_ = measurement;
            vel_ = 0.0;
            vel_out_ = 0.0;
            initialized_ = true;
            last_dt_ = dt;
            return pos_;
        }

        if (!initialized_)
        {
            pos_ = measurement;
            vel_ = 0.0;
            vel_out_ = 0.0;
            initialized_ = true;
            return pos_;
        }

        // 预测(含自身指令的扣除): 我们向右发正计数, 画面里的目标会向左走。
        const double predicted = pos_ + vel_ * dt - k_ * u_effective;
        const double residual = measurement - predicted;

        // tracker 预测框(没有新观测)只推进预测, 不用它校正。
        if (!use_measurement)
        {
            last_gated_ = false;
            pos_ = predicted;
            last_dt_ = dt;
            return pos_;
        }

        // 野值门限: 整拍丢弃。位置【原地不动】, 不外推到 predicted —— 外推等于拿一个刚被
        // 判定为不可信的测量去反推轨迹, 位置会被野值带着走(实测有 170px 的野值)。
        if (gate_ > 0.0 && std::abs(residual) > gate_)
        {
            last_gated_ = true;
            last_dt_ = dt;
            return pos_;
        }
        last_gated_ = false;

        // α-β 增益: β = α²/(2-α) 是"不过冲"的经典配对。τ=0 → α=1 → β=1, 位置当拍贴到
        // 测量值, 速度退化成"一拍差分 + 自身指令扣除"(所以对外必须再低通, 见 vel_tau_)。
        const double alpha = std::clamp(dt / (tau_ + dt), 1e-4, 1.0);
        const double beta = alpha * alpha / (2.0 - alpha);
        pos_ = predicted + alpha * residual;
        vel_ += (beta / dt) * residual;

        // 对外速度(喂前馈的那一份)做一阶低通: 瞬时差分噪声太大, 乘提前量就变成假误差。
        // 注意只有【报出去的值】被低通, 滤波状态 vel_ 保持瞬时, 否则预测项会被拖慢。
        const double alpha_v = (vel_tau_ > 0.0)
            ? std::clamp(dt / (vel_tau_ + dt), 0.0, 1.0) : 1.0;
        vel_out_ += (vel_ - vel_out_) * alpha_v;

        last_dt_ = dt;
        return pos_;
    }

private:
    // 返回"这一拍真正在画面里生效的那批计数": 延迟 lag_ 秒之前的【一拍】的计数。
    // 注意是【一拍】不是"这段时间的累计" —— 模型里 a_pred = a + v*dt - k*u 中的 u 是
    // "本拍新落地的位移", 用累计和会放大 lag/dt 倍(实测直接让速度估计被自己的指令带飞)。
    double delayed_counts(double dt) const
    {
        constexpr int kRing = 64;
        int back = std::clamp(static_cast<int>(lag_ / dt + 0.5), 1, kRing - 1);
        // 延迟线还没攒够时用【最新的】计数, 绝不能用 0 ——
        // 用 0 等于这几拍我们自己的位移没被扣除, 观测器就会把它误判成目标在动, 速度瞬间
        // 被带飞(实测启动 4 拍内 v̂ 冲到 -257px/s, 位置越跟越落后)。这跟实机上
        // 一按瞄准键就先窜一下 是同一个机制。
        if (back > pushed_ - 1)
            back = std::max(0, pushed_ - 1);
        return lag_ring_[(lag_head_ - 1 - back + 2 * kRing) % kRing];
    }

    double tau_ = 0.0;
    double k_ = 0.0;
    double gate_ = 40.0;
    double lag_ = 0.011;
    double vel_tau_ = 0.2;
    bool configured_ = false;   // configure() 是否已经把参数写进去过(参数不变就不 reset)

    double pos_ = 0.0;
    double vel_ = 0.0;       // 瞬时(一拍差分 + 自身指令扣除), 供预测项使用
    double vel_out_ = 0.0;   // 低通之后对外报出的速度, 供前馈使用
    bool initialized_ = false;
    bool last_gated_ = false;
    double last_dt_ = 0.0;

    double lag_ring_[64]{};
    int lag_head_ = 0;
    int pushed_ = 0;   // 已推入延迟线的样本数(启动保护用)
};

} // namespace boss

#endif // MOUSE_ANCHOR_OBSERVER_H
