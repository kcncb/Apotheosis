// 自动急停 (mouse/auto_stop.h) 的回归。
//
// 这一层最容易出的事故:
//   ① 把"我们自己注入的键"当成玩家按的键 -> 自激(按 S 判定玩家按 S -> 再去按 W);
//   ② 连点模式下每一发都重发 -> 反方向键变成"一直按着", 玩家根本走不动;
//   ③ 输入方式不支持(老 MAKCU 没有键盘通道)时还在发 -> 静默失效却看不出来。
#include "mouse/auto_stop.h"

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

using boss::AutoStopController;

AutoStopController::Keys wasd(bool w, bool s, bool a, bool d)
{
    AutoStopController::Keys k;
    k.forward = w; k.back = s; k.left = a; k.right = d;
    return k;
}

// ── [1] 没按方向键 / 没开火 -> 什么都不发 ──────────────────────────────────
void test_inert()
{
    std::printf("[1] 不发的情形\n");
    AutoStopController c;

    auto a1 = c.tick(true, wasd(false, false, false, false), true, 60, 1000);
    check(!a1.tap, "没按 WASD: 不发");

    auto a2 = c.tick(false, wasd(true, false, false, false), true, 60, 1008);
    check(!a2.tap, "没开火: 不发");

    auto a3 = c.tick(true, wasd(true, false, false, false), false, 60, 1016);
    check(!a3.tap, "功能关闭(或输入方式不支持): 不发");

    auto a4 = c.tick(true, wasd(true, false, false, false), true, 0, 1024);
    check(!a4.tap, "时长填 0: 不发");
}

// ── [2] 方向映射: 前/后轴优先 ─────────────────────────────────────────────
void test_direction_mapping()
{
    std::printf("[2] 方向映射\n");
    {
        AutoStopController c;
        auto a = c.tick(true, wasd(true, false, false, false), true, 60, 1000);
        check(a.tap && a.hid_key == AutoStopController::kHidS, "按 W -> 补 S");
    }
    {
        AutoStopController c;
        auto a = c.tick(true, wasd(false, true, false, false), true, 60, 1000);
        check(a.tap && a.hid_key == AutoStopController::kHidW, "按 S -> 补 W");
    }
    {
        AutoStopController c;
        auto a = c.tick(true, wasd(false, false, true, false), true, 60, 1000);
        check(a.tap && a.hid_key == AutoStopController::kHidD, "按 A -> 补 D");
    }
    {
        AutoStopController c;
        auto a = c.tick(true, wasd(false, false, false, true), true, 60, 1000);
        check(a.tap && a.hid_key == AutoStopController::kHidA, "按 D -> 补 A");
    }
    {
        // 斜向(W+A): 只补前后轴那一个, 文档里写明了这一点。
        AutoStopController c;
        auto a = c.tick(true, wasd(true, false, true, false), true, 60, 1000);
        check(a.tap && a.hid_key == AutoStopController::kHidS,
              "斜向 W+A -> 只补 S(前后轴优先)");
    }
}

// ── [3] 按着的一发内不重发; 过期后可以再发 ────────────────────────────────
void test_no_respam_while_holding()
{
    std::printf("[3] 短按窗口内不重发\n");
    AutoStopController c;
    const auto keys = wasd(true, false, false, false);

    const auto first = c.tick(true, keys, true, 60, 1000);
    check(first.tap, "第一发开火: 发");
    check(c.active(1000, 60), "发完处于按住的窗口内");

    // 连点模式: 60ms 之内又开了一枪 -> 不重发(否则键等于一直按着)
    const auto second = c.tick(true, keys, true, 60, 1030);
    check(!second.tap, "窗口内再开火: 不重发");

    // 窗口过去之后可以再发
    const auto third = c.tick(true, keys, true, 60, 1070);
    check(third.tap, "窗口过后再开火: 可以再补一次");
    check(c.active(1070, 60), "刚发完: 处于按住窗口内");
    check(c.active(1129, 60), "窗口内(1070+59): 仍算按住");
    check(!c.active(1130, 60), "到点(1070+60): 固件已弹起");
}

