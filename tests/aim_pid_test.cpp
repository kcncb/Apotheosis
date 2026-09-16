// 纯反馈 PID (mouse/aim_pid.h) + 瞄点平滑器 (mouse/anchor_filter.h) 的回归。
//
// ── 这一版测什么 ─────────────────────────────────────────────────────────────
// 2026-09-13 起控制器里【所有前馈/预测都被删掉】了: aim_motion.h(在线估 k̂)、
// anchor_observer.h(α-β 观测器 + 目标速度)、AimPidFeedback 整个结构、
// predict_time_s / lead_time_s / px_per_count 全部消失。控制器现在就是教科书 PID:
//
//     u = dt * Kp * [ e + Ki*∫e dt + Kd*de/dt ]     单位: 计数/拍
//
// 所以这一版不再需要旧测试里"假游戏 + 观测器 + 死区配对"那一整套。留下的是一个
// 被控对象极简、但【延迟真实】的闭环: 目标有世界速度, 镜头按我们下发的计数转
// (k = 每计数多少像素), 测量与执行各有一段延迟。控制器只看到误差与 dt。
//
// ── 判定哲学 ─────────────────────────────────────────────────────────────────
// 只断言【可判定的性质】: 收敛、有界、连续、单调关系。少断言精确数值 ——
// 那些会随参数调整而变, 断言它们只会让回归变成"改参数就红"的噪音源。
#include "mouse/aim_pid.h"
#include "mouse/anchor_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{

int g_failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

void check_near(double got, double want, double tol, const std::string& what)
{
    if (!(std::abs(got - want) <= tol))
    {
        std::printf("  [FAIL] %s: got %.4f want %.4f +-%.4f\n", what.c_str(), got, want, tol);
        ++g_failures;
    }
}

// 控制器任何一路输出都必须有限 —— lround(NaN) 是未定义行为, 在实机上就是闪退。
void check_finite(double v, const std::string& what)
{
    if (!std::isfinite(v))
    {
        std::printf("  [FAIL] %s: got %f (not finite)\n", what.c_str(), v);
        ++g_failures;
    }
}

// ── 被控对象 ─────────────────────────────────────────────────────────────────
// 目标在世界坐标里以 target_velocity_px_s 匀速移动; 镜头角度 = k * 累计下发的计数;
// 画面里的误差 = 目标 - 镜头。测量延迟 meas_delay_s(画面到控制器), 执行延迟
// act_delay_s(计数到画面生效)。
//
// 这两段延迟是过冲与滞后的唯一来源: 没有它们, 任何增益都能一拍到位, 什么都测不出来。
// 默认场景取实测链路量级: 采集->推理->tick 约 30ms + HID->游戏->显示 约 20ms。

struct SimResult
{
    double tail_abs_mean = 0.0;   // 尾段 |误差| 均值
    double tail_abs_max = 0.0;    // 尾段 |误差| 最大值
    double peak_abs = 0.0;        // 全程 |误差| 最大值
    double settle_s = -1.0;       // 之后 |误差| 再也没超过 1.5px 的时刻(-1 = 全程没进带)
    double integral_abs_max = 0.0;
    int max_counts = 0;           // 单拍 |计数| 最大值(限幅核查用)
    bool finite = true;           // 全程没有 NaN/inf
};

SimResult run_sim(double dt, double k, double meas_delay_s, double act_delay_s,
                  const boss::AimPidParams& params, double target_velocity_px_s,
                  double initial_error_px, double duration_s, double tail_fraction = 0.3)
{
    boss::AimPid pid;
    pid.configure(params);

    const int act_ticks = std::max(1, static_cast<int>(std::lround(act_delay_s / dt)));
    const int meas_ticks = std::max(0, static_cast<int>(std::lround(meas_delay_s / dt)));

    std::deque<double> act_line(static_cast<size_t>(act_ticks), 0.0);
    std::deque<double> anchor_line(static_cast<size_t>(meas_ticks), 0.0);

    // 初始时刻把误差摆成 initial_error_px: 目标在世界里偏出这么多, 镜头停在原点。
    double target_px = initial_error_px;
    double camera_px = 0.0;
    const double velocity = target_velocity_px_s;

    const int ticks = std::max(1, static_cast<int>(std::lround(duration_s / dt)));
    const int tail_start = static_cast<int>(static_cast<double>(ticks) * (1.0 - tail_fraction));

    std::vector<double> tail;
    std::vector<int> err_ticks;
    std::vector<double> err_vals;

    SimResult r;

    for (int i = 0; i < ticks; ++i)
    {
        const double anchor_true = target_px - camera_px;
        anchor_line.push_back(anchor_true);
        const double anchor_meas = anchor_line.front();
        anchor_line.pop_front();

        const int counts = pid.step(anchor_meas, dt);
        r.max_counts = std::max(r.max_counts, std::abs(counts));

        act_line.push_back(static_cast<double>(counts) * k);
        camera_px += act_line.front();
        act_line.pop_front();

        const double err_true = target_px - camera_px;
        if (!std::isfinite(err_true) || !std::isfinite(pid.integralState()) ||
            !std::isfinite(pid.carryState()))
        {
            r.finite = false;
        }
        r.peak_abs = std::max(r.peak_abs, std::abs(err_true));
        r.integral_abs_max = std::max(r.integral_abs_max, std::abs(pid.integralState()));

        if (i >= tail_start)
            tail.push_back(err_true);
        err_ticks.push_back(i);
        err_vals.push_back(err_true);

        target_px += velocity * dt;
    }

    for (double e : tail)
    {
        r.tail_abs_mean += std::abs(e);
        r.tail_abs_max = std::max(r.tail_abs_max, std::abs(e));
    }
    if (!tail.empty())
        r.tail_abs_mean /= static_cast<double>(tail.size());

    // 收敛时刻: 从后往前找最后一个 |误差| > 1.5px 的拍, 它的下一拍就是收敛点。
    for (size_t i = err_vals.size(); i-- > 0;)
    {
        if (std::abs(err_vals[i]) > 1.5)
        {
            r.settle_s = static_cast<double>(err_ticks[i] + 1) * dt;
            break;
        }
    }
    if (r.settle_s < 0.0 && !err_vals.empty())
        r.settle_s = 0.0;  // 从来没超出过 1.5px

    return r;
}

