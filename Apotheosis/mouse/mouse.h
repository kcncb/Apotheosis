#ifndef MOUSE_H
#define MOUSE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "latest_move_slot.h"
#include "control/predictive_controller.h"

// Forward declarations so that mouse.h stays light.
class MakcuConnection;
class MakcuNewConnection;

struct MouseRuntimeParams
{
    int detection_resolution = 320;
    motion::Calibration calibration;
};

class MouseThread
{
public:
    struct MovementFeedback
    {
        int dx = 0;
        int dy = 0;
        double latency_ms = 0.0;
        size_t backlog = 0;
        unsigned long long failed = 0;
    };

    MouseThread(
        const MouseRuntimeParams& params,
        MakcuConnection* makcuConnection = nullptr,
        MakcuNewConnection* makcuNewConnection = nullptr);
    ~MouseThread();

    MouseThread(const MouseThread&) = delete;
    MouseThread& operator=(const MouseThread&) = delete;

    void updateParams(const MouseRuntimeParams& params);

    void clearQueuedMoves();
    MovementFeedback consumeMovementFeedback();

    // Recursive: a high-level aim op holds this lock while
    // sendLeft{Down,Up}ToDriver may chain into the same lock on the same thread.
    std::recursive_mutex input_method_mutex;

    // ─── Raw driver channel (used by the Boss AI aim engine) ───────────────
    void sendRawMove(int dx, int dy, int64_t capture_ns = 0, int64_t aim_ns = 0, uint64_t command_id = 0);
    void sendPixelMove(double dx, double dy, motion::Calibration calibration,
                       int limit_x, int limit_y, int64_t capture_ns, int64_t aim_ns);
    std::shared_ptr<motion::CommandJournal> commandJournal() const { return journal_; }
    void pressLeftButton();
    void releaseLeftButton();

    // Input device hot-swap.
    void setMakcuConnection(MakcuConnection* makcu);
    void setMakcuNewConnection(MakcuNewConnection* makcuNew);

private:
    void moveWorkerLoop();
    void queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns, uint64_t command_id);
    bool sendMovementToDriver(int dx, int dy);

    void sendLeftDownToDriver();
    void sendLeftUpToDriver();

    MouseRuntimeParams params_{};
    std::shared_ptr<motion::CommandJournal> journal_ = std::make_shared<motion::CommandJournal>();
    motion::OutputMapper mapper_;
    std::mutex outputMtx_;
    uint64_t pendingCommandId_ = 0;

    // Async driver dispatch.
    mouse_async::LatestMoveSlot moveSlot_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    std::thread moveWorker_;
    std::atomic<bool> workerStop_{ false };
    std::atomic<long long> appliedDx_{ 0 };
    std::atomic<long long> appliedDy_{ 0 };
    std::atomic<long long> lastLatencyUs_{ 0 };
    std::atomic<unsigned long long> failedMoves_{ 0 };

    MakcuConnection* makcu_ = nullptr;
    MakcuNewConnection* makcu_new_ = nullptr;
};

#endif // MOUSE_H
