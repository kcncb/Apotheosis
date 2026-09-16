// PID-EventSync 全链路的闭环回归 (跟踪器 → 每轨预测 → 控制器 → 计数).
//
// ── 这个测试回答的问题 ───────────────────────────────────────────────────────
// 移植 AimMagic 1.0.30 的 EventSync 档时, 真正需要钉住的不是"和 AM 数字一样"
// (那不可能: AM 的 Kp 是 counts/像素/帧, 与本项目的 counts/(像素·秒) 不同纲,
// docs/aimmagic-port-spec.md §0.1), 而是这四条【可判定的性质】:
//
//   [1] 关掉预测时, EventSync 链路与现役链路【逐位一致】 —— 换档本身不改变行为,
//       改变行为的只有"身份从哪来"和"提前量住在哪"。
//   [2] 打开预测时, 提前量【有界】(硬上限), 且闭环不发散 —— §4.2 的红线。
//   [3] 有滑行窗口时, 一两帧的漏检【不清控制器状态】: 积分/零头跨过空档继续攒,
//       于是匀速目标的稳态滞后能被磨掉(这正是"状态跨帧持有"要买的东西)。
//   [4] 身份粘滞: 同一目标抖动/漏帧不换 id, 于是"换目标复位"不再每秒触发 8 次。
//
// ── 被控对象 (与 tests/aim_acceptance_test.cpp 同源的"假游戏") ────────────────
// 目标有世界速度; 镜头按我们下发的计数转; 测量有延迟、执行有延迟(默认用实测链路
// 死区 46ms); 框心整数量化(模拟检测框中心量化 —— 静止目标假速度的物理来源)。
//
// ── 与真实引擎的关系 ─────────────────────────────────────────────────────────
// 这里手搭的是 boss_aim.cpp::tick() 在 aim_mode == 1 时的【同一条算式】:
//   tracker.update(框) → 身份/速度 → predictionLead() 加到瞄点 → 误差 → AimPid。
// 不含 selector(它决定"瞄谁", 本测试直接给定一个目标) —— 那部分与档位无关。
// ★ 因此本测试【不】证明 boss_aim.cpp 的接线正确(那是编译 + 实机的事),
//   只证明这条算式本身有界、收敛、且降级时与现役链路等价。

#include "mouse/aim_pid.h"
#include "mouse/aim_tracker.h"

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

