#pragma once

#include <cuda_runtime.h>
#include <array>
#include <cstddef>
#include <memory>

struct GpuReadyEvent
{
    cudaEvent_t handle = nullptr;
    ~GpuReadyEvent() { if (handle) cudaEventDestroy(handle); }
};

// Reuse only events no consumer still holds. A frame delayed across a capture
// restart keeps its completion event alive, just like its pixel storage.
template<size_t N>
class GpuReadyEventPool
{
public:
    std::shared_ptr<GpuReadyEvent> record(cudaStream_t stream)
    {
        auto& event = events_[next_];
        next_ = (next_ + 1) % N;
        if (!event || event.use_count() != 1)
        {
            auto fresh = std::make_shared<GpuReadyEvent>();
            if (cudaEventCreateWithFlags(&fresh->handle, cudaEventDisableTiming) != cudaSuccess)
                return {};
            event = std::move(fresh);
        }
        if (cudaEventRecord(event->handle, stream) != cudaSuccess) return {};
        return event;
    }
private:
    std::array<std::shared_ptr<GpuReadyEvent>, N> events_{};
    size_t next_ = 0;
};
