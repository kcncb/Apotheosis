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
// 场景保真度(重要): 早期版本把"换向/落地/急停"写成速度瞬间阶跃, 而真实动作必然有
// 加速/减速过程, 那会把这些场景的误差夸大 2-3 倍(实测换向 p95 34.3 -> 14.9)。所以本
// 套件同时保留两套: 【缓】= 带加速度的真实过程, 【无后缀】= 物理上界(压力测试)。
// 同理跳跃必须用抛物线(重力连续减速), 连跳的下降段必须与上升段对称。
//
// 本套件测出来的结论(都是实测, 不是推导):
//   1) 「锁定强度」kf=0 会让前馈整条关死(ff_output = ff_state*dt*kf = 0), 此时「预测
//      速度」lr 毫无作用; 正确值 kf=1, >1 会过度提前、全面变差。
//   2) 「过冲控制」kd 的安全上限是 0.05: kd=0.08 在 3 帧延迟下略好(17.15->16.37),
//      但在 4 帧延迟下直接发散(355287)。
//   3) 「预测速度」lr 到 0.05 基本饱和(0.10 无进一步收益, 0.15 以上明显变差)。
//   4) 「瞄准速度」kp 与链路延迟补偿配套: 补偿开启后 kp=2.0 才既安全又最优。
//   5) 【否定结果】重锁时"保留前馈速度估计"(省掉从 0 重学)对结果无影响 —— 实测
//      翻滚(遮挡) 的 p95 与总体分完全不变。因为遮挡结束时目标往往已经停了, 旧速度
//      反而是错的; 且重锁代价本身不大(带遮挡 34.75 对不带遮挡 28.25)。
//   6) 【否定结果】比例项按误差幅值做增益调度(小误差保持原增益、大误差加权)在 3 帧
//      延迟下有效(翻滚缓 24.85->20.32, 喷气 17.95->15.42), 但会吃掉延迟裕度:
//      boost=0.3/门槛20px 时 4 帧延迟从 19.55 恶化到 29.77, boost>=0.5 直接发散。
//      用稳健性换约 2% 综合分不划算, 未采用。
//   7) 【最重要】"速度估计"确实是当前最大的剩余瓶颈, 但它是【回路】限制而不是算法限制:
//      给控制器【完美速度信息】(oracle)后, 3 帧延迟下综合分 17.15 -> 7.07(2.42 倍),
//      翻滚缓 24.85 -> 5.61、喷气 17.95 -> 3.67。即"知道真实速度"值 2.4 倍。
//      但没有任何因果估计器能拿到它:
//        · 提高递归增益(lr>=0.10)会失稳(17.1 -> 19.9 -> 40+);
//        · 自适应增益(用|创新|触发)会失稳(10+ 变体);
//        · 卡尔曼滤波只是【同一增益的重新参数化】(过程噪声 Q=50 <-> 等效 lr=0.0496,
//          与手调的 0.05 重合), 在等效增益下并不更好; 按真实机动加速度设的 Q(>=400)
//          会失稳;
//        · 把跟踪器卡尔曼平滑后的位置接进来(降低测量噪声)不改变最优 lr, 分数反而
//          略差(17.15 -> 17.77) —— 跟踪器自身的滞后比它消掉的噪声更贵。
//      结论: 估计器的速度被【回路延迟裕度】锁死, 不是被信息量锁死。要吃下这 3 倍,
//      只能减少端到端延迟(采集卡/链路), 或整体换成显式建模延迟的状态反馈结构。
//
//   7b) 【延迟 <-> 可达性能】的量化。⚠️ 2026 修订: 之前这张表是在一个【建模错误】
//       的测试台上测的 —— 指令延迟队列实际给出 mouse_lat+1 帧延迟, 却对控制器声明
//       mouse_lat 帧, 于是观测器的"已知指令"比真实早一帧, 创新里带上与指令变化相关
//       的系统误差, 高学习率会被放大到发散。那让"回路锁死估计器"的结论里混进了
//       测试台假象(追踪证据: 恒定 250px/s 下 ff 估计冲到 520, 且误差每帧变化 2.06px
//       而按 v*dt-指令 应为 1.08px)。修正后(检测2帧+指令1帧, 学习率按实测延迟定档):
//         总延迟        2帧      3帧      4帧      5帧
//         lr=0.05     12.05    14.40    16.22    19.05
//         lr=0.08     10.44    13.30    15.57    27.62   <- 5 帧起变差, 故加延迟上限
//         自动定档    10.44    13.30    15.88    19.05
//       出厂默认(kp=1 kd=0.01 kf=0 lr=0)对当前默认: 44.29 -> 13.30(3.33 倍)。
//       每减少一帧延迟就多解放一档学习率 -> 降延迟比继续调 PID 划算。
//   7c) 【剩余空间】完美视在速度(oracle: 把 敌人运动 + 自身视角位移 直接喂给前馈):
//         综合分 13.30 -> 3.09(4.3 倍)
//         喷气 11.86->1.92   翻滚(位移,缓) 17.32->2.66   摆头换向(缓) 12.41->2.17
//         复合身法 20.50->4.00   连跳 23.61->5.47   翻滚(遮挡) 22.75->5.25
//       即"速度估计"是最大的单一剩余杠杆。地板: 自身跳拉(单帧 140px 位移)即使速度
//       完美仍有 10.94 —— 位移阶跃只能靠比例项收, 前馈帮不上。
//       (注: 120Hz 下一帧 = 8.33ms; 实机 total 延迟按此换算。)
//   7d) 【延迟调度的完整清单】(全部由 link_latency_frames() 统一计算)
//       · 前馈学习率 lr : <=4帧 0.08 / 更高 0.05
//       · 比例增益 kp   : <=5.5帧 不设限 / 6帧 0.8 / 7-8帧 0.4 / 更高 0.25
//       · 微分增益 kd   : <=4.5帧 0.05 / 5-7帧 0.03 / 更高 0.02
//       · 门控尺度      : <=4.5帧 3.0 / 更高 6.0
//       实测复核(2026): 7-8 帧档 kp=0.4 是对的(38.7/50.6 对 0.8 的 81.3/137.1);
//       6 帧档 kp=0.8 与 1.2 相差仅 3%(24.94 对 24.22), 保持 0.8 以留安全余量;
//       9 帧以上任何组合都发散(链路极限)。
//   7e) 【补偿增益 K】K=0.5 是长延迟下的上限: 6 帧档 K=0.7 直接发散(17805),
//       K=0.9 更差。K 调高并不能换来更高的 kp —— 补偿质量的瓶颈在速度估计(见 7c)。
//   8) 【否定结果】以下"让速度估计变快"的尝试全部失败(均在修正后的测试台上重测):
//       自适应增益(|创新|触发) 3 组参数、高固定增益、按指令延迟对齐观测器模型、
//       去掉模型外反向压制、反向压制移到输出并加下限、卡尔曼观测器(含"Q=50 等效
//       lr=0.0496"的定量映射)、误差幅值增益调度、带不应期的脉冲式机动响应、
//       观测器每帧变化限速 —— 共 10+ 变体, 综合分 11.8~182(当前 11.99 @3帧),
//       且往往在长延迟档先发散。**同一个结论**: 估计器带宽被【整个回路的延迟裕度】
//       锁死, 不是被信息量锁死(零延迟下快观测器明显更优: 5.49 对 9.60)。
//       要再快只能降端到端延迟, 或把控制结构整体换成显式建模延迟的状态反馈。
//   8a) 【否定结果】加速度前馈(用 ff_derivative/dt 作为加速度, 前馈里加一个超前时间):
//       超前 0.01s 有小幅改善(2 帧 9.78->9.24, 3 帧 11.99->11.56, 约 4%), 但 0.03s
//       在 6 帧档发散(87.5); 对加速度先做低通滤波再用也救不回来(0.0125/0.03 ->
//       6 帧 58.4)。即它同样被回路限制, 且只有 3 倍余量 —— 为 4% 引入一个新的
//       不稳定维度不划算, 未采用。
//   8a2) 常数复核: kDynamicLrWeightFloor=0.25 已接近最优(0.0 -> 25.61@6帧,
//       1.0 -> 26.72); 高 Kf/反向的半径收缩因子统一为 0.25(实测优于 0.5/1.0)。
//       至此算法里所有可调常数都已被实测扫过。
//   8b) 【否定结果】反向判据改成基于误差【趋势】(ff·Δerr<0)或两者取或: 整体明显
//       变差(18.99/21.82/44.65 对 9.78/11.99/24.94 @2/3/6帧), 虽然连跳单项改善
//       (22.45->14.80)。保留现有的"基于误差符号"判据。
//   8c) 【否定结果】把前馈速度观测器改成"快观测器"(自适应增益/高固定增益/对齐指令
//      时序/去掉反向压制/上述组合, 共 10+ 变体)全部不稳定(综合分 38~182 对 17)。
//      但零延迟下快观测器明显更优(5.49 对 9.60) —— 说明限制来自【整个回路的延迟
//      裕度】, 不是观测器自身的数学, 内部怎么改都救不回来。要再快必须动链路延迟
//      或回路增益结构。
//
//   9) 【最终特性表】当前算法逐场景 p95(2 帧档 / 6 帧档), 括号内为出厂默认:
//        静止 0.00/0.00        匀速横移 0.92/6.08 (25.4/30.8)
//        滑铲 2.33/20.02       急停(缓) 2.75/10.50 (42.0/51.0)
//        抖动丢帧 1.00/7.00    跳跃(竖直) 9.33/19.44 (22.3/25.3)
//        连跳(水平) 9.83/15.68 喷气 9.79/35.09 (90.1/112.9)
//        摆头换向(缓) 12.34/24.17 (29.7/34.0)
//        复合身法(缓) 13.69/31.69 (54.5/63.7)
//        自身跳拉 15.06/38.56   翻滚(位移,缓) 16.87/36.57 (58.6/69.4)
//        压力上界(非物理): 摆头换向(阶跃) 29.17/41.67, 翻滚(位移) 23.25/37.00
//      即: 静止/匀速/滑铲/急停/丢帧这一类已压到 1-3px(框宽 20px 之内);
//      硬加速类仍差, 差距来源是速度估计的暂态, 而它被回路延迟锁死(见 7c/8)。
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
//   · 目标框经过真实的跟踪器(4 状态卡尔曼平滑)后才给 PIDF —— 与真实链路一致。
//
// 单位: 全部在 detection_resolution=320 的图像像素/秒。参考(90°FOV, 320px):
//   1° ≈ 3.56px; 6m 处 5m/s 横移 ≈ 168px/s; 滑铲 8-10m/s ≈ 300-350px/s;
//   翻滚/dash ≈ 500-750px/s; 喷气横移可到 900px/s 以上。
// =============================================================================
#include "../ava_exact/pidf_mode1_exact.hpp"
#include "../ava_exact/target_tracker_exact.hpp"
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
    // 第 10/11 个可选参数 = Y 轴的 kf / lr(默认与 X 轴相同)。
    // 分轴是原生设计的一部分(半径也用框宽/框高分开), 实测两轴最优值确实不同。
    c.kf_x = p.kf; c.kf_y = (argc > 10) ? std::atof(argv[10]) : p.kf;
    c.lr_x = p.lr; c.lr_y = (argc > 11) ? std::atof(argv[11]) : p.lr;
    c.movement_limit_x = p.limit; c.movement_limit_y = p.limit;
    PidfMode1State s = construct_pidf_mode1(c, 0.0);
    PidfDelayModelExact delay{};
    // 把环境的真实延迟告诉控制器(现实里由 latency_probe 实测给出):
    // 测量侧 = 检测+发布延迟; 指令侧 = 鼠标+游戏帧延迟。
    const int assumed_lat = (argc > 8) ? std::atoi(argv[8]) : e.det_lat;
    delay.measure_latency_sec = static_cast<double>(assumed_lat) * e.dt;
    // 第 9 个可选参数 = 控制器【假定】的指令延迟(帧), 用于测建模不准时的鲁棒性
    // argv[9] = 控制器【假定】的指令延迟帧数(默认与真实值一致)
    delay.command_latency_frames = (argc > 9)
        ? static_cast<double>(std::atoi(argv[9]))
        : static_cast<double>(e.mouse_lat);

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
    TargetTrackerExact tracker;

    const int frames = static_cast<int>(sc.duration / e.dt);
    const double jitter = (argc > 13) ? std::atof(argv[13]) : 0.0;
    double clock = 0.0;   // 抖动时钟: dt 在 (1±jitter) 之间跳, 模拟真实调度
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
        const double noise_px = (argc > 12) ? std::atof(argv[12]) : 0.35;
        const double nz = noise_px * noise[f % 10];
        const double measured = std::round((measured_true + nz) * 2.0) * 0.5;

        // 指令生效延迟: 队列长度 = mouse_lat+1, applied = dx_{f-mouse_lat}。
        // ⚠️ 之前这里是错位的: 实际给出 2 帧延迟却对控制器声明 1 帧
        // (delay.command_latency_frames = mouse_lat), 于是观测器的模型比真实
        // 早一帧, 高学习率会把这个系统性创新误差放大到发散 —— 这会让人误以为
        // "回路的延迟裕度锁死了估计器", 其实是模拟器的建模错误。现在两者一致。
        double applied = 0.0;

        // ── 真实链路: 检测 -> 跟踪器(4 状态卡尔曼, 平滑框中心) -> 锚点 -> PIDF ──
        // 关键: PIDF 拿到的【不是】带噪声的原始检测, 而是跟踪器卡尔曼平滑后的框。
        // 之前模拟器把噪声直接喂给 PIDF, 相当于把噪声估高了, 会让调出来的 lr 偏小。
        {
            const double center = 160.0 + measured_true + nz;
            SelectedTarget104Abi m{};
            m.class_id = 0; m.confidence = 0.9f;
            const double hw = (is_x ? p.box_w : p.box_h) * 0.5;
            const double hh = (is_x ? p.box_h : p.box_w) * 0.5;
            m.left = static_cast<float>(center - hw);
            m.right = static_cast<float>(center + hw);
            m.top = static_cast<float>(160.0 - hh);
            m.bottom = static_cast<float>(160.0 + hh);
            m.effective_width = static_cast<float>(hw * 2.0);
            m.effective_height = static_cast<float>(hh * 2.0);
            if (visible) tracker.update(&m);
            else tracker.update(nullptr);
            const SelectedTarget104Abi* out = tracker.build_output(nullptr);
            if (out != nullptr)
            {
                coast = 0;
                predicted = static_cast<double>(out->effective_center_x) - 160.0;
                visible = true;      // 跟踪器外推中仍算有效(与原代码 engine 的行为一致)
            }
            else
            {
                ++coast;
                predicted = last_meas + last_ev * static_cast<double>(coast) * e.dt;
            }
            last_meas = measured;
            last_ev = enemy_v;
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
            // 重新锁定: 真实链路是 install_new_aim_target_record_exact ->
            // reset_pidf_runtime -> reset_pidf_mode1(保留结构体, 只清状态标记),
            // 不是重建结构体 —— 这里必须照做, 否则测不出"重锁代价"。
            reset_pidf_mode1(s, static_cast<double>(f) * e.dt);
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
        static const double jit[] = {1.0, 1.15, 0.9, 1.08, 0.95, 1.2, 0.85, 1.02, 0.92, 1.1};
        const double dtf = e.dt * (1.0 + jitter * (jit[f % 10] - 1.0));
        clock += dtf;
        const auto out = update_pidf_mode1(s, delay, in, clock);
        const double dx = is_x ? static_cast<double>(out.dx) : static_cast<double>(out.dy);
        pending.push_back(dx);
        while (static_cast<int>(pending.size()) > e.mouse_lat + 1)
            pending.pop_front();
        applied = pending.front();      // = dx 于 mouse_lat 帧之前

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

    // 翻滚(位移): 0.22s 内 750px/s 冲出去, 不丢检测(冲刺本身)
    add("翻滚(位移)", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        *ev = (t >= 0.8 && t < 1.02) ? 750.0 : 0.0; });

    // 翻滚(位移,缓): 真实冲刺有加速过程 —— 0.12s 加速到 750, 保持 0.1s, 0.12s 减速
    add("翻滚(位移,缓)", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t >= 0.8 && t < 0.92) *ev = 750.0 * ((t - 0.8) / 0.12);
        else if (t >= 0.92 && t < 1.02) *ev = 750.0;
        else if (t >= 1.02 && t < 1.14) *ev = 750.0 * (1.0 - (t - 1.02) / 0.12);
        else *ev = 0.0; });

    // 翻滚(进掩体): 冲刺后被遮挡 0.18s, 再出现 —— 考验丢目标与重锁
    add("翻滚(遮挡)", "x", 2.0, [](double t, double* ev, double* sv, bool* vis) {
        *sv = 0;
        *ev = (t >= 0.8 && t < 1.02) ? 750.0 : 0.0;
        if (t >= 0.95 && t < 1.13) *vis = false; });

    // 跳跃: 真实施加的是重力 —— 竖直速度线性变化(抛物线), 不是瞬间翻符号。
    // 上升 0.25s: +400 -> 0, 再 0.25s: 0 -> -400 (g = 1600px/s^2)。
    add("跳跃(竖直)", "y", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t >= 0.6 && t < 1.1) *ev = 400.0 - 1600.0 * (t - 0.6);
        else *ev = 0.0; });

    // 连跳: 1.2Hz 连续弹跳, 每跳竖直走抛物线
    add("连跳", "y", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        const double ph = std::fmod(t, 0.415);         // 上升 0.2075s + 下降 0.2075s
        *ev = 380.0 - 1831.0 * ph; });

    // 连跳(水平): 每次落地水平反向 ±180, 但带 0.1s 减速过程(真实脚步摩擦)
    add("连跳(水平)", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        const double ph = std::fmod(t, 0.83);
        const bool forward = ph < 0.42;
        const double dir = forward ? 1.0 : -1.0;
        const double since = forward ? ph : (ph - 0.42);
        const double ramp = std::min(1.0, since / 0.1);
        *ev = 180.0 * dir * ramp; });

    // 摆头换向(真实: 带减速过程, 正弦速度) —— 玩家不可能瞬间反向
    add("摆头换向(缓)", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        *ev = 380.0 * std::sin(2.0 * 3.14159265358979 * 1.5 * t); });

    // 急停(真实: 0.15s 减速)
    add("急停(缓)", "x", 2.5, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 1.0) *ev = 420.0;
        else if (t < 1.15) *ev = 420.0 * (1.0 - (t - 1.0) / 0.15);
        else *ev = 0.0; });

    // 急停: 420px/s 跑 1s 后瞬间静止
    add("急停", "x", 2.5, [](double t, double* ev, double* sv, bool*) {
        *sv = 0; *ev = (t < 1.0) ? 420.0 : 0.0; });

    // 喷气: 0.14s 内 0->950, 保持 0.45s, 再用 0.15s 减速归零
    // (阶跃版保留在"喷气(急停)"里作为压力上界)
    add("喷气", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 0.14) *ev = 950.0 * (t / 0.14);
        else if (t < 0.59) *ev = 950.0;
        else if (t < 0.74) *ev = 950.0 * (1.0 - (t - 0.59) / 0.15);
        else *ev = 0.0; });

    // 喷气(急停): 950px/s 瞬间归零 —— 物理上界, 用于压测
    add("喷气(急停)", "x", 2.0, [](double t, double* ev, double* sv, bool*) {
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

    // 复合身法(阶跃上界): 横移 + 换向 + 喷气 + 急停, 全部瞬时切换
    add("复合身法", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 0.7) *ev = 300.0;
        else if (t < 1.0) *ev = -300.0;
        else if (t < 1.4) *ev = 600.0;
        else *ev = 0.0; });

    // 复合身法(缓): 同样的动作序列, 但每次切换都有真实加减速过程
    // (横移 0.1s 反向 / 喷气 0.12s 加速 / 0.15s 急停) —— 这是最贴近实战的一项
    add("复合身法(缓)", "x", 3.0, [](double t, double* ev, double* sv, bool*) {
        *sv = 0;
        if (t < 0.6) *ev = 300.0;
        else if (t < 0.75) *ev = 300.0 - 600.0 * ((t - 0.6) / 0.15);   // 0.15s 反向
        else if (t < 1.0) *ev = -300.0;
        else if (t < 1.12) *ev = -300.0 + 900.0 * ((t - 1.0) / 0.12);  // 0.12s 加速到 600
        else if (t < 1.4) *ev = 600.0;
        else if (t < 1.55) *ev = 600.0 * (1.0 - (t - 1.4) / 0.15);     // 0.15s 急停
        else *ev = 0.0; });
    return v;
}

int main(int argc, char** argv)
{
    // 参数位: kp kd kf lr 限幅 检测延迟 鼠标延迟 [假定测量延迟 [假定指令延迟 [kf_y [lr_y [噪声px [dt抖动 [检测帧率]]]]]]]
    Params p;
    Env e;
    if (argc > 1) p.kp = std::atof(argv[1]);
    if (argc > 2) p.kd = std::atof(argv[2]);
    if (argc > 3) p.kf = std::atof(argv[3]);
    if (argc > 4) p.lr = std::atof(argv[4]);
    if (argc > 5) p.limit = std::atoi(argv[5]);
    if (argc > 6) e.det_lat = std::atoi(argv[6]);
    if (argc > 7) e.mouse_lat = std::atoi(argv[7]);
    // argv[14] = 检测帧率(Hz), 默认 120。用于验证"秒级常数(tau)"与"帧级定档"在不同
    // 帧率下的配合 —— 例如 tau=0.0125s 在 60Hz 只有 0.75 帧、在 240Hz 是 3 帧。
    if (argc > 14) { const double fps = std::atof(argv[14]); if (fps > 1.0) e.dt = 1.0 / fps; }
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
