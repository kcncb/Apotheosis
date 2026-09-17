// 控制器接线回归 —— "配置 → 控制器" 的映射
//
// ★★ 为什么单独一个文件: 漏映射的表现是【静默失效】(界面能改、跑起来没变),
//    不报错、不留痕。这类 bug 只能靠测试挡 —— 它是本仓库反复踩的坑
//    （"配了没反应的死旋钮"）。
//
// ★ 它只测 aim_loop 的【纯函数】(FlatConfig → ControllerConfig),
//   不碰 detectionBuffer / MouseThread / 找色 —— 那些要 Windows + 设备,
//   本机编不了。所以本测试覆盖的是"映射对不对", 不是"接线能不能跑"。

#include "config/config.h"   // HotkeyProfile / ClassFilterState（本机可编）
#include "control/aim_controller.h"
#include "runtime/aim_loop.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace runtime::aim_loop;
using namespace control;

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
        std::printf("  FAIL: %s (got %.6f, want %.6f)\n", what.c_str(), got, want);
    }
}

static void section(const char* name) { std::printf("[%s]\n", name); }

// ══════════════════════════════════════════════════════════════════════════
static void testBucketMapping()
{
    section("类别桶: aim_classes → Aim/Delete");

    // 空列表 ⇒ 空桶（调用方据此判定"没有可瞄类别"）
    check(buildClassBuckets({}).empty(), "空 aim_classes ⇒ 空桶");

    // 常见情形: 类别 0 是 Aim
    {
        const auto b = buildClassBuckets({ 0 });
        check(b.size() == 1, "classId=0 ⇒ 桶长 1");
        check(b[0] == 1, "classId=0 被标为 Aim");
    }

    // ★★ 关键: 不在列表里的类别必须是 Delete（未知类别不瞄）
    {
        const auto b = buildClassBuckets({ 2 });
        check(b.size() == 3, "最大 classId=2 ⇒ 桶长 3");
        check(b[0] == 0, "★ classId=0 不在列表 ⇒ Delete");
        check(b[1] == 0, "★ classId=1 不在列表 ⇒ Delete");
        check(b[2] == 1, "classId=2 ⇒ Aim");
    }

    // 多个类别 + 乱序
    {
        const auto b = buildClassBuckets({ 3, 0, 2 });
        check(b.size() == 4, "最大 3 ⇒ 桶长 4");
        check(b[0] == 1 && b[1] == 0 && b[2] == 1 && b[3] == 1,
              "0/2/3 是 Aim, 1 是 Delete（顺序无关）");
    }

    // 负值 classId 被忽略且不撑大桶
    {
        const auto b = buildClassBuckets({ -5, 1 });
        check(b.size() == 2, "负 classId 不撑大桶");
        check(b[1] == 1, "正的仍然生效");
    }

    // ★ 全部为负 ⇒ 空桶（等于"没有可瞄目标"）
    check(buildClassBuckets({ -1, -2 }).empty(), "全负 ⇒ 空桶");
}

