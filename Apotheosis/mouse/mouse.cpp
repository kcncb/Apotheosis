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
    if (!(p.pid_x.p >= 0.0)) p.pid_x.p = 0.0;
    if (!(p.pid_x.i >= 0.0)) p.pid_x.i = 0.0;
    if (!(p.pid_x.d >= 0.0)) p.pid_x.d = 0.0;
    if (!(p.pid_y.p >= 0.0)) p.pid_y.p = 0.0;
    if (!(p.pid_y.i >= 0.0)) p.pid_y.i = 0.0;
    if (!(p.pid_y.d >= 0.0)) p.pid_y.d = 0.0;
    p.smart_trigger_hit_scale_x = std::clamp(p.smart_trigger_hit_scale_x, 0.05f, 1.0f);
    p.smart_trigger_hit_scale_y = std::clamp(p.smart_trigger_hit_scale_y, 0.05f, 1.0f);
    p.smart_trigger_reaction_ms = std::clamp(p.smart_trigger_reaction_ms, 0, 1000);
    p.smart_trigger_hold_ms     = std::clamp(p.smart_trigger_hold_ms,     5, 5000);
    p.smart_trigger_cooldown_ms = std::clamp(p.smart_trigger_cooldown_ms, 0, 5000);
    return p;
}

bool pid_gains_equal(const aim::PidGains& a, const aim::PidGains& b)
{
    return a.p == b.p && a.i == b.i && a.d == b.d;
}

} // namespace

// -------------------------------------------------------------------------
// Smart-trigger telemetry. Lives at namespace scope so the overlay can peek
// at it cheaply (atomic read, no lock). The trigger DOES actuate the bound
// input device (left button) — these mirror its state for the UI:
//   g_smart_trigger_ready              -> true while the button is held down
//   g_smart_trigger_hit_prob           -> on-target fraction [0,1] (1 = dead-centre)
//   g_smart_trigger_recent_variance_px -> current on-target dwell time in ms
// -------------------------------------------------------------------------
std::atomic<bool>   g_smart_trigger_ready{ false };
std::atomic<float>  g_smart_trigger_hit_prob{ 0.0f };
std::atomic<float>  g_smart_trigger_recent_variance_px{ 0.0f };

// -------------------------------------------------------------------------
// Flick / Track telemetry. Read-only from the overlay; the controller
// publishes the latest crosshair-to-target error and which gain set
// (Flick vs Track) is active so the user can tune the threshold.
// -------------------------------------------------------------------------
std::atomic<float> g_pid_last_err_px{ 0.0f };
// false = Flick(远/快甩增益), true = Track(近/稳跟增益)。供 UI 显示。
std::atomic<bool>  g_pid_mode_track{ false };
std::atomic<bool>  g_threat_depth_required{ false };

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
    // Always drop the dwell so a re-acquire must re-satisfy the reaction time.
    on_target_ = false;
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
        !pid_gains_equal(sanitized.pid_x, params_.pid_x) ||
        !pid_gains_equal(sanitized.pid_y, params_.pid_y);
    const bool kalman_changed = first_time ||
        sanitized.kalman.enabled    != params_.kalman.enabled ||
        sanitized.kalman.smoothness != params_.kalman.smoothness ||
        sanitized.kalman.lead       != params_.kalman.lead;

    params_ = sanitized;

    screen_width_ = params_.detection_resolution;
    screen_height_ = params_.detection_resolution;
    center_x_ = screen_width_ * 0.5;
    center_y_ = screen_height_ * 0.5;

    // 积分分离阈值与输出饱和限现由 PidController2D 内部按画面尺度自算(见 pid_controller.h),
    // 不再在此设置。X / Y 各一套独立增益。
    pid_.setScreenExtent(screen_width_);
    pid_.setGains(params_.pid_x, params_.pid_y);
    if (first_time || gains_changed)
        pid_.reset();

    g_smart_trigger_ready.store(false);
    g_smart_trigger_hit_prob.store(0.0f);
    g_smart_trigger_recent_variance_px.store(0.0f);

    // Param change implies the user may have toggled the trigger or swapped
    // hotkey — drop any in-progress fire and reset the dwell so we don't
    // carry trigger state across contexts.
    forceTriggerRelease();
    fire_cooldown_until_ = {};
    on_target_ = false;

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
                                   double outputScale)
{
    // err = target − pivot. Pivot is the visible reticle (crosshair-color
    // detected) when fed by the loop, otherwise detection-image centre.
    // Recoil shows up here as an extra Y component on err, and the PID is
    // free to issue whatever counter-move the gains produce.
    const double errX = targetX - pivotX;
    const double errY = targetY - pivotY;
    const double errPx = std::hypot(errX, errY);
    g_pid_last_err_px.store(static_cast<float>(errPx));

    // Wall-clock dt since the last PID step on this thread, so derivative /
    // integral terms have correct units across cadence changes (Path A
    // detection events vs Path B crosshair re-pushes run at different rates).
    const auto now = std::chrono::steady_clock::now();
    double dt = 1.0 / 120.0;
    if (has_prev_time_)
        dt = std::chrono::duration<double>(now - prev_time_).count();
    prev_time_ = now;
    has_prev_time_ = true;
    if (!(dt > 1e-6)) dt = 1e-6;
    if (dt > 0.25)    dt = 0.25;

    // X / Y 各一套独立 PID(每轴 P/I/D)。卡尔曼负责"目标在哪/将到哪",PID 只负责把
    // 准星平稳驱到该点。鲁棒处理全部内部,见 pid_controller.h。
    pid_.setGains(params_.pid_x, params_.pid_y);
    auto [rawX, rawY] = pid_.step(errX, errY, dt);

    // Path B passes outputScale < 1 to damp close-range jitter (stale target +
    // recoil fed at full gain); Path A uses 1.0.
    rawX *= outputScale;
    rawY *= outputScale;

    const int mx = static_cast<int>(std::lround(rawX));
    const int my = static_cast<int>(std::lround(rawY));
    queueMove(mx, my);

    // The triggerbot decides purely on crosshair-vs-target geometry, using
    // the OBSERVED target (set via setLockedTargetBox) rather than this
    // step's PID target — which may be a Kalman lead/prediction and would
    // desync the fire decision. Pass the crosshair (pivotX/pivotY).
    updateSmartTrigger(pivotX, pivotY);
}

