// 自动开镜 (mouse/trigger_scope.h) 的回归 —— 仿 AimMagic 的「开火方式」。
//
// 这一层最容易出的事故:
//   ① 右键卡在按下不放(会话停了还在开镜) —— 用户视角就是"鼠标右键坏了";
//   ② 点按模式把 down/up 挤在同一拍里, 游戏收不到这次点击;
//   ③ ready() 在手键不适用时返回 false, 把整个扳机卡死(一枪都打不出去)。
#include "mouse/trigger_scope.h"

#include <cstdio>
#include <string>

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

using boss::ScopeController;

// ── [1] 关闭 / 不适用时绝不碰右键, 且不能卡住扳机 ──────────────────────────
void test_disabled_is_inert()
{
    std::printf("[1] 关闭或不适用时完全不动右键\n");
    ScopeController sc;
    for (int i = 0; i < 5; ++i)
    {
        const auto a = sc.tick(/*in_zone=*/true, /*allowed=*/true, /*mode=*/0, 0, i * 8);
        check(!a.press_right && !a.release_right, "mode=0: 无右键动作");
        check(sc.ready(true, 0, 0, i * 8), "mode=0: ready 恒为 true");
    }

    // 热键自己绑了右键 -> allowed=false: 即使 mode=2 也不能碰右键。
    ScopeController sc2;
    for (int i = 0; i < 5; ++i)
    {
        const auto a = sc2.tick(true, /*allowed=*/false, /*mode=*/2, 0, i * 8);
        check(!a.press_right && !a.release_right, "allowed=false: 无右键动作");
        check(sc2.ready(false, 2, 0, i * 8), "allowed=false: ready 恒为 true");
    }
    check(!sc2.engaged(), "allowed=false: 不进入开镜态");
}

// ── [2] 长按模式: 进区按住, 离区松开, 全程只一次 ───────────────────────────
void test_hold_mode()
{
    std::printf("[2] 长按右键\n");
    ScopeController sc;
    const auto a1 = sc.tick(true, true, 2, 0, 100);
    check(a1.press_right && !a1.release_right, "进区: 按下右键");
    check(sc.ready(true, 2, 0, 100), "按下后立刻 ready");

    for (int i = 0; i < 4; ++i)
    {
        const auto a = sc.tick(true, true, 2, 0, 108 + i * 8);
        check(!a.press_right && !a.release_right, "持续在区: 不重复下发");
    }

    const auto a2 = sc.tick(false, true, 2, 0, 200);
    check(!a2.press_right && a2.release_right, "离区: 松开右键");
    check(!sc.engaged(), "离区后退出开镜态");
    check(!sc.ready(true, 2, 0, 200), "离区后 ready=false(要重新开镜才能开火)");

    const auto a3 = sc.tick(false, true, 2, 0, 208);
    check(!a3.press_right && !a3.release_right, "持续离区: 不重复下发");
}

// ── [3] 点按模式: 一次"点"必须跨两拍, 而且【只点一下】(不收镜) ──────────────
void test_tap_mode_spans_two_ticks()
{
    std::printf("[3] 点按右键只点一下\n");
    ScopeController sc;
    const auto a1 = sc.tick(true, true, 1, 0, 100);
    check(a1.press_right && !a1.release_right, "开的这一拍: 只按下, 不抬起(否则会被游戏合并掉)");

    const auto a2 = sc.tick(true, true, 1, 0, 108);
    check(!a2.press_right && a2.release_right, "下一拍: 补上抬指");

    // 2026-09-12 实机反馈后改: 开镜保持期间绝不再碰右键。
    for (int i = 0; i < 5; ++i)
    {
        const auto a = sc.tick(true, true, 1, 0, 116 + i * 8);
        check(!a.press_right && !a.release_right, "开镜保持: 不再重复点");
    }
    check(sc.engaged(), "仍然处于本次接敌的开镜态");

    // 离开命中区【也不再点一下收镜】—— 开镜状态交给用户自己处理。
    const auto a3 = sc.tick(false, true, 1, 0, 200);
    check(!a3.press_right && !a3.release_right, "离开命中区: 不自动收镜(不再点第二下)");
    check(sc.engaged(), "点按模式下接敌状态保持(同一次接敌里不再点)");

    // 同一次接敌里再回到命中区也不重新点(否则切换开镜的游戏会被来回切)。
    const auto a4 = sc.tick(true, true, 1, 0, 208);
    check(!a4.press_right && !a4.release_right, "同一次接敌内再次进区: 不重新点");
    // 等待闸门从【点下去那一刻】算, 不会因为离开再进区而重新计时。
    check(!sc.ready(true, 1, 200, 208), "再进区: 等待仍从点下去那刻算(还没到点)");
    check(sc.ready(true, 1, 200, 320), "再进区: 到了原定的等待时长即可开火");
}

