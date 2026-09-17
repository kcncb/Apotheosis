// ─────────────────────────────────────────────────────────────────────────────
// AimMagic PID 内核逐字移植的回归测试
// ─────────────────────────────────────────────────────────────────────────────
//
// 这一组测试的目的**不是**"证明控制器好用"(那是 aim_pid_test 的事), 而是
// **证明移植是忠实的**: 每条断言都对应 `docs/aimmagic-ground-truth.md` 里的
// 一条二进制证据。凡是"照抄但看起来奇怪"的地方, 都有一条测试钉住它 ——
// 这样后来的人想"顺手改成更合理的样子"时会立刻看到红灯。
//
// ★★ 特别重要的一类: **反向验证**。凡是"某功能关掉时与教科书一致"的断言,
//    都必须配一条"打开时必须不同"的断言; 否则那个功能可能从来没生效, 而
//    一致性断言会一直是绿的(CLAUDE.md 记过这个坑, 本轮又踩过一次)。
// ─────────────────────────────────────────────────────────────────────────────
#include "mouse/aim_pid_am.h"

#include <cmath>
#include <cstdio>
#include <string>

// ★ MSVC 默认不定义 M_PI(需要 _USE_MATH_DEFINES 且必须在 <cmath> 之前)。
//   这里自己定义, 避免依赖包含顺序 —— 而且真值必须与 AM 的 0x1401f8030 对齐。
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("[FAIL] %s\n", what.c_str());
}

static void section(const char* name)
{
    std::printf("\n== %s ==\n", name);
}

static bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

// ── 做一个"参数齐全"的轴 ───────────────────────────────────────────────────
static boss::AimPidAxis makeAxis()
{
    boss::AimPidAxis ax;
    ax.kp = 2.0;
    ax.kd = 0.01;
    ax.kf = 0.5;
    ax.i_clamp = 100.0;
    ax.kp_floor = 0.5;
    return ax;
}

// ═══ [1] 枚举数值必须与反汇编一致 ═══════════════════════════════════════════
//
// ★ 这一节是"防抄错"的第一道闸: AM 的档位是**内联立即数**, 不是 QML 列表下标。
//   我在整理文档时一度写成 0x7(因为 Ghidra 把 CMP 0x8 渲染成了一个 denormal
//   double 且印错了位模式), 差点把整张表抄错一位。
static void test_profile_enum()
{
    section("[1] 档位枚举 = 反汇编立即数");

    check(static_cast<int>(boss::AimPidProfile::kFree) == 0x8,
          "PID-Free = 0x8  (证据 1400570db CMP RCX, 0x8)");
    check(static_cast<int>(boss::AimPidProfile::kKalman) == 0xA,
          "PID-Kalman = 0xA (证据 14005725c CMP R9, 0xa)");
    check(static_cast<int>(boss::AimPidProfile::kAdaptive) == 0xC,
          "PID-Adaptive = 0xC (证据 140057100 CMP RCX, 0xc)");
    check(static_cast<int>(boss::AimPidProfile::kEventSync) == 0xD,
          "PID-EventSync/FrameSync = 0xD (证据 1400571d9/14005721c CMP R9, 0xd)");

    // ★ 明确钉住"不是列表下标"这件事: 列表里 Free 在第 2 位、Adaptive 在第 3 位,
    //   但真值是 8 和 12。
    check(static_cast<int>(boss::AimPidProfile::kFree) != 2,
          "★ Free 不是列表下标 2(它是 0x8)");
    check(static_cast<int>(boss::AimPidProfile::kAdaptive) != 3,
          "★ Adaptive 不是列表下标 3(它是 0xC)");
}

// ═══ [2] 常数真值表 ═════════════════════════════════════════════════════════
static void test_constants()
{
    section("[2] 常数(逐个 byte 复核)");

    check(near(boss::pid_am::kDBlendInit, 1.0, 1e-15),
          "0x1401f6358 = 1.0  (D 项式子里那个被误当历史的值)");
    check(near(boss::pid_am::kBlendPointThree, 0.3, 1e-15),
          "0x1401f8010 = 0.3  (Free/Adaptive 的 D 混合 + F 新值权重)");
    check(near(boss::pid_am::kBlendPointSeven, 0.7, 1e-15),
          "0x1401f8018 = 0.7  (F 历史权重; 0.3+0.7=1)");
    check(near(boss::pid_am::kNegFortyPi, -40.0 * M_PI, 1e-9),
          "★ 0x1401f8030 = -40π(带负号)");
    check(near(boss::pid_am::kDtFloor, 0.001, 1e-15),
          "0x1401f8000 = 0.001 (dt 下限)");
    check(near(boss::pid_am::kNanosPerSecond, 1e9, 1.0),
          "0x1401f8028 = 1e9 (ns→s)");
}

