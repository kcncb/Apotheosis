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

    // Smart trigger inputs.
    bool  smart_trigger_enabled = false;
    float smart_trigger_hit_scale_x = 0.60f;
    float smart_trigger_hit_scale_y = 0.60f;
    int   smart_trigger_reaction_ms = 40;
    int   smart_trigger_hold_ms = 45;
    int   smart_trigger_cooldown_ms = 55;
};

class MouseThread
{
public:
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

    // Called by mouse_thread_loop once per detection with the locked target's
    // OBSERVED aim anchor (pivot) and bbox half-width/half-height in detection
    // pixels. The triggerbot tests the live crosshair against this rectangle.
    void setLockedTargetBox(double pivotX, double pivotY,
                            double halfW, double halfH);

    // Drive the smart-trigger state machine.  Reads the locked bbox
    // (most recent setLockedTargetBox values) and the supplied crosshair
    // position, manages dwell / hold / cooldown, and writes LMB directly.
    // Call once per detection tick from the mouse loop.  When
    // smart_trigger_enabled is false this just releases any held button.
    void updateSmartTrigger(double crosshairX, double crosshairY);

    void forceTriggerRelease();

private:
    struct Move
    {
        int dx = 0;
        int dy = 0;
    };

    void moveWorkerLoop();
    void queueMove(int dx, int dy);
    void sendMovementToDriver(int dx, int dy);

    void sendLeftDownToDriver();
    void sendLeftUpToDriver();

    MouseRuntimeParams params_{};

    double screen_width_ = 320.0;
    double screen_height_ = 320.0;

    // Smart trigger geometry, set once per detection by the loop.
    std::atomic<double> trig_target_x_{ 0.0 };
    std::atomic<double> trig_target_y_{ 0.0 };
    std::atomic<double> trig_half_w_{ 0.0 };
    std::atomic<double> trig_half_h_{ 0.0 };

    // Auto-fire state machine.
    bool firing_active_ = false;
    bool on_target_ = false;
    std::chrono::steady_clock::time_point on_target_since_{};
    std::chrono::steady_clock::time_point fire_release_at_{};
    std::chrono::steady_clock::time_point fire_cooldown_until_{};

    // Async driver dispatch.
    std::queue<Move> moveQueue_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    const size_t queueLimit_ = 5;
    std::thread moveWorker_;
    std::atomic<bool> workerStop_{ false };

    Arduino* arduino_ = nullptr;
    KmboxAConnection* kmbox_a_ = nullptr;
    KmboxNetConnection* kmbox_net_ = nullptr;
    MakcuConnection* makcu_ = nullptr;
    GhubMouse* gHub_ = nullptr;
};

#endif // MOUSE_H
