#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")

#include "mouse_output_exact.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cvm::recovered {
namespace {

// dword_140BE5D70 guards all low-level virtual input calls made by the two
// worker threads.  A process-wide mutex reproduces that serialization point.
std::mutex g_native_input_call_mutex;

} // namespace

std::vector<RelativeMove> sanitize_movement_batch(
    std::span<const RelativeMove> input) {
    std::vector<RelativeMove> output;
    output.reserve(input.size());
    for (const RelativeMove move : input) {
        if (move.dx == 0 && move.dy == 0)
            continue;
        // Native unsigned range test:
        //   unsigned(component + 30000) <= 60000
        const bool x_valid =
            static_cast<std::uint32_t>(move.dx) + 30000u <= 60000u;
        const bool y_valid =
            static_cast<std::uint32_t>(move.dy) + 30000u <= 60000u;
        if (x_valid && y_valid)
            output.push_back(move);
    }
    return output;
}

ControllerMovementResult dispatch_controller_movement(
    const ControllerMovementConfig& config,
    std::int32_t dx,
    std::int32_t dy) {
    ControllerMovementResult result{};
    if (config.block_x)
        dx = 0;
    if (config.block_y)
        dy = 0;

    std::vector<RelativeMove> moves;
    if (dx != 0 || dy != 0) {
        if (config.movement_mode == 2) {
            if (config.mode2_splitter)
                moves = config.mode2_splitter(dx, dy);

            std::int32_t sum_x{};
            std::int32_t sum_y{};
            for (const auto move : moves) {
                // Native accumulation is 32-bit unsigned wrapping; assigning
                // the resulting bits to int32_t preserves the same ABI value.
                sum_x = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(sum_x)
                    + static_cast<std::uint32_t>(move.dx));
                sum_y = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(sum_y)
                    + static_cast<std::uint32_t>(move.dy));
            }
            if (moves.empty() || (sum_x == 0 && sum_y == 0))
                moves.push_back({dx, dy});
        } else {
            // 0x140052500..0x14005251F converts int -> float -> lroundf.
            moves.push_back({
                static_cast<std::int32_t>(std::lround(static_cast<float>(dx))),
                static_cast<std::int32_t>(std::lround(static_cast<float>(dy)))
            });
        }
    } else {
        // The native path still calls the executor layer with one zero pair;
        // the asynchronous sanitizer removes it before queue publication.
        moves.push_back({0, 0});
    }

    if (config.movement_mode == 2 && (dx != 0 || dy != 0)) {
        std::uint32_t accepted_x{};
        std::uint32_t accepted_y{};
        for (const auto move : moves) {
            accepted_x += static_cast<std::uint32_t>(move.dx);
            accepted_y += static_cast<std::uint32_t>(move.dy);
        }
        if (accepted_x == 0 && accepted_y == 0)
            result.accepted_total = {dx, dy};
        else
            result.accepted_total = {
                static_cast<std::int32_t>(accepted_x),
                static_cast<std::int32_t>(accepted_y)};
    } else {
        // Non-split mode returns the post-axis-mask request, not the rounded
        // pair passed to the executor.
        result.accepted_total = {dx, dy};
    }

    if (!config.executor_present || !config.executor_ready) {
        result.telemetry = {};
        return result;
    }

    result.submitted.reserve(moves.size());
    for (RelativeMove move : moves) {
        if (config.coordinate_transform)
            move = config.coordinate_transform(move);
        if (move.dx == 0 && move.dy == 0)
            continue;
        if (config.pre_submit_observer)
            config.pre_submit_observer(move);
        result.submitted.push_back(move);
    }

    if (!result.submitted.empty()) {
        if (config.executor)
            config.executor(result.submitted);
        std::uint32_t submitted_x{};
        std::uint32_t submitted_y{};
        for (const auto move : result.submitted) {
            submitted_x += static_cast<std::uint32_t>(move.dx);
            submitted_y += static_cast<std::uint32_t>(move.dy);
        }
        result.telemetry.summed_dx =
            static_cast<float>(static_cast<std::int32_t>(submitted_x));
        result.telemetry.summed_dy =
            static_cast<float>(static_cast<std::int32_t>(submitted_y));
        result.telemetry.valid = true;
        result.telemetry.executor_ran = true;
    }
    return result;
}

