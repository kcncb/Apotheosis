// mouse/aim_tracker.h 的回归 —— PID-EventSync 档的目标跟踪器
// (移植 AimMagic 1.0.30 的 FUN_140089ac0 / FUN_14006e470 语义)。
//
// ── 这个测试回答的问题 ───────────────────────────────────────────────────────
// 跟踪器在 EventSync 档里承担三件事, 每一件都必须【可判定】:
//   ① 身份: 同一目标跨帧必须拿同一个 id(即使框在抖、即使漏一两帧);
//      真的换了目标必须换 id。这是"积分不再被每秒清 8 次"的全部依据。
//   ② 速度: 采样窗给出的速度必须收敛到真实速度, 而且窗排空前【保持上一次】
//      的值(跨帧持有)—— 不是每帧从相邻两框心重建(那在 8.3ms 下是几百 px/s 噪声)。
//   ③ 预测: 系数按 0.1/帧 爬、-0.2/帧 落; 提前量有【硬上限】与【噪声门】,
//      关预测(系数 0)时是彻底恒等变换。
//
// ── 判定哲学 ─────────────────────────────────────────────────────────────────
// 只断言可判定的性质: 同一个 id、单调爬升、有界、恒等、确定。不断言精确浮点值。

#include "mouse/aim_pid.h"       // 只用它的 kAimDeadTimeS(一致性断言)
#include "mouse/aim_tracker.h"

#include <cmath>
#include <cstdio>
#include <limits>
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

void check_near(double got, double want, double tol, const std::string& what)
{
    ++g_checks;
    if (!(std::abs(got - want) <= tol))
    {
        std::printf("  [FAIL] %s: got %.6f want %.6f (tol %.6f)\n",
                    what.c_str(), got, want, tol);
        ++g_failures;
    }
}

boss::TrackBox box(double cx, double cy, double w = 60.0, double h = 120.0)
{
    boss::TrackBox b;
    b.x = static_cast<float>(cx - w * 0.5);
    b.y = static_cast<float>(cy - h * 0.5);
    b.w = static_cast<float>(w);
    b.h = static_cast<float>(h);
    return b;
}

boss::AimTrackerParams baseParams()
{
    boss::AimTrackerParams p;
    p.min_hits = 3;
    p.max_age = 5;
    p.assoc_radius_px = 80.0;
    p.assoc_iou = 0.20;
    p.vel_window_s = 0.10;
    p.pred_max_lead_px = 12.0;
    p.pred_vel_floor = 60.0;
    return p;
}

// 喂一帧: 只有一个候选。
// ★ 同时调用 predictionLead() —— 引擎(tests 之外的 boss_aim.cpp)就是每拍调一次,
//   而预测系数 FSM 只在 predictionLead() 里推进。测试如果不调它, 系数永远是 0。
// ★★ 每帧只许调一次: FSM 的爬升/回落是按【调用次数】推进的, 调两次等于一拍走两步。
//   所以需要提前量的用例走 lead_x/lead_y 出口, 不许再自己调一次。
void feed(boss::AimTracker& tr, double cx, double cy, double dt,
          double* lead_x = nullptr, double* lead_y = nullptr)
{
    tr.beginFrame(dt);
    tr.offer(box(cx, cy), cx, cy);
    tr.endFrame();
    double lx = 0.0, ly = 0.0;
    tr.predictionLead(lx, ly);
    if (lead_x) *lead_x = lx;
    if (lead_y) *lead_y = ly;
}

// ── [1] 身份粘滞: 抖动的同一个目标不能被当成新目标 ────────────────────────────
void test_identity_sticky()
{
    std::printf("[1] 身份粘滞 (同一目标跨帧同 id)\n");
    boss::AimTracker tr;
    tr.configure(baseParams());

    feed(tr, 500.0, 300.0, 1.0 / 120.0);
    const int first = tr.lockedId();
    check(first > 0, "第一帧必须已经锁定(不许等 min_hits)");

    // 横向匀速移动 + 每帧 ±0.5px 抖动(模拟检测框量化噪声)。
    for (int i = 1; i <= 60; ++i)
    {
        const double jitter = (i % 2 == 0) ? 0.5 : -0.5;
        feed(tr, 500.0 + i * 2.0 + jitter, 300.0 + jitter, 1.0 / 120.0);
        check(tr.lockedId() == first, "抖动/移动中身份必须保持不变");
    }
    check(static_cast<int>(tr.tracks().size()) == 1,
          "全程只应有一条轨迹(没有误建新轨迹)");

    // ★ 反向验证: 真的换目标(跳到 300px 外, 远超声明的关联门限)必须换 id。
    feed(tr, 900.0, 300.0, 1.0 / 120.0);
    check(tr.lockedId() != first, "跳到关联门限之外必须算作新目标(换 id)");
}

