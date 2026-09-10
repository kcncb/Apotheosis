#include "cpu_affinity_manager.h"

#include <iostream>

bool CPUAffinityManager::reserveCPUCores(int numCores)
{
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD totalCores = sysInfo.dwNumberOfProcessors;

    DWORD_PTR mask = 0;

    // 智能防抢占亲和性调优：
    // 如果系统核心数 >= 4，避开 CPU 0 和 CPU 1（系统中断与桌面服务重度占用区），
    // 优先绑定到 CPU 2, CPU 3 ... 高性能独立大核；如果核数较少则退化为常规掩码。
    if (totalCores >= 4)
    {
        int count = 0;
        for (DWORD i = 2; i < totalCores && count < numCores; ++i, ++count) {
            mask |= (1ULL << i);
        }
    }
    else
    {
        for (int i = 0; i < numCores && i < (int)totalCores; i++) {
            mask |= (1ULL << i);
        }
    }

    if (mask == 0) mask = 1;

    originalMask = SetProcessAffinityMask(GetCurrentProcess(), mask);
    if (originalMask == 0)
    {
        // 若进程级亲和性受限，尝试线程级绑定
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }

    // 提升进程基础优先级为 HIGH_PRIORITY_CLASS (高性能电竞级)
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
    {
        std::cerr << "[CPU] Failed to set process priority. GetLastError="
                  << GetLastError() << std::endl;
    }

    // 提升主线程优先级
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::cout << "[CPU] Affinity automatically tuned to dedicated cores (mask=0x"
              << std::hex << mask << std::dec << ") with HIGH priority." << std::endl;

    return true;
}

bool CPUAffinityManager::reserveSystemMemory(size_t reservedMemoryMB)
{
    size_t reservedSize = reservedMemoryMB * 1024 * 1024;

    reservedMemory = malloc(reservedSize);
    if (reservedMemory)
    {
        memset(reservedMemory, 0, reservedSize);
        return true;
    }
    std::cerr << "[CPU] Failed to reserve system memory: " << reservedMemoryMB << " MB." << std::endl;
    return false;
}