// ── [3b] 接敌途中复位不重新武装; 新的接敌才会再点一下 ──────────────────────
void test_rearm_only_on_new_engagement()
{
    std::printf("[3b] 只有新接敌才重新点\n");
    ScopeController sc;
    sc.tick(true, true, 1, 0, 100);          // 点一下
    sc.tick(true, true, 1, 0, 108);          // 抬指
    sc.flushUp();                            // 接敌途中复位(丢目标等): 只补抬指

    const auto a = sc.tick(true, true, 1, 0, 200);   // 又锁到目标
    check(!a.press_right, "途中复位后重新锁到目标: 不再点一下");

    // 接敌结束(热键松开/会话结束) -> 下一次接敌重新点。
    sc.forceRelease();
    const auto b = sc.tick(true, true, 1, 0, 300);
    check(b.press_right, "新的接敌: 重新点一下");
}

// ── [4] 开镜后等待闸门 ────────────────────────────────────────────────────
void test_scope_delay_gate()
{
    std::printf("[4] 开镜到开火之间的等待\n");
    // delay = 0 (AM 默认): 开镜那一拍就允许开火。
    ScopeController sc;
    sc.tick(true, true, 2, /*delay=*/0, 777);
    check(sc.ready(true, 2, 0, 777), "delay=0: 开镜当拍即可开火");

    // delay > 0: 必须等够, 第一颗子弹才出去(这段时间是给游戏做开镜过渡的)。
    ScopeController sc2;
    sc2.tick(true, true, 2, 50, 1000);
    check(!sc2.ready(true, 2, 50, 1000), "刚开镜那一拍: 还不许开火");
    check(!sc2.ready(true, 2, 50, 1020), "差 20ms: 还不许开火");
    check(sc2.ready(true, 2, 50, 1050), "到 50ms: 放行");
    check(sc2.ready(true, 2, 50, 1200), "之后一直放行");
}

// ── [5] 右键永远不许卡在按下 ──────────────────────────────────────────────
void test_force_release_never_sticks()
{
    std::printf("[5] 右键绝不卡在按下\n");

    // ★ 实机事故回归: 点按模式下"按下已发、抬指还没发"时接敌结束,
    //   以前会把抬指连同接敌状态一起丢掉 -> 右键卡在按下。
    {
        ScopeController sc;
        const auto down = sc.tick(true, true, 1, 0, 100);   // 发按下, 抬指待发
        check(down.press_right && !down.release_right, "先按下");
        const auto end = sc.forceRelease();                 // 接敌结束(丢目标/退出)
        check(end.release_right, "★ 结束时必须把抬指补上");
        check(!end.press_right, "★ 结束时不许多点一下(否则又是新的按下)");
        check(!sc.engaged(), "结束后退出开镜态");
        const auto again = sc.forceRelease();
        check(!again.press_right && !again.release_right, "重复结束: 无事发生");
    }

    // 接敌【途中】复位同样要补抬指, 但不重新武装。
    {
        ScopeController sc;
        sc.tick(true, true, 1, 0, 100);
        const auto flush = sc.flushUp();
        check(flush.release_right && !flush.press_right, "途中复位: 补抬指且不多点");
        const auto flush2 = sc.flushUp();
        check(!flush2.release_right, "重复途中复位: 不再重发抬指");
    }

    // 长按态被强制打断 -> 必须松开。
    {
        ScopeController sc;
        sc.tick(true, true, 2, 0, 100);
        const auto a = sc.forceRelease();
        check(a.release_right, "长按被强制打断: 松开右键");
        check(!sc.engaged(), "强制收镜后退出开镜态");
        const auto b = sc.forceRelease();
        check(!b.press_right && !b.release_right, "重复强制收镜: 无事发生");
    }

    // 从未开过镜时收镜必须是空操作(否则会凭空点一下右键)。
    {
        ScopeController sc;
        const auto e = sc.forceRelease();
        check(!e.press_right && !e.release_right, "未开镜时强制收镜为空操作");
    }
}

// ── [6] 中途改模式: 不能把按下的右键漏在游戏里 ─────────────────────────────
void test_mode_change_mid_session()
{
    std::printf("[6] 中途改模式\n");
    // 长按态被改成关闭: 松开右键, 不留残留。
    {
        ScopeController sc;
        sc.tick(true, true, 2, 0, 100);
        const auto a = sc.tick(true, true, 0, 0, 108);
        check(!a.press_right && a.release_right, "长按态改成关闭: 松开右键");
        check(!sc.engaged(), "退出开镜态");
    }
    // 长按态改成点按: 必须先把按住的松开, 再补一次"点"的那一下。
    // ★ FSM 里的下发顺序是"先抬后按", 所以同一拍里两个都要给出来。
    {
        ScopeController sc;
        sc.tick(true, true, 2, 0, 100);
        const auto a = sc.tick(true, true, 1, 0, 108);
        check(a.release_right, "长按改成点按: 先松开按住的");
        check(a.press_right, "长按改成点按: 再点一下");
        const auto b = sc.tick(true, true, 1, 0, 116);
        check(b.release_right && !b.press_right, "下一拍: 补上点按的抬指");
    }
}

} // namespace

int main()
{
    std::printf("trigger_scope (自动开镜) 回归\n");
    test_disabled_is_inert();
    test_hold_mode();
    test_tap_mode_spans_two_ticks();
    test_rearm_only_on_new_engagement();
    test_scope_delay_gate();
    test_force_release_never_sticks();
    test_mode_change_mid_session();

    if (g_failures == 0)
        std::printf("全部通过\n");
    else
        std::printf("%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