// ═══ [3] 第 1 帧早退 ════════════════════════════════════════════════════════
//
// 原版: `if (*(int*)(param_1 + 0x1f) == 1) { dVar21 = 0.0; goto LAB_1400572bf; }`
// ⇒ 第 1 帧 D 项为 0, 且**跳过**积分更新(setpoint_prev 也还没被写过)。
static void test_first_frame_early_out()
{
    section("[3] 第 1 帧: 只有 P 项");

    auto ax = makeAxis();
    boss::AimPidGates g;
    const double out = boss::amPidAxisStep(
        ax, g, /*setpoint=*/10.0, /*dt=*/0.008, boss::AimPidProfile::kEventSync,
        /*frame_index=*/1);

    check(near(out, 2.0 * 10.0, 1e-12), "第 1 帧 = Kp*setpoint = 20.0");
    check(near(ax.d_hist, 0.0, 1e-15), "第 1 帧不写 D 历史");
    // ★★ 第 1 帧**会**累加积分 —— 原版那段积分代码在早退【之前】。
    //    早退只跳过 D 与 F。我第一版把它写成"整段跳过", 被这条抓出来了。
    check(near(ax.integral, 2.0 * 10.0 * 0.008, 1e-12),
          "★★ 第 1 帧【仍然累加】积分(早退只跳 D/F)");
    check(near(ax.setpoint_prev, 0.0, 1e-15),
          "★ 第 1 帧不写 setpoint 历史(早退跳过了它)");

    // 第 2 帧开始才有 D/F。
    const double out2 = boss::amPidAxisStep(
        ax, g, 10.0, 0.008, boss::AimPidProfile::kEventSync, 2);
    check(!near(out2, 20.0, 1e-12),
          "★ 第 2 帧输出与第 1 帧不同(D 项开始参与)");
    check(near(ax.setpoint_prev, 10.0, 1e-15),
          "第 2 帧起 setpoint 历史才被写入");
}

// ═══ [4] D 项混合系数: ★ 必须是 exp, 不能是线性 ═══════════════════════════
//
// 这是本轮最要紧的一条。原版走 IAT 里的 exp(已用数值判据确认, 见 §0.11)。
// 线性式 `1 - 40π·dt` 在 dt≈7.96ms 处 = 1, 8.3ms 时 = 1.043 > 1 —— 荒谬。
static void test_d_blend_is_exp_not_linear()
{
    section("[4] ★★ D 混合系数 = 1 - exp(-40π·dt)");

    const double dt = 0.0083;
    const double expect = 1.0 - std::exp(-40.0 * M_PI * dt);
    check(near(expect, 0.6476, 5e-4),
          "dt=8.3ms ⇒ 系数 ≈ 0.6476(可手算复核)");

    // 通过行为反推系数: 用 kd=0、setpoint 恒定 ⇒ D 项 = (1-blend)*d_hist,
    // 从 d_hist 的衰减率可以直接量出 blend。
    // 第 2 帧给一个阶跃, 之后 setpoint 不变, 观察 d_hist 的比值。
    auto ax = makeAxis();
    ax.kd = 0.0;                      // 排除第二项
    boss::AimPidGates g;
    boss::amPidAxisStep(ax, g, 0.0, dt, boss::AimPidProfile::kEventSync, 1);
    boss::amPidAxisStep(ax, g, 0.0, dt, boss::AimPidProfile::kEventSync, 2);
    // 现在 d_hist = (1-blend)*0 = 0; 给一个只影响第二项之外的方式很难,
    // 所以直接改测: 用一个已知的非零 d_hist 起步。
    ax.d_hist = 1.0;
    ax.setpoint_prev = 5.0;
    const double out = boss::amPidAxisStep(
        ax, g, /*setpoint=*/5.0, dt, boss::AimPidProfile::kEventSync, 3);
    // setpoint 不变 ⇒ 第二项 = 0 ⇒ d_new = (1-blend)*1.0
    const double measured_blend = 1.0 - ax.d_hist;
    check(near(measured_blend, expect, 1e-9),
          "★ 实测衰减率 == 1-exp(-40π·dt)(证明用的是 exp 不是线性)");

    // ★★ 反向验证: 线性式 `1 + (−40π)·dt` 在 dt=8.3ms 会给出 −0.043 ——
    //    一个**负的**混合系数(它在 dt > 1/(40π) ≈ 7.96ms 处就变号了)。
    const double c = -40.0 * M_PI;
    const double linear = 1.0 + c * dt;
    check(linear < 0.0,
          "★ 线性式在 8.3ms 下 = −0.043 < 0(负系数, 物理上不可能)");
    check(!near(measured_blend, linear, 1e-6),
          "★★ 反向验证: 实测量【不等于】线性式");

    // ★★ 关键: exp 与线性在小 dt 下**也差一个量级**, 因为这里不是"dt→0 时
    //    1−e^x → −x"的形状 —— 常数带负号, 线性式是 `1 + c·dt`(→1),
    //    而真值是 `1 − e^{c·dt}`(→0)。**两者在 dt→0 时趋向不同的极限!**
    //    ⇒ 任何"用线性近似代替 exp"的写法在此处**必然**是错的, 不是精度问题。
    const double tiny = 0.0001;
    const double exp_tiny = 1.0 - std::exp(c * tiny);
    const double lin_tiny = 1.0 + c * tiny;
    check(exp_tiny < 0.02, "dt→0 时真值 →0(因为 1 − e^0 = 0)");
    check(lin_tiny > 0.98, "★ dt→0 时线性式 →1(极限都不同, 不是近似精度问题)");
    check(std::fabs(exp_tiny - lin_tiny) > 0.5,
          "★★ 两者在 dt→0 时相差近 1.0 ⇒ 线性近似在此处【根本不成立】");

    (void)out;
}

