#ifndef MOUSE_AIM_PID_H
#define MOUSE_AIM_PID_H

// 新瞄准控制器 [PID + 前馈]。
//
// ── 单位约定(这一版最重要的设计决定) ────────────────────────────────────────
//   输入: 误差 = 瞄点 - 准星, 单位【检测图像素】, 浮点(可以带小数)。
//   输出: 本拍要下发的【整数鼠标计数】。
//   中间量(误差、前馈、积分、微分、输出指令、取整零头)一律 double, 全程浮点像素域。
//   整数只在出口出现一次 —— 先把浮点误差取整再接着算, 等于人为制造死区:
//   差 0.4 像素时取整成 0, 这个 0 再乘上任何增益还是 0, 于是就"卡在差两像素不动"。
//   这里保留取整零头(亚计数余量), 攒够一个计数下一拍自然会发出去。
//
// ── 与已删除的旧 AVA PIDF 的区别 ────────────────────────────────────────────
//   旧链路在回路内部放了写死的"每计数多少像素"标定常数, 标定不准就在锚点附近每拍
//   反向自激, 所以整条被删。这里需要同一个量, 但它是【在线估出来的】(见
//   aim_motion.h), 有激励判据、有上下限、离散度不合格就不 ready; 未 ready 时前馈
//   整条关闭, 退化成普通 PID, 绝不会因为一个写死的常数而自激。
//
// ── 形式(dt 归一化) ─────────────────────────────────────────────────────────
//       e_used = e + 前馈
//       u      = dt * Kp * [ e_used + Ki*∫e_used dt + Kd*de/dt ]     u 单位: 计数/拍
//       Kp [计数/(像素·秒)]  回路速度。唯一与游戏灵敏度("每计数多少像素")挂钩的旋钮。
//       Ki [1/秒]            积分速率: 残余误差被抹掉的快慢, Ti = 1/Ki。
//       Kd [秒]              微分时间: 阻尼、压过冲; 微分输入先过一阶低通, 不放大抖动。
//
//   为什么三个增益都带时间量纲: 链路死区是按【秒】存在的(采集->推理->tick, 再加
//   HID->游戏->显示)。老实现是"每拍增益", 60fps 与 240fps 下死区折合的拍数差 4 倍,
//   同一组参数手感完全不同。归一化后 Kp 只管"多快", Ki/Kd 只管"形状", 而且这两个
//   形状参数与游戏灵敏度无关 —— 只有一个旋钮需要按灵敏度调。
//
//   【死区已删除(2026-09-12)】原来有一个 deadband_px 死区, 靠"误差小就不出力"防过冲。
//   它的代价是在瞄点周围留一个永久盲区, 并且制造"停—放"极限环: 停了 → 扰动把误差
//   推出去 → 满增益+攒下的积分一起放出来 → 过冲。加迟滞只把环加宽, 没解决过冲,
//   实机复测输出仍然以 10.2 次/秒 在"动/不动"之间翻转。
//   现在改用 p_full_scale_px【连续饱和】: 函数经过原点且连续, 不存在"停了"这个状态,
//   所以没有极限环; 而大误差段被压成常数幅值, 甩枪/换靶不再过冲 ——
//   那才是死区本来想解决的问题。
//
//   前馈 = -在途自身位移 + 目标自身速度 x (延迟预测 + 提前量)
//
//   【为什么积分积的是 e_used 而不是 e】 这是提前量能不能生效的关键。回路(含积分)
//   把 e_used 归零, 所以前馈给出的偏移能一直保持住; 如果前馈加在回路外面, 积分会把
//   它整个抵消掉, 提前量会在几百毫秒内自己消失。
//
//   【两个时间旋钮的分工, 不要混】
//     predict_time_s(延迟预测): 补偿链路盲区 —— 已经发出、画面还没显示出来的自身位移,
//        以及这段时间里目标自己的位移。稳态下这两项正好抵消, 所以它【只影响瞬态】
//        (甩枪、急停、目标机动), 不产生任何稳态瞄偏。物理量就是: 从画面拍摄到指令
//       生效的总延迟。
//     lead_time_s(提前量): 主动瞄目标的未来位置。稳态下就是 v*lead_time 的提前偏移,
//        专门用来打移动靶/有飞行时间的弹道。默认 0 = 关。
//   注意: 匀速移动的目标用真积分本来就【没有】稳态滞后(回路是二阶无差的, 延迟不产生
//   稳态偏差), 所以"追不上匀速目标"这件事靠 Ki 解决, 不需要提前量; 提前量解决的是
//   "想主动打目标前方"和"目标机动/甩枪时的瞬态滞后"。
//
// ── 调用约定 ────────────────────────────────────────────────────────────────
//   pid.configure(in.pid_x);
//   out.dx = pid.step(anchor.x - crosshair_x, dt, {v̂, k̂*在途计数});
//   换目标身份(motion_suppressed)/丢失目标时: pid.reset(); estimator.resetTargetMotion();
namespace boss
{

struct AimPidParams
{
    // 默认值是按"50ms 链路死区 + 每计数 0.25 像素"的闭环扫描定的(见 tests/aim_pid_test.cpp
    // 与提交说明), 判据是收敛时间与不发散:
    //   Kp=15 在 k 高到约 1.9 px/count 时仍不发散; 手感拖沓就往上加, 开始抖就退回来。
    //   Ki 只在小误差窗口里工作, 作用是消掉残余偏置("落不到位"), 不参与甩枪。
    //   Kd 很小: 检测框每帧一个计数台阶(0.25px)在微分眼里是 ~60px/s 的尖峰, Kd 大了
    //   会把这台阶放大成抖动, 实测 Kd 从 0 加到 0.15 收敛反而变差。
    double kp = 20.0;              // 计数/(像素·秒): 唯一与游戏灵敏度挂钩的旋钮
    double ki = 1.0;               // 1/秒: 积分速率(Ti = 1s)。匀速目标的滞后靠它磨掉,
                                   // 必须够快: 滞后量 = v/(k*Kp), 消掉它需要约 Ti。
    double kd = 0.01;              // 秒: 微分时间
    double integral_window_px = 40.0;  // 积分速率在这个误差以内是满的, 超过后按
                                       // window/|e| 衰减(0 = 不衰减)。防止甩枪阶段在
                                       // 死区里把积分绕成一大笔欠账。注意不能用硬开关:
                                       // 跟匀速目标时的误差本身就有几十像素, 关掉就等于
                                       // 退回 P-only, 永远差一截。
    double predict_time_s = 0.05;  // 链路延迟预测(秒), 0 = 关
    double lead_time_s = 0.0;      // 主动提前量(秒), 0 = 关

