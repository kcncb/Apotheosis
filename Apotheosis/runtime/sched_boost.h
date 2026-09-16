#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 调度特权 (process / thread QoS)
//
// 取自原神AI 的推理性能集:
//     process_qos=true, process_timer=true, process_priority=0x8000,
//     timer=true, thread_qos=true, thread_priority=true,
//     mmcss=true, mmcss_priority=true
//
// 折叠成本模块的三个动作:
//   1. 进程 → HIGH_PRIORITY_CLASS   (原神: process_priority=0x8000)
//   2. 线程 → MMCSS 注册            (原神: mmcss=true / thread_qos=true)
//   3. 全局 1ms 定时器分辨率        (原神: timer=true; 本方已在 main 里做)
//
// 这些【不提升吞吐】, 只把 CPU 抢占造成的 1~5ms 抖动按掉。体感由尾延迟决定,
// 所以这一层在"已经很快"之后仍然值得做。
// ─────────────────────────────────────────────────────────────────────────────

namespace sched_boost
{
// 把当前进程提到 HIGH_PRIORITY_CLASS。返回是否成功(已提升或本就更高)。
// 幂等: 重复调用安全。
bool boostProcessPriority();

// 把【当前线程】注册进 MMCSS。成功时返回一个非空句柄, 调用方负责在
// 线程退出前用它调用 unregisterCurrentThread()。
//
// taskName 为空时按 Windows 的 "Games" 任务类注册 (Win10+ 映射到
// "\Games\Games"), 这正是原神 thread_qos 对应的档位。
void* registerCurrentThreadWithMmcss(const char* taskName);

// 反注册 (与 registerCurrentThreadWithMmcss 配对)。
void unregisterCurrentThread(void* handle);

// RAII 包装: 在作用域内为当前线程持有 MMCSS 注册。
// 构造即尝试注册, 析构自动反注册。移动/拷贝禁用。
class ScopedThreadBoost
{
public:
    explicit ScopedThreadBoost(const char* taskName);
    ~ScopedThreadBoost();

    ScopedThreadBoost(const ScopedThreadBoost&) = delete;
    ScopedThreadBoost& operator=(const ScopedThreadBoost&) = delete;

    bool active() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};
}