// ═══ [5] D 项的 (1.0 - blend) 形状 ═════════════════════════════════════════
//
// ★ 原版 `dVar21 = (dVar21 - dVar18) * d_hist + ...` 里的 dVar21 在进这条
//   式子前被赋成 DAT_1401f6358 = 1.0。极易误读成 "d_hist - blend"。
static void test_d_term_uses_one_minus_blend()
{
    section("[5] ★★ D 项 = (1-blend)*d_hist + (Δs/dt)*Kd*blend");

    const double dt = 0.0083;
    auto ax = makeAxis();
    ax.kd = 0.0;
    boss::AimPidGates g;
    boss::amPidAxisStep(ax, g, 0.0, dt, boss::AimPidProfile::kEventSync, 1);
    boss::amPidAxisStep(ax, g, 0.0, dt, boss::AimPidProfile::kEventSync, 2);

    ax.d_hist = 10.0;      // 人为塞一个历史
    ax.setpoint_prev = 3.0;
    boss::amPidAxisStep(ax, g, 3.0, dt, boss::AimPidProfile::kEventSync, 3);
    const double blend = 1.0 - std::exp(-40.0 * M_PI * dt);
    check(near(ax.d_hist, (1.0 - blend) * 10.0, 1e-9),
          "★ 历史系数是 (1-blend)");

    // ★ 反向: 如果写成 (d_hist - blend)*d_hist, 结果会是 (10-0.6476)*10 = 93.5
    const double wrong = (10.0 - blend) * 10.0;
    check(!near(ax.d_hist, wrong, 1e-3),
          "★★ 反向验证: 结果【不等于】误读形状 (d_hist-blend)*d_hist");
}

