#include "hotkey_eval_exact.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ava::hotkey {
namespace {

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    for (char& c : result) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string normalize_simple(std::string_view input) {
    std::string value = trim(input);
    const std::string lower = lowercase(value);

    if (lower.empty() || lower == "none" || lower == "null" || lower == "off") {
        return "NONE";
    }

    if (lower == "m4" || lower == "x1" || lower == "mouse_x1") {
        return "X1MouseButton";
    }
    if (lower == "m5" || lower == "x2" || lower == "mouse_x2") {
        return "X2MouseButton";
    }
    if (lower == "lm" || lower == "mouse_left") {
        return "LeftMouseButton";
    }
    if (lower == "rm" || lower == "mouse_right") {
        return "RightMouseButton";
    }
    if (lower == "mm" || lower == "mouse_middle") {
        return "MiddleMouseButton";
    }

    constexpr std::string_view kKeyboardColon = "keyboard:";
    constexpr std::string_view kKeyboardUnderscore = "keyboard_";
    if (lower.starts_with(kKeyboardColon) ||
        lower.starts_with(kKeyboardUnderscore)) {
        value = trim(std::string_view(value).substr(9));
        if (value.size() == 1) {
            const unsigned char c = static_cast<unsigned char>(value[0]);
            if (std::isalpha(c) != 0) {
                value[0] = static_cast<char>(std::toupper(c));
            } else if (std::isdigit(c) != 0) {
                value = "Key" + value;
            }
        }
    }
    return value;
}

bool actuator_ready(const InputEnvironment& environment) {
    return environment.actuator_present && environment.actuator_ready &&
           environment.actuator_ready();
}

// sub_1403EA770: returns whether the actuator path handled the query.
bool try_actuator(std::string_view canonical,
                  const InputEnvironment& environment,
                  bool& down) {
    down = false;
    const auto mouse = parse_mouse_button(canonical);
    if (mouse) {
        if (environment.backend_mode < 1 || environment.backend_mode > 7) {
            return false;
        }
        // For modes 1..7 a recognized mouse button is considered handled even
        // when the actuator is absent/not ready; the output remains false.
        if (actuator_ready(environment) && environment.query_mouse_button) {
            if (!environment.query_mouse_button(*mouse, down)) {
                down = false;
            }
        }
        return true;
    }

    if (!actuator_ready(environment) || !environment.query_key_string) {
        return false;
    }
    if (!environment.query_key_string(canonical, down)) {
        down = false;
    }
    return true;
}

}  // namespace

std::string normalize_key_expression(std::string_view input) {
    std::string value = trim(input);
    const std::string lower = lowercase(value);
    if (lower.size() >= 8 && lower.starts_with("combo(") &&
        lower.back() == ')') {
        const std::string_view inner(value.data() + 6, value.size() - 7);
        const std::size_t plus = inner.find('+');
        // sub_1403D7FE0 accepts exactly one separator.
        if (plus != std::string_view::npos &&
            inner.find('+', plus + 1) == std::string_view::npos) {
            std::string left = normalize_simple(inner.substr(0, plus));
            std::string right = normalize_simple(inner.substr(plus + 1));
            if (left != "NONE" && right != "NONE") {
                if (right < left) {
                    std::swap(left, right);
                }
                return "Combo(" + left + "+" + right + ")";
            }
        }
        // Invalid Combo(...) input is normalized to an empty expression by
        // sub_1403D7FE0.
        return {};
    }
    return normalize_simple(value);
}

std::optional<std::pair<std::string, std::string>> split_combo(
    std::string_view normalized) {
    if (normalized.size() < 8 || !normalized.starts_with("Combo(") ||
        normalized.back() != ')') {
        return std::nullopt;
    }
    const std::string_view inner = normalized.substr(6, normalized.size() - 7);
    const std::size_t plus = inner.find('+');
    if (plus == std::string_view::npos) {
        return std::nullopt;
    }
    std::string left(inner.substr(0, plus));
    std::string right(inner.substr(plus + 1));
    if (left.empty() || right.empty()) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::move(left),
                                               std::move(right)};
}

std::optional<MouseButton> parse_mouse_button(std::string_view key) {
    if (key == "LeftMouseButton") return MouseButton::Left;
    if (key == "RightMouseButton") return MouseButton::Right;
    if (key == "MiddleMouseButton") return MouseButton::Middle;
    if (key == "X1MouseButton") return MouseButton::X1;
    if (key == "X2MouseButton") return MouseButton::X2;
    return std::nullopt;
}

bool is_key_expression_down(std::string_view expression,
                            const InputEnvironment& environment) {
    const std::string canonical = normalize_key_expression(expression);
    if (canonical.empty() || canonical == "NONE" || canonical == "None") {
        return false;
    }

    if (const auto combo = split_combo(canonical)) {
        return is_key_expression_down(combo->first, environment) &&
               is_key_expression_down(combo->second, environment);
    }

    bool down = false;
    if (try_actuator(canonical, environment, down)) {
        return down;
    }

    const auto item = key_to_vk().find(canonical);
    if (item == key_to_vk().end() || item->second <= 0 ||
        !environment.get_async_key_state) {
        return false;
    }
    return (environment.get_async_key_state(item->second) & 0x8000u) != 0;
}

}  // namespace ava::hotkey
