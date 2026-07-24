#pragma once

#include "timed_button_exact.hpp"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace cvm::recovered {

struct RelativeMove {
    std::int32_t dx{};
    std::int32_t dy{};
    friend bool operator==(const RelativeMove&, const RelativeMove&) = default;
};

// Raw x64 MSVC ABI record moved into object+160 by 0x1400670E0.  This is
// separate from the portable std::vector class below so memory snapshots can
// be decoded without pretending the host STL object is layout-compatible.
struct NativeMovementBatchRecordAbi {
    std::uint64_t begin{};                 // +0
    std::uint64_t end{};                   // +8
    std::uint64_t capacity{};              // +16
    std::uint8_t flag{};                   // +24; publisher writes zero
    std::uint8_t _pad19[7]{};
    std::uint64_t sequence{};              // +32
};

// x64 MSVC deque control block used at +224 and +344.  Element storage lives
// in separately allocated two-element (button) or one-element (key) blocks.
struct NativeMsvcDequeControlAbi {
    std::uint64_t proxy{};       // +0
    std::uint64_t map{};         // +8, pointer to block-pointer table
    std::uint64_t map_size{};    // +16
    std::uint64_t first_offset{};// +24
    std::uint64_t element_count{};// +32
};

// x64 MSVC std::map control fields: proxy/head pointer and element count.
struct NativeMsvcMapControlAbi {
    std::uint64_t head{};
    std::uint64_t element_count{};
};

// Observed base/derived layout for the WinAPI relative-input backend created
// at 0x14037A5D0.  Address-oriented padding includes the native mutex,
// condition variable, timed key/button queues and two thread handles.  Only
// fields proven by direct reads are named.
struct NativeAsyncRelativeMouseObservedAbi {
    std::uint64_t vtable{};                 // +0
    std::byte _native_sync_and_queues_008[152]{};
    NativeMovementBatchRecordAbi pending_batch{}; // +160
    std::uint8_t pending_valid{};           // +200
    std::byte _pad0c9[7]{};
    std::uint64_t epoch_snapshot{};         // +208
    std::uint64_t generation{};             // +216
    NativeMsvcDequeControlAbi button_command_queue{}; // +224..+263, 8-byte elements
    NativeTimedButtonSlotAbi button_timers[5]{};      // +264..+343
    NativeMsvcDequeControlAbi timed_key_queue{};      // +344..+383, 40-byte elements
    NativeMsvcMapControlAbi timed_key_state{};        // +384..+399
    std::uint8_t enabled_channel_mask{};    // +400
    std::uint8_t stop_requested{};          // +401
    std::uint8_t workers_running{};         // +402
    std::uint8_t secondary_epoch_flag{};    // +403
    std::byte _threads_and_sync_194[124]{}; // +404..+527
    std::byte backend_name_msvc_string[32]{}; // +528
    std::int32_t backend_selector{};        // +560
    std::byte _tail234[4]{};
};

// sub_140066F30: remove zero/out-of-range pairs before asynchronous enqueue.
std::vector<RelativeMove> sanitize_movement_batch(
    std::span<const RelativeMove> input);

struct MovementTelemetry {
    float summed_dx{};
    float summed_dy{};
    bool valid{};
    bool executor_ran{};
};

using MovementSplitter =
    std::function<std::vector<RelativeMove>(std::int32_t, std::int32_t)>;
using MovementTransform = std::function<RelativeMove(RelativeMove)>;
using MovementObserver = std::function<void(RelativeMove)>;
using MovementBatchExecutor =
    std::function<void(std::span<const RelativeMove>)>;

struct ControllerMovementConfig {
    bool block_x{};                         // controller +10877
    bool block_y{};                         // controller +10878
    std::int32_t movement_mode{};           // controller +208
    bool executor_present{true};
    bool executor_ready{true};
    MovementSplitter mode2_splitter{};      // controller +7616
    MovementTransform coordinate_transform{}; // controller +11120
    MovementObserver pre_submit_observer{}; // controller +11056
    MovementBatchExecutor executor{};       // object at controller +10624
};

