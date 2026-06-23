#include "glass_runtime.h"

#include <mutex>

namespace glass_runtime
{

namespace
{
std::mutex g_mtx;
Snapshot   g_snap;
} // namespace

Snapshot read()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void publish(Snapshot snap)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_snap = std::move(snap);
}

} // namespace glass_runtime
