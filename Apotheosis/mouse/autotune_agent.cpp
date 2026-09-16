// =============================================================================
// 调参 agent —— 主循环实现 + 提示词 + JSON 解析
// =============================================================================

#include "mouse/autotune_agent.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace boss::autotune
{

namespace
{

double now_s()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

// 在 JSON 文本里找 "key" 后面的数字。宽容: 找不到返回 false。
bool json_number(const std::string& j, const char* key, double& out)
{
    const std::string k = std::string("\"") + key + "\"";
    std::size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    while (p < j.size() && (j[p] == ':' || std::isspace(static_cast<unsigned char>(j[p]))))
        ++p;
    if (p >= j.size()) return false;
    char* endp = nullptr;
    const double v = std::strtod(j.c_str() + p, &endp);
    if (endp == j.c_str() + p) return false;
    out = v;
    return true;
}

bool json_bool(const std::string& j, const char* key, bool& out)
{
    const std::string k = std::string("\"") + key + "\"";
    std::size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    while (p < j.size() && (j[p] == ':' || std::isspace(static_cast<unsigned char>(j[p]))))
        ++p;
    if (j.compare(p, 4, "true") == 0) { out = true; return true; }
    if (j.compare(p, 5, "false") == 0) { out = false; return true; }
    // 也接受 0/1
    double v = 0.0;
    if (json_number(j, key, v)) { out = (v != 0.0); return true; }
    return false;
}

} // namespace

// ── 提示词 ──────────────────────────────────────────────────────────────────
// ★ 设计要点:
//   · 明确告诉模型【这是双机架构、k̂ 不可测】, 否则它会建议"先标定像素/计数"
//   · 明确给出稳定性天花板 Kp*s_max ≲ 52, 否则它会一路加 Kp
//   · 要求【只回答 JSON】, 便于解析
//   · 要求【一次只改 1~2 个参数】, 否则无法归因
std::string build_system_prompt()
{
    return
        u8"你是 FPS 自瞄控制器的 PID 调参助手。你在为一个【双机架构】的系统调参:\n"
        u8"  游戏机 --HDMI--> 采集卡 --USB--> 采集机(本程序 + 鼠标盒子)\n"
        u8"控制器是纯反馈 PID, 误差单位是【检测图像素】, 输出是【整数鼠标计数】。\n"
        u8"\n"
        u8"【关键约束, 必须遵守】\n"
        u8"1. 系统里【不存在】也不允许引入『每计数多少像素』这个量(k̂)。它在游戏机上,\n"
        u8"   双机架构下无法测量。不要建议任何需要它的方案(如标定灵敏度)。\n"
        u8"2. 稳定性天花板: Kp 与尺度增益 s_max 相乘是等效增益, 乘积超过约 52 就会自激。\n"
        u8"   提高 s_max 必须先降低 Kp。\n"
        u8"3. 在途补偿 beta: 补不足只是回到原来的延迟(安全), 补过头会正反馈发散。\n"
        u8"   实测最优在 1.6 附近; >2.4 通常开始变差, >3 必发散。beta 上调要非常保守。\n"
        u8"4. Kd 对延迟敏感。链路死区实测 46ms, 采集+推理延迟越大, Kd 越该调大。\n"
        u8"5. 误差中位很小【不代表】没有抖动: 自持抖动时中位可能只有 0.2px 却每拍反号。\n"
        u8"   必须看『翻转率』—— 高于 80% 就是自持抖动, 要【降低】Kp 而不是继续调。\n"
        u8"6. Ki 太小会导致匀速跟踪有永久滞后(残差 = v/(Kp·k̂), 不收敛)。\n"
        u8"   实测 Ki=0 时 300px/s 目标的残差可达 40px。Ki 不要低于 0.3。\n"
        u8"\n"
        u8"【调参策略】\n"
        u8"· 一次只改 1~2 个参数, 否则无法归因。\n"
        u8"· 过冲大 -> 增大 Kd 或降低 Kp; 抖动/翻转率高 -> 降低 Kp; 跟踪滞后大 -> 增大 Ki。\n"
        u8"· 已经收敛得很好时, 给出与原值几乎相同的参数(不要为了改而改)。\n"
        u8"\n"
        u8"【输出格式】只输出一个 JSON 对象, 不要任何解释、不要 markdown 围栏:\n"
        u8"{\"kp\":数值,\"ki\":数值,\"kd\":数值,\"beta\":数值,"
        u8"\"psat_px\":数值,\"scale_on\":true/false,\"scale_max\":数值,\"scale_min\":数值,"
        u8"\"predict_x\":数值,\"predict_y\":数值,"
        u8"\"predict_max_px\":整数,\"predict_vel_floor\":整数,"
        u8"\"reason\":\"一句话中文说明\"}\n"
        u8"未提及的参数保持原值。数值必须是有限数字。";
}

const char* mode_name(TuneMode m)
{
    switch (m)
    {
    case TuneMode::Static: return u8"静态目标";
    case TuneMode::Moving: return u8"动态目标";
    case TuneMode::Feel:   return u8"体感";
    }
    return u8"未知";
}

std::string build_user_prompt(const Knobs& current, const Metrics& m,
                              const TrackSegment& seg, TuneMode mode,
                              const std::string& feel_text,
                              int round_index, const std::string& last_note,
                              StepStyle style)
{
    std::string s;
    s += u8"第 " + std::to_string(round_index) + u8" 轮调参, 模式: ";
    s += mode_name(mode);
    s += u8"\n\n";

    s += u8"当前参数:\n  " + current.describe() + u8"\n\n";
    s += u8"实测指标:\n  " + m.summary() + u8"\n\n";

    // ── 这一段数据的来历 ────────────────────────────────────────────────────
    // ★ 为什么要告诉模型"这是用户主动采的一段"而不是"最近 N 秒":
    //   模型需要知道这段时间的用户意图是【在靶场里专门测这一段】, 这样它才会
    //   把误差解读成"跟随能力", 而不是"用户没在瞄"造成的随机误差。
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        u8"数据来历: 用户点「开始」后专门打了一段, 共 %.1f 秒 / %d 条样本"
        u8"(覆盖率 %.0f%%)。\n",
        seg.duration(), static_cast<int>(seg.samples.size()), seg.coverage * 100.0);
    s += buf;
    std::snprintf(buf, sizeof(buf),
        u8"数据质量: 期间野值跳变 %d 次, 框丢失 %d 次。\n",
        seg.jumps, seg.gaps);
    s += buf;
    std::snprintf(buf, sizeof(buf),
        u8"目标尺寸: 框高中位 %.1f px (p10 %.1f / p90 %.1f, 相对离散度 %.2f)。\n",
        seg.bbox_h_median, seg.bbox_h_p10, seg.bbox_h_p90, seg.bbox_h_spread());
    s += buf;
    // ★ 说清楚"玩家在甩枪是正常的" —— 否则模型看到误差里的大跳变会误以为
    //   数据坏了, 或者误以为玩家在乱动。这个背景能显著改变它对峰值的解读。
    s += u8"说明: 这段时间里用户的操作包含了正常的瞄准动作(甩枪/微调/跟随),\n"
         u8"      误差峰值里包含这些动作本身带来的部分, 那是正常的, 不要当成异常。\n";

    // ── 按模式给解读提示 ────────────────────────────────────────────────────
    // ★ 三个模式的算法目标不同, 必须明确告诉模型该看什么, 否则它会用同一套
    //   标准去评静态和动态数据 —— 而动态数据天生就有跟随滞后, 拿静态的标准
    //   去判会得出"Ki 太小"的错误结论(其实那部分是物理上消不掉的)。
    switch (mode)
    {
    case TuneMode::Static:
        s += u8"\n【本模式怎么看数据】靶子基本不动, 目标是【快速到位 + 停住不抖】。\n"
             u8"· 主要看: 首次到位时间、过冲峰值、稳态残差中位、翻转率。\n"
             u8"· 稳态残差应该接近 0。若残差中位明显 >0, 说明有静差或自持抖动。\n"
             u8"· 翻转率 >80% 且样本足够多 = 自持抖动, 要【降】Kp, 不要看残差小就以为没问题。\n"
             u8"· 误差的峰值大不代表坏 —— 那是玩家甩枪造成的, 看【稳态段】的残差。\n";
        break;
    case TuneMode::Moving:
        s += u8"\n【本模式怎么看数据】靶子在移动, 目标是【贴得住 + 机动后立刻再咬上】。\n"
             u8"· 主要看: 稳态跟随残差(中位/p90)、机动后的重新咬合、有没有振荡。\n"
             u8"· ★ 匀速跟随时存在【物理上消不掉的】滞后。不要为了把中位压到 0 而\n"
             u8"   无限加 Kp —— 那是做不到的, 只会换来振荡。\n"
             u8"· 缩小滞后的正确手段是【加 Ki】(消除匀速静差)和【在途补偿 beta】。\n"
             u8"· 尺寸离散度较大 = 靶子有远近变化, 这正是尺度调度该起作用的场合。\n"
             u8"· 若残差 p90 远大于中位, 说明大部分时间贴得住、偶尔被甩开, 此时\n"
             u8"   优先看机动峰值而不是中位。\n";
        break;
    case TuneMode::Feel:
        s += u8"\n【本模式怎么看数据】用户【亲口描述了他的体感】。\n"
             u8"· ★ 用户的话是【主证据】, 日志数字是【佐证】。\n"
             u8"· 两者一致时: 按用户的描述调, 并用数字确认方向。\n"
             u8"· 两者矛盾时(比如用户说抖但翻转率很低): 相信用户 —— 可能出问题的是\n"
             u8"   检测层面, 或者用户说的抖其实指别的现象。此时优先采纳最小幅度、\n"
             u8"   最保守的改动, 并在 reason 里说明你的不确定。\n"
             u8"· 不要无视用户的描述去纯按数字调。他说哪里不舒服, 就往那个方向改。\n";
        break;
    }

    if (!feel_text.empty())
    {
        s += u8"\n【用户原话】\n  ";
        s += feel_text;
        s += u8"\n";
    }

    if (!last_note.empty())
        s += u8"\n上一轮说明: " + last_note + u8"\n";

    // ── 本轮允许的改动幅度 (2026-09-15) ────────────────────────────────────
    //
    // ★ 这一段把【硬夹取的真实额度】如实告诉模型, 并明确鼓励它把额度用满。
    //
    //   改动前这里写的是"幅度要克制 —— 宁可小步走稳, 不要大改", 结果是三轮
    //   下来 Kp 只动了 10%、Ki 一动没动, 用户体感毫无变化(实测反馈)。让模型
    //   "保守"并不能换来安全 —— 安全由下面的硬夹取保证, 提示词该做的是让它在
    //   允许范围内【该大胆就大胆】。
    {
        const double rel_pct = step_style_rel(style) * 100.0;
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
            u8"\n【本轮改动幅度: %s】\n"
            u8"  你这一轮最多可以把每个参数改动 ±%.0f%%(硬限制, 超了会被自动夹回来)。\n",
            step_style_name(style), rel_pct);
        s += hdr;

        switch (style)
        {
        case StepStyle::Conservative:
            s += u8"  用户选了【保守】: 数据可能不够可信, 或他只想微调。\n"
                 u8"  请只改你在数据里看得最清楚的那 1 个参数, 幅度尽量小。\n"
                 u8"  如果指标看着已经不错, 就把参数原样返回(这完全正常)。\n";
            break;
        case StepStyle::Aggressive:
        {
            // 这一档的关键是"明确鼓励用满额度" —— 实测表明模型默认极其保守,
            // 不给这句话它只会挪一点点(这正是"已应用但体感没变化"的成因)。
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                u8"  用户选了【激进】: 他明确要求看到明显变化, 不要畏手畏脚。\n"
                u8"  ★ 如果指标显示当前参数明显偏离合理区间(Ki 远大于 1、Kp 明显偏低、\n"
                u8"    翻转率很高、稳态残差大), 请【一次调到位】, 把这 ±%.0f%% 的额度\n"
                u8"    用满, 不要只挪一点点。\n"
                u8"  ★ 如果连续几轮都在同方向微调却不见效, 更该加大步幅。\n"
                u8"  ★ 只有在数据本身不可信时才保持小步, 并在 reason 里说明。\n",
                rel_pct);
            s += buf;
            break;
        }
        case StepStyle::Steady:
        default:
            s += u8"  用户选了【平稳】: 默认档。\n"
                 u8"  在指标明确指向某个方向时, 可以把这个幅度用满; 不确定时就小一点。\n";
            break;
        }
    }

    s += u8"\n请给出这一轮要用的参数。输出 JSON。";
    return s;
}

