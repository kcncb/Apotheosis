#ifndef MOUSE_AIM_MOTION_H
#define MOUSE_AIM_MOTION_H

// 目标运动观测器 [每轴一个实例]。
//
// 为什么需要它 —— 先看清画面里的两个东西:
//   画面里瞄点的移动 = 目标自己的移动  +  我们自己在瞄造成的画面平移
//   后者的符号是反的: 我们向右转视角, 世界在画面里向左走。
//   所以"锚点的图像位移"这一个观测量里混了两件事, 想拿"目标自己的速度"就必须把
//   自运动减掉, 而自运动 = 计数 x (每计数多少像素)。这个"每计数多少像素"是游戏
//   灵敏度, 不能预设 —— 这一层负责【在线估它】, 而不是把它写死成一个标定常数。
//
// ── 怎么估: 窗口最小二乘, 而不是两点斜率 ────────────────────────────────────
//   模型(单轴):  a(t) = c + v*t - k*U(t)
//       a = 画面里的锚点坐标, U = 我们累计下发的计数, v = 目标自身速度, k = 每计数像素
//   在一个窗口(默认 0.35s, 每拍采样)上对 (c, v, k) 做三参数最小二乘。
//
//   为什么不能用两点的 (Δa/ΔU) 斜率: 那个式子把窗口里【目标自己走掉的那一段】全算进
//   灵敏度里了。实测: 目标 400px/s 时两点法把 0.25 估成 0.105(偏差 58%), 因为窗口里
//   目标的位移和我们自己的位移是同一个量级。三参数拟合能把两者分开 —— 前提是窗口里
//   U(t) 不是一条直线(甩枪过程是"冲出去再收住", 天然满足), 这一点用相关性卡住:
//   若 t 与 U 的相关系数超过 max_time_counts_corr 就直接放弃这个窗口。
//
// ── 安全设计(上一版预测控制器正是在这里崩的, 所以做厚一点) ─────────────────
//   · 没有激励(锚点没动 / 指令没动)的窗口直接拒绝, 绝不假装 ready。
//   · 拟合残差太大、灵敏度落在物理范围之外、样本不够 → 拒绝。
//   · 多个窗口的结果取中位数, 离群的丢掉。
//   · 未 ready 时对外输出 0, 前馈整条自动关闭 —— 退化成普通 PID, 不会乱动。
//   · 换目标只清速度与历史, k̂ 保留(灵敏度是游戏属性, 不是目标属性)。
#include <cstddef>
#include <vector>
namespace boss
{

struct AnchorMotionConfig
{
    double fit_window_s = 0.5;         // 拟合窗口长度(秒)
    int    min_samples = 12;            // 窗口内最少样本数
    double min_counts_span = 60.0;      // 窗口内 |ΔU| 下限(有指令激励)
    double min_anchor_span_px = 25.0;   // 窗口内 |Δa| 下限(有画面信号)
    double max_time_counts_corr = 0.995; // |corr(t, U)| 上限: 超过说明 k 与 v 不可分
    double max_fit_rms_px = 2.5;        // 拟合残差 RMS 上限(像素)
    // 锚点与"造成它的那批计数"之间的时间错位: 画面是 T 秒前拍的, 所以要拿那时的累计
    // 计数跟它配对, 而不是拿当前计数。对不齐的话, 快速甩枪时这个错位会被拟合当成
    // 灵敏度偏差(实测 k̂ 会偏高约 30%, 连带把提前量放大 1.3 倍)。
    double command_lag_s = 0.03;
    double px_per_count_min = 0.005;    // 灵敏度的物理范围
    double px_per_count_max = 8.0;
    int    max_fits = 5;                // 保留几个窗口结果取中位数
    int    min_fits_ready = 2;          // 至少几个才对外给数
    double velocity_window_s = 0.1;     // 速度反算用的短窗口
    double velocity_tau_s = 0.08;       // 速度输出的一阶低通时间常数
};

class AnchorMotionEstimator
{
public:
    // 默认构造必须已经处于可用状态: 内部保存拟合结果的数组长度来自配置,
    // 忘了 configure() 就会拿一个长度 0 的数组去写 —— 那是 2026-09-12 那次
    // 首次触发就闪退的原因。AimEngine 直接按成员持有本对象, 不会有人替它调用
    // configure(), 所以这里自己来。
    AnchorMotionEstimator() { configure(AnchorMotionConfig{}); }

    void configure(const AnchorMotionConfig& config);

    // 手填的"每计数像素"(0 = 仍然自动在线估算)。
    // 填了就直接用它: 前馈立刻生效, 不再等标定, 也不会因为画面乱(比如换目标抽搐)被关掉。
    // 这是游戏属性, 换灵敏度/开镜倍率要重设。
    void setManualPxPerCount(double value);

    // 换目标身份 / 丢目标: 清速度与历史, 保留 k̂(标定是游戏属性)。
    void resetTargetMotion();
    // 清掉 k̂ 本身: 换游戏 / 改灵敏度 / 用户手动重标定时用。
    void resetCalibration();

    // 每拍调用一次。
    //   anchor_px:         该轴瞄点的图像坐标(检测图像素, 浮点)
    //   counts_sent:       上一拍我们实际下发的计数(整数, 带符号)
    //   dt:                秒
    //   observation_valid: 本拍锚点是新观测(false = tracker 预测框, 不学)
    void update(double anchor_px, int counts_sent, double dt, bool observation_valid);

    bool calibrationReady() const;
    bool velocityReady() const;
    double pxPerCount() const;              // k̂, 未 ready 返回 0
    double targetVelocityPxPerSec() const;  // 目标自身速度(已扣掉自运动), px/s

    // 最近 seconds 秒内我们累计发出的计数。前馈用它算"在途自身位移"。
    double countsDuring(double seconds) const;

    // 遥测: 已接受的有效窗口数, 便于判断"还要甩几枪才能标定出来"。
    int acceptedFits() const { return fit_count_; }

private:
    struct Sample
    {
        double t = 0.0;             // 累积时间(秒)
        double anchor = 0.0;        // 像素
        double counts = 0.0;        // 累计计数(当前时刻)
        double counts_aligned = 0.0;// 累计计数(command_lag_s 之前, 与锚点同一时刻)
    };

    struct Fit
    {
        double px_per_count = 0.0;
        double velocity = 0.0;  // 像素/秒
        double rms = 0.0;
    };

    const Sample* sample_at_or_before(double t) const;
    void push_sample(double anchor, int counts_sent, double dt);
    void prune();
    void try_fit();
    void publish_from_fits(double dt);
    void update_target_velocity(double dt);

    AnchorMotionConfig config_{};
    double now_ = 0.0;
    double total_counts_ = 0.0;
    bool have_last_ = false;
    double last_anchor_ = 0.0;

    std::vector<Sample> history_;   // 按时间递增
    std::vector<Fit> fits_;         // 最近几个窗口的拟合结果
    int fit_next_ = 0;
    int fit_count_ = 0;
    double last_fit_t_ = -1.0;

    double px_per_count_ = 0.0;     // 自动估出来的 k̂
    double manual_px_per_count_ = 0.0;  // 用户手填的 k(>0 时优先, 见 setManualPxPerCount)
    double velocity_raw_ = 0.0;   // 窗口拟合给出的目标速度
    double velocity_ = 0.0;       // 低通后对外输出
    bool velocity_ready_ = false;
    bool velocity_initialized_ = false;
};

} // namespace boss

#endif // MOUSE_AIM_MOTION_H
