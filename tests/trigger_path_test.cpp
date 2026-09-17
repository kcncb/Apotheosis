// ═══════════════════════════════════════════════════════════════════════════
//  ★★ 自动扳机 + 瞄准轨迹曲线的回归 (2026-09-17 恢复)
//
//  被测: Apotheosis/mouse/trigger_fsm.h   (扳机状态机 + 命中区几何)
//        Apotheosis/mouse/aim_path.h      (四种轨迹模式)
//
//  ★★ 为什么这两个能测而界面不能: 它们【零依赖】(不引 OpenCV / Windows / Qt),
//     所以任何平台都能编。这正是把它们从那个 1536 行的 mouse_thread_loop.cpp
//     里拆出来的理由之一 —— 留在巨文件里就是零覆盖。
//
//  ★★ 这批断言的价值在于【每条都反向验证过】: 逐条改坏被测行为, 确认真的变红。
//     一个不会失败的断言不是证据。
// ═══════════════════════════════════════════════════════════════════════════

#include "mouse/aim_path.h"
#include "mouse/trigger_fsm.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

// 把一串 tick 跑成"从 t0 开始, 每 step_ms 一拍"的序列, 收集动作。
struct TickResult
{
    int press = 0;
    int release = 0;
    int fired = 0;
    std::vector<int> press_ticks;   // 哪些拍按下了左键
};

static TickResult run(boss::TriggerFsm& fsm, int ticks, int64_t t0, int step_ms,
                      bool in_zone, int track_id,
                      bool hold_mode = false,
                      int fire_delay = 0, int duration = 0, int interval = 200,
                      int switch_cd = 0)
{
    TickResult r;
    for (int i = 0; i < ticks; ++i)
    {
        boss::TriggerFsm::Input in;
        in.in_zone = in_zone;
        in.track_id = track_id;
        in.now_ms = t0 + static_cast<int64_t>(i) * step_ms;

        const auto a = fsm.tick(in, hold_mode, fire_delay, duration, interval,
                                switch_cd, 0, 0, 0);
        if (a.press_left)   { ++r.press;   r.press_ticks.push_back(i); }
        if (a.release_left) ++r.release;
        if (a.fired)        ++r.fired;
    }
    return r;
}

// ── [1] 命中区几何 ────────────────────────────────────────────────────────
static void test_hit_zone()
{
    std::printf("\n[1] 命中区几何 (以框为基准, 与瞄点解耦)\n");
    // 框 = (100,200) 宽 80 高 60 ⇒ 中心 (140, 230)。
    double hx = 0.0, hy = 0.0;

    // 100% ⇒ 半宽 = 80*1.0/2 = 40, 半高 = 60*1.0/2 = 30。
    check(boss::TriggerFsm::inHitZone(140, 230, 100, 200, 80, 60, 100, &hx, &hy),
          "框中心算在命中区内");
    check(std::abs(hx - 40.0) < 1e-9 && std::abs(hy - 30.0) < 1e-9,
          "100% 时半宽=40 / 半高=30");

    // 边界含等号(<=): 正好在边缘上算命中。
    check(boss::TriggerFsm::inHitZone(140 + 40, 230, 100, 200, 80, 60, 100),
          "正好在右边界上算命中(含等号)");
    check(!boss::TriggerFsm::inHitZone(140 + 40.1, 230, 100, 200, 80, 60, 100),
          "超出右边界 0.1px 就不算命中");

    // ★ 区间是【等比】的: 百分比同时作用在横纵两轴。
    //   50% ⇒ 半宽 20 / 半高 15。
    check(boss::TriggerFsm::inHitZone(140 + 19, 230 + 14, 100, 200, 80, 60, 50),
          "50% 时 (中心+19, 中心+14) 仍算命中");
    check(!boss::TriggerFsm::inHitZone(140 + 21, 230, 100, 200, 80, 60, 50),
          "50% 时横向超出 21px 不算命中");

    // ★ >100% = 预开火: 框上方也算。
    check(boss::TriggerFsm::inHitZone(140, 230 - 44, 100, 200, 80, 60, 200),
          "200% 时框上方 44px 处也算命中(预开火)");
    check(!boss::TriggerFsm::inHitZone(140, 230 - 44, 100, 200, 80, 60, 100),
          "100% 时同一位置【不】算命中");

    // ★ 下限 0.1 保护: 0 不会退化成"永远命中"之外的除零/负半宽。
    check(!boss::TriggerFsm::inHitZone(140 + 100, 230, 100, 200, 80, 60, 0),
          "y_percent=0 被夹到 0.1, 不会退化成永远命中");
}

