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
    return p;
}

} // namespace

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

void MouseThread::updateParams(const MouseRuntimeParams& in)
{
    const auto sanitized = sanitize(in);
    params_ = sanitized;
    screen_width_ = params_.detection_resolution;
    screen_height_ = params_.detection_resolution;
}

void MouseThread::queueMove(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    std::lock_guard<std::mutex> lg(queueMtx_);
    if (moveQueue_.size() >= queueLimit_)
    {
        // 队列满时不能丢掉已被控制器记账的位移，否则虚拟位置
        // 会与真实鼠标分离。合并到队尾可保持累计位移不变。
        Move& tail = moveQueue_.back();
        tail.dx += dx;
        tail.dy += dy;
    }
    else
    {
        moveQueue_.push({ dx, dy, std::chrono::steady_clock::now() });
    }
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
                const bool sent = sendMovementToDriver(m.dx, m.dy);
                if (sent) {
                    appliedDx_.fetch_add(m.dx, std::memory_order_release);
                    appliedDy_.fetch_add(m.dy, std::memory_order_release);
                } else {
                    failedMoves_.fetch_add(1, std::memory_order_release);
                }
                const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - m.queued_at).count();
                lastLatencyUs_.store(latency, std::memory_order_release);
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
    sendLeftDownToDriver();
}

void MouseThread::releaseLeftButton()
{
    sendLeftUpToDriver();
}

bool MouseThread::sendMovementToDriver(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return true;

    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (kmbox_net_)
    {
        kmbox_net_->move(dx, dy);
        return true;
    }
    else if (kmbox_a_)
    {
        kmbox_a_->move(dx, dy);
        return true;
    }
    else if (makcu_)
    {
        makcu_->move(dx, dy);
        return true;
    }
    else if (arduino_)
    {
        arduino_->move(dx, dy);
        return true;
    }
    else if (gHub_)
    {
        gHub_->mouse_xy(dx, dy);
        return true;
    }
    else
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        return SendInput(1, &in, sizeof(INPUT)) == 1;
    }
}

void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> lock(queueMtx_);
    std::queue<Move> empty;
    moveQueue_.swap(empty);
}

MouseThread::MovementFeedback MouseThread::consumeMovementFeedback()
{
    MovementFeedback out;
    out.dx = static_cast<int>(appliedDx_.exchange(0, std::memory_order_acq_rel));
    out.dy = static_cast<int>(appliedDy_.exchange(0, std::memory_order_acq_rel));
    out.latency_ms = static_cast<double>(
        lastLatencyUs_.load(std::memory_order_acquire)) / 1000.0;
    out.failed = failedMoves_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        out.backlog = moveQueue_.size();
    }
    return out;
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