// ═══ [6] F 项只在 Adaptive 档存在 ══════════════════════════════════════════
static void test_feedforward_only_adaptive()
{
    section("[6] F 项(前馈)只在 PID-Adaptive 档");

    const double dt = 0.008;

    // 非 Adaptive: kf 给再大也不能有 F 项。
    {
        auto ax = makeAxis();
        ax.kf = 1000.0;                 // 故意给一个荒谬的 Kf
        boss::AimPidGates g;
        double prev = 0.0;
        for (int i = 1; i <= 6; ++i)
        {
            const double s = static_cast<double>(i) * 5.0;   // 匀速
            prev = boss::amPidAxisStep(ax, g, s, dt,
                                       boss::AimPidProfile::kEventSync, i);
        }
        check(near(ax.f_hist, 0.0, 1e-15),
              "★ 非 Adaptive 档 f_hist 恒为 0(F 项没被算)");
        check(near(ax.f_accum, 0.0, 1e-15),
              "★ 非 Adaptive 档 f_accum 恒为 0");
    }

    // Adaptive: F 项必须真的产生贡献。
    {
        auto ax = makeAxis();
        ax.kf = 1000.0;
        boss::AimPidGates g;
        for (int i = 1; i <= 6; ++i)
        {
            const double s = static_cast<double>(i) * 5.0;
            boss::amPidAxisStep(ax, g, s, dt, boss::AimPidProfile::kAdaptive, i);
        }
        check(std::fabs(ax.f_hist) > 1e-9,
              "★ Adaptive 档 f_hist 非零(F 项真的在算)");

        // ★★ 反向验证: 同样的输入, Adaptive 的输出必须与 EventSync 不同。
        auto a2 = makeAxis();
        a2.kf = 1000.0;
        boss::AimPidGates g2;
        double o_ev = 0.0;
        for (int i = 1; i <= 6; ++i)
        {
            const double s = static_cast<double>(i) * 5.0;
            o_ev = boss::amPidAxisStep(a2, g2, s, dt,
                                       boss::AimPidProfile::kEventSync, i);
        }
        auto a3 = makeAxis();
        a3.kf = 1000.0;
        boss::AimPidGates g3;
        double o_ad = 0.0;
        for (int i = 1; i <= 6; ++i)
        {
            const double s = static_cast<double>(i) * 5.0;
            o_ad = boss::amPidAxisStep(a3, g3, s, dt,
                                       boss::AimPidProfile::kAdaptive, i);
        }
        check(!near(o_ev, o_ad, 1e-6),
              "★★ 反向验证: Adaptive 与 EventSync 的输出必须不同(F 项真的进了出口)");
    }
}

// ═══ [7] Adaptive 档的 Kp 替换 ═════════════════════════════════════════════
//
// 原版: `if (|setpoint| > |setpoint_prev|) Kp = kp_floor;` —— 严格大于, 且取绝对值。
static void test_adaptive_kp_substitution()
{
    section("[7] ★ Adaptive 档: |setpoint| 增大时改用 kp_floor");

    const double dt = 0.008;
    auto ax = makeAxis();
    ax.kp = 2.0;
    ax.kp_floor = 0.5;
    ax.i_clamp = 0.0;          // ★ Adaptive 跳过积分夹取, 但这里让积分不起作用
    boss::AimPidGates g;

    boss::amPidAxisStep(ax, g, 0.0, dt, boss::AimPidProfile::kAdaptive, 1);
    // setpoint_prev 现在是 0; 给一个增大的 setpoint ⇒ 应当用 kp_floor
    boss::amPidAxisStep(ax, g, 10.0, dt, boss::AimPidProfile::kAdaptive, 2);
    // 从 ax 无法直接读出用的是哪个 Kp, 所以用"输出/ setpoint"间接验证:
    // 第 3 帧 setpoint 不变(不增大) ⇒ 回到 kp
    const double o3 = boss::amPidAxisStep(
        ax, g, 10.0, dt, boss::AimPidProfile::kAdaptive, 3);
    check(std::isfinite(o3), "Adaptive 档输出有限");

    // ★ 严格的边界: setpoint 不变时【不】触发替换(严格大于)。
    auto b = makeAxis();
    b.kp = 2.0;
    b.kp_floor = 0.5;
    boss::AimPidGates g2;
    boss::amPidAxisStep(b, g2, 7.0, dt, boss::AimPidProfile::kAdaptive, 1);
    const double o_same = boss::amPidAxisStep(
        b, g2, 7.0, dt, boss::AimPidProfile::kAdaptive, 2);
    // setpoint 不变 ⇒ P 项 = kp*7 = 14; 若误用 kp_floor 则是 3.5
    check(o_same > 10.0,
          "★ setpoint 不变 ⇒ 用原 Kp(证明判据是严格大于, 不是 >=)");
}

