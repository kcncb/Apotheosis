// Regression: 瞄点预测 —— 对齐 AimMagic 1.0.30 FUN_14006e470 的真实公式。
//
// 依据: _amrev/AimMagic_RE/v1030/ANALYSIS_v1030.md §4.2
//       _amrev/AimMagic_RE/v108/ANALYSIS.md        §6.2/§6.3
//       _amrev/AimMagic_RE/v1030/qml_z/----_预测补偿...qml
//
// 原式:
//   sizeWeight = (maxW - w) / (maxW - minW)     ; minW < w < maxW 之外为 0
//   predX      = factor_x * motionX * sizeWeight
//   k          = (predX*predXPrev < 0) ? K_damp : 1.0
//   smX        = (1-k)*predXPrev + k*predX
//
// 本测试钉住:
//   [1] 尺寸权重 = AM 线性梯度, 端点/区间外行为精确
//   [2] 静止目标 => 严格零偏移 (绝不能多补一个像素)
//   [3] 预测偏移与三项成正比 (factor / motion / sizeWeight) —— 公式形状
//   [4] 方向翻转阻尼: 同号直通, 反号按 K_damp 滑过去
//   [5] 稳健性: NaN / 非法参数 / 边界
#include "mouse/aim_predict.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main()
{
    std::printf("aim_predict: AimMagic 预测公式对齐\n\n");

    // --- [1] sizeWeight == AM 线性梯度 --------------------------------------
    {
        boss::AimPredict p;
        p.configure(1.0, 1.0, 20.0, 80.0, 1.0);
        std::printf("[1] sizeWeight, minW=20 maxW=80\n");
        std::printf("    w=10 (区间外左) -> %.4f  (期望 0)\n", p.sizeWeight(10.0));
        std::printf("    w=20 (minW)     -> %.4f  (期望 0, AM 是【开区间】minW < w)\n", p.sizeWeight(20.0));
        std::printf("    w=20.001        -> %.4f  (期望 ~1)\n", p.sizeWeight(20.001));
        std::printf("    w=50 (中点)     -> %.4f  (期望 0.5)\n", p.sizeWeight(50.0));
        std::printf("    w=80 (maxW)     -> %.4f  (期望 0, AM 是【开区间】w < maxW)\n", p.sizeWeight(80.0));
        std::printf("    w=100(区间外右) -> %.4f  (期望 0)\n", p.sizeWeight(100.0));

        check(p.sizeWeight(10.0) == 0.0, "小于 minW => 权重 0");
        // AM 原文: sizeWeight = (minW < w < maxW) ? (maxW-w)/(maxW-minW) : 0
        // 两端都是【严格】不等号, 所以 w == minW 落在 else 分支 => 0。
        check(p.sizeWeight(20.0) == 0.0, "等于 minW => 权重 0 (AM 是开区间)");
        check(near(p.sizeWeight(20.001), 1.0, 5e-3), "略大于 minW => 权重趋近 1");
        check(near(p.sizeWeight(50.0), 0.5, 1e-12), "中点 => 权重 0.5 (线性)");
        check(p.sizeWeight(80.0) == 0.0, "等于 maxW => 权重 0 (开区间)");
        check(p.sizeWeight(100.0) == 0.0, "大于 maxW => 权重 0");

        // "目标越小补偿越强" —— 单调性
        bool mono = true;
        for (double w = 21.0; w < 79.0; w += 1.0)
            if (p.sizeWeight(w) <= p.sizeWeight(w + 1.0)) mono = false;
        check(mono, "权重随框宽严格单调递减 (目标越小补偿越强)");

        // maxW 退化时的自适应 (AM: maxW = max(minW+1, cfg->maxW))
        boss::AimPredict q;
        q.configure(1.0, 1.0, 50.0, 10.0, 1.0);   // maxW < minW
        check(q.sizeWeight(50.5) > 0.0, "maxW<=minW 时按 minW+1 兜底, 区间仍非空");
    }

    // --- [2] 静止目标严格零 --------------------------------------------------
    {
        boss::AimPredict p;
        p.configure(1.0, 1.0, 20.0, 80.0, 1.0);
        const auto o = p.compute(0.0, 0.0, 50.0, true);
        check(o.x == 0.0 && o.y == 0.0, "静止目标 (motion=0) 偏移严格为 0");

        // 关闭 (factor 全 0) 也要严格 0
        boss::AimPredict q;
        q.configure(0.0, 0.0, 20.0, 80.0, 1.0);
        const auto o2 = q.compute(500.0, 500.0, 50.0, true);
        check(!q.enabled() && o2.x == 0.0 && o2.y == 0.0,
              "factor=0 (UI 默认) 时完全关闭, 严格透传 0");

        // 区间外也要严格 0
        const auto o3 = p.compute(500.0, 0.0, 10.0, true);
        check(o3.x == 0.0, "框太小(区间外) => 0, 不补");
        const auto o4 = p.compute(500.0, 0.0, 100.0, true);
        check(o4.x == 0.0, "框太大(区间外) => 0, 不补");
    }

    // --- [3] 公式形状: predX = factor * motion * sizeWeight ------------------
    //
    // ★ 2026-09-14: 系数范围收到 ±0.2, 且新增【提前量硬上限】(默认 12px)与
    //   【速度噪声门】(默认 60px/s)。所以这里必须用小系数 + 小速度, 让结果落在
    //   上限【以内】, 否则量到的是夹取后的值而不是公式本身。
    //   上限与门限本身在 [3.1]/[3.2] 单独测 —— 把它们混在一起会让断言失去意义。
    {
        boss::AimPredict p;
        // v=200 已在速度门(60)的两倍以上 => gate=1 满额, 量到的是纯公式。
        p.configure(0.1, 0.1, 20.0, 80.0, 1.0);
        const double motion = 200.0, w = 50.0;      // sw = 0.5
        const double expect = 0.1 * 200.0 * 0.5;    // = 10.0px, 上限 12px 之内

        const auto a = p.compute(motion, 0.0, w, true);
        std::printf("\n[3] factor*motion*sizeWeight: 期望 %.4f, 实得 %.4f\n", expect, a.x);
        check(near(a.x, expect, 1e-9), "与 factor*motion*sizeWeight 精确相符");

        // 线性于 factor
        boss::AimPredict p2; p2.configure(0.12, 0.1, 20.0, 80.0, 1.0);
        const auto b = p2.compute(motion, 0.0, w, true);
        check(near(b.x, expect * 1.2, 1e-9), "factor x1.2 => 偏移 x1.2");

        // 线性于 motion。★ 必须留在【上限之内】: 0.1*0.5*v = 12 在 v=240 处封顶,
        //   所以用 150 -> 300 之外的区间会量到夹取值(上限本身在 [3.1] 单独测)。
        const auto c = p.compute(130.0, 0.0, w, true);
        const auto c2 = p.compute(200.0, 0.0, w, true);
        check(near(c2.x, 0.1 * 200.0 * 0.5, 1e-9) && c2.x > c.x,
              "motion 增大 => 偏移按比例增大 (仍在上限内)");

        // 线性于 sizeWeight (换一个框宽)
        const auto d = p.compute(150.0, 0.0, 35.0, true);   // sw = 0.75
        check(near(d.x, 0.1 * 150.0 * 0.75, 1e-9), "框宽 35 => sw=0.75, 偏移相符");
        // ★ 上面所有点都落在【速度门之上 + 上限之内】, 量到的才是纯公式。

        // Y 轴独立
        const auto e = p.compute(0.0, motion, w, true);
        check(near(e.y, expect, 1e-9) && e.x == 0.0, "Y 轴独立计算");
    }

    // --- [3.1] 提前量硬上限 (任务书 §4.2 第②条) -----------------------------
    //
    // ★ 这是本轮最重要的安全性质: 没有上限, 稳态瞄偏 = factor*sw*v 会随速度
    //   【线性增长】, 目标越快准星偏得越远 —— 正是 §4.2 明令禁止的形态。
    {
        std::printf("\n[3.1] 提前量硬上限\n");
        boss::AimPredict p;
        p.configure(0.2, 0.2, 20.0, 80.0, 1.0, /*max_lead_px=*/12.0);
        const double w = 20.001;   // sw -> 1.0 (最大)

        // 速度从 100 拉到 5000, 提前量必须【封顶在 12px】不再增长。
        double prev = 0.0;
        bool monotone_capped = true;
        for (double v : {100.0, 500.0, 1000.0, 3000.0, 5000.0})
        {
            const auto o = p.compute(v, 0.0, w, true);
            std::printf("    v=%-6.0f  lead=%.4f px\n", v, o.x);
            if (o.x > 12.0 + 1e-9) monotone_capped = false;
            prev = o.x;
        }
        check(monotone_capped, "任何速度下提前量都不超过硬上限");
        check(near(prev, 12.0, 1e-6), "高速下提前量恰好饱和在上限 (不再随 v 增长)");

        // 斜向运动的【模长】也不能超过上限 (否则实际幅度会到 sqrt(2) 倍)
        const auto diag = p.compute(5000.0, 5000.0, w, true);
        const double mag = std::hypot(diag.x, diag.y);
        std::printf("    斜向 (5000,5000): 模长 %.4f px\n", mag);
        check(mag <= 12.0 + 1e-9, "斜向运动的提前量模长同样受限 (不会被 sqrt(2) 放大)");

        // max_lead_px = 0 => 用内置默认 12px (不是"无上限")
        boss::AimPredict p2;
        p2.configure(0.2, 0.2, 20.0, 80.0, 1.0, 0.0);
        check(near(p2.maxLeadPx(), boss::AimPredict::kDefaultMaxLeadPx, 1e-12),
              "max_lead_px=0 回落到内置默认, 而不是无上限");
        // 填得过大也要被绝对天花板挡住
        boss::AimPredict p3;
        p3.configure(0.2, 0.2, 20.0, 80.0, 1.0, 1e9);
        check(p3.maxLeadPx() <= boss::AimPredict::kAbsMaxLeadPx + 1e-12,
              "填超大值被绝对天花板挡住");
    }

    // --- [3.2] 速度噪声门 (任务书 §4.2 第③条) -------------------------------
    //
    // 静止目标的 v̂ 是噪声: 实测 p99=46px/s、max=52px/s。不设门的话这些噪声会被
    // 系数乘成假提前量, 在 Kp=35 下就是瞄点上嗡嗡抖。
    {
        std::printf("\n[3.2] 速度噪声门 (默认 60px/s)\n");
        boss::AimPredict p;
        // ★ 系数取小, 保证"满额"点(2x 门限 = 120)的提前量仍在 12px 上限之内,
        //   否则量到的是夹取值, 看不出门限的形状。0.05*120*1.0 = 6.0px。
        p.configure(0.05, 0.05, 20.0, 80.0, 1.0, 12.0, /*vel_floor=*/60.0);
        const double w = 20.001;   // sw -> 1.0

        // 噪声带以内 (实测 p99=46 / max=52) 必须严格为 0
        for (double v : {0.0, 10.0, 46.0, 52.0, 60.0})
        {
            const auto o = p.compute(v, 0.0, w, true);
            check(o.x == 0.0, "速度在噪声门以内 => 提前量严格为 0");
        }

        // 门限之上必须【连续】长起来 —— 硬开关会在跨门限时产生一拍突变。
        const auto just_above = p.compute(61.0, 0.0, w, true);
        const auto mid         = p.compute(90.0, 0.0, w, true);
        const auto full        = p.compute(120.0, 0.0, w, true);   // 2x 门限 => gate=1
        std::printf("    v=61 -> %.4f, v=90 -> %.4f, v=120 -> %.4f\n",
                    just_above.x, mid.x, full.x);
        check(just_above.x > 0.0, "刚过门限就开始出力 (没有硬开关的死区)");
        check(just_above.x < mid.x && mid.x < full.x, "门限之上单调爬升");
        check(near(full.x, 0.05 * 120.0 * 1.0, 1e-3), "2x 门限处已达满额");
        // 连续性: 门限处两侧的值必须足够接近(斜坡而非跳变)
        check(std::abs(just_above.x - 0.0) < 0.5,
              "跨门限是连续斜坡, 不是一步跳到位");
    }

    // --- [4] 方向翻转阻尼 (AM: k = (predX*predXPrev<0) ? K_damp : 1) ---------
    //
    // ★ 系数与速度都取小值, 让提前量落在上限(12px)以内 —— 否则量到的是夹取结果,
    //   阻尼本身的行为就被掩盖了。
    {
        boss::AimPredict p;
        p.configure(0.1, 0.1, 20.0, 80.0, 0.25);    // K_damp = 0.25
        const double w = 50.0;                       // sw = 0.5
        // v=200 => 速度门之上(gate=1), lead 10px 在上限 12px 之内。
        const double v = 200.0;
        const double want_pos = 0.1 * v * 0.5;       // 10.0px

        // 稳态 +x
        auto o = p.compute(v, 0.0, w, true);
        p.commit(o);
        std::printf("\n[4] K_damp=0.25\n");
        std::printf("    稳态 +x: %.2f (期望 %.2f)\n", o.x, want_pos);

        // 同号下一拍: k=1 => 直接取新值, 不平滑
        const auto same = p.compute(v, 0.0, w, true);
        check(near(same.x, want_pos, 1e-9), "同号 => k=1, 无平滑, 直接取新值");

        // 反号: k=damp => smX = (1-0.25)*prev + 0.25*want
        p.commit(same);
        const auto flip = p.compute(-v, 0.0, w, true);
        const double want_neg = -want_pos;
        const double expect_flip = 0.75 * want_pos + 0.25 * want_neg;   // 2.5px
        std::printf("    翻转到 -v: %.4f (期望 %.4f, 未阻尼会是 %.4f)\n",
                    flip.x, expect_flip, want_neg);
        check(near(flip.x, expect_flip, 1e-9), "反号 => 按 K_damp 混合 (滑过去)");
        check(flip.x > want_neg, "翻转第一拍没有直接跳到目标值");

        // 继续反号若干拍应逐步靠近
        double prev = flip.x;
        p.commit(flip);
        bool converging = true;
        for (int i = 0; i < 40; ++i)
        {
            const auto s = p.compute(-v, 0.0, w, true);
            p.commit(s);
            if (s.x > prev + 1e-9) converging = false;   // must be decreasing
            prev = s.x;
        }
        std::printf("    40 拍后收敛到 %.4f (期望 %.4f)\n", prev, want_neg);
        check(converging, "反号后逐拍单调靠近新值");
        check(near(prev, want_neg, 1e-6), "最终收敛到新提前量");

        // K_damp = 1 => 完全不阻尼
        boss::AimPredict nd;
        nd.configure(0.1, 0.1, 20.0, 80.0, 1.0);
        auto a1 = nd.compute(v, 0.0, w, true); nd.commit(a1);
        auto a2 = nd.compute(-v, 0.0, w, true);
        check(near(a2.x, want_neg, 1e-9), "K_damp=1 => 翻转立刻生效 (无阻尼)");
    }

    // --- [5] 稳健性 ----------------------------------------------------------
    {
        boss::AimPredict p;
        p.configure(1.0, 1.0, 20.0, 80.0, 0.25);

        check(p.compute(400.0, 0.0, 50.0, false).x == 0.0, "速度不可信 => 0");
        check(p.compute(std::nan(""), 0.0, 50.0, true).x == 0.0, "motion NaN => 0");
        check(p.compute(400.0, 0.0, std::nan(""), true).x == 0.0, "宽度 NaN => 0");
        check(std::isfinite(p.compute(1e12, 1e12, 50.0, true).x), "极大速度 => 有限");
        check(p.sizeWeight(std::nan("")) == 0.0, "宽度 NaN => 权重 0");

        boss::AimPredict q;
        q.configure(std::nan(""), std::nan(""), -5.0, -5.0, std::nan(""));
        check(!q.enabled(), "全非法参数 => 关闭而不是爆炸");

        // reset 后第一拍直接采纳, 不残留旧状态
        boss::AimPredict r;
        r.configure(0.1, 0.1, 20.0, 80.0, 0.25);
        auto s1 = r.compute(200.0, 0.0, 50.0, true); r.commit(s1);
        r.reset();
        auto s2 = r.compute(-200.0, 0.0, 50.0, true);
        // reset 后 have_=false => 直接采纳, 不做阻尼混合 => 就是纯公式值
        check(near(s2.x, 0.1 * (-200.0) * 0.5, 1e-9),
              "reset 后不再受旧状态影响 (直接采纳)");
    }

    std::printf("\n%s (%d failures)\n",
                g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