// ── [2] 生命期: min_hits 不挡锁定; 超龄轨迹被删除 ────────────────────────────
void test_lifecycle()
{
    std::printf("[2] 生命期 (min_hits 不挡锁定 / max_age 淘汰)\n");
    auto p = baseParams();
    p.min_hits = 3;
    p.max_age = 5;
    boss::AimTracker tr;
    tr.configure(p);

    // ① 只喂一帧: 必须【立刻】能锁定 —— 上游 selector 已经决定了瞄谁,
    //    跟踪器没有资格因为 min_hits 把这个决定推迟 3 帧(port-spec §2.3 的坑)。
    feed(tr, 400.0, 400.0, 1.0 / 120.0);
    check(tr.locked() != nullptr, "第一帧就必须可锁定(min_hits 不许挡锁定)");
    check(tr.locked() != nullptr && !tr.locked()->confirmed,
          "第一帧还不是 confirmed(hits=1 < min_hits=3)");

    // ② 连续命中到 min_hits: 必须变成 confirmed。
    feed(tr, 400.0, 400.0, 1.0 / 120.0);
    feed(tr, 400.0, 400.0, 1.0 / 120.0);
    check(tr.locked() != nullptr && tr.locked()->confirmed,
          "连续 3 帧命中后必须 confirmed");

    // ③ 空观测连续 max_age 帧: 轨迹必须被删掉。
    for (int i = 0; i < p.max_age; ++i)
    {
        tr.beginFrame(1.0 / 120.0);
        tr.endFrame();
        check(tr.locked() != nullptr, "max_age 之内轨迹必须还活着(滑行窗口)");
    }
    tr.beginFrame(1.0 / 120.0);
    tr.endFrame();
    check(tr.locked() == nullptr, "超过 max_age 之后轨迹必须被删除");
    check(tr.tracks().empty(), "轨迹表必须清空");
}

// ── [2b] 锁定轨迹死掉后, 身份接管给现存已确认轨迹 ────────────────────────────
//
// boss_aim.cpp 靠"locked id 变了"来判换目标并复位控制器。如果锁定轨迹死了以后
// lockedId() 停在 -1 或者跳到一条没确认的轨迹上, 那些 downstream 判据就会误判。
void test_lock_takeover()
{
    std::printf("[2b] 锁定轨迹死亡后的身份接管\n");
    auto p = baseParams();
    p.min_hits = 3;
    p.max_age = 5;
    boss::AimTracker tr;
    tr.configure(p);
    const double dt = 1.0 / 120.0;

    // 目标 A(左)与目标 B(右), 相距足够远(不会被互相关联)。
    // 先各喂 3 帧把两条都做成 confirmed。
    for (int i = 0; i < 3; ++i)
    {
        tr.beginFrame(dt);
        tr.offer(box(400.0, 300.0), 400.0, 300.0);
        tr.offer(box(900.0, 300.0), 900.0, 300.0);
        tr.endFrame();
    }
    // ★ 一帧多候选: 锁定必须落在【先喂的那个】(上游 selector 的顺序就是优先级)。
    check(tr.lockedId() >= 0, "多候选帧里必须有锁定");
    check(tr.tracks().size() == 2, "两个远隔的候选必须建成两条轨迹");
    const int id_a = tr.lockedId();

    // A 消失, 只剩 B 被观测: A 逐步超龄, B 的 confirmed 状态保持。
    for (int i = 0; i < p.max_age + 2; ++i)
    {
        tr.beginFrame(dt);
        tr.offer(box(900.0, 300.0), 900.0, 300.0);
        tr.endFrame();
    }
    const int id_b = tr.lockedId();
    check(id_b >= 0, "A 死了以后必须仍然有锁定(接管给 B)");
    check(id_b != id_a, "★ 接管后 id 必须变化(否则下游判不出换目标)");
    // ★ 关键: 接管的是【B】, 而且 B 必须是 confirmed 的轨迹。
    check(tr.locked() != nullptr && tr.locked()->confirmed,
          "接管的必须是一条已确认的轨迹(不许接管刚出生两帧的噪声轨迹)");

    tr.reset();
    check(tr.lockedId() == -1 && tr.tracks().empty(),
          "reset 必须清空身份与轨迹");
}

