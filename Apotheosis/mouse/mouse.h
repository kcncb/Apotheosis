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
#include <optional>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include "AimbotTarget.h"
#include "aim_kalman.h"
#include "pid_controller.h"

// Forward declarations so that mouse.h stays light.
class Arduino;
class GhubMouse;
class KmboxAConnection;
class KmboxNetConnection;
class MakcuConnection;

// Aggregate of every parameter the MouseThread honors when executing moves.
// When an aim hotkey is active with `override_mouse = true` the loop fills
// this out from the HotkeyProfile; otherwise it mirrors the global config.
struct MouseRuntimeParams
{
    int detection_resolution = 320;
    int fovX = 106;
    int fovY = 74;

    aim::PidGains pid{};

    // Optional second PID gain set used by the Flick/Track state machine.
    // When `flick_track_enabled` is false, `pid` is used unconditionally.
    bool          flick_track_enabled = false;
    aim::PidGains pid_track{};
    float         flick_track_threshold_px = 30.0f;
    float         flick_track_hysteresis_px = 8.0f;

    // Smart trigger inputs. Mirrors HotkeyProfile fields; MouseThread reads
    // them on each step to actually dispatch left-button down/up to the
    // active input driver. `g_smart_trigger_ready` continues to expose the
    // gate state for the UI indicator.
    bool  smart_trigger_enabled = false;
    float smart_trigger_hit_radius_frac = 0.55f;
    float smart_trigger_variance_max_px = 6.0f;
    int   smart_trigger_window_frames = 8;
    float smart_trigger_min_prob = 0.70f;
    int   smart_trigger_fire_duration_ms = 40;

    float predictionInterval = 0.0f;

    aim::AimKalmanSettings kalman{};
    bool kalman_compensate_detection_delay = true;
    float kalman_additional_prediction_ms = 0.0f;
    float kalman_reset_timeout_sec = 0.5f;

    // Snap-lock aim assist. 0 disables; 1 = maximum adhesion. See
    // HotkeyProfile::aim_lock_strength for the gain-shaping formula.
    float aim_lock_strength = 0.0f;
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

    // Apply new runtime parameters. Resets PID and Kalman whenever
    // non-trivial changes are detected so a hotkey swap starts from a clean
    // controller state.
    void updateParams(const MouseRuntimeParams& params);

    // Pivot coordinates are in detection-image space (same resolution as
    // screen_width/height). moveMouseToObservedTarget runs one PID step using
    // the observation and updates the Kalman model; moveMouseToPredictedTarget
    // extrapolates the Kalman model forward without feeding it any new data,
    // used while the detector is stuttering and Kalman fallback is enabled.
    void moveMouseToObservedTarget(double pivotX, double pivotY);
    bool moveMouseToPredictedTarget();
    void clearQueuedMoves();

    // Override the aim reference point (PID "center") for the next movement.
    // Coordinates are in detection-image space. Pass std::nullopt to revert
    // to the static image center. Fed by the crosshair color detector so the
    // aimbot chases the player's real reticle instead of the image midpoint.
    void setDynamicAimCenter(std::optional<std::pair<double, double>> center);

    void resetPrediction();
    void checkAndResetPredictions();
    void setTargetDetected(bool v) { target_detected_.store(v); }
    void setLastTargetTime(const std::chrono::steady_clock::time_point& t) { last_target_time_ = t; }

    // Kalman predictions for overlays that want to draw "future positions".
    std::vector<std::pair<double, double>> predictFuturePositions(double pivotX,
                                                                   double pivotY,
                                                                   int frames);
    void storeFuturePositions(const std::vector<std::pair<double, double>>& positions);
    void clearFuturePositions();
    std::vector<std::pair<double, double>> getFuturePositions();

    // Input device hot-swap.
    void setArduinoConnection(Arduino* arduino);
    void setKmboxAConnection(KmboxAConnection* kmbox_a);
    void setKmboxNetConnection(KmboxNetConnection* kmbox_net);
    void setMakcuConnection(MakcuConnection* makcu);
    void setGHubMouse(GhubMouse* ghub);