struct ControllerMovementResult {
    RelativeMove accepted_total{};
    std::vector<RelativeMove> submitted{};
    MovementTelemetry telemetry{};
};

// Source reconstruction of sub_140052400 -> sub_1400526B0.  The optional
// callbacks expose the native plugin/vtable boundaries without hiding any
// ordering or fallback behavior.
ControllerMovementResult dispatch_controller_movement(
    const ControllerMovementConfig& config,
    std::int32_t dx,
    std::int32_t dy);

// Thread-safe reconstruction of the +160 pending-batch slot used by
// sub_1400670E0/sub_140068980.  It is deliberately latest-only, not a FIFO:
// publishing a new batch replaces a not-yet-consumed batch and increments the
// generation used by the worker to preempt the old sequence.
class LatestMovementBatchSlot {
public:
    struct TakenBatch {
        std::vector<RelativeMove> moves{};
        std::uint64_t generation{};
    };

    bool publish(std::span<const RelativeMove> moves);
    bool wait_take(TakenBatch& out);
    bool newer_than(std::uint64_t generation) const noexcept;
    void stop();
    std::uint64_t generation() const noexcept;

private:
    mutable std::mutex mutex_{};
    std::condition_variable wake_{};
    std::vector<RelativeMove> pending_{};
    std::atomic<std::uint64_t> generation_{};
    bool pending_valid_{};
    bool stopped_{};
};

class AsyncRelativeMouseExact {
public:
    using MoveSink = std::function<void(std::int32_t, std::int32_t)>;

    explicit AsyncRelativeMouseExact(MoveSink sink);
    ~AsyncRelativeMouseExact();
    AsyncRelativeMouseExact(const AsyncRelativeMouseExact&) = delete;
    AsyncRelativeMouseExact& operator=(const AsyncRelativeMouseExact&) = delete;

    bool submit(std::span<const RelativeMove> moves);
    void stop();

private:
    void worker_loop();

    MoveSink sink_{};
    LatestMovementBatchSlot slot_{};
    std::thread worker_{};
    std::once_flag stop_once_{};
};

enum class WinApiRelativeBackend : std::int32_t {
    mouse_event = 0,
    send_input = 1,
};

// sub_14037A7D0.  Returns the native API's return value: mouse_event is void
// (reported as zero here), SendInput returns the number of inserted events.
std::uint32_t emit_relative_move_winapi(WinApiRelativeBackend backend,
                                        std::int32_t dx,
                                        std::int32_t dy) noexcept;

// WinAPI derived vtable +0x1d0 (0x14037A860).  Channel is the native 0..4
// numbering.  mouse_event returns void (reported as zero); SendInput returns
// its inserted-event count.
std::uint32_t emit_mouse_button_winapi(WinApiRelativeBackend backend,
                                       MouseButtonChannel channel,
                                       bool pressed) noexcept;

// WinAPI derived vtable +0xf0/+0xf8 (0x14037A700/0x14037A710 ->
// 0x14037A9F0).  The original first resolves its key-name string through the
// recovered name->VK map; this low-level boundary accepts that resolved VK.
bool emit_virtual_key_winapi(std::uint16_t virtual_key,
                             bool pressed) noexcept;

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::RelativeMove) == 8);
static_assert(sizeof(cvm::recovered::NativeMovementBatchRecordAbi) == 40);
static_assert(offsetof(cvm::recovered::NativeMovementBatchRecordAbi, sequence) == 32);
static_assert(sizeof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi) == 568);
static_assert(sizeof(cvm::recovered::NativeMsvcDequeControlAbi) == 40);
static_assert(sizeof(cvm::recovered::NativeMsvcMapControlAbi) == 16);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, pending_batch) == 160);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, pending_valid) == 200);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, generation) == 216);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, button_command_queue) == 224);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, button_timers) == 264);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, timed_key_queue) == 344);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, timed_key_state) == 384);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, enabled_channel_mask) == 400);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, stop_requested) == 401);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, workers_running) == 402);
static_assert(offsetof(cvm::recovered::NativeAsyncRelativeMouseObservedAbi, backend_selector) == 560);
