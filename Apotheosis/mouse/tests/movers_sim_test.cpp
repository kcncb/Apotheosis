// =============================================================================
// 疾风 PredictiveMover — closed-loop behavior contract (Mac/Linux standalone).
//
// movers.{h,cpp} is pure C++; this harness compiles it without the Windows app
// and asserts the controller invariants in a FAITHFUL closed loop: the mover's
// output dx pans the view, so the target's on-screen position shifts by -dx on
// a configurable latency delay (capture->infer->mouse->render->recapture).
//
// Build & run (from repo root):
//   clang++ -std=c++17 -O2 Apotheosis/mouse/tests/movers_sim_test.cpp \
//           Apotheosis/mouse/movers.cpp -o /tmp/movers_test && /tmp/movers_test
//
// NOT part of the CMake `ai` target (lives in a subdir, has its own main()).
// =============================================================================
#include "../movers.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <deque>

using mover::PredictiveMover;
using mover::PredictiveParams;
using mover::Move;

struct Sim { double lag_mean; double jitter_rms; double max_pass; bool bad; };

// Closed loop on the X axis (Y held inert: anchor_y == crosshair_y).
//   offset0 : initial target offset from crosshair (px)
//   vel     : target world velocity (px/s)
//   noise   : detector anchor noise std (px)
//   latency : round-trip frames between issuing a move and it moving the target
static Sim run(PredictiveParams p, double bw, double bh, double vel,
               double noise, int latency, double offset0,
               double image, double dt, int frames, int warm, unsigned seed)
{
    PredictiveMover m; m.reset(); m.configure(p);
    std::mt19937 rng(seed); std::normal_distribution<double> nd(0.0, noise);
    const double cross = image * 0.5;
    double tgt = cross + offset0;
    std::deque<double> pipe;
    for (int k = 0; k < latency; ++k) pipe.push_back(0.0);

    const double sign0 = (offset0 >= 0.0) ? 1.0 : -1.0;
    double sl = 0, s2 = 0, worst_pass = 0; int n = 0; bool bad = false;

    for (int i = 0; i < frames; ++i)
    {
        tgt += vel * dt;
        const double a = tgt + nd(rng);
        Move mv = m.step(a, cross, cross, cross, bw, bh, image, dt, /*id*/7);
        if (mv.dx > 100000 || mv.dx < -100000) bad = true;   // divergence guard
        pipe.push_back((double)mv.dx);
        const double ap = pipe.front(); pipe.pop_front();
        tgt -= ap;
        const double rel = tgt - cross;            // remaining offset
        const double passed = -sign0 * rel;        // >0 means we crossed past target
        if (passed > worst_pass) worst_pass = passed;
        if (i >= warm) { sl += std::fabs(rel); s2 += (double)mv.dx * (double)mv.dx; ++n; }
    }
    if (n == 0) n = 1;
    return { sl / n, std::sqrt(s2 / n), worst_pass, bad };
}