// ── 解析模型回答 ────────────────────────────────────────────────────────────
bool parse_knobs_json(const std::string& json, const Knobs& current, Knobs& out,
                      std::string& err)
{
    if (json.empty()) { err = u8"模型没有返回 JSON"; return false; }
    out = current;   // 未提及的字段保持原值

    double v = 0.0;
    bool b = false;
    if (json_number(json, "kp", v)) out.kp = v;
    if (json_number(json, "ki", v)) out.ki = v;
    if (json_number(json, "kd", v)) out.kd = v;
    if (json_number(json, "beta", v)) out.beta = v;
    if (json_number(json, "psat_px", v)) out.psat_px = v;
    if (json_bool(json, "scale_on", b)) out.scale_on = b;
    if (json_number(json, "scale_max", v)) out.scale_max = v;
    if (json_number(json, "scale_min", v)) out.scale_min = v;
    // ★ 这里【没有】基准框高: 它不是模型能定的参数, 而是用户在界面上按按钮
    //   设定的观测值(见 Runtime::set_baseline_from_recent)。即使模型
    //   在 JSON 里塞了 base_h / scale_base_h, 也会被忽略。
    if (json_number(json, "predict_x", v)) out.predict_x = v;
    if (json_number(json, "predict_y", v)) out.predict_y = v;
    if (json_number(json, "predict_max_px", v)) out.predict_max_px = static_cast<int>(v);
    if (json_number(json, "predict_vel_floor", v)) out.predict_vel_floor = static_cast<int>(v);

    // 至少得认出一个字段, 否则视为解析失败(全默认值会静默把参数改光)
    const bool any = json.find("\"kp\"") != std::string::npos
                  || json.find("\"ki\"") != std::string::npos
                  || json.find("\"kd\"") != std::string::npos
                  || json.find("\"beta\"") != std::string::npos;
    if (!any) { err = u8"JSON 里没有认出任何已知参数名"; return false; }
    return true;
}

