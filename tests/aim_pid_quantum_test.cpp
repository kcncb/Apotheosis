// 亚量化抖动的回归 (mouse/aim_pid.h 的 subquantum_guard, 2026-09-13 重做)。
//
// ── 实机现象 ────────────────────────────────────────────────────────────────
// 高 Kp 锁定静止目标后, 准星有 60Hz 的细小抖动。日志实证(Kp=100, k̂=0.593):
//   · 收敛时 |误差| 中位 0.26px —— 比一个输出量子(1 计数 = 0.593px)还小;
//   · 在途补偿 |pnd| 中位 0.470px(≈1 计数的 80%) —— 比误差本身还大,
//     减一下就把误差顶成反号, P 项反向 -> 下一拍打对面 -> 自持振荡;
//   · Kp=100 的 X 轴 43~54% 的帧在发 ±1 计数, 同批帧 Kp=30 的 Y 轴只有 13%;
//   · 框自身逐帧变化中位只有 0.06px —— 【不是检测噪声】, 是回路自己制造的。
//
// ── 本测试的模式变化(为什么这份文件被重写过) ────────────────────────────────
// 老版本由【测试】喂一个假的"在途自身位移"给 PID(老 API 的 AimPidFeedback +
// px_per_count)。重写后 PID 自己在【计数域】记账(见 aim_pid.h 的 inflight_beta),
// 所以测试不再需要、也不应该模拟那批位移 —— 控制器发出的每个计数它自己都记着。
// 现在的假游戏只需要两件事: ①链路死区 ②"计数 -> 画面像素"的换算(那属于被控对象,
// 不属于控制器)。这更接近真实结构, 也让这份回归不再依赖任何被注入的假数据。
//
// ── 断言 ────────────────────────────────────────────────────────────────────
//   [1] 不加保护时确实能复现"尾段每拍都在 ±1 推"的抖动(否则模型错了, 后面没意义);
//   [2] 加保护后尾段下发比例大幅下降, 且静态残差【不退化】;
//   [3] 甩枪/跟枪段(大误差)的逐拍指令序列【完全不受影响】—— 保护是亚量化专用,
//       不能改变手感。这条用逐元素比较, 不给任何容差。
#include "mouse/aim_pid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok)
    {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

constexpr double kDt = 8.333e-3;        // 120fps
constexpr double kDeadTimeS = 0.046;    // 实测链路死区
constexpr double kPxPerCount = 0.593;   // 本机 k̂ —— 只属于【被控对象】, 不进控制器

struct RunResult
{
    int    nonzero_tail = 0;      // 尾段下发非零计数的拍数
    int    sign_flips = 0;        // 尾段相邻非零指令的符号翻转次数
    int    tail_ticks = 0;
    double tail_abs_err_mean = 0.0;
    double tail_abs_err_median = 0.0;
    double tail_abs_err_p90 = 0.0;
    std::vector<int> commands;    // 全部逐拍指令(用于逐元素对比)
};

// 静止目标 + 链路死区。保护开关由 params.subquantum_guard 控制。
//
// 老 API 的 px_per_count 参数已删除 —— 控制器不再消费 k̂。被控对象的换算仍然用
// kPxPerCount, 那是"游戏灵敏度", 与被控对象模型有关, 与控制器无关。
RunResult run_static(double kp, bool guard, double initial_offset_px = 40.0,
                     double duration_s = 3.0, bool quantize_measurement = true)
{
    boss::AimPid pid;
    boss::AimPidParams p;
    p.kp = kp;
    p.ki = 1.0;
    p.kd = 0.0;
    p.p_full_scale_px = 0.0;       // 本回归只关心出口的量化行为, 关掉 P 饱和
    // ★ 在途补偿必须打开: 亚量化抖动的前提就是"补偿量比残差还大"。
    //   beta=1.6 是本项目的生产值(见 aim_pid.h 的扫描表)。
    p.inflight_beta = 1.6;
    p.inflight_window_s = kDeadTimeS;
    p.subquantum_guard = guard;
    pid.configure(p);

    // 假游戏: 目标固定在 0, 准星从 initial_offset_px 外开始收敛。
    double cursor_px = initial_offset_px;
    const double target_px = 0.0;

    // 延迟队列: 每个计数在 kDeadTimeS 之后才让画面动。
    const int delay_ticks = std::max(1, static_cast<int>(std::lround(kDeadTimeS / kDt)));
    std::deque<int> pending(static_cast<size_t>(delay_ticks), 0);

    const int total_ticks = static_cast<int>(std::lround(duration_s / kDt));
    const int tail_from = total_ticks * 65 / 100;

    RunResult r;
    r.commands.reserve(static_cast<size_t>(total_ticks));

    std::vector<double> tail_err;
    int last_nonzero_sign = 0;
    double prev_cursor_px = cursor_px;
    (void)prev_cursor_px;

    for (int i = 0; i < total_ticks; ++i)
    {
        // ── 观测: 误差 = 目标 - 准星(像素)。检测框整数量化 —— 这是现实。 ──
        double err = target_px - cursor_px;
        if (quantize_measurement)
            err = std::round(err);

        const int counts = pid.step(err, kDt);
        r.commands.push_back(counts);

        // ── 被控对象: 延迟 kDeadTimeS 之后才有位移 ──
        pending.push_back(counts);
        const int landing = pending.front();
        pending.pop_front();
        cursor_px += landing * kPxPerCount;

        if (i >= tail_from)
        {
            ++r.tail_ticks;
            const double e = std::abs(target_px - cursor_px);
            tail_err.push_back(e);
            if (counts != 0)
            {
                ++r.nonzero_tail;
                const int sgn = counts > 0 ? 1 : -1;
                if (last_nonzero_sign != 0 && sgn != last_nonzero_sign)
                    ++r.sign_flips;
                last_nonzero_sign = sgn;
            }
        }
    }

    if (!tail_err.empty())
    {
        std::sort(tail_err.begin(), tail_err.end());
        double s = 0.0;
        for (double v : tail_err) s += v;
        r.tail_abs_err_mean = s / static_cast<double>(tail_err.size());
        r.tail_abs_err_median = tail_err[tail_err.size() / 2];
        r.tail_abs_err_p90 =
            tail_err[static_cast<size_t>(0.9 * (tail_err.size() - 1) + 0.5)];
    }
    return r;
}

// ── [1][2] 静默抖动: 保护开/关的对照 ───────────────────────────────────────
void test_static_jitter()
{
    std::printf("\n[1][2] 锁定静止目标后的亚量化抖动 (Kp 扫描)\n");
    std::printf("  %-6s %-22s %-22s %-10s\n",
                "Kp", "guard OFF (frames/flips)", "guard ON (frames/flips)", "residual");

    bool reproduced_any = false;
    for (double kp : {60.0, 100.0, 150.0})
    {
        const RunResult off = run_static(kp, false);
        const RunResult on  = run_static(kp, true);

        std::printf("  %-6.0f %-22s %-22s %.3f -> %.3f px\n",
                    kp,
                    (std::to_string(off.nonzero_tail) + "/" + std::to_string(off.sign_flips)).c_str(),
                    (std::to_string(on.nonzero_tail) + "/" + std::to_string(on.sign_flips)).c_str(),
                    off.tail_abs_err_median, on.tail_abs_err_median);

        // ★ Kp=150 超出 g_crit 太多, 尾段本身就是个【大极限环】(残差 276px),
        //   那不是"亚量化抖动"——抖动的前提是残差已经被压到亚量子级(<1px)。
        //   所以只在"确实进入了亚量化状态"的档位上检查静默性; 失控档位只要求
        //   保护不把它弄得更糟(下面那条统一的"残差不退化"仍然覆盖所有档位)。
        const bool sub_quantum_regime = off.tail_abs_err_median < 1.0;
        if (sub_quantum_regime)
        {
            // [2] 加保护后尾段下发【绝对量】必须很低。
            check(on.nonzero_tail <= std::max(2, off.tail_ticks / 20),
                  "guard ON: the tail is quiet (few output frames)");
        }
        else
        {
            std::printf("         (Kp=%.0f 未进入亚量化状态, 只检查不退化)\n", kp);
        }

        // [1] 不加保护时必须能复现"尾段一直在 ±1 推"。
        //     不要求每个 Kp 都抖 —— 抖动的出现依赖"残差 < 一个量子"这个条件。
        if (off.tail_ticks > 0
            && off.nonzero_tail > off.tail_ticks / 4
            && off.sign_flips > 0)
            reproduced_any = true;

        // [2] 残差【不退化】—— 对所有档位都成立。
        check(on.tail_abs_err_median <= off.tail_abs_err_median + 0.05,
              "guard ON: the static residual is not degraded");
    }
    check(reproduced_any,
          "the unguarded loop does reproduce sub-quantum +-1 chatter (model is valid)");
}

// ── [3] 甩枪途中(大误差段)逐拍完全一致 ─────────────────────────────────────
//
// ★ 这是本文件最重要的一条: 保护必须是"亚量化专用", 绝不能改变甩枪途中的手感。
//
// 【比较到哪一拍为止】不是整个 1.2s, 而是【误差进入亚量子级之前】。理由:
//   保护的定义就是"误差已压到不足一个计数时才介入", 所以它【注定】会在甩枪的最后
//   一两次下发上生效 —— 那正是它该干活的位置。拿整个窗口做逐元素比较, 等于要求
//   保护完全不存在, 那是自相矛盾的断言(实测: 两配置从第 93 拍起不同, off=-1 on=0,
//   而第 92 拍之前完全一致)。
//   所以这里比到"首次出现 |想发的量| < 1 个计数"的那一拍为止。
void test_large_error_identical()
{
    std::printf("\n[3] 甩枪途中(进入亚量子级之前)逐拍指令必须完全一致\n");

    // 为了知道"哪一拍进入亚量子级", 用 guard OFF 跑一遍并记录每拍的
    // "未补偿指令" 的大小 —— 它是判据①的输入。
    struct Trace { std::vector<int> commands; size_t first_subquantum; };
    auto trace = [](bool guard) {
        boss::AimPid pid;
        boss::AimPidParams p;
        p.kp = 100.0; p.ki = 1.0; p.kd = 0.0; p.p_full_scale_px = 0.0;
        p.inflight_beta = 1.6; p.inflight_window_s = kDeadTimeS;
        p.subquantum_guard = guard;
        pid.configure(p);

        Trace t;
        t.first_subquantum = SIZE_MAX;
        double cur = 400.0;
        const int dl = std::max(1, static_cast<int>(std::lround(kDeadTimeS / kDt)));
        std::deque<int> pend(static_cast<size_t>(dl), 0);
        for (int i = 0; i < 300; ++i)
        {
            const double e = 0.0 - cur;          // 不量化: [3] 只看控制器本身
            const int c = pid.step(e, kDt);
            t.commands.push_back(c);
            pend.push_back(c);
            cur += pend.front() * kPxPerCount;
            pend.pop_front();

            // 判据①用【真实的控制器输出】定位"哪一拍起进入亚量子级":
            // 保护只在"本拍想发的量不足一个计数"时才可能介入, 而"想发的量"就是
            // 未加保护的这一拍输出(计数域, 已经取整)。所以直接看指令绝对值即可,
            // 不需要再猜一个 P 项代理 —— 代理会把积分项漏掉, 从而过早触发。
            if (t.first_subquantum == SIZE_MAX && guard == false && std::abs(c) <= 1)
                t.first_subquantum = static_cast<size_t>(i);
        }
        return t;
    };

    const Trace off = trace(false);
    const Trace on  = trace(true);

    size_t compare_n = std::min(off.commands.size(), on.commands.size());
    if (off.first_subquantum != SIZE_MAX)
        compare_n = std::min(compare_n, off.first_subquantum);

    size_t first_diff = compare_n;
    for (size_t i = 0; i < compare_n; ++i)
    {
        if (off.commands[i] != on.commands[i]) { first_diff = i; break; }
    }

    std::printf("  甩枪途中比较前 %zu 拍(第 %zu 拍起进入亚量子级): ",
                compare_n,
                off.first_subquantum == SIZE_MAX ? 0 : off.first_subquantum);
    if (first_diff == compare_n)
        std::printf("两配置逐拍完全相同\n");
    else
        std::printf("第 %zu 拍开始不同: off=%d on=%d\n",
                    first_diff, off.commands[first_diff], on.commands[first_diff]);

    check(compare_n > 40, "the compared (large-error) segment is long enough to matter");
    check(first_diff == compare_n,
          "the sub-quantum guard does not touch the flick commands before sub-quantum state");
}

// ── [4] beta=0(补偿关闭)时保护应当是彻底的恒等变换 ─────────────────────────
void test_guard_is_inert_when_compensation_off()
{
    std::printf("\n[4] 关掉在途补偿时, 亚量化保护不得改变任何输出\n");
    auto run = [](bool guard) {
        boss::AimPid pid;
        boss::AimPidParams p;
        p.kp = 100.0; p.ki = 1.0; p.kd = 0.0; p.p_full_scale_px = 0.0;
        p.inflight_beta = 0.0;          // 补偿关闭
        p.subquantum_guard = guard;
        pid.configure(p);
        std::vector<int> out;
        double cur = 40.0;
        for (int i = 0; i < 360; ++i)
        {
            const double e = std::round(0.0 - cur);
            const int c = pid.step(e, kDt);
            out.push_back(c);
            cur += c * kPxPerCount;
        }
        return out;
    };
    const std::vector<int> a = run(false);
    const std::vector<int> b = run(true);
    bool same = (a.size() == b.size());
    for (size_t i = 0; same && i < a.size(); ++i) same = (a[i] == b[i]);
    std::printf("  beta=0: guard off/on 序列 %s\n", same ? "完全相同" : "不同");
    check(same, "with inflight_beta=0 the guard is an exact identity");
}

} // namespace

int main()
{
    std::printf("sub-quantum chatter: reproduce, suppress, and do not touch the flick\n");
    std::printf("(controller measures its own in-flight counts -- no k-hat anywhere)\n");
    test_static_jitter();
    test_large_error_identical();
    test_guard_is_inert_when_compensation_off();
    std::printf("\n%s (%d checks, %d failures)\n",
                g_failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED",
                g_checks, g_failures);
    return g_failures ? 1 : 0;
}
