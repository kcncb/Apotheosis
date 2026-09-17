// ── 跟踪器 + 预测状态机回归 (tests/aim_tracker_test.cpp) ─────────────────────
//
// 对象: mouse/aim_tracker.h —— AimMagic 1.0.30 的逐字移植 (2026-09-16 重写)。
// 全部判据的行号/常量来自 docs/aimmagic-ground-truth.md, 语料在
//   C:\Users\Administrator\Downloads\AimMagic_RE_extracted\AimMagic_RE\v1030\
//
// ★★ 这一版测试的写法与前一代【不同】: 前一代测的是"我们自己的设计意图"
//    (关联半径 / 累加窗速度 / 自运动增益 …), 而重写后这些都【不存在】了 ——
//    它们在 AM 里没有对应, 已整条删除。所以现在的断言直接写 AM 的行为:
//    常量真值、严格不等号、<= 与 < 的差别、两轴衰减不对称、落点就地改写。
//
// 覆盖:
//   [1]  关联: 类别必须相等 + IoU 严格大于门限(等于不算)
//   [2]  生命周期: min_hits/max_age 的 <= 语义与"漏 max_age 帧仍存活"
//   [3]  速度: 窗内沿用旧值(且不刷新时间戳) / 窗外 0.25/0.75 混合
//   [4]  滑行外推: 位置按 v*clamp(dt) 外推 + X*0.95 / Y*0.8 衰减
//   [5]  尺寸权重: 吃【框高】; h<=min 时权重保持 1.0, h>=max 时才是 0
//   [6]  系数 FSM: 涨 0.1 / 落 0.2 / 夹 [0,1]; "目标够快 且 自己在动"才涨
//   [7]  提前量: 用【平台位移】而不是目标速度(语义正确性的核心断言)
//   [8]  落点: 就地改写框心(out_cx/out_cy)
//   [9]  本项目自加的两道阀默认关闭 —— 默认行为必须与 AM 逐位一致
//  [10]  常量真值表(防止再次把 0.8 写成 0.75 / 把 10.0 写成 3.5057)

#include "mouse/aim_tracker.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_pass = 0;
int g_fail = 0;
std::string g_section;

void section(const char* name)
{
    g_section = name;
    std::printf("\n== %s ==\n", name);
}

void check(bool ok, const std::string& what)
{
    if (ok)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("[FAIL] %s :: %s\n", g_section.c_str(), what.c_str());
    }
}

void check_near(double got, double want, double tol, const std::string& what)
{
    const bool ok = std::abs(got - want) <= tol;
    if (!ok)
    {
        std::printf("[FAIL] %s :: %s (got %.9g, want %.9g, tol %.3g)\n",
                    g_section.c_str(), what.c_str(), got, want, tol);
    }
    check(ok, what);
}

boss::TrackBox box(double cx, double cy, double w = 60.0, double h = 80.0)
{
    boss::TrackBox b{};
    b.x = static_cast<float>(cx - w * 0.5);
    b.y = static_cast<float>(cy - h * 0.5);
    b.w = static_cast<float>(w);
    b.h = static_cast<float>(h);
    return b;
}

// ★ 预测相关用例专用的夹具参数。三条约束必须同时满足, 否则断言会变成空转:
//
//   ① 尺寸门 (sizeWeight): 高度必须落进 (pred_min_w, pred_max_w) 区间。
//      h = 20 == pred_min_w 时 `lo < h` 为假 ⇒ sw 保持 1.0(AM 的边界语义)。
//      想让 sw 取中间值就取 50。这里用 h=20 是为了压低下一条的门限。
//
//   ② 位移门 (系数涨落): X 轴门限 = max(30, 0.8 × 框高) × (1 + 1.5k)。
//      h = 20 ⇒ 0.8*20 = 16 < 30 ⇒ 门限被 30 托住。要涨, 目标每帧位移须 > 30px
//      ⇒ @120fps 需 > 3600px/s。取 9000px/s ⇒ 75px/帧, 留足余量。
//
//   ③ 关联必须撑得住: 框太窄时高速目标两帧不再重叠 ⇒ IoU 归零 ⇒ 每帧新建轨迹
//      ⇒ 速度永远重算不出来。取宽 400px, 9000px/s 下重叠仍充足。
//      ★ 这两个尺寸是夹具的选择, 不是 AM 的常数。
constexpr double kBoxW = 400.0;
constexpr double kBoxH = 20.0;
// 平台(自身)速度: 门限是 > 10px/帧 ⇒ @120fps 需 > 1200px/s。取 1500 ⇒ 12.5px/帧。
constexpr double kSelfV = 1500.0;
// 目标速度: 见 ②, 取 9000px/s。
constexpr double kTargetV = 9000.0;

