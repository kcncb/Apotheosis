// Regression: the dt low-pass in mouse_thread_loop (kDtSmoothTau = 25ms).
//
// Why this test exists: the aim loop used to feed the PID the BARE wall-clock
// interval between ticks. Real logs (chain_live.log, 41245 ticks) showed:
//     p10=7.45ms  p50=8.55ms  p90=11.64ms  max=23.22ms
// Since the PID scales its output by dt, that jitter is an effective-gain jitter.
// Simulated comparison, uniform 1/120 vs that logged distribution:
//     Kp=60   settle 0.83s -> 2.83s, jitter 0.25 -> 4.25 px, lag 52 -> 92 px
//     Kp=150  settle 0.43s -> 0.95s, jitter 0.25 -> 0.50 px, lag 21 -> 37 px
// i.e. jittery dt made flicks 2.2x slower and tracking lag 80% worse,
// independently of Kp. The low-pass fixes that WITHOUT lagging on real drops.
//
// This pins down both halves of that trade: jitter must be suppressed, and a
// sustained frame-rate change must still be followed quickly.
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Must match kDtSmoothTau in mouse_thread_loop.cpp.
static constexpr double kTau = 0.025;
static double alphaOf(double dt) { return dt / (kTau + dt); }

static std::vector<double> spread(const std::vector<double>& v, double lo, double hi)
{
    std::vector<double> o;
    for (double x : v)
        if (x >= lo && x <= hi) o.push_back(x);
    return o;
}
static double pct(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(v.size() * p))];
}

int main()
{
    std::printf("dt low-pass: suppress jitter, still follow real frame drops\n");

    const double logged_ms[] = {7.45, 8.00, 8.55, 9.60, 11.64, 13.5, 15.25, 23.22};

    // --- (a) jitter suppression under the logged distribution -----------------
    {
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> pick(0, 7);
        std::vector<double> raw, sm;
        double s = 0.0;
        for (int i = 0; i < 6000; ++i)
        {
            const double dt = logged_ms[pick(rng)] / 1000.0;
            if (i > 200) raw.push_back(dt * 1000.0);
            s = (s > 0.0) ? s + (dt - s) * alphaOf(dt) : dt;
            if (i > 200) sm.push_back(s * 1000.0);
        }
        const double rawSpread = pct(raw, 0.90) - pct(raw, 0.10);
        const double smSpread  = pct(sm, 0.90) - pct(sm, 0.10);

        std::printf("    raw      p10=%.2f p90=%.2f spread=%.2f ms\n",
                    pct(raw, 0.10), pct(raw, 0.90), rawSpread);
        std::printf("    smoothed p10=%.2f p90=%.2f spread=%.2f ms\n",
                    pct(sm, 0.10), pct(sm, 0.90), smSpread);

        check(smSpread < rawSpread * 0.6, "jitter spread cut by at least 40%");
        // The smoothed value must stay in the physically sensible band; a
        // low-pass must not drift outside the input range.
        check(pct(sm, 0.10) >= 7.0 && pct(sm, 0.90) <= 24.0,
              "smoothed dt stays within the observed range");
    }

    // --- (b) must FOLLOW a sustained drop (120Hz -> 60Hz) ---------------------
    {
        double s = 1.0 / 120.0;
        double t_settle = -1.0;
        for (int i = 0; i < 2000; ++i)
        {
            const double dt = (i < 20) ? 1.0 / 120.0 : 1.0 / 60.0;
            s = s + (dt - s) * alphaOf(dt);
            if (t_settle < 0.0 && std::abs(s - 1.0 / 60.0) < 0.001)
                t_settle = (i - 20) * dt;
        }
        std::printf("    converged to new dt after %.0f ms (final %.2f ms, target 16.67)\n",
                    t_settle * 1000.0, s * 1000.0);
        check(t_settle > 0.0 && t_settle < 0.150,
              "follows a sustained 120->60Hz change within 150ms");
        check(std::abs(s - 1.0 / 60.0) < 0.0005, "converges to the new dt exactly");
    }

    // --- (c) startup and pathological inputs must not produce a bad dt --------
    {
        // First tick: no history -> must equal the raw dt (no invented value).
        double s = 0.0;
        const double first = 0.0090;
        s = (s > 0.0) ? s + (first - s) * alphaOf(first) : first;
        check(std::abs(s - first) < 1e-12, "first tick passes through unchanged");

        // A huge gap (0.5s stall) must not blow up the state.
        double s2 = 1.0 / 120.0;
        s2 = s2 + (0.5 - s2) * alphaOf(0.5);
        check(std::isfinite(s2) && s2 > 0.0 && s2 < 0.5,
              "a 0.5s stall keeps dt finite and bounded");
    }

    std::printf("\n%s (%d failures)\n",
                g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
