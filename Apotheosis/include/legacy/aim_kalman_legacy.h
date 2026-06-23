#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

// =============================================================================
// 2D 目标估计器 —— 真·卡尔曼滤波(常加速度模型),极简参数面
// -----------------------------------------------------------------------------
// 引擎是教科书意义上的卡尔曼滤波:逐帧传播完整 3x3 误差协方差 P,用过程噪声 Q 与
// 测量噪声 R 在线计算最优增益 K(增益随不确定度自适应,刚锁定时快贴合、稳定后强平滑)。
// 运动模型为常加速度(状态 = [位置,速度,加速度]),对变速/转向无系统滞后。
// 内部还自带两项自适应,无需用户操心:
//   · 新息自适应过程噪声:用归一化新息(NIS)检测跳变/急转,瞬间放大 Q 快速重捕;
//   · 按目标尺寸缩放测量噪声:小目标/远距离自动加重平滑。
//
// 对外只暴露两个旋钮(其余全部内部最优默认):
//   平滑度 smoothness [0,1]: 越大越平滑抗抖(更信运动模型),越小越紧跟检测(反应快)。
//   预测提前量 lead  [0,2]: 卡尔曼向前外推的提前量强度。0=不预测(只平滑当前位置),
//                           1≈物理正确提前量,>1 主动多领先(适合快速横移/流水线)。
// =============================================================================

namespace aim
{

struct AimKalmanSettings
{
    bool   enabled = true;
    double smoothness = 0.5;   // [0,1] 平滑度
    double lead       = 1.0;   // [0,2] 预测提前量
};

struct AimKalmanTelemetry
{
    bool initialized = false;
    bool enabled = true;
    double dt = 0.0;
    int warmup_remaining = 0;

    double measurement_x = 0.0;
    double measurement_y = 0.0;
    double estimate_x = 0.0;
    double estimate_y = 0.0;
    double predicted_x = 0.0;
    double predicted_y = 0.0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
    double innovation_x = 0.0;
    double innovation_y = 0.0;
};

class AimKalman2D
{
public:
    AimKalman2D() { reset(); }

    void setSettings(const AimKalmanSettings& settings)
    {
        settings_ = clampSettings(settings);
        warmupRemaining_ = std::clamp(warmupRemaining_, 0, kWarmupFrames);
    }

    const AimKalmanSettings& settings() const { return settings_; }

    // 当前锁定目标 bbox 半径(短边/2,检测像素)。<=0 → 信任度 1。
    void setMeasurementHalfExtent(double halfExtentPx)
    {
        if (!(halfExtentPx > 0.0) || !std::isfinite(halfExtentPx))
        {
            measTrust_ = 1.0;
            return;
        }
        measTrust_ = std::clamp(halfExtentPx / kSizeRefPx, kSizeTrustMin, 1.0);
    }

    void reset()
    {
        xAxis_ = Axis();
        yAxis_ = Axis();
        initialized_ = false;
        warmupRemaining_ = kWarmupFrames;
        nisFast_ = 1.0;
        nisSlow_ = 1.0;
        measTrust_ = 1.0;
    }

    bool initialized() const { return initialized_; }

    std::pair<double, double> position() const
    {
        return { xAxis_.s[0], yAxis_.s[0] };
    }

    std::pair<double, double> velocity() const
    {
        return { xAxis_.s[1], yAxis_.s[1] };
    }

    // 预测点 = position + lead * (速度积分 + 加速度积分)。预热未结束/未初始化返回当前位置。
    std::pair<double, double> predict(double lookaheadSec) const
    {
        if (!initialized_)
            return {};
        const double lookahead = std::clamp(lookaheadSec, 0.0, 1.5);
        if (lookahead <= 0.0 || warmupRemaining_ > 0)
            return position();
        return { predictAxis(xAxis_, lookahead), predictAxis(yAxis_, lookahead) };
    }

    AimKalmanTelemetry update(double measurementX, double measurementY, double dt, double lookaheadSec)
    {
        AimKalmanTelemetry tlm;
        tlm.enabled = settings_.enabled;
        tlm.measurement_x = measurementX;
        tlm.measurement_y = measurementY;

        const double clampedDt = std::clamp(dt, 1e-4, 0.25);
        const double lookahead = std::clamp(lookaheadSec, 0.0, 1.5);
        tlm.dt = clampedDt;

        if (!initialized_)
        {
            initialize(measurementX, measurementY);
            return buildTelemetry(measurementX, measurementY, 0.0, 0.0, lookahead, clampedDt);
        }

        if (!settings_.enabled)
        {
            const double prevX = xAxis_.s[0];
            const double prevY = yAxis_.s[0];
            xAxis_.s[1] = clampAbs((measurementX - prevX) / clampedDt, kMaxVelocity);
            yAxis_.s[1] = clampAbs((measurementY - prevY) / clampedDt, kMaxVelocity);
            xAxis_.s[0] = measurementX;
            yAxis_.s[0] = measurementY;
            return buildTelemetry(measurementX, measurementY,
                                  measurementX - prevX, measurementY - prevY, lookahead, clampedDt);
        }

        const double R = measurementNoise();
        const double boost = currentProcessBoost();
        const double innovX = kfStep(xAxis_, measurementX, clampedDt, R, boost);
        const double innovY = kfStep(yAxis_, measurementY, clampedDt, R, boost);
        updateNis(innovX, innovY, R);

        return buildTelemetry(measurementX, measurementY, innovX, innovY, lookahead, clampedDt);
    }

private:
    struct Axis
    {
        double s[3]    = { 0.0, 0.0, 0.0 };
        double P[3][3] = { {0,0,0}, {0,0,0}, {0,0,0} };
    };