constexpr double kDt = 1.0 / 120.0;

// ── 一个把时间推着走的喂帧助手 ──────────────────────────────────────────────
struct Feeder
{
    boss::AimTracker tr;
    double t = 10.0;   // 单调时钟(秒)

    void step(const boss::TrackBox& b, int cls = 0, double dt = kDt)
    {
        tr.beginFrame(dt, t);
        tr.offer(b, cls);
        tr.endFrame();
        t += dt;
    }
    void miss(double dt = kDt)
    {
        tr.beginFrame(dt, t);
        tr.endFrame();
        t += dt;
    }
};

// ═══ [1] 关联: 类别相等 + IoU 严格大于 ══════════════════════════════════════
void test_association()
{
    section("[1] 关联 = 类别相等 + IoU 严格大于门限");

    {
        boss::AimTracker tr;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        tr.configure(p);
        double t = 1.0;
        for (int i = 0; i < 6; ++i)
        {
            tr.beginFrame(kDt, t);
            tr.offer(box(400.0, 300.0), 7);
            tr.endFrame();
            t += kDt;
        }
        check(tr.tracks().size() == 1, "同类别静止目标只建一条轨迹");
    }
    {
        // ★ 同位置、不同类别 ⇒ 各建各的(类别是必要条件)。
        boss::AimTracker tr;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        tr.configure(p);
        tr.beginFrame(kDt, 1.0);
        tr.offer(box(400.0, 300.0), 1);
        tr.offer(box(400.0, 300.0), 2);
        tr.endFrame();
        check(tr.tracks().size() == 2, "★ 类别不等即使在同位置也不关联");
    }
    {
        // ★ IoU 恰好等于门限 ⇒ 不命中。两个完全相同的框 IoU = 1.0;
        //   门限设成 1.0 时 "1.0 < 1.0" 为假 ⇒ 不命中。
        boss::AimTracker tr;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.assoc_iou = 1.0;
        tr.configure(p);
        tr.beginFrame(kDt, 1.0);
        tr.offer(box(400.0, 300.0), 0);
        tr.endFrame();
        tr.beginFrame(kDt, 1.0 + kDt);
        tr.offer(box(400.0, 300.0), 0);
        tr.endFrame();
        check(tr.tracks().size() == 2,
              "★ IoU == 门限不算命中(AM 用严格大于) ⇒ 新建第二条");
    }
    {
        boss::AimTracker tr;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.assoc_iou = 0.99;
        tr.configure(p);
        tr.beginFrame(kDt, 1.0);
        tr.offer(box(400.0, 300.0), 0);
        tr.endFrame();
        tr.beginFrame(kDt, 1.0 + kDt);
        tr.offer(box(400.0, 300.0), 0);
        tr.endFrame();
        check(tr.tracks().size() == 1, "IoU > 门限时关联上(不新建)");
    }
    {
        // ★ 没有距离半径: 门限 0 时无重叠(IoU=0)仍不关联 —— AM 没有距离兜底。
        boss::AimTracker tr;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.assoc_iou = 0.0;
        tr.configure(p);
        tr.beginFrame(kDt, 1.0);
        tr.offer(box(100.0, 100.0), 0);
        tr.endFrame();
        tr.beginFrame(kDt, 1.0 + kDt);
        tr.offer(box(900.0, 700.0), 0);
        tr.endFrame();
        check(tr.tracks().size() == 2,
              "★ 无重叠时 IoU=0 不大于门限 0 ⇒ 新建(AM 没有距离兜底)");
    }
}

