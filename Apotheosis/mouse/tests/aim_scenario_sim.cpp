// =============================================================================
// 多场景「模拟真实环境」瞄准测试台 (aim_scenario_sim)
// =============================================================================
//
// 为什么要它: 单元级的阶跃/方波测试无法反映真实链路 —— 检测延迟、检测噪声与
// 量化、指令生效延迟、遮挡后的外推与重锁, 这些才是玩家实际感受到的"焊不住"的来源。
// 本程序把上述要素全部建模, 再用真实控制器(ava_exact/pidf_mode1_exact)跑闭环,
// 对敌人各种身法逐场景打分, 并给出一个可比较的综合分(p95 偏差的平均, 越低越好)。
//
// 用法(参数全部可选, 顺序如下):
//   aim_scenario_sim [kp] [kd] [kf] [lr] [限幅] [检测延迟帧] [鼠标延迟帧]
// 例:
//   aim_scenario_sim                       // 界面默认值
//   aim_scenario_sim 1.6 0.05 1 0.05 0 2 1
//
// 单位: 全部在 detection_resolution=320 的图像像素/秒。参考(90°FOV, 320px):
//   1° ≈ 3.56px; 6m 处 5m/s 横移 ≈ 168px/s; 滑铲 8-10m/s ≈ 300-350px/s;
//   翻滚/dash ≈ 500-750px/s; 喷气横移可到 900px/s 以上。
//
// 调参提示(实测):
//   · 「锁定强度」kf 必须非零, 否则前馈整条关死(ff_output = ff_state*dt*kf = 0),
//     此时「预测速度」lr 调多少都没反应; 正确值是 kf=1, >1 会过度提前、全面变差。
//   · 「过冲控制」kd 在延迟下非常危险 —— kd≥0.1 会让回路发散, 保持 0.05 量级。
//   · 「预测速度」lr 到 0.05 就基本饱和, 0.10 无进一步收益; 低于 0.02 明显差。
//   · 「瞄准速度」kp 与链路延迟补偿配套: 补偿开启后 kp=2.0 才安全且最优。
//
// 第 8 个可选参数是"补偿假定延迟(帧)", 用来测延迟估计不准时的鲁棒性:
// 实测(行=真实检测延迟, 列=补偿实际使用的帧数), 过高估计会发散, 低估安全 ——
//   (真实1帧) 15.6 / 15.4 / 15.3 / 3425(炸)
//   (真实3帧) 26.3 / 24.4 / 22.4 /  22.1 /  650(炸)
// =============================================================================
// =============================================================================
// 多场景「模拟真实环境」测试台
// =============================================================================
// 目的: 用一个贴近真实的闭环模型, 衡量控制器在敌人各种身法下能不能把准星
//       焊在锚点上, 并给调参提供可比较的分数。
//
// 真实环境的要素(全部建模, 不是纯数学阶跃):
//   · 检测帧率与链路延迟: 控制环每个"新鲜检测帧"跑一次(与 mouse_thread_loop
//     的实际行为一致: 没有新帧就 continue 不 tick), 测量值来自 det_lat 帧之前。
//   · 检测噪声与量化: 中心点 ±0.35px 抖动 + 0.5px 量化。
//   · 鼠标生效延迟: 指令下发后 mouse_lat 帧才反映到视角。
//   · 视角模型: 准星在屏幕上不动, 是视角在转; 所以 PIDF 的 current_x 恒为
//     分辨率中心, 误差 = 锚点屏幕位置 - 中心。误差递推:
//         err += (敌人速度 + 自身位移速度) * dt - 生效的位移
//   · 自身动作(大跳/跳拉/摆头)表现为【视角先动】: 不经过控制器指令, 直接改误差。
//   · 遮挡: 若干帧没有测量, 控制器只能靠自身状态外推。
//
// 单位: 全部在 detection_resolution=320 的图像像素/秒。参考(90°FOV, 320px):
//   1° ≈ 3.56px; 6m 处 5m/s 横移 ≈ 168px/s; 滑铲 8-10m/s ≈ 300-350px/s;
//   翻滚/dash ≈ 500-750px/s; 喷气横移可到 900px/s 以上。
// =============================================================================
#include "../ava_exact/pidf_mode1_exact.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <vector>