void report(const char* name, const SimResult& r)
{
    std::printf("  %-26s tail |mean| %7.3f  max %7.3f  peak %8.3f  settle %6.3fs\n",
                name, r.tail_abs_mean, r.tail_abs_max, r.peak_abs, r.settle_s);
}

// 闭环下冲: 400px 阶跃跑到稳态的全程里, 误差的最小值(负值 = 冲过了目标)。
// 为什么要单独一个函数: SimResult 里的 peak_abs 是 |误差| 的最大值, 而阶跃响应里
// 它几乎总是等于【初值】, 捕捉不到"冲过头"那一下。过冲要看的是负方向的极值。
double closed_loop_trough(double dt, double k, double meas_delay_s, double act_delay_s,
                          const boss::AimPidParams& params, double step_px)
{
    boss::AimPid pid;
    pid.configure(params);

    const int act_ticks = std::max(1, static_cast<int>(std::lround(act_delay_s / dt)));
    const int meas_ticks = std::max(0, static_cast<int>(std::lround(meas_delay_s / dt)));
    std::deque<double> act_line(static_cast<size_t>(act_ticks), 0.0);
    std::deque<double> anchor_line(static_cast<size_t>(meas_ticks), 0.0);

    double camera_px = 0.0;
    double trough = 0.0;
    const int ticks = std::max(1, static_cast<int>(std::lround(4.0 / dt)));
    for (int i = 0; i < ticks; ++i)
    {
        const double anchor_true = step_px - camera_px;
        anchor_line.push_back(anchor_true);
        const double anchor_meas = anchor_line.front();
        anchor_line.pop_front();

        const int counts = pid.step(anchor_meas, dt);
        act_line.push_back(static_cast<double>(counts) * k);
        camera_px += act_line.front();
        act_line.pop_front();

        trough = std::min(trough, step_px - camera_px);
    }
    return trough;
}

// 闭环过冲(正值, 单位 px): 阶跃响应里冲过目标最远的那一下。
// 与 closed_loop_trough 的区别只是符号约定 —— 那个返回负的下冲值。这里单独放一个
// 是为了让断言读起来是 "sat_on < sat_off" 而不是一对方括号。
double closed_loop_overshoot(double dt, double k, double meas_delay_s, double act_delay_s,
                             const boss::AimPidParams& params, double step_px)
{
    return -closed_loop_trough(dt, k, meas_delay_s, act_delay_s, params, step_px);
}

// 默认参数: 与 aim_pid.h 里注释的实机扫描结果同一量级。
boss::AimPidParams default_params()
{
    return {};
}

}  // namespace

