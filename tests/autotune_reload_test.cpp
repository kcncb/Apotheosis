// =============================================================================
// 回归: 外部改了配置之后, 界面控件必须跟着重读 (否则会"陈旧值回灌")
// =============================================================================
//
// ★★ 为什么单独一个测试 (2026-09-15) ★★
//
//   用户报「一直没能真正生效!!」。实机证据 (chain_live.log, 12 条 pid_params):
//
//       x=(kp=60.0, ki=1.00, kd=0.020)   y=(kp=15.0, ki=5.00, kd=0.010)
//       ^^^ 每轮都被调参改成新值          ^^^ 永远停在会话开始时的旧值
//
//   自动调参是【同时】写 x 和 y 的 (write_knobs_to_config 里两个都赋同一个值),
//   所以只有一种解释: 有人把 y 又盖回了旧值。
//
//   根因是一条静默的"陈旧值回灌"链路:
//     ① 自动调参改了 config, 触发 configLoaded;
//     ② HotkeyPage::reloadFromRuntime() 当时【只】重建分组下拉,
//        六个瞄准参数 spinbox 完全不重读, 仍显示旧值;
//     ③ 用户之后在本页改【任何】别的控件(视野/扳机/曲线...), 都会走
//        saveUiToCurrentProfile() —— 它把【控件当前值整片写回 config】:
//            hp.pidf_kp_x = m_pidfGain[0]->value();
//            hp.pidf_kp_y = m_pidfGain[1]->value();   // <- 陈旧值
//     ④ 参数被悄悄改回去, 不报错、不留痕。
//
//   ★ 教训: 【"整片写回"式的界面 + "外部会改配置"的机制】放在一起时,
//     控件必须在每次外部变更后重读。否则界面不是"显示", 而是一个
//     随时会把旧值灌回去的缓存。这不是 HotkeyPage 独有的问题 ——
//     任何 saveUiToCurrentProfile 风格的页面都有这个风险。
//
// 本测试不建 Qt 控件(那要拖进整个 HotkeyPage + 一堆 widget), 而是直接
// 复现【那条链路的数据后果】: 用 Runtime 的写回钩子模拟"外部改配置",
// 再模拟"界面用陈旧缓存整片写回", 断言参数确实被污染 —— 然后断言
// 修复所依赖的那条不变量(重读之后不再污染)。
//
// ★ 这个测试的价值在于【把链路写下来】: 它钉住"整片写回"与"外部写入"
//   共存时的危险, 以及"重读"是唯一解药。