    // ── P 项饱和(取代死区) ──────────────────────────────────────────────────
    // 死区是"停—放"式防冲, 代价是永久丢精度(实测本机 5px 死区让 47.9% 的帧
    // |ey|>5px)。这里改成【连续饱和】: |e| 超过 p_full_scale_px 之后 P 项不再
    // 线性增长, 只保留方向; |e| 在该值以内则原样透传(满增益)。
    //
    //   为什么这样就不会冲过头: 甩枪/换靶瞬间的 e 是几百像素, 线性 P 会给出一个
    //   远超"走到目标"所需的输出 —— 那正是过冲的来源。饱和之后大误差段的等效
    //   增益自动按 p_full_scale_px/|e| 衰减, 而小误差段一个像素都不丢。
    //   与死区的本质区别: 这个函数在 0 附近是【连续的、过原点的】, 不制造
    //   "误差小于 X 就不出力"的盲区, 所以不会形成极限环(实测死区会以
    //   10.2 次/秒 的频率翻转输出动/不动)。
    //
    // 0 = 关闭饱和(纯线性)。默认 100px: 本机 FOV 半径 208px, 交战时典型误差
    // 十几像素, 所以 100px 只削甩枪/换靶那一下, 不影响跟枪。
    double p_full_scale_px = 100.0;
    int limit_counts = 0;          // 每拍输出上限(计数), 0 = 用内置上限。
};

// 观测器每拍给控制器的前馈输入。两个都为 0 时, 控制器就是普通 PID。
struct AimPidFeedback
{
    double target_velocity_px_s = 0.0;    // 目标自身速度(已扣掉自运动)
    double pending_self_motion_px = 0.0;  // 在途自身位移 = k̂ * 最近 predict_time 内的计数
};

class AimPid
{
public:
    // 每拍都可以调。非法值(非有限/负数)自动退回内置默认, 不会因为填错一个数自激。
    void configure(const AimPidParams& params);

    // 清积分/微分/零头。丢目标、换目标身份、重新开始跟踪时调用。
    void reset();

    // error_px: 像素误差(浮点); dt: 两次 tick 的真实墙钟间隔(秒)。
    // 返回本拍要下发的整数鼠标计数。坏测量(NaN/inf)返回 0 且保持状态。
    int step(double error_px, double dt, const AimPidFeedback& feedback = {});

    int outputLimit() const;

    // 「延迟预测」的值(秒)。★ 2026-09-12 起调用方【不能】再拿它当观测器的输入延迟用:
    // 那会把"画面里已经生效的自身位移"估成 9 拍前那批大指令(见 boss_aim.h 的
    // kAnchorObserverInputLagS)。它现在只表示"链路盲区里目标走掉的距离"。
    double predictTimeSeconds() const;

    // 本拍实际用于输出缩放的 dt(秒, 已夹在典型拍间隔附近)。遥测/排查用。
    double usedDt() const { return last_dt_; }

    // 遥测/回归测试用, 不参与控制。
    double outputCounts() const { return last_output_; }     // 取整前的浮点指令(含上一拍零头)
    double usedErrorPx() const { return last_used_error_; }  // 送给 P/I 的误差(含前馈)
    double feedforwardPx() const { return last_feedforward_; }
    double feedforwardScale() const { return last_ff_scale_; }  // 速度前馈噪声门(0~1)
    double pTerm() const { return last_p_; }
    double iTerm() const { return last_i_; }
    double dTerm() const { return last_d_; }
    double integralState() const { return integral_; }       // 像素·秒
    double derivativeState() const { return derivative_; }   // 像素/秒
    double carryState() const { return carry_; }             // 未发出的零头(计数)

private:
    AimPidParams params_{};
    bool configured_ = false;
    bool first_ = true;

    double integral_ = 0.0;    // 像素·秒
    double derivative_ = 0.0;  // 像素/秒
    double prev_error_ = 0.0;  // 像素
    double carry_ = 0.0;       // 上一拍取整丢掉的零头(计数)

    double last_p_ = 0.0;
    double last_i_ = 0.0;
    double last_d_ = 0.0;
    double last_output_ = 0.0;
    double last_used_error_ = 0.0;
    double last_feedforward_ = 0.0;
    double last_ff_scale_ = 0.0;
    double last_dt_ = 0.0;  // 本拍用于输出缩放的 dt
};

} // namespace boss

#endif // MOUSE_AIM_PID_H
