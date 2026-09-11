#include "mem/gpu_ready_event.h"
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; return 1; } } while (0)
int main()
{
    std::shared_ptr<GpuReadyEvent> delayed;
    {
        GpuReadyEventPool<1> pool;
        delayed = pool.record(nullptr);
        CHECK(delayed && liveEvents == 1);
        auto next = pool.record(nullptr);
        CHECK(next && next->handle != delayed->handle);
        CHECK(delayed->handle->records == 1); // never re-record a delayed frame's marker
        auto reusable = next->handle;
        next.reset();
        auto reused = pool.record(nullptr);
        CHECK(reused->handle == reusable);
        CHECK(reused->handle->records == 2);
    }
    CHECK(liveEvents == 1 && delayed->handle->records == 1); // survives capture teardown
    delayed.reset();
    CHECK(liveEvents == 0);
    {
        GpuReadyEventPool<1> pool;
        failCreate = true;
        CHECK(!pool.record(nullptr));
        failCreate = false;
        failRecord = true;
        CHECK(!pool.record(nullptr));
        failRecord = false;
        CHECK(pool.record(nullptr));
    }
    CHECK(liveEvents == 0);
    std::cout << "GPU event ownership: passed (instrumented CUDA API)\n";
}