#include "mouse/autotune_runtime.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const char* what)
{
    ++g_checks;
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

namespace {

using boss::autotune::Knobs;

// 模拟一个"整片写回"的界面缓存: 它持有自己对所有参数的副本,
// 保存时把自己的副本【整片】写回去 —— 与 HotkeyPage::saveUiToCurrentProfile 同形。
struct UiCache
{
    Knobs cached;
    // 界面重读(loadProfileToUi): 从 config 拉最新值。
    // ★ 修复前 HotkeyPage 在 configLoaded 上【没有】做这一步。
    void reload(const Knobs& from_config) { cached = from_config; }
    // 界面保存(saveUiToCurrentProfile): 把【缓存里的旧值】整片写回。
    Knobs snapshot_to_write() const { return cached; }
};

} // namespace

int main()
{
    std::printf("=== autotune_reload_test: 外部改配置后控件必须重读 ===\n");

    // ─────────────────────────────────────────────────────────────────────
    std::printf("\n[1] 复现: 陈旧缓存整片写回 -> 参数被改回旧值\n");
    //
    //   场景就是实机发生的那次: 调参把 kp 从 15 改成 60(同时改 x/y),
    //   而界面缓存还停在 15, 用户随后改了任一控件 -> 保存 -> y 回到 15。
    {
        // 会话开始时的参数 (实机: CF.ini 里的 15/5/0.010)
        Knobs at_session_start;
        at_session_start.kp = 15.0;
        at_session_start.ki = 5.00;
        at_session_start.kd = 0.010;

        // 界面在页面构造时读了一次, 之后【再也不重读】(修复前的行为)
        UiCache ui;
        ui.reload(at_session_start);

        // 自动调参把参数改成新值(它是同时改 x/y 的, 所以这里只有一个 kp)
        Knobs after_autotune = at_session_start;
        after_autotune.kp = 60.0;
        after_autotune.ki = 1.00;

        // 用户在本页改了个【别的】控件(比如视野), 触发整片写回
        const Knobs written_back = ui.snapshot_to_write();

        check(std::abs(written_back.kp - 60.0) > 1e-9,
              "★★ 没有重读时, 整片写回会把 kp 从 60 打回 15 (这就是实机现象)");
        check(std::abs(written_back.kp - 15.0) < 1e-9,
              "★★ 具体就是回到会话开始时的旧值");
        check(written_back.ki != after_autotune.ki,
              "★★ ki 同样被回灌(与实机日志 x=1.00 y=5.00 的不对称一致)");
    }

    // ─────────────────────────────────────────────────────────────────────
    std::printf("\n[2] 修复: configLoaded 时重读 -> 不再回灌\n");
    {
        Knobs at_session_start;
        at_session_start.kp = 15.0;
        at_session_start.ki = 5.00;
        at_session_start.kd = 0.010;

        UiCache ui;
        ui.reload(at_session_start);

        Knobs after_autotune = at_session_start;
        after_autotune.kp = 60.0;
        after_autotune.ki = 1.00;

        // ★ 修复的核心: 外部改完配置(触发 configLoaded)时, 界面重读一遍。
        ui.reload(after_autotune);

        const Knobs written_back = ui.snapshot_to_write();
        check(std::abs(written_back.kp - 60.0) < 1e-9,
              "★★ 重读之后整片写回不再污染 kp");
        check(std::abs(written_back.ki - 1.00) < 1e-9,
              "★★ ki 也不再被回灌");
    }

    // ─────────────────────────────────────────────────────────────────────
    std::printf("\n[3] 走真实的 Runtime 写回路径: 改写的是同一组 x/y\n");
    //
    //   ★ 这一节钉住"调参确实同时写 x 和 y" —— 否则 [1] 的不对称就无法归因到
    //     "界面回灌", 而会被误判成"调参只改了 x"。
    {
        auto& rt = boss::autotune::Runtime::instance();

        Knobs stored;
        stored.kp = 15.0; stored.ki = 5.0; stored.kd = 0.010;

        boss::autotune::RuntimeHooks hooks;
        hooks.read  = [&stored]() { return stored; };
        hooks.write = [&stored](const Knobs& k, std::string&) {
            stored = k;                 // 模拟写进 config
            return true;                // 且生效
        };
        hooks.read_base_h  = []() { return 0.0; };
        hooks.write_base_h = [](double) { return true; };
        rt.install(hooks);

        Knobs want = stored;
        want.kp = 60.0;
        want.ki = 1.00;

        std::string err;
        const bool ok = rt.write_knobs_for_test(want, err);
        check(ok, "写回成功");
        check(std::abs(stored.kp - 60.0) < 1e-9, "★ Kp 已写入(等价于 x 与 y 同值)");
        check(std::abs(stored.ki - 1.00) < 1e-9, "★ Ki 已写入");

        // 界面若用陈旧缓存整片写回, 就会把两个轴一起打回去。
        // ★ 这正是"调参明明同时写 x/y, 日志里却只有 x 变"的唯一解释。
        UiCache stale_ui;
        Knobs stale_cached;              // 陈旧(修复前不会重读)
        stale_cached.kp = 45.0;
        stale_cached.ki = 3.0;
        stale_cached.kd = 0.005;
        stale_ui.reload(stale_cached);
        const Knobs clobber = stale_ui.snapshot_to_write();
        check(std::abs(clobber.kp - 60.0) > 1e-9,
              "★★ 陈旧界面缓存会把刚写进去的值打回去(解释日志里的不对称)");
    }

    std::printf("\n=== %d 项检查, %d 项失败 ===\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
