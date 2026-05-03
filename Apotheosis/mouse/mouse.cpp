#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "mouse.h"
#include "capture.h"
#include "Apotheosis.h"
#include "Arduino.h"
#include "KmboxAConnection.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
#include "ghub.h"

namespace
{

MouseRuntimeParams sanitize(MouseRuntimeParams p)
{
    if (p.detection_resolution < 32) p.detection_resolution = 32;
    if (p.fovX < 1) p.fovX = 1;
    if (p.fovY < 1) p.fovY = 1;
    if (!(p.aim.speed_x >= 0.0)) p.aim.speed_x = 0.0;
    if (p.aim.speed_x > 2.0)     p.aim.speed_x = 2.0;
    if (!(p.aim.speed_y >= 0.0)) p.aim.speed_y = 0.0;
    if (p.aim.speed_y > 2.0)     p.aim.speed_y = 2.0;
    if (!(p.aim.lock_strength >= 0.0)) p.aim.lock_strength = 0.0;
    if (p.aim.lock_strength > 1.0)     p.aim.lock_strength = 1.0;
    if (p.smart_trigger_window_frames < 2) p.smart_trigger_window_frames = 2;
    if (p.smart_trigger_window_frames > 60) p.smart_trigger_window_frames = 60;
    if (!(p.smart_trigger_hit_radius_frac > 0.0f)) p.smart_trigger_hit_radius_frac = 0.0001f;
    if (p.smart_trigger_hit_radius_frac > 1.0f) p.smart_trigger_hit_radius_frac = 1.0f;
    if (!(p.smart_trigger_variance_max_px >= 0.0f)) p.smart_trigger_variance_max_px = 0.0f;
    if (!(p.smart_trigger_min_prob >= 0.0f)) p.smart_trigger_min_prob = 0.0f;
    if (p.smart_trigger_min_prob > 1.0f) p.smart_trigger_min_prob = 1.0f;
    if (p.smart_trigger_fire_duration_ms < 5)   p.smart_trigger_fire_duration_ms = 5;
    if (p.smart_trigger_fire_duration_ms > 1000) p.smart_trigger_fire_duration_ms = 1000;
    return p;
}

bool aim_gains_equal(const aim::AimGains& a, const aim::AimGains& b)
{
    return a.speed_x == b.speed_x
        && a.speed_y == b.speed_y
        && a.lock_strength == b.lock_strength;
}

} // namespace

// -------------------------------------------------------------------------
// Smart-trigger global state. Lives at namespace scope so the overlay can
// peek at it cheaply (atomic read, no lock). The trigger NEVER actuates the
// mouse — it's a hint surface for the user to wire to a virtual key on
// their hardware (kmbox / makcu) if they want.
// -------------------------------------------------------------------------
std::atomic<bool>   g_smart_trigger_ready{ false };
std::atomic<float>  g_smart_trigger_hit_prob{ 0.0f };
std::atomic<float>  g_smart_trigger_recent_variance_px{ 0.0f };

// -------------------------------------------------------------------------
// Aim telemetry. Read-only from the overlay; the controller publishes the
// latest crosshair-to-target error in detection pixels.
// -------------------------------------------------------------------------
std::atomic<float> g_pid_last_err_px{ 0.0f };

// Dynamic-FOV telemetry — published from mouse_thread_loop, consumed by
// the overlay to draw the aim region indicator.
std::atomic<float> g_dynamic_fov_radius_x_px{ 0.0f };
std::atomic<float> g_dynamic_fov_radius_y_px{ 0.0f };

MouseThread::MouseThread(
    const MouseRuntimeParams& params,
    Arduino* arduinoConnection,
    GhubMouse* gHubMouse,
    KmboxAConnection* kmboxAConnection,
    KmboxNetConnection* kmboxNetConnection,
    MakcuConnection* makcuConnection)
    : arduino_(arduinoConnection),
      kmbox_a_(kmboxAConnection),
      kmbox_net_(kmboxNetConnection),
      makcu_(makcuConnection),
      gHub_(gHubMouse)
{
    updateParams(params);
    last_target_time_ = std::chrono::steady_clock::now();
    moveWorker_ = std::thread(&MouseThread::moveWorkerLoop, this);
}

