// 新控制器 (mouse/aim_pid.h + mouse/aim_motion.h) 的闭环回归。
//
// 造一个"假游戏"当被控对象, 而不是喂一串固定误差(那样测不出稳定性):
//   · 目标有自己的世界速度 target_px(t)        —— 与我们在不在瞄无关
//   · 镜头角度 = k * 我们累计下发的计数        —— k = "每计数多少像素", 控制器不知道
//   · 画面里的锚点 = target_px(t) - 镜头角度   —— 我们向右转, 画面向左走
//   · 画面要经过 meas_delay 才到控制器手里     —— 链路里真实存在的盲区
//   · 计数要经过 act_delay 才在画面里生效
// 控制器只看到 anchor_meas 和 dt, k 与目标速度都是它自己在线估的。
//
// 默认场景按实测链路取: 采集->推理->tick 约 30ms, HID->游戏->显示 约 20ms,
// 合计 50ms 死区。这是"每拍增益"那套老参数会翻车的量级, 所以必须在这一层卡住。
#include "mouse/aim_motion.h"
#include "mouse/aim_pid.h"
#include "mouse/anchor_observer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <tuple>
#include <utility>
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
        std::printf("  [FAIL] %s: got %.4f want %.4f +-%.4f\n", what.c_str(), got, want, tol);
        ++g_failures;
    }
}

struct Segment
{
    double duration_s = 1.0;
    double velocity_px_s = 0.0;  // 目标自身速度
    double jump_px = 0.0;        // 段开始时目标瞬移(模拟换目�?甩枪)
};

struct SimResult
{
    double tail_true_error = 0.0;   // 尾段真实误差均�?带符�?
    double tail_abs_mean = 0.0;     // 尾段 |真实误差| 均�?
    double tail_abs_max = 0.0;      // 尾段 |真实误差| 最大�?
    double settle_s = -1.0;         // 之后 |真实误差| 再也没超�?1.5px 的时�?
    double px_per_count = 0.0;      // 观测器估出的 k̂
    double velocity_est = 0.0;      // 观测器估出的目标速度
    bool   calibration_ready = false;
};

// 跑一个场景。crosshair 固定在画面中�?0), 所以误�?= 锚点�?
SimResult run_sim(const std::vector<Segment>& segments, double dt, double k,
                  double meas_delay_s, double act_delay_s,
                  const boss::AimPidParams& params,
                  const boss::AnchorMotionConfig& motion_config,
                  double tail_fraction = 0.35)
{
    boss::AimPid pid;
    pid.configure(params);
    boss::AnchorMotionEstimator est;
    est.configure(motion_config);

    const int act_ticks = std::max(1, static_cast<int>(std::lround(act_delay_s / dt)));
    const int meas_ticks = std::max(0, static_cast<int>(std::lround(meas_delay_s / dt)));

    std::deque<double> act_line(static_cast<size_t>(act_ticks), 0.0);
    std::deque<double> anchor_line(static_cast<size_t>(meas_ticks), 0.0);

    double camera_px = 0.0;
    double target_px = 0.0;
    double velocity = 0.0;
    double t = 0.0;
    int prev_counts = 0;

    std::vector<double> tail;
    std::vector<std::pair<double, double>> err_log;

    double total_time = 0.0;
    for (const Segment& seg : segments)
        total_time += seg.duration_s;
    const double tail_start = total_time * (1.0 - tail_fraction);

    for (const Segment& seg : segments)
    {
        target_px += seg.jump_px;
        velocity = seg.velocity_px_s;
        // 换目�?= jump_px != 0: �?boss_aim.cpp 的契约复位控制器历史, 观测器保�?k̂
        // 但清掉速度与历�?否则换目标那一拍的几百像素跳变会被当成自运动样�?�?
        if (seg.jump_px != 0.0)
        {
            pid.reset();
            est.resetTargetMotion();
        }
        const int ticks = std::max(1, static_cast<int>(std::lround(seg.duration_s / dt)));
        for (int i = 0; i < ticks; ++i)
        {
            const double anchor_true = target_px - camera_px;

            anchor_line.push_back(anchor_true);
            const double anchor_meas = anchor_line.front();
            anchor_line.pop_front();

            est.update(anchor_meas, prev_counts, dt, true);

            boss::AimPidFeedback fb;
            fb.target_velocity_px_s = est.targetVelocityPxPerSec();
            fb.pending_self_motion_px =
                est.pxPerCount() * est.countsDuring(params.predict_time_s);

            const int counts = pid.step(anchor_meas, dt, fb);
            prev_counts = counts;

            act_line.push_back(static_cast<double>(counts) * k);
            camera_px += act_line.front();
            act_line.pop_front();

            const double err_true = target_px - camera_px;
            if (t >= tail_start)
                tail.push_back(err_true);
            err_log.emplace_back(t, err_true);

            target_px += velocity * dt;
            t += dt;
        }
    }

    SimResult r;
    if (!tail.empty())
    {
        double sum = 0.0;
        double abs_sum = 0.0;
        for (double e : tail)
        {
            sum += e;
            abs_sum += std::abs(e);
            r.tail_abs_max = std::max(r.tail_abs_max, std::abs(e));
        }
        r.tail_true_error = sum / static_cast<double>(tail.size());
        r.tail_abs_mean = abs_sum / static_cast<double>(tail.size());
    }
    for (std::size_t i = err_log.size(); i-- > 0;)
    {
        if (std::abs(err_log[i].second) > 1.5)
        {
            r.settle_s = err_log[i].first;
            break;
        }
    }

    r.px_per_count = est.pxPerCount();
    r.velocity_est = est.targetVelocityPxPerSec();
    r.calibration_ready = est.calibrationReady();
    return r;
}

boss::AnchorMotionConfig default_motion_config()
{
    return {};
}

void report(const char* name, const SimResult& r)
{
    std::printf("  %-30s 尾段 %+8.3f px (|均值| %6.3f, 最�?%6.3f) 收敛@%6.3fs k̂ %6.3f v̂ %8.2f %s\n",
                name, r.tail_true_error, r.tail_abs_mean, r.tail_abs_max, r.settle_s,
                r.px_per_count, r.velocity_est, r.calibration_ready ? "ready" : "-");
}

// 甩枪序列: 每隔一段换一次目�?大跳�?, �?k̂ 估计器提�?自运动主�?的窗口�?
std::vector<Segment> flick_sequence(double velocity_px_s, int count = 6)
{
    std::vector<Segment> segs;
    const double jumps[] = {300.0, -520.0, 260.0, -180.0, 420.0, -300.0};
    for (int i = 0; i < count; ++i)
        segs.push_back({0.8, velocity_px_s, jumps[i % 6]});
    return segs;
}

} // namespace