using namespace cvm::recovered;

// ── 参数(界面上的六个值, 两轴同值) ──────────────────────────────────────────
struct Params {
    double kp = 2.0;     // 瞄准速度
    double kd = 0.05;    // 过冲控制
    double kf = 1.0;     // 锁定强度
    double lr = 0.05;    // 预测速度
    int    limit = 0;    // 移动限幅(0=关)
    double box_w = 20.0, box_h = 48.0;   // 目标框(320 分辨率下中距人物)
};

// ── 环境 ────────────────────────────────────────────────────────────────────
struct Env {
    double dt = 1.0 / 120.0;   // 检测周期(控制环同频)
    int det_lat = 2;           // 检测+发布延迟(帧)
    int mouse_lat = 1;         // 指令生效延迟(帧)
    int occl_every = 0;        // 每 N 帧丢一帧(0=不丢)
    int cache = 3;             // lost_target_cache_frames: 丢帧后靠外推维持的帧数,
                               // 超过才真正丢目标(此时 PIDF 复位 + 准星不动)
};

// ── 场景: 逐帧给出敌人的视在速度 / 自身视在速度 / 是否可见 ─────────────────
struct Scenario {
    std::string name;
    std::string axis;                        // "x" 或 "y"
    std::function<void(double t, double* enemy_v, double* self_v, bool* visible)> f;
    double duration = 2.0;
};
struct Metrics {
    double mean = 0, p95 = 0, mx = 0;
    double locked_ratio = 0;   // |err| <= 3px 的帧占比
    double jitter = 0;         // 每帧位移变化量均值(静止时应为 0)
    double overshoot = 0;      // 换向/急停后的最大反向超出
    int relocks = 0;           // 丢目标后重新锁定的次数
};