bool LatestMovementBatchSlot::publish(std::span<const RelativeMove> moves) {
    auto sanitized = sanitize_movement_batch(moves);
    if (sanitized.empty())
        return false;

    {
        std::lock_guard lock(mutex_);
        if (stopped_)
            return false;
        pending_ = std::move(sanitized); // replaces, never appends
        pending_valid_ = true;
        generation_.fetch_add(1, std::memory_order_release);
    }
    wake_.notify_one();
    return true;
}

bool LatestMovementBatchSlot::wait_take(TakenBatch& out) {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] { return stopped_ || pending_valid_; });
    if (stopped_)
        return false;
    out.moves = std::move(pending_);
    pending_.clear();
    pending_valid_ = false;
    out.generation = generation_.load(std::memory_order_acquire);
    return true;
}

bool LatestMovementBatchSlot::newer_than(
    std::uint64_t generation) const noexcept {
    return generation_.load(std::memory_order_acquire) != generation;
}

void LatestMovementBatchSlot::stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        pending_.clear();
        pending_valid_ = false;
    }
    wake_.notify_all();
}

std::uint64_t LatestMovementBatchSlot::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

AsyncRelativeMouseExact::AsyncRelativeMouseExact(MoveSink sink)
    : sink_(std::move(sink)), worker_(&AsyncRelativeMouseExact::worker_loop, this) {}

AsyncRelativeMouseExact::~AsyncRelativeMouseExact() {
    stop();
}

bool AsyncRelativeMouseExact::submit(std::span<const RelativeMove> moves) {
    return slot_.publish(moves);
}

void AsyncRelativeMouseExact::stop() {
    std::call_once(stop_once_, [&] {
        slot_.stop();
        if (worker_.joinable())
            worker_.join();
    });
}

void AsyncRelativeMouseExact::worker_loop() {
    LatestMovementBatchSlot::TakenBatch batch;
    while (slot_.wait_take(batch)) {
        for (const RelativeMove move : batch.moves) {
            // sub_140068980 checks +216 before each vector element.  A newly
            // published batch therefore abandons the remaining old elements.
            if (slot_.newer_than(batch.generation))
                break;
            if (sink_) {
                std::lock_guard input_call_lock(g_native_input_call_mutex);
                sink_(move.dx, move.dy);
            }
        }
    }
}

std::uint32_t emit_relative_move_winapi(WinApiRelativeBackend backend,
                                        std::int32_t dx,
                                        std::int32_t dy) noexcept {
    if (backend == WinApiRelativeBackend::mouse_event) {
        ::mouse_event(MOUSEEVENTF_MOVE,
                      static_cast<DWORD>(dx),
                      static_cast<DWORD>(dy),
                      0,
                      0);
        return 0;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = 0;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;
    return ::SendInput(1, &input, sizeof(INPUT));
}

std::uint32_t emit_mouse_button_winapi(WinApiRelativeBackend backend,
                                       MouseButtonChannel channel,
                                       bool pressed) noexcept {
    DWORD flags{};
    DWORD mouse_data{};
    switch (channel) {
    case MouseButtonChannel::left:
        flags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case MouseButtonChannel::right:
        flags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case MouseButtonChannel::middle:
        flags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case MouseButtonChannel::x1:
        flags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        mouse_data = XBUTTON1;
        break;
    case MouseButtonChannel::x2:
        flags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        mouse_data = XBUTTON2;
        break;
    default:
        return 0;
    }

    if (backend == WinApiRelativeBackend::mouse_event) {
        ::mouse_event(flags, 0, 0, mouse_data, 0);
        return 0;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = mouse_data;
    input.mi.dwFlags = flags;
    return ::SendInput(1, &input, sizeof(INPUT));
}

bool emit_virtual_key_winapi(std::uint16_t virtual_key,
                             bool pressed) noexcept {
    if (virtual_key == 0)
        return false;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.wScan = 0;
    input.ki.dwFlags = pressed ? 0 : KEYEVENTF_KEYUP;
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;
    return ::SendInput(1, &input, sizeof(INPUT)) == 1;
}

} // namespace cvm::recovered
