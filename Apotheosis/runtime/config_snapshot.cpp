#include "runtime/config_snapshot.h"

#include "Apotheosis.h"
#include "config/config.h"

#include <atomic>
#include <mutex>

namespace runtime_config
{
namespace
{
std::shared_ptr<const Config> g_snapshot;
}

void publish()
{
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    std::shared_ptr<const Config> next = std::make_shared<const Config>(config);
    std::atomic_store_explicit(&g_snapshot, std::move(next), std::memory_order_release);
}

std::shared_ptr<const Config> read()
{
    auto current = std::atomic_load_explicit(&g_snapshot, std::memory_order_acquire);
    if (!current)
    {
        publish();
        current = std::atomic_load_explicit(&g_snapshot, std::memory_order_acquire);
    }
    return current;
}
}