// ═══ [2] 生命周期: <= 的语义 ════════════════════════════════════════════════
void test_lifecycle()
{
    section("[2] 生命周期 (min_hits / max_age 的 <= 语义)");

    {
        // AM L511: max_age < misses 才删 ⇒ 漏 max_age 帧后【仍存活】。
        Feeder f;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.max_age = 3;
        f.tr.configure(p);
        f.step(box(400.0, 300.0));
        check(f.tr.tracks().size() == 1, "首次观测建立轨迹");

        f.miss();
        check(f.tr.tracks().size() == 1, "漏 1 帧: 存活");
        f.miss();
        check(f.tr.tracks().size() == 1, "漏 2 帧: 存活");
        f.miss();
        check(f.tr.tracks().size() == 1, "★ 漏 3 帧(= max_age): 仍存活(判据是严格 <)");
        f.miss();
        check(f.tr.tracks().empty(), "漏 4 帧(= max_age+1): 删除");
    }
    {
        // emits(): min_hits <= hits && misses <= max_age(两个都是 <=)。
        Feeder f;
        boss::AimTrackerParams p;
        p.min_hits = 3;
        p.max_age = 5;
        f.tr.configure(p);
        f.step(box(400.0, 300.0));
        const auto* t1 = f.tr.locked();
        check(t1 && t1->hits == 1, "第 1 帧 hits == 1");
        check(t1 && !f.tr.emits(*t1), "hits=1 < min_hits=3 ⇒ 还不够格输出");

        f.step(box(400.0, 300.0));
        f.step(box(400.0, 300.0));
        const auto* t3 = f.tr.locked();
        check(t3 && t3->hits == 3, "第 3 帧 hits == 3");
        check(t3 && f.tr.emits(*t3), "★ hits == min_hits 就够格(判据是 <=)");
    }
    {
        // ★ hits 是【累计】不是"连续": AM 命中 +1, 漏帧时【不清零】。
        Feeder f;
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.max_age = 10;
        f.tr.configure(p);
        f.step(box(400.0, 300.0));
        f.step(box(400.0, 300.0));
        const int hits_before = f.tr.locked() ? f.tr.locked()->hits : -1;
        f.miss();
        const auto* t = f.tr.locked();
        check(t && t->hits == hits_before,
              "★ 漏帧不把 hits 清零(AM 的 hits 是累计命中数)");
        check(t && t->misses == 1, "漏帧让 misses +1");
    }
}

// ═══ [3] 速度: 窗内沿用 / 窗外混合 ══════════════════════════════════════════
void test_velocity()
{
    section("[3] 速度估计 (窗内沿用旧值 / 窗外 0.25:0.75 混合)");

    {
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.vel_sample_ms = 20.0;
        Feeder f;
        f.tr.configure(p);

        double cx = 400.0;
        const double v = 600.0;                     // px/s
        const double per_frame = v * kDt;
        f.step(box(cx, 300.0));
        check(f.tr.locked() && !f.tr.locked()->vel_valid,
              "首帧没有上一次观测 ⇒ vel_valid 仍为假");

        cx += per_frame;
        f.step(box(cx, 300.0));                     // 8.33ms < 20ms ⇒ 沿用
        check_near(f.tr.locked() ? f.tr.locked()->vel_x : -1.0, 0.0, 1e-9,
                   "★ 窗内(8.33ms < 20ms)沿用旧速度 ⇒ 仍是 0");

        cx += per_frame;
        f.step(box(cx, 300.0));                     // 16.7ms < 20ms ⇒ 沿用
        check_near(f.tr.locked() ? f.tr.locked()->vel_x : -1.0, 0.0, 1e-9,
                   "窗内(16.7ms < 20ms)继续沿用");

        // 第 4 帧: 距上次刷新的时间戳已 25ms > 20ms ⇒ 重算。
        cx += per_frame;
        f.step(box(cx, 300.0));
        const double vel_after = f.tr.locked() ? f.tr.locked()->vel_x : -1.0;
        // = (15px/0.025s)*0.25 + 0*0.75 = 150 px/s
        check_near(vel_after, 150.0, 1.0,
                   "★ 窗外重算: (15px/0.025s)*0.25 + 0*0.75 = 150 px/s");
        check(f.tr.locked() && f.tr.locked()->vel_valid, "重算之后 vel_valid 置真");
    }
    {
        // ★ 混合是 0.25 新 + 0.75 旧(不是"累加后除窗"): 缓慢逼近真值。
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.vel_sample_ms = 1.0;   // 每帧都重算
        Feeder f;
        f.tr.configure(p);
        double cx = 400.0;
        const double per_frame = 600.0 * kDt;   // 5px
        double prev = 0.0;
        f.step(box(cx, 300.0));
        for (int i = 0; i < 8; ++i)
        {
            cx += per_frame;
            f.step(box(cx, 300.0));
            const double v = f.tr.locked() ? f.tr.locked()->vel_x : -1.0;
            if (i > 0)
            {
                const double expect = 600.0 * 0.25 + prev * 0.75;
                check_near(v, expect, 1.0, "混合递推 0.25 新 + 0.75 旧");
            }
            check(v >= prev - 1e-9, "速度单调逼近真值(不上冲)");
            check(v <= 600.0 + 1e-6, "★ 混合不会瞬时冲到真值(有 0.75 惯性)");
            prev = v;
        }
    }
}