// ── AutoTuner ───────────────────────────────────────────────────────────────
AutoTuner::~AutoTuner() = default;

// ── 采样会话 ────────────────────────────────────────────────────────────────
//
// ★★ 2026-09-14 第二次改版: 边界由用户的两个按钮界定 ★★
//
//   点「开始」→ 记下当前日志的绝对序号作起点
//   你去打
//   点「结束」→ 记下终点, 取 [起点, 终点) 之间的全部日志
//
// ★ 为什么放弃"按住瞄准热键"切段:
//   ① 配套的"中途换目标"判据读的是 `target_switched`, 而它在
//      `boss_aim.cpp` 里的定义是 `motion_suppressed && anchor_jump >= 25px`
//      —— 那是【玩家在甩枪】, 不是"换了目标"。正常跟枪每次甩都触发,
//      于是几乎每一段都被判成脏数据。实测表现: 一直提示"中途换了目标"。
//   ② 更根本: 用【用户的意图】当边界比用【程序的信号】当边界可靠。
//      "我按了开始, 现在开始算"不需要任何解释, 也不会被引擎状态误触发。

void AutoTuner::begin_session()
{
    // ★ 不清历史: 用户可能想看前面几轮。只重置本次会话的区间标记。
    // ★ 起点 = 当前已推入的绝对序号, 也就是"从现在起的下一条样本"。
    seg_begin_idx_ = buffer_.pushed();
    seg_end_idx_ = -1;
    session_open_.store(true);
}

