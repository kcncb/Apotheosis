#ifndef MOUSE_H
#define MOUSE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "latest_move_slot.h"
#include "mouse_driver.h"

// Forward declarations so that mouse.h stays light.
class MakcuConnection;
class MakcuNewConnection;
class KmboxNetConnection;

struct MouseRuntimeParams
{
    int detection_resolution = 320;
};

class MouseThread
{
public:
    struct MovementFeedback
    {
        int dx = 0;
        int dy = 0;
        double latency_ms = 0.0;
        size_t backlog = 0;
        unsigned long long failed = 0;
    };

    MouseThread(
        const MouseRuntimeParams& params,
        MakcuConnection* makcuConnection = nullptr,
        MakcuNewConnection* makcuNewConnection = nullptr,
        KmboxNetConnection* kmboxNetConnection = nullptr);
    ~MouseThread();

    MouseThread(const MouseThread&) = delete;
    MouseThread& operator=(const MouseThread&) = delete;

    void updateParams(const MouseRuntimeParams& params);

    void clearQueuedMoves();
    MovementFeedback consumeMovementFeedback();

    // Recursive: a high-level aim op holds this lock while
    // sendLeft{Down,Up}ToDriver may chain into the same lock on the same thread.
    std::recursive_mutex input_method_mutex;

    // ─── Raw driver channel (used by the Boss AI aim engine) ───────────────
    void sendRawMove(int dx, int dy, int64_t capture_ns = 0, int64_t aim_ns = 0);
    void pressLeftButton();
    void releaseLeftButton();
    // 自动开镜 (mouse/trigger_scope.h) 用的右键通道。
    void pressRightButton();
    void releaseRightButton();

    // 自动急停 (mouse/auto_stop.h) 用的键盘通道。
    // hid_key = HID usage id; hold_ms 由固件定时弹起(自清, 不会卡键)。
    // 只有带 kCapKeyboard 的后端支持, 其它后端返回 false(功能自动失效)。
    bool tapKey(int hid_key, int hold_ms);

    // ─── 能力查询 (移植 AimMagic 驱动 vtable 的"按契约问, 不按类型分叉") ────
    //
    // ★ 加这个的意义: 旧代码想知道"能不能发键盘"只能写 `if (makcu_new_)`,
    //   每加一个后端就要改所有调用点。现在问的是能力, 不是类型。
    bool supports(uint32_t capability) const;
    uint32_t driverCapabilities() const;
    // 当前后端的名字 ("MAKCU" / "MAKCUNEW" / "KMBOXNET" / "(无)")。
    std::string driverName() const;
    // 一行中文状态, 给日志/UI 显示 —— 连不上时**带得上理由**。
    std::string driverStatus() const;

    // Input device hot-swap.
    void setMakcuConnection(MakcuConnection* makcu);
    void setMakcuNewConnection(MakcuNewConnection* makcuNew);
    void setKmboxNetConnection(KmboxNetConnection* kmboxNet);

private:
    void moveWorkerLoop();
    void queueMove(int dx, int dy, int64_t capture_ns, int64_t aim_ns);
    bool sendMovementToDriver(int dx, int dy);

    void sendLeftDownToDriver();
    void sendLeftUpToDriver();
    void sendRightDownToDriver();
    void sendRightUpToDriver();

    // 由三个裸指针重新解析出当前生效的后端。**不加锁**, 调用方持
    // input_method_mutex 或处在单线程的构造/热插拔阶段。
    void refreshDriver();

    MouseRuntimeParams params_{};
    std::mutex outputMtx_;

    // Async driver dispatch.
    mouse_async::LatestMoveSlot moveSlot_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    std::thread moveWorker_;
    std::atomic<bool> workerStop_{ false };
    std::atomic<long long> appliedDx_{ 0 };
    std::atomic<long long> appliedDy_{ 0 };
    std::atomic<long long> lastLatencyUs_{ 0 };
    std::atomic<unsigned long long> failedMoves_{ 0 };

    // 三个后端的连接对象 (可能同时只有一个非空, 由 input_method 决定)。
    MakcuConnection* makcu_ = nullptr;
    MakcuNewConnection* makcu_new_ = nullptr;
    KmboxNetConnection* kmbox_net_ = nullptr;

    // 当前生效的统一驱动。**非拥有指针** —— 它内部持有上面某一个连接对象,
    // 生命周期由 `driver_owned_` 管。
    mouse_driver::IDriver* driver_ = nullptr;
    std::unique_ptr<mouse_driver::IDriver> driver_owned_;
};

#endif // MOUSE_H
