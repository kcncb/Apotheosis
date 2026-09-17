// 控制器层逻辑回归 —— ①选靶 ②稳定器 ③滤波 ④瞄点 ⑤⑥PID+量化
//
// ★★ 设计纪律：每条断言都必须能【反向验证】—— 把被测代码改一行，它必须变红。
//    跑法见文件末尾的说明。
//
// ★ 本测试不依赖 OpenCV / Windows / Qt，任何平台都能编（见 CLAUDE.md）。

#include "control/aim_controller.h"
#include "control/alpha_beta_filter.h"
#include "control/anchor.h"
#include "control/pid_controller.h"
#include "control/selector.h"
#include "control/stabilizer.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace control;

// ── 极简断言框架 ─────────────────────────────────────────────────────────
static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

static void checkNear(double got, double want, double tol, const std::string& what)
{
    ++g_checks;
    if (!(std::fabs(got - want) <= tol))
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.6f, want %.6f ± %.6f)\n",
                    what.c_str(), got, want, tol);
    }
}

static void section(const char* name)
{
    std::printf("[%s]\n", name);
}

// ══════════════════════════════════════════════════════════════════════════
// ① 选靶
// ══════════════════════════════════════════════════════════════════════════
static void testSelector()
{
    section("① 筛选 + 选靶");

    // 类别桶：0=Aim, 1=Delete, 2=Filter, 3=Aim
    ClassBuckets buckets;
    buckets.byClassId = { Bucket::Aim, Bucket::Delete, Bucket::Filter, Bucket::Aim };

    std::vector<Candidate> cands;
    Candidate a; a.box = Box{ 100, 100, 40, 80 }; a.classId = 0; a.confidence = 0.9;
    Candidate b; b.box = Box{ 300, 100, 40, 80 }; b.classId = 1; b.confidence = 0.9; // Delete
    Candidate c; c.box = Box{ 500, 100, 40, 80 }; c.classId = 2; c.confidence = 0.9; // Filter
    Candidate d; d.box = Box{ 105, 300, 40, 80 }; d.classId = 3; d.confidence = 0.9;
    cands = { a, b, c, d };

    const std::vector<size_t> aimIdx = filterAimCandidates(cands, buckets);
    check(aimIdx.size() == 2, "Delete 与 Filter 被筛掉, 只剩 2 个 Aim");
    check(aimIdx[0] == 0 && aimIdx[1] == 3, "留下的是下标 0 与 3");

    // 越界类别 ⇒ 安全默认 Delete（未知类别不瞄）
    ClassBuckets small;
    small.byClassId = { Bucket::Aim };
    Candidate odd; odd.box = Box{ 0, 0, 10, 10 }; odd.classId = 99;
    std::vector<Candidate> one = { odd };
    check(filterAimCandidates(one, small).empty(),
          "越界 classId 视为 Delete (未知类别不瞄)");

    // 无效框被排除
    Candidate bad; bad.box = Box{ 0, 0, 0, 0 }; bad.classId = 0;
    std::vector<Candidate> inv = { bad };
    check(filterAimCandidates(inv, buckets).empty(), "无效框(w=0)被排除");

    // ── 选最近的 ──────────────────────────────────────────────────────
    SelectorConfig sc;
    sc.hysteresisRatio = 1.3;
    SelectorState st;
    const Vec2 cross{ 110, 140 };   // 靠近 a 的中心 (120,140)
    std::vector<Candidate> two = { a, d };
    const std::vector<size_t> idx2 = filterAimCandidates(two, buckets);
    TargetSelection sel = selectTarget(two, idx2, cross, sc, st);
    check(sel.found, "选到了目标");
    checkNear(sel.distancePx, 10.0, 1e-9, "选中的是更近的 a (距离 10px)");

    // ── 滞回：新目标必须【明显更近】才切换 ─────────────────────────────
    // a 稍远、d 稍近，但差距不到 k 倍 ⇒ 应继续保持 a。
    // a 中心 (120,140)，d 中心 (125,340)。
    // 把 cross 放在两者之间偏 d 一点：
    const Vec2 cross2{ 121, 250 };
    const double distA = (Vec2{120,140} - cross2).norm();   // ≈110.0045
    const double distD = (Vec2{125,340} - cross2).norm();   // ≈90.069
    check(distD < distA, "前提: d 比 a 更近");
    check(distD * 1.3 >= distA, "前提: d 的领先幅度【不到】k=1.3 倍");

    // 先锁定 a，再喂同样的两候选
    SelectorState st2;
    std::vector<Candidate> two2 = { a, d };
    TargetSelection first = selectTarget(two2, idx2, cross, sc, st2);
    check(first.classId == 0, "首帧锁定 a");

    TargetSelection second = selectTarget(two2, idx2, cross2, sc, st2);
    check(second.classId == 0,
          "滞回生效: d 虽更近但没到 1.3 倍, 仍保持锁定 a (classId=0)");

    // 把 k 设成 1.0（无滞回）⇒ 必须改成选 d
    SelectorConfig noHyst = sc;
    noHyst.hysteresisRatio = 1.0;
    SelectorState st3;
    selectTarget(two2, idx2, cross, noHyst, st3);          // 先锁 a
    TargetSelection switched = selectTarget(two2, idx2, cross2, noHyst, st3);
    check(switched.classId == 3,
          "k=1.0(无滞回) 时改选更近的 d —— 证明滞回确实来自 hysteresisRatio");

    // ── ★ maxDistancePx: 超出范围的候选不参与选择 ─────────────────────
    {
        SelectorConfig far;
        far.hysteresisRatio = 1.3;
        far.maxDistancePx = 50.0;
        SelectorState stF;
        const Vec2 crossF{ 110, 140 };      // 距 a 中心 10px
        std::vector<Candidate> one2 = { a };   // a 距 10px ⇒ 在范围内
        const std::vector<size_t> idxA = filterAimCandidates(one2, buckets);
        check(selectTarget(one2, idxA, crossF, far, stF).found,
              "maxDistancePx=50 时 10px 的目标可选");

        std::vector<Candidate> farAway = { d };  // d 中心 (125,340) 距 crossF 很远
        const std::vector<size_t> idxD = filterAimCandidates(farAway, buckets);
        SelectorState stF2;
        check(!selectTarget(farAway, idxD, crossF, far, stF2).found,
              "★ maxDistancePx=50 时超距目标被排除 (found=false)");

        // 同一次调用, 不限制时应能选到
        SelectorConfig nolimit = far;
        nolimit.maxDistancePx = 0.0;
        SelectorState stF3;
        check(selectTarget(farAway, idxD, crossF, nolimit, stF3).found,
              "maxDistancePx=0 (不限制) 时同一目标可选 —— 证明上一条来自该参数");
    }

    // 锁定目标消失 ⇒ 锁定失效
    SelectorState st4;
    selectTarget(two2, idx2, cross, sc, st4);
    std::vector<Candidate> onlyD = { d };
    const std::vector<size_t> idxOnlyD = filterAimCandidates(onlyD, buckets);
    TargetSelection afterGone = selectTarget(onlyD, idxOnlyD, cross2, sc, st4);
    check(afterGone.found && afterGone.classId == 3, "锁定目标消失后改选剩下的 d");

    // 空候选 ⇒ found=false 且状态复位
    SelectorState st5;
    std::vector<Candidate> none;
    TargetSelection empty = selectTarget(none, {}, cross, sc, st5);
    check(!empty.found, "无候选时 found=false");
    check(!st5.locked, "无候选时锁定状态被复位");
}

