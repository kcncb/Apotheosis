// 轨迹整形层 (mouse/aim_path.h) 的回归, 重点是 2026-09-12 新增的 WindMouse 曲线
// (仿 AimMagic 的 enable_mouse_curve)。
//
// 这一层最容易被改坏的两件事, 所以必须卡住:
//   ① 【只旋转不缩放】: 曲线给出的只是局部切线方向, 幅值必须仍等于控制器输出。
//      一旦它开始放大/缩小幅值, 现役 PID 的增益语义(和 46ms 死区的稳定边界)就全废。
//   ② 【不能改变收敛性】: 开着曲线也要能走到目标, 不能在目标附近绕圈。
#include "mouse/aim_path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

void check_near(double got, double want, double tol, const std::string& what)
{
    if (!(std::abs(got - want) <= tol))
    {
        std::printf("  [FAIL] %s: got %.6f want %.6f +-%.6f\n",
                    what.c_str(), got, want, tol);
        ++g_failures;
    }
}

boss::AimPathDriver::Params wind_params()
{
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::WindMouse;
    p.strength = 0.25;
    p.wind_gravity = 5.0;
    p.wind_wind = 2.0;
    p.wind_step = 10.0;
    p.wind_distance = 8.0;
    p.wind_threshold_px = 10.0;
    return p;
}

// ── [1] 直线模式必须逐像素透传 (所有模式的基线) ─────────────────────────────
void test_linear_passthrough()
{
    std::printf("[1] 直线模式透传\n");
    boss::AimPathDriver d;
    boss::AimPathDriver::Params p;
    p.mode = boss::AimPathDriver::Mode::Linear;
    d.configure(p);

    const auto r = d.step(500.0, 300.0, 400.0, 300.0, 0.008, 7, 12.5, -3.25);
    check_near(r.move_x, 12.5, 1e-12, "线性: X 原样透传");
    check_near(r.move_y, -3.25, 1e-12, "线性: Y 原样透传");
}

// ── [2] curve_threshold 门控: 两轴误差都不超阈值时整段旁路 ──────────────────
void test_wind_gate_bypasses_small_errors()
{
    std::printf("[2] WindMouse 门控旁路\n");
    boss::AimPathDriver d;
    d.configure(wind_params());

    // 误差 (6, -4) 两轴都在 10px 门控之内 -> 必须与控制器输出逐位相同。
    for (int i = 0; i < 5; ++i)
    {
        const auto r = d.step(106.0, 96.0, 100.0, 100.0, 0.008, 3, 7.0, 5.0);
        check_near(r.move_x, 7.0, 1e-12, "门控内: X 旁路为控制器输出");
        check_near(r.move_y, 5.0, 1e-12, "门控内: Y 旁路为控制器输出");
    }

    // 门控填 0 时, 同样的"大误差"要走曲线。
    // 注意这里必须用远大于 D0 的误差: WindMouse 在 dist < D0 时风力本来就被
    // 衰减光了, 那种情形下路径自然退化成直线 —— 这也是 AM 需要 curve_threshold
    // 这层门控的原因 (小误差下曲线本来就没意义)。
    boss::AimPathDriver d0;
    auto p0 = wind_params();
    p0.wind_threshold_px = 0.0;
    d0.configure(p0);
    bool curved = false;
    double cur_x = 100.0;
    for (int i = 0; i < 40; ++i)
    {
        const auto r = d0.step(400.0, 100.0, cur_x, 100.0, 0.008, 3, 25.0, 0.0);
        if (std::abs(r.move_y) > 1e-9)
            curved = true;
        cur_x += r.move_x;
    }
    check(curved, "门控=0 且误差远大于 D0 时曲线真的介入");
}

// ── [3] 幅值不变式: 曲线只改方向, 不改力度 ─────────────────────────────────
void test_wind_preserves_amplitude()
{
    std::printf("[3] WindMouse 只旋转不缩放\n");
    boss::AimPathDriver d;
    d.configure(wind_params());

    const double base_mag = std::hypot(30.0, 0.0);
    double worst = 0.0;
    double max_deviation = 0.0;
    for (int i = 0; i < 60; ++i)
    {
        // 误差 200px 远大于门控, 曲线全程生效; 锚点每拍按已发位移往前推进。
        const double cur_x = 100.0 + i * 3.0;
        const auto r = d.step(300.0, 100.0, cur_x, 100.0, 0.008, 11, 30.0, 0.0);
        const double mag = std::hypot(r.move_x, r.move_y);
        worst = std::max(worst, std::abs(mag - base_mag));
        max_deviation = std::max(max_deviation, std::abs(r.move_y));
    }
    check_near(worst, 0.0, 1e-9, "幅值逐拍等于控制器输出 |base|");
    check(max_deviation > 0.0, "曲线确实产生了侧向分量(不是退化成直线)");
}