std::size_t AutoTuner::session_sample_count() const
{
    if (!session_open_.load() || seg_begin_idx_ < 0) return 0;
    const long long pushed = buffer_.pushed();
    const long long oldest = buffer_.oldest();
    const long long from = seg_begin_idx_ > oldest ? seg_begin_idx_ : oldest;
    return pushed > from ? static_cast<std::size_t>(pushed - from) : 0;
}

TrackSegment AutoTuner::close_segment() const
{
    TrackSegment seg;
    long long b = seg_begin_idx_;
    long long e = seg_end_idx_;

    if (b < 0)
    {
        // 没打过起点标记: 退而求其次, 拿缓冲里最近的全部样本。
        // ★ 这条路径只在内部状态异常时走到(正常流程一定先 begin_session)。
        const long long oldest = buffer_.oldest();
        const long long pushed = buffer_.pushed();
        if (pushed <= oldest) return seg;
        b = oldest;
        e = pushed;
    }
    if (e < 0) e = buffer_.pushed();

    // ★ range() 会把已被淘汰的部分裁掉(而不是报错) —— 用户采样太久导致
    //   最早的样本被挤掉时, 我们尽量利用剩下还能拿到的数据, 够不够
    //   由 judge_segment 的样本数判据去决定。
    seg.samples = buffer_.range(b, e);
    if (seg.samples.empty()) return seg;

    seg.t_begin = seg.samples.front().t;
    seg.t_end   = seg.samples.back().t;

    // ── 覆盖率 ──────────────────────────────────────────────────────────────
    //
    // ★★ 这里之前算错过两次, 修正记录见 autotune_agent.h 的 TrackSegment ★★
    //
    //   旧公式 `n ÷ (跨度 ÷ 平均dt)` 有两个毛病:
    //   ① 把"程序没在跑"的死时间算进分母 —— 实测用户的日志里有 9.5s~29.6s
    //      的空洞, 于是覆盖率被判成 32%, **每一段都过不了门限**。
    //      用户侧的现象就是"为什么覆盖率一直不够"。
    //   ② 平均 dt(9.26ms) 比典型步进(8.3ms) 大, 慢帧把均值拉高, 所以连
    //      完全连续的一段也只会算出 111%, 永远不可能是 100%。
    //
    //   新定义: 正常间隔的样本数 ÷ 总样本数。
    //   dt_ms 是引擎自己报的每拍间隔, 它本来就记录了"这一拍之前空了多久",
    //   直接看它就行 —— 不需要用"跨度÷均值"去推测本该有多少条。
    //   "正常"的判据用中位数的 4 倍: 对慢帧不敏感(中位数不会被少数大值拉走),
    //   又能挡住真正的丢帧和空洞。
    {
        std::vector<double> dts;
        dts.reserve(seg.samples.size());
        for (const auto& s : seg.samples)
            if (s.dt_ms > 0.0) dts.push_back(s.dt_ms);

        if (!dts.empty())
        {
            std::sort(dts.begin(), dts.end());
            const double median_dt = dts[dts.size() / 2];
            const double limit = median_dt * 4.0;
            std::size_t ok = 0;
            for (double d : dts)
                if (d <= limit) ++ok;
            seg.coverage = static_cast<double>(ok) / static_cast<double>(dts.size());
        }
    }

    // ★★ 只统计【野值跳变】, 不统计 `target_switched`。
    //   理由见头文件: `target_switched` 的真实含义是"瞄点跳≥25px"(= 玩家甩枪),
    //   拿它当"数据不干净"的判据会误杀几乎每一个真实采样。
    //   野值跳变(jump_px > 40)才是真正会污染误差统计的东西 ——
    //   那是检测框瞬间跳到了完全无关的位置。
    for (const auto& s : seg.samples)
        if (s.jump_px > 40.0) ++seg.jumps;

    // 框中间丢失的次数: 相邻样本间隔明显超过正常帧间隔。
    // ★ 门限与覆盖率用同一个"正常"定义(中位数的 4 倍), 两者才不会互相打架。
    if (seg.samples.size() >= 2)
    {
        std::vector<double> dts;
        dts.reserve(seg.samples.size());
        for (const auto& s : seg.samples)
            if (s.dt_ms > 0.0) dts.push_back(s.dt_ms);
        if (!dts.empty())
        {
            std::sort(dts.begin(), dts.end());
            const double limit = dts[dts.size() / 2] * 4.0;
            for (const auto& s : seg.samples)
                if (s.dt_ms > limit) ++seg.gaps;
        }
    }

    // 框高分位数 —— 用来判"靶子是不是在远近变化"
    std::vector<double> h;
    h.reserve(seg.samples.size());
    for (const auto& s : seg.samples)
        if (s.bbox_h > 0.0) h.push_back(s.bbox_h);
    if (!h.empty())
    {
        std::sort(h.begin(), h.end());
        const std::size_t n = h.size();
        seg.bbox_h_median = h[n / 2];
        seg.bbox_h_p10 = h[n / 10];
        seg.bbox_h_p90 = h[(n * 9) / 10];
    }
    return seg;
}