void MouseThread::setLockedTargetBboxHalfExtent(double half_extent_px)
{
    locked_bbox_half_extent_px_.store(half_extent_px);
}

void MouseThread::setLockedTargetBox(double pivotX, double pivotY,
                                     double halfW, double halfH)
{
    trig_target_x_.store(pivotX);
    trig_target_y_.store(pivotY);
    trig_half_w_.store(halfW);
    trig_half_h_.store(halfH);
}

void MouseThread::updateSmartTrigger(double crosshairX, double crosshairY)
{
    const auto now = std::chrono::steady_clock::now();

    // Off-switch: release immediately if we were mid-fire and clear telemetry.
    if (!params_.smart_trigger_enabled)
    {
        forceTriggerRelease();
        g_smart_trigger_ready.store(false);
        g_smart_trigger_hit_prob.store(0.0f);
        g_smart_trigger_recent_variance_px.store(0.0f);
        return;
    }

    // --- On-target test --------------------------------------------------
    // Rectangular hit region centred on the observed target anchor, scaled
    // per-axis by hit_scale_{x,y}. half_w/half_h == 0 ⇒ no locked target.
    const double halfW = trig_half_w_.load();
    const double halfH = trig_half_h_.load();
    const double tolX  = halfW * static_cast<double>(params_.smart_trigger_hit_scale_x);
    const double tolY  = halfH * static_cast<double>(params_.smart_trigger_hit_scale_y);

    bool on_target = false;
    double hit_frac = 0.0; // 1 = dead-centre, 0 = at/over the edge
    if (tolX > 0.5 && tolY > 0.5)
    {
        const double ex = std::abs(crosshairX - trig_target_x_.load());
        const double ey = std::abs(crosshairY - trig_target_y_.load());
        on_target = (ex <= tolX) && (ey <= tolY);
        const double worst = std::max(ex / tolX, ey / tolY);
        hit_frac = std::clamp(1.0 - worst, 0.0, 1.0);
    }
    g_smart_trigger_hit_prob.store(static_cast<float>(hit_frac));

    // --- Dwell bookkeeping ----------------------------------------------
    // Track how long the crosshair has continuously sat inside the region.
    if (on_target)
    {
        if (!on_target_)
        {
            on_target_ = true;
            on_target_since_ = now;
        }
    }
    else
    {
        on_target_ = false;
    }
    const double dwell_ms = on_target_
        ? std::chrono::duration<double, std::milli>(now - on_target_since_).count()
        : 0.0;
    g_smart_trigger_recent_variance_px.store(static_cast<float>(dwell_ms));

    // --- State machine ---------------------------------------------------
    // 1) Release when the hold time elapses, then start the cooldown.
    if (firing_active_ && now >= fire_release_at_)
    {
        sendLeftUpToDriver();
        firing_active_ = false;
        fire_cooldown_until_ =
            now + std::chrono::milliseconds(params_.smart_trigger_cooldown_ms);
    }

    // 2) Start a tap once the crosshair has dwelt on target long enough and
    //    the cooldown has elapsed. Hotkey gating is implicit (this only runs
    //    while a hotkey is active; release force-clears any in-flight fire).
    const bool dwell_ok = on_target_
        && dwell_ms >= static_cast<double>(params_.smart_trigger_reaction_ms);
    if (dwell_ok && !firing_active_ && now >= fire_cooldown_until_)
    {
        sendLeftDownToDriver();
        firing_active_ = true;
        fire_release_at_ =
            now + std::chrono::milliseconds(params_.smart_trigger_hold_ms);
    }

    g_smart_trigger_ready.store(firing_active_);
}