// ── 单场景闭环 ──────────────────────────────────────────────────────────────
Metrics runScenario(const Scenario& sc, const Params& p, const Env& e, int argc, char** argv)
{
    PidfMode1Config c{};
    c.kp_x = p.kp; c.kp_y = p.kp;
    c.kd_x = p.kd; c.kd_y = p.kd;
    c.kf_x = p.kf; c.kf_y = p.kf;
    c.lr_x = p.lr; c.lr_y = p.lr;
    c.movement_limit_x = p.limit; c.movement_limit_y = p.limit;
    PidfMode1State s = construct_pidf_mode1(c, 0.0);
    PidfDelayModelExact delay{};
    // 把环境的真实延迟告诉控制器(现实里由 latency_probe 实测给出):
    // 测量侧 = 检测+发布延迟; 指令侧 = 鼠标+游戏帧延迟。
    const int assumed_lat = (argc > 8) ? std::atoi(argv[8]) : e.det_lat;
    delay.measure_latency_sec = static_cast<double>(assumed_lat) * e.dt;
    delay.command_latency_frames = static_cast<double>(e.mouse_lat);

    const bool is_x = (sc.axis == "x");
    const double rx = is_x ? p.box_w : p.box_h;
    const double ry = is_x ? p.box_h : p.box_w;

    std::deque<double> true_err;     // 真实误差历史(用于延迟)
    std::deque<double> pending;      // 待生效的位移
    double err = 0.0;
    std::vector<double> samples;
    double prev_move = 0.0, jitter_sum = 0.0;
    double last_meas = 0.0, last_ev = 0.0, predicted = 0.0;
    int n = 0, locked = 0, coast = 0, relocks = 0;
    bool lost = false;

    const int frames = static_cast<int>(sc.duration / e.dt);
    for (int f = 0; f < frames; ++f)
    {
        const double t = f * e.dt;
        double enemy_v = 0.0, self_v = 0.0; bool visible = true;
        sc.f(t, &enemy_v, &self_v, &visible);
        if (e.occl_every > 0 && (f % e.occl_every) == 0) visible = false;

        true_err.push_back(err);
        while (static_cast<int>(true_err.size()) > e.det_lat + 1) true_err.pop_front();
        const double measured_true = true_err.front();

        // 检测噪声 + 量化
        static const double noise[] = {0,1,-1,0,1,0,-1,1,0,-1};
        const double nz = 0.35 * noise[f % 10];
        const double measured = std::round((measured_true + nz) * 2.0) * 0.5;

        // 指令生效延迟: 队列长度 = mouse_lat+1, 最老的一条本帧生效
        double applied = 0.0;
        if (static_cast<int>(pending.size()) > e.mouse_lat)
        {
            applied = pending.front();
            pending.pop_front();
        }

        // 真实链路: 丢帧后先是 tracker/engine 用速度外推(cache 帧), 仍然没检测到
        // 才算丢目标 —— 那时 PIDF 被复位且准星不动(不 tick), 不是每帧喂 valid=0。
        if (visible)
        {
            coast = 0;
            last_meas = measured;
            last_ev = enemy_v;
            predicted = measured;
        }
        else
        {
            ++coast;
            predicted = last_meas + last_ev * static_cast<double>(coast) * e.dt;
        }
        if (coast > e.cache)
        {
            lost = true;
            err += (enemy_v + self_v) * e.dt;      // 目标继续跑, 准星不动
            true_err.back() = err;
            continue;
        }
        if (lost)
        {
            // 重新锁定: 原生会在新锁定时复位 PIDF
            s = construct_pidf_mode1(c, 0.0);
            reset_pidf_delay_model(delay);
            pending.clear();
            lost = false;
            relocks++;
        }
        PidfInputExact in{};
        in.valid = 1;
        in.target_x = 160.0 + (is_x ? predicted : 0.0);
        in.current_x = 160.0;
        in.target_y = 160.0 + (is_x ? 0.0 : predicted);
        in.current_y = 160.0;
        in.radius_x = rx; in.radius_y = ry;
        const auto out = update_pidf_mode1(s, delay, in, (f + 1) * e.dt);
        const double dx = is_x ? static_cast<double>(out.dx) : static_cast<double>(out.dy);
        pending.push_back(dx);

        // 环境推进: 敌人 + 自身动作改变误差; 已生效的指令把它拉回来
        err += (enemy_v + self_v) * e.dt - applied;

        if (f >= 30)   // 跳过起步
        {
            samples.push_back(std::fabs(err));
            if (std::fabs(err) <= 3.0) ++locked;
            ++n;
            jitter_sum += std::fabs(dx - prev_move);
        }
        prev_move = dx;
    }

    Metrics m;
    if (samples.empty()) return m;
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : samples) sum += v;
    m.mean = sum / samples.size();
    m.p95 = sorted[static_cast<std::size_t>(sorted.size() * 0.95)];
    m.mx = sorted.back();
    m.locked_ratio = static_cast<double>(locked) / n;
    m.jitter = jitter_sum / std::max(1, n);
    m.relocks = relocks;
    return m;
}