SegmentVerdict judge_segment(const TrackSegment& seg, TuneMode mode)
{
    SegmentVerdict v;

    // ★★ 这一组判据的原则: 只挡【会让指标失真】的事, 不挡"用户的动作"。
    //   凡是"我觉得这样不好"但实际不影响统计量的, 一律不拦 —— 拦了只会
    //   让用户反复重采。这一条是被实测教出来的: 上一版的"中途换目标"
    //   判据误读了引擎信号, 把甩枪当成了换靶, 几乎每一段都被拒。

    // ① 得有一段像样的数据
    if (seg.samples.size() < 60)
    {
        v.reason = u8"这一段数据太少(只有 " + std::to_string(seg.samples.size())
                 + u8" 条)。请在点「开始」之后多打一会儿 —— 至少 1~2 秒"
                   u8"(建议 3~5 秒)再点「结束」。";
        return v;
    }
    if (seg.duration() < 0.8)
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            u8"这一段只有 %.1f 秒。调参至少需要 1 秒的连续数据, 建议 3~5 秒。",
            seg.duration());
        v.reason = buf;
        return v;
    }

    // ② 必须真的锁到过目标 —— 没有框高就说明这段压根没有检测
    if (seg.bbox_h_median <= 0.0)
    {
        v.reason = u8"这一段没有有效的目标尺寸 —— 开始采样之后要先锁住目标再打。\n"
                   u8"（如果识别没开或者没框住东西, 日志里就没有可用的数据。）";
        return v;
    }

    // ③ 数据得基本连续。
    //    ★ 门限 0.50: "正常间隔的样本数占一半以上"。丢一会儿目标、
    //      中途松手都不影响 —— 只有大段空白才会掉到这以下。
    if (seg.coverage < 0.50)
    {
        char buf[288];
        std::snprintf(buf, sizeof(buf),
            u8"这一段数据不连续(连续样本只占 %.0f%%, 中间有 %d 处断档)。\n\n"
            u8"常见原因:\n"
            u8"· 识别没在跑 —— 检查一下识别是否已启动、是否一直锁着目标;\n"
            u8"· 采样期间切换了程序/页面, 瞄准循环停了;\n"
            u8"· 目标长时间不在视野里。\n\n"
            u8"请直接重采一次: 点「开始」之后【一直保持识别在跑】打 3~5 秒。",
            seg.coverage * 100.0, seg.gaps);
        v.reason = buf;
        return v;
    }

    // ④ 野值跳变 —— 这个【真的】会污染误差统计
    //    ★ 门限从 3 放到 12, 并且按比例判: 采样越长越可能碰上几次。
    const int jump_limit = 12 + static_cast<int>(seg.samples.size() / 200);
    if (seg.jumps > jump_limit)
    {
        v.reason = u8"这一段有 " + std::to_string(seg.jumps)
                 + u8" 次位置突变(>40px), 检测在跳, 这种数据调不出可信结果。\n"
                   u8"请检查识别是否稳定, 再重采一次。";
        return v;
    }

    // ⑤ 按模式检查"这段数据配不配这个模式"
    //    ★ 不做硬拒绝之外的额外惩罚; 只在明显错配时提示。
    if (mode == TuneMode::Static && seg.bbox_h_spread() > 0.60)
    {
        char buf[288];
        std::snprintf(buf, sizeof(buf),
            u8"你选的是【静态目标】, 但这段数据里目标尺寸变化很大(离散度 %.2f),\n"
            u8"说明目标(或你)在明显前后移动。\n\n"
            u8"如果靶子在移动, 请把模式改成【动态目标调参】再重采 —— 那个模式\n"
            u8"本来就按「跟随移动目标」来判, 这份数据在它那里是正常的。",
            seg.bbox_h_spread());
        v.reason = buf;
        return v;
    }

    v.ok = true;
    return v;
}

