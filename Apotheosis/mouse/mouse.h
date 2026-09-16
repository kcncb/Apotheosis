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

// Forward declarations so that mouse.h stays light.
class MakcuConnection;
class MakcuNewConnection;

struct MouseRuntimeParams
{
    int detection_resolution = 320;
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
    void sendRawMove(int dx, int dy, int64_t capture_ns = 0, int64_t aim_ns = 0);
    void pressLeftButton();
    void releaseLeftButton();
    // 自动开镜 (mouse/trigger_scope.h) 用的右键通道。
    // MAKCU 系盒子按 HID 通道编号: 1=左, 2=右。
    void pressRightButton();
    void releaseRightButton();

    // 自动急停 (mouse/auto_stop.h) 用的键盘通道。
    // hid_key = HID usage id; hold_ms 由固件定时弹起(自清, 不会卡键)。
    // 只有 MAKCUNEW 支持键盘注入, 其它输入方式返回 false(功能自动失效)。
    bool tapKey(int hid_key, int hold_ms);

    // Input device hot-swap.
    void setMakcuConnection(MakcuConnection* makcu);
    void setMakcuNewConnection(MakcuNewConnection* makcuNew);

private:
    void moveWorkerLoop();
    void queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns);
    bool sendMovementToDriver(int dx, int dy);

    void sendLeftDownToDriver();
    void sendLeftUpToDriver();
    void sendRightDownToDriver();
    void sendRightUpToDriver();

    MouseRuntimeParams params_{};
    std::mutex outputMtx_;

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