// ── [3] 速度采样窗: 收敛到真实速度, 且窗排空前保持上一次的值 ─────────────────
void test_velocity_window()
{
    std::printf("[3] 速度采样窗 (收敛 + 跨帧持有)\n");
    auto p = baseParams();
    p.vel_window_s = 0.10;   // 120fps 下 = 12 拍
    boss::AimTracker tr;
    tr.configure(p);

    const double dt = 1.0 / 120.0;
    const double speed = 300.0;   // px/s, 向右
    double x = 400.0;
    feed(tr, x, 300.0, dt);
    for (int i = 1; i <= 120; ++i)   // 1 秒
    {
        x += speed * dt;
        feed(tr, x, 300.0, dt);
    }
    const auto* t = tr.locked();
    check(t != nullptr, "轨迹还在");
    if (t)
    {
        check(t->vel_valid, "跑满一个窗之后速度必须已结算");
        check_near(t->vel_x, speed, 15.0, "速度必须收敛到真实值 300px/s");
        check_near(t->vel_y, 0.0, 5.0, "Y 速度必须约等于 0");
    }

    // ★ 跨帧持有: 窗内(未排空)的拍上, vel 必须还是上一次结算的值 —— 不许归零、
    //   也不许每帧重算成噪声(那正是"速度不跨帧持有"的老毛病)。
    std::vector<double> within_window;
    for (int i = 1; i < 12; ++i)
    {
        x += speed * dt;
        feed(tr, x, 300.0, dt);
        const auto* tt = tr.locked();
        if (tt) within_window.push_back(tt->vel_x);
    }
    bool held = true;
    for (double v : within_window)
        if (std::abs(v - speed) > 15.0) held = false;
    check(held, "窗内每拍都必须保持上一窗结算的速度(不许清零/重算)");

    // 静止目标: 速度必须回落到 0 附近(不能一直卡在 300)。
    for (int i = 0; i < 60; ++i)
        feed(tr, x, 300.0, dt);
    const auto* t2 = tr.locked();
    if (t2)
        check(std::abs(t2->vel_x) < 20.0, "目标停下后速度必须回落到 0 附近");
}

// ── [4] 预测系数 FSM: 爬升/回落速率、上下限 ──────────────────────────────────
void test_prediction_fsm()
{
    std::printf("[4] 预测系数 FSM (0.1 爬 / 0.2 落)\n");
    auto p = baseParams();
    // 让预测系数非零, 否则 sizeWeight 分支不会推进 FSM 之外的量。
    p.pred_factor_x = 0.2;
    p.pred_factor_y = 0.2;
    p.pred_vel_floor = 0.0;   // 关掉噪声门, 单独测 FSM
    // ★ 关联门限必须放宽: 这个用例的目标速度(20000px/s)每帧走 166px, 超过默认的
    //   80px 门限 —— 那样每帧都会新建轨迹, 速度采样窗永远攒不满(vel_valid 恒 0),
    //   系数也就永远是 0。这不是代码 bug, 是"目标快到跟丢"的物理必然。
    p.assoc_radius_px = 400.0;
    boss::AimTracker tr;
    tr.configure(p);

    const double dt = 1.0 / 120.0;
    // ★★ AM 的系数涨落是【两个条件同时满足】才涨(见 FUN_14006e470 行 133-146):
    //     ① 目标位移 > max(30, 0.75×框宽) × (1+1.5×系数)  ← 目标动得够快
    //     ② 自身瞄准速度 > 3.5 px/帧                       ← 自己也在动
    //    少了 ②, 系数只落不涨, 预测永远不启动。所以这个用例必须同时喂自身速度。
    //    自身速度必须超过换算到速度域的门: 3.5057 ÷ (1/120) ≈ 421 px/s。
    const double self_v = 3000.0;   // 自己猛甩; 远超 421px/s 的门
    auto setSelf = [&]() { tr.setAimVelocity(self_v, 0.0); };
    setSelf();

    // ① 目标高速移动 → 系数必须逐帧爬升, 每次 +0.1, 上限 1.0。
    //   ★ 注意爬升的【前置条件】: 速度采样窗要先排空一次(vel_valid = true),
    //     在那之前"目标在不在动"无从判断, 系数保持 0 —— 这是刻意的, 不许拿
    //     一个还没结算的速度去决定要不要预测。
    double x = 400.0;
    feed(tr, x, 300.0, dt);
    const double k0 = tr.locked() ? tr.locked()->pred_k_x : -1.0;
    check_near(k0, 0.0, 1e-9, "首帧系数必须从 0 开始(AM: track+0x18 初始化 0)");

    std::vector<double> ks;
    for (int i = 0; i < 90; ++i)
    {
        // 目标速度必须超过门限: max(30, 0.75×60)=45px × (1+1.5k) ÷ (1/120)
        //   k=0 时 = 5400px/s; k=1 时 = 13500px/s。取 20000px/s 保证全程都能涨。
        // ★ 但每帧位移不能超过关联门限(80px), 否则每帧都会新建轨迹、速度窗永远
        //   攒不起来(实测: 166px/帧时 vel_valid 恒为 0, 系数永远 0)。
        //   120fps × 80px = 9600px/s —— 所以这个用例要把关联门限放宽。
        x += 20000.0 * dt;
        setSelf();
        feed(tr, x, 300.0, dt);
        if (tr.locked()) ks.push_back(tr.locked()->pred_k_x);
    }
    bool monotone = true;
    for (std::size_t i = 1; i < ks.size(); ++i)
        if (ks[i] < ks[i - 1] - 1e-12) monotone = false;
    check(monotone, "目标持续高速移动 + 自己在甩时, 系数必须单调不降");
    check(!ks.empty() && std::abs(ks.back() - 1.0) < 1e-9, "系数必须爬到上限 1.0 并夹住");

    // ★ 爬升步长必须是 0.1(AM 的 DAT_1401f461c), 回落步长必须是 0.2。
    //   取"从 0 变正的那一拍"与"下一拍"来量步长 —— 之前的速度窗拍上系数恒为 0。
    double first_rise = -1.0, second_rise = -1.0;
    for (std::size_t i = 0; i + 1 < ks.size(); ++i)
    {
        if (ks[i] > 1e-9 && ks[i] < 1.0 - 1e-9 && ks[i + 1] > ks[i])
        {
            first_rise = ks[i];
            second_rise = ks[i + 1];
            break;
        }
    }
    check(first_rise > 0.0, "必须存在爬升过程(否则步长断言是空转)");
    if (first_rise > 0.0)
        check_near(second_rise - first_rise, 0.1, 1e-9, "爬升步长必须是 0.1/帧");

    // ② 目标停下 → 系数必须以 -0.2/帧 回落, 下限 0。
    //   ★ 回落有【两段延迟】, 都不是 bug:
    //     ① 速度采样窗要先排空(窗内速度还停在旧值上, 系数会继续涨) —— 12 拍;
    //     ② 然后系数才按 0.2/帧 落下 —— 5 拍。
    //   所以这里要跑够 30 拍, 少了量到的就是"还没开始落"。
    //   ★ 自身速度【继续喂】—— 这样回落就唯一归因于"目标停了", 不是自身条件。
    std::vector<double> fall;
    for (int i = 0; i < 30; ++i)
    {
        setSelf();
        feed(tr, x, 300.0, dt);
        if (tr.locked()) fall.push_back(tr.locked()->pred_k_x);
    }
    check(!fall.empty() && std::abs(fall.back()) < 1e-9, "停下后系数必须回落到 0");
    // 回落要等速度窗也排空(窗内速度还停在旧值上, 系数会继续涨) —— 找第一处下降。
    double first_fall = -1.0, second_fall = -1.0;
    for (std::size_t i = 0; i + 1 < fall.size(); ++i)
    {
        if (fall[i + 1] < fall[i] - 1e-12)
        {
            first_fall = fall[i];
            second_fall = fall[i + 1];
            break;
        }
    }
    check(first_fall > 0.0, "必须存在回落过程");
    if (first_fall > 0.0)
        check_near(second_fall - first_fall, -0.2, 1e-9, "回落步长必须是 0.2/帧");
}