    std::mutex input_method_mutex;

private:
    struct Move
    {
        int dx = 0;
        int dy = 0;
    };

    void moveWorkerLoop();
    void queueMove(int dx, int dy);
    void sendMovementToDriver(int dx, int dy);

    double currentDetectionDelaySec() const;
    double currentPredictionLookaheadSec(double detectionDelaySec) const;

    // Pushes a PID step to the driver based on the current target position
    // in screen-space. Does NOT touch the Kalman model; the caller decides
    // whether the target position came from an observation or from an
    // extrapolation.
    void drivePidToTarget(double targetX, double targetY);

    // Update the smart-trigger classification given the current pivot and
    // the latest queued mouse step. Called from drivePidToTarget. Writes
    // through to g_smart_trigger_ready / g_smart_trigger_hit_prob (in
    // mouse.cpp).
    void updateSmartTrigger(double errPx, int dx, int dy);

public:
    // Called by mouse_thread_loop once per detection so the trigger logic
    // knows the locked target's bbox half-extent in pixel space (used as
    // the "hitbox" radius for the hit-probability heuristic). Pass 0 to
    // clear.
    void setLockedTargetBboxHalfExtent(double half_extent_px);

    // Force-release the auto-fire button if it's currently held. Called
    // from clearQueuedMoves() (hotkey lifted) and from the destructor so a
    // fire-in-progress can never outlive the controller.
    void forceTriggerRelease();

private:
    // Driver-agnostic left-button down / up. Picks the same driver
    // sendMovementToDriver does. Holds input_method_mutex.
    void sendLeftDownToDriver();
    void sendLeftUpToDriver();

    MouseRuntimeParams params_{};

    double screen_width_ = 320.0;
    double screen_height_ = 320.0;
    double center_x_ = 160.0;
    double center_y_ = 160.0;

    // Crosshair-override reference point, mutated from the mouse loop thread
    // before each moveMouseTo* call. Atomics keep the read inside
    // drivePidToTarget lock-free.
    std::atomic<bool> dynamic_center_valid_{ false };
    std::atomic<double> dynamic_center_x_{ 0.0 };
    std::atomic<double> dynamic_center_y_{ 0.0 };

    aim::PidController2D pid_{};

    // Flick/Track mode tracking. `pid_mode_track_` reflects which gain set
    // was applied on the previous step so the boundary uses hysteresis (not
    // a single threshold) to flip. Reset on parameter change so a hotkey
    // swap starts in Flick mode.
    bool pid_mode_track_ = false;

    // Smart trigger. Recent dx/dy steps in a fixed-capacity ring (sized by
    // params_.smart_trigger_window_frames). `locked_bbox_half_extent_px_`
    // is updated externally — atomic so we don't have to grab a mutex from
    // both the PID step and the loop.
    std::atomic<double> locked_bbox_half_extent_px_{ 0.0 };
    std::vector<int> recent_dx_;
    std::vector<int> recent_dy_;
    size_t recent_steps_head_ = 0;

    // Auto-fire state. firing_active_ is true between the down and the
    // scheduled up. fire_cooldown_until_ enforces the post-release refractory
    // period so duty cycle stays at 50%.
    bool firing_active_ = false;
    std::chrono::steady_clock::time_point fire_release_at_{};
    std::chrono::steady_clock::time_point fire_cooldown_until_{};

    aim::AimKalman2D targetKalman_{};
    aim::AimKalmanTelemetry lastKalmanTelemetry_{};
    double lastPredictionLookaheadSec_ = 0.0;

    std::chrono::steady_clock::time_point prev_time_{};
    bool has_prev_time_ = false;
    std::chrono::steady_clock::time_point last_target_time_ = std::chrono::steady_clock::now();
    std::atomic<bool> target_detected_{ false };

    std::vector<std::pair<double, double>> futurePositions_;
    std::mutex futurePositionsMutex_;

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