// ═══ [4] 滑行外推 + 两轴衰减不对称 ══════════════════════════════════════════
void test_coasting()
{
    section("[4] 滑行外推 (位置 v*clamp(dt) + X*0.95 / Y*0.8)");

    boss::AimTrackerParams p;
    p.min_hits = 1;
    p.max_age = 20;
    p.vel_sample_ms = 1.0;
    Feeder f;
    f.tr.configure(p);

    double cx = 400.0, cy = 300.0;
    const double per = 600.0 * kDt;
    for (int i = 0; i < 40; ++i)
    {
        f.step(box(cx, cy));
        cx += per;
        cy += per;
    }
    const auto* t = f.tr.locked();
    check(t && t->vel_valid, "滑行前速度已有效");
    const double vx = t ? t->vel_x : 0.0;
    const double vy = t ? t->vel_y : 0.0;
    check(vx > 100.0 && vy > 100.0, "速度已逼近 600(两轴都非零)");

    const double last_cx = t ? t->out_cx : 0.0;
    const double last_cy = t ? t->out_cy : 0.0;
    f.miss();
    const auto* c1 = f.tr.locked();
    check(c1 != nullptr, "漏 1 帧后轨迹仍在(max_age=20)");
    if (c1)
    {
        check(c1->out_cx > last_cx + 1.0,
              "★ 滑行期位置按速度外推(不再原地不动)");
        check(c1->out_cy > last_cy + 1.0, "Y 轴同样外推");
        check_near(c1->vel_x, vx * 0.95, 1.0, "★ 滑行衰减 X = *0.95");
        check_near(c1->vel_y, vy * 0.8, 1.0, "★ 滑行衰减 Y = *0.8(与 X 不同!)");
        check(c1->vel_x > c1->vel_y - 1e-9,
              "★ X 衰减比 Y 慢 ⇒ 同初始速度下 X 速度更大(AM 原文如此)");
    }
    const double vx1 = c1 ? c1->vel_x : 0.0;
    f.miss();
    const auto* c2 = f.tr.locked();
    check(c2 && c2->vel_x < vx1, "继续滑行时速度继续衰减");
}

// ═══ [5] 尺寸权重: 吃框高 + 边界语义 ════════════════════════════════════════
void test_size_weight()
{
    section("[5] 尺寸权重 (吃【框高】; h<=min 时保持 1.0)");

    boss::AimTracker tr;
    boss::AimTrackerParams p;
    p.pred_min_w = 20.0;
    p.pred_max_w = 80.0;
    tr.configure(p);

    check_near(tr.sizeWeight(50.0), (80.0 - 50.0) / (80.0 - 20.0), 1e-12,
               "区间内: (max-h)/(max-min)");
    check_near(tr.sizeWeight(20.0), 1.0, 1e-12,
               "★ h == min: 保持 1.0(AM L109 的条件是严格 min < h)");
    check_near(tr.sizeWeight(10.0), 1.0, 1e-12,
               "★ h < min: 也保持 1.0(不是 0 —— 这是 AM 原文的形状)");
    check_near(tr.sizeWeight(80.0), 0.0, 1e-12, "h == max: 0(AM 的 else 支)");
    check_near(tr.sizeWeight(200.0), 0.0, 1e-12, "h > max: 0");
    check_near(tr.sizeWeight(79.0), (80.0 - 79.0) / (80.0 - 20.0), 1e-12,
               "接近 max 时权重趋 0");
    // ★ 边界语义对提前量的后果: 框高恰好 = max 时提前量恒 0。
    check_near(tr.sizeWeight(80.0) * 1000.0, 0.0, 1e-12,
               "★ 框高触及 max ⇒ 提前量被权重归零(尺寸门是真在起作用)");
}