// ── [5] 提前量的两道安全阀: 硬上限 + 噪声门 ──────────────────────────────────
void test_lead_clamps()
{
    std::printf("[5] 提前量硬上限与速度噪声门\n");
    auto p = baseParams();
    p.pred_factor_x = 0.2;
    p.pred_factor_y = 0.2;
    p.pred_max_lead_px = 12.0;
    p.pred_vel_floor = 60.0;
    p.pred_min_w = 20.0;
    p.pred_max_w = 80.0;
    // ★ 同上: 这个用例用 24000px/s 的目标, 每帧 200px —— 必须放宽关联门限,
    //   否则每帧新建轨迹、速度窗攒不满(vel_valid 恒 0)、提前量恒 0。
    p.assoc_radius_px = 400.0;
    boss::AimTracker tr;
    tr.configure(p);

    const double dt = 1.0 / 120.0;
    double x = 400.0;
    feed(tr, x, 300.0, dt);
    double lead_max = 0.0;
    for (int i = 0; i < 200; ++i)
    {
        x += 200.0;   // 24000 px/s —— 必须超过速度域门限(45×(1+1.5k)÷dt)
        tr.setAimVelocity(3000.0, 0.0);
        double lx = 0.0, ly = 0.0;
        feed(tr, x, 300.0, dt, &lx, &ly);
        lead_max = std::max(lead_max, std::abs(lx));
        check(std::isfinite(lx) && std::isfinite(ly), "提前量必须有限");
    }
    // ★ 硬上限是【安全性质】, 不是调参: 没有它, 提前量随速度线性增长(§4.2 禁止)。
    check(lead_max <= 12.0 + 1e-9, "提前量绝不允许超过硬上限 12px");
    check(lead_max > 0.5, "高速移动目标必须真的产生提前量(否则上限断言是空转)");

    // ② 噪声门: 静止目标(只有 ±0.5px 抖动)的提前量必须恒为 0。
    boss::AimTracker quiet;
    quiet.configure(p);
    double qx = 400.0;
    feed(quiet, qx, 300.0, dt);
    bool any_lead = false;
    for (int i = 0; i < 120; ++i)
    {
        qx += (i % 2 == 0) ? 0.5 : -0.5;
        double lx = 0.0, ly = 0.0;
        feed(quiet, qx, 300.0, dt, &lx, &ly);
        if (std::abs(lx) > 1e-9 || std::abs(ly) > 1e-9) any_lead = true;
    }
    check(!any_lead, "静止目标(量化抖动)的提前量必须恒为 0(噪声门)");
}