// ═══ [8] 积分夹取: Free/Adaptive 跳过 ══════════════════════════════════════
static void test_integral_clamp_bypass()
{
    section("[8] ★ 积分夹取: EventSync/Kalman 夹, Free/Adaptive 跳过");

    const double dt = 0.01;
    const double big = 1000.0;

    // EventSync: i_clamp=1 ⇒ 积分被夹在 [-1, 1]
    {
        auto ax = makeAxis();
        ax.i_clamp = 1.0;
        boss::AimPidGates g;
        for (int i = 1; i <= 50; ++i)
            boss::amPidAxisStep(ax, g, big, dt, boss::AimPidProfile::kEventSync, i);
        check(std::fabs(ax.integral) <= 1.0 + 1e-9,
              "★ EventSync: 积分被夹到 <= i_clamp");
    }

    // Free: 同样条件, 积分必须远超 1 ⇒ 证明真的跳过了夹取
    {
        auto ax = makeAxis();
        ax.i_clamp = 1.0;
        boss::AimPidGates g;
        for (int i = 1; i <= 50; ++i)
            boss::amPidAxisStep(ax, g, big, dt, boss::AimPidProfile::kFree, i);
        check(std::fabs(ax.integral) > 100.0,
              "★★ 反向验证: Free 档积分【没有】被夹(证明跳过夹取是真的)");
    }

    // Adaptive: 同样跳过
    {
        auto ax = makeAxis();
        ax.i_clamp = 1.0;
        boss::AimPidGates g;
        for (int i = 1; i <= 50; ++i)
            boss::amPidAxisStep(ax, g, big, dt, boss::AimPidProfile::kAdaptive, i);
        check(std::fabs(ax.integral) > 100.0,
              "★★ Free/Adaptive 档积分不被夹");
    }
}

// ═══ [9] 三处闸门布尔 ══════════════════════════════════════════════════════
//
// ★★ 原版的语义比"三个开关"微妙得多(见 §0.6):
//   cVar5 = (+0xFC && +0xFE)                     ← 积分累计的初值
//   if (Adaptive) { if(!+0xFD) integral=0;
//                   if (cVar5) cVar5 = +0xFD; }  ← ★ +0xFD 只在 Adaptive 档管用!
//   出口: if (+0xFE) 加上积分
//
// ⇒ **非 Adaptive 档 `+0xFD` 完全无效**。这一条极容易抄错成"全局开关"。
static void test_gates()
{
    section("[9] 闸门 +0xFC / +0xFD / +0xFE");

    const double dt = 0.01;

    // ── +0xFD 在 Adaptive 档: 清零且不累加 ────────────────────────────────
    {
        auto ax = makeAxis();
        boss::AimPidGates g;
        g.allow_integral = false;
        for (int i = 1; i <= 10; ++i)
            boss::amPidAxisStep(ax, g, 10.0, dt, boss::AimPidProfile::kAdaptive, i);
        check(near(ax.integral, 0.0, 1e-15),
              "★ Adaptive + +0xFD=0 ⇒ 积分恒为 0(既清零也不累加)");
    }

    // ── ★★★ +0xFD 在**非** Adaptive 档: 完全无效(反向验证) ────────────────
    {
        auto ax = makeAxis();
        boss::AimPidGates g;
        g.allow_integral = false;          // 故意关掉
        for (int i = 1; i <= 10; ++i)
            boss::amPidAxisStep(ax, g, 10.0, dt, boss::AimPidProfile::kEventSync, i);
        check(std::fabs(ax.integral) > 1e-9,
              "★★★ EventSync 档 +0xFD=0 【无效】, 积分照常累加"
              "(证明 +0xFD 是 Adaptive 专属闸门)");
    }

    // ── +0xFC 是积分累计的必要条件(全档通用) ──────────────────────────────
    {
        auto ax = makeAxis();
        boss::AimPidGates g;
        g.armed = false;                   // +0xFC = 0
        for (int i = 1; i <= 10; ++i)
            boss::amPidAxisStep(ax, g, 10.0, dt, boss::AimPidProfile::kEventSync, i);
        check(near(ax.integral, 0.0, 1e-15),
              "★ +0xFC=0 ⇒ 积分不累加(这条是全档通用的)");
    }

    // ── +0xFE = false ⇒ 出口不加积分, 但**累加仍然发生** ───────────────────
    {
        auto ax = makeAxis();
        boss::AimPidGates g;
        g.output_integral = false;         // +0xFE = 0 ⇒ cVar5 初值就是 false
        double o_last = 0.0;
        for (int i = 1; i <= 10; ++i)
            o_last = boss::amPidAxisStep(ax, g, 10.0, dt,
                                         boss::AimPidProfile::kEventSync, i);
        // cVar5 = FC && FE = false ⇒ 累加被跳过 ⇒ 积分保持 0
        check(near(ax.integral, 0.0, 1e-15),
              "★ +0xFE=0 ⇒ cVar5 初值为假 ⇒ 累加也被跳过(积分保持 0)");
        check(std::isfinite(o_last), "输出仍有限");
    }

    // ── ★★ 出口对 +0xFE 的**第二处**测试: 累加过之后再关掉它 ───────────────
    //   (证明出口那一处判断是独立存在的, 不只是靠 cVar5 间接生效)
    {
        auto ax = makeAxis();
        boss::AimPidGates g;
        // 先正常累加几拍, 让积分攒起来
        for (int i = 1; i <= 8; ++i)
            boss::amPidAxisStep(ax, g, 10.0, dt, boss::AimPidProfile::kEventSync, i);
        const double int_before = ax.integral;
        check(std::fabs(int_before) > 1e-9, "先让积分攒起来");

        // 现在关掉 +0xFE 并保持积分不变(靠同时关掉累加路径之外的办法不行,
        //  所以要比较"同样积分下, 出口加不加它")
        // ⇒ 直接比较两个平行轴: 一个 +0xFE 开一个关, 且**都**从相同积分起步
        auto on = ax;
        auto off = ax;
        boss::AimPidGates g_on;
        boss::AimPidGates g_off;
        g_off.output_integral = false;
        const double o_on = boss::amPidAxisStep(on, g_on, 10.0, dt,
                                                boss::AimPidProfile::kEventSync, 9);
        const double o_off = boss::amPidAxisStep(off, g_off, 10.0, dt,
                                                 boss::AimPidProfile::kEventSync, 9);
        check(!near(o_on, o_off, 1e-12),
              "★★ 反向验证: 同一积分下 +0xFE 开/关的输出必须不同"
              "(证明出口那一处判断真的存在)");
    }
}

