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

    // 经典离散 PID,X / Y 轴各一套独立的 P/I/D(见 pid_controller.h)。误差直接喂
    // 控制器,输出 lround 后送驱动。鲁棒处理(不完全微分/反算抗饱和/积分分离/微分限幅)
    // 全部内部自适应,不暴露;每轴对外仍只有 P/I/D 三个参数。
    aim::PidGains pid_x{};
    aim::PidGains pid_y{};

    // Smart trigger inputs. Mirrors HotkeyProfile fields; MouseThread runs a
    // self-contained geometric triggerbot state machine off these and
    // dispatches left-button down/up to the active input driver.
    // `g_smart_trigger_ready` exposes the firing state for the UI indicator.
    bool  smart_trigger_enabled = false;
    float smart_trigger_hit_scale_x = 0.60f;
    float smart_trigger_hit_scale_y = 0.60f;
    int   smart_trigger_reaction_ms = 40;
    int   smart_trigger_hold_ms = 45;
    int   smart_trigger_cooldown_ms = 55;

    float predictionInterval = 0.0f;

    // 卡尔曼:仅 启用 / 平滑度 / 预测提前量 三项(见 aim_kalman.h)。下面三个
    // 时序相关项不再暴露给用户,build_params 固定为合理默认。
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
    // centre; pass an explicit pivot when crosshair-color is active. dt is
    // the wall-clock seconds since the previous PID step on this thread.
    // `outputScale` attenuates the final mouse step (after gains/lock boost).
    // Path A (fresh observation) uses 1.0; Path B recoil-only re-pushes pass a
    // <1 value to damp close-range jitter from feeding a stale target at full
    // gain — there is no landing-zone state machine in the PID to do this.
    void driveAimToTarget(double targetX, double targetY,
                          double pivotX, double pivotY,
                          double outputScale = 1.0);

    // Run one step of the geometric triggerbot state machine. `crosshairX/Y`
    // is where the reticle currently sits (detection-image space). The target
    // anchor + bbox half-extents come from the atomics set by the loop. Called
    // from driveAimToTarget on every controller step. Writes through to
    // g_smart_trigger_ready / g_smart_trigger_hit_prob (in mouse.cpp).
    void updateSmartTrigger(double crosshairX, double crosshairY);

public:
    // Called by mouse_thread_loop once per detection so the Kalman filter can
    // scale its measurement noise by target size. Pass 0 to clear.
    void setLockedTargetBboxHalfExtent(double half_extent_px);

    // Called by mouse_thread_loop once per detection with the locked target's
    // OBSERVED aim anchor (pivot) and bbox half-width/half-height in detection
    // pixels. The triggerbot tests the live crosshair against this rectangle.
    // Pass (_, _, 0, 0) to clear — the trigger then never fires.
    void setLockedTargetBox(double pivotX, double pivotY,
                            double halfW, double halfH);

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

    aim::PidController2D pid_{};

    // `locked_bbox_half_extent_px_` (short-side/2) drives Kalman measurement
    // noise scaling. Atomic so the PID step and the loop don't contend.
    std::atomic<double> locked_bbox_half_extent_px_{ 0.0 };

    // Smart trigger geometry, set once per detection by the loop (atomic so
    // the controller step can read without a lock). The target anchor is the
    // OBSERVED pivot, never the Kalman prediction. half_w/half_h == 0 means
    // "no locked target" and the trigger holds fire.
    std::atomic<double> trig_target_x_{ 0.0 };
    std::atomic<double> trig_target_y_{ 0.0 };
    std::atomic<double> trig_half_w_{ 0.0 };
    std::atomic<double> trig_half_h_{ 0.0 };

    // Auto-fire state machine. firing_active_ is true between the press and
    // the scheduled release. on_target_since_ timestamps when the crosshair
    // first entered the hit region (reset whenever it leaves) so the reaction
    // dwell can be measured. fire_cooldown_until_ enforces the post-release
    // refractory period.
    bool firing_active_ = false;
    bool on_target_ = false;
    std::chrono::steady_clock::time_point on_target_since_{};
    std::chrono::steady_clock::time_point fire_release_at_{};
    std::chrono::steady_clock::time_point fire_cooldown_until_{};

    aim::AimKalman2D targetKalman_{};
    aim::AimKalmanTelemetry lastKalmanTelemetry_{};
    double lastPredictionLookaheadSec_ = 0.0;

    std::chrono::steady_clock::time_point prev_time_{};
    bool has_prev_time_ = false;
    // Kalman 用的独立时钟:只在 Path A(moveMouseToObservedTarget,真有新测量
    // 时)前进。和 prev_time_ 分开,否则 Path B 的重推/外推会把 prev_time_ 推到
    // 当前,让下一次 Path A 拿到一个被严重低估的 dt,导致 kalman 速度估计长期≈0
    // (现象=开/关卡尔曼效果差不多)。
    std::chrono::steady_clock::time_point kalman_prev_time_{};
    bool has_kalman_prev_time_ = false;
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