MouseThread::~MouseThread()
{
    // Critical: release any auto-fire that's still down BEFORE the worker
    // joins, otherwise a session shutdown mid-fire would leave the user's
    // gun glued to the trigger.
    forceTriggerRelease();
    workerStop_.store(true);
    queueCv_.notify_all();
    if (moveWorker_.joinable())
        moveWorker_.join();
}

void MouseThread::sendLeftDownToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (kmbox_net_)
        kmbox_net_->leftDown();
    else if (kmbox_a_)
        kmbox_a_->leftDown();
    else if (makcu_)
        makcu_->press(1);
    else if (arduino_)
        arduino_->press();
    else if (gHub_)
        gHub_->mouse_down(1);
    else
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &in, sizeof(INPUT));
    }
}

void MouseThread::sendLeftUpToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (kmbox_net_)
        kmbox_net_->leftUp();
    else if (kmbox_a_)
        kmbox_a_->leftUp();
    else if (makcu_)
        makcu_->release(1);
    else if (arduino_)
        arduino_->release();
    else if (gHub_)
        gHub_->mouse_up(1);
    else
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof(INPUT));
    }
}

void MouseThread::forceTriggerRelease()
{
    if (!firing_active_)
        return;
    sendLeftUpToDriver();
    firing_active_ = false;
    fire_release_at_ = {};
    g_smart_trigger_ready.store(false);
}

void MouseThread::updateParams(const MouseRuntimeParams& in)
{
    const auto sanitized = sanitize(in);
    const bool first_time = !(screen_width_ > 0.0);
    const bool resolution_changed = first_time ||
        sanitized.detection_resolution != params_.detection_resolution;
    const bool gains_changed = first_time ||
        !aim_gains_equal(sanitized.aim, params_.aim);
    const bool kalman_changed = first_time ||
        sanitized.kalman.enabled != params_.kalman.enabled ||
        sanitized.kalman.process_noise_position != params_.kalman.process_noise_position ||
        sanitized.kalman.process_noise_velocity != params_.kalman.process_noise_velocity ||
        sanitized.kalman.measurement_noise != params_.kalman.measurement_noise ||
        sanitized.kalman.velocity_damping != params_.kalman.velocity_damping ||
        sanitized.kalman.max_velocity != params_.kalman.max_velocity ||
        sanitized.kalman.warmup_frames != params_.kalman.warmup_frames;

    params_ = sanitized;

    screen_width_ = params_.detection_resolution;
    screen_height_ = params_.detection_resolution;
    center_x_ = screen_width_ * 0.5;
    center_y_ = screen_height_ * 0.5;

    aim_.setGains(params_.aim);
    if (first_time || gains_changed)
        aim_.reset();

    // Bezier 轨迹控制器:始终把最新参数推下去;速度从 aim 复制,曲线/跟随
    // 由 bezier_* 字段提供。参数变更不强制 reset 锚定 (会在下一次 engage
    // 条件满足时自然重锚),但在第一次或速度变化时清掉残留状态。
    {
        aim::BezierParams bp = params_.bezier;
        bp.speed_x = params_.aim.speed_x;
        bp.speed_y = params_.aim.speed_y;
        bezier_aim_.setParams(bp);
        if (first_time || gains_changed)
            bezier_aim_.reset();
    }

    // Resize the variance ring to match the current window.
    {
        const size_t cap = static_cast<size_t>(std::max(2, params_.smart_trigger_window_frames));
        if (recent_dx_.size() != cap)
        {
            recent_dx_.assign(cap, 0);
            recent_dy_.assign(cap, 0);
            recent_steps_head_ = 0;
        }
    }
    g_smart_trigger_ready.store(false);
    g_smart_trigger_hit_prob.store(0.0f);
    g_smart_trigger_recent_variance_px.store(0.0f);

    // Param change implies the user may have toggled the trigger or swapped
    // hotkey — drop any in-progress fire so we don't carry it across
    // contexts.
    forceTriggerRelease();
    fire_cooldown_until_ = {};

    targetKalman_.setSettings(params_.kalman);
    if (first_time || kalman_changed || resolution_changed)
    {
        targetKalman_.reset();
        lastKalmanTelemetry_ = {};
        lastPredictionLookaheadSec_ = 0.0;
        has_prev_time_ = false;
    }
}