    // ---- 内部固定常量(用户无需关心)----
    static constexpr int    kWarmupFrames = 3;        // 预热帧
    static constexpr double kMaxVelocity  = 20000.0;  // 速度硬上限(px/s)
    static constexpr double kVelDamping   = 0.08;     // 预测速度阻尼
    static constexpr double kJerkScale    = 1.5e5;    // 过程噪声急动量级
    static constexpr double kRBase        = 4.0;      // 测量噪声基准(px^2)
    static constexpr double kManeuverThr  = 2.5;      // 机动判定阈值(快/慢 NIS 比)
    static constexpr double kMaxBoost     = 4.0;      // 机动时过程噪声最大放大
    static constexpr double kSizeRefPx    = 36.0;     // 完全信任的目标半径
    static constexpr double kSizeTrustMin = 0.35;     // 小目标信任下限

    static AimKalmanSettings clampSettings(const AimKalmanSettings& in)
    {
        AimKalmanSettings out = in;
        out.smoothness = std::clamp(out.smoothness, 0.0, 1.0);
        out.lead       = std::clamp(out.lead,       0.0, 2.0);
        return out;
    }

    static double clampAbs(double v, double a) { return std::clamp(v, -a, a); }

    // 测量噪声 R:平滑度越大 R 越大(越平滑);小目标 R 再放大。
    double measurementNoise() const
    {
        double R = kRBase * (0.25 + 4.0 * settings_.smoothness);
        R /= std::max(1e-3, measTrust_ * measTrust_);
        return std::max(R, 1e-3);
    }

    // 过程噪声 PSD:平滑度越大 Q 越小(越平滑稳定)。
    double processPsd() const
    {
        const double sigmaJerk = kJerkScale * (1.2 - settings_.smoothness) + 1.0;
        return sigmaJerk * sigmaJerk;
    }

    // 机动自适应放大倍数(基于上一帧的 NIS 快/慢 EWMA)。
    double currentProcessBoost() const
    {
        const double ratio = nisFast_ / std::max(1e-6, nisSlow_);
        const double slope = (kMaxBoost - 1.0) / std::max(1e-6, kManeuverThr - 1.0);
        return std::clamp(1.0 + (ratio - 1.0) * slope, 1.0, kMaxBoost);
    }

    void updateNis(double innovX, double innovY, double R)
    {
        const double nis = (innovX * innovX + innovY * innovY) / std::max(1e-6, 2.0 * R);
        constexpr double kFast = 0.45;
        constexpr double kSlow = 0.02;
        nisFast_ += kFast * (nis - nisFast_);
        nisSlow_ += kSlow * (nis - nisSlow_);
        nisSlow_ = std::max(nisSlow_, 0.5);
    }

    void initialize(double zx, double zy)
    {
        xAxis_ = Axis();
        yAxis_ = Axis();
        xAxis_.s[0] = zx;
        yAxis_.s[0] = zy;
        const double pv = kMaxVelocity * kMaxVelocity;
        const double pa = pv * 16.0;
        const double r  = measurementNoise();
        xAxis_.P[0][0] = yAxis_.P[0][0] = r;
        xAxis_.P[1][1] = yAxis_.P[1][1] = pv;
        xAxis_.P[2][2] = yAxis_.P[2][2] = pa;
        warmupRemaining_ = kWarmupFrames;
        nisFast_ = 1.0;
        nisSlow_ = 1.0;
        initialized_ = true;
    }

