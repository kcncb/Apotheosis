#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cvm::recovered {

// Native channel numbering used by the +0x1d0 backend virtual method.
enum class MouseButtonChannel : std::uint8_t {
    left = 0,
    right = 1,
    middle = 2,
    x1 = 3,
    x2 = 4,
};

enum class NativeButtonCommandKind : std::uint8_t {
    set_state = 0,
    timed_press = 1,
};

// Eight-byte deque element accepted by sub_140067A70.  byte3 is ABI padding
// and is never read by native code.
struct NativeTimedButtonCommandAbi {
    NativeButtonCommandKind kind{}; // +0
    std::uint8_t channel{};          // +1, MouseButtonChannel
    std::uint8_t desired_or_timed{}; // +2: desired state, or 1 for timed press
    std::uint8_t _padding03{};       // +3, ignored
    std::int32_t duration_ms{};      // +4, clamped to at least 1 for timed_press
};

// Eight-byte vector element emitted by sub_140067810/sub_140067A70 and
// consumed by sub_140068440.
struct NativeTimedButtonEmitAbi {
    std::uint8_t channel{};          // +0
    std::uint8_t pressed{};          // +1
    std::uint8_t arm_timer{};        // +2
    std::uint8_t _padding03{};       // +3, native stack byte is unspecified
    std::int32_t duration_ms{};      // +4
};

// One of the five 16-byte slots at native object +264..+343.
struct NativeTimedButtonSlotAbi {
    std::uint8_t timer_active{};     // +0: deadline is armed
    std::uint8_t pending_arm{};      // +1: down action emitted-list not acked yet
    std::uint16_t _padding02{};
    std::int32_t pending_ms{};       // +4: extensions received before ack
    std::int64_t deadline_ns{};      // +8: steady-clock absolute ns
};

struct TimedButtonRuntimeExact {
    std::array<NativeTimedButtonSlotAbi, 5> slots{};
    std::uint8_t pressed_mask{};     // native object +400
};

// sub_140067810: expire armed deadlines, clear the pressed mask, and emit
// release actions.  pending_arm records intentionally have no deadline yet.
std::vector<NativeTimedButtonEmitAbi> collect_expired_button_actions(
    TimedButtonRuntimeExact& runtime,
    std::int64_t now_ns);

// sub_140067A70: consume queued manual/timed commands and build the backend
// action vector.  Valid native callers guarantee channel in [0,4].
std::vector<NativeTimedButtonEmitAbi> process_timed_button_commands(
    TimedButtonRuntimeExact& runtime,
    std::span<const NativeTimedButtonCommandAbi> commands,
    std::int64_t now_ns);

// State-update half of sub_140068440, run after its backend virtual call.
// A timed down changes pending_arm into timer_active; queued extensions are
// merged by max(), not added.
void acknowledge_emitted_button_actions(
    TimedButtonRuntimeExact& runtime,
    std::span<const NativeTimedButtonEmitAbi> actions,
    std::int64_t emitted_at_ns);

} // namespace cvm::recovered

static_assert(sizeof(cvm::recovered::NativeTimedButtonCommandAbi) == 8);
static_assert(offsetof(cvm::recovered::NativeTimedButtonCommandAbi, duration_ms) == 4);
static_assert(sizeof(cvm::recovered::NativeTimedButtonEmitAbi) == 8);
static_assert(offsetof(cvm::recovered::NativeTimedButtonEmitAbi, duration_ms) == 4);
static_assert(sizeof(cvm::recovered::NativeTimedButtonSlotAbi) == 16);
static_assert(offsetof(cvm::recovered::NativeTimedButtonSlotAbi, deadline_ns) == 8);
