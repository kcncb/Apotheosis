#include "pidf_mode1_exact.hpp"

#include <algorithm>
#include <cmath>

namespace cvm::recovered {
namespace {

double signum(double value) noexcept {
    return value > 0.0 ? 1.0 : (value < 0.0 ? -1.0 : 0.0);
}

void reset_feedforward(PidfMode1State& s) noexcept {
    s.ff_previous_error_x = s.ff_previous_error_y = 0.0;
    s.ff_state_x = s.ff_state_y = 0.0;
    s.correction_accum_x = s.correction_accum_y = 0.0;
    s.predicted_minus_move_x = s.predicted_minus_move_y = 0.0;
    s.ff_error_x = s.ff_error_y = 0.0;
    s.ff_derivative_x = s.ff_derivative_y = 0.0;
    s.ff_output_x = s.ff_output_y = 0.0;
    s.dynamic_lr_x = s.base_lr_x;
    s.dynamic_lr_y = s.base_lr_y;
}

void compute_opposition_flags(PidfMode1State& s,
                              double error_x,
                              double error_y,
                              double dt) noexcept {
    s.scratch_x = s.integral_x * error_x;
    s.integral_opposes_error_x =
        s.integral_enabled_x && s.scratch_x < 0.0;
    s.scratch_y = s.integral_y * error_y;
    s.integral_opposes_error_y =
        s.integral_enabled_y && s.scratch_y < 0.0;

    s.ff_output_x = dt * s.ff_state_x;
    s.ff_output_y = dt * s.ff_state_y;
    s.scratch_x = s.ff_output_x * error_x;
    s.ff_opposes_error_x = s.scratch_x < 0.0;
    s.scratch_y = s.ff_output_y * error_y;
    s.ff_opposes_error_y = s.scratch_y < 0.0;
    s.damp_x = s.integral_opposes_error_x || s.ff_opposes_error_x;
    s.damp_y = s.integral_opposes_error_y || s.ff_opposes_error_y;
}

// 每轴各用自己的尺度。
//
// 原实现在入口把输入里的两个半径塌成了 min(radius_x, radius_y), 于是两个轴共用
// 同一个尺度 —— 对人形框(例如 30 宽 × 80 高)就是【纵向的尺度被横向上限决定】:
// min 取到 30, 开锁定强度时自适应半径 = 15px, 也就是说纵向只要偏 15px 增益就塌掉,
// 而那个框有 80 高、人还在身上。
// 后果: 纵向的增益/前馈权限远弱于横向 → 上下移动的目标(起跳、蹲起)咬不住。
//
// 输入本来就把两个半径分开带了(PidfInputExact 的 radius_x/radius_y 来自框宽/框高),
// 这里只是不再把它们合并 —— 与这套控制器"X/Y 各自独立增益"的既有设计保持一致。
// 高 Kf 路径(仅 kf>1 时启用)与反向(damp)时的半径收缩因子都是 0.25。
// 实测(kf=2, aim_scenario_sim): 因子 0.25 -> 17.58/27.99(3帧/6帧),
// 0.5 -> 18.52/30.10, 1.0 -> 18.09/29.40, 故统一用 0.25。
// 注意这条分支在 kf<=1(默认值)时不会进入, 所以该改动对默认手感零影响。
void choose_adaptive_radius(PidfMode1State& s,
                            double radius_x,
                            double radius_y,
                            bool consider_high_kf,
                            double radius_scale) noexcept {
    s.adaptive_radius_x = radius_x * radius_scale + 0.000001;
    if (consider_high_kf && s.high_kf_enabled_x)
        s.adaptive_radius_x = radius_x * 0.25 + 0.000001;
    if (s.damp_x)
        s.adaptive_radius_x = radius_x * 0.25 + 0.000001;

    s.adaptive_radius_y = radius_y * radius_scale + 0.000001;
    if (consider_high_kf && s.high_kf_enabled_y)
        s.adaptive_radius_y = radius_y * 0.25 + 0.000001;
    if (s.damp_y)
        s.adaptive_radius_y = radius_y * 0.25 + 0.000001;
}

void compute_gaussian_weights(PidfMode1State& s) noexcept {
    const double zx = s.absolute_error_x / s.adaptive_radius_x;
    s.gaussian_weight_x = std::exp(zx * zx * -0.5);
    const double zy = s.absolute_error_y / s.adaptive_radius_y;
    s.gaussian_weight_y = std::exp(zy * zy * -0.5);
}

// dynamic_lr(前馈速度估计器的学习率) 的权重下限。
//
// 原实现直接拿【当前】高斯权重去缩放学习率, 而权重 = exp(-0.5*(err/radius)^2),
// 误差一大就趋近于 0。于是出现一个鸡生蛋的死结:
//   · 目标离准星很远时, 恰恰是最需要前馈(提前量)来快速贴上去的时候,
//     而估计器此时几乎停止学习 —— 必须先贴近了它才肯学目标往哪走;
//   · 表现就是"第一枪/拉远的目标永远慢半拍", 且 lr 调小会让它更明显。
// 给一个下限, 让它在远距离也保留最低限度的学习能力。
//
// 注意: 前馈的【施加】权重用的是 integral_weight(峰值闩锁), 本来就没有这个
// 问题; 这里修的只是"学习"侧。
constexpr double kDynamicLrWeightFloor = 0.25;

double lr_weight(double gaussian_weight) noexcept {
    return std::max(gaussian_weight, kDynamicLrWeightFloor);
}

void apply_high_kf_correction(PidfMode1State& s,
                              double dt,
                              double radius_x,
                              double radius_y,
                              double smoothing_radius_x,
                              double smoothing_radius_y) noexcept {
    if (s.kf_high_x == 0.0 && s.kf_high_y == 0.0) {
        s.correction_output_x = s.correction_output_y = 0.0;
        s.correction_accum_x = s.correction_accum_y = 0.0;
        return;
    }

    s.ff_output_x = s.ff_state_x * dt;
    s.ff_output_y = s.ff_state_y * dt;

    s.correction_ratio_x =
        std::tanh(std::fabs(s.ff_output_x) / smoothing_radius_x);
    s.correction_target_x = signum(s.ff_state_x) * radius_x;
    s.correction_delta_x =
        (((s.correction_target_x - s.ff_output_x) * s.correction_ratio_x
          + s.ff_output_x) * s.kf_high_x - s.correction_accum_x)
        * s.dynamic_lr_x;
    s.correction_accum_x += s.correction_delta_x;
    s.correction_output_x = s.correction_accum_x;

    s.correction_ratio_y =
        std::tanh(std::fabs(s.ff_output_y) / smoothing_radius_y);
    s.correction_target_y = signum(s.ff_state_y) * radius_y;
    s.correction_delta_y =
        (((s.correction_target_y - s.ff_output_y) * s.correction_ratio_y
          + s.ff_output_y) * s.kf_high_y - s.correction_accum_y)
        * s.dynamic_lr_y;
    s.correction_accum_y += s.correction_delta_y;
    s.correction_output_y = s.correction_accum_y;
}

// 前馈学习率的【延迟上限】。
//
// 为什么需要: 学习率是"前馈环"的增益, 而那条环路的相位裕度由链路延迟决定 ——
// 延迟越低, 能承受的学习率越高。aim_scenario_sim 实测(kp=2.0 kd=0.05 kf=1):
//     总延迟(测量+指令)   2帧     3帧     4帧     5帧
//     lr=0.05           12.05   14.40   16.22   19.05
//     lr=0.08           10.44   13.30   15.57   27.62  <- 5 帧开始变差
// 所以 <=4 帧时放开到 0.08, >=5 帧时收回 0.05。延迟由 latency_probe 每帧实测,
// 没测到(0)时按保守的 0.05, 与旧默认一致 —— 不会比现在更差。
//
// 用户把「预测速度」调得比这个上限更低时, 以用户的值为准(取 min)。
// 链路总延迟(帧) = 测量侧 + 指令侧。没测到延迟时返回一个大值(走最保守档)。
double link_latency_frames(const PidfDelayModelExact& delay,
                           double dt) noexcept {
    if (delay.measure_latency_sec <= 0.0)
        return 1.0e9;
    return delay.measure_latency_sec / dt + delay.command_latency_frames;
}

// 自适应半径的倍率(高斯门控的尺度): 它决定"误差多大时开始压制前馈"。
//
// 为什么它也该按延迟定档: 门控的尺度应当跟着【典型误差量级】走, 而典型误差随延迟
// 增长 —— 延迟越大, 同样的机动留下的暂态误差越大, 尺度太小就会把前馈整个压掉,
// 变成"追不上还不敢追"。
// 实测(aim_scenario_sim, 其余参数走各自定档):
//     倍率    2帧     3帧     4帧     6帧
//     1.5   10.44   12.77   15.98   31.20
//     3.0   10.34   12.85   15.68   27.13
//     5.0   10.66   13.43   16.21   25.92
//     6.0   10.63   13.77   16.47   25.66
//     8.0   10.62   14.09   16.84   25.85
// 低延迟 3.0 最好(6.0 会明显变差); 6 帧以上 6.0 最好(比 1.5 好 18%)。
double adaptive_radius_scale(const PidfDelayModelExact& delay,
                             double dt) noexcept {
    const double total_frames = link_latency_frames(delay, dt);
    if (total_frames > 1.0e8)
        return 3.0;                        // 没有实测延迟: 用全延迟段都稳的 3.0
    return total_frames <= 4.5 ? 3.0 : 6.0;
}

// 增益归一化: 把"每帧增益"换算到参考帧率。
//
// 为什么必须做: kp / kd / 前馈学习率都是【每帧】增益, 所以它们的【物理】增益
// ∝ 帧率 —— 同一个界面数值在 240Hz 下相当于 120Hz 的两倍, 在 60Hz 下只有一半。
// 而前馈位移是 ff_state*dt(物理量, 与帧率无关), 两者语义不一致, 于是同一套参数
// 在不同检测帧率下表现差异极大。实测(aim_scenario_sim, 同一套参数, 约 25ms 延迟):
//     帧率       60     90    120    144    240
//     不归一化 26.75  17.99  11.99   9.63  65.51   <- 两头都差(240Hz 会发散)
//     sqrt 归一 19.55  18.00  11.99  10.01  14.66   <- 采用
// 跨帧率离散度从 9.6~65.5 收窄到 10.0~19.6, 总体好 1.78 倍。
// 参考帧率取 120Hz(本项目调参所用的帧率), 于是在 120Hz 下本变换是恒等 ——
// 既有手感与已验证的调参结论都不变。
constexpr double kGainReferenceDtSec = 1.0 / 120.0;
constexpr double kGainRateScaleMin = 0.4;
constexpr double kGainRateScaleMax = 2.5;

double compute_rate_scale(PidfDelayModelExact& delay, double dt) noexcept {
    if (dt <= 0.0)
        return 1.0;
    // 一阶平滑: 首次直接用当帧值, 之后 0.1 步长(约 10 帧时间常数)
    delay.frame_dt_ema = (delay.frame_dt_ema <= 0.0)
        ? dt : (delay.frame_dt_ema * 0.9 + dt * 0.1);
    // 用【平方根】而不是线性: 线性缩放把"帧率更高 -> 信息更多 -> 可以更激进"这个
    // 好处也抹掉了。三方案实测(约 25ms 延迟, 逐帧率综合分):
    //     方案        60Hz    90Hz   120Hz   144Hz   240Hz    和
    //     线性       14.50   20.63   11.99   10.71   21.79   79.6
    //     sqrt       19.55   18.00   11.99   10.01   14.66   74.2   <- 采用
    //     不归一化   26.75   17.99   11.99    9.63   65.51  131.8
    // sqrt 在中频段与"不归一化"持平(保住了高帧率的好处), 又把 240Hz 从 65.5 压到
    // 14.7(比线性还好)。120Hz 处 sqrt(1)=1, 仍是恒等。
    double scale = std::sqrt(delay.frame_dt_ema / kGainReferenceDtSec);
    if (scale < kGainRateScaleMin) scale = kGainRateScaleMin;
    if (scale > kGainRateScaleMax) scale = kGainRateScaleMax;
    return scale;
}

double ff_learning_rate_cap(const PidfDelayModelExact& delay,
                            double dt) noexcept {
    const double total_frames = link_latency_frames(delay, dt);
    if (total_frames > 1.0e8)
        return 0.05;                       // 没有实测延迟, 保守
    return total_frames <= 4.0 ? 0.08 : 0.05;
}

// 比例增益 kp 的【延迟上限】。
//
// 为什么必须加: 比例环的相位裕度同样由链路延迟决定, 延迟一大, 高 kp 会直接
// 【发散】而不是"只是慢一点"。实测(aim_scenario_sim, kd=0.05 kf=1, 补偿开启):
//   总延迟(测量+指令)   6帧     7帧     8帧     12帧
//     kp=2.0          14323   8.7e6  1.4e9   8.8e7
//     kp=1.0             40    1861   1.8e5   5.5e7
//     kp=0.8             31     482   14366   1.2e8
//     kp=0.6             34     180    1574   2.2e8
//     kp=0.4             39      99     277   2.4e8
// 5 帧及以下 kp=2.0 仍是稳的(20.7), 所以不设上限; 6 帧收到 0.8、7-8 帧收到 0.4。
// 宁可【滞后】也不要【发散】—— 发散时准星会自己乱飞, 比滞后危险得多。
// 12 帧以上任何 kp 都稳不住, 那是链路本身的极限, 只能靠降延迟解决。
// 微分增益 kd 的【延迟上限】。
//
// 微分项吃的是延迟后的测量, 延迟一大它对相位是净害, 必须比 kp 收得更狠。
// 实测(aim_scenario_sim, kp 与 lr 均由定档给出, kf=1):
//     总延迟     2帧     3帧     5帧     6帧     7帧     8帧
//     kd=0.05  10.44  12.77  20.70  31.49  98.80  276.50
//     kd=0.03  10.93  13.46  20.13  31.20  44.83   85.16
//     kd=0.02  11.28  14.12  20.71  32.13  47.88   81.67
// 低延迟下 0.05 最好; 5-7 帧收到 0.03; 8 帧以上收到 0.02。
double derivative_gain_cap(const PidfDelayModelExact& delay,
                           double dt) noexcept {
    const double total_frames = link_latency_frames(delay, dt);
    if (total_frames > 1.0e8)
        return 0.03;                       // 没有实测延迟: 折中
    if (total_frames <= 4.5)
        return 0.05;
    return total_frames <= 7.5 ? 0.03 : 0.02;
}

double proportional_gain_cap(const PidfDelayModelExact& delay,
                             double dt) noexcept {
    const double total_frames = link_latency_frames(delay, dt);
    if (total_frames > 1.0e8)
        return 1.0;                        // 没有实测延迟: 取折中值, 宁可慢
    if (total_frames <= 5.5)
        return 1.0e9;                      // 不设上限, 用用户的值
    if (total_frames <= 6.5)
        return 0.8;
    if (total_frames <= 8.5)
        return 0.4;
    return 0.25;                           // 再高就只能"尽量别发散", 性能已不可用
}

// 前馈速度估计器: 学习率由调用方显式传入(当前 = dynamic_lr, 即基学习率 × 高斯权重
// 的下限 0.25)。这里把参数显式化, 是为了不去动 dynamic_lr 这个同时被复刻 ABI 布局
// 和高 Kf 修正项使用的字段。
void update_low_kf_feedforward(PidfMode1State& s,
                               double dt,
                               double ff_lr_x,
                               double ff_lr_y) noexcept {
    s.predicted_minus_move_x =
        s.ff_state_x * dt + s.ff_previous_error_x - s.previous_move_x;
    s.ff_error_x = s.corrected_error_x - s.predicted_minus_move_x;
    s.ff_derivative_x = s.ff_error_x / dt * ff_lr_x;
    s.ff_state_x += s.ff_derivative_x;
    s.ff_previous_error_x = s.corrected_error_x;
    s.ff_output_x = s.ff_state_x * dt * s.kf_low_x;

    s.predicted_minus_move_y =
        s.ff_state_y * dt + s.ff_previous_error_y - s.previous_move_y;
    s.ff_error_y = s.corrected_error_y - s.predicted_minus_move_y;
    s.ff_derivative_y = s.ff_error_y / dt * ff_lr_y;
    s.ff_state_y += s.ff_derivative_y;
    s.ff_previous_error_y = s.corrected_error_y;
    s.ff_output_y = s.ff_state_y * dt * s.kf_low_y;
}

// 前馈的「施加权重」。
//
// 原实现用 integral_weight(高斯权重的峰值闩锁)去缩放前馈位移, 意图是「离锚点越远,
// 前馈越不使劲」。但闩锁只能闩住它【见过】的东西: 锁定时目标就在 100px 外, 则高斯
// 权重从第一帧起就是 0.005 量级, 闩锁也就一直是 0.005 —— 前馈等于被关掉, 只剩比例
// 项在爬, 而比例项自己追不上匀速目标(见 apply_post_limits 上方的说明)。
//
// 实测(同一基准只切这一个常数; 300px/s 目标, 框宽 20, dt=8.33ms, 从 60/100/140px
//      三个初始偏差起步, 取前 150 帧的平均 |偏差|, 以及进到 5px 内所需帧数):
//
//        施加权重            锁定均偏差(kp=0.3/0.6/1.0)      进 5px(帧)
//        不门控 0.0 (旧行为)   85.4 / 40.9 / 17.5      未达(1200) / 217 / 107
//        本值   1.0            70.0 / 33.1 / 16.2         337 / 168 / 99
//        (0.25 与 0.5 介于两者之间, 随下限单调改善)
//
// 换向峰值偏差不变(24.0px @kp=0.6), 急停过冲不变, 静止目标仍然完全安静(|dx| 总和 0)。
// 也就是说: 前馈全额施加不会带来过冲, 换来的只是「锁上就能咬住」。
//
// kFfApplyWeightFloor = 1.0 表示完全不门控; 调回 0.0 即恢复旧的闩锁行为(单点可逆)。
constexpr double kFfApplyWeightFloor = 1.0;

std::int32_t quantize(double step,
                      double& residual,
                      double& rounded,
                      std::uint8_t& nonzero,
                      bool blocked) noexcept {
    if (blocked) {
        step = 0.0;
        residual = 0.0;
    }
    residual += step;
    rounded = std::rint(residual);
    const auto move = static_cast<std::int32_t>(rounded);
    nonzero = move != 0;
    if (move)
        residual -= static_cast<double>(move);
    return move;
}

void apply_post_limits(PidfMode1State& s,
                       double original_error_x,
                       double original_error_y) noexcept {
    if (s.config.deadzone_x > 0
        && static_cast<double>(s.config.deadzone_x) >= std::fabs(original_error_x)
        && std::abs(s.move_x) <= 1) {
        s.move_x = 0;
        s.move_nonzero_x = 0;
    }
    if (s.config.deadzone_y > 0
        && static_cast<double>(s.config.deadzone_y) >= std::fabs(original_error_y)
        && std::abs(s.move_y) <= 1) {
        s.move_y = 0;
        s.move_nonzero_y = 0;
    }
    if (s.config.movement_limit_x > 0) {
        const std::int32_t limited = std::clamp(
            s.move_x, -s.config.movement_limit_x, s.config.movement_limit_x);
        // 被限幅截掉的部分回灌 residual, 但【必须有界】。
        //
        // residual 的本职是"把亚像素的零头攒成 ±1 步"(quantize 里天然落在 ±0.5 内)。
        // 上一版把限幅截掉的部分也无界地灌进来, 于是"长期触顶"会把它攒成一个巨大数:
        // 实测限幅=1px/帧、追 240px/s 两秒后 residual 已达 2754 并继续涨, 目标一停
        // 准星仍以 ±1px/帧 永远滑行 —— 冲过头 308px 再反向冲, 600 帧不收敛。
        //
        // 夹在 ±1: 既不丢亚像素零头, 又保证触顶时输出最多只比上限多 1px,
        // 目标一停就能在一个帧内把账结清。
        // 实测(限幅=1px/帧, 先追 240px/s 两秒再急停):
        //     无界回灌(旧): 急停后冲过头 328 / 333 px(kp=0.6 / 1.0), 且永不归零
        //     夹在 ±1(现):  冲过头 2.0 px, 237 / 240 帧内归零
        constexpr double kResidualClamp = 1.0;
        s.residual_x = std::clamp(
            s.residual_x + static_cast<double>(s.move_x - limited),
            -kResidualClamp, kResidualClamp);
        s.move_x = limited;
        s.move_nonzero_x = s.move_x != 0;
    }
    if (s.config.movement_limit_y > 0) {
        const std::int32_t limited = std::clamp(
            s.move_y, -s.config.movement_limit_y, s.config.movement_limit_y);
        constexpr double kResidualClamp = 1.0;
        s.residual_y = std::clamp(
            s.residual_y + static_cast<double>(s.move_y - limited),
            -kResidualClamp, kResidualClamp);
        s.move_y = limited;
        s.move_nonzero_y = s.move_y != 0;
    }
}

// -----------------------------------------------------------------------------
// 链路延迟补偿(Smith 预测器)
// -----------------------------------------------------------------------------
//
// 误差的演化(在"设定值/输出"意义下)是:
//     err_{k+1} = err_k + v_k*dt - u_{k-cmd_lat}
// 其中 v 是目标速度, u 是下发的位移。控制环拿到的测量却是 meas_lat 帧之前的:
//     y_k = err_{k-meas_lat}
// 因此可以把它"推算"回当前:
//     err_k = y_k + Σ_{j=k-meas_lat}^{k-1} (v̂_j*dt - u_{j-cmd_lat})
// 右边两项都可用: u 是历史指令(精确已知), v̂ 用前馈状态估计。这就是补偿。
//
// 直接用这个推算值会很吵(它把 v̂ 的误差按 meas_lat 帧累加), 所以再套一层模型:
// 内部维护一个延迟自由的模型状态, 每帧先按上式推进, 再按增益 K 向测量修正:
//     x_k += K * (y_k - model_{k-meas_lat})
// 这既是标准的 Smith 预测器, 也是"用测量纠正模型、用模型抵抗延迟"的常规做法。
//
// K=0.5 是实测值: K>=0.7 在 3 帧以上延迟会发散; K=0.3 过于保守、收益很小。
// v̂ 另外做了 ±1500px/s 限幅 —— 补偿按 meas_lat 帧累加速度误差, 不限幅的话
// 一次估计失手就会被放大 meas_lat 倍。
constexpr double kDelayCompensationGain = 0.5;
constexpr double kDelayCompensationVelocityCap = 1500.0;

double delay_compensate(double* model_history,
                        double* command_history,
                        double& model_state,
                        bool& initialized,
                        const int step,
                        double measured,
                        double velocity,
                        const double dt,
                        const double measure_latency_sec,
                        const double command_latency_frames) noexcept {
    constexpr int N = PidfDelayModelExact::kHistory;
    if (!initialized) {
        model_state = measured;
        for (int i = 0; i < N; ++i) {
            model_history[i] = measured;
            command_history[i] = 0.0;
        }
        initialized = true;
        return measured;
    }

    // ⚠️ 这里【向下取整再减 1 帧】, 不是四舍五入 —— 实测(实际延迟 3 帧):
    //     补偿假定 2 帧 -> 21.9, 3 帧 -> 22.9, 4 帧 -> 24303, 6 帧 -> 8e9(发散)
    // 高估延迟会让模型去和"更早的模型历史"比较, 相位反向, 直接正反馈; 低估只是
    // 补偿不足(仍是安全的)。所以宁可少补一帧。
    int measure_lat = static_cast<int>(measure_latency_sec / dt) - 1;
    int command_lat = static_cast<int>(command_latency_frames + 0.5);
    if (measure_lat < 0) measure_lat = 0;
    if (measure_lat > N - 1) measure_lat = N - 1;
    if (command_lat < 0) command_lat = 0;

    if (step > 0) {
        const int command_index = ((step - 1 - command_lat) % N + N) % N;
        double bounded_velocity = velocity;
        if (bounded_velocity > kDelayCompensationVelocityCap)
            bounded_velocity = kDelayCompensationVelocityCap;
        if (bounded_velocity < -kDelayCompensationVelocityCap)
            bounded_velocity = -kDelayCompensationVelocityCap;
        model_state += bounded_velocity * dt - command_history[command_index];
    }

    // ⚠️ measure_lat == 0 时参照量必须是"本帧推进后的模型值"本身。
    // 若仍去读历史槽, 读到的会是 N 帧前的残留 —— 纠正项变成噪声, 直接发散
    // (实测: 假定延迟被安全余量压到 0 时综合分从 15.4 变 742099)。
    const double reference = (measure_lat == 0)
        ? model_state
        : model_history[((step - measure_lat) % N + N) % N];
    model_state += kDelayCompensationGain * (measured - reference);
    model_history[((step % N) + N) % N] = model_state;
    return model_state;
}

void apply_post_frame_damping(PidfMode1State& s) noexcept {
    if (s.damp_x) {
        s.integral_x *= s.gaussian_weight_x;
        s.ff_state_x *= s.gaussian_weight_x;
    }
    if (s.damp_y) {
        s.integral_y *= s.gaussian_weight_y;
        s.ff_state_y *= s.gaussian_weight_y;
    }
}

} // namespace

void reset_pidf_delay_model(PidfDelayModelExact& model) noexcept {
    model.step = 0;
    model.frame_dt_ema = 0.0;
    model.initialized = false;
    model.model_x.fill(0.0);
    model.model_y.fill(0.0);
    model.command_x.fill(0.0);
    model.command_y.fill(0.0);
}

void reset_pidf_mode1(PidfMode1State& s, double now_seconds) noexcept {
    s.initialized = 0;
    s.previous_error_x = s.previous_error_y = 0.0;
    s.previous_move_x = s.previous_move_y = 0.0;
    s.residual_x = s.residual_y = 0.0;
    s.previous_timestamp = now_seconds;
    s.integral_weight_x = s.integral_weight_y = 0.0;
    s.integral_x = s.integral_y = 0.0;
    s.d_filtered_x = s.d_filtered_y = 0.0;
    reset_feedforward(s);
}

PidfMode1State construct_pidf_mode1(const PidfMode1Config& config,
                                    double now_seconds) noexcept {
    PidfMode1State s{};
    s.config = config;
    s.kd_x = config.kd_x;
    s.kd_y = config.kd_y;
    s.kp_x = config.kp_x;
    s.kp_y = config.kp_y;
    s.ki_x = config.ki_x;
    s.ki_y = config.ki_y;
    s.base_lr_x = config.lr_x;
    s.base_lr_y = config.lr_y;
    s.dynamic_lr_x = config.lr_x;
    s.dynamic_lr_y = config.lr_y;
    s.kf_low_x = std::min(config.kf_x, 1.0);
    s.kf_low_y = std::min(config.kf_y, 1.0);
    s.kf_high_x = std::max(config.kf_x - 1.0, 0.0);
    s.kf_high_y = std::max(config.kf_y - 1.0, 0.0);
    s.integral_enabled_x = s.ki_x != 0.0;
    s.integral_enabled_y = s.ki_y != 0.0;
    reset_pidf_mode1(s, now_seconds);
    return s;
}

PidfNativeOutput update_pidf_mode1(PidfMode1State& s,
                                   PidfDelayModelExact& delay,
                                   const PidfInputExact& input,
                                   double now_seconds) noexcept {
    PidfNativeOutput out{};
    const double dt = now_seconds - s.previous_timestamp;
    // 每帧增益归一化到参考帧率(见 compute_rate_scale)
    const double rate_scale = compute_rate_scale(delay, dt);
    if (!input.valid) {
        reset_pidf_mode1(s, now_seconds);
        reset_pidf_delay_model(delay);
        return out;
    }

    // 每轴各用自己的尺度, 不再塌成 min(radius_x, radius_y)。
    // 输入给的是框宽/框高; 退化框(宽或高为 0)时兜到 1px, 避免自适应半径变成 ~0
    // 从而把高斯权重直接压成 0(原实现在这种情况下会静默失去前馈)。
    const double radius_x = std::max(1.0, input.radius_x);
    const double radius_y = std::max(1.0, input.radius_y);
    s.corrected_error_x = input.target_x - input.current_x;
    s.corrected_error_y = input.target_y - input.current_y;
    out.original_error_x = s.corrected_error_x;
    out.original_error_y = s.corrected_error_y;

    // 链路延迟补偿: 把 meas_lat 帧之前的测量推算回"当前", 供比例/微分/前馈共用。
    // 延迟为 0 时这里是恒等变换(与原行为逐位一致)。
    if (delay.measure_latency_sec > 0.0) {
        s.corrected_error_x = delay_compensate(
            delay.model_x.data(), delay.command_x.data(), delay.model_x_state,
            delay.initialized, delay.step, s.corrected_error_x, s.ff_state_x, dt,
            delay.measure_latency_sec, delay.command_latency_frames);
        s.corrected_error_y = delay_compensate(
            delay.model_y.data(), delay.command_y.data(), delay.model_y_state,
            delay.initialized, delay.step, s.corrected_error_y, s.ff_state_y, dt,
            delay.measure_latency_sec, delay.command_latency_frames);
    }

    if (!s.initialized) {
        reset_pidf_mode1(s, now_seconds);
        s.previous_error_x = s.corrected_error_x;
        s.previous_error_y = s.corrected_error_y;
        s.ff_previous_error_x = s.corrected_error_x;
        s.ff_previous_error_y = s.corrected_error_y;
        s.initialized = 1;
        out.initialized_this_frame = 1;
        return out;
    }

    compute_opposition_flags(
        s, s.corrected_error_x, s.corrected_error_y, dt);
    s.absolute_error_x = std::fabs(s.corrected_error_x);
    s.absolute_error_y = std::fabs(s.corrected_error_y);
    s.high_kf_enabled_x = s.kf_high_x > 0.0;
    s.high_kf_enabled_y = s.kf_high_y > 0.0;
    // 门控尺度也按延迟定档(见 adaptive_radius_scale)
    const double radius_scale = adaptive_radius_scale(delay, dt);
    choose_adaptive_radius(s, radius_x, radius_y, true, radius_scale);
    compute_gaussian_weights(s);
    s.dynamic_lr_x = lr_weight(s.gaussian_weight_x) * s.base_lr_x;
    s.dynamic_lr_y = lr_weight(s.gaussian_weight_y) * s.base_lr_y;

    apply_high_kf_correction(
        s, dt, radius_x, radius_y,
        radius_x * 0.05 + 0.000001, radius_y * 0.05 + 0.000001);
    if (s.kf_high_x == 0.0 && s.kf_high_y == 0.0) {
        s.corrected_error_x += s.correction_output_x;
        s.corrected_error_y += s.correction_output_y;
    } else {
        // Native code recomputes the opposition-dependent Gaussian envelope
        // before weighting the high-Kf correction output.
        s.absolute_error_x = std::fabs(s.corrected_error_x);
        s.absolute_error_y = std::fabs(s.corrected_error_y);
        compute_opposition_flags(
            s, s.corrected_error_x, s.corrected_error_y, dt);
        choose_adaptive_radius(s, radius_x, radius_y, true, radius_scale);
        compute_gaussian_weights(s);
        s.corrected_error_x +=
            s.gaussian_weight_x * s.correction_output_x;
        s.corrected_error_y +=
            s.gaussian_weight_y * s.correction_output_y;
    }
    out.corrected_error_x = s.corrected_error_x;
    out.corrected_error_y = s.corrected_error_y;

    s.absolute_error_x = std::fabs(s.corrected_error_x);
    s.absolute_error_y = std::fabs(s.corrected_error_y);
    compute_opposition_flags(
        s, s.corrected_error_x, s.corrected_error_y, dt);
    choose_adaptive_radius(s, radius_x, radius_y, false, radius_scale);
    compute_gaussian_weights(s);
    s.integral_weight_x =
        std::max(s.integral_weight_x, s.gaussian_weight_x);
    s.integral_weight_y =
        std::max(s.integral_weight_y, s.gaussian_weight_y);
    s.dynamic_lr_x = s.base_lr_x * lr_weight(s.gaussian_weight_x);
    s.dynamic_lr_y = s.base_lr_y * lr_weight(s.gaussian_weight_y);

    if (s.integral_enabled_x)
        s.integral_x +=
            dt * s.corrected_error_x * s.integral_weight_x;
    if (s.integral_enabled_y)
        s.integral_y +=
            dt * s.corrected_error_y * s.integral_weight_y;

    // D 项: 先算原始微分, 再过一阶低通, 用滤波后的值参与求和。
    //
    // 原实现在这一项上有两个毛病, 都会直接体现为"准星在身上抖 / 焊不住":
    //   1) 除以 dt 把手感上最敏感的帧间隔抖动直接放大成输出抖动(dt 就是检测
    //      间隔, 本身就在抖);
    //   2) 它是对【误差】求导 —— 目标框一跳(头/身类别翻转、检测抖动、重新
    //      锁定)就打出一个微分尖峰, 而 Kp 越高这个尖峰越猛。
    // 低通写成时间常数形式(与 dt 无关), 于是 240fps 和 60fps 的手感一致。
    // tau 约 2 帧: 既压掉逐帧毛刺, 又保留追踪运动趋势的阻尼作用。
    // 高延迟时把 kd 也收到安全档(见 derivative_gain_cap)
    const double kd_cap = derivative_gain_cap(delay, dt);
    // 实测(aim_scenario_sim, 其余参数走各自定档, kp=2.0 kd=0.05 kf=1 lr=0.08):
    //     tau     2帧     3帧     4帧     5帧     6帧
    //     0.020  10.34   12.85   15.68   21.50   25.66   <- 原值
    //     0.0125  9.78   11.99   15.19   19.62   24.94   <- 采用
    //     0.010   9.41   11.76   15.32   18.92   25.21
    //     0.0075  9.36   11.61   17.56   18.71   25.06
    //     0.006   9.25   11.49   79.81   18.41   24.97   <- 4 帧档发散
    //     0.005   9.18   11.32  1124.2   18.15   24.88   <- 4 帧档发散
    //     0.002   9.06   11.27   6.7e7   18.02   26.23
    // ⚠️ 存在一个【4 帧档的离散共振悬崖】: tau 短到 0.008 以下时恰好在 4 帧延迟
    // 发散(而 3/5/6 帧都正常)。所以不能一味取短 —— 0.0125 离悬崖有 56% 余量,
    // 且在各延迟下都比原值 0.020 好(2 帧 5%、3 帧 7%、4 帧 3%、5 帧 9%、6 帧 3%)。
    // 原值 0.020(约 2 帧)当初是为压高 kd 下的微分尖峰加的, 但现在 kd 已被延迟定档
    // 限在 0.05 以内, 那个顾虑不再成立。静止场景在全部取值下抖动均为 0.00。
    constexpr double kDerivativeTauSec = 0.0125;

    const double d_alpha = 1.0 - std::exp(-dt / kDerivativeTauSec);
    const double d_raw_x =
        (s.corrected_error_x - s.previous_error_x)
        * std::min(s.kd_x, kd_cap) * rate_scale / dt;
    const double d_raw_y =
        (s.corrected_error_y - s.previous_error_y)
        * std::min(s.kd_y, kd_cap) * rate_scale / dt;
    s.d_filtered_x += (d_raw_x - s.d_filtered_x) * d_alpha;
    s.d_filtered_y += (d_raw_y - s.d_filtered_y) * d_alpha;
    s.scratch_x = s.d_filtered_x;
    s.scratch_y = s.d_filtered_y;

    // 高延迟时把 kp 收到安全档(见 proportional_gain_cap): 宁可滞后, 不可发散。
    const double kp_cap = proportional_gain_cap(delay, dt);
    const double proportional_integral_x =
        s.integral_x * s.ki_x
        + s.corrected_error_x * std::min(s.kp_x, kp_cap) * rate_scale;
    s.raw_pid_x = s.scratch_x + proportional_integral_x;
    const double proportional_integral_y =
        s.integral_y * s.ki_y
        + s.corrected_error_y * std::min(s.kp_y, kp_cap) * rate_scale;
    s.raw_pid_y = s.scratch_y + proportional_integral_y;

    // 前馈速度估计: 学习率沿用 dynamic_lr(基学习率 × 高斯权重下限)。
    //
    // ⚠️ 不要把它改成裸的基学习率。实测过: 学习侧不门控能让"从 100px 外锁定"的
    // 收敛从 218 帧降到 140 帧, 但代价是「预测速度(LR)=1.0 + 高 Kf」这一档直接跑飞
    // —— 回归测试里 noisy cadence 的 high-Kf/high-LR 用例从 tail-mean 61px 变成
    // -450px 且位移全部钉在限幅上。原因: LR=1.0 时估计器每帧增益已达 ~125px/s,
    // 高斯门控原本正是在大误差下给它兜稳定性, 去掉就把这层兜底拆了。
    // 所以这里保持门控, 只把参数显式传进来(便于以后单独调, 且不必动 dynamic_lr
    // 这个被复刻 ABI 与高 Kf 修正项共用的字段)。
    // 上限作用在【基学习率】上, 再乘高斯门控 —— 这样当上限 <= 用户设定值时,
    // 前馈学习率与旧行为逐位一致(不会在高延迟场景悄悄放大)。
    const double ff_lr_cap = ff_learning_rate_cap(delay, dt);
    update_low_kf_feedforward(
        s, dt,
        lr_weight(s.gaussian_weight_x) * std::min(s.base_lr_x, ff_lr_cap) * rate_scale,
        lr_weight(s.gaussian_weight_y) * std::min(s.base_lr_y, ff_lr_cap) * rate_scale);
    // frame_divisor 目前没有任何调用方设置(见 pid_input_pipeline.cpp), 所以
    // frame_scale 恒为 1; 保留是为了不改动原生算式结构。
    const double frame_scale = 1.0 / std::max(
        static_cast<double>(input.frame_divisor) * 0.25, 1.0);

    // ★ 下面的 0.1 是这套控制器的原生增益标定, 【不要】当多余的系数删掉。
    //
    // 比例+微分项被乘 0.1, 而前馈项按施加权重全额相加, 两者的相对
    // 权重是原生设计的一部分:
    //     每帧位移 = 0.1*(D + Kp*err) + ff_apply_weight*ff
    // 去掉 0.1 会让比例项比前馈强 10 倍, 前馈随之失效 —— 等于把一套
    // "预测型"控制器改成"反应型", 方向是反的。所以只把刻度写明, 不动它。
    //
    // 真实含义: UI 的「瞄准速度」= 实际每帧收敛比例 × 10。
    //     Kp = 1.0  → 每帧走掉剩余误差的 10%
    //     Kp = 10.0 → 约 100%, 即一帧到位(此时必须靠 Kd 压过冲)
    // UI 的提示文案里写了这个换算, 用户不用再猜。
    //
    // 施加权重: 见 kFfApplyWeightFloor 的说明 —— 默认不再用距离闩锁掐前馈。
    const double ff_apply_x =
        std::max(s.integral_weight_x, kFfApplyWeightFloor);
    const double ff_apply_y =
        std::max(s.integral_weight_y, kFfApplyWeightFloor);
    s.scratch_x = ff_apply_x * s.ff_output_x;
    s.raw_pid_x = (s.raw_pid_x * 0.1 + s.scratch_x) * frame_scale;
    s.scratch_y = ff_apply_y * s.ff_output_y;
    s.raw_pid_y = (s.raw_pid_y * 0.1 + s.scratch_y) * frame_scale;

    s.move_x = quantize(
        s.raw_pid_x, s.residual_x, s.rounded_x,
        s.move_nonzero_x, s.axis_blocked_x != 0);
    s.move_y = quantize(
        s.raw_pid_y, s.residual_y, s.rounded_y,
        s.move_nonzero_y, s.axis_blocked_y != 0);
    apply_post_limits(s, out.original_error_x, out.original_error_y);

    out.dx = s.move_x;
    out.dy = s.move_y;
    out.has_move = out.dx != 0 || out.dy != 0;

    if (delay.measure_latency_sec > 0.0) {
        constexpr int N = PidfDelayModelExact::kHistory;
        delay.command_x[((delay.step % N) + N) % N] = static_cast<double>(out.dx);
        delay.command_y[((delay.step % N) + N) % N] = static_cast<double>(out.dy);
        ++delay.step;
    }
    apply_post_frame_damping(s);
    s.previous_timestamp = now_seconds;
    s.previous_error_x = s.corrected_error_x;
    s.previous_error_y = s.corrected_error_y;
    s.previous_move_x = static_cast<double>(out.dx);
    s.previous_move_y = static_cast<double>(out.dy);
    return out;
}

} // namespace cvm::recovered
