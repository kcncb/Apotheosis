#include "aim_telemetry.h"

#include <algorithm>
#include <cmath>

namespace runtime
{

// =========================================================================
// ReplayBuffer
// =========================================================================

ReplayBuffer& ReplayBuffer::instance()
{
    static ReplayBuffer b;
    return b;
}

void ReplayBuffer::setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = enabled;
    if (!enabled_)
        frames_.clear();
}

bool ReplayBuffer::enabled() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

void ReplayBuffer::setRetentionSeconds(int seconds)
{
    std::lock_guard<std::mutex> lk(mu_);
    retention_seconds_ = std::clamp(seconds, 1, 60);
}

void ReplayBuffer::push(const ReplayFrame& frame)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_)
        return;

    frames_.push_back(frame);

    // Trim by age. We don't bound by count because frame rate varies.
    const auto cutoff = frame.ts - std::chrono::seconds(retention_seconds_);
    while (!frames_.empty() && frames_.front().ts < cutoff)
        frames_.pop_front();
}

void ReplayBuffer::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    frames_.clear();
}

std::vector<ReplayFrame> ReplayBuffer::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<ReplayFrame>(frames_.begin(), frames_.end());
}

size_t ReplayBuffer::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return frames_.size();
}

} // namespace runtime
