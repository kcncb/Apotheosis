#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "mouse.h"
#include "capture.h"
#include "Apotheosis.h"
#include "Makcu.h"
#include "MakcuNew.h"
#include "runtime/latency_probe.h"

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
    MakcuConnection* makcuConnection,
    MakcuNewConnection* makcuNewConnection)
    : makcu_(makcuConnection),
      makcu_new_(makcuNewConnection)
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
    if (makcu_)
        makcu_->press(1);
    else if (makcu_new_)
        makcu_new_->press(1);
}

void MouseThread::sendLeftUpToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (makcu_)
        makcu_->release(1);
    else if (makcu_new_)
        makcu_new_->release(1);
}

void MouseThread::updateParams(const MouseRuntimeParams& in)
{
    const auto sanitized = sanitize(in);
    params_ = sanitized;
}

void MouseThread::queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns, uint64_t command_id)
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
    journal_->cancel(pendingCommandId_);
    pendingCommandId_ = command_id;
    moveSlot_.replace(dx, dy, std::chrono::steady_clock::now(), capture_ns, aim_ns, command_id);
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
            pendingCommandId_ = 0;
            ul.unlock();

            // 每个 movement pair 发送前检查 generation。新帧到达后，
            // 已取出的旧批次立即作废，不再沿旧方向继续发送。
            if (!moveSlot_.isCurrent(move.generation))
            {
                journal_->cancel(move.command_id);
                continue;
            }

            const bool sent = sendMovementToDriver(move.dx, move.dy);
            journal_->complete(move.command_id, sent, runtime::latency::nowNs());
            if (sent) {
                runtime::latency::markMoveSent(move.capture_ns, move.aim_ns);
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

void MouseThread::sendPixelMove(double dx, double dy, motion::Calibration calibration,
                               int limit_x, int limit_y, int64_t capture_ns, int64_t aim_ns)
{
    std::array<int, 2> counts;
    {
        std::lock_guard<std::mutex> lock(outputMtx_);
        counts = mapper_.map({dx, dy}, calibration, limit_x, limit_y);
    }
    if (counts[0] == 0 && counts[1] == 0) return;
    const auto id = journal_->queued(counts[0], counts[1], calibration, runtime::latency::nowNs());
    sendRawMove(counts[0], counts[1], capture_ns, aim_ns, id);
}

void MouseThread::sendRawMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns, uint64_t command_id)
{
    if (dx == 0 && dy == 0) return;
    if (!command_id) command_id = journal_->queued(dx, dy, {}, runtime::latency::nowNs());
    {
        std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
        if (makcu_new_)
        {
            // MAKCUNEW 是【直发】: 不走 moveWorkerLoop。于是那条路上维护的
            // lastLatencyUs_ / appliedD* / failedMoves_ 永远不会被更新 ——
            // 直接后果是延迟探针的 aim2mv(T3→T4) 与 E2E 恒为 0.00, 日志看起来
            // 像"写出零延迟", 实际上是没测。这里就地把真实写出耗时与位移量补上,
            // 让遥测对这条路径同样成立(语义与队列路径一致: 决策→写出完成)。
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = makcu_new_->move(dx, dy);
            journal_->complete(command_id, ok, runtime::latency::nowNs());
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            lastLatencyUs_.store(static_cast<long long>(elapsed_us),
                                 std::memory_order_release);
            if (ok)
            {
                if (dx != 0 || dy != 0) runtime::latency::markMoveSent(capture_ns, aim_ns);
                appliedDx_.fetch_add(dx, std::memory_order_release);
                appliedDy_.fetch_add(dy, std::memory_order_release);
            }
            else
            {
                failedMoves_.fetch_add(1, std::memory_order_release);
            }
            return;
        }
    }
    queueMove(dx, dy, capture_ns, aim_ns, command_id);
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
    if (makcu_)
    {
        if (!makcu_->isOpen()) return false;
        makcu_->move(dx, dy);
        return makcu_->isOpen();
    }
    return false;
}

void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> lock(queueMtx_);
    // generation 同步提升，可抢占 worker 已取出但尚未发送的旧移动。
    moveSlot_.clear();
    pendingCommandId_ = 0;
    journal_->cancelQueued();
    {
        std::lock_guard<std::mutex> outputLock(outputMtx_);
        mapper_.reset();
    }
    std::lock_guard<std::recursive_mutex> inputLock(input_method_mutex);
    if (makcu_new_) makcu_new_->cancelMove();
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

void MouseThread::setMakcuConnection(MakcuConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_ = newMakcu;
}

void MouseThread::setMakcuNewConnection(MakcuNewConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_new_ = newMakcu;
}
