#include "pid_controller.h"

#include <algorithm>
#include <cmath>

namespace control {

double stabilityRatio(double kp, double dtSec)
{
    if (kCriticalGain <= 0.0 || dtSec <= 0.0)
        return 0.0;
    // g = Kp · dt · k̂  —— 这是回路增益（无量纲）。
    const double g = kp * dtSec * kCountsPerPixel;
    return g / kCriticalGain;
}

namespace {

// 把误差夹到 P 项饱和区（连续饱和，不是死区）。
// ★ 与死区的本质区别：这个函数【经过原点且连续】，
//   所以不存在"误差小于阈值就完全不出力"那个状态，
//   也就不存在死区造成的极限环（实测 10.2 次/秒的翻转抖动）。
double saturateForP(double e, double fullScale)
{
    if (fullScale <= 0.0)
        return e;
    return std::clamp(e, -fullScale, fullScale);
}

} // namespace

double PidController::stepAxis(Axis axis, double error, double dtSec,
                               AxisState& st, AxisTelemetry& tm, bool& unwound)
{
    unwound = false;

    const double kp = (axis == Axis::X) ? cfg_.kpX : cfg_.kpY;
    const double ki = (axis == Axis::X) ? cfg_.kiX : cfg_.kiY;
    const double kd = (axis == Axis::X) ? cfg_.kdX : cfg_.kdY;

    // ── 1. 积分（先判回吐，再累加） ─────────────────────────────────────
    if (st.hasPrev && error * st.prevError < 0.0)
    {
        // ★ 按分量判反向（用户决定）。压枪（y）与跟枪（x）是独立发生的，
        //   一个反向不代表另一个也反向。
        // ★ 收紧到几十毫秒（用户修正了历史上的 0.2s）—— 见 pid_controller.h。
        const double decay = std::exp(-dtSec / std::max(cfg_.tauUnwindSec, 1e-6));
        st.integral *= decay;
        unwound = true;
    }

    if (ki != 0.0)
    {
        st.integral += ki * error * dtSec;

        // ── clamp 抗饱和（非反算式；反算式要整定 Tt，而 Tt 无实测依据） ──
        const double iMax = (cfg_.iMax > 0.0)
            ? cfg_.iMax
            : std::max(1.0, static_cast<double>(cfg_.maxOutputCounts));
        st.integral = std::clamp(st.integral, -iMax, iMax);
    }
    else
    {
        // ki == 0 时积分器保持 0，避免"关了积分但历史值还在起作用"。
        st.integral = 0.0;
    }

    // ── 2. 微分（作用在低通后的 de 上） ──────────────────────────────────
    double deriv = 0.0;
    if (kd != 0.0)
    {
        const double de = error - st.prevError;
        if (cfg_.tauDerivSec > 0.0 && st.hasPrev)
        {
            // 一阶低通：lp += (de − lp) · a，a = 1 − exp(−dt/τ)
            const double a = 1.0 - std::exp(-dtSec / cfg_.tauDerivSec);
            st.derivLp += (de - st.derivLp) * a;
        }
        else
        {
            st.derivLp = de;
        }
        deriv = st.derivLp / dtSec;
    }
    else
    {
        st.derivLp = 0.0;
    }

    // ── 3. FK 结构求和 ───────────────────────────────────────────────────
    // ★★ 无死区（已整项删除，见 pid_controller.h 与方案 §5.1）。
    //    末段不冲过头靠 saturateForP 的连续饱和 —— 它经过原点且连续，
    //    所以不存在"误差太小就完全不出力"那个状态，也就没有死区的极限环。
    const double p = saturateForP(error, cfg_.pFullScalePx);
    const double u = dtSec * kp * (p + st.integral + kd * deriv);

    // ── 4. 出口：唯一一次量化 + 余量结转 ─────────────────────────────────
    const double limited = std::clamp(
        u,
        -static_cast<double>(cfg_.maxOutputCounts),
        static_cast<double>(cfg_.maxOutputCounts));

    const double withCarry = limited + st.carry;
    const double rounded = std::round(withCarry);
    // ★ 截掉的位移【不攒成欠账】（§3.2.2）：所以在限幅【之后】才做余量结转，
    //   否则被限幅砍掉的部分会以 carry 的形式偷偷积累，表现为"松手后冲一下"。
    const double iMaxOut = static_cast<double>(cfg_.maxOutputCounts);
    const double countsClamped = std::clamp(rounded, -iMaxOut, iMaxOut);

    st.carry = withCarry - countsClamped;

    // ── 5. 状态推进与遥测 ────────────────────────────────────────────────
    st.prevError = error;
    st.hasPrev = true;

    tm.error = error;
    tm.p = p;
    tm.i = st.integral;
    tm.d = kd * deriv;
    tm.u = u;
    tm.counts = static_cast<int>(countsClamped);
    tm.carry = st.carry;
    tm.stabilityRatio = stabilityRatio(kp, dtSec);

    return u;
}

Counts PidController::update(const Vec2& anchor, const Vec2& cross, double dtSec)
{
    Counts out;

    // dt 异常：本拍不输出、不改状态。
    // ★ 不"猜"一个 dt —— 那会在速度项里混进假速度，而假速度比不输出糟得多。
    if (!(dtSec > 0.0))
        return out;

    const double eX = anchor.x - cross.x;
    const double eY = anchor.y - cross.y;

    bool uwX = false, uwY = false;
    stepAxis(Axis::X, eX, dtSec, stateX_, telemetry_.x, uwX);
    stepAxis(Axis::Y, eY, dtSec, stateY_, telemetry_.y, uwY);
    telemetry_.unwoundX = uwX;
    telemetry_.unwoundY = uwY;

    out.x = telemetry_.x.counts;
    out.y = telemetry_.y.counts;
    return out;
}

void PidController::reset()
{
    stateX_.reset();
    stateY_.reset();
    telemetry_ = ControlTelemetry{};
}

} // namespace control