// ══════════════════════════════════════════════════════════════════════════
// ② 稳定器 —— ★ 最关键的一条: 它必须【不平滑】
// ══════════════════════════════════════════════════════════════════════════
static void testStabilizer()
{
    section("② 稳定器 (认目标 + 剔除异常框, 不滤波)");

    StabilizerConfig cfg;
    cfg.matchCenterRatio = 0.5;
    cfg.areaRatioTol = 2.0;
    cfg.kSnapMult = 1.15;
    cfg.minAspect = 0.2;
    cfg.maxAspect = 5.0;

    // 宽高比
    check(aspectRatioPlausible(Box{0,0,40,80}, cfg), "40x80 (宽高比 0.5) 合法");
    // ★ 200x40 的宽高比正好是 5.0 = maxAspect ⇒ 落在边界【上】, 应通过
    //   (判定是 aspect <= maxAspect, 闭区间)
    check(aspectRatioPlausible(Box{0,0,200,40}, cfg), "200x40 (宽高比恰为 5.0) 在闭边界上, 合法");
    check(!aspectRatioPlausible(Box{0,0,201,40}, cfg), "201x40 (宽高比 > 5.0) 被拒");
    check(!aspectRatioPlausible(Box{0,0,400,40}, cfg), "400x40 (宽高比 10) 被拒");

    // 首帧
    StabilizerState st;
    Candidate c1; c1.box = Box{ 100, 100, 40, 80 };
    StabilizerResult r1 = stabilize(c1, cfg, st);
    check(r1.verdict == StabilizerVerdict::NoHistory, "首帧判为 NoHistory");
    check(r1.accepted, "首帧 accepted");

    // ★★★ 核心断言: 输出框 == 输入框 (逐位相同, 证明没有平滑)
    //   这是 D6 的直接验证。若有人在这里加一行平滑, 本断言必须变红。
    Candidate c2; c2.box = Box{ 103.7, 101.2, 40.5, 79.3 };
    StabilizerResult r2 = stabilize(c2, cfg, st);
    check(r2.box.x == c2.box.x && r2.box.y == c2.box.y &&
          r2.box.w == c2.box.w && r2.box.h == c2.box.h,
          "★ 输出框与输入框逐位相同 —— 稳定器不做任何位置平滑 (D6)");

    // 突变: 中心跳得很远
    StabilizerState st2;
    Candidate base; base.box = Box{ 100, 100, 40, 80 };
    stabilize(base, cfg, st2);
    Candidate teleport; teleport.box = Box{ 500, 500, 40, 80 };
    StabilizerResult rT = stabilize(teleport, cfg, st2);
    check(rT.verdict == StabilizerVerdict::Snap, "中心瞬移 ⇒ Snap (下游需硬重置)");

    // 突变: 尺寸突变
    StabilizerState st3;
    stabilize(base, cfg, st3);
    Candidate grew; grew.box = Box{ 100, 100, 200, 400 };   // 面积 25 倍
    StabilizerResult rG = stabilize(grew, cfg, st3);
    check(rG.verdict == StabilizerVerdict::Snap, "面积突变 ⇒ Snap");

    // ★★ 尺寸突变【但中心不动】—— 这条用例才能孤立"尺寸判据"。
    //    之前的尺寸用例中心也动了, 所以就算把尺寸判据整个删掉,
    //    "中心太远"这条回退路径照样会返回 Snap ⇒ 那条断言咬不住尺寸逻辑。
    //    (实测: 把 snapped 里的 !sizeOk 拿掉, 旧用例依然全绿。)
    {
        StabilizerState stSz;
        Candidate base2; base2.box = Box{ 100, 100, 40, 80 };   // 中心 (120,140), 面积 3200
        stabilize(base2, cfg, stSz);

        // 中心仍是 (120,140), 但面积 9 倍 ⇒ 只有尺寸判据能发现
        Candidate sameCenterGrew; sameCenterGrew.box = Box{ 60, 20, 120, 240 };
        checkNear(sameCenterGrew.box.centerX(), 120.0, 1e-9, "前提: 新框中心不变");
        checkNear(sameCenterGrew.box.centerY(), 140.0, 1e-9, "前提: 新框中心不变");
        checkNear(sameCenterGrew.box.area() / base2.box.area(), 9.0, 1e-9,
                  "前提: 面积比 9 倍 (超出 areaRatioTol=2)");

        const StabilizerResult rSz = stabilize(sameCenterGrew, cfg, stSz);
        check(rSz.verdict == StabilizerVerdict::Snap,
              "★ 中心不动但面积 9 倍 ⇒ Snap (证明尺寸判据独立生效, "
              "不是靠'中心太远'那条回退路径)");

        // 反向: 中心不动、面积在容差内 ⇒ 必须判 Ok
        StabilizerState stOk;
        stabilize(base2, cfg, stOk);
        Candidate sameCenterSame; sameCenterSame.box = Box{ 105, 110, 30, 60 };  // 中心同, 面积比 ~0.56
        checkNear(sameCenterSame.box.centerX(), 120.0, 1e-9, "前提: 中心不变");
        checkNear(sameCenterSame.box.area() / base2.box.area() > 0.5, true, 0.0,
                  "前提: 面积比在容差内");
        check(stabilize(sameCenterSame, cfg, stOk).verdict == StabilizerVerdict::Ok,
              "中心不动且面积在容差内 ⇒ Ok (证明上面的 Snap 不是无差别触发)");
    }

    // 形状离谱 ⇒ Rejected, 且【不更新基准】
    StabilizerState st4;
    stabilize(base, cfg, st4);
    const Box before = st4.lastBox;
    Candidate weird; weird.box = Box{ 100, 100, 400, 40 };  // 宽高比 10
    StabilizerResult rW = stabilize(weird, cfg, st4);
    check(rW.verdict == StabilizerVerdict::Rejected, "宽高比离谱 ⇒ Rejected");
    check(!rW.accepted, "Rejected 时 accepted=false");
    check(st4.lastBox.x == before.x && st4.lastBox.w == before.w,
          "★ 被剔除的框不成为下一帧基准 (否则误检会带偏基准)");
}