void MouseThread::queueMove(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    std::lock_guard<std::mutex> lg(queueMtx_);
    if (moveQueue_.size() >= queueLimit_)
        moveQueue_.pop();
    moveQueue_.push({ dx, dy });
    queueCv_.notify_one();
}

void MouseThread::moveWorkerLoop()
{
    try
    {
        while (!workerStop_.load())
        {
            std::unique_lock<std::mutex> ul(queueMtx_);
            queueCv_.wait(ul, [&] { return workerStop_.load() || !moveQueue_.empty(); });
            while (!moveQueue_.empty())
            {
                Move m = moveQueue_.front();
                moveQueue_.pop();
                ul.unlock();
                sendMovementToDriver(m.dx, m.dy);
                ul.lock();
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Mouse] Move worker crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[Mouse] Move worker crashed: unknown exception." << std::endl;
    }
}

void MouseThread::sendMovementToDriver(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (kmbox_net_)
    {
        kmbox_net_->move(dx, dy);
    }
    else if (kmbox_a_)
    {
        kmbox_a_->move(dx, dy);
    }
    else if (makcu_)
    {
        makcu_->move(dx, dy);
    }
    else if (arduino_)
    {
        arduino_->move(dx, dy);
    }
    else if (gHub_)
    {
        gHub_->mouse_xy(dx, dy);
    }
    else
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in, sizeof(INPUT));
    }
}

double MouseThread::currentDetectionDelaySec() const
{
    double detectionDelaySec = 0.05;
    if (g_detector)
        detectionDelaySec = g_detector->lastInferenceTime().count() * 0.001;
    if (!std::isfinite(detectionDelaySec))
        detectionDelaySec = 0.05;
    return std::clamp(detectionDelaySec, 0.0, 0.2);
}

double MouseThread::currentPredictionLookaheadSec(double detectionDelaySec) const
{
    double lookahead = std::max(0.0, static_cast<double>(params_.predictionInterval));
    if (params_.kalman_compensate_detection_delay)
        lookahead += std::max(0.0, detectionDelaySec);
    lookahead += static_cast<double>(params_.kalman_additional_prediction_ms) * 0.001;
    return std::clamp(lookahead, 0.0, 1.5);
}

void MouseThread::driveAimToTarget(double targetX, double targetY,
                                   double pivotX, double pivotY,
                                   double lock_attenuation)
{
    // err = target − pivot. Pivot is the visible reticle (crosshair-color
    // detected) when fed by the loop, otherwise screen centre. Recoil shows
    // up here as an extra Y component on err, and the no-clamp controller
    // is free to issue whatever counter-move the gains produce.
    double errX = targetX - pivotX;
    double errY = targetY - pivotY;
    const double errPx = std::hypot(errX, errY);
    g_pid_last_err_px.store(static_cast<float>(errPx));

    prev_time_ = std::chrono::steady_clock::now();
    has_prev_time_ = true;

    // Bezier 轨迹模式:用曲线控制器把直线 err 替换成曲线偏置后的等效 err,
    // 后续仍走同一份 P 控制器残差累积。锁定 ID 这一层不可见,传 -1 由
    // controller 用距离阈值兜底重锚。
    if (params_.bezier_enabled)
    {
        auto [bx, by] = bezier_aim_.computeError(targetX, targetY,
                                                 pivotX, pivotY, -1);
        errX = bx;
        errY = by;
    }

    aim::AimGains g = params_.aim;
    g.lock_attenuation = std::clamp(lock_attenuation, 0.0, 1.0);
    aim_.setGains(g);
    auto [mx, my] = aim_.step(errX, errY, screen_width_);
    queueMove(mx, my);

    updateSmartTrigger(errPx, mx, my);
}

