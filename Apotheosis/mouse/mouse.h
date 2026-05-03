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

#include "AimbotTarget.h"
#include "aim_controller.h"
#include "aim_kalman.h"
#include "bezier_aim_controller.h"

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

    aim::AimGains aim{};

    // 瞄准曲线 (Bezier 轨迹模式)。bezier_enabled = false 时所有相关字段
    // 静默忽略,driveAimToTarget 沿用现有 Direct 路径。
    bool bezier_enabled = false;
    aim::BezierParams bezier{};

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

    // Apply new runtime parameters. Resets the aim controller and Kalman
    // whenever non-trivial changes are detected so a hotkey swap starts
    // from a clean controller state.
    void updateParams(const MouseRuntimeParams& params);

    // Pivot coordinates are in detection-image space (same resolution as
    // screen_width/height). moveMouseToObservedTarget runs one controller
    // step using the observation and updates the Kalman model;
    // moveMouseToPredictedTarget extrapolates the Kalman model forward
    // without feeding it any new data, used while the detector is
    // stuttering and Kalman fallback is enabled.
    // `targetPivot{X,Y}` is the head/body anchor the tracker chose. `crosshair{X,Y}`
    // is where the visible reticle is right now — pass detection-image centre
    // when crosshair-color is off / stale, otherwise the detected position.
    void moveMouseToObservedTarget(double targetPivotX, double targetPivotY,
                                   double crosshairX, double crosshairY);
    bool moveMouseToPredictedTarget(double crosshairX, double crosshairY);

    // Push a controller step using the supplied target/pivot WITHOUT
    // touching the Kalman model. Used by the loop to react to a fresh
    // crosshair snapshot between detection events: the target hasn't
    // moved (no new observation) but the visible reticle did (recoil),
    // so err must be recomputed and a corrective move issued. Feeding
    // this re-push into Kalman would double-count the same observation.
    void moveMouseUsingLastTarget(double targetX, double targetY,
                                  double crosshairX, double crosshairY);
    void clearQueuedMoves();

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

    // Recursive: a high-level aim op (moveMouseToObservedTarget, etc.) holds
    // this lock while driveAimToTarget → updateSmartTrigger may chain into
    // sendLeft{Down,Up}ToDriver, which re-enters the same lock on the same
    // thread. A non-recursive mutex was UB and crashed on fire.
    std::recursive_mutex input_method_mutex;

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

    // Pushes a controller step to the driver based on the current target
    // position in screen-space. Does NOT touch the Kalman model; the caller
    // decides whether the target position came from an observation or from
    // an extrapolation. The pivot (crosshair) defaults to detection-image
    // centre; pass an explicit pivot when crosshair-color is active.
    void driveAimToTarget(double targetX, double targetY,
                          double pivotX, double pivotY,
                          double lock_attenuation = 1.0);

    // Update the smart-trigger classification given the current pivot and
    // the latest queued mouse step. Called from driveAimToTarget. Writes
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

    aim::AimController aim_{};
    aim::BezierTrajectoryController bezier_aim_{};

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
