#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>

namespace capture
{
// Driver timestamps are not a measurement of the HDMI pipeline. Keep validity
// and freshness separate from a measured zero; all times here are explicit so
// missing samples, long stalls and clock changes can be regression tested.
class DeviceFrameAge
{
public:
    void update(uint64_t timestamp, double qpc100ns, double file100ns, int64_t now_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!timestamp) { invalidateLocked(); return; }
        const double ts = static_cast<double>(timestamp);
        const double qpc_ms = (qpc100ns - ts) / 1.0e4;
        const double file_ms = (file100ns - ts) / 1.0e4;
        int epoch = epoch_;
        double ms = epoch == 2 ? file_ms : qpc_ms;
        if (!epoch)
        {
            if (qpc100ns > 0 && plausible(qpc_ms)) { epoch = 1; ms = qpc_ms; }
            else if (file100ns > 0 && plausible(file_ms)) { epoch = 2; ms = file_ms; }
        }
        if (!epoch || !plausible(ms)) { invalidateLocked(); return; }
        epoch_ = epoch;
        ms = ms < 0.0 ? 0.0 : ms;
        const double us = ms * 1000.0;
        // A stale interval starts a new series instead of blending in old data.
        if (last_ns_ && now_ns - last_ns_ > kFreshnessNs) samples_ = 0;
        ema_us_ = samples_ == 0 ? us
            : samples_ < 10 ? (ema_us_ * samples_ + us) / (samples_ + 1)
            : ema_us_ * 0.9 + us * 0.1;
        if (samples_ < 10) ++samples_;
        last_ns_ = now_ns;
    }

    int read(int64_t now_ns) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!samples_ || now_ns < last_ns_ || now_ns - last_ns_ > kFreshnessNs) return -1;
        return static_cast<int>(std::lround(ema_us_));
    }

    void invalidate()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        invalidateLocked();
    }

private:
    // Distinguishes the epochs (hundreds of years apart) without hiding real
    // queueing above 500 ms. Ten minutes also fits safely in signed microseconds.
    static bool plausible(double ms) { return std::isfinite(ms) && ms >= -2.0 && ms <= 600000.0; }
    static constexpr int64_t kFreshnessNs = 2'000'000'000;
    void invalidateLocked() { samples_ = 0; epoch_ = 0; last_ns_ = 0; ema_us_ = 0; }
    mutable std::mutex mutex_;
    int epoch_ = 0;
    int samples_ = 0;
    double ema_us_ = 0;
    int64_t last_ns_ = 0;
};
}