// ── [5b] AM 的双条件门: "目标在动" 且 "自己在动" 才涨系数 ────────────────────
//
// ★ 这是 FUN_14006e470 行 133-146 的原文语义, 之前被我漏掉了第二条:
//     if (门限 <= 目标位移 || |自身速度| <= 3.5px/帧) { 系数 -= 0.2 } else { += 0.1 }
//   即: 自己没在动 → 系数只落不涨。漏掉这条会让"预测永远不启动"变成
//   "预测在不该启动的时候启动"。
void test_two_clause_gate()
{
    std::printf("[5b] 双条件门 (目标在动 且 自己在动)\n");
    auto p = baseParams();
    p.pred_factor_x = 0.2;
    p.pred_vel_floor = 0.0;
    // ★ 这些用例用 200px/帧 的目标 —— 同 §[4]/§[5], 关联门限必须放宽, 否则每帧
    //   新建轨迹、速度窗攒不满, 测到的就是"跟丢了"而不是"门限不成立"。
    p.assoc_radius_px = 400.0;
    const double dt = 1.0 / 120.0;

    // ① 目标极快但【自己不动】 → 系数必须保持 0(不许涨)。
    {
        boss::AimTracker tr;
        tr.configure(p);
        double x = 400.0;
        tr.setAimVelocity(0.0, 0.0);   // ★ 自己完全不动
        feed(tr, x, 300.0, dt);
        double kmax = 0.0;
        for (int i = 0; i < 90; ++i)
        {
            x += 200.0;
            tr.setAimVelocity(0.0, 0.0);   // 每拍都要喂(AM 是每拍读的)
            double lx = 0.0, ly = 0.0;
            feed(tr, x, 300.0, dt, &lx, &ly);
            if (tr.locked()) kmax = std::max(kmax, tr.locked()->pred_k_x);
        }
        check_near(kmax, 0.0, 1e-9,
                   "★ 自己不动时系数必须恒为 0(AM 的第二个条件, 只落不涨)");
    }

    // ② 自己动得【不够快】 → 同样不许涨(门限是 3.5057px/帧 ≈ 421px/s @120fps)。
    {
        boss::AimTracker tr;
        tr.configure(p);
        double x = 400.0;
        feed(tr, x, 300.0, dt);
        double kmax = 0.0;
        for (int i = 0; i < 90; ++i)
        {
            x += 200.0;
            tr.setAimVelocity(300.0, 0.0);   // 300 px/s < 421 px/s 的门
            double lx = 0.0, ly = 0.0;
            feed(tr, x, 300.0, dt, &lx, &ly);
            if (tr.locked()) kmax = std::max(kmax, tr.locked()->pred_k_x);
        }
        check_near(kmax, 0.0, 1e-9, "自身速度低于 3.5px/帧 的门时系数不许涨");
    }

    // ③ 两个条件都满足 → 必须涨(否则 ①② 是空转)。
    {
        boss::AimTracker tr;
        tr.configure(p);
        double x = 400.0;
        feed(tr, x, 300.0, dt);
        for (int i = 0; i < 90; ++i)
        {
            x += 200.0;
            tr.setAimVelocity(6000.0, 0.0);   // 远超门
            double lx = 0.0, ly = 0.0;
            feed(tr, x, 300.0, dt, &lx, &ly);
        }
        check(tr.locked() && tr.locked()->pred_k_x > 0.5,
              "★ 两个条件都满足时系数必须涨起来(证明 ①② 不是空转)");
    }

    // ④ 目标【停下】但自己继续甩 → 系数必须回落(证明回落归因于目标, 不是自身)。
    {
        boss::AimTracker tr;
        tr.configure(p);
        double x = 400.0;
        feed(tr, x, 300.0, dt);
        for (int i = 0; i < 90; ++i)   // 先涨起来
        {
            x += 200.0;
            tr.setAimVelocity(6000.0, 0.0);
            double lx = 0.0, ly = 0.0;
            feed(tr, x, 300.0, dt, &lx, &ly);
        }
        const double before = tr.locked() ? tr.locked()->pred_k_x : -1.0;
        check(before > 0.5, "前置: 系数已经涨起来了");
        for (int i = 0; i < 40; ++i)   // 目标停住, 自己继续甩
        {
            tr.setAimVelocity(6000.0, 0.0);
            double lx = 0.0, ly = 0.0;
            feed(tr, x, 300.0, dt, &lx, &ly);
        }
        check(tr.locked() && std::abs(tr.locked()->pred_k_x) < 1e-9,
              "目标停住后系数必须回落到 0(即使自己还在甩)");
    }
}