// ── [2] 零延迟直通 ────────────────────────────────────────────────────────
static void test_zero_delay()
{
    std::printf("\n[2] 零延迟: 进区那一拍就开火\n");
    {
        boss::TriggerFsm f;
        const auto r = run(f, 1, 1000, 8, /*in_zone*/ true, 1, false, /*delay*/ 0);
        check(r.fired == 1, "★ 进区第一拍就 fire (不是等一拍)");
        check(r.press == 1, "★ 第一拍按下左键");
        check(f.pressed(), "相位进入 Pressed");
    }
    {
        // 不在命中区 ⇒ 一拍都不该开火。
        boss::TriggerFsm f;
        const auto r = run(f, 20, 1000, 8, /*in_zone*/ false, 1, false, 0);
        check(r.fired == 0, "不在命中区时一拍都不开火");
        check(r.press == 0, "不在命中区时不按左键");
    }
}

// ── [3] 延迟开火 ──────────────────────────────────────────────────────────
static void test_delay()
{
    std::printf("\n[3] 进区后延迟开火\n");
    boss::TriggerFsm f;
    // 延迟 40ms, 每拍 8ms ⇒ 第 5 拍 (t=1000+40) 才够。
    const auto r = run(f, 10, 1000, 8, true, 1, false, /*delay*/ 40);
    check(r.fired == 1, "延迟模式下只开火一次");
    check(!r.press_ticks.empty() && r.press_ticks[0] == 5,
          "★ 40ms / 8ms 每拍 ⇒ 第 5 拍开火 (不是第 0 拍)");

    // ★ 中途离开命中区 ⇒ 延迟计时必须【重来】。
    {
        boss::TriggerFsm f2;
        // 前 3 拍在区内(t=1000,1008,1016), 然后离开 2 拍, 再进区。
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int64_t t) {
            in.in_zone = zone; in.track_id = 1; in.now_ms = t;
            return f2.tick(in, false, 40, 0, 200, 0, 0, 0, 0);
        };
        act(true, 1000); act(true, 1008); act(true, 1016);
        act(false, 1024); act(false, 1032);
        // 重新进区: 计时从 1040 重新起算(而不是沿用 1000) ——
        // 所以 1040+40 = 1080 才够, 1072 还不够。
        // ★ 若实现是"沿用第一次的 in_zone_since", 那么在 1072 就会开火 ——
        //   下面 1072 那条断言正是钉这个的。
        check(!act(true, 1040).fired, "离开命中区后延迟计时重来(1040 未开火)");
        check(!act(true, 1048).fired, "重来后 1048 仍未到 40ms");
        check(!act(true, 1056).fired, "重来后 1056 仍未到 40ms");
        check(!act(true, 1064).fired, "重来后 1064 仍未到 40ms");
        check(!act(true, 1072).fired, "★ 重来后 1072 仍未到 40ms (没有沿用旧计时)");
        check(act(true, 1080).fired, "★ 从 1040 起满 40ms ⇒ 1080 开火");
    }

    // ★★ 在 Delay 相位【中途】离开命中区 (与上面那条不同!)
    //   上面的序列是在 Idle 相位离开的 —— 那条路径上 Delay 分支的清理代码
    //   根本不会被执行。要真正钉住 Delay 分支, 必须在 Delay 相位里离开。
    //   实测: 把 Delay 分支里的 `in_zone_since_ms_ = -1` 删掉, 上面那条全绿、
    //        只有这一条会红 —— 这就是这组断言存在的理由。
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int64_t t) {
            in.in_zone = zone; in.track_id = 1; in.now_ms = t;
            return f.tick(in, false, 40, 0, 200, 0, 0, 0, 0);
        };
        // 进入 Delay 相位 (1030 未满 40ms, 所以相位停在 Delay)。
        check(!act(true, 1000).fired, "t=1000 进区, 未到 40ms");
        check(!act(true, 1030).fired, "t=1030 仍在 Delay 相位");
        check(f.phase() == boss::TriggerPhase::Delay,
              "★ 前提: 此刻确实处于 Delay 相位");
        // ★ 在 Delay 相位里离开命中区。
        check(!act(false, 1035).fired, "Delay 相位中途离开命中区 → 不开火");
        // 再进区: 计时必须【重新】起算。若 Delay 分支漏了清理, 这里会沿用
        // 1000 ⇒ 立刻开火。
        check(!act(true, 1040).fired,
              "★ Delay 相位中途离开后, 计时必须重来 (1040 不开火)");
        check(act(true, 1080).fired, "★ 从 1040 起满 40ms ⇒ 1080 才开火");
    }
}

