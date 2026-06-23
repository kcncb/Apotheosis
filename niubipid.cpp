#include "mpid.hpp"

#include <algorithm>
#include <random>

namespace aim {

    //==============================================================================
    // IncrementalPid 实现
    //==============================================================================

    void IncrementalPid::configure(double kp, double ki, double kd) noexcept {
        kp_ = kp;
        ki_ = ki;
        kd_ = kd;
    }

    void IncrementalPid::reset(double output) noexcept {
        output_ = output;
        previous_output_ = output;
        previous_error_ = 0.0;
        previous_previous_error_ = 0.0;
    }

    void IncrementalPid::set_output_limits(double min_val, double max_val) noexcept {
        output_min_ = min_val;
        output_max_ = max_val;
        // 立即将当前输出钳位到新的限幅范围内
        output_ = clamp(output_, output_min_, output_max_);
    }

    double IncrementalPid::update(double error, double scale) noexcept {
        // 保存上一次输出值，用于后续计算
        previous_output_ = output_;

        // 输入死区：小误差时置零，避免积分累积（硬编码阈值0.3）
        if (std::abs(error) < 0.3) {
            error = 0.0;
        }

        // 增量式 PID 公式：
        // Δout = Kp*(e - e1) + Ki*e + Kd*(e - 2*e1 + e2)
        // out  = out_prev + Δout
        // 
        // 各项含义：
        // - Kp*(e - e1)：比例项，响应误差变化
        // - Ki*e：积分项，累积误差消除稳态偏差
        // - Kd*(e - 2*e1 + e2)：微分项，预测误差趋势抑制超调
        double delta =
            kp_ * (error - previous_error_) +
            ki_ * error +
            kd_ * (error - 2.0 * previous_error_ + previous_previous_error_);

        // 对增量应用缩放因子（用于S曲线平滑启动）
        // 这确保积分项在启动阶段也被衰减，避免过冲
        delta *= scale;

        output_ += delta;

        // 输出死区：小输出时衰减，抑制振荡（硬编码阈值0.5）
        if (std::abs(output_) < 0.5) {
            output_ *= 0.9;
        }

        // 抗积分饱和：将输出钳位到限幅范围
        // 防止输出超出物理限制时积分项继续累积
        output_ = clamp(output_, output_min_, output_max_);

        // 更新误差历史
        previous_previous_error_ = previous_error_;
        previous_error_ = error;

        return output_;
    }

    //==============================================================================
    // PerlinNoise1D 实现
    //==============================================================================

    PerlinNoise1D::PerlinNoise1D(std::uint32_t seed) {
        // 使用 Mersenne Twister 随机数生成器初始化噪声值表
        std::mt19937 rng{ seed };
        std::uniform_real_distribution<double> dist{ 0.0, 1.0 };
        std::generate(values_.begin(), values_.end(), [&] { return dist(rng); });
    }

    double PerlinNoise1D::noise(double x) const noexcept {
        // 计算整数部分和小数部分
        // 使用位运算 & 255 确保索引在查找表范围内
        const auto xi = static_cast<std::size_t>(std::floor(x)) & 255;
        const double xf = x - std::floor(x);

        // 应用平滑步进函数使噪声在整数边界平滑过渡
        const double u = fade(xf);

        // 从查找表获取相邻两个随机值
        const double a = values_[xi];
        const double b = values_[(xi + 1) & 255];

        // 线性插值并映射到 [-1, 1] 范围
        return lerp(a, b, u) * 2.0 - 1.0;
    }

    //==============================================================================
    // DerivativePredictor 实现
    //==============================================================================

    void DerivativePredictor::reset() noexcept {
        last_error_x_ = 0.0;
        last_error_y_ = 0.0;
        smooth_vel_x_ = 0.0;
        smooth_vel_y_ = 0.0;
        smooth_acc_x_ = 0.0;
        smooth_acc_y_ = 0.0;
        has_last_ = false;
    }