// ═══ [6] 系数 FSM ═══════════════════════════════════════════════════════════
void test_factor_fsm()
{
    section("[6] 系数 FSM (涨 0.1 / 落 0.2 / 夹 [0,1])");

    // ★★ 注意: FSM 的推进发生在 predictionLead() 里 —— 它**既是查询也是步进**。
    //    所以夹具必须每帧调它一次, 否则系数永远停在 0, 断言会变成空转。
    //    (这正是 AM 的形状: FUN_14006e470 每帧被调一次, 顺带推进 track+0x18/0x1C。)
    auto run = [](double platform_vx, double target_v, int frames) {
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.max_age = 5000;
        p.vel_sample_ms = 1.0;
        p.pred_factor_x = 1.0;
        p.pred_factor_y = 1.0;
        Feeder f;
        f.tr.configure(p);
        double cx = 400.0;
        const double per = target_v * kDt;
        for (int i = 0; i < frames; ++i)
        {
            f.tr.setPlatformVelocity(platform_vx, platform_vx);
            f.step(box(cx, 300.0, kBoxW, kBoxH));
            cx += per;
            double lx = 0.0, ly = 0.0;
            f.tr.predictionLead(lx, ly);   // ★ 推进 FSM
        }
        return f.tr.locked() ? f.tr.locked()->pred_k_x : -1.0;
    };

    // ★ 涨的条件 = "目标位移够大 且 平台位移够大"(AM L133-135 是一条复合 if)。
    //   平台速度 0 ⇒ 永远算"自己没动" ⇒ 系数只落不涨 = 停在 0。
    const double k_no_platform = run(0.0, kTargetV, 300);
    check_near(k_no_platform, 0.0, 1e-9,
               "★ 平台速度为 0 时系数恒 0(AM: 自己没在动 ⇒ 只落不涨)");

    const double k_rising = run(kSelfV, kTargetV, 300);
    check(k_rising > 0.5, "★ 平台与目标都在动 ⇒ 系数爬升");

    const double k_10 = run(kSelfV, kTargetV, 10);
    check(k_10 > 0.3, "★ 爬升确实发生(10 帧已明显大于 0)");

    {
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.max_age = 5000;
        p.vel_sample_ms = 1.0;
        p.pred_factor_x = 1.0;
        Feeder f;
        f.tr.configure(p);
        double cx = 400.0;
        for (int i = 0; i < 300; ++i)   // 先爬满
        {
            f.tr.setPlatformVelocity(kSelfV, 0.0);
            f.step(box(cx, 300.0, kBoxW, kBoxH));
            cx += kTargetV * kDt;
            double lx = 0.0, ly = 0.0;
            f.tr.predictionLead(lx, ly);
        }
        const double k_max = f.tr.locked() ? f.tr.locked()->pred_k_x : -1.0;
        check(k_max > 0.9, "先爬到接近 1.0");

        for (int i = 0; i < 3; ++i)     // 目标定住 ⇒ 位移为 0 ⇒ 回落
        {
            f.tr.setPlatformVelocity(kSelfV, 0.0);
            f.step(box(cx, 300.0, kBoxW, kBoxH));
            double lx = 0.0, ly = 0.0;
            f.tr.predictionLead(lx, ly);
        }
        const double k_after3 = f.tr.locked() ? f.tr.locked()->pred_k_x : -1.0;
        check_near(k_max - k_after3, 0.6, 0.25, "★ 回落约 0.2/帧(3 帧约 -0.6)");
    }
    check(run(kSelfV, kTargetV, 100000) <= 1.0 + 1e-12, "系数不超过 1.0");
    check(run(0.0, 0.0, 100) >= -1e-12, "系数不低于 0.0");
}