// ── [4] 确定性: 同一串输入给同一串输出 ─────────────────────────────────────
void test_wind_deterministic()
{
    std::printf("[4] WindMouse 可复现\n");
    auto run = [](std::vector<std::pair<double, double>>& out) {
        boss::AimPathDriver d;
        d.configure(wind_params());
        double cur_x = 100.0;
        for (int i = 0; i < 80; ++i)
        {
            const auto r = d.step(400.0, 100.0, cur_x, 100.0, 0.008, 5, 25.0, 0.0);
            out.emplace_back(r.move_x, r.move_y);
            cur_x += r.move_x;
        }
    };
    std::vector<std::pair<double, double>> a, b;
    run(a);
    run(b);
    check(a.size() == b.size() && a == b, "两次运行的位移序列完全一致");

    // 换一个新目标 id 必须重新摇一条路径 (否则连续几段长得一模一样)。
    std::vector<std::pair<double, double>> c;
    {
        boss::AimPathDriver d;
        d.configure(wind_params());
        double cur_x = 100.0;
        for (int i = 0; i < 80; ++i)
        {
            const auto r = d.step(400.0, 100.0, cur_x, 100.0, 0.008, 6, 25.0, 0.0);
            c.emplace_back(r.move_x, r.move_y);
            cur_x += r.move_x;
        }
    }
    check(a != c, "换目标后路径不同");
}

// ── [5] 收敛性: 开着 WindMouse 也要走到目标, 不能绕圈 ───────────────────────
void test_wind_converges()
{
    std::printf("[5] WindMouse 闭环收敛\n");
    for (const double wind : {2.0, 8.0, 20.0})
    {
        boss::AimPathDriver d;
        auto p = wind_params();
        p.wind_wind = wind;
        p.strength = 0.5;
        d.configure(p);

        // 简陋的"假游戏": 目标不动, 镜头按我们的计数转 (1 计数 = 1 像素),
        // 控制器就是一个比例项。曲线只整形, 所以必须收敛。
        double cur = 0.0;
        const double goal = 420.0;
        double last_abs_err = 1e9;
        int frames_to_settle = -1;
        for (int i = 0; i < 400; ++i)
        {
            const double err = goal - cur;
            const double base = 0.35 * err;          // 比例控制器
            const auto r = d.step(goal, 0.0, cur, 0.0, 0.008, 21, base, 0.0, 3.0);
            cur += r.move_x;
            const double abs_err = std::abs(goal - cur);
            if (abs_err <= 1.0 && frames_to_settle < 0)
                frames_to_settle = i;
            last_abs_err = abs_err;
        }
        check(frames_to_settle >= 0, "风力 " + std::to_string(wind) + ": 最终进入 ±1px");
        check(last_abs_err <= 1.0, "风力 " + std::to_string(wind) + ": 尾帧误差收敛");
    }
}

// ── [6] 近目标保护: settle_radius 内退回控制器原始方向 ─────────────────────
void test_settle_radius_returns_to_straight()
{
    std::printf("[6] 近目标回退直线\n");
    boss::AimPathDriver d;
    auto p = wind_params();
    p.wind_threshold_px = 0.0;   // 关掉门控, 让 settle_radius 独自生效
    d.configure(p);

    const auto r = d.step(110.0, 100.0, 100.0, 100.0, 0.008, 9, 8.0, 2.0, /*settle*/ 30.0);
    check_near(r.move_x, 8.0, 1e-12, "settle 内: X 回退控制器输出");
    check_near(r.move_y, 2.0, 1e-12, "settle 内: Y 回退控制器输出");
}

// ── [7] 参数改变必须让路径失效重摇 ─────────────────────────────────────────
void test_param_change_resets_path()
{
    std::printf("[7] 参数变化重摇路径\n");
    // 采样到"曲线确实介入了"的那一拍: 起段那一拍 entry_fade=0, 曲线影响恒为 0,
    // 所以必须多跑几拍再比 (这也是一条要守的性质: 起手永远不歪)。
    auto run = [](double wind) {
        boss::AimPathDriver d;
        auto p = wind_params();
        p.wind_wind = wind;
        d.configure(p);
        std::vector<double> ys;
        double cur_x = 100.0;
        for (int i = 0; i < 12; ++i)
        {
            const auto r = d.step(400.0, 100.0, cur_x, 100.0, 0.008, 4, 25.0, 0.0);
            ys.push_back(r.move_y);
            cur_x += r.move_x;
        }
        return ys;
    };
    const auto soft = run(2.0);
    const auto hard = run(30.0);

    check_near(soft.front(), 0.0, 1e-12, "起手段: 曲线影响为 0 (不歪头)");
    const bool differs = std::abs(soft.back() - hard.back()) > 1e-9
                      || std::abs(soft[6] - hard[6]) > 1e-9;
    check(differs, "改风力后路径已重摇");

    // 幅值不变式对改参数后的每一拍都成立。
    boss::AimPathDriver d;
    d.configure(wind_params());
    double cur_x = 100.0;
    for (int i = 0; i < 12; ++i)
    {
        const auto r = d.step(400.0, 100.0, cur_x, 100.0, 0.008, 4, 25.0, 0.0);
        check_near(std::hypot(r.move_x, r.move_y), 25.0, 1e-9, "幅值恒等于控制器输出");
        cur_x += r.move_x;
    }
}

}  // namespace

int main()
{
    std::printf("aim_path (轨迹整形层) 回归\n");
    test_linear_passthrough();
    test_wind_gate_bypasses_small_errors();
    test_wind_preserves_amplitude();
    test_wind_deterministic();
    test_wind_converges();
    test_settle_radius_returns_to_straight();
    test_param_change_resets_path();

    if (g_failures == 0)
        std::printf("全部通过\n");
    else
        std::printf("%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
