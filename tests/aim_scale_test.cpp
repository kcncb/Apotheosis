// 距离尺度 (mouse/aim_scale.h) 的回归。
//
// ── 这个模块测什么 ───────────────────────────────────────────────────────────
// G6「近大远小」要求控制器显式处理"近处画面速度大、远处画面速度小"。本模块的
// 职责只是把 bbox.height 映射成一个【连续、有界、单调】的乘数 s:
//
//     s = clamp( (h / H₀)^γ , s_min , s_max )
//
// 判定哲学(与项目其它回归一致): 只断言可判定的性质 ——
//   连续 / 有界 / 单调 / h=H₀ 处恒为 1.0 / 关掉时逐位中性。
// 不断言具体的 s 数值(那是参数, 调了就不该红)。
//
// ── ★ 2026-09-14 重写: 设计从"两个绝对阈值"改成"单基准" ★ ──────────────────
//   旧设计要用户填 near_h_px / far_h_px 两个绝对像素阈值, 但用户【测不出当前
//   框高】, 换游戏/分辨率也失效。新设计只有一个【自动学】的基准 H₀(用户整定
//   参数时那一段的框高中位数), 用户一个像素值都不用填。
//   s_min 也从"硬钉 1.0"放开到允许 < 1.0(远处真的降增益)。
//   本测试相应重写, 旧断言不再有意义。
//
// ── 这里【故意不测】的东西 ───────────────────────────────────────────────────
// 不测"尺度是否真的提高了跟枪精度" —— 那需要闭环, 归 aim_acceptance_test 的
// G1~G7 用例。本测试只保证这个模块自身的形状正确, 不越界。
#include "mouse/aim_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
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
        std::printf("  [FAIL] %s: got %.6f want %.6f +-%.6f\n", what.c_str(), got, want, tol);
        ++g_failures;
    }
}

// 造一组"已配置好基准"的参数, 供多数用例复用。
boss::AimScaleParams pb(double base_h, double smin = 1.0, double smax = 1.5)
{
    boss::AimScaleParams p;
    p.base_h_px = base_h;
    p.s_min = smin;
    p.s_max = smax;
    p.gamma = 1.0;
    p.smooth_tau_s = 0.0;   // 纯映射用例不关心平滑
    return p;
}

} // namespace