// ═══ [10] setpoint 历史必须在对的时刻更新 ══════════════════════════════════
static void test_setpoint_history_order()
{
    section("[10] setpoint 历史更新时机");

    const double dt = 0.008;
    auto ax = makeAxis();
    boss::AimPidGates g;
    boss::amPidAxisStep(ax, g, 1.0, dt, boss::AimPidProfile::kEventSync, 1);
    check(near(ax.setpoint_prev, 0.0, 1e-15),
          "★ 第 1 帧早退 ⇒ setpoint_prev 仍是 0(原版 goto 跳过赋值)");

    boss::amPidAxisStep(ax, g, 1.0, dt, boss::AimPidProfile::kEventSync, 2);
    check(near(ax.setpoint_prev, 1.0, 1e-15),
          "第 2 帧后 setpoint_prev 才被写入");
}

// ═══ [11] 整体行为: 收敛性(移植版必须仍是一个能用的控制器) ════════════════
static void test_still_converges()
{
    section("[11] 移植版仍是可用控制器(简单闭环)");

    // 一阶被控对象: 位置 += 输出 * 增益
    const double plant_gain = 0.5;
    const double target = 100.0;
    const double dt = 0.008;

    auto ax = makeAxis();
    ax.kp = 0.8;
    ax.kd = 0.02;
    ax.kf = 0.0;
    ax.i_clamp = 50.0;
    boss::AimPidGates g;

    double pos = 0.0;
    for (int i = 1; i <= 400; ++i)
    {
        const double out = boss::amPidAxisStep(
            ax, g, target - pos, dt, boss::AimPidProfile::kEventSync, i);
        pos += out * plant_gain * dt;
        if (!std::isfinite(pos)) break;
    }
    check(std::isfinite(pos), "闭环不发散");
    check(std::fabs(target - pos) < 20.0,
          "闭环能把误差压到 20 以内(移植版仍是一个控制器)");
}

int main()
{
    std::printf("=== AimMagic PID 内核逐字移植回归 ===\n");

    test_profile_enum();
    test_constants();
    test_first_frame_early_out();
    test_d_blend_is_exp_not_linear();
    test_d_term_uses_one_minus_blend();
    test_feedforward_only_adaptive();
    test_adaptive_kp_substitution();
    test_integral_clamp_bypass();
    test_gates();
    test_setpoint_history_order();
    test_still_converges();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
