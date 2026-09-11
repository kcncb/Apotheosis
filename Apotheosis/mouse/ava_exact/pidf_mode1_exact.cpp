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

// 前馈位移 = 估计速度 × 前视时间。
//
// 原生算式是 `速度 × dt × kf_low`: 即只把误差预测【一帧】那么远, 恰好抵消
// 目标在一帧内的移动。但整条链路还有一段自己的滞后(采集卡内部 + 解码 + 推理
// + 发布 + 控制环唤醒, 即探针的 total = T3-T0), 那一段没人补 —— 原设计里补它的
// 是被单独一级 QX 弹道预测, 而当前集成把 QX 关掉了。
//
// 所以这里把前视时间从 dt 扩成 (dt + lead_time_sec): 得到的位移正好是
// "目标在 dt 内会走多远" + "目标在这条链路的滞后里会走多远", 后者就是补偿。
// lead_time_sec 由 latency_probe 每帧实测给出, 不是常数。
//
// lead_time_sec = 0 时 (dt + 0) == dt, 与原生实现逐位一致 —— 便于 A/B 对照。
void update_low_kf_feedforward(PidfMode1State& s,
                               double dt,
                               double lead_time_sec) noexcept {
    const double lookahead = dt + lead_time_sec;
    s.predicted_minus_move_x =
        s.ff_state_x * dt + s.ff_previous_error_x - s.previous_move_x;
    s.ff_error_x = s.corrected_error_x - s.predicted_minus_move_x;
    s.ff_derivative_x = s.ff_error_x / dt * s.dynamic_lr_x;
    s.ff_state_x += s.ff_derivative_x;
    s.ff_previous_error_x = s.corrected_error_x;
    s.ff_output_x = s.ff_state_x * lookahead * s.kf_low_x;

    s.predicted_minus_move_y =
        s.ff_state_y * dt + s.ff_previous_error_y - s.previous_move_y;
    s.ff_error_y = s.corrected_error_y - s.predicted_minus_move_y;
    s.ff_derivative_y = s.ff_error_y / dt * s.dynamic_lr_y;
    s.ff_state_y += s.ff_derivative_y;
    s.ff_previous_error_y = s.corrected_error_y;
    s.ff_output_y = s.ff_state_y * lookahead * s.kf_low_y;
}

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
        // 被限幅截掉的部分回灌 residual。
        // 原样实现直接丢弃, 于是每次触顶都永久少走一段位移 —— 准星会稳定停在
        // 目标前方一点点、再也补不上来(表现为"永远差最后几像素")。
        s.residual_x += static_cast<double>(s.move_x - limited);
        s.move_x = limited;
        s.move_nonzero_x = s.move_x != 0;
    }
    if (s.config.movement_limit_y > 0) {
        const std::int32_t limited = std::clamp(
            s.move_y, -s.config.movement_limit_y, s.config.movement_limit_y);
        s.residual_y += static_cast<double>(s.move_y - limited);
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
                                   const PidfContextInput& context,
                                   double now_seconds) noexcept {
    PidfNativeOutput out{};
    const double dt = now_seconds - s.previous_timestamp;

    // 提前时间: 取实测值, 但夹一个上限。
    //
    // 上限是安全阀而不是标定值: 探针在启动初期、采集抖动、或统计被污染时可能
    // 给出离谱的数, 而前视时间是直接乘在位移上的 —— 不夹住的话一次异常读数就
    // 会把准星甩到目标前面很远。0.2s 远大于任何合理链路延迟。
    constexpr double kMaxLeadTimeSec = 0.20;
    const double lead_time_sec =
        std::clamp(context.lead_time_sec, 0.0, kMaxLeadTimeSec);

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

    update_low_kf_feedforward(s, dt, lead_time_sec);
    // frame_divisor 目前没有任何调用方设置(见 pid_input_pipeline.cpp), 所以
    // frame_scale 恒为 1; 保留是为了不改动原生算式结构。
    const double frame_scale = 1.0 / std::max(
        static_cast<double>(input.frame_divisor) * 0.25, 1.0);

    // ★ 下面的 0.1 是这套控制器的原生增益标定, 【不要】当多余的系数删掉。
    //
    // 比例+微分项被乘 0.1, 而前馈项按 integral_weight 全额相加, 两者的相对
    // 权重是原生设计的一部分:
    //     每帧位移 = 0.1*(D + Kp*err) + integral_weight*ff
    // 去掉 0.1 会让比例项比前馈强 10 倍, 前馈(提前量)随之失效 —— 等于把一套
    // "预测型"控制器改成"反应型", 方向是反的。所以只把刻度写明, 不动它。
    //
    // 真实含义: UI 的「瞄准速度」= 实际每帧收敛比例 × 10。
    //     Kp = 1.0  → 每帧走掉剩余误差的 10%
    //     Kp = 10.0 → 约 100%, 即一帧到位(此时必须靠 Kd 压过冲)
    // UI 的提示文案里写了这个换算, 用户不用再猜。
    s.scratch_x = s.integral_weight_x * s.ff_output_x;
    s.raw_pid_x = (s.raw_pid_x * 0.1 + s.scratch_x) * frame_scale;
    s.scratch_y = s.integral_weight_y * s.ff_output_y;
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