    std::pair<double, double> DerivativePredictor::predict(
        double error_x,
        double error_y,
        double prev_move_x,
        double prev_move_y,
        double dt) {

        // 首次调用时初始化历史数据，返回零预测
        if (!has_last_) {
            last_error_x_ = error_x;
            last_error_y_ = error_y;
            has_last_ = true;
            return { 0.0, 0.0 };
        }

        // 限制时间增量范围，防止异常值
        dt = clamp(dt, 0.001, 0.05);

        // 计算速度：误差变化 + 上一次移动量
        // 误差变化反映目标相对运动，移动量反映自身补偿
        double vel_x = clamp((error_x - last_error_x_ + prev_move_x) / dt, -3000.0, 3000.0);
        double vel_y = clamp((error_y - last_error_y_ + prev_move_y) / dt, -3000.0, 3000.0);

        // 过冲抑制：当误差和速度方向相反时（正在过冲），大幅降低速度
        // 防止预测器在目标附近产生剧烈振荡
        if (std::abs(error_x) > 5.0 && vel_x * error_x < 0.0) {
            vel_x *= 0.1;
        }
        if (std::abs(error_y) > 5.0 && vel_y * error_y < 0.0) {
            vel_y *= 0.1;
        }

        // 计算加速度：速度变化率
        double acc_x = clamp((vel_x - smooth_vel_x_) / dt, -5000.0, 5000.0);
        double acc_y = clamp((vel_y - smooth_vel_y_) / dt, -5000.0, 5000.0);

        // 指数平滑系数：基于时间增量自适应调整
        // 较大的时间增量使用较大的平滑系数，更快响应变化
        const double alpha_v = clamp(1.0 - std::pow(0.75, dt / 0.01), 0.05, 0.8);
        const double alpha_a = clamp(1.0 - std::pow(0.85, dt / 0.01), 0.05, 0.8);

        // 更新平滑后的速度和加速度
        smooth_vel_x_ += (vel_x - smooth_vel_x_) * alpha_v;
        smooth_vel_y_ += (vel_y - smooth_vel_y_) * alpha_v;
        smooth_acc_x_ += (acc_x - smooth_acc_x_) * alpha_a;
        smooth_acc_y_ += (acc_y - smooth_acc_y_) * alpha_a;

        // 保存当前误差用于下次计算
        last_error_x_ = error_x;
        last_error_y_ = error_y;

        // 使用运动学方程预测位置：x = v*t + 0.5*a*t^2
        // 这是匀加速运动的位移公式
        return {
            smooth_vel_x_ * dt + 0.5 * smooth_acc_x_ * dt * dt,
            smooth_vel_y_ * dt + 0.5 * smooth_acc_y_ * dt * dt
        };
    }

    //==============================================================================
    // AimController 实现
    //==============================================================================

    AimController::AimController()
        : last_time_(std::chrono::steady_clock::now())
        , lock_start_(std::chrono::steady_clock::now())
        , noise_x_(12345)  // X 轴噪声使用固定种子
        , noise_y_(54321)  // Y 轴噪声使用不同种子，避免同步
    {}

    void AimController::reset() noexcept {
        // 重置 PID 控制器
        pid_x_.reset();
        pid_y_.reset();

        // 重置状态变量
        last_raw_x_ = 0.0;
        last_raw_y_ = 0.0;
        last_output_x_ = 0.0;
        last_output_y_ = 0.0;

        // 重置时间戳
        last_time_ = std::chrono::steady_clock::now();
        lock_start_ = std::chrono::steady_clock::now();

        // 重置噪声和预测相关状态
        noise_time_x_ = 0.0;
        noise_time_y_ = 100.0;

        // 重置运动预测器
        predictor_.reset();
    }

    void AimController::configure_pid(double kp, double ki, double kd) noexcept {
        // 同时配置双轴 PID 参数
        pid_x_.configure(kp, ki, kd);
        pid_y_.configure(kp, ki, kd);
    }

    void AimController::configure_pid_x(double kp, double ki, double kd) noexcept {
        // 仅配置 X 轴 PID 参数
        pid_x_.configure(kp, ki, kd);
    }

    void AimController::configure_pid_y(double kp, double ki, double kd) noexcept {
        // 仅配置 Y 轴 PID 参数
        pid_y_.configure(kp, ki, kd);
    }

