#pragma once

// =============================================================================
// 调参 agent —— 从【真实运行日志】提取可调参用的性能指标
// =============================================================================
//
// 为什么要有这一层 (2026-09-14)
//   LLM 不能直接读 chainlog: 120fps 下 10 秒就是 1200 帧 x 15 字段, 喂进去既
//   超长又噪声极大, 而且模型没法可靠地"心算"过冲和稳态。所以先把原始逐帧记录
//   压成【少数几个有物理意义的标量】, 再交给 LLM 判断该往哪调。
//
//   ★ 这一层算错了, 后面 LLM 再聪明也是被喂垃圾 —— 所以指标定义必须写死在
//     代码里并有回归测试, 不能交给模型自由发挥。
//
// 与仿真台的关系
//   仿真台(tests/aim_acceptance_test.cpp 等)用【已知的真值】算指标;
//   这里用【运行日志反推】—— 两者算的是同一组定义, 但数据来源完全不同:
//     · 仿真台知道目标在哪(真值)      · 这里只知道控制器看到的误差
//   ★ 因此这里算出的"误差"是【检测误差】(画面里瞄点与准星之差), 它已经包含
//     了链路延迟、推理延迟、检测框抖动 —— 这正是调参【应该】依据的量。
//     仿真台的真值误差会偏乐观, 日志里的检测误差才是实机手感。
//
// 数据来源
//   runtime::chainlog 的 SecPid 段(段 2), 每帧一条, 字段见 mouse_thread_loop.cpp:
//     ex/ey     原始误差(像素)      eux/euy   加前馈后送入 P/I 的误差
//     ix/iy     积分状态            cmdx/y    取整前的浮点指令(计数)
//     carryx/y  未发出的计数零头     lim       本拍输出上限
//     sup       本拍是否压制运动    switch    是否刚换目标
//     jump_px   瞄点跳变            dt_ms     本拍 dt
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace boss::autotune {

// ── 单拍样本 ────────────────────────────────────────────────────────────────
// 只保留调参真正需要的字段。刻意不含 k̂: 控制器不用它, 调参也不需要它。
struct Sample
{
    std::int64_t frame = -1;
    double t = 0.0;            // 单调时钟秒
    double dt_ms = 0.0;
    double ex = 0.0, ey = 0.0; // 原始误差(像素) —— 【主指标用这个】
    double ex_used = 0.0;      // 送入 P/I 的误差(已扣前馈)
    double ix = 0.0, iy = 0.0; // 积分状态
    double cmdx = 0.0, cmdy = 0.0;   // 浮点指令(计数)
    double carryx = 0.0, carryy = 0.0;
    bool suppressed = false;   // 运动压制
    bool switched = false;     // 换目标
    double jump_px = 0.0;      // 瞄点跳变
    // 本拍检测框高度(像素)。★ 只用于学【基准框高 H₀】—— 即"用户整定参数时目标
    // 有多大", 它是尺度调度的参照点(见 mouse/aim_scale.h)。0 = 本行没带这个字段
    // (老日志格式), 学习时会被跳过。
    double bbox_h = 0.0;
    bool valid = false;
};

// ── 一个窗口的指标 ──────────────────────────────────────────────────────────
struct Metrics
{
    int n = 0;                 // 有效样本数
    double span_s = 0.0;       // 窗口时长

    // 误差幅值的统计量(像素)
    double err_median = 0.0;
    double err_p90 = 0.0;
    double err_p99 = 0.0;
    double err_max = 0.0;
    double err_mean = 0.0;
    double err_rms = 0.0;

    // 过冲: 误差【反号】后能达到的最大幅值。静止收敛时用它衡量"冲过头多少"。
    // ★ 定义: 取窗口内 ey 反号的样本(|ey| 的最大值), 而不是全程峰值 ——
    //   全程峰值在"目标刚开始移动"时必然很大, 那不是过冲。
    double overshoot_x = 0.0;
    double overshoot_y = 0.0;

    // 抖动: 指令(整数化前)的符号翻转率。自持 ±1 计数抖动的特征值。
    // ★ 关键: 稳态残差很小【不代表】没有抖动 —— 抖动时残差中位可能只有
    //   0.2px 却每拍在反号。所以翻转率必须单独看, 见 CLAUDE.md 亚量子保护那节。
    int nonzero_out = 0;
    int sign_flips = 0;
    double flip_ratio = 0.0;   // sign_flips / nonzero_out

    // 发散迹象: 误差不收敛地增长, 或出现非有限值
    bool divergent = false;

    // 换目标次数(窗口内) —— 用来判断这段数据是否干净(换目标时误差会跳变)
    int switches = 0;
    int suppressed = 0;

    std::string summary() const;   // 给 LLM 看的紧凑文本
};

// ── 解析 ────────────────────────────────────────────────────────────────────

// 从一行 chainlog 解析出 SecPid 样本。不是 SecPid 段或解析失败返回 false。
// 格式: L,2,frame=123,t=1.2345,dt_ms=8.33,...,ex=1.20,ey=-0.30,...
// ★ 刻意写得很宽容: 未知键直接跳过, 这样将来往日志里加字段不会让这里失效。
inline bool parse_pid_line(const char* line, Sample& out)
{
    out = Sample{};
    if (line == nullptr) return false;
    // 必须是 "L,2," (段 2 = SecPid)
    if (!(line[0] == 'L' && line[1] == ',' && line[2] == '2' && line[3] == ','))
        return false;

    bool saw_error = false;
    const char* p = line + 4;
    while (*p != '\0')
    {
        // 找 key=value 对
        const char* eq = p;
        while (*eq != '\0' && *eq != '=' && *eq != ',') ++eq;
        if (*eq != '=') break;
        const char* key = p;
        const std::size_t key_len = static_cast<std::size_t>(eq - p);
        const char* val = eq + 1;
        const char* end = val;
        while (*end != '\0' && *end != ',') ++end;

        const std::size_t val_len = static_cast<std::size_t>(end - val);
        char buf[64];
        if (val_len < sizeof(buf))
        {
            std::memcpy(buf, val, val_len);
            buf[val_len] = '\0';

            auto key_is = [&](const char* k) {
                return key_len == std::strlen(k) &&
                       std::strncmp(key, k, key_len) == 0;
            };
            if (key_is("frame"))      out.frame = std::atoll(buf);
            else if (key_is("t"))     out.t = std::atof(buf);
            else if (key_is("dt_ms")) out.dt_ms = std::atof(buf);
            else if (key_is("ex"))  { out.ex = std::atof(buf); saw_error = true; }
            else if (key_is("ey"))  { out.ey = std::atof(buf); saw_error = true; }
            else if (key_is("eux"))   out.ex_used = std::atof(buf);
            else if (key_is("ix"))    out.ix = std::atof(buf);
            else if (key_is("iy"))    out.iy = std::atof(buf);
            else if (key_is("cmdx"))  out.cmdx = std::atof(buf);
            else if (key_is("cmdy"))  out.cmdy = std::atof(buf);
            else if (key_is("carryx"))out.carryx = std::atof(buf);
            else if (key_is("carryy"))out.carryy = std::atof(buf);
            else if (key_is("sup"))   out.suppressed = (std::atoi(buf) != 0);
            else if (key_is("switch"))out.switched = (std::atoi(buf) != 0);
            else if (key_is("jump_px"))out.jump_px = std::atof(buf);
            else if (key_is("bbh"))    out.bbox_h = std::atof(buf);
            // 其余键(kx/ky/vx/vy/lim/ipx/...)有意忽略
        }
        if (*end == '\0') break;
        p = end + 1;
    }
    out.valid = saw_error;
    return out.valid;
}

// ── 计算指标 ────────────────────────────────────────────────────────────────

inline double percentile(const std::vector<double>& sorted, double q)
{
    if (sorted.empty()) return 0.0;
    const double idx = q * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

inline Metrics compute(const std::vector<Sample>& samples)
{
    Metrics m;
    std::vector<double> mag;   // 误差幅值 hypot(ex,ey) —— 【保持时间顺序】
    std::vector<double> ordered; // 同上, 排序副本仅供分位数使用
    mag.reserve(samples.size());

    for (const auto& s : samples)
    {
        if (!s.valid) continue;
        ++m.n;
        if (s.switched) ++m.switches;
        if (s.suppressed) ++m.suppressed;
        if (m.n == 1) m.span_s = 0.0;
        else m.span_s = s.t - samples.front().t;

        const double e = std::hypot(s.ex, s.ey);
        if (!std::isfinite(e)) { m.divergent = true; continue; }
        mag.push_back(e);
    }
    if (mag.empty()) return m;

    // 过冲: 误差反号的样本里 |ey| 的最大值(以 Y 为主, 因为横向甩枪主要在 Y;
    // 但两个轴都算出来, 让调用方按当前瞄的是哪个轴取)。
    // ★ 只统计"反号"的样本, 保证不是把"目标刚出现时的大误差"当成过冲。
    {
        double ox = 0.0, oy = 0.0;
        bool any_pos_x = false, any_neg_x = false;
        bool any_pos_y = false, any_neg_y = false;
        for (const auto& s : samples)
        {
            if (!s.valid || !std::isfinite(s.ex) || !std::isfinite(s.ey)) continue;
            if (s.ex > 0.0) any_pos_x = true; else if (s.ex < 0.0) any_neg_x = true;
            if (s.ey > 0.0) any_pos_y = true; else if (s.ey < 0.0) any_neg_y = true;
        }
        // 两个方向都出现过才算"有反号", 否则整段是单向逼近, 过冲记 0
        if (any_pos_x && any_neg_x)
            for (const auto& s : samples)
                if (s.valid && s.ex < 0.0) ox = std::max(ox, -s.ex);
        if (any_pos_y && any_neg_y)
            for (const auto& s : samples)
                if (s.valid && s.ey < 0.0) oy = std::max(oy, -s.ey);
        m.overshoot_x = ox;
        m.overshoot_y = oy;
    }

    // 指令符号翻转率: 只在【非零】指令之间数翻转 —— 零指令是"没动",
    // 把它算进去会把"偶尔停一下"误判成抖动。
    {
        int last = 0;
        for (const auto& s : samples)
        {
            if (!s.valid || !std::isfinite(s.cmdy)) continue;
            const double c = s.cmdy;
            if (std::abs(c) < 0.5) continue;      // 取整后为 0, 不算下发
            ++m.nonzero_out;
            const int sign = (c > 0.0) ? 1 : -1;
            if (last != 0 && sign != last) ++m.sign_flips;
            last = sign;
        }
        m.flip_ratio = (m.nonzero_out > 0)
            ? static_cast<double>(m.sign_flips) / static_cast<double>(m.nonzero_out)
            : 0.0;
    }

    // ★ mag 必须保持【时间顺序】—— 下面的发散判据要按时间取头尾。
    //   早期版本在这里直接 std::sort(mag), 结果"前 25%"变成了"误差最小的
    //   25%", 于是任何一段有大小起伏的【正常收敛】数据都被判成发散
    //   (最大的必然比最小的大 3 倍以上)。这会让 agent 永远在原地回滚。
    //   所以分位数走 ordered 副本, 时间序列保持原样。
    ordered = mag;
    std::sort(ordered.begin(), ordered.end());
    m.err_median = percentile(ordered, 0.50);
    m.err_p90 = percentile(ordered, 0.90);
    m.err_p99 = percentile(ordered, 0.99);
    m.err_max = ordered.back();
    double sum = 0.0, sq = 0.0;
    for (double v : mag) { sum += v; sq += v * v; }
    m.err_mean = sum / static_cast<double>(mag.size());
    m.err_rms = std::sqrt(sq / static_cast<double>(mag.size()));

    // 发散判定: 尾段(时间上的后 25%)的误差中位显著大于前段(时间上的前 25%),
    // 或者出现超大误差。
    // ★ 阈值刻意宽松: 实机换目标/急停都会让误差尖一下, 不能一有尖峰就判发散。
    // ★ 必须按【时间顺序】切, 不能按幅值排序后切 —— 理由见上面的注释。
    if (mag.size() >= 32)
    {
        const std::size_t q = mag.size() / 4;
        std::vector<double> head(mag.begin(), mag.begin() + static_cast<std::ptrdiff_t>(q));
        std::vector<double> tail(mag.end() - static_cast<std::ptrdiff_t>(q), mag.end());
        std::sort(head.begin(), head.end());
        std::sort(tail.begin(), tail.end());
        const double hm = percentile(head, 0.5);
        const double tm = percentile(tail, 0.5);
        if (tm > 5.0 && tm > hm * 3.0) m.divergent = true;
    }
    if (m.err_max > 500.0) m.divergent = true;

    return m;
}

// 给 LLM 的紧凑摘要。刻意用中文键名 + 单位, 避免模型把像素当成计数。
inline std::string Metrics::summary() const
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "样本=%d 时长=%.2fs | 误差px: 中位%.2f p90=%.2f p99=%.2f 最大%.2f RMS=%.2f | "
        "过冲px: X=%.2f Y=%.2f | 指令下发=%d 翻号=%d 翻转率=%.0f%% | "
        "换目标=%d 压制=%d | %s",
        n, span_s, err_median, err_p90, err_p99, err_max, err_rms,
        overshoot_x, overshoot_y, nonzero_out, sign_flips, flip_ratio * 100.0,
        switches, suppressed,
        divergent ? "★判为发散" : "未发散");
    return std::string(buf);
}

} // namespace boss::autotune