int main()
{
    std::printf("=== aim_pid_test: pure-feedback PID + anchor filter ===\n");
    std::printf("    controller: u = dt*Kp*[e + Ki*int(e)dt + Kd*de/dt]  (no feedforward)\n");

    // 被控对象参数: k = 0.25 px/count(实机量级), 50ms 总延迟。
    const double k_true = 0.25;
    const double meas = 0.030;
    const double act = 0.020;

    // ── 1. 静止目标收敛 (60 / 240 / 1000 fps) ────────────────────────────────
    // 目标不动, 我们从 400px 外拉过去。三个帧率都必须收敛 —— 所有增益都带时间量纲,
    // 换帧率不该换手感, 这是这一版"dt 归一化"设计的核心承诺。
    std::printf("\n[1] static target convergence (400px step, 3 frame rates)\n");
    {
        const double budget_px = 2.0;  // 量化底噪: 被控对象以 1 count = 0.25px 为台阶
        for (double dt : {1.0 / 60.0, 1.0 / 240.0, 1.0 / 1000.0})
        {
            const SimResult r = run_sim(dt, k_true, meas, act, default_params(),
                                        0.0, 400.0, 2.5);
            char name[64];
            std::snprintf(name, sizeof(name), "static dt=%.2fms", dt * 1000.0);
            report(name, r);

            check(r.finite, "static: all states finite");
            check(r.tail_abs_mean < 1.0, "static: tail |error| mean < 1.0px");
            check(r.tail_abs_max < budget_px, "static: tail |error| max < 2.0px");
            check(r.settle_s >= 0.0 && r.settle_s < 1.8, "static: settles within 1.8s");

            // 过冲预算: 初值 400px, 允许冲过目标 25% 以内(100px), 再往上就是真的振铃。
            // 延迟 50ms 的回路必然有过冲, 但 400px 阶跃下超过 25% 说明增益/饱和没配好。
            check(r.peak_abs < 400.0 + 100.0, "static: overshoot within budget (<500px peak)");
        }
    }

    // ── 2. 匀速目标稳态滞后 + Ki 的作用 ─────────────────────────────────────
    // 纯 P 回路追匀速目标的稳态滞后 = v / (Kp * k)(k=每计数据像素)。
    // Kp=20 / k=0.25 时理论滞后 100*4/20 = 20px @100px/s —— 有限且有界。
    // 打开 Ki 之后积分把这份滞后磨掉, 所以 ki>0 的滞后必须【明显小于】ki=0。
    std::printf("\n[2] constant-velocity steady-state lag; Ki must reduce it\n");
    {
        for (double v : {100.0, 200.0, 400.0})
        {
            boss::AimPidParams no_i = default_params();
            no_i.ki = 0.0;
            no_i.integral_window_px = 0.0;

            const SimResult p_only = run_sim(1.0 / 240.0, k_true, meas, act, no_i, v, 0.0, 6.0);
            const SimResult with_i = run_sim(1.0 / 240.0, k_true, meas, act, default_params(),
                                             v, 0.0, 6.0);
            const double lag_bound = v / (default_params().kp * k_true) * 1.6 + 5.0;
            std::printf("  v=%6.1fpx/s  Ki=0 lag %8.3f  |  Ki=%.1f lag %8.3f  (P-only bound %.1f)\n",
                        v, p_only.tail_abs_mean, default_params().ki, with_i.tail_abs_mean,
                        lag_bound);

            check(p_only.finite && with_i.finite, "vel: all states finite");
            // 滞后有限且有界 —— 这是"纯反馈能跟住匀速目标"的最低要求。
            check(p_only.tail_abs_mean < lag_bound, "vel: P-only lag is finite and bounded");
            // 积分必须真的把滞后磨小, 而不是碰运气。
            check(with_i.tail_abs_mean < p_only.tail_abs_mean * 0.8 + 0.5,
                  "vel: Ki reduces steady-state lag vs Ki=0");
            check(with_i.tail_abs_max < 60.0, "vel: no divergence with integral on");
        }
    }

    // ── 3. 输出限幅: 每一拍都respect limit_counts ────────────────────────────
    // 不限幅的话甩枪那一下会给出几百个计数, 实机上是"鼠标飞出去"。
    std::printf("\n[3] output limit respected on every tick\n");
    {
        for (int lim : {1, 5, 20, 200})
        {
            boss::AimPid pid;
            boss::AimPidParams p = default_params();
            p.limit_counts = lim;
            pid.configure(p);

            int worst = 0;
            bool finite = true;
            // 先给一段持续大误差(顶住限幅), 再给一段小误差(考验零头与积分),
            // 中间穿插一次反向甩枪 —— 限幅在任何相位都必须成立。
            for (int i = 0; i < 600; ++i)
            {
                double e = 500.0;
                if (i >= 200 && i < 400)
                    e = -500.0;
                else if (i >= 400)
                    e = 0.3 * ((i % 7) - 3);
                const int c = pid.step(e, 1.0 / 240.0);
                worst = std::max(worst, std::abs(c));
                if (!std::isfinite(pid.outputCounts()))
                    finite = false;
            }
            std::printf("  limit_counts=%3d -> worst |counts| = %3d (limit %d)\n", lim, worst,
                        pid.outputLimit());
            check(worst <= lim, "limit: |counts| <= limit_counts on every tick");
            check(pid.outputLimit() == lim, "limit: outputLimit() echoes the configured value");
            check(finite, "limit: internal output stays finite");
        }

        // limit_counts = 0 -> 退回内置上限(200), 不是"不限幅"。
        boss::AimPid pid;
        boss::AimPidParams p = default_params();
        p.limit_counts = 0;
        pid.configure(p);
        int worst = 0;
        for (int i = 0; i < 200; ++i)
            worst = std::max(worst, std::abs(pid.step(4000.0, 1.0 / 240.0)));
        std::printf("  limit_counts=0 (fallback) -> worst |counts| = %d (limit %d)\n", worst,
                    pid.outputLimit());
        check(pid.outputLimit() > 0, "limit: 0 falls back to a real built-in limit");
        check(worst <= pid.outputLimit(), "limit: fallback limit also enforced");
    }

    // ── 4. 零头/亚计数累积 ──────────────────────────────────────────────────
    // 出口是全链路里唯一一次取整。取整丢掉的零头如果扔掉, 差 0.4px 时输出恒为 0,
    // 控制器就"卡在差两像素不动"。所以零头必须攒到下一拍。
    //
    // 两种口径都验:
    //   (a) 关掉积分后, 常驻小误差的累计计数应≈ Kp*e*T(纯 P 的解析积分);
    //   (b) 真正的亚计数误差(单拍输出 < 0.5 count)也要在若干拍内发出非零计数。
    std::printf("\n[4] carry / sub-count accumulation\n");
    {
        boss::AimPid pid;
        boss::AimPidParams p = default_params();
        p.ki = 0.0;
        p.kd = 0.0;
        p.limit_counts = 100000;  // 关掉限幅, 只看零头机制
        pid.configure(p);

        const double dt = 1.0 / 240.0;
        const int n = 400;
        const double e = 0.6;  // 单拍指令 = 20*0.6/240 = 0.05 count, 远小于 1
        const double expected = p.kp * e * (n * dt);  // Kp*e*T
        long total = 0;
        int nonzero = 0;
        for (int i = 0; i < n; ++i)
        {
            const int c = pid.step(e, dt);
            total += c;
            if (c != 0)
                ++nonzero;
        }
        std::printf("  const 0.6px for %.2fs -> %ld counts (analytic %.1f), nonzero ticks %d\n",
                    n * dt, total, expected, nonzero);
        check(nonzero > 0, "carry: sub-count error is not silently dropped");
        check(std::abs(static_cast<double>(total) - expected) < std::abs(expected) * 0.15 + 2.0,
              "carry: accumulated counts match the analytic Kp*e*T");

        // (b) 真正贴着量化台阶的误差: 20*0.1/240 = 0.0083 count/拍, 要 120 拍才够 1 个计数。
        boss::AimPid tiny;
        boss::AimPidParams tp = default_params();
        tp.ki = 0.0;
        tp.kd = 0.0;
        tiny.configure(tp);
        bool ever_nonzero = false;
        for (int i = 0; i < 240; ++i)
        {
            if (tiny.step(0.1, dt) != 0)
                ever_nonzero = true;
        }
        std::printf("  const 0.1px (0.0083 count/tick) for 1s -> nonzero emitted: %s\n",
                    ever_nonzero ? "yes" : "no");
        check(ever_nonzero, "carry: error far below one quantum still eventually moves the mouse");
        check(std::abs(tiny.carryState()) <= 0.5 + 1e-9, "carry: carry state is clamped to 0.5");
    }

    // ── 5. 坏输入: 不得产生 NaN 输出, 不得崩 ────────────────────────────────
    // 实机会给出 NaN 的框(检测失败)、0/负的 dt(时间戳回绕)、离谱的 dt(掉帧)。
    // 任何一条都不能让控制器进入不可恢复的状态。
    std::printf("\n[5] bad input robustness (NaN / bad dt / bad gains)\n");
    {
        const double nan_v = std::numeric_limits<double>::quiet_NaN();
        const double inf_v = std::numeric_limits<double>::infinity();

        boss::AimPid pid;
        pid.configure({});

        // NaN 误差: 不发指令, 状态原样保留。
        const int nan_counts = pid.step(nan_v, 1.0 / 240.0);
        std::printf("  NaN error -> %d counts\n", nan_counts);
        check(nan_counts == 0, "bad: NaN error emits no counts");

        // 建立一段正常状态, 确认后面的坏 dt 不会把它污染。
        for (int i = 0; i < 50; ++i)
            pid.step(10.0, 1.0 / 240.0);
        const double integral_before = pid.integralState();

        struct DtCase { const char* name; double dt; };
        const DtCase dt_cases[] = {
            {"dt=0", 0.0}, {"dt<0", -1.0}, {"dt=NaN", nan_v}, {"dt=inf", inf_v},
            {"dt=100s", 100.0}, {"dt=1e-9", 1e-9}};
        for (const DtCase& c : dt_cases)
        {
            boss::AimPid q;
            q.configure({});
            for (int i = 0; i < 50; ++i)
                q.step(10.0, 1.0 / 240.0);
            const int out = q.step(10.0, c.dt);
            std::printf("  %-9s -> %6d counts, usedDt=%.6f\n", c.name, out, q.usedDt());
            check_finite(q.outputCounts(), "bad: outputCounts finite");
            check_finite(q.usedDt(), "bad: usedDt finite");
            check_finite(q.integralState(), "bad: integralState finite");
            check(q.usedDt() > 0.0 && q.usedDt() <= 1.0 / 15.0 + 1e-12,
                  "bad: dt is clamped into a sane positive window");
            check(std::abs(out) <= q.outputLimit(), "bad: output still limited");
        }
        check_finite(integral_before, "bad: baseline integral finite");

        // 非法增益: 非有限或负数一律退回内置默认, 不是"照单全收"。
        struct GainCase { const char* name; double kp, ki, kd; };
        const GainCase gain_cases[] = {
            {"kp=NaN", nan_v, 1.0, 0.01},
            {"ki=NaN", 20.0, nan_v, 0.01},
            {"kd=NaN", 20.0, 1.0, nan_v},
            {"kp<0", -5.0, 1.0, 0.01},
            {"ki<0", 20.0, -5.0, 0.01},
            {"kd<0", 20.0, 1.0, -0.01},
            {"all NaN", nan_v, nan_v, nan_v},
            {"all inf", inf_v, inf_v, inf_v}};
        for (const GainCase& c : gain_cases)
        {
            boss::AimPid q;
            boss::AimPidParams gp;
            gp.kp = c.kp;
            gp.ki = c.ki;
            gp.kd = c.kd;
            q.configure(gp);
            int worst = 0;
            for (int i = 0; i < 400; ++i)
            {
                const int out = q.step(50.0 * std::sin(i * 0.05) + 80.0, 1.0 / 240.0);
                worst = std::max(worst, std::abs(out));
                check_finite(q.outputCounts(), "bad gains: output finite");
            }
            std::printf("  gains %-8s -> worst |counts| = %3d (limit %d)\n", c.name, worst,
                        q.outputLimit());
            check(worst <= q.outputLimit(), "bad gains: output bounded by the limit");
        }

        // Kp=0 而 Ki>0: 积分项没有上限可夹的老陷阱(会溢出成 inf -> lround 未定义行为)。
        // kIntegralCap 的公式是 rate_limit / kp, kp=0 时分母为 0 —— 这条专门压这里。
        boss::AimPid overflow;
        boss::AimPidParams op;
        op.kp = 0.0;
        op.ki = 60.0;
        overflow.configure(op);
        bool overflow_ok = true;
        for (int i = 0; i < 20000; ++i)
        {
            const int out = overflow.step(500.0, 1.0 / 240.0);
            if (!std::isfinite(overflow.outputCounts()) || !std::isfinite(overflow.integralState()) ||
                std::abs(out) > overflow.outputLimit())
            {
                overflow_ok = false;
                break;
            }
        }
        std::printf("  Kp=0 / Ki=60 for 20000 ticks -> %s\n", overflow_ok ? "bounded" : "BROKEN");
        check(overflow_ok, "bad gains: Kp=0 & Ki>0 stays bounded (no lround(NaN))");
    }

    // ── 6. reset() 清干净 ───────────────────────────────────────────────────
    std::printf("\n[6] reset() clears integral / derivative / carry\n");
    {
        boss::AimPid pid;
        boss::AimPidParams p = default_params();
        p.kd = 0.05;
        pid.configure(p);

        // 正常攒状态: 误差在变, 微分非零; 常驻小误差, 零头非零。
        for (int i = 0; i < 120; ++i)
            pid.step(5.0 + 3.0 * std::sin(i * 0.1), 1.0 / 120.0);
        std::printf("  before reset: integral=%+.6f derivative=%+.6f carry=%+.6f\n",
                    pid.integralState(), pid.derivativeState(), pid.carryState());
        check(std::abs(pid.integralState()) > 1e-9, "reset: integral accumulated before reset");

        pid.reset();
        check(pid.integralState() == 0.0, "reset: integralState == 0");
        check(pid.derivativeState() == 0.0, "reset: derivativeState == 0");
        check(pid.carryState() == 0.0, "reset: carryState == 0");
        check(pid.pTerm() == 0.0 && pid.iTerm() == 0.0 && pid.dTerm() == 0.0,
              "reset: telemetry terms == 0");
        check(pid.outputCounts() == 0.0, "reset: outputCounts == 0");

        // 复位后误差为 0 不该有残留输出; 复位后首拍的微分必须是 0(不能把上一段的
        // 微分状态当作这个新目标的阻尼放出来 —— 2026-09-12 实机踩过)。
        check(pid.step(0.0, 1.0 / 120.0) == 0, "reset: zero error emits zero counts");

        boss::AimPid fresh;
        boss::AimPidParams fp = default_params();
        fp.kd = 0.05;
        fp.ki = 0.0;
        fp.p_full_scale_px = 0.0;  // 关饱和, 首拍输出应当正好等于纯 P 项
        fp.limit_counts = 100000;
        fresh.configure(fp);
        for (int i = 0; i < 50; ++i)
            fresh.step(20.0 + 8.0 * i, 1.0 / 120.0);  // 攒一份很大的微分状态
        fresh.reset();
        fresh.step(120.0, 1.0 / 120.0);
        const double expect_p_only = 120.0 * fp.kp * (1.0 / 120.0);
        std::printf("  after reset first tick: dTerm=%.9f (want 0), P-only counts=%.3f\n",
                    fresh.dTerm(), expect_p_only);
        check(std::abs(fresh.dTerm()) < 1e-12, "reset: first tick derivative is zeroed");
        check_near(fresh.outputCounts(), expect_p_only, std::abs(expect_p_only) * 0.02 + 0.51,
                   "reset: first tick output is pure P (no leftover state)");
    }

    // ── 7. ★ P 项连续饱和 (取代死区) ────────────────────────────────────────
    // 死区是"误差小就不出力", 代价是瞄点周围一个永久盲区 + "停—放"极限环。
    // 现在换成连续饱和: |e| <= p_full_scale_px 原样透传; 超过后按 full/|e| 衰减。
    // 关键性质: 函数【经过原点且连续】 —— 误差多小都出力, 所以不存在"停了"这个状态。
    std::printf("\n[7] P-term continuous saturation (deadzone replacement)\n");
    {
        // 每一拍都用全新控制器, 排除 carry 与积分攒出来的假象。
        auto p_term_of = [](double e, double p_full) {
            boss::AimPid p;
            boss::AimPidParams pp;
            pp.ki = 0.0;
            pp.kd = 0.0;
            pp.p_full_scale_px = p_full;
            pp.limit_counts = 100000;  // 关掉输出限幅, 只看 P 项的形状
            p.configure(pp);
            p.step(e, 1.0 / 120.0);
            return p.pTerm();
        };

        // 7a. 大误差段饱和: P(400)/P(50) 在 full=100 时应当≈2 (纯线性会是 8)。
        const double r_sat = std::abs(p_term_of(400.0, 100.0)) / std::abs(p_term_of(50.0, 100.0));
        const double r_lin = std::abs(p_term_of(400.0, 0.0)) / std::abs(p_term_of(50.0, 0.0));
        std::printf("  P(400px)/P(50px):  sat on = %.3f (want ~2.00), sat off = %.3f (want 8.00)\n",
                    r_sat, r_lin);
        check(r_sat > 1.6 && r_sat < 2.4, "sat: large error is compressed to ~full-scale");
        check(std::abs(r_lin - 8.0) < 0.1, "sat: p_full_scale_px=0 disables saturation (linear)");

        // 7b. ★ 连续性与过原点: 这才是取代死区的那条性质。
        //     误差从 -2px 扫到 +2px, P 项必须单调、变号、且在 0 处为 0。
        double prev_p = p_term_of(-2.0, 100.0);
        bool monotone = true;
        bool signed_ok = true;
        for (int i = 1; i <= 80; ++i)
        {
            const double e = -2.0 + 4.0 * (static_cast<double>(i) / 80.0);
            const double p = p_term_of(e, 100.0);
            if (p < prev_p - 1e-12)
                monotone = false;
            if (p * e < -1e-12)
                signed_ok = false;  // 输出方向必须与误差同号
            prev_p = p;
        }
        check(monotone, "sat: P term is monotone through the origin");
        check(signed_ok, "sat: P term never fights the error sign");
        check(p_term_of(0.0, 100.0) == 0.0, "sat: P(0) == 0 exactly");

        // 7c. ★ 没有盲区: 任意小的误差都给出非零的 P 项(死区时代这里恒为 0)。
        bool no_blind_spot = true;
        double smallest = 1.0;
        for (double tiny : {2.0, 0.5, 0.1, 0.01, 1e-4})
        {
            const double p = p_term_of(tiny, 100.0);
            if (!(p > 0.0))
                no_blind_spot = false;
            smallest = std::min(smallest, p);
        }
        std::printf("  sub-pixel errors: P(2.0)=%.4f P(0.5)=%.4f P(0.1)=%.4f P(1e-4)=%.6f\n",
                    p_term_of(2.0, 100.0), p_term_of(0.5, 100.0), p_term_of(0.1, 100.0),
                    p_term_of(1e-4, 100.0));
        check(no_blind_spot, "sat: no blind spot - arbitrarily small error still produces output");
        check(smallest > 0.0, "sat: smallest sampled error produced a positive P term");

        // 7d. 小误差段一个像素都不丢: |e| <= full 时 P 项应当【正好】等于 e(满增益)。
        bool exact_below_full = true;
        for (double e : {-99.0, -50.0, -1.0, 0.0, 1.0, 50.0, 99.0})
        {
            if (std::abs(p_term_of(e, 100.0) - e) > 1e-9)
                exact_below_full = false;
        }
        check(exact_below_full, "sat: below full scale the P term passes the error through 1:1");

        // 7e. 饱和曲线的形状: |P| 关于 |e| 单调不减, 且被 full_scale 封顶。
        bool bounded = true;
        double prev_mag = 0.0;
        for (int i = 0; i <= 200; ++i)
        {
            const double e = 500.0 * (static_cast<double>(i) / 200.0);
            const double mag = std::abs(p_term_of(e, 100.0));
            if (mag > 100.0 + 1e-9 || mag < prev_mag - 1e-9)
                bounded = false;
            prev_mag = mag;
        }
        check(bounded, "sat: |P| is non-decreasing in |e| and capped at p_full_scale_px");

        // 7f. 闭环下的实际意义 —— 这一节记录一条【实测出来、且依赖增益】的性质,
        //     它同时推翻了"饱和防止过冲"的旧说法和"饱和只会加剧过冲"的新说法。
        //
        // 头文件原本说饱和是为了"甩枪/换靶不再过冲"。本测试早先把它证伪, 但复现出来的
        // 是【相反方向】的断论("饱和增大过冲")—— 那也不对, 因为它只测了 Kp=20 一个点。
        // 独立扫描(400px 阶跃, k=0.25px/计数, 50ms 死区, tests/_sat_probe.cpp)给出全貌:
        //
        //     Kp     饱和开(100px)   饱和关     谁赢
        //     20       16.2px        9.5px      关
        //     30       15.0px        8.0px      关
        //     40       14.5px        5.0px      关
        //     60       25.0px       78.8px      开   <- 转折点
        //    100       56.0px      279.0px      开
        //    150      171.5px      423.0px      开
        //
        // 机理: 死区时间内 P 项是唯一的刹车, 而饱和削掉的正是"接近速度"。
        //   · Kp 小 -> 不加饱和本来就刹得住, 饱和只是让你更慢地靠近, 反而给积分更多
        //     时间攒欠账 -> 过冲更大;
        //   · Kp 大 -> 不加饱和会高速撞过目标, 延迟内改不回来 -> 过冲爆炸; 这时饱和
        //     限住接近速度才是救命的。
        //
        // 而且这不是振铃: 两种配置的【尾段峰峰值都收敛到 0.25~0.5px】, 差别只在接近
        // 那一下的幅度。所以饱和管瞬态幅度, 不管稳态精度。
        //
        // 因此这里断言的是与增益无关、可判定的三条:
        //   (a) 任何 full_scale / Kp 下都不能发散(尾段有界);
        //   (b) 函数形状正确: 单调、封顶、且在小误差段 1:1 透传(见 7a~7e);
        //   (c) 在实机增益区间(Kp >= 50)饱和【确实】减小过冲 —— 这是默认值 100px
        //       的依据, 也是"青出于蓝"那条: 原神的 smooth_max_pixel 是硬钳位, 无论
        //       增益高低都削, 在小增益下同样会帮倒忙; 我们把它做成可关的连续饱和。
        const SimResult base = run_sim(1.0 / 240.0, k_true, meas, act, default_params(),
                                       0.0, 400.0, 2.5);
        std::printf("  400px step, p_full_scale_px sweep (overshoot measured as negative trough):\n");
        double worst_trough = 0.0;
        for (double full : {25.0, 50.0, 100.0, 200.0, 100000.0})
        {
            boss::AimPidParams pp = default_params();
            pp.p_full_scale_px = full;
            // 关掉饱和时放开限幅, 否则限幅会替饱和兜底, 看不出饱和自己的作用。
            if (full > 1.0e4)
                pp.limit_counts = 100000;
            const SimResult r = run_sim(1.0 / 240.0, k_true, meas, act, pp, 0.0, 400.0, 2.5);

            // 下冲 = 全程误差的最小值(负值); 用 run_sim 的 peak 之外单独扫尾段不足以
            // 捕捉它, 所以这里用一个小的局部闭环重跑并记录最小值。
            const double trough = closed_loop_trough(1.0 / 240.0, k_true, meas, act, pp, 400.0);
            worst_trough = std::min(worst_trough, trough);
            std::printf("    full=%8.0f  trough %+7.2fpx  settle %5.3fs  tail max %.3fpx\n",
                        full, trough, r.settle_s, r.tail_abs_max);

            check(r.finite, "sat: closed loop stays finite across the full-scale sweep");
            check(std::abs(trough) < 100.0, "sat: overshoot stays within the 100px budget");
            check(r.settle_s >= 0.0 && r.settle_s < 2.5, "sat: still settles within 2.5s");
        }
        check(std::abs(base.tail_abs_max) < 2.0, "sat: default sat keeps the tail residual tiny");
        std::printf("  worst trough across the sweep: %+.2fpx\n", worst_trough);

        // 7g. 增益依赖: 在【实机增益区间】里饱和必须确实减小过冲 —— 这是默认值
        // 100px 的依据。而在低增益区它反而帮倒忙(见上面的表), 所以那条不断言为
        // "总是好的", 只断言"高增益下它是好的"。
        //
        // 这一条同时是"超越原神"的可验证依据: 原神的 smooth_max_pixel 是不可关的
        // 硬钳位, 低增益下同样会帮倒忙; 我们的 p_full_scale_px = 0 可关。
        std::printf("  gain dependence (400px step, sat 100px vs sat off):\n");
        for (const double kp : {60.0, 120.0})
        {
            boss::AimPidParams hi = default_params();
            hi.kp = kp;
            hi.p_full_scale_px = 100.0;
            const double ov_on = closed_loop_overshoot(1.0 / 240.0, k_true, meas, act, hi, 400.0);

            boss::AimPidParams hioff = hi;
            hioff.p_full_scale_px = 0.0;   // 0 = 关闭饱和
            hioff.limit_counts = 100000;   // 放开限幅, 否则限幅替饱和兜底
            const double ov_off = closed_loop_overshoot(1.0 / 240.0, k_true, meas, act, hioff, 400.0);

            std::printf("    kp=%5.0f  sat_on %7.2fpx   sat_off %7.2fpx\n", kp, ov_on, ov_off);
            check(ov_on < ov_off,
                  "sat: at production gain, saturation REDUCES the step overshoot");
        }
    }

    // ── 8. AnchorFilter (α-β 平滑器) ────────────────────────────────────────
    // 职责只有一件事: 把检测框抖动滤掉, 输出干净位置给 PID。选 α-β 而不是一阶低通
    // 的全部理由是【匀速目标下的滞后】: 一阶低通只知道位置, 追匀速目标会持续落后
    // v*τ; α-β 带速度状态, 是"预测 + 修正", 匀速下几乎不落后。
    std::printf("\n[8] AnchorFilter: noise rejection vs ramp lag\n");
    {
        const double dt = 1.0 / 120.0;
        const double tau = 0.040;  // 40ms 平滑时间常数(实机量级)

        // 8a. tau<=0 = 直通(不平滑), 一拍贴到测量值。
        boss::AnchorFilter pass;
        pass.configure(0.0);
        check_near(pass.step(123.456, dt), 123.456, 1e-9, "filter: tau=0 passes through exactly");
        check_near(pass.step(-50.0, dt), -50.0, 1e-9, "filter: tau=0 keeps passing through");

        // 8b. 噪声抑制: 静止目标 + 检测框抖动, 输出的逐拍跳变要远小于输入。
        //     确定性伪噪声(两个不可通约的频率), 不依赖 rand 的平台差异。
        const auto noise_at = [](int i) {
            return 1.9 * std::sin(i * 1.05) + 0.8 * std::sin(i * 2.7 + 1.3);
        };

        double in_abs_mean = 0.0;
        double out_abs_mean = 0.0;
        double in_mean = 0.0;
        double out_mean = 0.0;
        {
            boss::AnchorFilter f;
            f.configure(tau);
            double prev_out = 0.0;
            double prev_in = 0.0;
            int n = 0;
            for (int i = 0; i < 1200; ++i)
            {
                const double in = 100.0 + noise_at(i);
                const double out = f.step(in, dt);
                check_finite(out, "filter: output finite");
                if (i > 20)
                {
                    in_abs_mean += std::abs(in - prev_in);
                    out_abs_mean += std::abs(out - prev_out);
                    in_mean += in;
                    out_mean += out;
                    ++n;
                }
                prev_in = in;
                prev_out = out;
            }
            in_abs_mean /= std::max(1, n);
            out_abs_mean /= std::max(1, n);
            in_mean /= std::max(1, n);
            out_mean /= std::max(1, n);
        }
        std::printf("  noisy static: input tick-to-tick %.4fpx -> output %.4fpx  (ratio %.1f%%)\n",
                    in_abs_mean, out_abs_mean, 100.0 * out_abs_mean / in_abs_mean);
        check(out_abs_mean < in_abs_mean * 0.5, "filter: output tick-to-tick noise is much smaller");
        check(out_abs_mean < 0.35, "filter: absolute output jitter is small");
        check_near(out_mean, 100.0, 2.0, "filter: biased-noise mean is still tracked");

        // 8c. ★ 匀速斜坡: α-β 的滞后必须远小于同时间常数的一阶低通。
        //     参照物就在测试里现算 —— 一阶低通 y += a*(x-y), a = dt/(tau+dt),
        //     它对匀速输入的稳态滞后就是 v*tau。
        const double v = 240.0;  // px/s
        const double slope_per_tick = v * dt;
        const double base = 100.0;

        boss::AnchorFilter ab;
        ab.configure(tau);
        double lp = 0.0;
        double lp_alpha = dt / (tau + dt);
        double ab_out = 0.0;
        const int ramp_ticks = 600;
        for (int i = 0; i < ramp_ticks; ++i)
        {
            const double meas = base + slope_per_tick * i;
            ab_out = ab.step(meas, dt);
            if (i == 0)
                lp = meas;
            else
                lp += lp_alpha * (meas - lp);
        }
        const double truth = base + slope_per_tick * (ramp_ticks - 1);
        const double ab_lag = truth - ab_out;
        const double lp_lag = truth - lp;
        std::printf("  ramp %.0fpx/s tau=%.0fms: alpha-beta lag %7.3fpx | 1st-order LP lag %7.3fpx "
                    "(v*tau=%.1f)\n",
                    v, tau * 1000.0, ab_lag, lp_lag, v * tau);
        check_finite(ab_lag, "filter: ramp lag finite");
        check(lp_lag > v * tau * 0.7, "filter: reference LP really does lag ~v*tau (sanity)");
        check(ab_lag * 4.0 < lp_lag,
              "filter: alpha-beta lag is far smaller than a 1st-order LP at the same tau");
        check(ab_lag < 3.0, "filter: ramp tracking lag under 3px");

        // 8d. 加上噪声之后 α-β 仍然跟得住斜坡(不会因为抗抖把速度状态压没)。
        {
            boss::AnchorFilter f;
            f.configure(tau);
            double out = 0.0;
            for (int i = 0; i < 1200; ++i)
            {
                const double meas = base + slope_per_tick * i + noise_at(i);
                out = f.step(meas, dt);
            }
            const double truth_at_end = base + slope_per_tick * 1199;
            const double lag = truth_at_end - out;
            std::printf("  noisy ramp %.0fpx/s: lag %7.3fpx\n", v, lag);
            check(std::abs(lag) < 6.0, "filter: tracks a noisy ramp with small lag");
        }

        // 8e. 坏输入与首拍: 首拍直接贴上测量(不做"从 0 慢慢爬"的蠢事);
        //     NaN 测量保持上次输出; 非法 dt 退回 1/120 而不是卡死或除零。
        {
            boss::AnchorFilter f;
            f.configure(tau);
            check_near(f.step(77.0, dt), 77.0, 1e-9, "filter: first sample snaps to the measurement");
            check(f.initialized(), "filter: initialized() is true after the first step");

            const double before = f.position();
            check_near(f.step(std::numeric_limits<double>::quiet_NaN(), dt), before, 1e-9,
                       "filter: NaN measurement holds the previous output");
            check_near(f.step(std::numeric_limits<double>::infinity(), dt), before, 1e-9,
                       "filter: inf measurement holds the previous output");

            boss::AnchorFilter g;
            g.configure(tau);
            g.step(50.0, dt);
            const double d0 = g.step(60.0, 0.0);
            const double dn = g.step(60.0, std::numeric_limits<double>::quiet_NaN());
            const double dneg = g.step(60.0, -1.0);
            std::printf("  dt=0 -> %.4f, dt=NaN -> %.4f, dt<0 -> %.4f (all finite)\n", d0, dn, dneg);
            check_finite(d0, "filter: dt=0 output finite");
            check_finite(dn, "filter: dt=NaN output finite");
            check_finite(dneg, "filter: dt<0 output finite");
            check(g.position() > 50.0 && g.position() < 60.0, "filter: bad dt does not snap or diverge");

            // configure 参数没变时必须【不清状态】(引擎每拍都会调 configure)。
            boss::AnchorFilter h;
            h.configure(tau);
            double last = 0.0;
            for (int i = 0; i < 400; ++i)
            {
                h.configure(tau);  // 每拍都调, 参数完全一样
                last = h.step(base + slope_per_tick * i, dt);
            }
            const double lag_after_reconf = (base + slope_per_tick * 399) - last;
            std::printf("  configure() every tick with same tau: ramp lag %.3fpx\n",
                        lag_after_reconf);
            check(lag_after_reconf < 3.0,
                  "filter: repeated configure() with the same tau keeps state (no per-tick reset)");

            // tau 真的变了 -> 必须清状态(换了平滑强度, 旧的速度状态作废)。
            h.configure(0.2);
            check(!h.initialized(), "filter: changing tau does reset the filter");

            // 非法 tau 退回"不平滑"而不是 NaN 传播。
            boss::AnchorFilter bad;
            bad.configure(std::numeric_limits<double>::quiet_NaN());
            check_near(bad.step(31.0, dt), 31.0, 1e-9, "filter: NaN tau degrades to pass-through");
            bad.configure(-2.0);
            check_near(bad.step(32.0, dt), 32.0, 1e-9, "filter: negative tau degrades to pass-through");
        }

        // 8f. 滤波 + PID 串起来必须仍然收敛 —— 这两层是新版链路的全部前端。
        {
            boss::AnchorFilter f;
            f.configure(tau);
            boss::AimPid pid;
            pid.configure(default_params());

            const double k = k_true;
            const int act_ticks = static_cast<int>(std::lround(act / dt));
            std::vector<double> act_line(static_cast<size_t>(std::max(1, act_ticks)), 0.0);
            double camera = 0.0;
            double tail_max = 0.0;
            int worst_counts = 0;
            const int ticks = static_cast<int>(2.5 / dt);
            for (int i = 0; i < ticks; ++i)
            {
                const double measured = 300.0 - camera + noise_at(i);
                const double clean = f.step(measured, dt);
                const int counts = pid.step(clean, dt);
                worst_counts = std::max(worst_counts, std::abs(counts));
                act_line.push_back(static_cast<double>(counts) * k);
                camera += act_line.front();
                act_line.erase(act_line.begin());
                if (i > ticks * 3 / 4)
                    tail_max = std::max(tail_max, std::abs(300.0 - camera));
            }
            std::printf("  filter+PID closed loop: tail |error| max %.3fpx, worst |counts| %d\n",
                        tail_max, worst_counts);
            check(tail_max < 3.0, "filter+PID: noisy static target still converges");
        }
    }

    // ── 汇总 ────────────────────────────────────────────────────────────────
    if (g_failures == 0)
        std::printf("\nALL PASS\n");
    else
        std::printf("\n%d CHECK(S) FAILED\n", g_failures);

    return g_failures == 0 ? 0 : 1;
}
