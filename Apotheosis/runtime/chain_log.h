#pragma once

// =============================================================================
// 全链路排查日志 (chain log)
// =============================================================================
//
// 目的: 采集链路 -> 推理 -> 目标选择/跟踪 -> 瞄点 -> 找色枢轴 -> PIDF 控制
//       -> 位移下发(mouse/HID) -> 自动扳机 这一段出问题时, 能拿到【逐帧】的完整
//       现场。所以设计成"永远在写、随时能倒出来看"的形式, 而不是"事先开日志、
//       复现一次、再翻文件"。
//
// 设计要点
//   · 固定环形缓冲: 每条记录定长, 覆盖写。内存恒定(默认 8192 条 x 512B = 4MB),
//     不需要时几乎零成本, 不需要时也不会撑爆磁盘。出问题后 dump 出来就能看到
//     【最近 N 帧】的全部细节。
//   · 零分配: push 时只做一次 snprintf 到记录自己的缓冲里, 不做任何 new/malloc。
//   · 分段开关: 采集/推理/选择/瞄点/找色/控制/下发/扳机 各一段, 可只开关心的段
//     (见 Level 与 runtime::chainlog::set_section)。
//   · 单行 key=value: 便于 grep/awk, 也便于贴群里求助。
//
// 怎么用(排查流程)
//   1. 复现问题。
//   2. 在主循环或任意线程里调用 runtime::chainlog::dump("logs/aim_chain.log");
//      (本仓库已在瞄准主循环的会话结束处与"确认锁丢失"处自动 dump, 见
//       mouse_thread_loop.cpp 的 chain_log_auto_dump() 调用)
//   3. 打开文件: 每行一条记录, 带 frame= (帧号) 与 t= (单调时钟秒), 同一帧的
//      采集/推理/选择/控制/下发/扳机记录用同一个 frame= 关联。
//
// 记录格式: L,<段>,frame=<n>,t=<sec>,k=v,k=v,...
//   L 是固定前缀(便于和程序里其它输出区分)。
//
// 环境变量(启动时读一次, 便于免编译调试)
//   APOTH_CHAINLOG=0   关闭(默认仍记录"关键事件"与逐帧摘要)
//   APOTH_CHAINLOG=1   关键事件 + 每次 dump 的统计
//   APOTH_CHAINLOG=2   逐帧全量(推荐排查时用)
//   APOTH_CHAINLOG=3   逐帧全量 + 每个候选目标明细(最详细, 开销最大)
//
// 开销: 逐帧全量约 5-15 条记录/帧, 每条 ~200 字节, 120Hz 下约 1-2MB/s 的写入量,
//       全在内存里, 对瞄准回路的影响远小于一帧抖动。
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace runtime::chainlog {

// ── 配置 ────────────────────────────────────────────────────────────────────
enum Section : int
{
    SecCapture = 0,     // 采集: 帧号/时间戳/分辨率/后端/FPS
    SecDetect,          // 推理: 模型/耗时/检测数/框明细
    SecSelect,          // 选择与跟踪: 候选/命中槽位/代数/跟踪器状态
    SecAimpoint,        // 瞄点: 框 -> 锚点(比例/模式)
    SecCrosshair,       // 找色: 枢轴/命中/平滑状态
    SecControl,         // 控制器: 误差/PIDF 内部/生效增益/输出
    SecExec,            // 下发: 限幅/曲线整形/队列/实际发送
    SecTrigger,         // 自动扳机: 状态机/命中区判定/按下松开
    SecLatency,         // 延迟探针: 各阶段耗时
    SecCount
};

// 记录上限(超过则覆盖最旧)。8192 条在 120Hz、10 条/帧下 ≈ 6.8 秒现场。
inline constexpr int kCapacity = 8192;
inline constexpr int kLineSize = 512;

struct Sink
{
    std::mutex mutex;
    char lines[kCapacity][kLineSize];
    std::int64_t frame[kCapacity]{};
    double time[kCapacity]{};
    std::int64_t written = 0;      // 累计写入条数(用于判断是否被覆盖)
    std::int64_t pending = 0;      // 自上次 dump 之后新增的条数
    std::int64_t sequence = 0;     // 单调序号(全局排序用)
};

inline Sink& sink()
{
    static Sink instance;
    return instance;
}

// 级别: 0 只记关键事件, 1 额外记 dump 统计, 2 逐帧全量, 3 含候选明细
inline int& level()
{
    static int value = -1;
    if (value < 0)
    {
        const char* env = std::getenv("APOTH_CHAINLOG");
        value = (env != nullptr) ? std::atoi(env) : 2;
        if (value < 0) value = 0;
        if (value > 3) value = 3;
    }
    return value;
}

inline double now_seconds()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

inline bool enabled(Section section, int minimum_level)
{
    (void)section;
    return level() >= minimum_level;
}

// 逐帧上下文: 主循环每帧设置一次, 之后所有记录自动带上 frame/t,
// 不需要在每个调用点重复传参。
struct FrameContext
{
    std::int64_t frame = -1;
    double time = 0.0;
    bool valid = false;
};
inline FrameContext& frame_context()
{
    static FrameContext context;
    return context;
}
inline void begin_frame(std::int64_t frame_id)
{
    auto& context = frame_context();
    context.frame = frame_id;
    context.time = now_seconds();
    context.valid = true;
}
inline void end_frame()
{
    frame_context().valid = false;
}

// ── 写入 ────────────────────────────────────────────────────────────────────
#if defined(__GNUC__) || defined(__clang__)
#define APOTH_CHAINLOG_PRINTF(fmt_index, arg_index) \
    __attribute__((format(printf, fmt_index, arg_index)))
#else
#define APOTH_CHAINLOG_PRINTF(fmt_index, arg_index)   // MSVC 不支持 __attribute__
#endif

APOTH_CHAINLOG_PRINTF(3, 4)
inline void write(Section section, int minimum_level, const char* format, ...)
{
    if (!enabled(section, minimum_level))
        return;
    auto& s = sink();
    std::lock_guard<std::mutex> guard(s.mutex);
    const std::int64_t slot = s.sequence % kCapacity;
    const auto& context = frame_context();
    char* line = s.lines[slot];
    int used = std::snprintf(line, kLineSize, "L,%d", static_cast<int>(section));
    if (context.valid && used > 0 && used < kLineSize)
        used += std::snprintf(line + used, kLineSize - used,
                              ",frame=%lld,t=%.4f",
                              static_cast<long long>(context.frame), context.time);
    if (used > 0 && used < kLineSize)
    {
        va_list args;
        va_start(args, format);
        const int added = std::vsnprintf(line + used, kLineSize - used, format, args);
        va_end(args);
        if (added > 0)
            used += added;
    }
    if (used >= kLineSize)
        used = kLineSize - 1;
    line[used < 0 ? 0 : used] = '\0';
    s.frame[slot] = context.valid ? context.frame : -1;
    s.time[slot] = context.valid ? context.time : now_seconds();
    ++s.sequence;
    ++s.written;
    ++s.pending;
}

// 关键事件: 不受逐帧开关影响(级别 >= 0 就记), 用于记录"为什么没瞄/为什么松手"
// 这类一次性判定。用 key=value 写清原因, 便于事后统计。
inline void event(const char* format, ...)
{
    if (level() < 0)
        return;
    auto& s = sink();
    std::lock_guard<std::mutex> guard(s.mutex);
    const std::int64_t slot = s.sequence % kCapacity;
    char* line = s.lines[slot];
    int used = std::snprintf(line, kLineSize, "L,event");
    const auto& context = frame_context();
    if (context.valid && used > 0 && used < kLineSize)
        used += std::snprintf(line + used, kLineSize - used,
                              ",frame=%lld,t=%.4f",
                              static_cast<long long>(context.frame), context.time);
    if (used > 0 && used < kLineSize)
    {
        va_list args;
        va_start(args, format);
        const int added = std::vsnprintf(line + used, kLineSize - used, format, args);
        va_end(args);
        if (added > 0)
            used += added;
    }
    if (used >= kLineSize)
        used = kLineSize - 1;
    line[used < 0 ? 0 : used] = '\0';
    s.frame[slot] = context.valid ? context.frame : -1;
    s.time[slot] = now_seconds();
    ++s.sequence;
    ++s.written;
    ++s.pending;
}

// ── 导出 ────────────────────────────────────────────────────────────────────
// 把环形缓冲里的记录按时间顺序写成文本文件。返回写入的条数(<0 表示打不开文件)。
inline std::int64_t dump(const char* path)
{
    auto& s = sink();
    std::lock_guard<std::mutex> guard(s.mutex);
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
        return -1;
    const std::int64_t total = s.sequence < kCapacity ? s.sequence : kCapacity;
    const std::int64_t first = s.sequence - total;
    std::fprintf(file,
                 "# Apotheosis chain log  records=%lld capacity=%d level=%d "
                 "dropped=%lld\n",
                 static_cast<long long>(total), kCapacity, level(),
                 static_cast<long long>(s.written - total));
    std::fprintf(file,
                 "# 格式: L,<段>,frame=<帧号>,t=<秒>,key=value,...\n"
                 "# 段: 0采集 1推理 2选择/跟踪 3瞄点 4找色 5控制 6下发 7扳机 8延迟; "
                 "event=关键事件\n");
    for (std::int64_t i = first; i < s.sequence; ++i)
    {
        const std::int64_t slot = i % kCapacity;
        if (s.lines[slot][0] == '\0')
            continue;
        std::fwrite(s.lines[slot], 1, std::strlen(s.lines[slot]), file);
        std::fputc('\n', file);
    }
    std::fclose(file);
    s.pending = 0;
    return total;
}

} // namespace runtime::chainlog