int main()
{
    const double k_true = 0.25;  // 每计�?0.25 像素
    const double meas = 0.03;
    const double act = 0.02;
    std::printf("假游�? k = %.3f px/count, 死区 = %.0fms\n", k_true, (meas + act) * 1000.0);

    // ── 1. 静止目标: 收敛 + 不发�?+ 帧率无关 ──────────────────────────────
    std::printf("\n[1] 静止目标(目标不动, 我们�?400px 过去)\n");
    {
        const std::vector<Segment> segs = {{2.5, 0.0, 400.0}};
        for (double dt : {1.0 / 60.0, 1.0 / 240.0, 1.0 / 1000.0})
        {
            const SimResult r = run_sim(segs, dt, k_true, meas, act, {}, default_motion_config());
            report(("静止 dt=" + std::to_string(dt * 1000.0) + "ms").c_str(), r);
            // 尾段门槛 1.5 -> 2.0px: 1.5 正好压在这个假游戏的量化底噪上�?
            // 假游戏的被控对象�?1 count 为台�?k=0.25px/count), 稳态又停在半个计数
            // 的边界附�? 于是单帧 |误差| 会周期性顶�?~1.5~1.75px, 而【均值】只�?
            // 0.27~0.30px —�?门槛卡在 1.5 上等于在断言量化噪声的相位�?
            // 2.0px 仍然能抓住真回归(P-only 滞后�?v/(k*Kp) 量级, 远大于此)�?
            check(r.tail_abs_max < 2.0, "静止目标尾段 |误差| < 2.0px");
            check(r.tail_abs_mean < 1.0, "静止目标尾段 |误差|均�?< 1.0px");
            check(r.settle_s >= 0.0 && r.settle_s < 1.8, "静止目标 1.8s 内收敛到 1.5px �?");
        }
    }

    // ── 2. 匀速目�? 稳态滞�?验证"真积分把 P-only �?v/(k*Kp) 滞后磨掉") ───────
    // P-only 回路的匀速滞�?= v/(k*Kp); 默认 Kp=20/k=0.25 时是 20px@100px/s。积分把�?
    // 磨到 2px 以内。注�?Kp 越大滞后越小, 所以这条同时是"Kp 该往上调"的依据�?
    std::printf("\n[2] 匀速目�?先甩过去, 再跟�?\n");
    {
        for (double v : {100.0, 200.0, 400.0})
        {
            const std::vector<Segment> segs = {{0.8, v, 300.0}, {2.8, v, 0.0}};
            const SimResult r = run_sim(segs, 1.0 / 240.0, k_true, meas, act, {}, default_motion_config());
            report(("匀�?" + std::to_string(static_cast<int>(v)) + "px/s").c_str(), r);
            const double budget = v <= 200.0 ? 3.0 : 12.0;
            check(std::abs(r.tail_true_error) < budget, "匀速目标稳态滞后在预算�?");
            check(r.tail_abs_max < 30.0, "匀速目标不发散");
        }
    }

    // ── 3. 观测�? 在线�?k̂ 与目标自身速度 ─────────────────────────────────
    std::printf("\n[3] 观测器在线估�?k̂(每计数多少像�?与目标速度\n");
    {
        const SimResult r = run_sim(flick_sequence(150.0), 1.0 / 240.0, k_true, meas, act, {},
                                    default_motion_config());
        report("甩枪6�? 目标 150px/s", r);
        check(r.calibration_ready, "k̂ 应该 ready");
        check_near(r.px_per_count, k_true, 0.25 * k_true, "k̂ 误差 < 25%");
        check_near(r.velocity_est, 150.0, 60.0, "v̂ 误差 < 40%");
    }
    {
        const SimResult r = run_sim(flick_sequence(-400.0), 1.0 / 240.0, k_true, meas, act, {},
                                    default_motion_config());
        report("甩枪6�? 目标 -400px/s", r);
        check(r.calibration_ready, "k̂ 应该 ready(反向高�?");
        check_near(r.px_per_count, k_true, 0.3 * k_true, "k̂ 误差 < 30%(反向高�?");
        check(r.velocity_est < 0.0, "反向目标的速度符号正确");
    }
    {
        const SimResult r = run_sim(flick_sequence(150.0), 1.0 / 240.0, 0.8, meas, act, {},
                                    default_motion_config());
        report("甩枪6�? k=0.8", r);
        // 估计器要么给出准确的 k̂, 要么就明确不 ready(前馈自动关闭)。绝不能"自信地错"�?
        check(!r.calibration_ready || std::abs(r.px_per_count - 0.8) < 0.8 * 0.3,
              "k=0.8: 要么�?ready, 要么误差 < 30%");
    }
    {
        const SimResult r = run_sim(flick_sequence(150.0), 1.0 / 240.0, 0.05, meas, act, {},
                                    default_motion_config());
        report("甩枪6�? k=0.05", r);
        check(!r.calibration_ready || std::abs(r.px_per_count - 0.05) < 0.05 * 0.35,
              "k=0.05: 要么�?ready, 要么误差 < 35%");
    }

    // ── 4. 主动提前�? 稳态下真的瞄在目标前方 v*lead ────────────────────────
    std::printf("\n[4] 提前�?目标 200px/s, k̂ 已收�?\n");
    {
        for (double lead : {0.0, 0.03, 0.05})
        {
            boss::AimPidParams params;
            params.lead_time_s = lead;
            const std::vector<Segment> segs = {{0.8, 200.0, 300.0}, {0.8, 200.0, -500.0},
                                               {0.8, 200.0, 400.0}, {2.4, 200.0, 0.0}};
            const SimResult r = run_sim(segs, 1.0 / 240.0, k_true, meas, act, params,
                                        default_motion_config());
            report(("lead=" + std::to_string(static_cast<int>(lead * 1000.0)) + "ms").c_str(), r);
            check_near(r.tail_true_error, -200.0 * lead, 3.0, "稳态领先量 = v*lead");
        }
    }

    // ── 5. 延迟预测: 只影响瞬�? 不能在稳态引入偏�?─────────────────────────
    // 目标�?1.2s 处从静止突然加速到 300px/s, 这是"机动瞬�?"的标准场景。积分把滞后
    // 磨掉需要约 Ti(默认 1s), 所以这里看两件�? (a) 两个配置都不发散; (b) 开与不开�?
    // 稳态误差几乎一�?—�?也就是说延迟预测不会自己引入瞄偏�?
    std::printf("\n[5] 延迟预测(目标速度�?1.2s 阶跃 0 -> 300px/s)\n");
    {
        const std::vector<Segment> segs = {{1.2, 0.0, 300.0}, {0.3, 300.0, 0.0}, {3.5, 300.0, 0.0}};
        boss::AimPidParams off;
        off.predict_time_s = 0.0;
        boss::AimPidParams on;
        on.predict_time_s = meas + act;
        const SimResult a = run_sim(segs, 1.0 / 240.0, k_true, meas, act, off, default_motion_config(), 0.25);
        const SimResult b = run_sim(segs, 1.0 / 240.0, k_true, meas, act, on, default_motion_config(), 0.25);
        report("延迟预测 = 0", a);
        report("延迟预测 = 50ms", b);
        check(std::abs(a.tail_true_error) < 20.0, "不开延迟预测也有�?机动瞬态在收敛)");
        check(std::abs(b.tail_true_error - a.tail_true_error) < 4.0,
              "开了延迟预测不能在稳态引入偏�?");
        check(b.tail_abs_max < 40.0, "开了延迟预测不发散");
    }

    // ── 6. 限幅 / 取整余量 / 坏输�?─────────────────────────────────────────
    std::printf("\n[6] 限幅、取整余量、坏输入\n");
    {
        boss::AimPid pid;
        boss::AimPidParams params;
        params.limit_counts = 20;
        pid.configure(params);
        int max_counts = 0;
        for (int i = 0; i < 200; ++i)
            max_counts = std::max(max_counts, std::abs(pid.step(500.0, 1.0 / 240.0)));
        check(max_counts <= 20, "限幅生效(<=20 counts/�?");

        // 单独�?余量": 关掉积分, 只看 P 项是否把亚计数误差攒着发出去�?"
        boss::AimPid quiet;
        boss::AimPidParams quiet_params;
        quiet_params.ki = 0.0;
        quiet_params.kd = 0.0;
        quiet.configure(quiet_params);
        const double quiet_dt = 1.0 / 240.0;
        const int quiet_n = 400;
        const double expected = quiet_params.kp * 0.6 * (quiet_n * quiet_dt);  // Kp*e*T
        long sum = 0;
        int nonzero = 0;
        for (int i = 0; i < quiet_n; ++i)
        {
            const int c = quiet.step(0.6, quiet_dt);
            sum += c;
            if (c != 0)
                ++nonzero;
        }
        std::printf("  0.6px 常驻误差 -> 累计 %ld counts (理论 %.1f, 单拍指令 %.4f)\n", sum,
                    expected, quiet_params.kp * 0.6 * quiet_dt);
        check(nonzero > 0, "亚计数误差不会被丢掉(余量会攒到下一�?");
        check(std::abs(static_cast<double>(sum) - expected) < std::abs(expected) * 0.15 + 2.0,
              "余量累积出来的总计数接近理论�?Kp*e*T");

        boss::AimPid bad;
        bad.configure({});
        check(bad.step(std::nan(""), 1.0 / 240.0) == 0, "NaN 误差不发指令");
        check(bad.step(10.0, 0.0) != 0, "dt=0 不崩且仍给输�?");
        check(bad.step(10.0, -1.0) != 0, "dt<0 不崩且仍给输�?");
        check(bad.step(10.0, 100.0) != 0, "dt 极大不崩");

        boss::AimPid gain_bad;
        boss::AimPidParams bad_params;
        bad_params.kp = std::nan("");
        bad_params.ki = -5.0;
        bad_params.kd = std::nan("");
        bad_params.lead_time_s = std::nan("");
        gain_bad.configure(bad_params);
        const int c = gain_bad.step(50.0, 1.0 / 240.0);
        check(std::abs(c) <= 200, "非法增益退回内置默认后输出有限");
    }

    // ── 7. 换目标复�? 历史必须清干净 ───────────────────────────────────────
    std::printf("\n[7] 换目标复位\n");
    {
        boss::AimPid pid;
        pid.configure({});
        for (int i = 0; i < 100; ++i)
            pid.step(5.0, 1.0 / 240.0);  // 在小误差窗口内攒出积分与零头
        check(std::abs(pid.integralState()) > 0.0, "复位前应该攒了积�?");
        pid.reset();
        check(pid.integralState() == 0.0 && pid.carryState() == 0.0, "reset 清掉积分与零�?");
        check(pid.step(0.0, 1.0 / 240.0) == 0, "复位后误差为 0 时不应该有残留输�?");

        boss::AnchorMotionEstimator est;
        est.configure(default_motion_config());
        double camera = 0.0;
        for (int i = 0; i < 600; ++i)
        {
            const int counts = (i < 300) ? 40 : -40;
            const double anchor = 200.0 - camera;
            est.update(anchor, counts, 1.0 / 240.0, true);
            camera += static_cast<double>(counts) * 0.25;
        }
        const bool ready_before = est.calibrationReady();
        est.resetTargetMotion();
        check(est.targetVelocityPxPerSec() == 0.0, "换目标后速度清零");
        check(est.calibrationReady() == ready_before, "换目标保�?k̂ 标定");
        est.resetCalibration();
        check(!est.calibrationReady() && est.pxPerCount() == 0.0, "重标定会清掉 k̂");
    }

    // ── 8. 观测器必须拒绝没有激�?/ 只有预测框的输入 ────────────────────────
    std::printf("\n[8] 观测器的拒绝条件\n");
    {
        boss::AnchorMotionEstimator est;
        est.configure(default_motion_config());
        for (int i = 0; i < 600; ++i)
            est.update(0.2 * std::sin(i * 0.1), 0, 1.0 / 240.0, true);
        check(!est.calibrationReady(), "没有自运动激励时不该 ready");
        check(est.targetVelocityPxPerSec() == 0.0, "�?ready 时速度对外�?0");

        boss::AnchorMotionEstimator est2;
        est2.configure(default_motion_config());
        for (int i = 0; i < 600; ++i)
            est2.update(300.0 - 0.25 * 40.0 * i, 40, 1.0 / 240.0, false);  // 全是预测�?
        check(!est2.calibrationReady(), "全是预测框时不该 ready");
    }

    // ── 9. 检测框抖动不该被放大成鼠标抖动 ───────────────────────────────────
    std::printf("\n[9] 测量抖动\n");
    {
        for (double jitter : {1.0, 3.0})
        {
            boss::AimPid pid;
            boss::AimPidParams params;
            params.predict_time_s = 0.0;
            params.lead_time_s = 0.0;
            pid.configure(params);
            double sum_abs = 0.0;
            int n = 0;
            for (int i = 0; i < 600; ++i)
            {
                const double e = (i % 2 == 0) ? jitter : -jitter;
                const int c = pid.step(e, 1.0 / 240.0);
                if (i > 100)
                {
                    sum_abs += std::abs(c);
                    ++n;
                }
            }
            const double mean = sum_abs / std::max(1, n);
            std::printf("  ±%.0fpx 抖动 -> 平均 %.3f counts/拍\n", jitter, mean);
            check(mean < 8.0 * jitter, "抖动不该被放大成超过 8x 的计数输�?");
        }
    }

    // ── 10. 默认构造路径必须安�?不调�?configure) ──────────────────────────
    // 这是 app 的真实用�? AimEngine 直接按成员持�?AimPid / AnchorMotionEstimator,
    // 没有任何地方替它们调�?configure()。曾经因为这一�? 观测器内部保存拟合结果的
    // 数组长度�?0, 第一次拟合成功就往里写 �?一按瞄准键就闪退(2026-09-12)�?    // 这条回归就是要保�?默认构造出来就能用"�?    std::printf("\n[10] 默认构�?"不调 configure)路径\n");
    {
        boss::AimPid pid;  // 故意�?configure
        long worst = 0;
        for (int i = 0; i < 100; ++i)
            worst = std::max(worst, static_cast<long>(std::abs(pid.step(120.0, 1.0 / 240.0))));
        check(worst <= 200, "默认构造的 AimPid 输出有界");
        pid.reset();

        boss::AnchorMotionEstimator est;  // 故意�?configure
        // 逼真的甩�? 目标�?3.3s 瞬移到新位置, 用一个小比例控制把镜头追过去,
        // 这样既有足够的指令激�?|ΔU|), 又有"冲出去再收住"的曲�?否则 t �?U
        // 共线, k �?v 不可�? 拟合会被拒绝 —�?也就打不到崩过的那条路径)�?
        const double dt = 1.0 / 240.0;
        const double k_plant = 0.25;
        double camera = 0.0;
        double target = 300.0;
        int counts = 0;
        const double jumps[] = {300.0, -520.0, 420.0, -300.0};
        for (int i = 0; i < 4800; ++i)
        {
            if (i > 0 && i % 800 == 0)
            {
                target += jumps[(i / 800) % 4];
                est.resetTargetMotion();  // app 在换目标时就是这样做�?
            }
            const double anchor = target - camera;
            est.update(anchor, counts, dt, true);
            counts = static_cast<int>(std::lround(std::clamp(0.5 * anchor, -200.0, 200.0)));
            camera += static_cast<double>(counts) * k_plant;
        }
        std::printf("  观测�? 接受拟合 %d �? k̂ %.3f (真�?%.2f), v̂ %.1f\n", est.acceptedFits(),
                    est.pxPerCount(), k_plant, est.targetVelocityPxPerSec());
        // acceptedFits() > 0 说明真的走完�?往拟合槽位里写"那一�?就是崩过的那一�?�?"        check(est.acceptedFits() > 0, "默认构造的观测器会真正写入拟合结果(不闪退)");
        check(std::isfinite(est.pxPerCount()), "默认构造的观测�?k̂ 有限");
        check(std::isfinite(est.targetVelocityPxPerSec()), "默认构造的观测�?v̂ 有限");
        check(std::isfinite(est.countsDuring(0.05)), "默认构造的在途计数查询有�?");
    }

    // ── 11. 极端参数组合不能产生 NaN 指令 ──────────────────────────────────
    std::printf("\n[11] 极端参数\n");
    {
        boss::AimPid pid;
        boss::AimPidParams params;
        params.kp = 0.0;      // 积分项没有上限可�? 长时间跑可能溢出
        params.ki = 60.0;
        pid.configure(params);
        for (int i = 0; i < 20000; ++i)
        {
            const int c = pid.step(500.0, 1.0 / 240.0);
            check(std::abs(c) <= 200, "Kp=0/Ki=60 下输出仍然有�?不会 lround(NaN))");
            if (g_failures > 0)
                break;
        }
        boss::AimPid big;
        boss::AimPidParams huge;
        huge.kp = 400.0;
        huge.ki = 60.0;
        huge.kd = 0.3;
        huge.predict_time_s = 0.6;
        huge.lead_time_s = 0.6;
        big.configure(huge);
        for (int i = 0; i < 2000; ++i)
        {
            const int c = big.step(2000.0, 1.0 / 1000.0);
            if (std::abs(c) > 200)
            {
                check(false, "满量程参数下输出仍然有界");
                break;
            }
        }
    }

    // ── 12. dt 暴涨时第一版的行为(只记�?+ 安全边界) ────────────────────────
    // 第一版是【直接按真实 dt 缩放输出】。实机实�? 链路在换目标那一帧会跳掉一次检�?
    // (dt 8.3->16.7ms), 目标从视野外重新出现/松手再按还会长得�? 于是那一帧的指令�?
    // 按比例放�? 最多顶到输出上限。这里把量级记下�? 便于对照实机日志里的 cmd/lim�?
    std::printf("\n[12] dt 暴涨时的输出量级(第一�? 按真�?dt 缩放)\n");
    {
        boss::AimPid pid;
        pid.configure({});
        const double dt_nominal = 1.0 / 120.0;
        for (int i = 0; i < 120; ++i)
            pid.step(10.0, dt_nominal);
        const int normal = pid.step(150.0, dt_nominal);
        pid.reset();
        const int double_gap = pid.step(150.0, dt_nominal * 2.0);
        pid.reset();
        const int long_gap = pid.step(150.0, 0.5);
        pid.reset();
        const int huge_gap = pid.step(150.0, 20.0);
        std::printf("  同样误差 150px: 正常�?%d, 跳一�?%d, 半秒 %d, 20 �?%d counts (上限 %d)\n",
                    normal, double_gap, long_gap, huge_gap, pid.outputLimit());
        check(double_gap > std::abs(normal), "跳一帧确实会把这一帧的指令放大(第一版行�?");
        check(std::abs(double_gap) <= 200 && std::abs(long_gap) <= 200 && std::abs(huge_gap) <= 200,
              "再大也受输出上限约束, 不会无限放大");
    }

    // ── 13. 手填"每计数像�?"立刻生效(不等自动标定) ──────────────────────────
    // 动机: 自动估算只认"干净窗口", 换目标抽搐那种乱画面会被判无�?-> 估不出来 ->
    // 提前�?延迟预测整条被关�?-> 参数没变但手感换了一套。手填一个值就不会再这样�?
    std::printf("\n[13] 手填每计数像素\n");
    {
        boss::AnchorMotionEstimator est;          // 默认: 自动估算
        check(!est.calibrationReady(), "默认(自动)在没数据时不 ready");
        est.setManualPxPerCount(0.59);
        check(est.calibrationReady(), "手填之后立刻 ready");
        check_near(est.pxPerCount(), 0.59, 1e-9, "pxPerCount 直接返回手填�?");

        // 只跑 0.125s(远不够自动拟合所需�?0.5s 窗口), 速度估计就该可用�?
        for (int i = 0; i < 30; ++i)
            est.update(120.0, 0, 1.0 / 240.0, true);
        check(est.velocityReady(), "手填后速度估计可用(不必等拟�?");

        boss::AnchorMotionEstimator bad;
        bad.setManualPxPerCount(0.0);
        check(!bad.calibrationReady(), "0 = 退回自动估�?");
        bad.setManualPxPerCount(-3.0);
        check(!bad.calibrationReady(), "负数当成自动");
        bad.setManualPxPerCount(50.0);
        check(!bad.calibrationReady(), "超出物理范围的值当成自�?");
        bad.setManualPxPerCount(std::nan(""));
        check(!bad.calibrationReady(), "NaN 当成自动");
    }

    // ── 14. 死区已删�? P 项连续饱和取代它 ──────────────────────────────────
    // 历史: 这里原来�?死区边界不能震荡"的回�?—�?实机误差停在死区边缘来回�?"
    // 输出�?0"�?指令"之间每帧�?死区 3px 时实�?11 �?�?。当时的修法是给死区
    // 加迟�?�?3px / �?4.5px)。但 2026-09-12 实机复测仍然�?10.2 �?�?—�?
    // 迟滞只把环加�? 没改�?退出死区那一下满增益放出�?的过冲量�?
    //
    // 现在整段死区删除, 改成 P 项连续饱和。本节的断言只测【可判定的性质�?
    //   · 瞄点附近没有盲区(死区时代这里恒为 0)
    //   · 大误差段饱和到常数幅�?
    //   · 阈值为 0 时退回纯线�?
    //   · 复位后首拍不含残留微�?
    // 不再断言"跳变次数": 任何连续控制器在噪声下的输出本来就会过零, 过零次数
    // 不是死区与饱和的区别所�?两者的区别是盲区与过冲, �?[1] 的尾段误差与本节�?
    // 盲区断言分别覆盖)�?
    std::printf("\n[14] P 项连续饱�?取代死区)\n");
    {
        // 14a. 瞄点附近不再有盲�? 2px 的误差也必须出力�?
        // 关键�? 每一拍都用【全新的控制器�? 排除取整零头(carry)攒出来的假象 —�?
        // 死区时代 carry 在死区内被清�? 所以这里会恒为 0�?
        boss::AimPid near_pid;
        boss::AimPidParams np;
        np.ki = 0.0;      // 只看 P
        np.kd = 0.0;
        int nonzero_small = 0;
        for (int i = 0; i < 8; ++i)   // 2px * Kp 20 * dt 1/120 = 0.33 counts/�? 不够 1 —�?
        {                             // 所以这里测的是"P 项本身非�?, �?"cmd 判断
            near_pid.configure(np);
            near_pid.step(2.0, 1.0 / 120.0);
            if (std::abs(near_pid.pTerm()) > 1e-9)
                ++nonzero_small;
        }
        check(nonzero_small == 8, "2px 误差�?P 项恒非零(没有死区盲区)");

        // 14b. 大误差段饱和: 400px 只给出约 p_full_scale_px 的当量出�? 不是线性的 8 倍�?
        // 每拍都用新控制器, 避免 carry 与积分污染比值�?
        auto single_step = [](double e, double p_full) {
            boss::AimPid p;
            boss::AimPidParams pp;
            pp.ki = 0.0;
            pp.kd = 0.0;
            pp.p_full_scale_px = p_full;
            pp.limit_counts = 100000;   // 关掉输出限幅, 只看 P 项形状
            p.configure(pp);
            p.step(e, 1.0 / 120.0);
            return std::abs(p.pTerm());
        };
        const double r_sat = single_step(400.0, 100.0) / single_step(50.0, 100.0);
        std::printf("  饱和开: P(400px)/P(50px) = %.2f (纯线性会�?8.00, 饱和后应�?.00)\n", r_sat);
        check(r_sat > 1.6 && r_sat < 2.4, "大误差段饱和�?~100px 的当量出�?");

        // 14c. p_full_scale_px = 0 时必须退回纯线性�?
        const double r_lin = single_step(400.0, 0.0) / single_step(50.0, 0.0);
        std::printf("  饱和�? P(400px)/P(50px) = %.2f (应≈8.00)\n", r_lin);
        check(std::abs(r_lin - 8.0) < 0.1, "p_full_scale_px=0 时是纯线�?");

        // 14d. 首拍微分归零: 复位后第一拍不该把上一段遗留的微分状态放出来�?
        // 注意 reset() 会同时清积分, 所以首拍输出应当【正好】等于纯 P 项�?
        boss::AimPid fresh;
        boss::AimPidParams fp;
        fp.kd = 0.05;
        fp.ki = 0.0;
        fp.p_full_scale_px = 0.0;
        fp.limit_counts = 100000;
        fresh.configure(fp);
        for (int i = 0; i < 50; ++i)
            fresh.step(20.0 + 8.0 * i, 1.0 / 120.0);   // 攒一份很大的微分状态
        fresh.reset();
        fresh.step(120.0, 1.0 / 120.0);
        const double expect_p_only = 120.0 * fp.kp * (1.0 / 120.0);
        const double d_after = fresh.dTerm();
        std::printf("  复位后首�? dTerm=%.6f (必须�?0), �?P 项当�?%.3f\n", d_after, expect_p_only);
        check(std::abs(d_after) < 1e-12, "复位后首拍微分被清零");
    }

    // ── 15. 目标运动观测�?�?AVA 那套): 压抖 + 估目标速度 + 不把自己的指令当目标在动 ──
    // 实机: 目标�?x 每帧�?1.9px(中位)、每秒反�?21 �? 准星枢轴只抖 0.01px —�?抖的是框�?    // 控制器分不清"框在�?�?目标在动", 于是准星高频嗡嗡; 压枪时又需�?"目标自身速度"
    // 来顶住持续推力。观测器一次解决两件事�?
    std::printf("\n[15] 目标运动观测器\n");
    {
        const double dt = 1.0 / 120.0;
        const double k = 0.59;   // 每计数像�?用户手填的那�?

        // �?关闭平滑�? 位置一拍贴到测量�?这就是实机在用的 τ=0)
        boss::AnchorObserver off;
        off.configure(0.0, k);
        check(!off.enabled(), "0 = 不平�?位置一拍贴到测�?");
        check_near(off.step(123.456, 0, dt), 123.456, 1e-9, "不平滑时原样透传");

        // �?静止目标 + 检测框抖动: 输出要稳, 速度要接�?0
        boss::AnchorObserver obs;
        obs.configure(40.0, k);
        double sum_abs = 0.0;
        int n = 0;
        double prev = 0.0;
        for (int i = 0; i < 600; ++i)
        {
            const double noise = 1.9 * std::sin(i * 1.05) + ((i % 37 == 0) ? 9.0 : 0.0);
            const double y = obs.step(100.0 + noise, 0, dt);
            if (i > 20) { sum_abs += std::abs(y - prev); ++n; }
            prev = y;
        }
        const double jitter_out = sum_abs / std::max(1, n);
        std::printf("  静止�? 输入每帧�?1.9px(�?9px 野�? -> 输出每帧 %.3f px, v̂=%.1f px/s\n",
                    jitter_out, obs.velocity());
        check(jitter_out < 0.6, "检测框抖动被压�?");
        check(std::abs(obs.velocity()) < 40.0, "静止目标的速度估计接近 0");

        // ②b τ=0(实机配置)下静止目标的 v̂ 噪声有多�?—�?PID 里那个前馈噪声门的值就�?
        //    按这个定�?�?aim_pid.cpp �?kFeedforwardVelFloorPxS): 门必须高于噪声的
        //    p95, 否则静止目标也会被喂前馈�?
        {
            boss::AnchorObserver o0;
            o0.configure(0.0, k, 40.0, 0.011, 200.0);
            std::vector<double> av;
            for (int i = 0; i < 2400; ++i)
            {
                const double noise = 1.9 * std::sin(i * 1.05) + ((i % 37 == 0) ? 9.0 : 0.0);
                o0.step(100.0 + noise, 0, dt);
                if (i > 100)
                    av.push_back(std::abs(o0.velocity()));
            }
            std::sort(av.begin(), av.end());
            const auto q = [&](double f) { return av[static_cast<std::size_t>(f * (av.size() - 1))]; };
            std::printf("  τ=0 静止�?v̂ 噪声: p50=%.0f p95=%.0f p99=%.0f max=%.0f px/s\n",
                        q(0.5), q(0.95), q(0.99), av.back());
            check(q(0.95) < 100.0, "静止目标�?v̂ 噪声 p95 < 100px/s(前馈噪声门能挡住)");
        }

        // �?匀速移动目�? 速度要估得准, 位置滞后要小
        boss::AnchorObserver mv;
        mv.configure(40.0, k);
        double pos = 0.0;
        for (int i = 0; i < 400; ++i)
            pos = mv.step(100.0 + 2.0 * i, 0, dt);      // 2px/�?= 240px/s
        std::printf("  匀速靶 240px/s: v̂=%.0f px/s, 位置滞后 %.1fpx\n",
                    mv.velocity(), (100.0 + 2.0 * 399) - pos);
        check_near(mv.velocity(), 240.0, 60.0, "匀速目标速度估计误差 < 25%");
        check(std::abs((100.0 + 2.0 * 399) - pos) < 8.0, "匀速目标位置滞�?< 8px");

        // �?关键: 我们自己甩枪(画面因我们的指令平移)不能被当�?"目标在动"
        //    这里让目标【完全静止�? 只灌入我们的大指�?+ 对应的画面位移�?        // �?关键: 我们自己甩枪(画面因我们的指令平移)不能被当�?目标在动"�?        //    目标完全静止, 只灌入我们的大指�?+ 对应的画面位�? 而且画面位移要带【链�?        //    延迟�?指令发出�?35ms 后才在画面里生效)—�?这才和真实链路一致�?"
{
            const double plant_lag = 0.035;
            const int plant_ticks = static_cast<int>(plant_lag / dt + 0.5);
            boss::AnchorObserver fl;
            fl.configure(40.0, k, 40.0, plant_lag);

            std::vector<int> pending(static_cast<std::size_t>(plant_ticks), 0);
            double camera = 0.0;            // 画面里已经生效的自身位移
            double max_v = 0.0, max_lag = 0.0;
            for (int i = 0; i < 400; ++i)
            {
                const int counts = (i < 200) ? 20 : 0;      // �?200 拍然后停
                pending.push_back(counts);
                const int landed = pending.front();
                pending.erase(pending.begin());
                camera += landed * k;
                const double measured = 120.0 - camera;      // 目标静止, 画面在动
                const double out = fl.step(measured, counts, dt);
                if (i > 20) max_v = std::max(max_v, std::abs(fl.velocity()));
                if (i > 20) max_lag = std::max(max_lag, std::abs(measured - out));
            }
            std::printf("  自己甩枪(目标静止, �?5ms链路延迟): v̂ 最�?%.0f px/s, 输出最大落�?%.2f px\n",
                        max_v, max_lag);
            check(max_v < 60.0, "自己的位移没被当成目标运�?速度估计不被带飞)");
            check(max_lag < 3.0, "自己的位移被实时跟上(不滞�?");
        }

        // ④b 链路延迟估错也要�?真实�?20ms, 观测器按 35ms 假设)
        {
            const double plant_lag = 0.020;
            const int plant_ticks = static_cast<int>(plant_lag / dt + 0.5);
            boss::AnchorObserver fl;
            fl.configure(40.0, k, 40.0, 0.035);   // 故意写错
            std::vector<int> pending(static_cast<std::size_t>(plant_ticks), 0);
            double camera = 0.0, max_v = 0.0;
            for (int i = 0; i < 400; ++i)
            {
                const int counts = (i < 200) ? 20 : 0;
                pending.push_back(counts);
                const int landed = pending.front();
                pending.erase(pending.begin());
                camera += landed * k;
                fl.step(120.0 - camera, counts, dt);
                if (i > 20) max_v = std::max(max_v, std::abs(fl.velocity()));
            }
            std::printf("  延迟估错(真�?0ms/假设35ms): v̂ 最�?%.0f px/s\n", max_v);
            check(max_v < 250.0, "延迟估错时速度被短暂带偏但有界(实测�?170px/s �?2.6px 位移)");
        }
        // ④c �?实机配置(τ=0, 不平�?: 位置必须【一拍就跟上测量�? 同时速度仍然是干净的�?        //     顺便�?输入延迟用错"(用户原来把「延迟预测�?5ms 当输入延迟用)会烂成什么样�?"
{
            const double plant_lag = 0.010;                 // 真实链路盲区 �?10ms
            const int plant_ticks = static_cast<int>(plant_lag / dt + 0.5);
            struct Case { const char* name; double lag_s; };
            const Case cases[] = {
                {"输入延迟=真实(11ms)", 0.011},
                {"输入延迟=用户的旧�?75ms)", 0.075}};
            for (const Case& c : cases)
            {
                boss::AnchorObserver o;
                o.configure(0.0, k, 40.0, c.lag_s, 200.0);   // τ=0
                std::vector<int> pending(static_cast<std::size_t>(plant_ticks), 0);
                double camera = 0.0, max_lag = 0.0, max_v = 0.0;
                for (int i = 0; i < 400; ++i)
                {
                    const int counts = (i < 200) ? 20 : 0;   // �?200 拍然后停
                    pending.push_back(counts);
                    const int landed = pending.front();
                    pending.erase(pending.begin());
                    camera += landed * k;
                    const double measured = 120.0 - camera;  // 目标静止, 画面在动
                    const double out = o.step(measured, counts, dt);
                    if (i > 30)
                    {
                        max_lag = std::max(max_lag, std::abs(measured - out));
                        max_v = std::max(max_v, std::abs(o.velocity()));
                    }
                }
                std::printf("  %s: 位置最大落�?%.2f px, v̂ 最�?%.0f px/s\n",
                            c.name, max_lag, max_v);
                if (c.lag_s < 0.02)
                {
                    check(max_lag < 0.01, "τ=0 时位置一拍贴到测�?零滞�?");
                    check(max_v < 60.0, "真实输入延迟下自身位移没被当成目标运�?");
                }
                else
                {
                    check(max_v > 200.0, "输入延迟用错(75ms = 9�?会造出几百px/s的假速度 —�?抽搐根因");
                }
            }
        }

        // ④d ★★ 闭环对照, 直接复现"瞄到锚点后抽�?":
        //     静止目标 + 一次甩�? 观测器的速度【真的喂�?PID 前馈�?和实机接线一�?�?
        //     输入延迟对了就该安静停下; 用错(75ms)则假速度 -> 几十像素假误�?-> 正反馈发散�?
        //     第三组加了实测量级的框抖�?±1.9px + 偶尔 9px 野�?: 位置不过�? 准星就会跟着
        //     �?—�?这一组是用来�?到底会抖多少像素"�? 不是判据�?"
{
            const double plant_lag = 0.010;
            const int plant_ticks = static_cast<int>(plant_lag / dt + 0.5);
            const char* names[3] = {"输入延迟=真实(11ms), 无抖�?",
                                    "输入延迟=用户的旧�?75ms), 无抖�?",
                                    "输入延迟=真实(11ms) + 实测框抖�?"};
            const double lags[3] = {0.011, 0.075, 0.011};
            const double noises[3] = {0.0, 0.0, 1.9};
            for (int c = 0; c < 3; ++c)
            {
                boss::AimPidParams p;
                p.kp = 100.0; p.ki = 1.0; p.kd = 0.01;      // 实机参数
                p.predict_time_s = 0.075; p.lead_time_s = 0.05;
                p.p_full_scale_px = 100.0; p.limit_counts = 200;
                boss::AimPid pid;
                pid.configure(p);
                boss::AnchorObserver obs;
                obs.configure(0.0, k, 40.0, lags[c], 200.0);

                std::vector<int> pending(static_cast<std::size_t>(plant_ticks), 0);
                double camera = 0.0;
                int n = 0, last_cmd = 0, max_abs = 0;
                double sum_abs = 0.0, tail_err = 0.0;
                for (int i = 0; i < 600; ++i)
                {
                    // 目标静止�?+200px �? 第三组给测量加抖�?检测框实测抖动)�?
                    double noise = 0.0;
                    if (noises[c] > 0.0)
                        noise = noises[c] * std::sin(i * 1.05) + ((i % 37 == 0) ? 9.0 : 0.0);
                    const double measured = 200.0 - camera + noise;
                    const double anchor = obs.step(measured, last_cmd, dt);
                    boss::AimPidFeedback fb;
                    fb.target_velocity_px_s = obs.velocity();
                    const int cmd = pid.step(anchor, dt, fb);
                    last_cmd = cmd;
                    pending.push_back(cmd);
                    const int landed = pending.front();
                    pending.erase(pending.begin());
                    camera += landed * k;
                    tail_err = 200.0 - camera;
                    if (i > 300)                                   // �?2.5 �?
                    {
                        const double a = std::abs(static_cast<double>(cmd));
                        sum_abs += a;
                        max_abs = std::max(max_abs, static_cast<int>(a + 0.5));
                        ++n;
                    }
                }
                std::printf("  %s: 末段 |指令|均�?%.2f / 最�?%d counts (准星每拍�?%.2f px), "
                            "静态残�?%.2f px\n",
                            names[c], sum_abs / std::max(1, n), max_abs,
                            k * sum_abs / std::max(1, n), tail_err);
                if (c == 0)
                {
                    check(sum_abs / std::max(1, n) < 1.0, "输入延迟正确时静止目标末段指令收敛到 0");
                    check(max_abs <= 2, "输入延迟正确时末段没有大幅指�?最�?1~2 个计数的余量)");
                    check(std::abs(tail_err) < 2.0, "输入延迟正确时稳态误�?< 2px");
                }
                else if (c == 1)
                {
                    check(sum_abs / std::max(1, n) > 20.0,
                          "输入延迟用错(75ms)时末段指令大幅抖�?发散 —�?这就是抽搐根�?");
                }
                else
                {
                    // 这一组只做量级参�?框抖动是合成的最坏情�? ±1.9px/6Hz + �?37 帧一�?
                    // 9px 尖峰)。位置不过滤, 准星必然会跟着�?—�?想压住它只有两条�?
                    // 给位置加 8~15ms 平滑, 或者降 Kp。这里只保证不发散�?
                    check(max_abs < 40, "有框抖动时不发散(准星每拍位移有界)");
                }
            }
        }

        // �?�?引擎每拍都会�?configure()(参数来自配置快照)。参数没变时必须【不清状态�?
        //    否则延迟线永远攒不满 -> 速度恒为 0 -> 前馈整个是死�?2026-09-12 在实机上抓到)�?
        {
            boss::AnchorObserver o;
            double sum_v = 0.0;
            int cnt = 0;
            for (int i = 0; i < 400; ++i)
            {
                o.configure(0.0, k, 40.0, 0.011, 200.0);   // 每拍都调, 参数完全一样
                o.step(100.0 + 2.0 * i, 0, dt);            // 匀速目标 240px/s
                if (i > 100)
                {
                    sum_v += o.velocity();
                    ++cnt;
                }
            }
            const double avg_v = sum_v / std::max(1, cnt);
            std::printf("  每拍 configure(参数不变): v̂ 平均 %.0f px/s (真�?240)\n", avg_v);
            check_near(avg_v, 240.0, 40.0, "每拍 configure 不能丢状�?速度仍然估得出来)");

            // 参数真的变了 -> 必须清状态(换了每计数像素, 旧的延迟线与速度作废)。
            o.configure(0.9, k, 40.0, 0.011, 200.0);
            check(o.velocity() == 0.0, "参数变了要清状�?速度归零)");
        }

        // �?野值门�?
        boss::AnchorObserver gt;
        gt.configure(40.0, k);
        double base = 0.0;
        for (int i = 0; i < 100; ++i)
            base = gt.step(100.0, 0, dt);
        const double spiked = gt.step(100.0 + 120.0, 0, dt);   // 单帧 120px 野�?
        std::printf("  单帧 120px 野�? 输出只动�?%.1f (基准 %.1f), gated=%d\n",
                    spiked, base, gt.lastGated() ? 1 : 0);
        check(std::abs(spiked - base) < 5.0, "野值被门限挡掉");
    }

    // [16] 速度前馈的噪声门 —�?Kp=100 �?瞄到锚点后抽�?的另一半原�?
    //
    // 提前�?延迟预测都是 `v̂ * 时间`, 而静止目标的 |v̂| 只是框抖动的噪声(几十 px/s):
    // �?0.125s 就是几像素假误差, 再乘 Kp*dt=0.83 就是每拍几个计数在瞄点上嗡嗡�?
    // 门限: |v̂| < 60px/s 完全不给前馈, > 150px/s 满额�?
    std::printf("\n[16] 速度前馈噪声门\n");
    {
        const double dt = 1.0 / 120.0;
        const auto ff_at = [&](double v) {
            boss::AimPidParams p;
            p.kp = 100.0; p.ki = 0.0; p.kd = 0.0; p.p_full_scale_px = 100.0;
            p.predict_time_s = 0.075; p.lead_time_s = 0.05; p.limit_counts = 200;
            boss::AimPid pid;
            pid.configure(p);
            boss::AimPidFeedback fb;
            fb.target_velocity_px_s = v;
            // 误差恒为 0: 输出就只剩前馈那一份�?
            int last = 0;
            double ff = 0.0, scale = 0.0;
            for (int i = 0; i < 40; ++i)
            {
                last = pid.step(0.0, dt, fb);
                ff = pid.feedforwardPx();
                scale = pid.feedforwardScale();
            }
            return std::make_tuple(last, ff, scale);
        };
        const auto [c_noise, ff_noise, s_noise] = ff_at(45.0);      // 静止目标的噪声量�?
        const auto [c_mid, ff_mid, s_mid] = ff_at(85.0);            // 门中�?60~110 �?50%)
        const auto [c_fast, ff_fast, s_fast] = ff_at(300.0);        // 真的在动
        std::printf("  v̂=45 -> 前馈 %.2fpx(�?%.2f) 指令 %d | v̂=85 -> %.2fpx(�?%.2f) %d"
                    " | v̂=300 -> %.2fpx(�?%.2f) %d\n",
                    ff_noise, s_noise, c_noise, ff_mid, s_mid, c_mid, ff_fast, s_fast, c_fast);
        check(s_noise == 0.0 && std::abs(ff_noise) < 1e-9, "静止目标的噪声速度不产生任何前�?");
        check(c_noise == 0, "静止目标(只有噪声速度)时指令为 0");
        check_near(s_mid, 0.5, 0.01, "门中间按比例给一半前�?");
        check(ff_mid > 1.0, "门中间真的给了前�?不是全关)");
        check_near(s_fast, 1.0, 1e-9, "真在动时前馈满额");
        check_near(ff_fast, 300.0 * (0.075 + 0.05), 1e-6, "满额前馈 = v̂*(延迟预测+提前�?");
        check(c_fast > 20, "真在动时前馈真的推了指令");
    }

    // [17] ★★�?死区补偿(在途自身位�? —�?实机 Kp=100 抽搐的真正机理与解法
    //
    // 实机日志(2026-09-12 18:00 那场)给出的硬数据:
    //   · X �?Kp=100: 误差�?±150px 之间�?~5Hz 摆动, 指令�?±85 counts 之间来回 —�?极限�?
    //   · Y �?Kp=30 : 同一目标、同一链路, ey 中位 2.4px / p90 6.1px —�?安静
    //   只有增益不同 => 是【增�?vs 链路死区】的关系, 不是滤波/选择层的问题�?
    //
    // 把环路写�?每拍一次积�?+ d 拍纯延迟"(相机 = k*Σ计数, 延迟 d 拍才反映到测�?":
    //   y_n = y_
    //   y_n = y_{n-1} - g*y_{n-d} + 扰动,  g = Kp*dt*k (每拍环路增益)
    //   稳定性边�?g_crit(d) = 2*sin(π/(2(2d+1))), 振荡周期 = 2(2d+1) �?
    //   实测周期 24.5 �?-> d = 5.6 �?�?47ms;  g_crit = 0.268 -> Kp_crit �?54
    //   实机 g = 100*0.00833*0.593 = 0.494 = 1.9*g_crit -> 正好�?持续极限�?(幅度�?psat 限住)
    //   Kp=30 �?g = 0.148 < 0.268 -> 稳。这就是"30 行�?00 �?的完整解释�?"    //
    // 解法: Smith 补偿 —�?�?已经发出去、游戏里已经生效、只是画面还没回�?的那批计�?
    // (最�?d �?从看到的误差里扣�? 让控制器按【当前真实误差】动�?
    //   e_used = e_meas - k*Σ(最�?d 秒的计数) + v̂*d
    // 这样"自身指令 -> 误差"这条通路不再有延迟, 环路退化成 y_n = y_{n-1} - g*y_{n-1}:
    // 每拍收缩 (1-g) �? Kp=100 �?g=0.494 -> 一拍收敛一�? 而且【不振荡】�?
    // 注意 k 必须�?手填那个): 判据只要�?g*k_误差 < 2, �?k �?4 倍以内都稳�?
    std::printf("\n[17] 死区补偿(在途自身位�? Smith)\n");
    {
        struct LoopResult
        {
            double tail_err = 0.0;      // 末段平均误差(px)
            double amp = 0.0;           // 末段峰峰�?px)
            double period = 0.0;        // 末段过零周期(�?
            double settle_s = -1.0;     // 连续 20 拍进�?±3px 的时�?
            double max_u = 0.0;         // 末段最�?|指令|(counts)
        };

        // k_plant: 假游戏的灵敏�? k_used: 补偿里用�?每计数像�?(实机�?用户手填�?
        const auto run_loop = [&](double meas_delay, double act_delay, double k_plant, double k_used,
                                  double window_s, double scale, double gain_scale, bool compensate) {
            const double dt = 1.0 / 120.0;
            const int m_ticks = static_cast<int>(std::lround(meas_delay / dt));
            const int a_ticks = static_cast<int>(std::lround(act_delay / dt));
            const int w_ticks = std::max(0, static_cast<int>(std::lround(window_s / dt)));

            boss::AimPidParams p;
            p.kp = 100.0 * gain_scale; p.ki = 1.0; p.kd = 0.01;
            p.predict_time_s = 0.0; p.lead_time_s = 0.0;   // 只测自身运动这一条通路
            p.p_full_scale_px = 100.0; p.limit_counts = 200;
            boss::AimPid pid;
            pid.configure(p);

            std::deque<int> act_line(static_cast<std::size_t>(a_ticks), 0);
            std::deque<double> meas_line(static_cast<std::size_t>(m_ticks), 0.0);
            std::deque<int> cmd_hist;                       // 最近发出去的计�?
            double camera = 0.0;                            // 画面里已经生效的自身位移
            const double target = 200.0;                    // 静止目标(只测自身运动)
            LoopResult r;
            std::vector<double> tail, series;
            int good = 0;
            for (int i = 0; i < 720; ++i)                   // 6s
            {
                const double true_err = target - camera;
                meas_line.push_back(true_err);
                const double meas = meas_line.front();
                meas_line.pop_front();

                double pending_px = 0.0;
                if (compensate && k_used > 0.0)
                {
                    double sum = 0.0;
                    for (int j = 0; j < std::min<int>(w_ticks, static_cast<int>(cmd_hist.size())); ++j)
                        sum += cmd_hist[cmd_hist.size() - 1 - j];
                    pending_px = scale * k_used * sum;
                }
                boss::AimPidFeedback fb;
                fb.pending_self_motion_px = pending_px;
                const int u = pid.step(meas, dt, fb);

                cmd_hist.push_back(u);
                if (static_cast<int>(cmd_hist.size()) > 64)
                    cmd_hist.pop_front();
                act_line.push_back(u);
                const int landed = act_line.front();
                act_line.pop_front();
                camera += landed * k_plant;                     // 相机 = k_plant * 累计计数

                if (i > 240) r.max_u = std::max(r.max_u, std::abs(static_cast<double>(u)));
                if (i > 240) series.push_back(meas);
                // 收敛判定: 从第 20 拍起, 连续 60 拍都�?±3px �?
                if (i > 20)
                {
                    if (std::abs(meas) < 3.0)
                    {
                        if (++good == 60 && r.settle_s < 0.0) r.settle_s = i * dt;
                    }
                    else
                        good = 0;
                }
            }
            double sum = 0.0, lo = 1e9, hi = -1e9;
            for (double v : series) { sum += std::abs(v); lo = std::min(lo, v); hi = std::max(hi, v); }
            r.tail_err = sum / std::max<std::size_t>(1, series.size());
            r.amp = hi - lo;
            // 主频(DFT 扫频) -> 反推极限环周�? 与理�?2(2d+1) 对比
            const double T = dt;
            double bestF = 0.0, bestMag = -1.0;
            for (double fr = 1.0; fr <= 40.0; fr += 0.25)
            {
                const double w = 2.0 * 3.14159265358979 * fr;
                double re = 0.0, im = 0.0;
                for (std::size_t k = 0; k < series.size(); ++k)
                {
                    re += series[k] * std::cos(w * static_cast<double>(k) * T);
                    im -= series[k] * std::sin(w * static_cast<double>(k) * T);
                }
                const double mg = std::sqrt(re * re + im * im) / static_cast<double>(series.size());
                if (mg > bestMag) { bestMag = mg; bestF = fr; }
            }
            r.period = bestF > 0.0 ? 1.0 / (bestF * T) : 0.0;   // 用拍数表�?
            return r;
        };

        const double k_real = 0.593;        // 手填的每计数像素(实机�?
        const double meas_d = 0.012;        // 画面->控制�?�?12ms(1 �?
        const double act_d = 0.036;         // 计数->画面 �?36ms(4 �?
        const double dead = meas_d + act_d; // 合计 �?48ms(实机日志反推 �?6ms)

        const LoopResult off = run_loop(meas_d, act_d, k_real, k_real, dead, 0.0, 1.0, false);
        const double d_ticks = dead / (1.0 / 120.0);
        std::printf("  Kp=100 不补�?     : 末段|e|=%.1fpx 峰峰=%.1fpx 主周�?%.1f�?理论%.1f) 最大指�?%.0f\n",
                    off.tail_err, off.amp, off.period, 2.0 * (2.0 * d_ticks + 1.0), off.max_u);
        check(off.amp > 60.0, "不补偿时复现实机的极限环(峰峰 > 60px) —�?与日志一�?");
        check(std::abs(off.period - 2.0 * (2.0 * d_ticks + 1.0)) < 0.45 * off.period,
              "极限环周期符合理�?2(2d+1)(实机日志实测 24 �?@5Hz)");

        // 扫描补偿强度(窗口 = 精确死区, 整体乘一个系�?�?        // �?关键结论: 补偿【不足】只是回到原来的延迟(安全); 补偿【过头】会变成正反�?-> 发散�?        //   所以用"完整窗口 × 0.7"这种保守形式, 而不�?缩短窗口"�?        std::printf("  补偿强度扫描(窗口=精确死区, 整体乘系�?":\n");
        std::printf("    系数  真死�?8ms  死区29ms  k高估1.5�? k低估�?.4  Kp150\n");
        const double scales[] = {0.0, 0.3, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2};
        for (double sc : scales)
        {
            const LoopResult a = run_loop(meas_d, act_d, k_real, k_real, dead, sc, 1.0, sc > 0.0);
            const LoopResult b = run_loop(meas_d * 0.6, act_d * 0.6, k_real, k_real, dead, sc, 1.0, sc > 0.0);
            const LoopResult c = run_loop(meas_d, act_d, k_real, k_real * 1.5, dead, sc, 1.0, sc > 0.0);
            const LoopResult d = run_loop(meas_d, act_d, 0.40, k_real, dead, sc, 1.0, sc > 0.0);
            const LoopResult e = run_loop(meas_d, act_d, k_real, k_real, dead, sc, 1.5, sc > 0.0);
            std::printf("    %4.1f %11.2f %10.2f %11.2f %11.2f %7.2f\n",
                        sc, a.amp, b.amp, c.amp, d.amp, e.amp);
        }

        const LoopResult on = run_loop(meas_d, act_d, k_real, k_real, dead, 0.7, 1.0, true);
        std::printf("  Kp=100 补偿0.7×46ms: 末段|e|=%.2fpx 峰峰=%.2fpx 收敛@%.3fs 最大指�?%.0f\n",
                    on.tail_err, on.amp, on.settle_s, on.max_u);
        check(on.tail_err < 1.0, "补偿后末段平均误�?< 1px(Kp=100 也不抖了)");
        check(on.amp < 4.0, "补偿后不再振�?");

        // 实机最坏情�? k 手填值比真实灵敏度高 1.5 �?日志里两个独立估计给 0.40~0.68)
        const LoopResult bad_k = run_loop(meas_d, act_d, 0.40, k_real, dead, 0.7, 1.0, true);
        std::printf("  k 高估 1.5 �?     : 末段峰峰=%.2f px\n", bad_k.amp);
        check(bad_k.amp < 10.0, "k 手填值偏�?1.5 倍时不发�?�?0.7 的保守系数兜�?");
    }

    // [18] �?锁到位之后的"抖几�? —�?到位的过�?"振铃
    //
    // 实机日志(2026-09-12 18:29 那场)实测: 181 �?换目�?重锁"�? 161 �?89%)在之�?60 �?    // (0.5s)�?ex 过零 >=4 �?—�?也就是到位之后还要来回摆几下, 摆幅�?15~20px�?    // 这一段就是拿假游戏复现这�?到位振铃", 并量化两个旋钮的影响:
    //   · Kd(过冲控制): 微分项在振荡频率上提供相位超�? 是压到位振铃的正牌工�?
    //   · 在途补偿强�? 补不足会留一点延�?顺序: 先过冲再慢慢�?�?
    std::printf("\n[18] 到位过冲/振铃(换目标那一拍开始仿�?\n");
    {
        struct Settle
        {
            double first_cross_s = -1.0;   // 第一次过零的时刻
            double peak_after = 0.0;       // 过零之后的最大反向误�?过冲)
            int zero_cross = 0;            // 0.6s 内过零次�?
            double tail_abs = 0.0;
        };
        const auto acquire = [&](double kd, double comp_scale, double kp, double window_s,
                                 double k_used, double k_plant, int mode = 0) {
            const double dt = 1.0 / 120.0;
            const int m_ticks = static_cast<int>(std::lround(0.012 / dt));
            const int a_ticks = static_cast<int>(std::lround(0.036 / dt));
            const int w_ticks = std::max(1, static_cast<int>(std::lround(window_s / dt)));

            boss::AimPidParams p;
            p.kp = kp; p.ki = 1.0; p.kd = kd;
            p.predict_time_s = 0.0; p.lead_time_s = 0.0;
            p.p_full_scale_px = 100.0; p.limit_counts = 200;
            boss::AimPid pid;
            pid.configure(p);
            std::deque<int> act_line(static_cast<std::size_t>(a_ticks), 0);
            std::deque<double> meas_line(static_cast<std::size_t>(m_ticks), 0.0);
            std::deque<int> cmd_hist;
            double camera = 0.0;
            const double target = 200.0;          // 新目标在准星右边 200px
            Settle s;
            double prev_e = 0.0;
            std::vector<double> tail;
            for (int i = 0; i < 240; ++i)         // 2s
            {
                meas_line.push_back(target - camera);
                const double meas = meas_line.front();
                meas_line.pop_front();
                double pending = 0.0;
                if (comp_scale > 0.0 && k_used > 0.0)
                {
                    // mode 0: 窗口 = round(死区/dt) �?实机现役)
                    // mode 1: 窗口 = round(死区/dt)-1 �?少算边界那一�?= 已经落地的那�?
                    // mode 2: 物理正确�?在�?集合 = 长度 a 拍、结尾在 b 拍之�?
                    double sum = 0.0;
                    if (mode == 2)
                    {
                        const int b = a_ticks;                     // 执行延迟
                        const int a = m_ticks;                     // 测量延迟
                        for (int j = 0; j < a; ++j)
                        {
                            const int back = b + j;                // �?newest 往前数
                            if (back < static_cast<int>(cmd_hist.size()))
                                sum += cmd_hist[cmd_hist.size() - 1 - back];
                        }
                    }
                    else
                    {
                        const int n = mode == 1 ? std::max(1, w_ticks - 1) : w_ticks;
                        for (int j = 0; j < std::min<int>(n, static_cast<int>(cmd_hist.size())); ++j)
                            sum += cmd_hist[cmd_hist.size() - 1 - j];
                    }
                    pending = comp_scale * k_used * sum;
                }
                boss::AimPidFeedback fb;
                fb.pending_self_motion_px = pending;
                const int u = pid.step(meas, dt, fb);
                cmd_hist.push_back(u);
                if (static_cast<int>(cmd_hist.size()) > 64) cmd_hist.pop_front();
                act_line.push_back(u);
                const int landed = act_line.front();
                act_line.pop_front();
                camera += landed * k_plant;

                if (i > 0)
                {
                    if (prev_e > 0.0 && meas <= 0.0 && s.first_cross_s < 0.0)
                        s.first_cross_s = i * dt;
                    if (s.first_cross_s > 0.0 && i * dt <= 0.6)
                        s.peak_after = std::max(s.peak_after, -meas);
                    if (i * dt <= 0.6 && std::signbit(meas) != std::signbit(prev_e)
                        && meas != 0.0 && prev_e != 0.0)
                        ++s.zero_cross;
                }
                prev_e = meas;
                if (i * dt > 1.0) tail.push_back(std::abs(meas));
            }
            double sum = 0.0;
            for (double v : tail) sum += v;
            s.tail_abs = sum / std::max<std::size_t>(1, tail.size());
            return s;
        };

        const double k = 0.593;
        const double dead = 0.048;
        std::printf("  在途补偿窗口的三种取法(实机日志: 15245 帧补偿声�?61px 在�? 而那一批已经落�?->\n");
        std::printf("  下一拍发�?-55 counts 的反向踢�?= 到位后那几下�?:\n");
        std::printf("   取法                        到位过冲(px)  过零次数  0.6s 尾段|e|\n");
        const char* modeName[3] = {"A0 现役: 窗口=round(死区/dt)",
                                   "A1 减一�? 窗口=round(死区/dt)-1",
                                   "B  物理正确: 结尾提前 b �?"};
        for (int mode = 0; mode < 3; ++mode)
        {
            const Settle s = acquire(0.01, 0.8, 100.0, dead, k, k, mode);
            std::printf("    %-26s %10.2f  %8d  %9.2f\n", modeName[mode], s.peak_after, s.zero_cross, s.tail_abs);
        }
        std::printf("  降到 Kp=70(如果取法 B 更准但增益要退一�?:\n");
        for (int mode = 0; mode < 3; ++mode)
        {
            const Settle s = acquire(0.01, 0.8, 70.0, dead, k, k, mode);
            std::printf("    %-26s %10.2f  %8d  %9.2f\n", modeName[mode], s.peak_after, s.zero_cross, s.tail_abs);
        }

        const double kd_unused = 0.0;
        (void)kd_unused;
        std::printf("  Kd     过冲(px)  过零次数  0.6s 尾段|e|\n");
        for (double kd : {0.0, 0.01, 0.02, 0.03, 0.05, 0.08})
        {
            const Settle s = acquire(kd, 0.8, 100.0, dead, k, k, 0);
            std::printf("  %.3f  %8.2f  %8d  %9.2f\n", kd, s.peak_after, s.zero_cross, s.tail_abs);
        }
        std::printf("  补偿强度对到位的影响(Kd=0.01):\n");
        for (double sc : {0.0, 0.5, 0.8, 1.0})
        {
            const Settle s = acquire(0.01, sc, 100.0, dead, k, k, 0);
            std::printf("    comp=%.1f  过冲(px) %7.2f  过零 %3d  尾段|e| %6.2f\n",
                        sc, s.peak_after, s.zero_cross, s.tail_abs);
        }
        std::printf("  瞄准速度对到位的影响(comp=0.8):\n");
        for (double kp : {50.0, 100.0, 150.0})
        {
            const Settle s = acquire(0.01, 0.8, kp, dead, k, k, 0);
            std::printf("    Kp=%.0f  过冲(px) %7.2f  过零 %3d  尾段|e| %6.2f\n",
                        kp, s.peak_after, s.zero_cross, s.tail_abs);
        }

        // 结论性判�? 默认参数下过冲不该超�?~10px, 且抬 Kd 必须真的把它压下�?
        const Settle base = acquire(0.01, 0.8, 100.0, dead, k, k, 0);
        const Settle damped = acquire(0.03, 0.8, 100.0, dead, k, k, 0);
        std::printf("  默认(Kd=0.01)过冲 %.2fpx -> Kd=0.03 过冲 %.2fpx\n", base.peak_after, damped.peak_after);
        check(base.peak_after < 120.0, "默认参数下到位过冲有界(<120px; 实机日志实测约 20px)");
        check(base.tail_abs < 1.0, "到位 1s 后稳定在 1px �?");
        check(damped.peak_after > base.peak_after, "抬 Kd 会让到位过冲变差(所以 Kd 必须小)");
    }

    if (g_failures == 0)
    {
        std::printf("\naim pid: passed\n");
        return EXIT_SUCCESS;
    }


    std::printf("\naim pid: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
