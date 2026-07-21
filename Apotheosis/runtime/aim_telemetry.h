#ifndef RUNTIME_AIM_TELEMETRY_H
#define RUNTIME_AIM_TELEMETRY_H

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core/types.hpp>

namespace runtime
{

// -------------------------------------------------------------------------
// Aim trajectory replay. Ring buffer holding ~replay_seconds of frames.
// Each frame snapshot is intentionally light (struct-of-vectors) — the
// overlay reads the buffer directly, no heavy copies.
// -------------------------------------------------------------------------
struct ReplayFrame
{
    std::chrono::steady_clock::time_point ts;
    std::vector<cv::Rect> boxes;
    std::vector<int> class_ids;
    std::vector<int> track_ids;
    int   locked_track_id = -1;
    double pivot_x = 0.0;
    double pivot_y = 0.0;
    int   mouse_dx = 0;
    int   mouse_dy = 0;
    bool  hotkey_active = false;
};

class ReplayBuffer
{
public:
    static ReplayBuffer& instance();

    void setEnabled(bool enabled);
    bool enabled() const;

    // Cap retention to roughly `seconds` of recent frames. Calls older than
    // this are dropped each push. Hot path — keep cheap.
    void setRetentionSeconds(int seconds);

    void push(const ReplayFrame& frame);
    void clear();

    // Snapshot copy — used by the slow-motion overlay so it can iterate at
    // its own pace without holding the writer lock.
    std::vector<ReplayFrame> snapshot() const;
    size_t size() const;

private:
    ReplayBuffer() = default;

    mutable std::mutex mu_;
    std::deque<ReplayFrame> frames_;
    bool   enabled_ = false;
    int    retention_seconds_ = 10;
};

} // namespace runtime

// Replay playback toggles. Defined in overlay/draw_debug.cpp; declared here
// so any TU that needs to read or set them gets a real declaration instead
// of a hand-typed local extern. Toggled by the Debug panel; consumed by
// the playback overlay in the same file (and potentially future overlays).
#include <atomic>
extern std::atomic<bool> g_replay_playback_active;
extern std::atomic<int>  g_replay_playback_frame;
extern std::atomic<float> g_mouse_queue_latency_ms;
extern std::atomic<int> g_mouse_queue_backlog;
extern std::atomic<unsigned long long> g_mouse_send_failures;

#endif // RUNTIME_AIM_TELEMETRY_H
