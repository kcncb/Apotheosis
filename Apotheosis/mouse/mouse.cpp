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
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        workerStop_.store(true);
        moveSlot_.clear();
    }
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

    // CVM sanitizer：任一轴越出 inclusive [-30000,30000] 时丢弃整条移动。
    const auto valid_component = [](int value) {
        return static_cast<std::uint32_t>(value) + 30000u <= 60000u;
    };
    if (!valid_component(dx) || !valid_component(dy))
        return;

    std::lock_guard<std::mutex> lg(queueMtx_);
    // latest-only：新检测帧直接覆盖尚未消费的旧移动，并提升 generation。
    moveSlot_.replace(dx, dy, std::chrono::steady_clock::now());
    queueCv_.notify_one();
}

void MouseThread::moveWorkerLoop()
{
    try
    {
        while (!workerStop_.load())
        {
            std::unique_lock<std::mutex> ul(queueMtx_);
            queueCv_.wait(ul, [&] {
                return workerStop_.load() || moveSlot_.hasPending();
            });
            if (workerStop_.load())
                break;

            mouse_async::PendingMove move;
            if (!moveSlot_.take(move))
                continue;
            ul.unlock();

            // 每个 movement pair 发送前检查 generation。新帧到达后，
            // 已取出的旧批次立即作废，不再沿旧方向继续发送。
            if (!moveSlot_.isCurrent(move.generation))
                continue;

            const bool sent = sendMovementToDriver(move.dx, move.dy);
            if (sent) {
                appliedDx_.fetch_add(move.dx, std::memory_order_release);
                appliedDy_.fetch_add(move.dy, std::memory_order_release);
            } else {
                failedMoves_.fetch_add(1, std::memory_order_release);
            }
            const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - move.queued_at).count();
            lastLatencyUs_.store(latency, std::memory_order_release);
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
    // generation 同步提升，可抢占 worker 已取出但尚未发送的旧移动。
    moveSlot_.clear();
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
        out.backlog = moveSlot_.pendingCount();
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
