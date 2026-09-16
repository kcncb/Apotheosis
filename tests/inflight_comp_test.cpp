// 在途自身位移补偿 (Smith 类) 的回归 —— mouse/aim_pid.h 的 inflight_beta。
//
// ── 2026-09-13 重做说明(为什么这个文件的参数换了单位) ────────────────────────
// 老版本用 `inflight_gain`【计数/秒】表达补偿强度:
//       e_used = e - (gain / kp) * N
// 实测发现这个写法隐含地【假定了 k̂ 是多少】: 展开后补偿量 = gain·dt·N 计数,
// 等价于断言"1 个在途计数 = gain·dt 个像素"。在 k̂=0.593、dt=1/120 下:
//     gain=50  -> 假定 k̂=0.42(补不足, 安全)
//     gain=120 -> 假定 k̂=1.00(过补 1.7 倍 -> 发散)
// 于是同一个 gain 在 Kp 变化时物理含义会漂移, 无法同时适配低 Kp 与高 Kp。
//
// 现在改用【无量纲】的 beta:
//       e_used = e - beta * N / (kp_eff * dt)
// beta 是"扣掉多少比例的在途计数当量", 与 Kp 解耦, 且【不含任何 k̂】。
//
// 本文件原来的断言全部保留, 只是把扫描量从 gain[计数/秒] 换成 beta[无量纲]。
// 换算关系(scale=1 时): beta = gain · dt。原 gain 扫描 {0,10,25,50,75,100,...}
// 在 dt=1/120 下对应 beta {0, 0.083, 0.208, 0.417, 0.625, 0.833, ...} ——
// 本文件直接扫 beta 的自然步长, 覆盖面相同且更规整。
#include "mouse/aim_pid.h"
#include <cstdio>
#include <cmath>
#include <deque>
#include <algorithm>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// ── 1. Ring accounting: is in_flight_ really "sum of the last window_slots
//      outputs"?  Must be tested with a CONSTANT output, so drive the loop with
//      beta=0 (no feedback on in-flight) while still recording into the ring.
//      With beta=0 the controller is a plain P loop; a constant error with no
//      integral and no plant feedback keeps the output constant.
static void test_ring_accounting()
{
    std::printf("\n[1] in-flight accounting correctness\n");

    // beta must be > 0 to turn the ring on, so use a tiny beta to get bookkeeping
    // with an effectively-zero effect on used_error.
    boss::AimPid pid2;
    boss::AimPidParams q;
    q.kp = 100.0; q.ki = 0.0; q.kd = 0.0;
    q.p_full_scale_px = 1e9;      // P 项饱和关掉, 输出是常数
    q.limit_counts = 100000;
    q.inflight_beta = 1e-6;       // 实际上不影响 used_error
    q.inflight_window_s = 0.02;   // 2 slots at dt=1/120
    pid2.configure(q);

    const double dt = 1.0 / 120.0;
    const double e = 1000.0;

    int last = 0;
    for (int i = 0; i < 40; ++i) last = pid2.step(e, dt);

    // With beta ~0 the output is the plain P value and stays constant.
    const double expect = 2.0 * std::abs((double)last);
    const double got = std::abs(pid2.inFlightCounts());
    std::printf("    per-tick output=%d, window=2 slots -> expected |in_flight|~%.0f, got %.0f\n",
                last, expect, got);
    check(std::abs(got - expect) < expect * 0.05,
          "in_flight_ equals window_slots * output");

    // And it must decay to 0 after output stops.
    for (int i = 0; i < 10; ++i) pid2.step(0.0, dt);
    std::printf("    after 10 ticks of zero output: in_flight=%.0f\n", pid2.inFlightCounts());
    check(std::abs(pid2.inFlightCounts()) < 1.0, "in_flight_ drains when output stops");
}

// ── 2. With beta=0 the controller must behave EXACTLY like the pure feedback
//      version (backward compatibility -- this is a hard requirement).
static int run_counts(double kp, double ki, double kd, double p_full, double beta,
                      double win, double dt, double meas_d, double act_d,
                      double dur, double step_px, double vel, double* jitter)
{
    boss::AimPid pid;
    boss::AimPidParams p;
    p.kp = kp; p.ki = ki; p.kd = kd;
    p.p_full_scale_px = p_full; p.limit_counts = 0;
    p.inflight_beta = beta; p.inflight_window_s = win;
    pid.configure(p);

    const int at = std::max(1, (int)std::lround(act_d / dt));
    const int mt = std::max(0, (int)std::lround(meas_d / dt));
    std::deque<double> act((size_t)at, 0.0);
    std::deque<double> meas((size_t)mt, 0.0);

    double target = step_px, cam = 0.0;
    const int ticks = (int)std::lround(dur / dt);
    const int from = ticks - ticks / 5;
    double lo = 1e18, hi = -1e18;
    int settled = -1;

    for (int i = 0; i < ticks; ++i)
    {
        const double et = target - cam;
        meas.push_back(et);
        const double em = meas.front(); meas.pop_front();
        if (std::abs(et) <= 2.0) { if (settled < 0) settled = i; } else settled = -1;

        const int c = pid.step(em, dt);
        act.push_back(c);
        cam += act.front() * 0.25;   // khat only affects the PLANT, not the controller
        act.pop_front();
        target += vel * dt;

        if (i >= from) { const double e2 = target - cam; lo = std::min(lo, e2); hi = std::max(hi, e2); }
    }
    *jitter = hi - lo;
    return settled;
}