void MouseThread::setLockedTargetBboxHalfExtent(double half_extent_px)
{
    locked_bbox_half_extent_px_.store(half_extent_px);
}

void MouseThread::updateSmartTrigger(double errPx, int dx, int dy)
{
    const auto now = std::chrono::steady_clock::now();

    // Trigger off-switch: any time the toggle is false we must release if
    // we're still mid-fire. Same is true if the locked target evaporated
    // (half_extent goes to 0).
    if (!params_.smart_trigger_enabled)
    {
        if (firing_active_)
            forceTriggerRelease();
        g_smart_trigger_ready.store(false);
        g_smart_trigger_hit_prob.store(0.0f);
        return;
    }

    // Push latest step into the variance ring.
    if (recent_dx_.empty() || recent_dy_.empty())
    {
        const size_t cap = static_cast<size_t>(std::max(2, params_.smart_trigger_window_frames));
        recent_dx_.assign(cap, 0);
        recent_dy_.assign(cap, 0);
        recent_steps_head_ = 0;
    }
    const size_t cap = recent_dx_.size();
    recent_dx_[recent_steps_head_] = dx;
    recent_dy_[recent_steps_head_] = dy;
    recent_steps_head_ = (recent_steps_head_ + 1) % cap;

    // RMS magnitude over the window. We use combined dx/dy because either
    // axis being noisy is a valid "gun isn't settled" signal.
    double sumsq = 0.0;
    for (size_t i = 0; i < cap; ++i)
    {
        const double v = std::hypot(static_cast<double>(recent_dx_[i]),
                                    static_cast<double>(recent_dy_[i]));
        sumsq += v * v;
    }
    const double rms = std::sqrt(sumsq / static_cast<double>(cap));
    g_smart_trigger_recent_variance_px.store(static_cast<float>(rms));

    // Hit probability heuristic: 1 when crosshair is dead-on, 0 when it is
    // outside an effective hitbox of half_extent * hit_radius_frac. Linear
    // ramp in between. Without a locked bbox we treat hit prob as 0 — the
    // trigger will never fire on an empty target, which is the safe default.
    const double half = locked_bbox_half_extent_px_.load();
    double prob = 0.0;
    if (half > 0.5)
    {
        const double effective = half * static_cast<double>(params_.smart_trigger_hit_radius_frac);
        if (effective > 0.5)
        {
            const double inside = 1.0 - std::clamp(errPx / effective, 0.0, 1.0);
            prob = inside;
        }
    }
    g_smart_trigger_hit_prob.store(static_cast<float>(prob));

    // Step 1: if we're already firing, release when the hold time has
    // elapsed. We always release on schedule — even if the gates are still
    // open — so duty cycle stays bounded by the cooldown logic below. This
    // turns continuous "ready" into a controlled tap-tap-tap rather than a
    // glued button.
    if (firing_active_ && now >= fire_release_at_)
    {
        sendLeftUpToDriver();
        firing_active_ = false;
        const auto hold = std::chrono::milliseconds(params_.smart_trigger_fire_duration_ms);
        // Refractory == hold time → 50% max duty cycle. Keeps full-auto
        // mode realistic and gives semi-auto plenty of recovery between
        // taps.
        fire_cooldown_until_ = now + hold;
    }

    const bool prob_ok = prob >= static_cast<double>(params_.smart_trigger_min_prob);
    const bool variance_ok = rms <= static_cast<double>(params_.smart_trigger_variance_max_px);
    const bool gates_open = prob_ok && variance_ok;

    // Step 2: open a new fire window if all gates are aligned and the
    // cooldown has elapsed. The aim hotkey gating is implicit — this
    // function is only called from driveAimToTarget, which only runs while
    // a hotkey is active. Hotkey release calls clearQueuedMoves →
    // forceTriggerRelease, so an in-flight fire is always cleaned up.
    if (gates_open && !firing_active_ && now >= fire_cooldown_until_)
    {
        sendLeftDownToDriver();
        firing_active_ = true;
        fire_release_at_ = now + std::chrono::milliseconds(params_.smart_trigger_fire_duration_ms);
    }

    // `g_smart_trigger_ready` reflects the actual button state so the UI
    // LED matches reality during the hold+cooldown cycle. Gate state is
    // reconstructable from prob/rms which are separate atomics.
    g_smart_trigger_ready.store(firing_active_);
}