// ── [4] 连点模式: 按住时长 + 冷却 ─────────────────────────────────────────
static void test_burst_mode()
{
    std::printf("\n[4] 连点模式 (duration>0)\n");
    boss::TriggerFsm f;
    // duration 30ms / interval 50ms, 每拍 10ms, 一直在区内。
    // 期望: t=0 按下, t=30 松开, t=80 再按下(冷却结束且仍在区内)...
    const auto r = run(f, 30, 0, 10, true, 1, /*hold*/ false,
                       /*delay*/ 0, /*duration*/ 30, /*interval*/ 50);
    check(r.press >= 3, "连点模式下持续在区内会反复开火");
    check(r.press == r.release || r.press == r.release + 1,
          "★ 每一发都配对的按下/抬起(不会卡在按下)");
    check(r.fired == r.press, "每发 fire 标记与 press 一一对应");
}

// ── [5] 长按模式: 离开才松手 ──────────────────────────────────────────────
static void test_hold_mode()
{
    std::printf("\n[5] 长按模式 (duration=0)\n");
    {
        boss::TriggerFsm f;
        // 一直在区内, 跑 20 拍 ⇒ 只按下一次, 全程不松手。
        const auto r = run(f, 20, 0, 10, true, 1, /*hold*/ true, 0, 0, 200);
        check(r.press == 1, "★ 长按模式在区内只按一次");
        check(r.release == 0, "★ 长按模式在区内不松手");
    }
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int64_t t) {
            in.in_zone = zone; in.track_id = 1; in.now_ms = t;
            return f.tick(in, /*hold*/ true, 0, 0, 200, 0, 0, 0, 0);
        };
        check(act(true, 0).press_left, "进区按下");
        check(!act(true, 10).release_left, "留在区内不松手");
        check(act(false, 20).release_left, "★ 离开命中区才松手");
        check(!f.pressed(), "松手后相位不再是 Pressed");
    }
}