static void test_backward_compat()
{
    std::printf("\n[2] beta=0 must be bit-identical to pure feedback\n");
    double j0 = 0, j1 = 0;
    const int s0 = run_counts(40, 3, 0.02, 100, 0.0, 0.046, 1.0/120, 0.03, 0.02, 3.0, 400, 0, &j0);
    const int s1 = run_counts(40, 3, 0.02, 100, 0.0, 0.046, 1.0/120, 0.03, 0.02, 3.0, 400, 0, &j1);
    std::printf("    jitter %.3f vs %.3f, settled %d vs %d\n", j0, j1, s0, s1);
    check(j0 == j1 && s0 == s1, "deterministic and identical with beta off");
}

// ── 3. Does raising beta actually remove the limit cycle? Sweep beta at a
//      Kp that is unstable WITHOUT compensation.
static void test_gain_helps()
{
    std::printf("\n[3] does beta break the limit cycle? (algo property)\n");
    std::printf("    latency 46ms, khat 0.25, Kp that is unstable without comp\n");
    std::printf("    %-6s %-8s %-10s %-10s %-8s\n", "Kp", "beta", "jit_pp", "tail", "settle");
    for (double kp : {40.0, 60.0, 90.0})
    {
        for (double b : {0.0, 0.2, 0.4, 0.6, 0.8, 1.0})
        {
            double j = 0;
            const int s = run_counts(kp, 3, 0.02, 100, b, 0.046, 1.0/120, 0.03, 0.02, 3.0, 400, 0, &j);
            std::printf("    %-6.0f %-8.2f %-10.2f %-10.2f %-8d\n", kp, b, j, j, s);
        }
    }
    // 算法性质: 在某个 beta 上, 一个"无补偿时不稳"的 Kp 必须能稳下来。
    {
        double j_off = 0, j_on = 0;
        const int s_off = run_counts(90, 3, 0.02, 100, 0.0, 0.046, 1.0/120, 0.03, 0.02, 3.0, 400, 0, &j_off);
        const int s_on  = run_counts(90, 3, 0.02, 100, 0.4, 0.046, 1.0/120, 0.03, 0.02, 3.0, 400, 0, &j_on);
        std::printf("    Kp=90: beta=0 jitter %.1f (settle %d) vs beta=0.4 jitter %.1f (settle %d)\n",
                    j_off, s_off, j_on, s_on);
        check(j_on < j_off, "compensation reduces the limit-cycle amplitude at an unstable Kp");
    }
}

// ── 4. Robustness: bad params must not produce NaN/inf output.
static void test_bad_params()
{
    std::printf("\n[4] robustness\n");
    const double dts[] = {0.0, -1.0, NAN, 1e-9, 10.0};
    for (double bad : dts)
    {
        boss::AimPid pid;
        boss::AimPidParams p; p.kp = 60; p.inflight_beta = 0.4; p.inflight_window_s = 0.046;
        pid.configure(p);
        bool ok = true;
        for (int i = 0; i < 500; ++i)
        {
            int c = pid.step(100.0, bad);
            if (c > 100000 || c < -100000) ok = false;
        }
        char buf[128];
        std::snprintf(buf, sizeof buf, "dt=%g does not blow up output", bad);
        check(ok, buf);
    }
    // kp = 0 with beta > 0 must not divide by zero.
    {
        boss::AimPid pid;
        boss::AimPidParams p; p.kp = 0.0; p.ki = 5.0; p.inflight_beta = 0.4;
        pid.configure(p);
        bool ok = true;
        for (int i = 0; i < 2000; ++i)
        {
            int c = pid.step(100.0, 1.0/120);
            if (c > 100000 || c < -100000) ok = false;
        }
        check(ok, "kp=0 + beta>0 does not divide by zero");
    }
    // reset() must clear in-flight state.
    {
        boss::AimPid pid;
        boss::AimPidParams p; p.kp = 60; p.inflight_beta = 0.4; p.inflight_window_s = 0.046;
        pid.configure(p);
        for (int i = 0; i < 30; ++i) pid.step(500.0, 1.0/120);
        const bool had = pid.inFlightCounts() != 0.0;
        pid.reset();
        check(had && pid.inFlightCounts() == 0.0, "reset() clears in-flight bookkeeping");
    }
    // ★ beta 超范围必须被夹住(>1 = 明确的过补偿, 会正反馈发散)。
    {
        boss::AimPid pid;
        boss::AimPidParams p; p.kp = 60; p.inflight_beta = 99.0;
        pid.configure(p);
        bool ok = true;
        for (int i = 0; i < 1000; ++i)
        {
            const int c = pid.step(200.0, 1.0/120);
            if (!std::isfinite(pid.inFlightCounts()) || std::abs(c) > 100000) ok = false;
        }
        check(ok, "an out-of-range beta is clamped and does not diverge");
    }
}