void MouseThread::moveMouseToObservedTarget(double targetPivotX, double targetPivotY,
                                             double crosshairX, double crosshairY)
{
    std::lock_guard<std::recursive_mutex> lg(input_method_mutex);

    // Kalman dt 必须用"上次 kalman 更新到现在"的真实时间,而不是 prev_time_
    // (后者每次 driveAimToTarget 都会刷新,包括 Path B 的重推/外推路径)。
    // 否则:Path B 在两次检测之间触发一次 -> prev_time_ 被推到几毫秒前 ->
    // 下一次 Path A 喂给 kalman 的 dt 远小于真实测量间隔 -> 状态外推被严重
    // 低估、测量新息巨大但 k1 因 p10 还没起来近 0、velocity 几乎不更新 ->
    // 稳态下 velocity≈0、predict() ≈ measurement,表现为"开/关 kalman 效果差不多"。
    const auto now = std::chrono::steady_clock::now();
    double dt = 1.0 / 120.0;
    if (has_kalman_prev_time_)
        dt = std::chrono::duration<double>(now - kalman_prev_time_).count();
    kalman_prev_time_ = now;
    has_kalman_prev_time_ = true;
    if (!(dt > 1e-6)) dt = 1e-6;
    if (dt > 0.25) dt = 0.25;

    const double detectionDelaySec = currentDetectionDelaySec();
    const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
    lastPredictionLookaheadSec_ = lookaheadSec;

    targetKalman_.setSettings(params_.kalman);
    // 小目标/远距离 bbox 更小 → 测量更抖 → 让 Kalman 自动加重平滑。
    targetKalman_.setMeasurementHalfExtent(locked_bbox_half_extent_px_.load());
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
    // Path B (recoil-only re-push using stale target): crosshair moved due to
    // recoil, the target hasn't. Feed the PID at reduced output so close-range
    // jitter from re-pushing a stale target at full gain doesn't shake the
    // reticle (the PID has no landing-zone deadband of its own).
    constexpr double kPathBOutputAttenuation = 0.65;
    driveAimToTarget(targetX, targetY, crosshairX, crosshairY,
                     kPathBOutputAttenuation);
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

    // Same as moveMouseUsingLastTarget: re-pushing an extrapolated target, so
    // attenuate the output to damp close-range jitter.
    constexpr double kPathBOutputAttenuation = 0.65;
    driveAimToTarget(predicted.first, predicted.second,
                     crosshairX, crosshairY, kPathBOutputAttenuation);
    return true;
}

void MouseThread::clearQueuedMoves()
{
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        std::queue<Move> empty;
        moveQueue_.swap(empty);
    }
    pid_.reset();

    // Hotkey lifted (or anything else that pre-empts pursuit) — make sure
    // we never leave the auto-fire button stuck down. The cooldown stays in
    // place so re-acquiring the hotkey doesn't immediately re-fire.
    forceTriggerRelease();
}

void MouseThread::resetPrediction()
{
    clearQueuedMoves();
    has_prev_time_ = false;
    has_kalman_prev_time_ = false;
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
        fire_cooldown_until_ =
            now + std::chrono::milliseconds(params_.smart_trigger_cooldown_ms);
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