void MouseThread::moveMouseToObservedTarget(double targetPivotX, double targetPivotY,
                                             double crosshairX, double crosshairY)
{
    std::lock_guard<std::recursive_mutex> lg(input_method_mutex);

    const auto now = std::chrono::steady_clock::now();
    double dt = 1.0 / 120.0;
    if (has_prev_time_)
        dt = std::chrono::duration<double>(now - prev_time_).count();
    if (!(dt > 1e-6)) dt = 1e-6;
    if (dt > 0.25) dt = 0.25;

    const double detectionDelaySec = currentDetectionDelaySec();
    const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
    lastPredictionLookaheadSec_ = lookaheadSec;

    targetKalman_.setSettings(params_.kalman);
    lastKalmanTelemetry_ = targetKalman_.update(targetPivotX, targetPivotY, dt, lookaheadSec);

    double targetX = targetPivotX;
    double targetY = targetPivotY;
    if (params_.kalman.enabled && lastKalmanTelemetry_.initialized)
    {
        if (std::isfinite(lastKalmanTelemetry_.predicted_x))
            targetX = lastKalmanTelemetry_.predicted_x;
        if (std::isfinite(lastKalmanTelemetry_.predicted_y))
            targetY = lastKalmanTelemetry_.predicted_y;
    }

    driveAimToTarget(targetX, targetY, crosshairX, crosshairY);
}

void MouseThread::moveMouseUsingLastTarget(double targetX, double targetY,
                                            double crosshairX, double crosshairY)
{
    std::lock_guard<std::recursive_mutex> lg(input_method_mutex);
    // Path B (recoil-only re-push using stale target). Crosshair-color saw
    // the reticle move because of recoil, but the target hasn't actually
    // moved — feeding full lock-snap here amplifies muzzle-jitter into a
    // visible shake. Knock the lock multiplier down ~35% for this single
    // step so corrections rely mostly on the proportional speed term.
    constexpr double kPathBLockAttenuation = 0.65;
    driveAimToTarget(targetX, targetY, crosshairX, crosshairY,
                     kPathBLockAttenuation);
}

bool MouseThread::moveMouseToPredictedTarget(double crosshairX, double crosshairY)
{
    if (!params_.kalman.enabled)
        return false;

    std::lock_guard<std::recursive_mutex> lg(input_method_mutex);

    if (!targetKalman_.initialized())
        return false;

    const auto now = std::chrono::steady_clock::now();
    const double sinceTarget = std::chrono::duration<double>(now - last_target_time_).count();
    if (sinceTarget > params_.kalman_reset_timeout_sec)
        return false;

    const double detectionDelaySec = currentDetectionDelaySec();
    const double baseLookahead = currentPredictionLookaheadSec(detectionDelaySec);
    const double lookahead = std::clamp(baseLookahead + sinceTarget, 0.0, 1.5);
    auto predicted = targetKalman_.predict(lookahead);
    if (!std::isfinite(predicted.first) || !std::isfinite(predicted.second))
        return false;

    // Same Path B reasoning as moveMouseUsingLastTarget: this is a re-push
    // driven by a fresh crosshair snapshot or stale-target Kalman extrapolation,
    // not a fresh detection observation, so soften the lock boost.
    constexpr double kPathBLockAttenuation = 0.65;
    driveAimToTarget(predicted.first, predicted.second,
                     crosshairX, crosshairY, kPathBLockAttenuation);
    return true;
}