// ── 场景库 ──────────────────────────────────────────────────────────────────
std::vector<Scenario> scenarios()
{
    std::vector<Scenario> v;
    auto add = [&](const char* name, const char* axis, double dur,
                   std::function<void(double, double*, double*, bool*)> f) {
        v.push_back({name, axis, std::move(f), dur});
    };

    add("静止", "x", 2.0, [](double, double* ev, double* sv, bool*) { *ev = 0; *sv = 0; });
    add("匀速横移", "x", 2.0, [](double, double* ev, double* sv, bool*) { *ev = 250; *sv = 0; });

    // 滑铲: 瞬间提到 550 再线性衰减到 180(起身), 并伴随蹲下(框变矮)
    add("滑铲", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 0.6) *ev = 550.0;
        else if (t < 1.2) *ev = 550.0 + (180.0 - 550.0) * ((t - 0.6) / 0.6);
        else *ev = 180.0; });

    // 翻滚: 0.22s 内 750px/s 冲出去, 中间 0.18s 被遮挡
    add("翻滚", "x", 2.0, [](double t, double* ev, double* sv, bool* vis) {
        *sv = 0;
        if (t >= 0.8 && t < 1.02) *ev = 750.0;
        else *ev = 0.0;
        if (t >= 0.95 && t < 1.13) *vis = false; });

    // 跳跃: 竖直抛物线 ±480, 带 150 水平漂移
    add("跳跃(竖直)", "y", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t >= 0.6 && t < 0.95) *ev = 480.0;
        else if (t >= 0.95 && t < 1.35) *ev = -480.0;
        else *ev = 0.0; });

    // 连跳: 1.2Hz, 空中竖直 ±450, 每次落地水平反向 ±180
    add("连跳", "y", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        const double ph = std::fmod(t, 0.83);
        *ev = (ph < 0.42) ? 450.0 : -450.0; });

    // 连跳(水平): 每次落地水平反向 ±180, 空中不变
    add("连跳(水平)", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        const double ph = std::fmod(t, 0.83);
        *ev = (ph < 0.42) ? 180.0 : -180.0; });

    // 急停: 420px/s 跑 1s 后瞬间静止
    add("急停", "x", 2.5, [](double t, double* ev, double* sv, bool*) {
        *sv = 0; *ev = (t < 1.0) ? 420.0 : 0.0; });

    // 喷气: 0.14s 内 0->950, 保持 0.45s, 再瞬间归零
    add("喷气", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 0.14) *ev = 950.0 * (t / 0.14);
        else if (t < 0.59) *ev = 950.0;
        else *ev = 0.0; });

    // 摆头: ±380 方波, 1.5Hz
    add("摆头换向", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0; *ev = (std::fmod(t, 0.667) < 0.333) ? 380.0 : -380.0; });

    // 自身大跳/跳拉: 敌人基本不动, 是【视角】被玩家自己甩动
    add("自身跳拉(瞬移)", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *ev = 60.0;
        *sv = (std::fabs(t - 0.8) < 0.004) ? 140.0 / 0.0083 : 0.0; });
    add("自身大跳(加速)", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *ev = 0.0;
        *sv = (t >= 0.8 && t < 1.15) ? 500.0 : 0.0; });

    // 遮挡: 300px/s 横移中每 12 帧丢一帧
    add("检测抖动/丢帧", "x", 2.0, [](double, double* ev, double* sv, bool*) { *ev = 300; *sv = 0; });

    // 复合身法: 横移 + 换向 + 竖直 + 急停
    add("复合身法", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 0.7) *ev = 300.0;
        else if (t < 1.0) *ev = -300.0;
        else if (t < 1.4) *ev = 600.0;
        else *ev = 0.0; });
    return v;
}

int main(int argc, char** argv)
{
    Params p;
    Env e;
    if (argc > 1) p.kp = std::atof(argv[1]);
    if (argc > 2) p.kd = std::atof(argv[2]);
    if (argc > 3) p.kf = std::atof(argv[3]);
    if (argc > 4) p.lr = std::atof(argv[4]);
    if (argc > 5) p.limit = std::atoi(argv[5]);
    if (argc > 6) e.det_lat = std::atoi(argv[6]);
    if (argc > 7) e.mouse_lat = std::atoi(argv[7]);
    std::printf("参数: kp=%.3f kd=%.3f kf=%.3f lr=%.3f 限幅=%d | 环境: %.0fHz 检测延迟%d帧 鼠标延迟%d帧 | 补偿假定延迟=%d帧\n",
                p.kp, p.kd, p.kf, p.lr, p.limit, 1.0 / e.dt, e.det_lat, e.mouse_lat,
                argc > 8 ? std::atoi(argv[8]) : e.det_lat);
    std::printf("%-16s %8s %8s %8s %8s %8s %5s\n", "场景", "均偏差", "p95", "最大", "锁定率", "抖动", "重锁");
    double score = 0.0; int n = 0;
    for (const auto& sc : scenarios())
    {
        if (sc.name == "检测抖动/丢帧") e.occl_every = 12; else e.occl_every = 0;
        const Metrics m = runScenario(sc, p, e, argc, argv);
        std::printf("%-16s %7.2f %8.2f %8.2f %7.1f%% %8.2f %5d\n",
                    sc.name.c_str(), m.mean, m.p95, m.mx, m.locked_ratio * 100.0, m.jitter, m.relocks);
        score += m.p95; ++n;
    }
    std::printf("%-16s %8s %8s %8s %8s %8s\n", "综合", "", "", "", "", "");
    std::printf("综合分数(p95 平均, 越低越好) = %.3f\n", score / n);
    return 0;
}
