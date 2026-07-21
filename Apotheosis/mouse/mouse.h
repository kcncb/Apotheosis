#ifndef MOUSE_H
#define MOUSE_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

// Forward declarations so that mouse.h stays light.
class Arduino;
class GhubMouse;
class KmboxAConnection;
class KmboxNetConnection;
class MakcuConnection;

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
        Arduino* arduinoConnection = nullptr,
        GhubMouse* gHubMouse = nullptr,
        KmboxAConnection* kmboxAConnection = nullptr,
        KmboxNetConnection* kmboxNetConnection = nullptr,
        MakcuConnection* makcuConnection = nullptr);
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
    void sendRawMove(int dx, int dy);
    void pressLeftButton();
    void releaseLeftButton();

    // Input device hot-swap.
    void setArduinoConnection(Arduino* arduino);
    void setKmboxAConnection(KmboxAConnection* kmbox_a);
    void setKmboxNetConnection(KmboxNetConnection* kmbox_net);
    void setMakcuConnection(MakcuConnection* makcu);
    void setGHubMouse(GhubMouse* ghub);

private:
    struct Move
    {
        int dx = 0;
        int dy = 0;
        std::chrono::steady_clock::time_point queued_at{};
    };

    void moveWorkerLoop();
    void queueMove(int dx, int dy);
    bool sendMovementToDriver(int dx, int dy);

    void sendLeftDownToDriver();
    void sendLeftUpToDriver();

    MouseRuntimeParams params_{};

    double screen_width_ = 320.0;
    double screen_height_ = 320.0;

    // Async driver dispatch.
    std::queue<Move> moveQueue_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    const size_t queueLimit_ = 5;
    std::thread moveWorker_;
    std::atomic<bool> workerStop_{ false };
    std::atomic<long long> appliedDx_{ 0 };
    std::atomic<long long> appliedDy_{ 0 };
    std::atomic<long long> lastLatencyUs_{ 0 };
    std::atomic<unsigned long long> failedMoves_{ 0 };

    Arduino* arduino_ = nullptr;
    KmboxAConnection* kmbox_a_ = nullptr;
    KmboxNetConnection* kmbox_net_ = nullptr;
    MakcuConnection* makcu_ = nullptr;
    GhubMouse* gHub_ = nullptr;
};

#endif // MOUSE_H
