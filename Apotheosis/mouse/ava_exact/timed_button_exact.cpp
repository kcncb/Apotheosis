#include "timed_button_exact.hpp"

#include <algorithm>

namespace cvm::recovered {
namespace {

constexpr std::int64_t ns_per_ms = 1'000'000;

std::int32_t native_duration(std::int32_t duration_ms) noexcept {
    return duration_ms > 0 ? duration_ms : 1;
}

void clear_slot(NativeTimedButtonSlotAbi& slot) noexcept {
    slot = {};
}

} // namespace

std::vector<NativeTimedButtonEmitAbi> collect_expired_button_actions(
    TimedButtonRuntimeExact& runtime,
    std::int64_t now_ns) {
    std::vector<NativeTimedButtonEmitAbi> output;
    output.reserve(5);
    for (std::uint8_t channel = 0; channel < 5; ++channel) {
        auto& slot = runtime.slots[channel];
        if (!slot.timer_active || now_ns < slot.deadline_ns)
            continue;

        clear_slot(slot);
        const auto bit = static_cast<std::uint8_t>(1u << channel);
        if ((runtime.pressed_mask & bit) == 0)
            continue;
        runtime.pressed_mask = static_cast<std::uint8_t>(
            runtime.pressed_mask & static_cast<std::uint8_t>(~bit));
        output.push_back({channel, 0, 0, 0, 0});
    }
    return output;
}

std::vector<NativeTimedButtonEmitAbi> process_timed_button_commands(
    TimedButtonRuntimeExact& runtime,
    std::span<const NativeTimedButtonCommandAbi> commands,
    std::int64_t now_ns) {
    std::vector<NativeTimedButtonEmitAbi> output;
    output.reserve(commands.size());

    for (const auto& command : commands) {
        const std::uint8_t channel = command.channel;
        auto& slot = runtime.slots[channel]; // native precondition: channel < 5
        const auto bit = static_cast<std::uint8_t>(1u << channel);
        const bool pressed = (runtime.pressed_mask & bit) != 0;

        if (command.kind == NativeButtonCommandKind::timed_press) {
            const std::int32_t duration = native_duration(command.duration_ms);
            if (slot.pending_arm && pressed) {
                slot.pending_ms = std::max(slot.pending_ms, duration);
            } else if (slot.timer_active && pressed) {
                slot.deadline_ns = std::max(
                    slot.deadline_ns,
                    now_ns + ns_per_ms * static_cast<std::int64_t>(duration));
            } else {
                clear_slot(slot);
                if (!pressed) {
                    runtime.pressed_mask = static_cast<std::uint8_t>(
                        runtime.pressed_mask | bit);
                    slot.pending_arm = 1;
                    slot.pending_ms = duration;
                    output.push_back({channel, 1, 1, 0, duration});
                }
            }
            continue;
        }

        const bool desired = command.desired_or_timed != 0;
        if (desired) {
            // A manual down cancels an existing timer and leaves an already
            // pressed channel down without emitting a duplicate event.
            clear_slot(slot);
            if (!pressed) {
                runtime.pressed_mask = static_cast<std::uint8_t>(
                    runtime.pressed_mask | bit);
                output.push_back({channel, 1, 0, 0, 0});
            }
        } else if (slot.timer_active || slot.pending_arm) {
            // A manual up is suppressed while a timed press owns the channel.
            // The native branch only clears stale timer bytes when the mask is
            // already off.
            if (!pressed)
                clear_slot(slot);
        } else {
            clear_slot(slot);
            if (pressed) {
                runtime.pressed_mask = static_cast<std::uint8_t>(
                    runtime.pressed_mask & static_cast<std::uint8_t>(~bit));
                output.push_back({channel, 0, 0, 0, 0});
            }
        }
    }
    return output;
}

void acknowledge_emitted_button_actions(
    TimedButtonRuntimeExact& runtime,
    std::span<const NativeTimedButtonEmitAbi> actions,
    std::int64_t emitted_at_ns) {
    for (const auto& action : actions) {
        if (!action.arm_timer || !action.pressed)
            continue;

        const std::uint8_t channel = action.channel;
        auto& slot = runtime.slots[channel];
        const auto bit = static_cast<std::uint8_t>(1u << channel);
        if (!slot.pending_arm || (runtime.pressed_mask & bit) == 0)
            continue;

        const std::int32_t duration = std::max(
            slot.pending_ms, native_duration(action.duration_ms));
        slot.timer_active = 1;
        slot.pending_arm = 0;
        slot.pending_ms = 0;
        slot.deadline_ns = emitted_at_ns
            + ns_per_ms * static_cast<std::int64_t>(duration);
    }
}

} // namespace cvm::recovered