void AutoTuner::end_session()
{
    const bool was_open = session_open_.exchange(false);
    if (!was_open) return;

    // 终点 = 当前的绝对序号(即"最后一条已收到的样本"之后)
    seg_end_idx_ = buffer_.pushed();

    const TrackSegment seg = close_segment();
    {
        std::lock_guard<std::mutex> g(seg_mutex_);
        last_seg_ = seg;
        have_last_seg_ = true;
    }

    int index = 0;
    {
        std::lock_guard<std::mutex> g(hist_mutex_);
        index = static_cast<int>(history_.size()) + 1;
    }

    const Round r = run_one_round(index, seg);
    {
        std::lock_guard<std::mutex> g(hist_mutex_);
        history_.push_back(r);
        if (history_.size() > 200) history_.erase(history_.begin());
    }
    if (on_round_) on_round_(r);
}

bool AutoTuner::last_segment_info(TrackSegment& out) const
{
    std::lock_guard<std::mutex> g(seg_mutex_);
    if (!have_last_seg_) return false;
    // ★ 只回传概要, 不回传样本(样本可能有几千条, UI 不需要)
    out = last_seg_;
    out.samples.clear();
    out.samples.shrink_to_fit();
    return true;
}

std::vector<Round> AutoTuner::history() const
{
    std::lock_guard<std::mutex> g(hist_mutex_);
    return history_;
}