    // 单轴完整卡尔曼一步(预测 + 更新),返回新息。常加速度模型。
    double kfStep(Axis& a, double z, double dt, double R, double qBoost)
    {
        const double dt2 = dt * dt;

        // 预测: x = F x
        double xp[3];
        xp[0] = a.s[0] + dt * a.s[1] + 0.5 * dt2 * a.s[2];
        xp[1] = a.s[1] + dt * a.s[2];
        xp[2] = a.s[2];

        // P_pred = F P F^T
        double Pp[3][3];
        fPfT(a.P, dt, Pp);

        // 过程噪声 Q(连续白噪声急动) * 机动放大
        const double q = processPsd() * qBoost;
        const double t = dt, t2 = t*t, t3 = t2*t, t4 = t3*t, t5 = t4*t;
        Pp[0][0] += q * t5 / 20.0; Pp[0][1] += q * t4 / 8.0; Pp[0][2] += q * t3 / 6.0;
        Pp[1][0] += q * t4 / 8.0;  Pp[1][1] += q * t3 / 3.0; Pp[1][2] += q * t2 / 2.0;
        Pp[2][0] += q * t3 / 6.0;  Pp[2][1] += q * t2 / 2.0; Pp[2][2] += q * t;

        // 更新: H = [1,0,0]
        const double S = Pp[0][0] + R;
        const double innov = z - xp[0];
        const double Sinv = (S > 1e-12) ? 1.0 / S : 0.0;
        const double K0 = Pp[0][0] * Sinv;
        const double K1 = Pp[1][0] * Sinv;
        const double K2 = Pp[2][0] * Sinv;

        a.s[0] = xp[0] + K0 * innov;
        a.s[1] = xp[1] + K1 * innov;
        a.s[2] = xp[2] + K2 * innov;

        // P = (I - K H) P_pred  (H 仅取第 0 行)
        for (int j = 0; j < 3; ++j)
        {
            const double r0 = Pp[0][j];
            a.P[0][j] = Pp[0][j] - K0 * r0;
            a.P[1][j] = Pp[1][j] - K1 * r0;
            a.P[2][j] = Pp[2][j] - K2 * r0;
        }

        a.s[1] = clampAbs(a.s[1], kMaxVelocity);
        a.s[2] = clampAbs(a.s[2], kMaxVelocity * 20.0);
        return innov;
    }

    // P_pred = F P F^T, F = [[1,dt,dt^2/2],[0,1,dt],[0,0,1]]
    static void fPfT(const double P[3][3], double dt, double out[3][3])
    {
        const double f01 = dt, f02 = 0.5 * dt * dt, f12 = dt;
        double FP[3][3];
        FP[0][0] = P[0][0] + f01*P[1][0] + f02*P[2][0];
        FP[0][1] = P[0][1] + f01*P[1][1] + f02*P[2][1];
        FP[0][2] = P[0][2] + f01*P[1][2] + f02*P[2][2];
        FP[1][0] = P[1][0] + f12*P[2][0];
        FP[1][1] = P[1][1] + f12*P[2][1];
        FP[1][2] = P[1][2] + f12*P[2][2];
        FP[2][0] = P[2][0];
        FP[2][1] = P[2][1];
        FP[2][2] = P[2][2];
        out[0][0] = FP[0][0] + f01*FP[0][1] + f02*FP[0][2];
        out[0][1] = FP[0][1] + f12*FP[0][2];
        out[0][2] = FP[0][2];
        out[1][0] = FP[1][0] + f01*FP[1][1] + f02*FP[1][2];
        out[1][1] = FP[1][1] + f12*FP[1][2];
        out[1][2] = FP[1][2];
        out[2][0] = FP[2][0] + f01*FP[2][1] + f02*FP[2][2];
        out[2][1] = FP[2][1] + f12*FP[2][2];
        out[2][2] = FP[2][2];
    }

    double predictAxis(const Axis& a, double lookahead) const
    {
        const double v = clampAbs(a.s[1], kMaxVelocity);
        const double d = kVelDamping;
        const double velTerm = (d <= 1e-6) ? v * lookahead
                                           : v * (1.0 - std::exp(-d * lookahead)) / d;
        double accTerm = 0.5 * a.s[2] * lookahead * lookahead;
        const double cap = std::abs(velTerm);
        accTerm = std::clamp(accTerm, -cap, cap);
        return a.s[0] + settings_.lead * (velTerm + accTerm);
    }

    AimKalmanTelemetry buildTelemetry(double mx, double my, double innovX, double innovY,
                                      double lookahead, double dt)
    {
        AimKalmanTelemetry tlm;
        tlm.initialized = initialized_;
        tlm.enabled = settings_.enabled;
        tlm.dt = dt;
        tlm.warmup_remaining = warmupRemaining_;
        tlm.measurement_x = mx;
        tlm.measurement_y = my;
        tlm.estimate_x = xAxis_.s[0];
        tlm.estimate_y = yAxis_.s[0];
        tlm.velocity_x = xAxis_.s[1];
        tlm.velocity_y = yAxis_.s[1];
        tlm.innovation_x = innovX;
        tlm.innovation_y = innovY;

        if (warmupRemaining_ > 0)
        {
            --warmupRemaining_;
            tlm.predicted_x = xAxis_.s[0];
            tlm.predicted_y = yAxis_.s[0];
            tlm.warmup_remaining = warmupRemaining_;
            return tlm;
        }

        auto fut = predict(lookahead);
        tlm.predicted_x = fut.first;
        tlm.predicted_y = fut.second;
        return tlm;
    }

private:
    AimKalmanSettings settings_{};
    Axis xAxis_{};
    Axis yAxis_{};
    bool initialized_ = false;
    int  warmupRemaining_ = 0;

    double nisFast_ = 1.0;
    double nisSlow_ = 1.0;
    double measTrust_ = 1.0;
};

} // namespace aim