// ═══ [7] 提前量 = 平台位移 * 系数(不是目标速度!) ═══════════════════════════
void test_lead_uses_platform_velocity()
{
    section("[7] 提前量用【平台位移】而不是目标速度");

    auto lead_for = [](double target_v, double platform_v) {
        boss::AimTrackerParams p;
        p.min_hits = 1;
        p.max_age = 5000;
        p.vel_sample_ms = 1.0;
        p.pred_factor_x = 1.0;
        Feeder f;
        f.tr.configure(p);
        double cx = 400.0;
        double last = 0.0;
        for (int i = 0; i < 400; ++i)
        {
            f.tr.setPlatformVelocity(platform_v, 0.0);
            f.step(box(cx, 300.0, kBoxW, kBoxH));
            cx += target_v * kDt;
            double lx = 0.0, ly = 0.0;
            f.tr.predictionLead(lx, ly);
            last = lx;
        }
        return last;
    };

    const double lead_fast_platform = lead_for(kTargetV, kSelfV);
    const double lead_slow_platform = lead_for(kTargetV, kSelfV * 0.5);

    check(std::abs(lead_fast_platform) > std::abs(lead_slow_platform) + 1.0,
          "★ 目标速度相同、平台速度更大 ⇒ 提前量更大(证明吃的是平台位移)");

    // ★★ 目标静止 + 平台在甩 ⇒ 提前量仍然是 0。这条断言一开始我写反了,
    //    以为"预测的是自己的滞后 ⇒ 目标不动也该有提前量"。逐行核对 AM L133-135
    //    后确认**不是**:
    //      uVar5 = 0x7FFFFFFF(符号位掩码, 即 |x|)
    //      if ( gate*(1+1.5k) <= |框位移|  ||  |自身位移| <= 10 )  →  落
    //      else                                                    →  涨
    //    ⇒ 涨 ⟺ |框位移| > gate*(1+1.5k)  **且**  |自身位移| > 10
    //    **框位移本身就在涨的条件里**, 所以目标静止时系数恒 0, 与平台无关。
    //    换句话说 AM 的语义是"目标自己动得快 **且** 我也在动"才预测 ——
    //    两个条件都是必要条件, 缺一不可。
    const double lead_target_still = lead_for(0.0, kSelfV);
    check_near(lead_target_still, 0.0, 1e-9,
               "★★ 目标静止时无论平台多快, 系数都涨不起来(框位移是涨的必要条件)");

    // ★ 平台不动、目标在动 ⇒ 提前量恒 0(自身位移是另一半必要条件)
    const double lead_platform_still = lead_for(20000.0, 0.0);
    check_near(lead_platform_still, 0.0, 1e-9,
               "★★ 目标猛动但平台不动 ⇒ 提前量恒 0(两个必要条件缺一不可)");
}

// ═══ [8] 就地改写框心 ═══════════════════════════════════════════════════════
void test_inplace_box_rewrite()
{
    section("[8] 落点 = 就地改写框心 (AM L177-178)");

    boss::AimTrackerParams p;
    p.min_hits = 1;
    p.max_age = 5000;
    p.vel_sample_ms = 1.0;
    p.pred_factor_x = 1.0;
    Feeder f;
    f.tr.configure(p);
    double cx = 400.0;
    for (int i = 0; i < 400; ++i)
    {
        f.tr.setPlatformVelocity(kSelfV, 0.0);
        f.step(box(cx, 300.0, kBoxW, kBoxH));
        cx += kTargetV * kDt;
    }
    double lx = 0.0, ly = 0.0;
    f.tr.predictionLead(lx, ly);
    const auto* t = f.tr.locked();
    check(t != nullptr, "轨迹存在");
    if (t)
    {
        const double base_cx = t->box.x + t->box.w * 0.5;
        check_near(t->out_cx, base_cx + lx, 1e-6,
                   "★ out_cx = 框心 + 提前量(就地改写)");
        check(std::abs(lx) > 1.0, "提前量确实非零(否则上一条断言是空转)");
    }
}