    void AimController::set_output_limits(double min_val, double max_val) noexcept {
        // 设置双轴输出限幅
        pid_x_.set_output_limits(min_val, max_val);
        pid_y_.set_output_limits(min_val, max_val);
    }

    AimOutput AimController::update(
        double raw_x,
        double raw_y,
        double pred_weight_x,
        double pred_weight_y,
        double init_scale,
        double ramp_time,
        double output_max,
        double noise_amp) {

        // 计算时间增量
        const auto now = std::chrono::steady_clock::now();
        const double dt = clamp(
            std::chrono::duration<double>(now - last_time_).count(),
            0.001,
            0.05
        );
        last_time_ = now;

        // 计算当前目标距离和目标跳变距离
        const double distance = std::hypot(raw_x, raw_y);
        const double target_jump = std::hypot(raw_x - last_raw_x_, raw_y - last_raw_y_);

        // 目标跳变检测：当目标位置大幅变化时重置状态
        // 这是新目标锁定的触发条件
        const auto lock_start_time = std::chrono::duration<double>(lock_start_.time_since_epoch()).count();

        if (lock_start_time <= 0.0 || target_jump > 40.0) {
            // 记录锁定开始时间
            lock_start_ = now;

            // 重置预测器和 PID 控制器
            predictor_.reset();
            pid_x_.reset();
            pid_y_.reset();
        }

        // 从运动预测器获取预测值
        auto [pred_x, pred_y] = predictor_.predict(
            raw_x, raw_y, last_output_x_, last_output_y_, dt
        );

        // 将预测值钳位到合理范围
        // 范围基于原始误差的 1.5 倍，最小 30，最大 60
        const double pred_limit_x = std::min(std::max(std::abs(raw_x) * 1.5, 30.0), 60.0);
        const double pred_limit_y = std::min(std::max(std::abs(raw_y) * 1.5, 30.0), 60.0);
        pred_x = clamp(pred_x, -pred_limit_x, pred_limit_x);
        pred_y = clamp(pred_y, -pred_limit_y, pred_limit_y);

        // 预测值过大时重置预测器，防止异常
        if (std::abs(pred_x) > 100.0 || std::abs(pred_y) > 100.0) {
            predictor_.reset();
            pred_x = pred_y = 0.0;
        }

        // 融合误差：原始误差 + 预测值 * 权重
        const double fused_x = raw_x + pred_x * pred_weight_x;
        const double fused_y = raw_y + pred_y * pred_weight_y;

        // 计算渐入缩放比例：使用 smoothstep 函数实现平滑过渡
        // 从 init_scale 渐变到 1.0，避免锁定瞬间的大幅移动
        const double elapsed = std::chrono::duration<double>(now - lock_start_).count();
        const double progress = clamp(elapsed / std::max(ramp_time, 0.001), 0.0, 1.0);
        const double ramp = progress * progress * (3.0 - 2.0 * progress);  // smoothstep
        const double scale = init_scale + (1.0 - init_scale) * ramp;

        // 应用增量式 PID 控制
        // 将缩放因子传入 PID update，使积分项也被衰减，避免过冲
        double out_x = pid_x_.update(fused_x, scale);
        double out_y = pid_y_.update(fused_y, scale);

        // 添加柏林噪声：模拟人类操作的自然抖动
        // 噪声时间参数随时间推进，产生连续变化的噪声
        noise_time_x_ += dt * 5.0;
        noise_time_y_ += dt * 5.0;
        out_x += noise_x_.noise(noise_time_x_) * noise_amp;
        out_y += noise_y_.noise(noise_time_y_) * noise_amp;

        // 最终输出钳位
        out_x = clamp(out_x, -output_max, output_max);
        out_y = clamp(out_y, -output_max, output_max);

        // 保存状态用于下次迭代
        last_raw_x_ = raw_x;
        last_raw_y_ = raw_y;
        last_output_x_ = out_x;
        last_output_y_ = out_y;

        // 返回输出结构
        return AimOutput{
            out_x,
            out_y,
            std::hypot(fused_x, fused_y),  // 融合误差向量长度
            pred_x,
            pred_y,
            fused_x,
            fused_y
        };
    }

} // namespace aim