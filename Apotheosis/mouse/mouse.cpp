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
    p.smart_trigger_hit_scale_x = std::clamp(p.smart_trigger_hit_scale_x, 0.05f, 1.0f);
    p.smart_trigger_hit_scale_y = std::clamp(p.smart_trigger_hit_scale_y, 0.05f, 1.0f);
    p.smart_trigger_reaction_ms = std::clamp(p.smart_trigger_reaction_ms, 0, 1000);
    p.smart_trigger_hold_ms     = std::clamp(p.smart_trigger_hold_ms,     5, 5000);
    p.smart_trigger_cooldown_ms = std::clamp(p.smart_trigger_cooldown_ms, 0, 5000);
    return p;
}

} // namespace

// Smart-trigger telemetry.
std::atomic<bool>   g_smart_trigger_ready{ false };
std::atomic<float>  g_smart_trigger_hit_prob{ 0.0f };
std::atomic<float>  g_smart_trigger_recent_variance_px{ 0.0f };

// Flick / Track telemetry (kept for overlay compat).
std::atomic<float> g_pid_last_err_px{ 0.0f };
std::atomic<bool>  g_pid_mode_track{ false };

// Dynamic-FOV telemetry.
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
    moveWorker_ = std::thread(&MouseThread::moveWorkerLoop, this);
}

MouseThread::~MouseThread()
{
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
    params_ = sanitized;
    screen_width_ = params_.detection_resolution;
    screen_height_ = params_.detection_resolution;

    g_smart_trigger_ready.store(false);
    g_smart_trigger_hit_prob.store(0.0f);
    g_smart_trigger_recent_variance_px.store(0.0f);

    forceTriggerRelease();
    fire_cooldown_until_ = {};
    on_target_ = false;
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

void MouseThread::sendRawMove(int dx, int dy)
{
    queueMove(dx, dy);
}

void MouseThread::pressLeftButton()
{
    if (firing_active_)
        return;
    sendLeftDownToDriver();
    firing_active_ = true;
    g_smart_trigger_ready.store(true);
}

void MouseThread::releaseLeftButton()
{
    if (!firing_active_)
        return;
    sendLeftUpToDriver();
    firing_active_ = false;
    g_smart_trigger_ready.store(false);
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
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(INPUT));
    }
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

    if (!params_.smart_trigger_enabled)
    {
        forceTriggerRelease();
        g_smart_trigger_ready.store(false);
        g_smart_trigger_hit_prob.store(0.0f);
        g_smart_trigger_recent_variance_px.store(0.0f);
        return;
    }

    const double halfW = trig_half_w_.load();
    const double halfH = trig_half_h_.load();
    const double tolX  = halfW * static_cast<double>(params_.smart_trigger_hit_scale_x);
    const double tolY  = halfH * static_cast<double>(params_.smart_trigger_hit_scale_y);

    bool on_target = false;
    double hit_frac = 0.0;
    if (tolX > 0.5 && tolY > 0.5)
    {
        const double ex = std::abs(crosshairX - trig_target_x_.load());
        const double ey = std::abs(crosshairY - trig_target_y_.load());
        on_target = (ex <= tolX) && (ey <= tolY);
        const double worst = std::max(ex / tolX, ey / tolY);
        hit_frac = std::clamp(1.0 - worst, 0.0, 1.0);
    }
    g_smart_trigger_hit_prob.store(static_cast<float>(hit_frac));

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

    // 扫射语义 (与"点射"区分):
    //   smart_trigger_hold_ms 解读为「单次扣下的最小按住时间」(防止
    //   单帧抖动立刻松手),而不是「强制松手时点」。一旦最小按住时间
    //   走完,只要准星仍在命中框内就**持续按住**真扫射,目标丢失才松手;
    //   随后 cooldown_ms 才参与节流,防止短抖造成的快速点-松-点。
    //   这样用户填 1000ms 就是「至少按住 1 秒,期间命中就继续扫射」,
    //   而不是「1 秒后强制松手再 cooldown」造成的伪点射。
    if (firing_active_ && now >= fire_release_at_ && !on_target_)
    {
        sendLeftUpToDriver();
        firing_active_ = false;
        fire_cooldown_until_ =
            now + std::chrono::milliseconds(params_.smart_trigger_cooldown_ms);
    }

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

void MouseThread::clearQueuedMoves()
{
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        std::queue<Move> empty;
        moveQueue_.swap(empty);
    }
    forceTriggerRelease();
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
