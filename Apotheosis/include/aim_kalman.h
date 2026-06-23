#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

// =============================================================================
// 2D 目标估计器 —— IMM (Interacting Multiple Model) 双模卡尔曼 · 三角洲专用
// -----------------------------------------------------------------------------
// 双模 IMM，每轴并行跑两个滤波器，按本帧新息似然给出模型后验概率，再加权融合输出：
//
//   · 模型 A —— 常加速度（CA）+ STF 强跟踪渐消因子 λ
//       状态 [位置, 速度, 加速度]，F 为标准 CA 转移矩阵，过程噪声为急动 PSD。
//       周东华 STF λ 对模型失配/机动突变做在线渐消，跟得住快速横移与急转。
//
//   · 模型 B —— 急停阻尼（Damped Stop）
//       同 [位置, 速度, 加速度] 状态，但每步预测后对速度/加速度按阻尼系数 ρ_v、ρ_a
//       衰减；过程噪声偏向位置项、速度项收紧。这等价于先验"目标趋向于停止"，
//       一旦目标急停（贴掩体侧身蹲下）模型 B 的新息会瞬间小于模型 A，似然主导 → 后验
//       概率向 B 偏移，融合输出的速度估计被压向 0，PID 速度前馈不再过冲。
//
// 模式转移矩阵 P[i][j] 表示从模型 i 转到模型 j 的先验概率：
//      [ P_AA  P_AB ]   [ 0.95  0.05 ]
//      [ P_BA  P_BB ] = [ 0.15  0.85 ]
//   设计意图：模型 A 是默认（运动），偶尔切到 B；一旦切到 B 倾向于停一会再回 A。
//
// IMM 标准三步循环：
//   ① Mixing       —— 用 μ_{j|i} = P_{ji}·μ_j / c_i 把上一拍状态做"交互混合"
//   ② Filter step  —— 每个模型用自己的混合先验做 predict+update，得似然 Λ_i
//   ③ Combination  —— 用更新后的 μ_i 加权 x_i、P_i，得本拍融合输出
//
// 对外仍只暴露两个旋钮（其余全部内部最优默认）：
//   平滑度 smoothness [0,1]: 越大越平滑抗抖,越小越紧跟检测。
//   预测提前量 lead  [0,2]:  卡尔曼向前外推的提前量强度。0=不预测,1≈物理正确,>1 主动多领先。
//
// API 与上一代单模型版本完全兼容：position()/velocity()/predict()/update() 行为不变，
// telemetry 中 lambda_x/y 现在汇报模型 A 的渐消因子（模型 B 不用 STF）。
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
    // STF 渐消因子 (模型 A 的 λ ≥ 1)。> 1 = 检测到机动/突变正在强跟踪重捕。
    double lambda_x = 1.0;
    double lambda_y = 1.0;
    // IMM 模型 B (急停阻尼) 当前后验概率 ∈ [0,1]。接近 1 = 模型相信目标已停。
    // 调试用：开火时如果这个值飙到 0.7+ 说明目标在掩体后急停，PID 前馈自动收敛。
    double stop_prob_x = 0.0;
    double stop_prob_y = 0.0;
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
        xAxis_ = ImmAxis();
        yAxis_ = ImmAxis();
        initialized_ = false;
        warmupRemaining_ = kWarmupFrames;
        measTrust_ = 1.0;
    }

    bool initialized() const { return initialized_; }

    std::pair<double, double> position() const
    {
        return { combinedPos(xAxis_), combinedPos(yAxis_) };
    }

    std::pair<double, double> velocity() const
    {
        return { combinedVel(xAxis_), combinedVel(yAxis_) };
    }

    std::pair<double, double> predict(double lookaheadSec) const
    {
        if (!initialized_)
            return {};
        const double lookahead = std::clamp(lookaheadSec, 0.0, 1.5);
        if (lookahead <= 0.0 || warmupRemaining_ > 0)
            return position();
        return { predictImmAxis(xAxis_, lookahead), predictImmAxis(yAxis_, lookahead) };
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
            return buildTelemetry(measurementX, measurementY, 0.0, 0.0, 1.0, 1.0, lookahead, clampedDt);
        }

        // 卡尔曼关掉时直接拟合差分，不进 IMM。
        if (!settings_.enabled)
        {
            const double prevX = combinedPos(xAxis_);
            const double prevY = combinedPos(yAxis_);
            const double vx = clampAbs((measurementX - prevX) / clampedDt, kMaxVelocity);
            const double vy = clampAbs((measurementY - prevY) / clampedDt, kMaxVelocity);
            // 把两个模型都拉到测量值，模式概率重置。
            for (auto& m : xAxis_.models) { m.s[0] = measurementX; m.s[1] = vx; m.s[2] = 0.0; }
            for (auto& m : yAxis_.models) { m.s[0] = measurementY; m.s[1] = vy; m.s[2] = 0.0; }
            xAxis_.mu[0] = yAxis_.mu[0] = 1.0;
            xAxis_.mu[1] = yAxis_.mu[1] = 0.0;
            return buildTelemetry(measurementX, measurementY,
                                  measurementX - prevX, measurementY - prevY,
                                  1.0, 1.0, lookahead, clampedDt);
        }

        const double R    = measurementNoise();
        const double q    = processPsd();
        const double beta = weakeningFactor();

        double lambdaX = 1.0, lambdaY = 1.0;
        const double innovX = immStep(xAxis_, measurementX, clampedDt, R, q, beta, lambdaX);
        const double innovY = immStep(yAxis_, measurementY, clampedDt, R, q, beta, lambdaY);

        return buildTelemetry(measurementX, measurementY, innovX, innovY,
                              lambdaX, lambdaY, lookahead, clampedDt);
    }