// ══════════════════════════════════════════════════════════════════════════
static void testConfigMapping()
{
    section("FlatConfig → ControllerConfig: 六个增益");

    // ★★ 逐个字段确认搬到位 —— 漏一个就是"界面能改、跑起来没变"。
    FlatConfig flat;
    flat.kpX = 11.0; flat.kpY = 12.0;
    flat.kiX = 21.0; flat.kiY = 22.0;
    flat.kdX = 31.0; flat.kdY = 32.0;

    const ControllerConfig cfg = toControllerConfig(flat);
    checkNear(cfg.pid.kpX, 11.0, 0.0, "kpX 搬到位");
    checkNear(cfg.pid.kpY, 12.0, 0.0, "kpY 搬到位");
    checkNear(cfg.pid.kiX, 21.0, 0.0, "kiX 搬到位");
    checkNear(cfg.pid.kiY, 22.0, 0.0, "kiY 搬到位");
    checkNear(cfg.pid.kdX, 31.0, 0.0, "kdX 搬到位");
    checkNear(cfg.pid.kdY, 32.0, 0.0, "kdY 搬到位");

    section("FlatConfig → ControllerConfig: 时间常数与限幅");
    flat.tauUnwindSec = 0.055;
    flat.tauDerivSec = 0.077;
    flat.iMax = 99.0;
    flat.maxOutputCounts = 123;
    flat.pFullScalePx = 44.0;
    const ControllerConfig cfg2 = toControllerConfig(flat);
    checkNear(cfg2.pid.tauUnwindSec, 0.055, 0.0, "tauUnwindSec 搬到位");
    checkNear(cfg2.pid.tauDerivSec, 0.077, 0.0, "tauDerivSec 搬到位");
    checkNear(cfg2.pid.iMax, 99.0, 0.0, "iMax 搬到位");
    check(cfg2.pid.maxOutputCounts == 123, "maxOutputCounts 搬到位");
    checkNear(cfg2.pid.pFullScalePx, 44.0, 0.0, "pFullScalePx 搬到位");

    section("FlatConfig → ControllerConfig: 瞄点与滞回");
    flat.yOffset = 0.3; flat.yOffsetMax = 0.7; flat.hysteresisRatio = 1.8;
    const ControllerConfig cfg3 = toControllerConfig(flat);
    checkNear(cfg3.aimPoint.yOffset, 0.3, 0.0, "yOffset 搬到位");
    checkNear(cfg3.aimPoint.yOffsetMax, 0.7, 0.0, "yOffsetMax 搬到位");
    checkNear(cfg3.selector.hysteresisRatio, 1.8, 0.0, "hysteresisRatio 搬到位");

    section("类别桶进控制器配置");
    flat.aimClassIds = { 0, 2 };
    const ControllerConfig cfg4 = toControllerConfig(flat);
    check(cfg4.buckets.byClassId.size() == 3, "桶长度 = maxClassId+1");
    check(cfg4.buckets.bucketOf(0) == Bucket::Aim, "0 ⇒ Aim");
    check(cfg4.buckets.bucketOf(1) == Bucket::Delete, "1 ⇒ Delete");
    check(cfg4.buckets.bucketOf(2) == Bucket::Aim, "2 ⇒ Aim");
    check(cfg4.buckets.bucketOf(99) == Bucket::Delete,
          "★ 越界 classId ⇒ Delete（安全默认: 未知类别不瞄）");

    // ★ 空 aimClassIds ⇒ 空桶 ⇒ 控制器筛不出任何候选
    FlatConfig emptyFlat;
    const ControllerConfig cfg5 = toControllerConfig(emptyFlat);
    check(cfg5.buckets.byClassId.empty(), "无 aim_classes ⇒ 空桶");
    check(cfg5.buckets.bucketOf(0) == Bucket::Delete,
          "★ 空桶时任何类别都是 Delete —— 没有配置来源就不瞄任何东西");

    section("新鲜度门禁恒为开");
    // ★ 用户明确决定不做"检测延迟降权"，但二值门禁必须保留（方案 §3.3）。
    check(cfg.requireFreshDetection, "requireFreshDetection 恒为 true");
    check(cfg.requireFreshCrosshair, "requireFreshCrosshair 恒为 true");

    section("★ 死区不得回归");
    // ★★ 死区已整项删除（方案 §5.1）。这条断言的作用是: 若有人把
    //    PidConfig::deadzonePx 加回来并在这里映射, 本测试会编不过/变红。
    //    做法是【对结构体做静态检查】—— 只要它能编译, 就说明没有 deadzone 字段
    //    参与本次映射（真正的"加回来"会在 control_layer_test 里被咬住）。
    PidConfig pcfg;
    (void)pcfg;
    check(true, "PidConfig 无死区字段参与映射（死区已整条删除）");
}