// ══════════════════════════════════════════════════════════════════════════
// ③ α-β 滤波
// ══════════════════════════════════════════════════════════════════════════
static void testAlphaBeta()
{
    section("③ α-β 滤波");

    // α/β 的边界
    checkNear(AlphaBetaFilter::alphaForTau(0.03, 0.0), 0.0, 1e-12, "dt=0 ⇒ α=0");
    checkNear(AlphaBetaFilter::alphaForTau(0.0, 0.008), 1.0, 1e-12, "τ=0 ⇒ α=1 (直通)");
    const double a = AlphaBetaFilter::alphaForTau(0.03, 0.00833);
    check(a > 0.0 && a < 1.0, "正常 dt 下 α ∈ (0,1)");
    checkNear(a, 1.0 - std::exp(-0.00833/0.03), 1e-12, "α = 1−exp(−dt/τ)");
    const double b = AlphaBetaFilter::betaForTau(0.03, 0.00833);
    check(b > 0.0 && b < a, "β < α (β 的收敛更慢, 否则速度会发散)");

    // 首帧采纳观测
    AlphaBetaFilter f;
    check(!f.initialized(), "初始未初始化");
    f.observe(Vec2{100, 200}, 0.00833);
    check(f.initialized(), "首帧后已初始化");
    checkNear(f.position().x, 100.0, 1e-9, "首帧位置=观测");

    // 阶跃: 静止目标突然跳到远处 ⇒ 应逐步逼近, 不瞬间到达
    f.reset();
    f.observe(Vec2{0, 0}, 0.00833);
    f.observe(Vec2{100, 0}, 0.00833);
    const double after1 = f.position().x;
    check(after1 > 0.0 && after1 < 100.0,
          "阶跃后一拍位置在 (0,100) 之间 —— 既没不动也没瞬间到达");

    // 持续跟随: 多拍后应收敛到观测
    for (int i = 0; i < 200; ++i)
        f.observe(Vec2{100, 0}, 0.00833);
    checkNear(f.position().x, 100.0, 1e-6, "持续喂同一点 ⇒ 收敛到该点");

    // ── ★ 复位必须清掉速度 (否则新目标会被旧速度外推) ────────────────
    AlphaBetaFilter f2;
    for (int i = 0; i < 50; ++i)
        f2.observe(Vec2{ static_cast<double>(i) * 10.0, 0 }, 0.00833);
    f2.reset();
    check(!f2.initialized(), "reset 后回到未初始化");
    f2.observe(Vec2{0, 0}, 0.00833);
    checkNear(f2.position().x, 0.0, 1e-9, "reset 后首帧直接采纳观测 (速度已清零)");

    // ★★ 首帧速度必须为 0 —— 不能"猜"一个初速。
    //    猜错的话前几拍会被错误外推, 而 reset 的语义就是"对旧目标一无所知"。
    //    验证方式: 首帧采纳观测, 第二拍若观测不动, 位置必须【保持不动】。
    //    (若首帧给了非零速度, 第二拍会被外推到别处。)
    {
        AlphaBetaFilter g;
        g.observe(Vec2{500, 500}, 0.00833);
        checkNear(g.position().x, 500.0, 1e-9, "首帧位置=观测");
        g.observe(Vec2{500, 500}, 0.00833);
        checkNear(g.position().x, 500.0, 1e-9,
                  "★ 首帧速度为零: 观测不动时第二拍位置也不动 "
                  "(若首帧给了非零速度, 这里会被外推出去)");
    }

    // ★★ 速度【只】是内部状态 —— 复位后残余速度不得影响后续。
    //    对比: 全新滤波器 vs 跑过再复位的滤波器, 喂同样序列必须完全一致。
    {
        const std::vector<Vec2> seq = {
            {0,0}, {10,0}, {20,0}, {30,0}, {40,0}
        };
        AlphaBetaFilter fresh;
        for (const Vec2& p : seq) fresh.observe(p, 0.00833);

        AlphaBetaFilter reused;
        for (int i = 0; i < 30; ++i) reused.observe(Vec2{1000.0 + i * 50.0, 700}, 0.00833);
        reused.reset();
        for (const Vec2& p : seq) reused.observe(p, 0.00833);

        checkNear(fresh.position().x, reused.position().x, 0.0,
                  "★ 复位后与全新滤波器逐位相同 —— 旧速度没有残留");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// ④ 瞄点
// ══════════════════════════════════════════════════════════════════════════
static void testAnchor()
{
    section("④ 瞄点 (中心点 + y偏移)");

    const Vec2 center{ 200, 300 };
    const Box box{ 180, 260, 40, 80 };   // 中心正是 (200,300), 高 80

    // yOffset: 1=框顶, 0.5=中心, 0=框底
    checkNear(anchorFromOffset(center, box, 1.0).y, 260.0, 1e-9, "yOffset=1 ⇒ 框顶 (260)");
    checkNear(anchorFromOffset(center, box, 0.5).y, 300.0, 1e-9, "yOffset=0.5 ⇒ 中心 (300)");
    checkNear(anchorFromOffset(center, box, 0.0).y, 340.0, 1e-9, "yOffset=0 ⇒ 框底 (340)");
    checkNear(anchorFromOffset(center, box, 0.0).x, 200.0, 1e-9, "x 不受 yOffset 影响");

    // 不随机时: yOffsetMax == yOffset ⇒ 恒等于该值
    AimPointConfig cfg;
    cfg.yOffset = 0.25;
    cfg.yOffsetMax = 0.25;
    for (uint64_t i = 0; i < 5; ++i)
        checkNear(computeAnchor(center, box, cfg, i).y, 320.0, 1e-9,
                  "yOffset 区间退化时恒定 (0.25 ⇒ 320)");

    // 随机区间: 结果必须落在 [lo, hi] 内, 且【同一帧可复现】
    AimPointConfig rnd;
    rnd.yOffset = 0.4;
    rnd.yOffsetMax = 0.6;
    rnd.randomSeed = 12345;
    bool inRange = true;
    for (uint64_t i = 0; i < 200; ++i)
    {
        const double y = computeAnchor(center, box, rnd, i).y;
        // yOffset 0.4 ⇒ y=308; 0.6 ⇒ y=292
        if (y < 292.0 - 1e-9 || y > 308.0 + 1e-9) inRange = false;
    }
    check(inRange, "随机 yOffset 始终落在 [yOffset, yOffsetMax] 对应的区间内");

    const double y1 = computeAnchor(center, box, rnd, 7).y;
    const double y2 = computeAnchor(center, box, rnd, 7).y;
    checkNear(y1, y2, 0.0, "同一 frameIndex 结果完全可复现 (单测不会闪烁)");

    // 分布确实散开 (不是恒定值)
    bool sawDifferent = false;
    const double yA = computeAnchor(center, box, rnd, 1).y;
    for (uint64_t i = 2; i < 50; ++i)
        if (std::fabs(computeAnchor(center, box, rnd, i).y - yA) > 1e-9) sawDifferent = true;
    check(sawDifferent, "不同帧的随机 yOffset 确实不同 (不是伪随机失效)");

    // 区间写反也能工作
    AimPointConfig rev;
    rev.yOffset = 0.6;
    rev.yOffsetMax = 0.4;
    rev.randomSeed = 1;
    const double yr = computeAnchor(center, box, rev, 3).y;
    check(yr >= 292.0 - 1e-9 && yr <= 308.0 + 1e-9, "yOffset/yOffsetMax 写反也落在同一区间");
}

// ══════════════════════════════════════════════════════════════════════════
// ⑤⑥ PID + 量化
// ══════════════════════════════════════════════════════════════════════════
static void testPid()
{
    section("⑤⑥ PID + 量化结转");

    const double dt = 1.0 / 120.0;

    // ── 纯 P: 输出 = lround(dt·Kp·e) ──────────────────────────────────
    {
        PidConfig cfg;   // 默认 kp=35, ki=kd=0
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{110, 0}, Vec2{100, 0}, dt);   // e=10
        // u = dt·Kp·e = 0.008333·35·10 = 2.9167 ⇒ 3
        check(c.x == 3, "纯 P: e=10 ⇒ 3 counts (dt·35·10 = 2.917 ⇒ round 3)");
        check(c.y == 0, "y 误差 0 ⇒ 输出 0");
    }

    // ── 零增益 ⇒ 零输出 (证明没有偷偷加东西) ──────────────────────────
    {
        PidConfig cfg;
        cfg.kpX = cfg.kpY = 0.0;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{1000, 1000}, Vec2{0, 0}, dt);
        check(c.x == 0 && c.y == 0, "Kp=0 ⇒ 输出恒 0");
    }

    // ── ★ 余量结转: 小误差反复施加, 必须累出非零输出 ──────────────────
    //   这是"卡在差两像素不动"那个老问题的直接回归。
    {
        PidConfig cfg;
        cfg.kpX = cfg.kpY = 0.1;    // 故意很小
        // e=0.6px ⇒ u = 0.008333·0.1·0.6 = 0.0005 counts/拍
        // ★ 500 拍累积 = 0.25 counts, 【攒不够 1】⇒ 总下发应当恰好是 0。
        //   这条钉的是"结转不放大": 只有真攒够 1 才发, 不许四舍五入提前发。
        PidController pid2(cfg);
        int total = 0;
        for (int i = 0; i < 500; ++i)
            total += pid2.update(Vec2{100.6, 0}, Vec2{100, 0}, dt).x;
        check(total == 0,
              "★ 结转不放大: 500 拍累积 0.25 counts (<1) ⇒ 总下发为 0");

        // 换一个能攒够的误差: e=3 ⇒ u = 0.0025/拍 × 500 = 1.25 ⇒ 应下发 1
        PidController pid3(cfg);
        int total3 = 0;
        for (int i = 0; i < 500; ++i)
            total3 += pid3.update(Vec2{103.0, 0}, Vec2{100, 0}, dt).x;
        check(total3 == 1, "★ 结转生效: 500 拍累积 1.25 counts ⇒ 总下发 1");

        // ★★ 这条才是"卡在差两像素不动"那个老 bug 的直接回归:
        //    用极小的 u/拍 持续施加, 足够多拍之后【必须】出非零。
        //    2500 拍 × 0.0005 = 1.25 ⇒ 必须下发 1。
        PidController pid4(cfg);
        int total4 = 0;
        for (int i = 0; i < 2500; ++i)
            total4 += pid4.update(Vec2{100.6, 0}, Vec2{100, 0}, dt).x;
        check(total4 == 1,
              "★ 余量结转生效: 0.0005 counts/拍 累加 2500 拍 ⇒ 必须出 1 "
              "(不结转的话永远是 0, 就是当年'卡在差两像素不动'的 bug)");
    }

    // ── 分方向: x 与 y 增益互不影响 ───────────────────────────────────
    {
        PidConfig cfg;
        cfg.kpX = 100.0;
        cfg.kpY = 0.0;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{200, 200}, Vec2{100, 100}, dt);
        check(c.x != 0, "Kp_x=100 ⇒ x 有输出");
        check(c.y == 0, "★ Kp_y=0 ⇒ y 无输出 (证明两套增益独立)");
    }

    // ── 限幅 ──────────────────────────────────────────────────────────
    {
        PidConfig cfg;
        cfg.kpX = 100000.0;
        cfg.maxOutputCounts = 50;
        PidController pid(cfg);
        const Counts c = pid.update(Vec2{10000, 0}, Vec2{0, 0}, dt);
        check(std::abs(c.x) <= 50, "输出被限幅夹住 (|counts| <= maxOutputCounts)");
    }

    // ── ★★ 无死区: 任意小的误差都必须出力 ────────────────────────────
    //    死区已整项删除(实测 5px 会引出 10.2 次/秒的翻转抖动)。
    //    这条钉住"没有人把死区加回来" —— 最小误差也必须产生输出。
    {
        PidConfig cfg;
        cfg.kpX = 100000.0;   // 放大增益, 让极小误差也够 1 count
        PidController pid(cfg);
        const Counts tiny = pid.update(Vec2{0.01, 0}, Vec2{0, 0}, dt);
        check(tiny.x > 0,
              "★ 无死区: 误差 0.01px 也照常出力 (若有人加回死区, 这里会变 0)");

        // P 项必须【经过原点且连续】—— 小误差时 P 项就等于误差本身
        PidController pid2(cfg);
        pid2.update(Vec2{0.01, 0}, Vec2{0, 0}, dt);
        checkNear(pid2.telemetry().x.p, 0.01, 1e-12,
                  "★ P 项经过原点: 0.01px 误差 ⇒ P=0.01 (连续, 无死区台阶)");
    }

    // ── ★★ D 项: 低通必须真的起作用 ──────────────────────────────────
    //    (之前 kd 默认 0 ⇒ D 项整段被跳过, 所以低通从没被测过。)
    {
        // 无低通(τ=0): 误差阶跃时 D 项 = de/dt, 是冲激式的巨大值
        // 有低通(τ=20ms): 同一个阶跃被抹平, D 项显著更小
        auto dTermAfterStep = [dt](double tau) {
            PidConfig c;
            c.kpX = 1.0;
            c.kdX = 1.0;
            c.tauDerivSec = tau;
            PidController p(c);
            p.update(Vec2{0, 0}, Vec2{0, 0}, dt);          // 建立 prevError=0
            p.update(Vec2{100, 0}, Vec2{0, 0}, dt);        // 误差阶跃到 100
            return p.telemetry().x.d;
        };
        const double dNoLp = dTermAfterStep(0.0);
        const double dLp = dTermAfterStep(0.020);
        check(dNoLp > 0.0, "D 项在误差阶跃时为正 (τ=0)");
        check(dLp > 0.0, "D 项在误差阶跃时为正 (有低通)");
        check(dLp < dNoLp,
              "★ D 项低通生效: 同样阶跃下 τ=20ms 的 D 项小于 τ=0 (直通) —— "
              "这就是压制零惯性急停尖峰的机制");

        // ★ kd=0 时 D 项必须恒为 0(不偷偷起作用)
        PidConfig z; z.kpX = 1.0; z.kdX = 0.0;
        PidController pz(z);
        pz.update(Vec2{0, 0}, Vec2{0, 0}, dt);
        pz.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        checkNear(pz.telemetry().x.d, 0.0, 1e-12, "★ kd=0 ⇒ D 项恒为 0");

        // ★ 低通是【指数收敛】不是"一次性全给" —— 连续同向阶跃下方差应衰减
        PidConfig c2; c2.kpX = 1.0; c2.kdX = 1.0; c2.tauDerivSec = 0.020;
        PidController p2(c2);
        p2.update(Vec2{0, 0}, Vec2{0, 0}, dt);
        p2.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        const double d1 = p2.telemetry().x.d;
        p2.update(Vec2{100, 0}, Vec2{0, 0}, dt);   // 误差不再变化 ⇒ de=0 ⇒ 低通状态衰减
        const double d2 = p2.telemetry().x.d;
        check(d2 < d1,
              "★ 误差不再变化时 D 项经低通衰减 (不是保持尖峰值)");
    }

    // ── ★ P 项连续饱和: 误差超过 pFullScalePx 后 P 项不再增大 ──────────
    //    与死区的本质区别: 它连续且经过原点 ⇒ 不存在"停了"这个状态。
    {
        PidConfig a; a.kpX = 1.0; a.pFullScalePx = 0.0;    // 不饱和
        PidConfig b; b.kpX = 1.0; b.pFullScalePx = 10.0;   // 饱和在 10px
        PidController pa(a), pb(b);
        // 误差 100px: 不饱和时 u = dt·1·100; 饱和后 u = dt·1·10
        const Counts ca = pa.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        const Counts cb = pb.update(Vec2{100, 0}, Vec2{0, 0}, dt);
        check(ca.x > cb.x, "★ pFullScalePx 生效: 大误差下饱和版输出更小");
        checkNear(pb.telemetry().x.p, 10.0, 1e-9, "★ P 项被夹到 pFullScalePx");

        // ★ 饱和必须【经过原点且连续】—— 小误差时与不饱和版一致
        PidController pa2(a), pb2(b);
        const Counts sa = pa2.update(Vec2{3, 0}, Vec2{0, 0}, dt);
        const Counts sb = pb2.update(Vec2{3, 0}, Vec2{0, 0}, dt);
        check(sa.x == sb.x, "★ 小误差(|e|<满量程)时饱和不介入 —— 两侧输出一致");
        checkNear(pb2.telemetry().x.p, 3.0, 1e-9, "小误差时 P 项未被夹");
    }

    // ── ★ 限幅在【余量结转之前】—— 被截掉的位移不许攒成欠账 ──────────
    //    (若限幅放在结转之后, 被砍掉的部分会以 carry 形式积累,
    //     表现为"松手后准星冲一下"。)
    {
        PidConfig cfg;
        cfg.kpX = 1e6;              // 故意巨大, 一定撞限幅
        cfg.maxOutputCounts = 10;
        PidController pid(cfg);
        int total = 0;
        for (int i = 0; i < 100; ++i)
            total += pid.update(Vec2{10000, 0}, Vec2{0, 0}, dt).x;
        // 每拍都被限幅到 10 ⇒ 100 拍总下发必须【恰为】1000。
        // 若被砍的位移进了 carry, 总数会【超过】1000(欠账被补发)。
        check(total == 1000,
              "★ 限幅截掉的位移不攒欠账: 100 拍 × 限幅 10 ⇒ 总下发恰为 1000");
        check(std::fabs(pid.telemetry().x.carry) <= 1.0,
              "★ 撞限幅时 carry 保持有界 (不积累欠账)");
    }

    // ── dt 非法 ⇒ 不输出且不改状态 ────────────────────────────────────
    {
        PidController pid;
        const Counts c0 = pid.update(Vec2{200, 0}, Vec2{0, 0}, 0.0);
        check(c0.x == 0 && c0.y == 0, "dt=0 ⇒ 输出 0");
        const Counts cN = pid.update(Vec2{200, 0}, Vec2{0, 0}, -1.0);
        check(cN.x == 0 && cN.y == 0, "dt<0 ⇒ 输出 0 (不以假 dt 出假速度)");
    }

    // ── 积分: 消稳态误差 ──────────────────────────────────────────────
    {
        PidConfig cfg;
        cfg.kpX = 1.0;
        cfg.kiX = 20.0;
        PidController pid(cfg);
        int total = 0;
        for (int i = 0; i < 120; ++i)
            total += pid.update(Vec2{50, 0}, Vec2{0, 0}, dt).x;
        check(total > 0, "Ki>0 时持续误差累积出输出");
        check(pid.telemetry().x.i != 0.0, "积分器有值");
    }

    // ── ★ 积分回吐: 误差反向时积分被快速削减 ──────────────────────────
    {
        PidConfig cfg;
        cfg.kiX = 20.0;
        cfg.tauUnwindSec = 0.030;
        PidController pid(cfg);
        // 先同向累积几拍
        for (int i = 0; i < 20; ++i)
            pid.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        const double before = pid.telemetry().x.i;
        check(before > 0.0, "同向累积后积分为正");

        // 反向一拍 ⇒ 应被削减
        pid.update(Vec2{-50, 0}, Vec2{0, 0}, dt);
        check(pid.telemetry().unwoundX, "反向时标出 unwoundX");
        const double after = pid.telemetry().x.i;
        check(std::fabs(after) < std::fabs(before), "★ 反向时积分数值被削减 (回吐生效)");
    }

    // ── ★ 回吐时间常数: 0.03s 必须比 0.2s 吐得更快 ────────────────────
    {
        // ★ 直接比较"纯回吐衰减因子" —— 这是被测量的定义式,
        //   不依赖 PID 的其他部分, 所以断言是干净的。
        const double decayFast = std::exp(-dt / 0.030);
        const double decaySlow = std::exp(-dt / 0.200);
        check(decayFast < decaySlow,
              "★ τ=30ms 的衰减因子小于 τ=200ms —— 用户修正了历史过慢的 0.2s");

        // 再确认它确实作用到了积分上: 反向两拍后, τ 小的积分更小
        auto integralAfterTwoReverse = [dt](double tau) {
            PidConfig c;
            c.kiX = 20.0;
            c.tauUnwindSec = tau;
            PidController p(c);
            for (int i = 0; i < 20; ++i) p.update(Vec2{50, 0}, Vec2{0, 0}, dt);
            p.update(Vec2{-50, 0}, Vec2{0, 0}, dt);
            p.update(Vec2{-50, 0}, Vec2{0, 0}, dt);
            return std::fabs(p.telemetry().x.i);
        };
        check(integralAfterTwoReverse(0.030) < integralAfterTwoReverse(0.200),
              "★ 实跑验证: τ=30ms 时反向两拍后的积分小于 τ=200ms");
    }

    // ── clamp 抗饱和 ─────────────────────────────────────────────────
    {
        PidConfig cfg;
        cfg.kiX = 1000.0;
        cfg.iMax = 5.0;
        PidController pid(cfg);
        for (int i = 0; i < 1000; ++i)
            pid.update(Vec2{500, 0}, Vec2{0, 0}, dt);
        check(std::fabs(pid.telemetry().x.i) <= 5.0 + 1e-9,
              "★ 积分被 clamp 夹住 (iMax=5) —— 抗饱和生效");
    }

    // ── 稳定线读数 ────────────────────────────────────────────────────
    {
        // Kp=52, dt=1/120 ⇒ g = 52·0.008333·0.593 ≈ 0.2570, /0.2602 ≈ 0.988
        const double r52 = stabilityRatio(52.0, dt);
        check(r52 > 0.9 && r52 < 1.0, "Kp=52 时稳定线读数接近但未越过 1.0");
        const double r100 = stabilityRatio(100.0, dt);
        check(r100 > 1.0, "★ Kp=100 时读数 > 1.0 (与历史实测'Kp=100 抽'一致)");
        const double r30 = stabilityRatio(30.0, dt);
        check(r30 < 1.0, "Kp=30 时读数 < 1.0 (与历史实测'Kp=30 稳'一致)");
        check(r100 > r30, "读数随 Kp 单调增");
    }

    // ── 复位 ──────────────────────────────────────────────────────────
    {
        PidConfig cfg;
        cfg.kiX = 20.0;
        PidController pid(cfg);
        for (int i = 0; i < 20; ++i) pid.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        check(pid.telemetry().x.i != 0.0, "复位前积分非零");

        pid.reset();
        check(pid.telemetry().x.i == 0.0, "reset 后遥测积分清零");

        // ★★ 关键: 上面那条只看【遥测快照】—— 而遥测是 reset() 里单独清掉的,
        //    所以它【咬不住】AxisState::reset() 是否真的清了内部状态。
        //    (实测: 把 AxisState::reset() 里的 integral=0 删掉, 上面那条依然绿。)
        //    真正能证明内部状态被清的, 是【再跑一拍看输出】——
        //    若内部积分没清, 输出会比"干净起点"大一截。
        PidController clean(cfg);
        const Counts afterReset = pid.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        const Counts fromClean = clean.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        check(afterReset.x == fromClean.x,
              "★ reset 后第一拍的输出 == 全新控制器第一拍的输出 "
              "(证明内部积分/余量/历史真的被清了, 不是只清了遥测)");

        // 反向再证: 不复位的话, 累积的积分会把输出顶得更大
        PidController dirty(cfg);
        for (int i = 0; i < 20; ++i) dirty.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        const Counts withoutReset = dirty.update(Vec2{50, 0}, Vec2{0, 0}, dt);
        check(withoutReset.x >= afterReset.x,
              "不复位时输出不小于复位后 (证明上面那条比较是有区分度的)");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// 整链 ①→⑥
// ══════════════════════════════════════════════════════════════════════════
static void testFullChain()
{
    section("整链 ①→⑥");

    ControllerConfig cfg;
    cfg.buckets.byClassId = { Bucket::Aim };
    cfg.selector.hysteresisRatio = 1.3;
    cfg.aimPoint.yOffset = 0.5;
    cfg.aimPoint.yOffsetMax = 0.5;   // 不随机, 便于断言
    cfg.pid.kpX = 35.0;
    cfg.pid.kpY = 35.0;

    const double dt = 1.0 / 120.0;

    // ── 无候选 ⇒ 不 engage ───────────────────────────────────────────
    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        const ControlOutput out = ac.update(in);
        check(!out.engaged, "无候选时不 engage");
        check(out.counts.x == 0 && out.counts.y == 0, "无候选时输出 0");
        check(out.idleReason == ControlOutput::IdleReason::NoCandidates,
              "idleReason = NoCandidates");
    }

    // ── 检测不新鲜 ⇒ 不 engage ───────────────────────────────────────
    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.cross = Vec2{ 320, 240 };
        in.dtSec = dt;
        Candidate c; c.box = Box{ 300, 200, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates.push_back(c);
        in.detectionFresh = false;
        const ControlOutput out = ac.update(in);
        check(!out.engaged, "检测不新鲜时不 engage");
        check(out.idleReason == ControlOutput::IdleReason::StaleDetection,
              "idleReason = StaleDetection");
    }

    // ── 正常一拍: 输出方向必须指向目标 ───────────────────────────────
    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.cross = Vec2{ 320, 240 };          // 准星在中心
        in.dtSec = dt;
        // 目标在准星右下方
        Candidate c; c.box = Box{ 380, 300, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates.push_back(c);

        const ControlOutput out = ac.update(in);
        check(out.engaged, "有目标时 engage");
        check(out.error.x > 0, "误差 x 为正 (目标在右)");
        check(out.error.y > 0, "误差 y 为正 (目标在下)");
        check(out.counts.x > 0, "★ 输出 x 为正 (往右拉)");
        check(out.counts.y > 0, "★ 输出 y 为正 (往下拉)");

        // 锚点应在框中心 (yOffset=0.5)
        checkNear(out.anchor.x, 400.0, 1.0, "锚点 x ≈ 框中心 x (400)");
        checkNear(out.anchor.y, 340.0, 1.0, "锚点 y ≈ 框中心 y (340)");
    }

    // ── ★ 突变 ⇒ 滤波与 PID 被复位 ───────────────────────────────────
    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = dt;
        in.cross = Vec2{ 320, 240 };

        Candidate c; c.box = Box{ 300, 200, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        for (int i = 0; i < 10; ++i)
        {
            in.candidates = { c };
            in.frameIndex = static_cast<uint64_t>(i);
            ac.update(in);
        }
        check(ac.filter()->initialized(), "正常跟随中滤波器已初始化");

        // 瞬移到很远的地方
        Candidate tele; tele.box = Box{ 900, 900, 40, 80 }; tele.classId = 0; tele.confidence = 0.9;
        in.candidates = { tele };
        in.frameIndex = 100;
        const ControlOutput out = ac.update(in);
        check(out.engaged, "瞬移后仍然 engage");
        // Snap 会 reset 滤波器, 然后本帧 observe 首帧 ⇒ 位置直接采纳新观测
        checkNear(ac.filter()->position().x, 920.0, 1.0,
                  "★ 瞬移后滤波器被复位并直接采纳新位置 (而不是从旧位置慢慢滑过去)");
    }

    // ── 形状离谱的框 ⇒ 不出力 ────────────────────────────────────────
    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = dt;
        in.cross = Vec2{ 320, 240 };
        Candidate weird; weird.box = Box{ 300, 200, 800, 20 };  // 宽高比 40
        weird.classId = 0; weird.confidence = 0.9;
        in.candidates = { weird };
        const ControlOutput out = ac.update(in);
        check(!out.engaged, "宽高比离谱的框不 engage");
        check(out.idleReason == ControlOutput::IdleReason::RejectedByStabilizer,
              "idleReason = RejectedByStabilizer");
    }

    // ── reset ────────────────────────────────────────────────────────
    {
        AimController ac;
        ac.setConfig(cfg);
        ControlInput in;
        in.dtSec = dt;
        in.cross = Vec2{ 320, 240 };
        Candidate c; c.box = Box{ 380, 300, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        ac.update(in);
        check(ac.filter()->initialized(), "更新后已初始化");
        ac.reset();
        check(!ac.filter()->initialized(), "reset 后滤波器未初始化");
    }

    // ── ★ 二选一: setFilter(nullptr) 恢复 α-β, 不存在"两个同时生效" ──
    {
        AimController ac;
        ac.setConfig(cfg);
        check(ac.filter() != nullptr, "默认有滤波器");
        ac.setFilter(nullptr);
        check(ac.filter() != nullptr, "setFilter(nullptr) 恢复默认 α-β (不是变成没有滤波)");
    }
}

int main()
{
    std::printf("=== 控制器层逻辑回归 ===\n");

    testSelector();
    testStabilizer();
    testAlphaBeta();
    testAnchor();
    testPid();
    testFullChain();

    std::printf("\n%d 项断言, 失败 %d\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("全部通过\n");
    return g_failures == 0 ? 0 : 1;
}