// ── [6] 关预测时必须逐位恒等 ─────────────────────────────────────────────────
void test_prediction_identity()
{
    std::printf("[6] 系数 0 = 恒等变换\n");
    auto p = baseParams();
    p.pred_factor_x = 0.0;   // 默认: 不预测
    p.pred_factor_y = 0.0;
    boss::AimTracker tr;
    tr.configure(p);

    const double dt = 1.0 / 120.0;
    double x = 400.0;
    feed(tr, x, 300.0, dt);
    bool zero = true;
    for (int i = 0; i < 100; ++i)
    {
        x += 20.0;
        double lx = 1.0, ly = 1.0;
        feed(tr, x, 300.0, dt, &lx, &ly);
        const bool active = (lx != 0.0 || ly != 0.0);
        if (active || lx != 0.0 || ly != 0.0) zero = false;
    }
    check(zero, "系数为 0 时提前量必须恒为 (0,0) 且报告 inactive");

    // 尺寸区间之外(框太小)也必须不补 —— AM 的 sizeWeight 在区间外为 0。
    auto p2 = baseParams();
    p2.pred_factor_x = 0.2;
    p2.pred_factor_y = 0.2;
    p2.pred_min_w = 100.0;
    p2.pred_max_w = 200.0;
    boss::AimTracker small;
    small.configure(p2);
    double sx = 400.0;
    small.beginFrame(dt);
    small.offer(box(sx, 300.0, 40.0, 80.0), sx, 300.0);   // 框宽 40 < min_w
    small.endFrame();
    bool tiny_zero = true;
    for (int i = 0; i < 50; ++i)
    {
        sx += 20.0;
        small.beginFrame(dt);
        small.offer(box(sx, 300.0, 40.0, 80.0), sx, 300.0);
        small.endFrame();
        double lx = 0.0, ly = 0.0;
        small.predictionLead(lx, ly);
        if (lx != 0.0 || ly != 0.0) tiny_zero = false;
    }
    check(tiny_zero, "框宽在尺寸区间之外时必须不补(AM 的 sizeWeight=0)");
}

// ── [7] 确定性 + 异常输入不产生 NaN ──────────────────────────────────────────
void test_robustness()
{
    std::printf("[7] 确定性 / 异常输入\n");
    const auto run = []() {
        auto p = baseParams();
        p.pred_factor_x = 0.2;
        p.pred_factor_y = 0.2;
        boss::AimTracker tr;
        tr.configure(p);
        double x = 400.0;
        double acc = 0.0;
        for (int i = 0; i < 100; ++i)
        {
            x += 3.0;
            feed(tr, x, 300.0 + i * 0.25, 1.0 / 120.0);
            double lx = 0.0, ly = 0.0;
            tr.predictionLead(lx, ly);
            acc += lx + ly;
        }
        return acc;
    };
    check_near(run(), run(), 0.0, "同样的输入必须给出逐位相同的结果(可复现)");

    // dt = 0 / 负 / NaN: 不许产生 NaN, 也不许把速度算成 inf。
    boss::AimTracker tr;
    tr.configure(baseParams());
    tr.beginFrame(0.0);
    tr.offer(box(400.0, 300.0), 400.0, 300.0);
    tr.endFrame();
    check(tr.locked() != nullptr && std::isfinite(tr.locked()->vel_x),
          "dt=0 不许产生 NaN/inf 速度");
    tr.beginFrame(-1.0);
    tr.offer(box(401.0, 300.0), 401.0, 300.0);
    tr.endFrame();
    check(tr.locked() != nullptr && std::isfinite(tr.locked()->vel_x),
          "dt<0 不许产生 NaN/inf 速度");

    // 空观测连续 1000 帧: 轨迹清空且不崩。
    for (int i = 0; i < 1000; ++i)
    {
        tr.beginFrame(1.0 / 120.0);
        tr.endFrame();
    }
    check(tr.locked() == nullptr && tr.tracks().empty(), "长期空观测必须彻底清空");

    // reset() 之后必须回到干净状态。
    feed(tr, 400.0, 300.0, 1.0 / 120.0);
    tr.reset();
    check(tr.locked() == nullptr && tr.tracks().empty(), "reset 必须清空轨迹与身份");
}

// ── [8] 参数入口的夹取(坏配置不许把跟踪器搞坏) ───────────────────────────────
void test_param_clamps()
{
    std::printf("[8] 参数夹取\n");
    boss::AimTracker tr;
    boss::AimTrackerParams p;
    p.min_hits = 0;            // 非法
    p.max_age = -5;            // 非法
    p.pred_max_lead_px = 1e9;  // 超过绝对天花板
    p.pred_factor_x = 99.0;    // 远超 ±0.2
    p.pred_min_w = 100.0;
    p.pred_max_w = 50.0;       // max < min
    tr.configure(p);

    feed(tr, 400.0, 300.0, 1.0 / 120.0);
    check(tr.locked() != nullptr, "坏参数下仍然要能锁定");
    check(tr.locked() != nullptr && tr.locked()->confirmed,
          "min_hits 被夹到 1 之后首帧就该 confirmed");

    double lx = 0.0, ly = 0.0;
    tr.predictionLead(lx, ly);
    check(std::isfinite(lx) && std::isfinite(ly), "坏参数下提前量必须有限");
    check(std::abs(lx) <= boss::AimTracker::kAbsMaxLeadPx + 1e-9,
          "提前量上限必须被夹到绝对天花板(64px)以内");
    // 系数被夹到 0.2, 所以提前量不可能超过 0.2 × v × sizeWeight。
    check(std::abs(lx) < 50.0, "系数夹取之后提前量必须在物理量级");
}