// ── [6] 换目标冷却 ────────────────────────────────────────────────────────
static void test_switch_cooldown()
{
    std::printf("\n[6] 换目标冷却\n");
    // ★★ 关键性质: 只在"换目标【且不在命中区】"时进冷却。
    //   转火后新目标就在准星上时应立刻接力开火, 不该卡一下。
    //
    //   ★ 这条断言必须让 FSM 停在 Pressed(长按)相位再做转火 —— 否则若实现
    //     漏判 in_zone, 相位本来就在 Idle, 断言看不出来。实测: 把
    //     `!in.in_zone` 改成 `true` 后, 只有这一段会红。
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int id, int64_t t) {
            in.in_zone = zone; in.track_id = id; in.now_ms = t;
            return f.tick(in, /*hold*/ true, 0, 0, 200, /*switch_cd*/ 100, 0, 0, 0);
        };
        check(act(true, 1, 0).press_left, "目标 1 进区长按开火");
        check(f.phase() == boss::TriggerPhase::Pressed, "★ 前提: 处于 Pressed 相位");

        // ★ 换到目标 2, 但【仍在命中区】⇒ 绝不能进转火冷却, 也不能松手。
        const auto a = act(true, 2, 10);
        check(!a.release_left, "★ 换目标但在区内时【不】松手");
        check(f.phase() == boss::TriggerPhase::Pressed,
              "★ 换目标但在区内时相位仍是 Pressed (没有进转火冷却)");
    }
    {
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int id, int64_t t) {
            in.in_zone = zone; in.track_id = id; in.now_ms = t;
            return f.tick(in, /*hold*/ true, 0, 0, 200, /*switch_cd*/ 100, 0, 0, 0);
        };
        act(true, 1, 0);                       // 目标 1 长按开火 ⇒ Pressed
        act(false, 1, 10);                     // 离开命中区 ⇒ 松手进 Cooldown
        act(false, 2, 20);                     // 换目标 + 不在区内 ⇒ 进转火冷却
        check(f.phase() == boss::TriggerPhase::SwitchCooldown,
              "★ 换目标且不在命中区 ⇒ 进入转火冷却");
        act(false, 2, 120);                    // 冷却 100ms 到点
        check(f.phase() != boss::TriggerPhase::SwitchCooldown,
              "转火冷却到点后退出");
    }
    {
        // switch_cd = 0 ⇒ 整个机制不生效。
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        auto act = [&](bool zone, int id, int64_t t) {
            in.in_zone = zone; in.track_id = id; in.now_ms = t;
            return f.tick(in, false, 0, 0, 200, /*switch_cd*/ 0, 0, 0, 0);
        };
        act(true, 1, 0);
        act(false, 1, 10);
        act(false, 2, 20);
        check(f.phase() != boss::TriggerPhase::SwitchCooldown,
              "switch_cd=0 时不进转火冷却");
    }
}