int main()
{
    const double IMG = 320.0, DT = 1.0 / 60.0;
    const double bw_s = 24, bh_s = 28;     // far/small box
    const double bw_b = 140, bh_b = 150;   // near/big box

    PredictiveParams base; base.kp_x = base.kp_y = 0.15; base.kd = 0.10; base.pred_weight = 1.0;

    int fails = 0; char buf[256];
    auto check = [&](bool ok, const char* name, const char* detail) {
        printf("  [%s] %-42s %s\n", ok ? "PASS" : "FAIL", name, detail);
        if (!ok) ++fails;
    };

    printf("== 疾风 PredictiveMover contract ==\n");

    // T1: velocity feedforward must cut steady-state lag vs no feedforward.
    {
        PredictiveParams p0 = base; p0.pred_weight = 0.0;
        PredictiveParams p1 = base; p1.pred_weight = 1.0;
        Sim s0 = run(p0, bw_s, bh_s, 250, 0.0, 3, 0, IMG, DT, 3000, 800, 7);
        Sim s1 = run(p1, bw_s, bh_s, 250, 0.0, 3, 0, IMG, DT, 3000, 800, 7);
        snprintf(buf, sizeof buf, "lag pred0=%.2f -> pred1=%.2f", s0.lag_mean, s1.lag_mean);
        check(s1.lag_mean < 0.5 * s0.lag_mean && s1.lag_mean < 5.0, "feedforward cuts lag (>2x, <5px)", buf);
    }

    // T2: pred_weight must STRICTLY reduce lag (not be a no-op like the old lead).
    {
        double pw[3] = {0.0, 0.5, 1.0}; double v[3];
        for (int i = 0; i < 3; ++i) { PredictiveParams p = base; p.pred_weight = pw[i];
            v[i] = run(p, bw_s, bh_s, 250, 0.0, 3, 0, IMG, DT, 3000, 800, 7).lag_mean; }
        snprintf(buf, sizeof buf, "lag(0/0.5/1.0)=%.2f/%.2f/%.2f", v[0], v[1], v[2]);
        check(v[1] < v[0] * 0.85 && v[2] < v[1] * 0.85, "pred_weight is a live knob", buf);
    }

    // T3: static target + noise + worst latency: jitter must stay small.
    {
        Sim s = run(base, bw_s, bh_s, 0.0, 2.0, 4, 0, IMG, DT, 4000, 1000, 7);
        snprintf(buf, sizeof buf, "jitter_rms=%.2f px (kp0.15,lat4,noise2)", s.jitter_rms);
        check(s.jitter_rms < 1.5 && !s.bad, "static jitter small", buf);
    }

    // T4: real conditions — LOW lag AND no limit cycle simultaneously.
    {
        Sim s = run(base, bw_s, bh_s, 250, 2.0, 3, 0, IMG, DT, 4000, 1000, 7);
        snprintf(buf, sizeof buf, "lag=%.2f jitter=%.2f (kp0.15,pred1)", s.lag_mean, s.jitter_rms);
        check(s.lag_mean < 5.0 && s.jitter_rms < 8.0 && !s.bad, "low lag + low jitter together", buf);
    }

    // T5 (regression guard): big/near box feedforward is gated off, so a static
    //     approach never overshoots (feedforward adds zero). High-ish kp, no latency.
    {
        PredictiveParams p = base; p.kp_x = p.kp_y = 1.0; p.pred_weight = 1.0;
        Sim s = run(p, bw_b, bh_b, 0.0, 0.0, 0, 50.0, IMG, DT, 600, 0, 7);
        snprintf(buf, sizeof buf, "max pass-through=%.3f px", s.max_pass);
        check(s.max_pass < 1.0 && !s.bad, "big box: no overshoot (ff gated)", buf);
    }

    // T7: feedforward must stay STABLE across unknown real loop latency (the
    //     ring-buffer dead-time model is the whole point). pred=1.0, vel 250.
    {
        bool ok = true; double worst_lag = 0, worst_jit = 0;
        for (int L = 1; L <= 5; ++L) {
            Sim s = run(base, bw_s, bh_s, 250, 2.0, L, 0, IMG, DT, 3000, 800, 7);
            if (s.lag_mean > worst_lag) worst_lag = s.lag_mean;
            if (s.jitter_rms > worst_jit) worst_jit = s.jitter_rms;
            if (s.jitter_rms > 10.0 || s.bad) ok = false;   // 10 >> tracking motion ~4.2 => no limit cycle
        }
        snprintf(buf, sizeof buf, "real lat 1..5: worst lag=%.1f jitter=%.1f", worst_lag, worst_jit);
        check(ok, "feedforward stable across latency", buf);
    }

    // T6 (regression guard): no divergence across a param sweep.
    {
        bool any_bad = false;
        for (double kp : {0.1, 0.2, 0.3}) for (double pr : {0.0, 0.5, 1.0}) for (double v : {0.0, 250.0, 500.0}) {
            PredictiveParams p = base; p.kp_x = p.kp_y = kp; p.pred_weight = pr;
            if (run(p, bw_s, bh_s, v, 2.0, 3, 0, IMG, DT, 1500, 500, 7).bad) any_bad = true;
        }
        check(!any_bad, "no divergence across sweep", "kp{.1,.2,.3} x pred{0,.5,1} x vel{0,250,500}");
    }

    printf("== %s (%d failure%s) ==\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