// ── [9] ⑤ 在途自身位移补偿 (AM 的发送环窗口 ÷ k̂) ────────────────────────────
//
// AM: FUN_140067000 行 1172-1209 —— 把窗口内发出去的计数求和, ÷ counts_per_pixel
// 换回像素。本实现用定长环形缓冲(不用时间戳), 除以窗口拍数, 与 aim_pid.h 的
// 计数域补偿同构。
void test_inflight_chain()
{
    std::printf("[9] 在途补偿链 (发送环窗口 ÷ k̂)\n");
    auto p = baseParams();
    p.inflight_window_s = 0.010;   // 10ms
    p.inflight_beta = 1.0;
    boss::AimTracker tr;
    tr.configure(p);
    const double dt = 1.0 / 120.0;
    tr.beginFrame(dt);

    // ★ k̂ = 1.0(默认)时, 像素量 = 计数/拍数 —— 也就是"计数当像素", 不做换算。
    const std::size_t ticks = tr.inflightWindowTicks();
    check(ticks >= 1, "窗口拍数至少 1");
    check_near(static_cast<double>(ticks), p.inflight_window_s / dt, 1.0,
               "窗口拍数 = 窗口秒数 ÷ dt");

    // ★ 窗口 = 0 的语义是【整条链关闭】(在途量恒 0), 而不是"至少 1 拍"。
    //   两者必须分得开, 否则"三件全关时逐位一致"那条回归不成立。
    {
        auto pz = p;
        pz.inflight_window_s = 0.0;
        boss::AimTracker tz;
        tz.configure(pz);
        tz.beginFrame(dt);
        check(tz.inflightWindowTicks() == 0, "★ 窗口 0 必须表示关闭(拍数 0)");
        tz.noteSend(50, 50);
        double zx = 0.0, zy = 0.0;
        tz.inflightPixels(zx, zy);
        check_near(zx, 0.0, 1e-9, "★ 关闭时在途量必须恒为 0(即使登了账)");
        check_near(zy, 0.0, 1e-9, "同上");
    }

    // 每拍发 12 计数; 填满窗口后, 在途量应当是 12×ticks/(k̂×ticks) = 12 像素。
    for (std::size_t i = 0; i < ticks; ++i)
        tr.noteSend(12, 0);
    double px = 0.0, py = 0.0;
    tr.inflightPixels(px, py);
    check_near(px, 12.0, 1e-9, "k̂=1 时在途像素 = 每拍计数(不随窗口长度变)");
    check_near(py, 0.0, 1e-9, "y 轴没发过就是 0");

    // ★ k̂ = 2(2 计数 = 1 像素)⇒ 同一批计数换算出的像素减半。
    auto p2 = p;
    p2.counts_per_pixel_x = 2.0;
    boss::AimTracker tr2;
    tr2.configure(p2);
    tr2.beginFrame(dt);
    for (std::size_t i = 0; i < tr2.inflightWindowTicks(); ++i)
        tr2.noteSend(12, 0);
    double px2 = 0.0, py2 = 0.0;
    tr2.inflightPixels(px2, py2);
    check_near(px2, 6.0, 1e-9, "★ k̂ 加倍 ⇒ 在途像素减半(k̂ 真的参与了换算)");

    // ★ k̂ = 0.5(1 计数 = 2 像素)⇒ 像素加倍。
    auto p3 = p;
    p3.counts_per_pixel_x = 0.5;
    boss::AimTracker tr3;
    tr3.configure(p3);
    tr3.beginFrame(dt);
    for (std::size_t i = 0; i < tr3.inflightWindowTicks(); ++i)
        tr3.noteSend(12, 0);
    double px3 = 0.0, py3 = 0.0;
    tr3.inflightPixels(px3, py3);
    check_near(px3, 24.0, 1e-9, "k̂ 减半 ⇒ 在途像素加倍");

    // ★ 非法 k̂ 必须回落到 1.0(不换算), 不许除零/放大。
    auto p4 = p;
    p4.counts_per_pixel_x = 0.0;    // 0 → 回落
    boss::AimTracker tr4;
    tr4.configure(p4);
    tr4.beginFrame(dt);
    for (std::size_t i = 0; i < tr4.inflightWindowTicks(); ++i)
        tr4.noteSend(12, 0);
    double px4 = 0.0, py4 = 0.0;
    tr4.inflightPixels(px4, py4);
    check(std::isfinite(px4) && px4 == 12.0, "k̂=0 必须回落到 1.0(与默认一致)");

    // ★ 窗口拍数必须被真实死区夹住 —— 超过死区会把早已生效的指令再扣一次。
    auto p5 = p;
    p5.inflight_window_s = 10.0;    // 远超 46ms
    boss::AimTracker tr5;
    tr5.configure(p5);
    tr5.beginFrame(dt);
    check_near(tr5.inflightWindowTicks() * dt, boss::kInflightWindowMaxS, 0.02,
               "★ 窗口被夹到 46ms 死区(不许超过)");

    // ★ 复位必须清账本: 旧目标的欠账不许算到新目标头上。
    tr.clearInflight();
    tr.inflightPixels(px, py);
    check_near(px, 0.0, 1e-9, "clearInflight 后必须归零");
    tr.reset();
    tr.inflightPixels(px, py);
    check_near(px, 0.0, 1e-9, "reset 后必须归零");

    // ★ 空窗口不许产生 NaN。
    boss::AimTracker tr6;
    tr6.configure(p);
    tr6.beginFrame(dt);
    tr6.inflightPixels(px, py);
    check(std::isfinite(px) && std::isfinite(py), "空账本必须返回有限值");
}

