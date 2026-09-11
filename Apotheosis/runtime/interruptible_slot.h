#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace runtime
{
// One outstanding asynchronous operation. The callback owns a reference to the
// slot, never to its consumer. A missing callback cannot prevent cancellation.
template<class T>
class InterruptibleSlot
{
public:
    void publish(T value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        value_ = std::move(value);
        ready_ = true;
        cv_.notify_one();
    }

    bool wait(T& value, const std::atomic<bool>& stop)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!ready_ && !closed_ && !stop.load())
            cv_.wait_for(lock, std::chrono::milliseconds(20));
        if (closed_ || stop.load()) return false;
        value = std::move(value_);
        ready_ = false;
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        value_ = T{};
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    T value_{};
    bool ready_ = false;
    bool closed_ = false;
};
}