void MouseThread::clearQueuedMoves()
{
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        std::queue<Move> empty;
        moveQueue_.swap(empty);
    }
    aim_.reset();
    bezier_aim_.reset();

    // Hotkey lifted (or anything else that pre-empts pursuit) — make sure
    // we never leave the auto-fire button stuck down. The cooldown stays in
    // place so re-acquiring the hotkey doesn't immediately re-fire.
    forceTriggerRelease();
}

void MouseThread::resetPrediction()
{
    clearQueuedMoves();
    has_prev_time_ = false;
    targetKalman_.reset();
    lastKalmanTelemetry_ = {};
    lastPredictionLookaheadSec_ = 0.0;
    target_detected_.store(false);
}

void MouseThread::checkAndResetPredictions()
{
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_target_time_).count();
    const double timeoutSec = std::clamp(static_cast<double>(params_.kalman_reset_timeout_sec), 0.05, 3.0);

    if (elapsed > timeoutSec && target_detected_.load())
        resetPrediction();

    // Trigger watchdog — runs every loop iteration regardless of whether
    // PID stepped this frame. Guarantees a scheduled release happens even
    // when detections briefly stop arriving (e.g. target went behind cover
    // one frame after the press). Without this, a stall would leave the
    // button held until the hotkey is lifted.
    if (firing_active_ && now >= fire_release_at_)
    {
        sendLeftUpToDriver();
        firing_active_ = false;
        const auto hold = std::chrono::milliseconds(params_.smart_trigger_fire_duration_ms);
        fire_cooldown_until_ = now + hold;
        g_smart_trigger_ready.store(false);
    }
}

std::vector<std::pair<double, double>> MouseThread::predictFuturePositions(double pivotX,
                                                                            double pivotY,
                                                                            int frames)
{
    (void)pivotX;
    (void)pivotY;

    std::vector<std::pair<double, double>> result;
    if (frames <= 0)
        return result;

    result.reserve(static_cast<size_t>(frames));

    const double fixedFps = 30.0;
    const double frame_time = 1.0 / fixedFps;

    targetKalman_.setSettings(params_.kalman);
    if (targetKalman_.initialized())
    {
        const double detectionDelaySec = currentDetectionDelaySec();
        const double baseLookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        for (int i = 1; i <= frames; ++i)
        {
            const double t = baseLookaheadSec + frame_time * i;
            auto predicted = targetKalman_.predict(t);
            if (!std::isfinite(predicted.first) || !std::isfinite(predicted.second))
                continue;
            result.push_back(predicted);
        }
    }

    return result;
}

void MouseThread::storeFuturePositions(const std::vector<std::pair<double, double>>& positions)
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex_);
    futurePositions_ = positions;
}

void MouseThread::clearFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex_);
    futurePositions_.clear();
}

std::vector<std::pair<double, double>> MouseThread::getFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex_);
    return futurePositions_;
}

void MouseThread::setArduinoConnection(Arduino* newArduino)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    arduino_ = newArduino;
}

void MouseThread::setKmboxAConnection(KmboxAConnection* newKmbox_a)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    kmbox_a_ = newKmbox_a;
}

void MouseThread::setKmboxNetConnection(KmboxNetConnection* newKmbox_net)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    kmbox_net_ = newKmbox_net;
}

void MouseThread::setMakcuConnection(MakcuConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_ = newMakcu;
}

void MouseThread::setGHubMouse(GhubMouse* newGHub)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    gHub_ = newGHub;
}