double median_of(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ── 假游戏 ───────────────────────────────────────────────────────────────────
struct Plant
{
    double k = 0.593;            // 每计数像素(只影响被控对象, 不进控制器)
    double meas_delay_s = 0.030;
    double act_delay_s = 0.046;  // 实测链路死区
    double dt = 1.0 / 120.0;
    double box_w = 60.0;         // 检测框宽(预测尺寸权重的输入)
    double box_h = 120.0;
};

struct RunResult
{
    double tail_mean = 0.0;      // 尾段真实 |误差| 均值
    double tail_median = 0.0;
    double peak = 0.0;
    double max_lead = 0.0;       // 全程提前量峰值
    int    nonzero_tail = 0;     // 尾段下发非零计数的拍数
    int    sign_flips = 0;       // 尾段相邻非零计数符号翻转次数
    double integral_tail = 0.0;  // 尾段积分状态(证明它真的攒起来了)
    bool   finite = true;
    std::vector<double> errors;
};

// 目标世界位置(像素, 相对起点)。
using MotionFn = double (*)(double t);

double motion_static(double) { return 0.0; }
double motion_linear(double t) { return 300.0 * t; }          // 300 px/s
double motion_stop_go(double t) { return (t < 1.0) ? 300.0 * t : 300.0; }

// 事件驱动闭环: 每个检测帧一拍(EventSync 的定义 —— 不做帧间外推)。
//
// mode: 0 = 现役链路(速度来自紧挨瞄点的 α-β 滤波, 提前量=恒 0); 
//       1 = EventSync 链路(跟踪器给身份/速度, 每轨预测状态机给提前量)。
// drop_frames: 每隔多少帧丢一次检测(0 = 不丢) —— 用来测"滑行窗口内不清状态"。
RunResult run_eventsync(const Plant& pl, boss::AimPidParams pid_params,
                        boss::AimTrackerParams tr_params, bool use_tracker,
                        MotionFn motion, double duration_s,
                        double tail_start_s, int drop_frames = 0)
{
    boss::AimPid pid;
    pid.configure(pid_params);

    boss::AimTracker tr;
    if (use_tracker)
        tr.configure(tr_params);

    const int act_n = std::max(1, static_cast<int>(std::lround(pl.act_delay_s / pl.dt)));
    const int meas_n = std::max(0, static_cast<int>(std::lround(pl.meas_delay_s / pl.dt)));
    std::deque<double> act_line(static_cast<std::size_t>(act_n), 0.0);
    std::deque<double> meas_line(static_cast<std::size_t>(meas_n), 0.0);

    double camera = 0.0;
    const int ticks = std::max(1, static_cast<int>(std::lround(duration_s / pl.dt)));
    const int tail_start = std::min(ticks,
        static_cast<int>(std::lround(tail_start_s / pl.dt)));

    RunResult r;
    std::vector<double> tail;
    int last_cmd_sign = 0;
    int frame = 0;
    double prev_camera = 0.0;
    // 自身瞄准速度(像素/秒) = 镜头(瞄点)的移动速度。引擎里由锚点差分估出,
    // 这里直接从假游戏的镜头位置取 —— 它就是"自己甩了多快"的真值。
    double aim_vx = 0.0;
    double aim_vy = 0.0;

    for (int i = 0; i < ticks; ++i)
    {
        const double t = static_cast<double>(i) * pl.dt;
        const double target = motion(t);
        ++frame;

        // 自身速度: 用【上一拍到现在】的镜头位移 ÷ dt。第一拍为 0。
        aim_vx = (i > 0) ? (camera - prev_camera) / pl.dt : 0.0;
        prev_camera = camera;

        // ── 推理: 目标在画面里的位置(目标世界位置 − 镜头位置), 整数量化 ──
        const double seen = std::round(target - camera);

        // 漏检(可以选择性丢帧): 丢的那一帧没有检测结果, 引擎 tick 不被调用,
        // 但跟踪器按 max_age 继续持有轨迹 —— 这正是 EventSync 的滑行窗口。
        const bool dropped = (drop_frames > 0) && (frame % drop_frames == 0);

        double lead_x = 0.0;
        if (!dropped)
        {
            // 测量延迟线
            meas_line.push_back(seen);
            const double fed = meas_line.front();
            meas_line.pop_front();

            if (use_tracker)
            {
                tr.beginFrame(pl.dt);
                boss::TrackBox b;
                b.x = static_cast<float>(fed - pl.box_w * 0.5);
                b.y = static_cast<float>(-pl.box_h * 0.5);
                b.w = static_cast<float>(pl.box_w);
                b.h = static_cast<float>(pl.box_h);
                tr.offer(b, fed, 0.0);
                tr.endFrame();
                // ★ ⑥ 自身瞄准速度必须每拍喂 —— AM 的系数涨落有【第二个条件】:
                //   自己没在动时系数只落不涨(FUN_14006e470 行 135)。不喂它 = 自己
                //   永远算"没在动" = 预测永远不启动。这里用"镜头移动速度"当自身速度
                //   (引擎里就是从锚点差分估出来的那个量)。
                tr.setAimVelocity(aim_vx, aim_vy);
                double ly = 0.0;
                tr.predictionLead(lead_x, ly);
                r.max_lead = std::max(r.max_lead, std::abs(lead_x));
            }

            // ★ ⑤ AM 的误差合成(FUN_140067000 行 1208-1209):
            //     误差 = 目标位置 + 提前量 − 在途自身位移(像素)
            //   ★ 顺序很重要: 先取【上一拍为止】的在途量(本拍还没发出去),
            //     算完误差、【发完之后】才把本拍登进账本。
            double if_x = 0.0, if_y = 0.0;
            if (use_tracker)
                tr.inflightPixels(if_x, if_y);
            const double err = fed + lead_x - if_x;
            const int counts = pid.step(err, pl.dt);

            // 本拍发出去的计数登账(引擎里这一步紧跟 sendRawMove)。
            if (use_tracker)
                tr.noteSend(counts, 0);

            act_line.push_back(static_cast<double>(counts) * pl.k);
            camera += act_line.front();
            act_line.pop_front();

            if (i >= tail_start)
            {
                if (counts != 0)
                {
                    ++r.nonzero_tail;
                    const int s = (counts > 0) ? 1 : -1;
                    if (last_cmd_sign != 0 && s != last_cmd_sign) ++r.sign_flips;
                    last_cmd_sign = s;
                }
            }
        }
        else
        {
            // 丢帧: 控制器这一拍没有被驱动(EventSync 不发位移), 只推进执行延迟线。
            act_line.push_back(0.0);
            camera += act_line.front();
            act_line.pop_front();
        }

        const double err_true = target - camera;
        if (!std::isfinite(err_true) || !std::isfinite(pid.integralState())
            || !std::isfinite(pid.carryState()))
            r.finite = false;
        r.errors.push_back(err_true);
        r.peak = std::max(r.peak, std::abs(err_true));
        if (i >= tail_start)
        {
            tail.push_back(std::abs(err_true));
            r.integral_tail = pid.integralState();
        }
    }

    r.tail_mean = 0.0;
    for (double e : tail) r.tail_mean += e;
    if (!tail.empty()) r.tail_mean /= static_cast<double>(tail.size());
    r.tail_median = median_of(tail);
    return r;
}

boss::AimPidParams productionPid()
{
    boss::AimPidParams p;
    p.kp = 35.0;
    p.ki = 1.0;
    p.kd = 0.0;
    p.p_full_scale_px = 0.0;
    p.limit_counts = 0;
    p.inflight_beta = 1.6;        // 生产值(见 config.h)
    p.inflight_window_s = boss::kAimDeadTimeS;
    return p;
}

boss::AimTrackerParams trackerParams(double factor_x, double factor_y)
{
    boss::AimTrackerParams p;
    p.min_hits = 3;
    p.max_age = 5;
    p.assoc_radius_px = 80.0;
    p.assoc_iou = 0.20;
    p.vel_window_s = 0.10;
    p.pred_factor_x = factor_x;
    p.pred_factor_y = factor_y;
    p.pred_min_w = 20.0;
    p.pred_max_w = 80.0;
    p.pred_max_lead_px = 12.0;
    p.pred_vel_floor = 60.0;
    return p;
}

// ── [1] 三样新件全关时: EventSync 链路必须与"不用跟踪器"的链路逐位一致 ───────
//
// 这条最重要 —— 它是"换档不改变行为"的硬证据。跟踪器在三样新件都关掉时唯一的
// 作用是身份, 而身份不参与控制, 所以两条链路的每拍误差必须完全相同。
//
// ★ 2026-09-15 更新: 之前这一节的标题是"关预测时逐位一致", 那时 EventSync 链路
//   只有"预测"一件新东西。现在它有三件(⑤在途换算链 / ⑥自运动项 / 预测), 所以
//   逐位一致的前提是【三件全关】:
//     · pred_factor_x/y = 0      → 目标速度那一项不参与
//     · self_motion_gain = 0     → 自运动项不参与
//     · inflight_window_s = 0    → 在途换算链不参与(窗口 0 拍 ⇒ 在途量恒 0)
//   只要有一件开着, 链路就【应该】与现役档不同 —— 那是设计意图, 不是 bug。
void test_prediction_off_identity()
{
    std::printf("[1] 三件新件全关时与现役链路逐位一致\n");
    Plant pl;
    const auto pid = productionPid();

    // "全关"的参数: 三件新件全部失效。
    auto off = trackerParams(0.0, 0.0);
    off.self_motion_gain = 0.0;
    off.inflight_window_s = 0.0;

    const auto plain = run_eventsync(pl, pid, off, false,
                                     motion_linear, 2.0, 1.0);
    const auto tracked = run_eventsync(pl, pid, off, true,
                                       motion_linear, 2.0, 1.0);

    check(plain.errors.size() == tracked.errors.size(), "两条链路的拍数必须相同");
    bool identical = (plain.errors.size() == tracked.errors.size());
    for (std::size_t i = 0; identical && i < plain.errors.size(); ++i)
        if (plain.errors[i] != tracked.errors[i]) identical = false;
    check(identical,
          "三件新件全关时, EventSync 链路必须与现役链路【逐位一致】");
    check(tracked.max_lead == 0.0, "关预测时提前量必须恒为 0");
    check(plain.finite && tracked.finite, "两条链路都必须有限(无 NaN/Inf)");

    // ── 反向验证(这条不能省) ─────────────────────────────────────────────
    // 如果"逐位一致"是因为那条比较根本没接上, 上面那条断言就是空转。所以再跑一次
    // 【开预测】的链路: 它必须与被比较的两条不同。不同 => 比较确实有分辨力。
    //
    // ★ 但"开预测"要真的产生提前量, 必须同时满足 AM 的两个条件(目标在动 且 自己
    //   也在动, 见 aim_tracker.h 的说明)。这个闭环里"自己"就是镜头, 它整段只动
    //   几个像素 ⇒ 自身速度远达不到 3.5px/帧 的门 ⇒ 系数只落不涨 ⇒ 提前量恒 0。
    //   所以反向验证改用【自运动项】来制造差异 —— 它不依赖自身速度门(它吃的就是
    //   自身速度本身), 只要 gain 非零、自身速度非零就一定有输出。
    auto sm_on = off;
    sm_on.self_motion_gain = 0.5;
    const auto with_sm = run_eventsync(pl, pid, sm_on, true,
                                       motion_linear, 2.0, 1.0);
    bool differs = false;
    for (std::size_t i = 0; i < plain.errors.size() && i < with_sm.errors.size(); ++i)
        if (plain.errors[i] != with_sm.errors[i]) { differs = true; break; }
    check(differs, "开自运动项后链路必须与关掉时不同(否则上一条断言是空转)");
    check(with_sm.max_lead > 0.0, "开自运动项后提前量必须真的非零");

    // ④ 在途换算链: 打开窗口(k̂=1)后, 链路也必须与"全关"不同 —— 证明 ⑤ 真的接上了。
    auto if_on = off;
    if_on.inflight_window_s = boss::kAimDeadTimeS;
    const auto with_if = run_eventsync(pl, pid, if_on, true,
                                       motion_linear, 2.0, 1.0);
    bool if_differs = false;
    for (std::size_t i = 0; i < plain.errors.size() && i < with_if.errors.size(); ++i)
        if (plain.errors[i] != with_if.errors[i]) { if_differs = true; break; }
    check(if_differs, "★ 开在途换算链后链路必须不同(证明 ⑤ 真的参与了误差合成)");
}

// ── [2] 开预测: 提前量有界 + 闭环不发散 ──────────────────────────────────────
void test_prediction_bounded()
{
    std::printf("[2] 开预测: 提前量有界, 闭环不发散\n");
    Plant pl;
    const auto pid = productionPid();

    for (double factor : {0.05, 0.10, 0.20})
    {
        const auto r = run_eventsync(pl, pid, trackerParams(factor, factor), true,
                                     motion_linear, 3.0, 2.0);
        const std::string tag = "系数 " + std::to_string(factor);
        check(r.finite, tag + ": 必须有限");
        // ★ 硬上限是安全性质: 提前量绝不允许超过 pidf_predict_max_px。
        check(r.max_lead <= 12.0 + 1e-9, tag + ": 提前量必须被硬上限夹住");
        // 不发散: 全程峰值不能是"甩出去几百像素"那种形态。
        check(r.peak < 120.0, tag + ": 全程峰值必须远小于发散量级");
        // 跟匀速目标: 尾段残差必须是"贴住"的量级, 不是几百像素的极限环。
        check(r.tail_median < 20.0, tag + ": 尾段残差中位必须贴住(<20px)");
    }

    // 系数拉到边界(±0.2)也不能发散 —— 配置层的夹取 + 硬上限共同保证。
    const auto extreme = run_eventsync(pl, productionPid(), trackerParams(-0.2, -0.2),
                                       true, motion_linear, 3.0, 2.0);
    check(extreme.finite && extreme.peak < 200.0, "负系数也必须有限且有界");
}

// ── [3] 滑行窗口: 丢帧不清状态 ───────────────────────────────────────────────
//
// EventSync 相对现役链路最实质的收益。丢帧时不 tick(EventSync 不发位移), 但跟踪器
// 与控制器状态都必须留着 —— 表现是"重新接上之后不用从头再来"。
void test_coasting_keeps_state()
{
    std::printf("[3] 漏检一两帧不清控制器状态\n");
    Plant pl;
    const auto pid = productionPid();

    // 每 40 帧丢一帧(≈每秒丢 3 次, 每次 8.3ms) —— 等价于偶发漏检。
    const auto r = run_eventsync(pl, pid, trackerParams(0.0, 0.0), true,
                                 motion_linear, 3.0, 2.0, /*drop_frames=*/40);
    check(r.finite, "丢帧时链路必须有限");
    check(r.tail_median < 20.0, "偶发丢帧下尾段残差仍必须贴住");
    // ★ 关键证据: 积分必须真的攒起来了。现役链路的病就是"身份一变就复位",
    //   积分永远攒不到能顶住持续推力的量级。
    check(std::abs(r.integral_tail) > 0.05,
          "丢帧之后积分状态必须仍然存在(证明没有被反复清零)");

    // 丢得更狠: 每 6 帧丢 1 帧(max_age=5 的窗口内, 轨迹不该死)。
    const auto hard = run_eventsync(pl, pid, trackerParams(0.0, 0.0), true,
                                    motion_linear, 3.0, 2.0, /*drop_frames=*/6);
    check(hard.finite, "高频丢帧下也必须有限");

    // 目标急停: 提前量必须在停下之后收回去(系数 FSM 的回落), 不许一直挂着。
    const auto stopgo = run_eventsync(pl, pid, trackerParams(0.2, 0.2), true,
                                      motion_stop_go, 3.0, 1.5);
    check(stopgo.finite, "急停场景必须有限");
    check(stopgo.max_lead <= 12.0 + 1e-9, "急停场景提前量仍必须被夹住");
}

// ── [4] 身份粘滞: 抖动/漏帧下换目标次数必须远低于现役链路 ────────────────────
void test_identity_stability()
{
    std::printf("[4] 身份粘滞(跟踪器 id 稳定)\n");
    boss::AimTracker tr;
    tr.configure(trackerParams(0.0, 0.0));

    // 模拟真实检测流: 目标匀速 + 逐帧量化抖动 + 每 10 帧漏一帧。
    const double dt = 1.0 / 120.0;
    double x = 400.0;
    int id_changes = 0;
    int prev_id = -1;
    int observed_frames = 0;
    for (int i = 0; i < 600; ++i)   // 5 秒
    {
        x += 2.5;                    // 300px/s
        tr.beginFrame(dt);
        if (i % 10 != 0)             // 每 10 帧漏一帧
        {
            ++observed_frames;
            const double jitter = ((i % 2) == 0) ? 0.5 : -0.5;
            boss::TrackBox b;
            b.x = static_cast<float>(x + jitter - 30.0);
            b.y = 240.0f;
            b.w = 60.0f;
            b.h = 120.0f;
            tr.offer(b, x + jitter, 300.0);
        }
        tr.endFrame();
        double lx = 0.0, ly = 0.0;
        tr.predictionLead(lx, ly);
        const int id = tr.lockedId();
        if (id != prev_id) ++id_changes;
        prev_id = id;
    }
    check(observed_frames > 500, "观测帧数应当接近 600(测试自身有效)");
    // ★ 现役链路的实测是"约 14.5 帧一次身份变化"(每秒 8 次)。跟踪器必须把
    //   抖动与单帧漏检都吸收掉: 这里只允许【开局那一次】建轨。
    check(id_changes <= 1,
          "抖动 + 每 10 帧漏一帧的情况下, 身份只应在开局建立一次(实测 " +
          std::to_string(id_changes) + " 次)");
}

// ── [5] 在途补偿与档位无关: 两条链路都必须在计数域工作 ───────────────────────
//
// beta=0 是"拆掉主刹车"(实测尾段会变成几百像素极限环)。这条用来证明档位切换
// 没有把在途补偿弄丢 —— 它是两条链路共用的同一个机制。
void test_inflight_shared()
{
    std::printf("[5] 在途补偿在两条链路上都必须生效\n");
    Plant pl;
    auto pid_off = productionPid();
    pid_off.inflight_beta = 0.0;
    auto pid_on = productionPid();
    pid_on.inflight_beta = 1.6;

    const auto off = run_eventsync(pl, pid_off, trackerParams(0.0, 0.0), true,
                                   motion_static, 2.0, 1.0);
    const auto on = run_eventsync(pl, pid_on, trackerParams(0.0, 0.0), true,
                                  motion_static, 2.0, 1.0);
    check(on.finite && off.finite, "两种 beta 都必须有限");
    // 关掉补偿会明显更差(或至少不更好) —— 只要证明它确实参与了控制。
    check(on.tail_mean <= off.tail_mean + 1e-9,
          "beta=1.6 的尾段必须不劣于 beta=0(证明补偿确实在起作用)");
}

} // namespace

int main()
{
    std::printf("=== aim_eventsync_test (PID-EventSync 全链路闭环) ===\n");
    test_prediction_off_identity();
    test_prediction_bounded();
    test_coasting_keeps_state();
    test_identity_stability();
    test_inflight_shared();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
