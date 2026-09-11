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
void choose_adaptive_radius(PidfMode1State& s,
                            double radius_x,
                            double radius_y,
                            bool consider_high_kf) noexcept {
    s.adaptive_radius_x = radius_x * 1.5 + 0.000001;
    if (consider_high_kf && s.high_kf_enabled_x)
        s.adaptive_radius_x = radius_x * 0.5 + 0.000001;
    if (s.damp_x)
        s.adaptive_radius_x = radius_x * 0.25 + 0.000001;

    s.adaptive_radius_y = radius_y * 1.5 + 0.000001;
    if (consider_high_kf && s.high_kf_enabled_y)
        s.adaptive_radius_y = radius_y * 0.5 + 0.000001;
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
                                   const PidfInputExact& input,
                                   double now_seconds) noexcept {
    PidfNativeOutput out{};
    const double dt = now_seconds - s.previous_timestamp;
    if (!input.valid) {
        reset_pidf_mode1(s, now_seconds);
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
    choose_adaptive_radius(s, radius_x, radius_y, true);
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
        choose_adaptive_radius(s, radius_x, radius_y, true);
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
    choose_adaptive_radius(s, radius_x, radius_y, false);
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
    constexpr double kDerivativeTauSec = 0.020;
    const double d_alpha = 1.0 - std::exp(-dt / kDerivativeTauSec);
    const double d_raw_x =
        (s.corrected_error_x - s.previous_error_x) * s.kd_x / dt;
    const double d_raw_y =
        (s.corrected_error_y - s.previous_error_y) * s.kd_y / dt;
    s.d_filtered_x += (d_raw_x - s.d_filtered_x) * d_alpha;
    s.d_filtered_y += (d_raw_y - s.d_filtered_y) * d_alpha;
    s.scratch_x = s.d_filtered_x;
    s.scratch_y = s.d_filtered_y;

    const double proportional_integral_x =
        s.integral_x * s.ki_x + s.corrected_error_x * s.kp_x;
    s.raw_pid_x = s.scratch_x + proportional_integral_x;
    const double proportional_integral_y =
        s.integral_y * s.ki_y + s.corrected_error_y * s.kp_y;
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
    update_low_kf_feedforward(s, dt, s.dynamic_lr_x, s.dynamic_lr_y);
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

    apply_post_frame_damping(s);
    s.previous_timestamp = now_seconds;
    s.previous_error_x = s.corrected_error_x;
    s.previous_error_y = s.corrected_error_y;
    s.previous_move_x = static_cast<double>(out.dx);
    s.previous_move_y = static_cast<double>(out.dy);
    return out;
}

} // namespace cvm::recovered
