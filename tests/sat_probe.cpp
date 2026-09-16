// Independent check: does P-term saturation reduce overshoot on a step?
//
// This probe exists because aim_pid_test.cpp concluded the OPPOSITE of what
// aim_pid.h claims. The goal is to find out which one is measuring wrong.
//
// Deliberately minimal: no shared harness, own plant. k = 0.25 px/count,
// 50 ms actuator dead time, step response, overshoot = most negative error
// after the first zero crossing.
#include "mouse/aim_pid.h"
#include <cstdio>
#include <cmath>
#include <deque>

struct Result
{
    double overshoot = 0.0;   // px past the target (>= 0 means "went too far")
    double settle_s  = -1.0;
    double tail_max  = 0.0;
};

// full_scale : p_full_scale_px (1e9 == saturation disabled)
// limit      : limit_counts   (0 == built-in 200)
static Result run_ex(double full_scale, double limit, double kp, double ki, double kd,
                     double dt, int ticks, int gate, double step_px)
{
    boss::AimPid pid;
    boss::AimPidParams p;
    p.kp = kp;
    p.ki = ki;
    p.kd = kd;
    p.p_full_scale_px = full_scale;
    p.limit_counts = static_cast<int>(limit);
    pid.configure(p);

    std::deque<int> inflight(static_cast<size_t>(gate), 0);
    double pos = 0.0;          // camera offset from start position, px
    Result r;
    bool crossed = false;
    int  settled_at = -1;

    for (int i = 0; i < ticks; ++i)
    {
        const double err = step_px - pos;   // what the controller sees

        if (!crossed && err <= 0.0)
            crossed = true;
        if (crossed && -err > r.overshoot)
            r.overshoot = -err;

        // settle: |err| stays within 2px for the rest of the run
        if (std::abs(err) <= 2.0)
        {
            if (settled_at < 0) settled_at = i;
        }
        else
        {
            settled_at = -1;
        }

        const int cmd = pid.step(err, dt);
        inflight.push_back(cmd);
        const int landed = inflight.front();
        inflight.pop_front();
        pos += landed * 0.25;       // k = 0.25 px per count
    }

    if (settled_at >= 0)
        r.settle_s = settled_at * dt;

    const double final_err = step_px - pos;
    r.tail_max = std::abs(final_err);
    return r;
}

// Peak-to-peak of the error over the last 20% of the run: distinguishes a
// sustained limit cycle (large) from a single overshoot that then settles (small).
static double late_ptp(double full_scale, double limit, double kp,
                       double dt, int ticks, int gate, double step_px)
{
    boss::AimPid pid;
    boss::AimPidParams p;
    p.kp = kp; p.ki = 1.0; p.kd = 0.01;
    p.p_full_scale_px = full_scale;
    p.limit_counts = static_cast<int>(limit);
    pid.configure(p);

    std::deque<int> inflight(static_cast<size_t>(gate), 0);
    double pos = 0.0;
    const int from = ticks - ticks / 5;
    double lo = 1e18, hi = -1e18;

    for (int i = 0; i < ticks; ++i)
    {
        const double err = step_px - pos;
        const int cmd = pid.step(err, dt);
        inflight.push_back(cmd);
        const int landed = inflight.front();
        inflight.pop_front();
        pos += landed * 0.25;

        if (i >= from)
        {
            const double e2 = step_px - pos;
            lo = std::min(lo, e2);
            hi = std::max(hi, e2);
        }
    }
    return hi - lo;
}

static void sweep(const char* title, double kp, double limit)
{
    std::printf("\n%s   (kp=%.0f, limit_counts=%s)\n", title, kp,
                limit == 0.0 ? "0=built-in 200" : "100000");
    std::printf("  %-12s %-14s %-12s %-12s\n", "full_scale", "overshoot(px)", "settle(s)", "tail(px)");
    for (double fs : {25.0, 50.0, 100.0, 200.0, 1.0e9})
    {
        // ki/kd matching boss::AimPidParams defaults; kp is the caller's.
        const Result r = run_ex(fs, limit, kp, 1.0, 0.01, 1.0 / 240.0, 3200, 12, 400.0);
        std::printf("  %-12.0f %-14.3f %-12.3f %-12.3f\n",
                    fs, r.overshoot, r.settle_s, r.tail_max);
    }
}

int main()
{
    std::printf("=== P-term saturation vs overshoot (400px step, k=0.25px/count, 50ms dead time) ===\n");

    // The confound in aim_pid_test.cpp: it raised limit_counts to 100000 in the
    // SAME case where it disabled saturation. Run both ways to expose it.
    sweep("A. limiter LEFT ON (built-in 200)", 60.0, 0.0);
    sweep("B. limiter RAISED to 100000",      60.0, 100000.0);

    sweep("C. low gain, limiter ON",          20.0, 0.0);

    // E: where is the crossover? overshoot vs Kp, sat on (full=100) vs sat off.
    std::printf("\nE. crossover: overshoot(px) vs Kp   [sat ON full=100 | sat OFF]\n");
    std::printf("  %-8s %-16s %-16s %-10s\n", "kp", "sat_on", "sat_off", "winner");
    for (double kp : {10.0, 20.0, 30.0, 40.0, 60.0, 100.0, 150.0})
    {
        const Result on  = run_ex(100.0, 0.0, kp, 1.0, 0.01, 1.0 / 240.0, 3200, 12, 400.0);
        const Result off = run_ex(1.0e9, 0.0, kp, 1.0, 0.01, 1.0 / 240.0, 3200, 12, 400.0);
        const char* w = (on.overshoot < off.overshoot) ? "sat ON" :
                        (on.overshoot > off.overshoot) ? "sat OFF" : "tie";
        std::printf("  %-8.0f %-16.2f %-16.2f %-10s\n", kp, on.overshoot, off.overshoot, w);
    }

    // F: is the sat-OFF blow-up a limit cycle (unstable loop) or just one big swing?
    // Report peak-to-peak of the error over the LAST 20% of the run.
    std::printf("\nF. is sat-OFF a sustained oscillation? (late-window peak-to-peak px)\n");
    std::printf("  %-8s %-22s %-22s\n", "kp", "sat_on", "sat_off");
    for (double kp : {20.0, 40.0, 60.0, 100.0})
    {
        const double on  = late_ptp(100.0, 0.0, kp, 1.0 / 240.0, 2400, 12, 400.0);
        const double off = late_ptp(1.0e9, 0.0, kp, 1.0 / 240.0, 2400, 12, 400.0);
        std::printf("  %-8.0f %-22.2f %-22.2f\n", kp, on, off);
    }

    return 0;
}