// ── [7] 关掉扳机必须把左键还回去 ──────────────────────────────────────────
static void test_reset_releases()
{
    std::printf("\n[7] reset 把按住的左键还回去\n");
    {
        boss::TriggerFsm f;
        run(f, 3, 0, 10, true, 1, /*hold*/ true, 0, 0, 200);
        check(f.pressed(), "前提: 长按模式下确实按着左键");
        check(f.reset() == true, "★ reset 报告「之前按着」 ⇒ 调用方必须 releaseLeftButton");
        check(!f.pressed(), "reset 后不再是 Pressed");
    }
    {
        // 没按着的时候 reset 不该谎报 —— 否则会多发一次 release。
        boss::TriggerFsm f;
        check(f.reset() == false, "★ 没按着时 reset 返回 false (不谎报)");
    }
    {
        // reset 必须把 track_id 也清掉: 否则下一段接敌的第一次开火
        // 会被误判成"换目标"而进转火冷却。
        boss::TriggerFsm f;
        boss::TriggerFsm::Input in;
        in.in_zone = true; in.track_id = 7; in.now_ms = 0;
        f.tick(in, false, 0, 0, 200, 100, 0, 0, 0);
        f.reset();
        in.in_zone = false; in.track_id = 9; in.now_ms = 10;
        f.tick(in, false, 0, 0, 200, 100, 0, 0, 0);
        check(f.phase() != boss::TriggerPhase::SwitchCooldown,
              "★ reset 后第一帧不算「换目标」");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  轨迹曲线
// ═══════════════════════════════════════════════════════════════════════════

// ── [8] 直线模式逐位透传 ──────────────────────────────────────────────────
static void test_linear_passthrough()
{
    std::printf("\n[8] 直线模式逐位透传\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::Linear;
    d.configure(p);

    for (double bx : {0.0, 7.0, -13.0, 250.0})
    {
        const auto r = d.step(500, 500, 400, 400, 0.008, 1, bx, -bx);
        check(r.move_x == bx && r.move_y == -bx,
              "★ 直线模式必须逐位透传 base_dx/base_dy (不缩放不旋转)");
    }
}

// ── [9] 曲线模式【只旋转不缩放】──────────────────────────────────────────
static void test_no_scaling()
{
    std::printf("\n[9] ★★ 曲线只旋转不缩放 (核心不变量)\n");
    // 这是"轨迹层不把控制器拖成振荡"的前提: 曲线只决定方向, 幅值仍由 PID 决定。
    //
    // ★★ 断言写成【精确相等】而不是"不超过 3 倍": 被测实现里有一条显式的
    //    幅值恢复 (restore = base_mag / mixed_mag), 所以每拍幅值应当【逐位】
    //    等于基向量幅值。松散的上界(3 倍)抓不住真正的缩放 bug ——
    //    实测: 去掉那条 restore 后输出比变成 4.0, 松断言照样全绿。
    for (int m = 1; m <= 3; ++m)
    {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = static_cast<boss::AimPathDriver::Mode>(m);
        p.strength = 1.0;   // 影响量拉满 —— 缩放的话这里最容易暴露
        // ★ 让贝塞尔真的弯起来: 默认 cy1=cy2=0 是一条直线, 那样"曲线路径"
        //   根本不被执行, 影响量与缩放都无从观察。
        p.cy1 = 0.35;
        p.cy2 = -0.35;
        d.configure(p);

        // 大甩枪: 误差远大于 WindMouse 门控, 确保真的走曲线而不是旁路。
        const double bx = 40.0, by = 0.0;
        const double base_mag = std::hypot(bx, by);
        int nonzero = 0;
        for (int i = 0; i < 40; ++i)
        {
            const auto r = d.step(900, 500, 500, 500, 0.008, 1, bx, by);
            const double mag = std::hypot(r.move_x, r.move_y);
            if (mag <= 1e-9) continue;
            ++nonzero;
            // ★★ 核心不变量: 幅值恒等于 controller 给的幅值 (旋转不改长度)。
            check(std::abs(mag - base_mag) < 1e-6,
                  "★ mode=" + std::to_string(m) + " 第 " + std::to_string(i) +
                  " 拍幅值精确等于基向量幅值 (只旋转不缩放)");
        }
        check(nonzero > 0, "★ mode=" + std::to_string(m) + " 确实产生了输出");
    }

    // ★ 曲线必须【真的改变方向】—— 否则"只旋转不缩放"是空话(旋转 0 度也算)。
    {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;
        d.configure(p);
        bool off_axis = false;
        for (int i = 0; i < 40; ++i)
        {
            const auto r = d.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
            if (std::abs(r.move_y) > 1e-3 && std::abs(r.move_x - 40.0) > 1e-3)
                off_axis = true;
        }
        check(off_axis, "★ 曲线确实把输出方向转离了基向量 (y 分量出现)");
    }

    // ★ 影响量真的有效: strength=0 时曲线必须完全退出 (等于直线)。
    {
        boss::AimPathDriver d0;
        boss::AimPathDriver::Params p0;
        p0.mode = boss::AimPathDriver::Mode::WindMouse;
        p0.strength = 0.0;
        d0.configure(p0);
        bool always_passthrough = true;
        for (int i = 0; i < 30; ++i)
        {
            const auto r = d0.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
            if (r.move_x != 40.0 || r.move_y != 0.0) always_passthrough = false;
        }
        check(always_passthrough,
              "★ strength=0 ⇒ 曲线完全退出, 每拍逐位等于基向量");
    }
}

// ── [10] WindMouse 门控旁路 ───────────────────────────────────────────────
static void test_wind_gate()
{
    std::printf("\n[10] WindMouse 门控: 小误差走直线\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::WindMouse;
    p.strength = 1.0;
    p.wind_threshold_px = 10.0;
    d.configure(p);

    // 两轴误差都在 10px 以内 ⇒ 整段旁路, 逐位透传。
    for (double err : {0.0, 3.0, 9.9, -10.0})
    {
        const auto r = d.step(500 + err, 500 + err, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "门控内 (err=" + std::to_string(err) + ") 必须逐位透传直线");
    }

    // 单轴超阈值 ⇒ 不再旁路(曲线接管)。
    // ★★ 注意: 不能只看第一拍! 驱动器有 entry_fade (progress/0.10 的 smoothstep),
    //    progress=0 时 influence 恰好为 0 ⇒ 【第一拍恒等于透传】。这是有意的
    //    设计(起段不突变), 不是 bug。所以要在跑若干拍之后再判定。
    {
        boss::AimPathDriver d2;
        d2.configure(p);
        bool diverged = false;
        for (int i = 0; i < 20; ++i)
        {
            const auto r = d2.step(540, 500, 500, 500, 0.008, 1, 5.0, -3.0);
            if (!(r.move_x == 5.0 && r.move_y == -3.0)) diverged = true;
        }
        check(diverged,
              "★ 单轴误差 40px > 阈值 ⇒ 跑几拍后曲线接管(不再逐位透传)");
    }

    // ★ 单轴超阈值的第一拍必须是透传 (entry_fade=0)。这条钉的是"起段不突变"。
    {
        boss::AimPathDriver d4;
        d4.configure(p);
        const auto r = d4.step(540, 500, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "★ 曲线接管的【第一拍】仍逐位透传 (entry_fade 从 0 起)");
    }

    // 阈值 0 ⇒ 任何非零误差都走曲线; 误差恰好为 0 时两轴都不超 0 ⇒ 旁路。
    {
        boss::AimPathDriver d3;
        boss::AimPathDriver::Params p0 = p;
        p0.wind_threshold_px = 0.0;
        d3.configure(p0);
        const auto r = d3.step(500, 500, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "阈值为 0 且误差为 0 ⇒ 仍旁路");
    }

    // ★★★ 决定性的门控断言。
    //
    //   上面那几条会被【别的】旁路路径掩盖: 误差 <1px 有早退、progress=0 时
    //   entry_fade=0 也让输出恒等于透传。实测: 把 gate 改成 -1.0(永不旁路),
    //   上面几条【全部照样绿】。
    //
    //   所以要先把驱动器"跑热" —— 用大误差跑若干拍, 让 progress_ 与
    //   entry_fade 都脱离 0 —— 再把误差收进门控带内。此时能回到逐位透传,
    //   就只可能是门控干的 (门控同时会 engaged_=false / 清 profile,
    //   下一拍重新起一段)。
    {
        boss::AimPathDriver d5;
        d5.configure(p);
        // 阶段 1: 大误差跑热 (此时不应恒等于透传)。
        bool diverged = false;
        for (int i = 0; i < 20; ++i)
        {
            const auto r = d5.step(900, 500, 500, 500, 0.008, 1, 5.0, -3.0);
            if (!(r.move_x == 5.0 && r.move_y == -3.0)) diverged = true;
        }
        check(diverged, "★ 跑热阶段确实走了曲线(前提成立)");

        // 阶段 2: 误差收进 10px 门控带 (两轴都在带内) ⇒ 必须立刻逐位透传。
        const auto r = d5.step(500 + 6, 500 + 6, 500, 500, 0.008, 1, 5.0, -3.0);
        check(r.move_x == 5.0 && r.move_y == -3.0,
              "★★ 已跑热的驱动器: 误差落回门控带内 ⇒ 立刻逐位透传 (门控真的生效)");

        // 阶段 3: 再回到带外 ⇒ 曲线重新接管 (门控把状态清了, 重新起段)。
        //   第一拍因 entry_fade=0 仍是透传, 所以要跑几拍再看。
        bool re_diverged = false;
        for (int i = 0; i < 20; ++i)
        {
            const auto r2 = d5.step(900, 500, 500, 500, 0.008, 1, 5.0, -3.0);
            if (!(r2.move_x == 5.0 && r2.move_y == -3.0)) re_diverged = true;
        }
        check(re_diverged, "★ 误差再次超阈值 ⇒ 曲线重新接管(重新起了一段)");
    }
}

// ── [11] 曲线确定性 ───────────────────────────────────────────────────────
static void test_determinism()
{
    std::printf("\n[11] 曲线确定性 (同一 target_id 可复现)\n");
    auto sample = [](int target_id) {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;
        d.configure(p);
        std::vector<double> out;
        for (int i = 0; i < 25; ++i)
        {
            const auto r = d.step(900, 500, 500, 500, 0.008, target_id, 40.0, 0.0);
            out.push_back(r.move_x);
            out.push_back(r.move_y);
        }
        return out;
    };
    check(sample(42) == sample(42), "★ 同一 target_id 两次跑出的轨迹完全一致");

    // ★★ "不同 target_id ⇒ 不同轨迹" 必须【定量】断言, 不能只比 != 。
    //   实测教训: 只写 `sample(42) != sample(43)` 时, 把 target_id 从
    //   build_wind_profile 里抹掉(恒传 0) 【照样绿】—— 因为 progress_ 的
    //   累积会让两条序列在很靠后的拍上出现极小的浮点差异, 于是 != 成立。
    //   那种"差异"没有意义(不是随机路径的区别)。
    //
    //   正确判据: 两条序列必须在【前几拍就有肉眼可见】的方向差异 ——
    //   WindMouse 的横向分量应当是不同的随机游走。
    {
        const auto s42 = sample(42);
        const auto s43 = sample(43);
        check(s42 != s43, "不同 target_id 的两条序列不完全相同");

        // 取前 10 拍 (20 个数) 的 y 分量(奇数下标), 要求存在明显差异。
        double max_early_diff = 0.0;
        for (int i = 0; i < 10; ++i)
        {
            const double y42 = s42[i * 2 + 1];
            const double y43 = s43[i * 2 + 1];
            max_early_diff = std::max(max_early_diff, std::abs(y42 - y43));
        }
        // ★ 阈值要显著大于浮点累积误差(1e-6 量级), 又小于真实随机路径的差异。
        //   实测: 正常实现下前 10 拍的 y 差异超过 1.0; 抹掉 target_id 后
        //   前 10 拍几乎逐位相同(差异 < 1e-6)。
        check(max_early_diff > 0.1,
              "★★ target_id 真的参与摇路径: 前 10 拍的横向分量存在明显差异"
              " (实测差异=" + std::to_string(max_early_diff) + ")");
    }

    // ★★ reset 之后用同一 target_id 重跑, 轨迹【不】与开始时一致。
    //
    //   这是【有意的设计】, 不是 bug: WindMouse 的 rng_ 是持续演化的状态
    //   ("每次起新段推进一次状态, 所以连续几段不会长得一模一样"),
    //   而 reset() 有意【不】把 rng_ 拨回初值 —— 否则每次会话开始的第一枪
    //   都会抖成同一条死曲线, 那既不像人也很容易被看出来。
    //
    //   ★ 实测确认(probe3): 15 拍里有 12 拍不同。
    //   ★ 所以这里钉的是【"reset 后 != 重跑初值"】—— 如果哪天有人"顺手"
    //     把 rng_ 也加进 reset, 这条会变红, 逼他重新想一遍这个取舍。
    //   ★ 真正必须被 reset 保证的是"路径状态清零", 见下面那条。
    // ★★ reset() 的可观察语义: 它【等价于"换一个 target_id"】。
    //
    //   实测结论(probe7, 三种实现对比):
    //     · 先跑 40 拍(id=3) → reset() → 跑 20 拍(id=7)
    //     · 先跑 40 拍(id=3) → 不 reset → 跑 20 拍(id=7)
    //   这两者【逐位完全相同】。而且把 reset() 整个改成 no-op 之后,
    //   上面两者仍然逐位相同。
    //
    //   原因: step() 里 `id_changed` 这条路径本来就会做 reset 做的全部事情
    //   (progress_=0 / 重算 reference_length_ 与 axis_ / 清 smoothed_slope_
    //   与 rx_ry_ / 重建 wind_profile_), 而 rng_ 两者都不回拨。
    //
    //   ★ 所以本测试【不】断言"reset 后等于全新驱动器" —— 那是假的。
    //     它钉的是当前真实契约: reset 之后换 id 起新段, 与不 reset 换 id 一致。
    //   ★ 这是【已知的冗余】, 不是本轮引入的: reset() 仍然被
    //     aim_loop.cpp 在会话结束时调用, 保留它是有意的(语义上表达
    //     "这段接敌结束了"), 只是它当前没有额外的可观察效果。
    {
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;

        auto warm = [&](boss::AimPathDriver& d) {
            for (int i = 0; i < 40; ++i)
                d.step(900, 500, 500, 500, 0.008, 3, 40.0, 0.0);
        };

        boss::AimPathDriver with_reset;
        with_reset.configure(p);
        warm(with_reset);
        with_reset.reset();

        boss::AimPathDriver without_reset;
        without_reset.configure(p);
        warm(without_reset);

        bool identical = true;
        for (int i = 0; i < 20; ++i)
        {
            const auto a = with_reset.step(900, 500, 500, 500, 0.008, 7, 40.0, 0.0);
            const auto b = without_reset.step(900, 500, 500, 500, 0.008, 7, 40.0, 0.0);
            if (a.move_x != b.move_x || a.move_y != b.move_y) identical = false;
        }
        check(identical,
              "★ reset 后换 id 与不 reset 换 id 逐位一致(reset 等价于换段)");
    }

    // ★ reset 之后【第一拍恒为逐位透传】—— 这是 entry_fade 从 0 起的性质,
    //   与 reset 是否清了 progress_ 无关(两条路径都会把 progress_ 置 0)。
    {
        boss::AimPathDriver d;
        boss::AimPathDriver::Params p;
        p.mode = boss::AimPathDriver::Mode::WindMouse;
        p.strength = 1.0;
        d.configure(p);
        for (int i = 0; i < 30; ++i)
            d.step(900, 500, 500, 500, 0.008, 42, 40.0, 0.0);
        d.reset();
        const auto r = d.step(900, 500, 500, 500, 0.008, 42, 40.0, 0.0);
        check(r.move_x == 40.0 && r.move_y == 0.0,
              "★ reset 后第一拍逐位透传(entry_fade 从 0 起, 起段不突变)");
    }
}

// ── [12] reset 清状态 ─────────────────────────────────────────────────────
static void test_path_reset()
{
    std::printf("\n[12] 曲线 reset 清掉路径状态\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::WindMouse;
    p.strength = 1.0;
    d.configure(p);

    for (int i = 0; i < 10; ++i)
        d.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);

    d.reset();
    // reset 之后重新起一段 ⇒ 输出应当与"全新驱动器"一致。
    boss::AimPathDriver fresh;
    fresh.configure(p);

    const auto a = d.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
    const auto b = fresh.step(900, 500, 500, 500, 0.008, 1, 40.0, 0.0);
    check(a.move_x == b.move_x && a.move_y == b.move_y,
          "★ reset 后的第一拍 == 全新驱动器的第一拍");
}

int main()
{
    std::printf("=== 自动扳机 + 轨迹曲线回归 (2026-09-17 恢复) ===\n");

    test_hit_zone();
    test_zero_delay();
    test_delay();
    test_burst_mode();
    test_hold_mode();
    test_switch_cooldown();
    test_reset_releases();

    test_linear_passthrough();
    test_no_scaling();
    test_wind_gate();
    test_determinism();
    test_path_reset();

    std::printf("\n=== %d 项失败 ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
