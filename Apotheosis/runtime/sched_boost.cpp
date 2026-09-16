#include "sched_boost.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <avrt.h>

#include <iostream>

// avrt.lib 在 CMakeLists 里链接; 用 #pragma comment 兜底, 这样即使目标里
// 漏了链接也不会在链接期炸掉。
#pragma comment(lib, "avrt.lib")

namespace sched_boost
{

bool boostProcessPriority()
{
    // HIGH_PRIORITY_CLASS (0x00000080)。原神日志里的 process_priority=0x8000
    // 是它自己枚举的档位值, 对应的就是这一档。
    //
    // 不选 REALTIME_PRIORITY_CLASS: 那会让本进程抢占系统关键线程(磁盘/网络/
    // 音频), 在推理只占几毫秒的场景下收益为零而风险很大。
    if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        return true;

    std::cerr << "[Sched] SetPriorityClass(HIGH) failed, gle=" << GetLastError()
              << " (继续以默认优先级运行)" << std::endl;
    return false;
}

void* registerCurrentThreadWithMmcss(const char* taskName)
{
    // MMCSS (Multimedia Class Scheduler Service) 把线程划进一个受保护的
    // 调度类别: 系统会为它预留 CPU 带宽, 并抑制其它线程(包括普通
    // 后台线程)对它的抢占。
    //
    // 传入的 taskName "Games" 由 Windows 自己映射到 "\Games\Games";
    // 若显式给了带反斜杠的完整路径则原样使用。
    const wchar_t* task = L"Games";
    wchar_t wide[128] = {};
    if (taskName && *taskName)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, taskName, -1, wide,
                                          static_cast<int>(std::size(wide)));
        if (n > 0)
            task = wide;
    }

    DWORD taskIndex = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &taskIndex);
    if (!h)
    {
        // 失败通常是权限/服务未启动。不致命, 只记录 —— 上层会照常推理,
        // 只是拿不到这一档的调度保护。
        std::cerr << "[Sched] AvSetMmThreadCharacteristics failed, gle="
                  << GetLastError() << " (推理线程按普通优先级运行)" << std::endl;
        return nullptr;
    }

    // 把该线程在 MMCSS 类别内提到最高相对优先级 (原神 thread_priority=true)。
    // AvSetMmThreadPriority 只在已注册 MMCSS 的前提下有意义。
    //
    // 用 AVRT_PRIORITY_CRITICAL: 该枚举在 avrt.h 里定义, 但 HIGHEST 这个
    // 名字在部分 SDK 版本里缺失 —— CRITICAL 是语义最强的档且一直存在。
    if (!AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL))
    {
        std::cerr << "[Sched] AvSetMmThreadPriority failed, gle=" << GetLastError()
                  << " (保持 MMCSS 默认档)" << std::endl;
    }

    return h;
}

void unregisterCurrentThread(void* handle)
{
    if (handle)
        AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle));
}

ScopedThreadBoost::ScopedThreadBoost(const char* taskName)
{
    handle_ = registerCurrentThreadWithMmcss(taskName);
}

ScopedThreadBoost::~ScopedThreadBoost()
{
    unregisterCurrentThread(handle_);
    handle_ = nullptr;
}

} // namespace sched_boost