int main()
{
    std::printf("=== aim_scale_test: 距离尺度 (G6, 单基准设计) ===\n");
    const double dt = 1.0 / 120.0;

    // ── 1. 形状: 有界 / 单调 / h=H₀ 处 1.0 / 区间外夹住 ─────────────────────
    std::printf("\n[1] 形状: 有界、单调、基准处中性、区间外夹住\n");
    {
        boss::AimScale sc;
        const double H0 = 100.0;
        boss::AimScaleParams p = pb(H0, 0.5, 1.5);
        sc.configure(p);

        const double lo = p.s_min, hi = p.s_max;

        // h = 基准 -> 恰好 1.0(这是整个设计的中性点, 必须精确)
        check_near(sc.mapHeightToScale(H0), 1.0, 1e-12, "h == 基准 -> s = 1.0");

        // 等比关系: 高度翻倍 -> 倍率翻倍(γ=1); 减半 -> 减半
        // ★ 用宽区间(s_min/s_max 放得很开)才能看到纯粹的等比关系 ——
        //   否则量到的是夹取端点, 不是公式。
        check_near(sc.mapHeightToScale(H0 * 0.5), 0.5, 1e-12, "h = 基准/2 -> s = 0.5(等比)");

        boss::AimScale wide;
        wide.configure(pb(H0, 0.05, 20.0));   // 端点够宽, 不夹
        check_near(wide.mapHeightToScale(H0 * 2.0), 2.0, 1e-9, "h = 基准x2 -> s = 2.0(等比)");
        check_near(wide.mapHeightToScale(H0 * 4.0), 4.0, 1e-9, "h = 基准x4 -> s = 4.0(等比)");
        check_near(wide.mapHeightToScale(H0 * 0.25), 0.25, 1e-12, "h = 基准/4 -> s = 0.25(等比)");

        // 单调不减
        double prev = -1e9;
        bool mono = true;
        for (double h = 1.0; h < 2000.0; h *= 1.05)
        {
            const double s = sc.mapHeightToScale(h);
            if (s < prev - 1e-12) mono = false;
            prev = s;
        }
        check(mono, "单调不减");

        // 有界 + 区间外夹住
        check_near(sc.mapHeightToScale(1e9), hi, 1e-12, "极大框高 -> 夹到 s_max");
        check_near(sc.mapHeightToScale(1e-9), lo, 1e-12, "极小框高 -> 夹到 s_min");
        bool bounded = true;
        for (double h = 0.5; h < 5000.0; h *= 1.1)
        {
            const double s = sc.mapHeightToScale(h);
            if (s < lo - 1e-12 || s > hi + 1e-12) bounded = false;
        }
        check(bounded, "全区间有界");

        // 连续: 相邻采样点不跳变
        bool continuous = true;
        prev = sc.mapHeightToScale(1.0);
        for (double h = 1.0; h < 2000.0; h *= 1.01)
        {
            const double s = sc.mapHeightToScale(h);
            if (std::abs(s - prev) > 0.05) continuous = false;
            prev = s;
        }
        check(continuous, "连续(无硬切换)");
    }

    // ── 2. 没有基准 = 中性 ──────────────────────────────────────────────────
    std::printf("\n[2] 没有基准 -> 恒为 1.0(与关闭本项逐位相同)\n");
    {
        boss::AimScale sc;
        boss::AimScaleParams p;   // 默认 base_h_px = 0
        sc.configure(p);

        check_near(sc.mapHeightToScale(10.0), 1.0, 1e-12, "无基准: 小框 -> 1.0");
        check_near(sc.mapHeightToScale(160.0), 1.0, 1e-12, "无基准: 大框 -> 1.0");
        check_near(sc.mapHeightToScale(1000.0), 1.0, 1e-12, "无基准: 超大框 -> 1.0");

        // 负基准 / NaN 基准也必须是中性, 不能算出怪值
        boss::AimScaleParams neg = p;
        neg.base_h_px = -100.0;
        boss::AimScale sc2;
        sc2.configure(neg);
        check_near(sc2.mapHeightToScale(160.0), 1.0, 1e-12, "负基准 -> 中性");

        boss::AimScaleParams nan = p;
        nan.base_h_px = std::numeric_limits<double>::quiet_NaN();
        boss::AimScale sc3;
        sc3.configure(nan);
        check_near(sc3.mapHeightToScale(160.0), 1.0, 1e-12, "NaN 基准 -> 中性");
    }

    // ── 3. 远处降增益(s_min < 1 现在真的生效) ───────────────────────────────
    std::printf("\n[3] 远处降增益: s_min < 1 必须真的压下去\n");
    {
        boss::AimScale sc;
        const double H0 = 100.0;
        sc.configure(pb(H0, 0.4, 1.5));

        // 远于基准 -> 低于 1.0
        check(sc.mapHeightToScale(H0 * 0.5) < 1.0, "框高减半 -> 倍率低于 1.0");
        check_near(sc.mapHeightToScale(H0 * 0.5), 0.5, 1e-12, "框高减半 -> 0.5");
        // 一直降到 s_min 为止
        check_near(sc.mapHeightToScale(H0 * 0.01), 0.4, 1e-12, "极远 -> 夹到 s_min");
        check(sc.mapHeightToScale(H0 * 3.0) > 1.0, "近于基准 -> 高于 1.0");
    }

    // ── 4. 端点顺序保护 ─────────────────────────────────────────────────────
    std::printf("\n[4] 端点顺序: 填反了不能输出怪值\n");
    {
        boss::AimScale sc;
        boss::AimScaleParams p = pb(100.0);
        p.s_min = 2.0;          // 反了
        p.s_max = 0.5;
        sc.configure(p);
        const double s = sc.mapHeightToScale(1000.0);
        check(std::isfinite(s), "端点填反 -> 仍是有限值");
        check(s >= sc.params().s_min - 1e-12 && s <= sc.params().s_max + 1e-12,
              "端点填反 -> 仍落在 (交换后的) 区间内");
        check(sc.params().s_min <= sc.params().s_max, "端点填反 -> configure 里已交换");
    }

    // ── 5. 非法输入 ─────────────────────────────────────────────────────────
    std::printf("\n[5] 非法输入\n");
    {
        boss::AimScale sc;
        sc.configure(pb(100.0));
        check_near(sc.mapHeightToScale(0.0), 1.0, 1e-12, "h = 0 -> 中性");
        check_near(sc.mapHeightToScale(-5.0), 1.0, 1e-12, "h < 0 -> 中性");
        check_near(sc.mapHeightToScale(std::numeric_limits<double>::quiet_NaN()), 1.0,
                   1e-12, "h = NaN -> 中性");
        check_near(sc.mapHeightToScale(std::numeric_limits<double>::infinity()), 1.0,
                   1e-12, "h = Inf -> 中性");

        // 非法 s_min/s_max/gamma 一律回落到默认
        boss::AimScale bad;
        boss::AimScaleParams bp;
        bp.base_h_px = 100.0;
        bp.s_min = std::numeric_limits<double>::quiet_NaN();
        bp.s_max = -3.0;
        bp.gamma = 0.0;
        bad.configure(bp);
        const double s = bad.mapHeightToScale(100.0);
        check(std::isfinite(s), "非法参数 -> 仍输出有限值");
        check(s > 0.0, "非法参数 -> 仍为正");
        check(bad.params().gamma > 0.0, "gamma <= 0 -> 回落到 1.0");
    }

    // ── 6. step(): 平滑与首拍 ───────────────────────────────────────────────
    std::printf("\n[6] step(): 首拍直接贴上、平滑收敛、坏输入保持\n");
    {
        boss::AimScale sc;
        boss::AimScaleParams p = pb(100.0);
        p.smooth_tau_s = 0.120;
        sc.configure(p);

        // 首拍必须直接采用, 不能从 1.0 慢慢爬 —— 否则锁定后的头几拍会用错尺度。
        const double s0 = sc.step(200.0, dt);
        check_near(s0, 1.5, 1e-12, "首拍直接贴上(不做爬升)");
        check(sc.initialized(), "首拍后已初始化");

        // 换到很小的框 -> 平滑下降, 不瞬间跳
        // ★ 注意: 一拍只走 alpha ≈ dt/(tau+dt) ≈ 6.5%, 所以这里要比较【平滑后的
        //   框高】而不是 s —— 一拍之后 h_smoothed 只从 200 走到约 188, 而
        //   s = 188/100 = 1.88 仍被 s_max=1.5 夹住, 于是 s 看不出变化。
        //   这正是"夹取端点会掩盖内部状态"的一个实例。
        const double h_before = sc.smoothedHeight();
        sc.step(20.0, dt);
        const double h_after = sc.smoothedHeight();
        check(h_after < h_before, "框变小 -> 平滑框高下降");
        check(h_after > 20.0 + 1.0, "一拍之内不会瞬跳到底(有平滑)");

        // 喂够长时间 -> 收敛到稳态值
        // ★ s_min 必须放得比目标值低, 否则量到的是下限而不是收敛值。
        boss::AimScale conv;
        boss::AimScaleParams cp = pb(100.0, 0.1, 1.5);
        cp.smooth_tau_s = 0.120;
        conv.configure(cp);
        conv.step(200.0, dt);
        for (int i = 0; i < 3000; ++i) conv.step(20.0, dt);   // 25 秒, 远超 120ms 时间常数
        check_near(conv.smoothedHeight(), 20.0, 0.01, "长时间后平滑框高收敛到 20");
        check_near(conv.scale(), 0.2, 1e-6, "长时间后 s 收敛到 (20/100) = 0.2");

        // 坏输入沿用上一次的值, 不能把尺度打回 1.0
        const double before = sc.scale();
        check_near(sc.step(0.0, dt), before, 1e-12, "h=0 沿用上次尺度");
        check_near(sc.step(-1.0, dt), before, 1e-12, "h<0 沿用上次尺度");

        // 复位回中性
        sc.reset();
        check_near(sc.scale(), 1.0, 1e-12, "reset -> 中性 1.0");
        check(!sc.initialized(), "reset -> 未初始化");
    }

    // ── 7. smooth_tau = 0 时不平滑 ──────────────────────────────────────────
    std::printf("\n[7] smooth_tau_s = 0 -> 不平滑(纯映射)\n");
    {
        boss::AimScale sc;
        sc.configure(pb(100.0, 0.3, 1.5));
        check_near(sc.step(300.0, dt), 1.5, 1e-12, "无平滑: 一拍到位(夹到上限)");
        check_near(sc.step(30.0, dt), 0.3, 1e-12, "无平滑: 一拍到位(夹到下限)");
    }

    // ── 8. 帧率无关 ─────────────────────────────────────────────────────────
    std::printf("\n[8] 步长无关: 同样的时间跨度应到同样的地方\n");
    {
        boss::AimScale a, b;
        boss::AimScaleParams p = pb(100.0);
        p.smooth_tau_s = 0.120;
        a.configure(p);
        b.configure(p);

        a.step(500.0, 1.0 / 60.0);
        b.step(500.0, 1.0 / 1000.0);
        for (int i = 0; i < 60; ++i) a.step(20.0, 1.0 / 60.0);      // 1 秒 @60fps
        for (int i = 0; i < 1000; ++i) b.step(20.0, 1.0 / 1000.0);  // 1 秒 @1000fps

        // 指数平滑本身对步长是近似不变的(一阶)，允许小差异
        check_near(a.scale(), b.scale(), 0.02, "60fps 与 1000fps 收敛到同一处");
    }

    // ── 9. 基准从哪来 (2026-09-14 改版) ────────────────────────────────────
    //
    // ★ 这个类【不再自己采基准】。旧版这里测的是
    //   begin_baseline_sample / add_baseline_sample / finish_baseline_sample,
    //   与 autotune::Runtime 里那套重复 —— 两份实现迟早不一致, 已删除。
    //
    //   现在唯一来源是 autotune::Runtime(界面按钮 → 最近窗口框高中位数 →
    //   写进 config)。本类只负责"给定基准之后怎么映射", 所以这里测的是
    //   【外部设进来的基准被正确使用】, 而不是"基准怎么算出来的"。
    //   基准算法本身由 autotune_test 覆盖。
    std::printf("\n[9] 基准由外部设入(本类不再自己采)\n");
    {
        boss::AimScale sc;

        // 基准合法 -> 直接用
        auto p = pb(100.0);
        sc.configure(p);
        sc.step(100.0, 1.0);               // 大 dt, 让平滑一步到位
        check_near(sc.scale(), 1.0, 0.02, "h = 基准 -> 中性 1.0");

        // 基准 = 0 (未学到) -> 无论框高多少都是中性
        auto p2 = pb(0.0);
        p2.s_min = 0.30;
        p2.s_max = 1.50;
        boss::AimScale sc2;
        sc2.configure(p2);
        sc2.step(500.0, 1.0);
        check_near(sc2.scale(), 1.0, 1e-9, "无基准 -> 框高大也恒为 1.0");
        sc2.step(5.0, 1.0);
        check_near(sc2.scale(), 1.0, 1e-9, "无基准 -> 框高小也恒为 1.0");

        // 非法基准(过小/过大)在 configure 里被归一成 0 -> 中性
        for (double bad : {1.0, 0.5, 9999.0, -3.0})
        {
            auto p3 = pb(bad);
            boss::AimScale sc3;
            sc3.configure(p3);
            check_near(sc3.params().base_h_px, 0.0, 1e-9,
                       "非法基准被归一成 0");
            sc3.step(300.0, 1.0);
            check_near(sc3.scale(), 1.0, 1e-9, "非法基准 -> 中性");
        }
    }

    // ── 10. 配置热更新会清状态, 但同样配置不反复清 ──────────────────────────
    std::printf("\n[10] configure(): 参数变了才复位\n");
    {
        boss::AimScale sc;
        const auto p = pb(100.0);
        sc.configure(p);
        sc.step(200.0, dt);
        const bool inited = sc.initialized();
        check(inited, "配置后有状态");

        // 每拍都调 configure() 是引擎的实际调用方式 —— 相同参数不能把平滑器打回原点
        // (2026-09-12 在 AnchorObserver 上踩过这个坑: 延迟线永远攒不满)
        for (int i = 0; i < 50; ++i) sc.configure(p);
        check(sc.initialized(), "重复 configure(同参数) 不清状态");
        check_near(sc.scale(), 1.5, 1e-12, "重复 configure(同参数) 尺度不变");

        // 参数真的变了 -> 复位
        auto q = p;
        q.base_h_px = 200.0;
        sc.configure(q);
        check(!sc.initialized(), "基准变化 -> 复位");
        check_near(sc.scale(), 1.0, 1e-12, "复位到中性而不是上一个值");
    }

    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
