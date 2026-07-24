#ifndef MOUSE_LATEST_MOVE_SLOT_H
#define MOUSE_LATEST_MOVE_SLOT_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace mouse_async
{

struct PendingMove
{
    int dx = 0;
    int dy = 0;
    std::chrono::steady_clock::time_point queued_at{};
    std::uint64_t generation = 0;
};

// 调用方用自己的互斥量保护 replace/take/clear/hasPending。
// generation 是原子值，worker 解锁后仍可判断已取出的旧批次是否被抢占。
class LatestMoveSlot
{
public:
    std::uint64_t replace(int dx, int dy,
                          std::chrono::steady_clock::time_point queued_at) noexcept
    {
        const std::uint64_t generation =
            generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        pending_ = { dx, dy, queued_at, generation };
        has_pending_ = true;
        return generation;
    }

    bool take(PendingMove& output) noexcept
    {
        if (!has_pending_)
            return false;
        output = pending_;
        has_pending_ = false;
        return true;
    }

    void clear() noexcept
    {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        has_pending_ = false;
        pending_ = {};
    }

    bool isCurrent(std::uint64_t generation) const noexcept
    {
        return generation_.load(std::memory_order_acquire) == generation;
    }

    bool hasPending() const noexcept { return has_pending_; }
    std::size_t pendingCount() const noexcept { return has_pending_ ? 1u : 0u; }

private:
    PendingMove pending_{};
    bool has_pending_ = false;
    std::atomic<std::uint64_t> generation_{ 0 };
};

} // namespace mouse_async

#endif // MOUSE_LATEST_MOVE_SLOT_H