// ═══ [9] 两道自加阀默认关闭 ⇒ 默认与 AM 逐位一致 ═══════════════════════════
void test_self_added_valves_default_off()
{
    section("[9] 自加安全阀默认关闭");

    boss::AimTrackerParams p;
    check_near(p.pred_max_lead_px, 0.0, 1e-12,
               "★ 硬像素上限默认 0 = 无上限 = AM 原样");
    check_near(p.pred_vel_floor, 0.0, 1e-12,
               "★ 速度噪声门默认 0 = 无门 = AM 原样");
    check(p.enable_tracking, "跟踪默认开启");

    auto run = [](double cap, double floorv) {
        boss::AimTrackerParams q;
        q.min_hits = 1;
        q.max_age = 5000;
        q.vel_sample_ms = 1.0;
        q.pred_factor_x = 1.0;
        q.pred_max_lead_px = cap;
        q.pred_vel_floor = floorv;
        Feeder f;
        f.tr.configure(q);
        double cx = 400.0;
        double acc = 0.0;
        for (int i = 0; i < 400; ++i)
        {
            f.tr.setPlatformVelocity(kSelfV, 0.0);
            f.step(box(cx, 300.0, kBoxW, kBoxH));
            cx += kTargetV * kDt;
            double lx = 0.0, ly = 0.0;
            f.tr.predictionLead(lx, ly);
            acc += lx;
        }
        return acc;
    };
    check_near(run(0.0, 0.0), run(0.0, 0.0), 1e-12, "确定性(同输入同输出)");

    // ★ 反向验证: 把上限打开必须【改变】结果, 否则这条阀是空转的。
    check(std::abs(run(2.0, 0.0)) < std::abs(run(0.0, 0.0)) - 1e-6,
          "★ 打开硬上限后累计提前量必须变小(阀真的在起作用)");
}

// ═══ [9.5] 滑行外推必须【被消费】—— 否则它就是死代码 ═══════════════════════
//
// ★ 这一节的存在理由: `out_cx/out_cy` 是 AM 滑行外推的落点, 但"算出来"不等于
//   "用上了"。boss_aim.cpp 里必须把它真的加到瞄点上, 否则外推是空转。
//   本层测不了 boss_aim(它依赖 OpenCV), 所以这里至少钉住【外推量本身是有效的、
//   可区分的】: 滑行期的 out_cx 必须显著偏离最后一次观测的框心。
void test_coast_offset_is_meaningful()
{
    section("[9.5] 滑行外推的偏移量确实可区分(供 boss_aim 消费)");

    boss::AimTrackerParams p;
    p.min_hits = 1;
    p.max_age = 30;
    p.vel_sample_ms = 1.0;
    Feeder f;
    f.tr.configure(p);

    // 建立一条向东匀速的轨迹。
    double cx = 400.0;
    const double per = 900.0 * kDt;      // 900 px/s ⇒ 7.5px/帧
    for (int i = 0; i < 60; ++i)
    {
        f.step(box(cx, 300.0, 200.0, 60.0));
        cx += per;
    }
    const auto* t = f.tr.locked();
    check(t && t->vel_valid, "滑行前速度有效");

    // 漏一帧: 位置必须外推出去, 并且偏移量足够大(能被下游看见)。
    f.miss();
    const auto* c = f.tr.locked();
    check(c != nullptr, "漏 1 帧后轨迹仍在");
    if (c)
    {
        const double obs_cx_now = c->box.x + c->box.w * 0.5;
        const double coast_dx = c->out_cx - obs_cx_now;
        const double expect = std::abs(c->vel_x) * kDt;
        check(std::abs(coast_dx) > 1.0,
              "★ 滑行偏移必须 > 1px(否则下游消费了也看不出来)");
        check_near(std::abs(coast_dx), expect, expect * 0.2 + 1e-9,
                   "★ 滑行偏移 = |速度| × dt(与 AM 的 v*clamp(dt) 一致)");
        check(c->out_cx > obs_cx_now,
              "向东运动的轨迹, 外推位置必须在观测位置的东侧");
    }

    // ★ 静止目标: 滑行偏移必须是 0(不能凭空造出位移)。
    {
        boss::AimTrackerParams q;
        q.min_hits = 1;
        q.max_age = 30;
        Feeder g;
        g.tr.configure(q);
        for (int i = 0; i < 60; ++i)
            g.step(box(400.0, 300.0, 200.0, 60.0));
        g.miss();
        const auto* s = g.tr.locked();
        check(s != nullptr, "静止目标漏帧后轨迹仍在");
        if (s)
        {
            const double obs_cx = s->box.x + s->box.w * 0.5;
            check_near(s->out_cx - obs_cx, 0.0, 1e-9,
                       "★ 静止目标的滑行偏移恒为 0(不凭空造位移)");
        }
    }
}