// ── [4] ★ 注入的键不能被当成玩家按的键(自激防护) ─────────────────────────
void test_injected_key_not_self_exciting()
{
    std::printf("[4] 自激防护\n");
    AutoStopController c;

    // 第一次: 玩家按 W -> 我们补 S
    auto a1 = c.tick(true, wasd(true, false, false, false), true, 60, 1000);
    check(a1.tap && a1.hid_key == AutoStopController::kHidS, "补 S");

    // 之后 GetAsyncKeyState 会同时读到 W 和我们注入的 S。
    // 若不做剔除, 逻辑会看到 S -> 去补 W -> 来回打, 玩家的移动就废了。
    const auto raw_with_injection = wasd(true, true, false, false);
    const auto a2 = c.tick(true, raw_with_injection, true, 60, 1070);
    check(a2.tap && a2.hid_key == AutoStopController::kHidS,
          "读到我们注入的 S 时仍然只补 S(不会反过来补 W)");

    // 玩家松开 W、只剩我们注入的 S: 不应再补任何东西
    AutoStopController c2;
    c2.tick(true, wasd(true, false, false, false), true, 60, 1000);   // 注入 S
    const auto a3 = c2.tick(true, wasd(false, true, false, false), true, 60, 1070);
    check(!a3.tap, "只剩我们注入的 S(玩家已松开 W): 不再补");
}

// ── [4b] ★★ 剔除不能"永久生效": 换个方向之后仍要能急停 ───────────────────
// 上一版的 bug: last_hid_key_ 一旦设上就不再清零, 于是补过一次 S 之后
// keys.back 被永久剔掉 —— 玩家按 S 后退时再也急停不了, A/D 同理。
// 急停只在"第一次用的那个方向"上有效。这个测试用【同一个控制器】连做四个方向。
void test_exclusion_is_not_permanent()
{
    std::printf("[4b] 四个方向轮着来(同一个控制器)\n");
    AutoStopController c;
    int64_t t = 1000;

    struct Case { bool w, s, a, d; int hid; const char* name; };
    const Case cases[] = {
        {true,  false, false, false, AutoStopController::kHidS, "W->S"},
        {false, true,  false, false, AutoStopController::kHidW, "S->W"},
        {false, false, true,  false, AutoStopController::kHidD, "A->D"},
        {false, false, false, true,  AutoStopController::kHidA, "D->A"},
        {true,  false, false, false, AutoStopController::kHidS, "再回 W->S"},
        {false, false, false, true,  AutoStopController::kHidA, "再回 D->A"},
    };

    for (const auto& cs : cases)
    {
        t += 300;   // 每一发之间隔开, 模拟真实的开火间隔
        const auto a = c.tick(true, wasd(cs.w, cs.s, cs.a, cs.d), true, 60, t);
        const std::string what = std::string("同一个控制器里 ") + cs.name;
        check(a.tap, what + ": 必须补键");
        check(a.hid_key == cs.hid, what + ": 补对了键");
    }
}

// ── [5] reset 之后可以重新判定, 且不发送任何东西 ─────────────────────────
void test_reset()
{
    std::printf("[5] reset\n");
    AutoStopController c;
    c.tick(true, wasd(true, false, false, false), true, 60, 1000);
    c.reset();
    check(!c.active(1000, 60), "reset 后不再处于按住窗口(键本身由固件弹起)");
    const auto a = c.tick(true, wasd(true, false, false, false), true, 60, 1005);
    check(a.tap, "reset 后可以重新补");
}

} // namespace

int main()
{
    std::printf("auto_stop (自动急停) 回归\n");
    test_inert();
    test_direction_mapping();
    test_no_respam_while_holding();
    test_injected_key_not_self_exciting();
    test_exclusion_is_not_permanent();
    test_reset();

    if (g_failures == 0)
        std::printf("全部通过\n");
    else
        std::printf("%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
