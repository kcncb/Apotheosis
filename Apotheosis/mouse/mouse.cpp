#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "mouse.h"
#include "capture.h"
#include "Apotheosis.h"
#include "Makcu.h"
#include "MakcuNew.h"
#include "kmboxNetConnection.h"
#include "mouse_driver.h"
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
    MakcuNewConnection* makcuNewConnection,
    KmboxNetConnection* kmboxNetConnection)
    : makcu_(makcuConnection),
      makcu_new_(makcuNewConnection),
      kmbox_net_(kmboxNetConnection)
{
    updateParams(params);
    refreshDriver();
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
    if (driver_) driver_->leftDown();
}

void MouseThread::sendLeftUpToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->leftUp();
}

// 右键 = 通道 2 (见 Makcu.cpp 的 mouseButtonFromChannel: 1=左 2=右 3=中 4/5=侧键)。
// 自动开镜靠它; 与左键共用同一把递归锁, 保证"开镜指令"和"开火指令"的先后顺序
// 在串口写出这一层不会被换序。
void MouseThread::sendRightDownToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->rightDown();
}

void MouseThread::sendRightUpToDriver()
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (driver_) driver_->rightUp();
}

void MouseThread::updateParams(const MouseRuntimeParams& in)
{
    const auto sanitized = sanitize(in);
    params_ = sanitized;
}

void MouseThread::queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns)
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
    moveSlot_.replace(dx, dy, std::chrono::steady_clock::now(), capture_ns, aim_ns);
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

void MouseThread::sendRawMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns)
{
    if (dx == 0 && dy == 0) return;
    {
        std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
        // ★ 哪些后端走【直发】、哪些走队列, 以前是写死 `if (makcu_new_)`,
        //   等价于"按类型分叉"。现在这个决定收进 IDriver::directSend()
        //   (见 mouse_driver.h), 加后端时不用再回来改这里。
        if (driver_ && driver_->directSend())
        {
            // 直发后端不走 moveWorkerLoop。于是那条路上维护的
            // lastLatencyUs_ / appliedD* / failedMoves_ 永远不会被更新 ——
            // 直接后果是延迟探针的 aim2mv(T3→T4) 与 E2E 恒为 0.00, 日志看起来
            // 像"写出零延迟", 实际上是没测。这里就地把真实写出耗时与位移量补上,
            // 让遥测对这条路径同样成立(语义与队列路径一致: 决策→写出完成)。
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = driver_->move(dx, dy);
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
    queueMove(dx, dy, capture_ns, aim_ns);
}

void MouseThread::pressLeftButton()
{
    sendLeftDownToDriver();
}

void MouseThread::releaseLeftButton()
{
    sendLeftUpToDriver();
}

void MouseThread::pressRightButton()
{
    sendRightDownToDriver();
}

void MouseThread::releaseRightButton()
{
    sendRightUpToDriver();
}

bool MouseThread::tapKey(int hid_key, int hold_ms)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    // 按**能力**判断, 不按类型: 没有 kCapKeyboard 的后端(MAKCU 官方库)
    // 直接失败, 调用方据此把"自动急停"当成不可用(不会静默什么都不做)。
    if (!driver_ || !driver_->capabilities()) return false;
    return driver_->tapKey(hid_key, hold_ms);
}

bool MouseThread::supports(uint32_t capability) const
{
    return (driverCapabilities() & capability) != 0;
}

uint32_t MouseThread::driverCapabilities() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(input_method_mutex));
    return driver_ ? driver_->capabilities() : mouse_driver::kCapNone;
}

std::string MouseThread::driverName() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(input_method_mutex));
    return driver_ ? driver_->name() : u8"(无)";
}

std::string MouseThread::driverStatus() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(input_method_mutex));
    if (!driver_)
        return u8"鼠标后端 (未选择) 不可用: 输入方式没有对应到任何已支持的后端";
    if (!driver_->isOpen())
        return mouse_driver::describeStatus(driver_->name(), false, driver_->lastError());
    // 能力位也一起报 —— 用户据此知道"为什么自动急停在这个档位没反应"。
    return mouse_driver::describeStatus(
        driver_->name(), true,
        std::string(u8"能力: ") + mouse_driver::describeCapabilities(driver_->capabilities()));
}

bool MouseThread::sendMovementToDriver(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return true;

    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    if (!driver_) return false;
    return driver_->move(dx, dy);
}

void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> lock(queueMtx_);
    // generation 同步提升，可抢占 worker 已取出但尚未发送的旧移动。
    moveSlot_.clear();
    std::lock_guard<std::recursive_mutex> inputLock(input_method_mutex);
    if (driver_) driver_->cancelMove();
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

// 由三个连接裸指针重新解析出当前生效的统一驱动。
//
// ★ 优先级顺序 = "哪个被接线了就用哪个", 与旧代码 `if (makcu_) ... else if
//   (makcu_new_)` 的优先级一致; 新增的 KMBOXNET 放最后, 所以它**不会**改变
//   既有两档的行为(老配置里 kmbox_net_ 恒为 nullptr)。
//
// ★ 调用方必须持 input_method_mutex (或者在单线程的构造阶段)。
void MouseThread::refreshDriver()
{
    // 先把所有权放开, 再按当前指针重建 —— 顺序反了会 delete 掉正在用的对象。
    driver_owned_.reset();
    driver_ = nullptr;

    if (makcu_)
    {
        driver_owned_ = std::make_unique<mouse_driver::WrappedMakcuDriver>(makcu_);
        driver_ = driver_owned_.get();
    }
    else if (makcu_new_)
    {
        driver_owned_ = std::make_unique<mouse_driver::WrappedMakcuNewDriver>(makcu_new_);
        driver_ = driver_owned_.get();
    }
    else if (kmbox_net_)
    {
        driver_owned_ = std::make_unique<mouse_driver::WrappedKmboxNetDriver>(kmbox_net_);
        driver_ = driver_owned_.get();
    }
}

void MouseThread::setMakcuConnection(MakcuConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_ = newMakcu;
    refreshDriver();
}

void MouseThread::setMakcuNewConnection(MakcuNewConnection* newMakcu)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    makcu_new_ = newMakcu;
    refreshDriver();
}

void MouseThread::setKmboxNetConnection(KmboxNetConnection* newKmboxNet)
{
    std::lock_guard<std::recursive_mutex> lock(input_method_mutex);
    kmbox_net_ = newKmboxNet;
    refreshDriver();
}