private:
    // ---- 单个滤波器实例（IMM 中的一种模型）----
    struct ModelState
    {
        double s[3]    = { 0.0, 0.0, 0.0 };       // [pos, vel, acc]
        double P[3][3] = { {0,0,0}, {0,0,0}, {0,0,0} };
        double v0      = 0.0;                      // STF 渐消记忆新息方差（仅模型 A 用）
        bool   hasV0   = false;
    };

    // ---- 每轴 IMM 容器：M=2 模型并行 ----
    struct ImmAxis
    {
        ModelState models[2];   // [0]=CA, [1]=DampedStop
        double mu[2] = { 0.8, 0.2 };  // 初始模式概率：默认相信目标在运动
    };

    // ---- 内部常量 ----
    static constexpr int    kWarmupFrames = 3;
    static constexpr double kMaxVelocity  = 20000.0;
    static constexpr double kVelDamping   = 0.08;
    static constexpr double kJerkScale    = 1.0e5;
    static constexpr double kRBase        = 4.0;
    static constexpr double kSizeRefPx    = 36.0;
    static constexpr double kSizeTrustMin = 0.35;
    // STF 常量
    static constexpr double kStfRho    = 0.95;
    static constexpr double kStfBeta0  = 1.0;
    static constexpr double kStfBetaK  = 3.0;
    static constexpr double kLambdaMax = 100.0;
    // IMM 转移矩阵 P[i][j] = P(mode i → j)。模型 0=CA（运动），1=Stop（急停）。
    // 行和必须 = 1。CA 是默认且粘性；Stop 不那么粘，便于目标恢复运动后快速切回。
    static constexpr double kImmTrans[2][2] = {
        { 0.95, 0.05 },
        { 0.15, 0.85 }
    };
    // 模型 B 的阻尼系数：每步预测后乘到速度 / 加速度上。
    // ρ_v = 0.70 意味着 vel 每帧衰减 30%，~3-4 帧降到 1/3。三角洲一次急停 ~50ms = 6 帧
    // 已 100Hz 检测下足够把速度估计压到接近 0。
    static constexpr double kStopVelDamp = 0.70;
    static constexpr double kStopAccDamp = 0.50;
    // 模型 B 的过程噪声放大系数（让位置可塑、速度/加速度被压制）。
    static constexpr double kStopQPosScale = 2.0;
    static constexpr double kStopQVelScale = 0.10;
    static constexpr double kStopQAccScale = 0.10;
    // 模式概率下限，防止某个模型概率塌到 0 之后无法恢复。
    static constexpr double kMuFloor = 0.02;
    // 似然下限，防止 exp(-huge) 直接归零。
    static constexpr double kLikelihoodFloor = 1e-30;

    static AimKalmanSettings clampSettings(const AimKalmanSettings& in)
    {
        AimKalmanSettings out = in;
        out.smoothness = std::clamp(out.smoothness, 0.0, 1.0);
        out.lead       = std::clamp(out.lead,       0.0, 2.0);
        return out;
    }

    static double clampAbs(double v, double a) { return std::clamp(v, -a, a); }

    double measurementNoise() const
    {
        double R = kRBase * (0.25 + 4.0 * settings_.smoothness);
        R /= std::max(1e-3, measTrust_ * measTrust_);
        return std::max(R, 1e-3);
    }

    double processPsd() const
    {
        const double sigmaJerk = kJerkScale * (1.2 - settings_.smoothness) + 1.0;
        return sigmaJerk * sigmaJerk;
    }

    double weakeningFactor() const
    {
        return kStfBeta0 + kStfBetaK * settings_.smoothness;
    }

    void initialize(double zx, double zy)
    {
        xAxis_ = ImmAxis();
        yAxis_ = ImmAxis();
        const double pv = kMaxVelocity * kMaxVelocity;
        const double pa = pv * 16.0;
        const double r  = measurementNoise();
        for (int m = 0; m < 2; ++m)
        {
            xAxis_.models[m].s[0] = zx;
            yAxis_.models[m].s[0] = zy;
            xAxis_.models[m].P[0][0] = yAxis_.models[m].P[0][0] = r;
            xAxis_.models[m].P[1][1] = yAxis_.models[m].P[1][1] = pv;
            xAxis_.models[m].P[2][2] = yAxis_.models[m].P[2][2] = pa;
        }
        warmupRemaining_ = kWarmupFrames;
        initialized_ = true;
    }

    // ---- 状态融合（IMM 输出层）----
    static double combinedPos(const ImmAxis& a)
    {
        return a.mu[0] * a.models[0].s[0] + a.mu[1] * a.models[1].s[0];
    }
    static double combinedVel(const ImmAxis& a)
    {
        return a.mu[0] * a.models[0].s[1] + a.mu[1] * a.models[1].s[1];
    }

    // 单轴 IMM 一步：mixing → 各模型 filter step → mode prob 更新 → combine。
    // 返回融合新息（z − combinedPos_prior），lambdaOut 返回模型 A 的 STF 渐消因子。
    double immStep(ImmAxis& a, double z, double dt, double R, double q, double beta,
                   double& lambdaOut)
    {
        // ---- ① Mixing：交互混合 ----
        // c_j = Σ_i P_{ij} × μ_i^{k-1}    (j 是目标模型)
        // μ_{i|j} = P_{ij} × μ_i / c_j     (i→j 条件混合概率)
        double c[2];
        c[0] = kImmTrans[0][0] * a.mu[0] + kImmTrans[1][0] * a.mu[1];
        c[1] = kImmTrans[0][1] * a.mu[0] + kImmTrans[1][1] * a.mu[1];
        // 防止 c = 0
        c[0] = std::max(c[0], 1e-12);
        c[1] = std::max(c[1], 1e-12);

        double w[2][2];  // w[j][i] = P(模型 i 在上一拍 | 当前混合到模型 j)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                w[j][i] = kImmTrans[i][j] * a.mu[i] / c[j];

        // 混合状态 x_{0j} 和协方差 P_{0j}
        ModelState mixed[2];
        for (int j = 0; j < 2; ++j)
        {
            for (int k = 0; k < 3; ++k)
                mixed[j].s[k] = w[j][0] * a.models[0].s[k] + w[j][1] * a.models[1].s[k];

            for (int r = 0; r < 3; ++r)
                for (int cc = 0; cc < 3; ++cc)
                {
                    double v = 0.0;
                    for (int i = 0; i < 2; ++i)
                    {
                        const double ds_r = a.models[i].s[r] - mixed[j].s[r];
                        const double ds_c = a.models[i].s[cc] - mixed[j].s[cc];
                        v += w[j][i] * (a.models[i].P[r][cc] + ds_r * ds_c);
                    }
                    mixed[j].P[r][cc] = v;
                }
            // STF 记忆只传递给同一物理模型——直接继承本模型的 v0/hasV0
            mixed[j].v0 = a.models[j].v0;
            mixed[j].hasV0 = a.models[j].hasV0;
        }

        // ---- ② Filter step：每个模型独立 predict+update，并返回似然 ----
        double S[2];        // 新息协方差
        double innov[2];    // 新息
        double lambda[2] = { 1.0, 1.0 };

        // 模型 A：CA + STF
        innov[0] = kfStepCA(mixed[0], z, dt, R, q, beta, lambda[0], S[0]);
        // 模型 B：CA F + 阻尼 + 调权过程噪声，无 STF
        innov[1] = kfStepStop(mixed[1], z, dt, R, q, S[1]);

        // 写回各模型最新状态
        a.models[0] = mixed[0];
        a.models[1] = mixed[1];

        // ---- ③ Mode probability update ----
        // Λ_j = N(innov_j; 0, S_j) — 高斯似然
        double L[2];
        for (int j = 0; j < 2; ++j)
        {
            const double Sj = std::max(S[j], 1e-9);
            const double e2 = innov[j] * innov[j];
            // 用稳健形式：先算 -0.5 × (e²/S + log(2π S))
            const double logL = -0.5 * (e2 / Sj + std::log(2.0 * 3.14159265358979323846 * Sj));
            L[j] = std::exp(std::max(logL, std::log(kLikelihoodFloor)));
        }

        // μ_j^k = c_j × Λ_j / Σ
        double mu_new[2] = { c[0] * L[0], c[1] * L[1] };
        const double mu_sum = mu_new[0] + mu_new[1];
        if (mu_sum > 1e-12)
        {
            mu_new[0] /= mu_sum;
            mu_new[1] /= mu_sum;
        }
        else
        {
            // 兜底：极端数值情况退回均匀
            mu_new[0] = mu_new[1] = 0.5;
        }
        // 加 floor 防塌缩
        mu_new[0] = std::max(mu_new[0], kMuFloor);
        mu_new[1] = std::max(mu_new[1], kMuFloor);
        const double renorm = mu_new[0] + mu_new[1];
        a.mu[0] = mu_new[0] / renorm;
        a.mu[1] = mu_new[1] / renorm;

        // ---- ④ Combination（供 telemetry / 外部读取；状态本身保留分立）----
        // position()/velocity() 用 a.mu × a.models 计算，无需在此显式融合。

        lambdaOut = lambda[0];  // 报告模型 A 的渐消因子（B 不用 STF）
        // 融合新息：用 mu 加权（与组合状态一致）
        return a.mu[0] * innov[0] + a.mu[1] * innov[1];
    }

    // ---- 模型 A：CA + STF ----（与上一代单模型版本逻辑一致，额外返回 S）
    double kfStepCA(ModelState& a, double z, double dt, double R, double q, double beta,
                    double& lambdaOut, double& Sout)
    {
        const double dt2 = dt * dt;
        double xp[3];
        xp[0] = a.s[0] + dt * a.s[1] + 0.5 * dt2 * a.s[2];
        xp[1] = a.s[1] + dt * a.s[2];
        xp[2] = a.s[2];

        double FPFt[3][3];
        fPfT(a.P, dt, FPFt);

        const double t = dt, t2 = t*t, t3 = t2*t, t4 = t3*t, t5 = t4*t;
        const double Q00 = q * t5 / 20.0;

        const double innov = z - xp[0];

        if (!a.hasV0) { a.v0 = innov * innov; a.hasV0 = true; }
        else          { a.v0 = (kStfRho * a.v0 + innov * innov) / (1.0 + kStfRho); }
        const double N = a.v0 - Q00 - beta * R;
        const double M = FPFt[0][0];
        double lambda = 1.0;
        if (M > 1e-9 && N > M)
            lambda = std::min(N / M, kLambdaMax);
        lambdaOut = lambda;

        double Pp[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                Pp[i][j] = lambda * FPFt[i][j];
        Pp[0][0] += Q00;        Pp[0][1] += q*t4/8.0;  Pp[0][2] += q*t3/6.0;
        Pp[1][0] += q*t4/8.0;   Pp[1][1] += q*t3/3.0;  Pp[1][2] += q*t2/2.0;
        Pp[2][0] += q*t3/6.0;   Pp[2][1] += q*t2/2.0;  Pp[2][2] += q*t;

        const double S = Pp[0][0] + R;
        Sout = S;
        const double Sinv = (S > 1e-12) ? 1.0 / S : 0.0;
        const double K0 = Pp[0][0] * Sinv;
        const double K1 = Pp[1][0] * Sinv;
        const double K2 = Pp[2][0] * Sinv;

        a.s[0] = xp[0] + K0 * innov;
        a.s[1] = xp[1] + K1 * innov;
        a.s[2] = xp[2] + K2 * innov;

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

    // ---- 模型 B：急停阻尼 ----
    // 同 CA 状态空间，但每步预测后将 vel/acc 衰减，过程噪声偏向位置项。
    // 不用 STF（B 的目标就是"相信目标静止"，机动越大它越该掉权重，由 IMM 似然来做）。
    double kfStepStop(ModelState& a, double z, double dt, double R, double q,
                      double& Sout)
    {
        const double dt2 = dt * dt;

        // 预测：CA 转移 + 衰减
        double xp[3];
        xp[0] = a.s[0] + dt * a.s[1] + 0.5 * dt2 * a.s[2];
        xp[1] = (a.s[1] + dt * a.s[2]) * kStopVelDamp;   // 速度被压
        xp[2] = a.s[2] * kStopAccDamp;                    // 加速度被压

        // 协方差预测：FPFt + 阻尼对 vel/acc 的影响（近似为对 P 行/列乘以阻尼平方）
        double FPFt[3][3];
        fPfT(a.P, dt, FPFt);
        // 应用速度/加速度阻尼到 P（保留 0-行/列，因为位置不衰减）
        const double dv = kStopVelDamp, da = kStopAccDamp;
        const double scale[3] = { 1.0, dv, da };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                FPFt[i][j] *= scale[i] * scale[j];

        const double t = dt, t2 = t*t, t3 = t2*t, t4 = t3*t, t5 = t4*t;
        const double Q00 = kStopQPosScale * q * t5 / 20.0;
        const double Q01 = kStopQVelScale * q * t4 / 8.0;
        const double Q02 = kStopQAccScale * q * t3 / 6.0;
        const double Q11 = kStopQVelScale * q * t3 / 3.0;
        const double Q12 = kStopQAccScale * q * t2 / 2.0;
        const double Q22 = kStopQAccScale * q * t;

        double Pp[3][3];
        Pp[0][0] = FPFt[0][0] + Q00;  Pp[0][1] = FPFt[0][1] + Q01;  Pp[0][2] = FPFt[0][2] + Q02;
        Pp[1][0] = FPFt[1][0] + Q01;  Pp[1][1] = FPFt[1][1] + Q11;  Pp[1][2] = FPFt[1][2] + Q12;
        Pp[2][0] = FPFt[2][0] + Q02;  Pp[2][1] = FPFt[2][1] + Q12;  Pp[2][2] = FPFt[2][2] + Q22;

        const double innov = z - xp[0];
        const double S = Pp[0][0] + R;
        Sout = S;
        const double Sinv = (S > 1e-12) ? 1.0 / S : 0.0;
        const double K0 = Pp[0][0] * Sinv;
        const double K1 = Pp[1][0] * Sinv;
        const double K2 = Pp[2][0] * Sinv;

        a.s[0] = xp[0] + K0 * innov;
        a.s[1] = xp[1] + K1 * innov;
        a.s[2] = xp[2] + K2 * innov;

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

    // 单个模型的预测外推（CA 假设）
    double predictModel(const ModelState& a, double lookahead) const
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

    // IMM 融合预测：模型 A 用 CA 外推；模型 B 因为认为目标静止，外推退化为当前位置。
    // 这正是 IMM 抑制急停过冲的关键 —— 当 mu[Stop] 高时，融合预测自动塌向 position。
    double predictImmAxis(const ImmAxis& a, double lookahead) const
    {
        const double predA = predictModel(a.models[0], lookahead);
        const double predB = a.models[1].s[0];  // Stop 模型相信目标在原位
        return a.mu[0] * predA + a.mu[1] * predB;
    }

    AimKalmanTelemetry buildTelemetry(double mx, double my, double innovX, double innovY,
                                      double lambdaX, double lambdaY,
                                      double lookahead, double dt)
    {
        AimKalmanTelemetry tlm;
        tlm.initialized = initialized_;
        tlm.enabled = settings_.enabled;
        tlm.dt = dt;
        tlm.warmup_remaining = warmupRemaining_;
        tlm.measurement_x = mx;
        tlm.measurement_y = my;
        tlm.estimate_x = combinedPos(xAxis_);
        tlm.estimate_y = combinedPos(yAxis_);
        tlm.velocity_x = combinedVel(xAxis_);
        tlm.velocity_y = combinedVel(yAxis_);
        tlm.innovation_x = innovX;
        tlm.innovation_y = innovY;
        tlm.lambda_x = lambdaX;
        tlm.lambda_y = lambdaY;
        tlm.stop_prob_x = xAxis_.mu[1];
        tlm.stop_prob_y = yAxis_.mu[1];

        if (warmupRemaining_ > 0)
        {
            --warmupRemaining_;
            tlm.predicted_x = tlm.estimate_x;
            tlm.predicted_y = tlm.estimate_y;
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
    ImmAxis xAxis_{};
    ImmAxis yAxis_{};
    bool initialized_ = false;
    int  warmupRemaining_ = 0;
    double measTrust_ = 1.0;
};

} // namespace aim
