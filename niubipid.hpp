#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace aim {

    /// 将值钳位到 [low, high] 范围内
    /// @tparam T 数值类型
    /// @param value 待钳位的值
    /// @param low 下界
    /// @param high 上界
    /// @return 钳位后的值
    template <typename T>
    constexpr T clamp(T value, T low, T high) noexcept {
        return value < low ? low : (value > high ? high : value);
    }

    /// 增量式 PID 控制器
    /// 输出公式：out += Kp*(e - e1) + Ki*e + Kd*(e - 2*e1 + e2)
    /// 与位置式 PID 不同，增量式 PID 输出的是控制量的增量，而非绝对值
    /// 优点：计算量小、不易积分饱和、切换时冲击小
    class IncrementalPid {
    public:
        /// 配置 PID 参数
        /// @param kp 比例增益：响应速度，过大会导致振荡
        /// @param ki 积分增益：消除稳态误差，过大会导致积分饱和
        /// @param kd 微分增益：抑制超调，改善动态性能
        void configure(double kp, double ki, double kd) noexcept;

        /// 重置控制器状态，可指定初始输出值
        /// @param output 初始输出值，默认为 0
        void reset(double output = 0.0) noexcept;

        /// 使用当前误差更新控制器，返回新的输出值
        /// @param error 当前误差值
        /// @param scale 增量缩放因子（用于S曲线平滑启动），默认为1.0
        /// @return 更新后的输出值
        [[nodiscard]] double update(double error, double scale = 1.0) noexcept;

        /// 设置输出限幅范围，用于抗积分饱和
        /// 当输出超出限幅时，积分项不再累积，防止系统失控
        /// @param min_val 输出下限
        /// @param max_val 输出上限
        void set_output_limits(double min_val, double max_val) noexcept;

        /// 获取当前输出值
        [[nodiscard]] double output() const noexcept { return output_; }
        /// 获取上一次输出值
        [[nodiscard]] double previous_output() const noexcept { return previous_output_; }
        /// 获取上一次误差值 e1
        [[nodiscard]] double previous_error() const noexcept { return previous_error_; }
        /// 获取比例增益
        [[nodiscard]] double kp() const noexcept { return kp_; }
        /// 获取积分增益
        [[nodiscard]] double ki() const noexcept { return ki_; }
        /// 获取微分增益
        [[nodiscard]] double kd() const noexcept { return kd_; }

    private:
        double output_ = 0.0;                  ///< 当前输出值
        double previous_output_ = 0.0;         ///< 上一次输出值
        double previous_error_ = 0.0;          ///< 上一次误差 e1
        double previous_previous_error_ = 0.0; ///< 上上次误差 e2
        double kp_ = 0.0;                      ///< 比例增益
        double ki_ = 0.0;                      ///< 积分增益
        double kd_ = 0.0;                      ///< 微分增益
        double output_min_ = -std::numeric_limits<double>::max(); ///< 输出下限
        double output_max_ = std::numeric_limits<double>::max();  ///< 输出上限
    };

    /// 一维柏林噪声生成器
    /// 产生平滑的随机值，输出范围约 [-1, 1]
    /// 用于模拟自然、有机的运动轨迹，避免机械感
    class PerlinNoise1D {
    public:
        /// 构造函数，可选种子
        /// @param seed 随机种子，用于可复现性
        explicit PerlinNoise1D(std::uint32_t seed = 0);

        /// 在位置 x 处生成噪声值
        /// @param x 噪声空间中的位置
        /// @return 噪声值，范围约 [-1, 1]
        [[nodiscard]] double noise(double x) const noexcept;

    private:
        static constexpr std::size_t TABLE_SIZE = 256; ///< 查找表大小

        /// 平滑步进衰减函数：6t^5 - 15t^4 + 10t^3
        /// 使噪声在整数边界处平滑过渡
        [[nodiscard]] static constexpr double fade(double t) noexcept {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        /// 线性插值
        [[nodiscard]] static constexpr double lerp(double a, double b, double t) noexcept {
            return a + (b - a) * t;
        }

        std::array<double, TABLE_SIZE> values_; ///< 随机值查找表
    };

    /// 基于导数估计的运动预测器
    /// 通过速度和加速度预测实现平滑跟踪
    /// 核心思想：利用误差变化率和加速度进行运动学预测
    class DerivativePredictor {
    public:
        DerivativePredictor() = default;

        /// 重置所有内部状态
        void reset() noexcept;

        /// 根据误差历史预测未来位置
        /// 使用运动学方程：x = v*t + 0.5*a*t^2
        /// @param error_x 当前 X 轴误差
        /// @param error_y 当前 Y 轴误差
        /// @param prev_move_x 上一次 X 轴移动量
        /// @param prev_move_y 上一次 Y 轴移动量
        /// @param dt 时间增量（秒）
        /// @return 预测的 (x, y) 偏移量
        [[nodiscard]] std::pair<double, double> predict(
            double error_x,
            double error_y,
            double prev_move_x,
            double prev_move_y,
            double dt);

    private:
        double last_error_x_ = 0.0;  ///< 上一次 X 轴误差
        double last_error_y_ = 0.0;  ///< 上一次 Y 轴误差
        double smooth_vel_x_ = 0.0;  ///< 平滑后的 X 轴速度
        double smooth_vel_y_ = 0.0;  ///< 平滑后的 Y 轴速度
        double smooth_acc_x_ = 0.0;  ///< 平滑后的 X 轴加速度
        double smooth_acc_y_ = 0.0;  ///< 平滑后的 Y 轴加速度
        bool has_last_ = false;      ///< 是否有历史数据
    };

    /// AimController 输出结构体
    struct AimOutput {
        double move_x;       ///< X 轴移动输出
        double move_y;       ///< Y 轴移动输出
        double curve_len;    ///< 融合误差向量的长度
        double predicted_x;  ///< 预测的 X 轴偏移
        double predicted_y;  ///< 预测的 Y 轴偏移
        double fused_x;      ///< 融合后的 X 轴误差（原始 + 预测 + 曲线）
        double fused_y;      ///< 融合后的 Y 轴误差（原始 + 预测 + 曲线）
    };

    /// PID 瞄准控制器，集成预测和曲线平滑
    /// 
    /// 该控制器整合以下功能：
    /// - 增量式 PID 控制实现平滑跟踪
    /// - 基于导数的运动预测
    /// - 柏林噪声产生自然运动
    /// - 曲线调整优化接近轨迹
    /// 
    /// 工作流程：
    /// 1. 目标跳变检测 → 重置状态
    /// 2. 运动预测 → 估计目标未来位置
    /// 3. 曲线调整 → 产生自然接近轨迹
    /// 4. PID 控制 → 计算移动量
    /// 5. 噪声叠加 → 模拟人类操作
    class AimController {
    public:
        AimController();

        /// 重置所有内部状态
        void reset() noexcept;

        /// 配置双轴 PID 参数
        /// @param kp 比例增益
        /// @param ki 积分增益
        /// @param kd 微分增益
        void configure_pid(double kp, double ki, double kd) noexcept;

        /// 配置 X 轴 PID 参数
        void configure_pid_x(double kp, double ki, double kd) noexcept;

        /// 配置 Y 轴 PID 参数
        void configure_pid_y(double kp, double ki, double kd) noexcept;

        /// 设置输出限幅，用于抗积分饱和
        void set_output_limits(double min_val, double max_val) noexcept;

        /// 使用新目标误差更新控制器
        /// @param raw_x 原始 X 轴误差（到目标的距离）
        /// @param raw_y 原始 Y 轴误差（到目标的距离）
        /// @param pred_weight_x X 轴预测权重（0-1）
        /// @param pred_weight_y Y 轴预测权重（0-1）
        /// @param init_scale 初始输出缩放比例（0-1），用于渐入效果
        /// @param ramp_time 渐入时间（秒），从 init_scale 到 1.0 的时间
        /// @param output_max 最大输出幅度
        /// @param noise_amp 噪声幅度，用于模拟人类操作的自然抖动
        /// @return AimOutput 包含移动量和调试信息
        [[nodiscard]] AimOutput update(
            double raw_x,
            double raw_y,
            double pred_weight_x,
            double pred_weight_y,
            double init_scale,
            double ramp_time,
            double output_max,
            double noise_amp);

        /// 访问 X 轴 PID 控制器（只读）
        [[nodiscard]] const IncrementalPid& pid_x() const noexcept { return pid_x_; }
        /// 访问 Y 轴 PID 控制器（只读）
        [[nodiscard]] const IncrementalPid& pid_y() const noexcept { return pid_y_; }

    private:
        IncrementalPid pid_x_;   ///< X 轴增量式 PID 控制器
        IncrementalPid pid_y_;   ///< Y 轴增量式 PID 控制器
        double last_raw_x_ = 0.0;       ///< 上一次原始 X 轴误差
        double last_raw_y_ = 0.0;       ///< 上一次原始 Y 轴误差
        double last_output_x_ = 0.0;    ///< 上一次 X 轴输出
        double last_output_y_ = 0.0;    ///< 上一次 Y 轴输出
        std::chrono::steady_clock::time_point last_time_;  ///< 上一次更新时间
        std::chrono::steady_clock::time_point lock_start_; ///< 目标锁定开始时间
        PerlinNoise1D noise_x_;         ///< X 轴柏林噪声生成器
        PerlinNoise1D noise_y_;         ///< Y 轴柏林噪声生成器
        double noise_time_x_ = 0.0;     ///< X 轴噪声时间参数
        double noise_time_y_ = 100.0;   ///< Y 轴噪声时间参数（初始偏移避免同步）
        DerivativePredictor predictor_; ///< 运动预测器
    };

} // namespace aim