// ═══ [10] 常量真值表 ════════════════════════════════════════════════════════
void test_constants()
{
    section("[10] 常量真值 (防再次抄错)");

    // ★ 这三条是本轮复核抓出来的真错误, 必须钉死。
    check_near(boss::kPredGateSizeRatioAlt, 0.8, 1e-12,
               "★★ 尺寸系数真值 0.8(此前本项目写 0.75 —— 错)");
    check_near(boss::kPredSelfMoveGatePx, 10.0, 1e-12,
               "★★ 平台位移门真值 10.0(此前本项目写 3.5057 —— 错)");
    check_near(boss::kPredNormalBlend, 0.05, 1e-12,
               "★★ 正常低通混合 0.05(此前写成 1.0 直通 —— 反了)");

    check_near(boss::kPredFactorRiseStep, 0.1, 1e-12, "爬升 0.1");
    check_near(boss::kPredFactorFallStep, 0.2, 1e-12, "回落 0.2");
    check_near(boss::kPredFactorMax, 1.0, 1e-12, "上限 1.0");
    check_near(boss::kPredGateSizeRatio, 1.5, 1e-12, "门限系数 1.5");
    check_near(boss::kPredGateMinPx, 30.0, 1e-12, "门限下限 30px");
    check_near(boss::kPredFlipBlend, 0.02, 1e-12, "翻转混合 0.02");
    check_near(boss::kVelBlendNew, 0.25, 1e-12, "速度混合 0.25");
    check_near(boss::kVelBlendOld, 0.75, 1e-12, "速度混合 0.75");
    check_near(boss::kVelBlendNew + boss::kVelBlendOld, 1.0, 1e-12,
               "两个混合系数和为 1.0(不是漏掉归一)");
    check_near(boss::kCoastDecayX, 0.95, 1e-12, "滑行衰减 X 0.95");
    check_near(boss::kCoastDecayY, 0.8, 1e-12, "滑行衰减 Y 0.8");
    check_near(boss::kDefaultAssocIou, 0.3, 1e-12,
               "默认 IoU 门限 0.3(AM 的 tracking_iou_threshold)");
    check_near(boss::kDefaultVelSampleMs, 20.0, 1e-12,
               "默认速度采样窗 20ms(AM 的 tracking_velocity_sample_ms)");

    boss::AimTrackerParams p;
    check(p.min_hits == 3, "默认 min_hits = 3");
    check(p.max_age == 5, "默认 max_age = 5");
    check_near(p.pred_min_w, 20.0, 1e-12, "默认预测尺寸下限 20");
    check_near(p.pred_max_w, 80.0, 1e-12, "默认预测尺寸上限 80");

    // ★ 两个混合系数都是小量 ⇒ 输出是重低通。这条断言防止有人把 0.05 改成 1.0。
    check(boss::kPredNormalBlend < 0.2 && boss::kPredFlipBlend < 0.2,
          "★ 两个低通混合系数都是小量(正常 0.05 / 翻转 0.02)");
}

}  // namespace

int main()
{
    std::printf("=== aim_tracker_test (AimMagic 1.0.30 逐字移植回归) ===\n");

    test_association();
    test_lifecycle();
    test_velocity();
    test_coasting();
    test_size_weight();
    test_factor_fsm();
    test_lead_uses_platform_velocity();
    test_inplace_box_rewrite();
    test_self_added_valves_default_off();
    test_coast_offset_is_meaningful();
    test_constants();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