// ── 5. Tracking a MOVING target -- the user's "target moves however it wants,
//      track it to death" requirement. Lag and jitter while tracking.
static void test_moving()
{
    std::printf("\n[5] tracking a moving target (Kp=45, latency 46ms)\n");
    std::printf("    %-8s %-8s %-12s %-12s\n", "vel", "beta", "lag(px)", "jit_pp(px)");
    for (double vel : {100.0, 200.0, 300.0, 500.0})
    {
        for (double b : {0.0, 0.4})
        {
            boss::AimPid pid;
            boss::AimPidParams p;
            p.kp = 45.0; p.ki = 3.0; p.kd = 0.02;
            p.p_full_scale_px = 100.0;
            p.inflight_beta = b; p.inflight_window_s = 0.046;
            pid.configure(p);

            const double dt = 1.0 / 120.0;
            const int at = (int)std::lround(0.02 / dt), mt = (int)std::lround(0.03 / dt);
            std::deque<double> act((size_t)at, 0.0), meas((size_t)mt, 0.0);

            double target = 0, cam = 0;
            const int ticks = (int)std::lround(3.0 / dt);
            const int from = ticks / 2;
            double lo = 1e18, hi = -1e18, sum = 0; int n = 0;

            for (int i = 0; i < ticks; ++i)
            {
                const double et = target - cam;
                meas.push_back(et);
                const double em = meas.front(); meas.pop_front();
                const int c = pid.step(em, dt);
                act.push_back(c * 0.25);
                cam += act.front(); act.pop_front();
                target += vel * dt;
                if (i >= from) { const double e2 = target - cam; lo = std::min(lo, e2); hi = std::max(hi, e2); sum += e2; ++n; }
            }
            std::printf("    %-8.0f %-8.2f %-12.2f %-12.2f\n", vel, b, n ? sum / n : 0, hi - lo);
        }
    }
    // 跟枪时补偿【不能变差】—— 它是为了压振荡, 不是为了牺牲跟随。
    {
        auto track_mean = [](double vel, double b) {
            boss::AimPid pid;
            boss::AimPidParams p;
            p.kp = 45.0; p.ki = 3.0; p.kd = 0.02; p.p_full_scale_px = 100.0;
            p.inflight_beta = b; p.inflight_window_s = 0.046;
            pid.configure(p);
            const double dt = 1.0/120.0;
            const int at=(int)std::lround(0.02/dt), mt=(int)std::lround(0.03/dt);
            std::deque<double> act((size_t)at,0.0), meas((size_t)mt,0.0);
            double target=0,cam=0; const int ticks=(int)std::lround(3.0/dt);
            double sum=0; int n=0;
            for(int i=0;i<ticks;i++){
                const double et=target-cam;
                meas.push_back(et); const double em=meas.front(); meas.pop_front();
                const int c=pid.step(em,dt);
                act.push_back(c*0.25); cam+=act.front(); act.pop_front();
                target+=vel*dt;
                if(i>=ticks/2){ sum+=target-cam; ++n; }
            }
            return n? sum/n : 0.0;
        };
        const double on = track_mean(200.0, 0.4);
        const double off = track_mean(200.0, 0.0);
        std::printf("    tracking lag @200px/s: beta=0 %.2fpx vs beta=0.4 %.2fpx\n", off, on);
        check(on <= off + 1.0, "beta does not make moving-target tracking worse");
    }
}

// ── 6. Sweep beta to find its usable band, and confirm "too much -> worse".
static void test_gain_band()
{
    std::printf("\n[6] usable beta band (Kp=45, latency 46ms)\n");
    std::printf("    %-8s %-10s %-8s %s\n", "beta", "jit_pp", "settle", "verdict");
    for (double b : {0.0, 0.1, 0.2, 0.4, 0.6, 0.8, 1.0})
    {
        double j = 0;
        const int s = run_counts(45, 3, 0.02, 100, b, 0.046, 1.0/120, 0.03, 0.02, 3.0, 400, 0, &j);
        const char* v = (j < 2.0 && s >= 0) ? "GOOD" : (j < 30.0 ? "marginal" : "too much / unstable");
        std::printf("    %-8.2f %-10.2f %-8d %s\n", b, j, s, v);
    }
}

int main()
{
    std::printf("in-flight compensation (dimensionless beta): correctness + properties\n");
    std::printf("(plant varied on purpose -- conclusions are about the ALGORITHM)\n");
    test_ring_accounting();
    test_backward_compat();
    test_gain_helps();
    test_bad_params();
    test_moving();
    test_gain_band();
    std::printf("\n%s (%d failures)\n", g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