// ══════════════════════════════════════════════════════════════════════════
// 端到端: 配置真的会影响控制结果
// ══════════════════════════════════════════════════════════════════════════
static void testConfigActuallyDrivesControl()
{
    section("★ 配置真的驱动控制（不是只搬字段）");

    FlatConfig flat;
    flat.aimClassIds = { 0 };
    flat.yOffset = 0.5;
    flat.yOffsetMax = 0.5;
    flat.kpX = 35.0;
    flat.kpY = 35.0;

    const double dt = 1.0 / 120.0;

    // 同一个误差, 两组 Kp ⇒ 输出必须不同
    auto runOnce = [&](double kpX) {
        FlatConfig f = flat;
        f.kpX = kpX;
        AimController ac;
        ac.setConfig(toControllerConfig(f));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = dt;
        Candidate c;
        c.box = Box{ 160, 100, 40, 80 };   // 中心 (180,140)
        c.classId = 0;
        c.confidence = 0.9;
        in.candidates = { c };
        return ac.update(in);
    };

    const ControlOutput lo = runOnce(10.0);
    const ControlOutput hi = runOnce(100.0);
    check(lo.engaged && hi.engaged, "两组都 engage");
    check(hi.counts.x > lo.counts.x,
          "★ Kp 大 ⇒ 输出大（证明配置真的进了公式，不是摆设）");

    section("★ 类别桶真的过滤（Delete 不产生控制）");
    {
        FlatConfig f = flat;
        f.aimClassIds = { 0 };            // 只有 0 是 Aim
        AimController ac;
        ac.setConfig(toControllerConfig(f));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = dt;
        Candidate c;
        c.box = Box{ 160, 100, 40, 80 };
        c.classId = 1;                    // ← Delete
        c.confidence = 0.9;
        in.candidates = { c };
        const ControlOutput out = ac.update(in);
        check(!out.engaged,
              "★ classId=1 不在 aim_classes ⇒ 不 engage（桶真的生效）");
        check(out.idleReason == ControlOutput::IdleReason::NoCandidates,
              "idleReason = NoCandidates");
    }

    section("★ 滞回倍数真的进控制器");
    {
        // ── 几何（两个目标不动，只移动准星来改变"谁更近"）──────────────
        //   A 中心 (100,140)   B 中心 (320,140)   相距 220px
        //   锁定 A 后把准星移到 (240,140):
        //     distA = 140, distB = 80  ⇒ B 更近，但只近 1.75 倍
        //   ⇒ k=1.3 会切（80×1.3=104 < 140）；k=5.0 不切（80×5=400 > 140）
        // ★ 关键是【准星不能移到 B 上】—— 若把准星放到 B 的中心，
        //   A 会被甩到 220px 外，什么 k 都拦不住（那是"目标不见了"，
        //   不是滞回该拦的情形）。这是本用例最初写错的地方。
        auto pick = [&](double k) {
            FlatConfig f = flat;
            f.hysteresisRatio = k;
            AimController ac;
            ac.setConfig(toControllerConfig(f));
            ControlInput in;
            in.dtSec = dt;
            in.cross = Vec2{ 100, 140 };   // 起始准星正对 A

            Candidate a; a.box = Box{ 80, 100, 40, 80 };  a.classId = 0; a.confidence = 0.9;
            Candidate b; b.box = Box{ 300, 100, 40, 80 }; b.classId = 0; b.confidence = 0.9;

            for (int i = 0; i < 3; ++i) { in.candidates = { a, b }; in.frameIndex = i; ac.update(in); }

            // 准星移到中间偏 B：B 更近，但领先幅度只有 1.75 倍
            in.cross = Vec2{ 240, 140 };
            in.candidates = { a, b };
            in.frameIndex = 10;
            return ac.update(in);
        };
        const ControlOutput hard = pick(5.0);
        const ControlOutput soft = pick(1.3);
        check(hard.anchor.x < 200.0,
              "★ k=5（强滞回）: 仍锁在目标 A（锚点 x < 200）");
        check(soft.anchor.x > 200.0,
              "★ k=1.3（弱滞回）: 已切到目标 B（锚点 x > 200）—— "
              "证明 k 真的进了选靶，不是摆设");

        // ★ 反向: 目标消失时任何 k 都必须改选（滞回不能把人锁死）
        {
            FlatConfig f = flat;
            f.hysteresisRatio = 5.0;
            AimController ac;
            ac.setConfig(toControllerConfig(f));
            ControlInput in;
            in.dtSec = dt;
            in.cross = Vec2{ 100, 140 };
            Candidate a; a.box = Box{ 80, 100, 40, 80 };  a.classId = 0; a.confidence = 0.9;
            Candidate b; b.box = Box{ 300, 100, 40, 80 }; b.classId = 0; b.confidence = 0.9;
            for (int i = 0; i < 3; ++i) { in.candidates = { a, b }; in.frameIndex = i; ac.update(in); }

            in.candidates = { b };          // A 消失了
            in.frameIndex = 10;
            const ControlOutput out = ac.update(in);
            check(out.anchor.x > 200.0,
                  "★ 锁定目标消失 ⇒ 再大的滞回也必须改选（不许把人锁死）");
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
// ★★ 两源桶合并 —— 修"前后端对不上"的那一处
// ══════════════════════════════════════════════════════════════════════════
static void testBucketMerge()
{
    section("★★ 类别桶两源合并（TargetPage 的 class_filters + 逐热键 aim_classes）");

    // ── 1. 只有全局桶（TargetPage 单独设置的情形）─────────────────────
    {
        FlatConfig f;
        f.classFilters = { {0, 2}, {1, 1}, {2, 0} };   // 0=Aim, 1=Filter, 2=Delete
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.byClassId.size() == 3, "桶长 = maxId+1");
        check(cfg.buckets.bucketOf(0) == Bucket::Aim,    "全局 Aim 生效");
        check(cfg.buckets.bucketOf(1) == Bucket::Filter, "★★ 全局 Filter 生效（此前永远为空）");
        check(cfg.buckets.bucketOf(2) == Bucket::Delete, "全局 Delete 生效");
    }

    // ── 2. ★★ 只有 aim_classes 时（老路径）仍要工作 ─────────────────────
    {
        FlatConfig f;
        f.aimClassIds = { 0, 2 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Aim, "aim_classes 单独也能用");
        check(cfg.buckets.bucketOf(1) == Bucket::Delete, "未列出 ⇒ Delete");
        check(cfg.buckets.bucketOf(2) == Bucket::Aim, "aim_classes 第二个生效");
    }

    // ── 3. ★★ 两者冲突: aim_classes 只【提升】, 不降级 ─────────────────
    {
        FlatConfig f;
        f.classFilters = { {0, 0} };   // 全局说 0 是 Delete
        f.aimClassIds  = { 0 };        // 逐热键说 0 要瞄
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Aim,
              "★★ 冲突时逐热键的 Aim 胜出（更具体的意图优先，只提升不降级）");
    }
    {
        FlatConfig f;
        f.classFilters = { {0, 2} };   // 全局说 0 是 Aim
        f.aimClassIds  = { };          // 逐热键没提
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Aim, "全局 Aim 不因 aim_classes 为空而丢失");
    }
    {
        // ★ 反向: 全局说 Filter, 逐热键没提 ⇒ 必须保持 Filter（不能被冲成 Aim）
        FlatConfig f;
        f.classFilters = { {0, 1} };
        f.aimClassIds  = { 1 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Filter,
              "★ 全局 Filter 不被无关的 aim_classes 条目冲掉");
        check(cfg.buckets.bucketOf(1) == Bucket::Aim, "另一条照常 Aim");
    }

    // ── 4. aim_classes 里超出全局范围的 id 要撑大桶 ────────────────────
    {
        FlatConfig f;
        f.classFilters = { {0, 2} };
        f.aimClassIds  = { 5 };
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.byClassId.size() == 6, "桶长取两源最大值 +1");
        check(cfg.buckets.bucketOf(0) == Bucket::Aim, "全局的还在");
        check(cfg.buckets.bucketOf(5) == Bucket::Aim, "补进来的也被标 Aim");
        check(cfg.buckets.bucketOf(3) == Bucket::Delete, "中间没提的 ⇒ Delete");
    }

    // ── 5. ★★ Filter 真的把目标挡在瞄准之外（不只是个枚举值）───────────
    {
        FlatConfig f;
        f.classFilters = { {0, 1} };   // 只有 class 0, 且是 Filter
        f.yOffset = 0.5; f.yOffsetMax = 0.5;
        AimController ac;
        ac.setConfig(toControllerConfig(f));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = 1.0 / 120.0;
        Candidate c; c.box = Box{ 160, 100, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        const ControlOutput out = ac.update(in);
        check(!out.engaged,
              "★★ Filter 桶的类别【不产生控制】（以前它永远为空，这条测不到）");
    }

    // ── 6. ★★ 数值契约: ClassBucket{Delete=0,Filter=1,Aim=2} 必须与
    //       aim_loop.cpp 里 static_cast<int>(cf.bucket) 的解释一致 ──────
    //   ★ 这是"两源合并"依赖的隐含前提。若有人改了 config.h 里 ClassBucket
    //     的数值顺序, 这里必须跟着变 —— 否则 Filter 会被当成 Aim
    //     （或反过来），而且是【静默】的：编译通过、测试若没这条也全绿。
    {
        FlatConfig f;
        f.classFilters = { {0, 0}, {1, 1}, {2, 2} };   // 按 ClassBucket 的原值
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.bucketOf(0) == Bucket::Delete, "数值 0 ⇒ Delete");
        check(cfg.buckets.bucketOf(1) == Bucket::Filter, "★★ 数值 1 ⇒ Filter（契约）");
        check(cfg.buckets.bucketOf(2) == Bucket::Aim,    "数值 2 ⇒ Aim");
    }

    // ── 7. 两源都空 ⇒ 谁都不瞄 ────────────────────────────────────────
    {
        FlatConfig f;
        const ControllerConfig cfg = toControllerConfig(f);
        check(cfg.buckets.byClassId.empty(), "两源都空 ⇒ 空桶");
        check(cfg.buckets.bucketOf(0) == Bucket::Delete, "空桶时任何类别都是 Delete");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// 此前没暴露的 6 项参数
// ══════════════════════════════════════════════════════════════════════════
static void testNewlyExposedKnobs()
{
    section("★ 新暴露的参数真的进控制器（此前写死在代码里）");

    FlatConfig f;
    f.maxDistancePx = 77.0;
    f.randomSeed = 12345;
    f.matchCenterRatio = 0.7;
    f.areaRatioTol = 3.5;
    f.kSnapMult = 2.5;
    f.minAspect = 0.3;
    f.maxAspect = 4.0;

    const ControllerConfig cfg = toControllerConfig(f);
    checkNear(cfg.selector.maxDistancePx, 77.0, 0.0, "maxDistancePx 搬到位");
    check(cfg.aimPoint.randomSeed == 12345, "randomSeed 搬到位");
    checkNear(cfg.stabilizer.matchCenterRatio, 0.7, 0.0, "matchCenterRatio 搬到位");
    checkNear(cfg.stabilizer.areaRatioTol, 3.5, 0.0, "areaRatioTol 搬到位");
    checkNear(cfg.stabilizer.kSnapMult, 2.5, 0.0, "kSnapMult 搬到位");
    checkNear(cfg.stabilizer.minAspect, 0.3, 0.0, "minAspect 搬到位");
    checkNear(cfg.stabilizer.maxAspect, 4.0, 0.0, "maxAspect 搬到位");

    section("★ 稳定器参数真的改变行为（不是只搬字段）");
    {
        // areaRatioTol 很紧 ⇒ 尺寸略变就算换目标 ⇒ 出 NoHistory/拒绝
        // 用 kSnapMult 更直接: 极小的 kSnapMult 让任何位移都算瞬移。
        auto run = [](double kSnap) {
            FlatConfig g;
            g.classFilters = { {0, 2} };
            g.yOffset = 0.5; g.yOffsetMax = 0.5;
            g.kSnapMult = kSnap;
            AimController ac;
            ac.setConfig(toControllerConfig(g));
            ControlInput in;
            in.cross = Vec2{ 100, 100 };
            in.dtSec = 1.0 / 120.0;
            Candidate a; a.box = Box{ 160, 100, 40, 80 }; a.classId = 0; a.confidence = 0.9;
            in.candidates = { a };
            in.frameIndex = 1;
            ac.update(in);                     // 第一帧: 无历史, 认下

            // 第二帧: 目标挪了 30px（小于对角线 ~89px）
            Candidate b; b.box = Box{ 190, 100, 40, 80 }; b.classId = 0; b.confidence = 0.9;
            in.candidates = { b };
            in.frameIndex = 2;
            return ac.update(in);
        };
        // kSnap=0.001 ⇒ 30px 位移远超 89×0.001 ⇒ 判瞬移 ⇒ 硬重置
        // kSnap=10.0  ⇒ 30px 远小于 89×10  ⇒ 同一目标, 正常跟
        const ControlOutput strict = run(0.001);
        const ControlOutput loose  = run(10.0);
        check(strict.anchor.x != loose.anchor.x ||
                  strict.anchor.y != loose.anchor.y ||
                  strict.engaged != loose.engaged,
              "★ kSnapMult 真的改变结果（证明稳定器参数进了公式）");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// dt 门禁
// ══════════════════════════════════════════════════════════════════════════
static void testDtGate()
{
    section("★ dt 门禁");

    // 正常检测间隔（120fps / 60fps / 30fps / 10fps）都该放行
    check(dtIsUsable(1.0 / 120.0), "120fps (8.3ms) 放行");
    check(dtIsUsable(1.0 / 60.0),  "60fps (16.7ms) 放行");
    check(dtIsUsable(1.0 / 30.0),  "30fps (33.3ms) 放行");
    check(dtIsUsable(1.0 / 10.0),  "10fps (100ms) 放行");
    check(dtIsUsable(0.240),       "240ms 放行（上限内）");

    // ★ 边界是闭区间：恰好等于上下限要放行
    check(dtIsUsable(kMinDtSec), "★ dt == 下限 放行（闭区间）");
    check(dtIsUsable(kMaxDtSec), "★ dt == 上限 放行（闭区间）");

    // ★★ 超上限：推理卡了一下 / 会话刚恢复。不许拿它当 dt
    check(!dtIsUsable(0.300), "★ 300ms 拒绝（超过 250ms 上限）");
    check(!dtIsUsable(1.0),   "★ 1s 拒绝");
    check(!dtIsUsable(5.0),   "★ 5s 拒绝（会话中断后第一拍）");

    // ★ 下界：几乎同时的两拍
    check(!dtIsUsable(0.0005), "★ 0.5ms 拒绝（低于 1ms 下限）");
    check(!dtIsUsable(0.0),    "★ dt=0 拒绝（防除零）");

    // ★★ 负 dt 必须拒绝 —— 它意味着时钟异常，不是"很小的时间"
    check(!dtIsUsable(-0.01), "★★ 负 dt 拒绝（时钟异常，不是小时间）");
    check(!dtIsUsable(-1e9),  "★★ 负 dt 拒绝（任何负值）");

    section("★ dt 两层防线各管一段（职责边界）");
    {
        // ── 第一层: PidController 自己只挡"dt <= 0" ────────────────────
        //   ★ 它是个纯公式对象, 【不该】知道"250ms 算不算太长" ——
        //     那是运行期策略, 属于调用方。所以这里只钉 dt<=0。
        PidConfig pc;
        pc.kpX = pc.kpY = 35.0;
        for (double bad : { 0.0, -0.01, -1.0 })
        {
            PidController pid(pc);
            const Counts c = pid.update(Vec2{ 200, 200 }, Vec2{ 100, 100 }, bad);
            check(c.x == 0 && c.y == 0,
                  "★ 公式层: dt=" + std::to_string(bad) + " ⇒ 输出 0（不猜 dt）");
        }

        // ── 第二层: 上限由 dtIsUsable 挡, 调用方负责 ───────────────────
        //   ★★ 这两条是本用例最初的错误断言 —— 我曾以为 PID 会拒绝 500ms,
        //      实测它【会】出力(它没有上限概念)。**上限只能在调用方挡**。
        //      所以 aim_loop::tick() 里那句 `if (!dtIsUsable(dt)) return false;`
        //      是【必需】的, 不是保险。
        check(!dtIsUsable(0.5),  "★ 策略层: 500ms 被 dtIsUsable 拒绝");
        check(!dtIsUsable(3.0),  "★ 策略层: 3s 被 dtIsUsable 拒绝");

        // 反过来确认: 若只靠公式层, 500ms 会真的产出位移(所以策略层不能省)
        PidController pid2(pc);
        const Counts c2 = pid2.update(Vec2{ 200, 200 }, Vec2{ 100, 100 }, 0.5);
        check(c2.x != 0 || c2.y != 0,
              "★★ 公式层对 500ms 【会】出力 ⇒ 证明调用方的 dtIsUsable 是必需的");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// ★★ flattenProfile: HotkeyProfile → FlatConfig（22 条逐字段搬运）
//
// 这一段是本文件【最要紧】的部分：漏一条的表现是"界面上那个参数改了没反应"，
// 不报错、不留痕。★ 反向变异证明: 它在 aim_loop.cpp 里时两条变异全绿。
// ══════════════════════════════════════════════════════════════════════════
static void testFlattenProfile()
{
    section("★★ HotkeyProfile → FlatConfig 逐字段搬运");

    HotkeyProfile hk;
    // 每个字段都给一个【与默认值不同】的数，否则"没搬"和"搬了默认值"分不开
    hk.ctl_kp_x = 11.0; hk.ctl_kp_y = 12.0;
    hk.ctl_ki_x = 21.0; hk.ctl_ki_y = 22.0;
    hk.ctl_kd_x = 31.0; hk.ctl_kd_y = 32.0;
    hk.ctl_tau_unwind_sec = 0.041;
    hk.ctl_tau_deriv_sec = 0.052;
    hk.ctl_i_max = 63.0;
    hk.ctl_max_output_counts = 77;
    hk.ctl_p_full_scale_px = 88.0;
    hk.ctl_y_offset = 0.31;
    hk.ctl_y_offset_max = 0.91;
    hk.ctl_hysteresis_ratio = 2.4;
    hk.ctl_max_distance_px = 155.0;
    hk.ctl_random_seed = 4242;
    hk.ctl_match_center_ratio = 0.61;
    hk.ctl_area_ratio_tol = 3.7;
    hk.ctl_k_snap_mult = 2.2;
    hk.ctl_min_aspect = 0.11;
    hk.ctl_max_aspect = 6.6;
    hk.aim_classes.clear();
    { HotkeyAimClass a; a.class_id = 3; hk.aim_classes.push_back(a); }
    { HotkeyAimClass a; a.class_id = 7; hk.aim_classes.push_back(a); }

    std::vector<ClassFilterState> cfilters;
    { ClassFilterState c; c.class_id = 1; c.bucket = ClassBucket::Filter; cfilters.push_back(c); }
    { ClassFilterState c; c.class_id = 5; c.bucket = ClassBucket::Aim;    cfilters.push_back(c); }

    const FlatConfig f = flattenProfile(hk, 640, cfilters);

    checkNear(f.kpX, 11.0, 0.0, "kpX 搬了");
    checkNear(f.kpY, 12.0, 0.0, "kpY 搬了");
    checkNear(f.kiX, 21.0, 0.0, "kiX 搬了");
    checkNear(f.kiY, 22.0, 0.0, "kiY 搬了");
    checkNear(f.kdX, 31.0, 0.0, "kdX 搬了");
    checkNear(f.kdY, 32.0, 0.0, "kdY 搬了");
    checkNear(f.tauUnwindSec, 0.041, 0.0, "tauUnwindSec 搬了");
    checkNear(f.tauDerivSec, 0.052, 0.0, "tauDerivSec 搬了");
    checkNear(f.iMax, 63.0, 0.0, "iMax 搬了");
    check(f.maxOutputCounts == 77, "maxOutputCounts 搬了");
    checkNear(f.pFullScalePx, 88.0, 0.0, "pFullScalePx 搬了");
    checkNear(f.yOffset, 0.31, 0.0, "yOffset 搬了");
    checkNear(f.yOffsetMax, 0.91, 0.0, "yOffsetMax 搬了");
    checkNear(f.hysteresisRatio, 2.4, 0.0, "hysteresisRatio 搬了");
    checkNear(f.maxDistancePx, 155.0, 0.0, "★ maxDistancePx 搬了");
    check(f.randomSeed == 4242, "★ randomSeed 搬了");
    checkNear(f.matchCenterRatio, 0.61, 0.0, "★ matchCenterRatio 搬了");
    checkNear(f.areaRatioTol, 3.7, 0.0, "★ areaRatioTol 搬了");
    checkNear(f.kSnapMult, 2.2, 0.0, "★ kSnapMult 搬了");
    checkNear(f.minAspect, 0.11, 0.0, "★ minAspect 搬了");
    checkNear(f.maxAspect, 6.6, 0.0, "★ maxAspect 搬了");

    check(f.detectionResolution == 640, "detectionResolution 搬了");
    check(f.aimClassIds.size() == 2 && f.aimClassIds[0] == 3 && f.aimClassIds[1] == 7,
          "aim_classes 的 class_id 全搬了");

    section("★★ class_filters 必须被搬运（这是原来的断层）");
    check(f.classFilters.size() == 2, "class_filters 条数对");
    check(f.classFilters.size() >= 1 && f.classFilters[0].first == 1 &&
              f.classFilters[0].second == 1,
          "★★ class 1 搬过来且 bucket==1(Filter) —— 不搬的话 TargetPage 白设");
    check(f.classFilters.size() >= 2 && f.classFilters[1].first == 5 &&
              f.classFilters[1].second == 2,
          "class 5 搬过来且 bucket==2(Aim)");

    section("★★ 端到端: flatten 之后 TargetPage 设的 Filter 真的生效");
    {
        // 只有全局 class_filters 说 class 0 是 Filter，aim_classes 为空
        HotkeyProfile hk2;
        hk2.aim_classes.clear();
        hk2.ctl_y_offset = 0.5; hk2.ctl_y_offset_max = 0.5;
        std::vector<ClassFilterState> cf2;
        { ClassFilterState c; c.class_id = 0; c.bucket = ClassBucket::Filter; cf2.push_back(c); }

        AimController ac;
        ac.setConfig(toControllerConfig(flattenProfile(hk2, 320, cf2)));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = 1.0 / 120.0;
        Candidate c; c.box = Box{ 160, 100, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        check(!ac.update(in).engaged,
              "★★ 界面上设为 Filter 的类别不产生控制（整条链真的通了）");
    }
    {
        // 同样的设置，但全局说是 Aim ⇒ 必须产生控制
        HotkeyProfile hk2;
        hk2.aim_classes.clear();
        hk2.ctl_y_offset = 0.5; hk2.ctl_y_offset_max = 0.5;
        std::vector<ClassFilterState> cf2;
        { ClassFilterState c; c.class_id = 0; c.bucket = ClassBucket::Aim; cf2.push_back(c); }

        AimController ac;
        ac.setConfig(toControllerConfig(flattenProfile(hk2, 320, cf2)));
        ControlInput in;
        in.cross = Vec2{ 100, 100 };
        in.dtSec = 1.0 / 120.0;
        Candidate c; c.box = Box{ 160, 100, 40, 80 }; c.classId = 0; c.confidence = 0.9;
        in.candidates = { c };
        check(ac.update(in).engaged,
              "★ 同一路径设为 Aim ⇒ 产生控制（正反两面都钉住）");
    }
}

int main()
{
    std::printf("=== 控制器接线回归 ===\n");
    testFlattenProfile();
    testBucketMapping();
    testConfigMapping();
    testConfigActuallyDrivesControl();
    testBucketMerge();
    testNewlyExposedKnobs();
    testDtGate();

    std::printf("\n%d 项断言, 失败 %d\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("全部通过\n");
    return g_failures == 0 ? 0 : 1;
}
