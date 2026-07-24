#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ava::hotkey {

// Canonical mouse indices used by sub_1403E7BD0 and the actuator vtable slot
// +0xC8.  WinApiActuator::query_mouse_button (sub_14037A720) maps these to
// VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1 and VK_XBUTTON2.
enum class MouseButton : std::uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,
    X2 = 4,
};

struct InputEnvironment {
    // dword_140BD7E54. Modes 1..7 use the actuator's mouse-button query;
    // mode 0/unknown falls back to GetAsyncKeyState through key_to_vk.
    int backend_mode = 0;
    bool actuator_present = false;

    // Actuator vtable +0x08.
    std::function<bool()> actuator_ready;

    // Actuator vtable +0xC8. Return value means the backend handled the query;
    // down receives the current state.
    std::function<bool(MouseButton, bool& down)> query_mouse_button;

    // Actuator vtable +0x110. Used for non-mouse key strings when available.
    std::function<bool(std::string_view canonical_key, bool& down)> query_key_string;

    // Win32 GetAsyncKeyState-compatible callback. Only bit 0x8000 is observed.
    std::function<std::uint16_t(int virtual_key)> get_async_key_state;
};

// sub_1403D8A60 / sub_1403D7FE0 / sub_1403D7650 semantic reconstruction.
std::string normalize_key_expression(std::string_view input);

// sub_1403D8BA0. A normalized combo is exactly Combo(lhs+rhs).
std::optional<std::pair<std::string, std::string>> split_combo(
    std::string_view normalized);

// sub_1403E7BD0.
std::optional<MouseButton> parse_mouse_button(std::string_view canonical_key);

// The exact 123-entry name -> Win32 VK table constructed by sub_1400011F0.
const std::unordered_map<std::string, int>& key_to_vk();

// sub_1403EA3F0 semantic reconstruction. Combo operands are ANDed and use
// normal C++ short-circuit order, matching the recovered code.
bool is_key_expression_down(std::string_view expression,
                            const InputEnvironment& environment);

}  // namespace ava::hotkey