void AutoTuner::clear_history()
{
    std::lock_guard<std::mutex> g(hist_mutex_);
    history_.clear();
}

Round AutoTuner::end_session_for_test(const TrackSegment& seg)
{
    return run_one_round(1, seg);
}

Round AutoTuner::run_one_round(int index, const TrackSegment& seg)
{
    Round r;
    r.index = index;
    r.wall_s = now_s();
    r.mode = mode_;
    r.style = step_style_;
    r.segment = seg;
    r.feel_text = feel_text_;

    if (!get_)
    {
        r.error = u8"未接入参数读写(内部错误)";
        return r;
    }

    // ① 数据够不够干净 —— ★ 硬判据, 独立于 LLM
    const SegmentVerdict verdict = judge_segment(seg, mode_);
    if (!verdict.ok)
    {
        r.error = verdict.reason;
        return r;
    }

    r.metrics = compute(seg.samples);
    if (r.metrics.n < 32)
    {
        r.error = u8"样本太少(" + std::to_string(r.metrics.n)
                + u8" 条), 先在靶场持续对同一目标几秒";
        return r;
    }

    // ★ 体感模式必须真的有用户描述 —— 否则这个模式就没有意义了
    if (mode_ == TuneMode::Feel && feel_text_.empty())
    {
        r.error = u8"体感模式需要你先在下面写一句感受(比如「跟枪拖后腿」"
                 u8"「到了靶子上会抽」), 再点结束。";
        return r;
    }

    const Knobs current = get_();
    r.requested = current;
    r.applied = current;

    // ② 回滚判定 —— ★ 不看模型, 直接决定
    {
        const RollbackDecision d =
            should_rollback(r.metrics, prev_metrics_, have_prev_metrics_);
        if (d.rollback && have_last_good_)
        {
            std::string werr;
            // ★ 回滚也要验: 回滚失败必须让用户知道, 否则他以为已经退回安全值了。
            if (!set_(last_good_, werr))
            {
                r.error = u8"检测到需要回滚, 但回滚没能生效: " + werr;
                return r;
            }
            r.applied = last_good_;
            r.rolled_back = true;
            r.rollback_reason = d.reason;
            r.clamp_notes = u8"已回滚, 本轮不请求模型";
            prev_metrics_ = r.metrics;
            have_prev_metrics_ = true;
            return r;
        }
    }

    // ③ 问模型
    const std::string sys = build_system_prompt();
    const std::string usr = build_user_prompt(current, r.metrics, seg, mode_,
                                              feel_text_, index, "", step_style_);
    const LlmResult lr = reply_fn_ ? reply_fn_(llm_, sys, usr)
                                   : chat_completion(llm_, sys, usr);
    if (!lr.ok)
    {
        r.error = lr.error;
        return r;
    }
    r.model_reply = lr.text.size() > 400 ? lr.text.substr(0, 400) : lr.text;

    // ④ 解析
    const std::string js = extract_json_object(lr.text);
    Knobs want = current;
    std::string perr;
    if (!parse_knobs_json(js, current, want, perr))
    {
        r.error = perr;
        return r;
    }
    r.requested = want;

    // ⑤ 夹取 —— ★ 无论模型说什么, 落在控制器上的必须在安全域内
    //    ★ 幅度上限由用户选的档位决定(保守 ±10% / 平稳 ±20% / 激进 ±50%)。
    //      这里必须和提示词里告诉模型的额度【完全一致】: 提示词说 ±50% 而这里
    //      砍到 ±20%, 用户就会看到"选了激进还是没变化"—— 那正是这次的病。
    const ClampReport cr = apply_limits(current, want, step_style_);
    r.applied = cr.applied;
    r.clamp_notes = cr.notes;

    // ⑥ 落地 —— ★ 必须确认【运行时真的采用了】, 不能"写出去就算成功"
    //
    //   ★★ 这一步是 2026-09-15 修的核心 bug ★★
    //   用户报"自动调参不好用, 体感根本不明显"。根因不是模型调得少, 而是
    //   【参数压根没生效】: active.txt 指向 CF.ini(kp=15), 而实际在跑的是
    //   live_tune.ini(kp=6.2), 界面上却一直显示"已应用"。
    //   写盘成功 != 运行时采用。现在只有快照真的变成新值才算成功; 否则把
    //   "想要 / 文件里 / 实际在跑"三者一起报给用户。
    std::string werr;
    if (!set_(cr.applied, werr))
    {
        r.error = u8"参数没能真正生效(已回滚到原值):\n" + werr;
        // 写不进去就别记成"可用的新参数", 否则下一轮会以它为基准继续偏。
        return r;
    }

    // 记录"已知可用"用于回滚: 只在指标健康时更新
    if (!r.metrics.divergent)
    {
        last_good_ = cr.applied;
        have_last_good_ = true;
    }
    prev_metrics_ = r.metrics;
    have_prev_metrics_ = true;
    return r;
}

} // namespace boss::autotune