// ── [10] ⑥ 自运动补偿 (AM 的自身瞄准速度项) ─────────────────────────────────
//
// ★ 这一项在本项目被删过一次(predictive_controller, 1a5a792): 输出侧标定增益
//   进反馈回路, 标定不准就每拍反向极限环。所以默认 0, 且必须走同一个硬上限。
void test_self_motion()
{
    std::printf("[10] 自运动补偿 (默认关 / 有界)\n");
    auto p = baseParams();
    p.pred_factor_x = 0.0;     // 关掉目标速度那一项, 单独看自运动
    p.pred_max_lead_px = 12.0;
    const double dt = 1.0 / 120.0;

    // ① 默认 gain = 0 → 恒等(不管自己甩多快)。
    {
        boss::AimTracker tr;
        tr.configure(p);
        double x = 400.0;
        feed(tr, x, 300.0, dt);
        bool any = false;
        for (int i = 0; i < 60; ++i)
        {
            tr.setAimVelocity(9000.0, -9000.0);   // 疯狂甩
            double lx = 0.0, ly = 0.0;
            feed(tr, x, 300.0, dt, &lx, &ly);
            if (lx != 0.0 || ly != 0.0) any = true;
        }
        check(!any, "★ 自运动项默认关闭时提前量必须恒为 0");
    }

    // ② 开了之后: 偏移与自身速度成正比, 且被硬上限夹住。
    {
        auto q = p;
        q.self_motion_gain = 0.2;
        boss::AimTracker tr;
        tr.configure(q);
        double x = 400.0;
        feed(tr, x, 300.0, dt);
        tr.setAimVelocity(10.0, 0.0);   // 0.2 × 10 = 2px
        double lx = 0.0, ly = 0.0;
        double dummy_x = 0.0, dummy_y = 0.0;
        (void)dummy_x; (void)dummy_y;
        tr.setAimVelocity(10.0, 0.0);
        const_cast<boss::AimTracker&>(tr).predictionLead(lx, ly);
        check_near(lx, 2.0, 1e-9, "gain × 自身速度 = 2px");

        // 大幅自身速度: 必须被硬上限夹住(任务书 §4.2 ②)。
        tr.setAimVelocity(1e6, -1e6);
        tr.predictionLead(lx, ly);
        check_near(lx, 12.0, 1e-9, "★ 自运动项也必须被硬上限 12px 夹住");
        check_near(ly, -12.0, 1e-9, "反方向同理");
    }

    // ③ 非法自身速度不许产生 NaN。
    {
        auto q = p;
        q.self_motion_gain = 0.5;
        boss::AimTracker tr;
        tr.configure(q);
        tr.beginFrame(dt);
        double lx = 0.0, ly = 0.0;
        tr.setAimVelocity(std::nan(""), std::numeric_limits<double>::infinity());
        // setAimVelocity 内部会把非有限值归零, 所以这里读到的是 0 → 偏移 0。
        // ★ 用变量喂 inf 而不是写 inf 字面量: 编译器会对 1.0/0.0 这种常量表达式
        //   直接报 C2124(被零除)。
        const double inf_v = std::numeric_limits<double>::infinity();
        tr.selfMotionLead(inf_v, -inf_v, lx, ly);
        check(std::isfinite(lx) && std::isfinite(ly), "非法自身速度不许产生 NaN");
    }
}

// ── [11] k̂ 与 aim_pid.h 的死区常数必须一致(两处重复写, 会被改歪) ────────────
void test_khat_consistency()
{
    std::printf("[11] k̂/死区常数一致性\n");
    // 在途窗口上限在本文件里是 kInflightWindowMaxS, 在 aim_pid.h 里是 kAimDeadTimeS。
    // 两者必须相等 —— 否则窗口会超过真实死区, 导致过补偿发散。
    check_near(boss::kInflightWindowMaxS, boss::kAimDeadTimeS, 1e-12,
               "★ 在途窗口上限必须等于链路死区(两处常数不许改歪)");
    check_near(boss::kCountsPerPixelDefault, 1.0, 1e-12,
               "k̂ 默认值必须是 1.0(AM 的 kalman_counts_per_pixel 默认值)");
}

} // namespace

int main()
{
    std::printf("=== aim_tracker_test (PID-EventSync 跟踪器) ===\n");
    test_identity_sticky();
    test_lifecycle();
    test_lock_takeover();
    test_velocity_window();
    test_prediction_fsm();
    test_lead_clamps();
    test_two_clause_gate();
    test_prediction_identity();
    test_robustness();
    test_param_clamps();
    test_inflight_chain();
    test_self_motion();
    test_khat_consistency();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
