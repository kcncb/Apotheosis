#include "aim_key_selector.hpp"

#include <algorithm>

namespace ava::hotkey {
namespace {

bool is_none(std::string_view key) {
    return key.empty() || key == "NONE" || key == "None";
}

bool profile_enabled(const AimKeyRuntimeView& runtime, std::string_view key) {
    return runtime.profile_enabled && runtime.profile_enabled(key);
}

bool configured(const AimKeyRuntimeView& runtime, std::string_view key) {
    if (is_none(key)) return false;
    return std::any_of(runtime.configured_aim_keys.begin(),
                       runtime.configured_aim_keys.end(),
                       [&](const std::string& item) {
                           return normalize_key_expression(item) == key;
                       });
}

// sub_1403EAC50.
bool pressed_and_eligible(const AimKeyRuntimeView& runtime,
                          const InputEnvironment& input,
                          const std::unordered_set<std::string>& already_active,
                          std::string_view key) {
    if (!is_key_expression_down(key, input)) return false;
    if (already_active.contains(std::string(key))) return true;
    return !runtime.block_new_press || !runtime.block_new_press(key);
}

void remove_active(AimKeySelectionState& state, std::string_view key) {
    state.active_key_set.erase(std::string(key));
    state.active_keys.erase(
        std::remove(state.active_keys.begin(), state.active_keys.end(), key),
        state.active_keys.end());
}

void insert_active(AimKeySelectionState& state, const std::string& key) {
    if (state.active_key_set.insert(key).second) {
        state.active_keys.push_back(key);
    }
}

std::vector<std::string> filtered_candidates(
    const std::vector<std::string>& keys,
    const std::unordered_set<std::string>& active_key_set) {
    std::unordered_set<std::string> combo_operands;
    for (const std::string& raw : active_key_set) {
        const std::string key = normalize_key_expression(raw);
        if (const auto combo = split_combo(key)) {
            combo_operands.insert(combo->first);
            combo_operands.insert(combo->second);
        }
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> result;
    result.reserve(keys.size());
    for (const std::string& raw : keys) {
        const std::string key = normalize_key_expression(raw);
        if (is_none(key)) continue;
        if (!split_combo(key) && combo_operands.contains(key)) continue;
        if (seen.insert(key).second) result.push_back(key);
    }
    return result;
}

}  // namespace

std::string choose_active_aim_key(
    const AimKeyRuntimeView& runtime,
    const std::vector<std::string>& active_keys,
    const std::unordered_set<std::string>& active_key_set) {
    const std::vector<std::string> candidates =
        filtered_candidates(active_keys, active_key_set);
    if (candidates.empty()) return {};

    if (runtime.key_selection_mode == 1 &&
        !runtime.configured_aim_keys.empty()) {
        const std::unordered_set<std::string> candidate_set(candidates.begin(),
                                                            candidates.end());
        const std::vector<std::string> ordered =
            filtered_candidates(runtime.configured_aim_keys, active_key_set);
        for (const std::string& key : ordered) {
            if (candidate_set.contains(key)) return key;
        }
    }
    return candidates.back();
}

AimKeySelectionResult update_aim_key_selection(
    AimKeySelectionState& state,
    const AimKeyRuntimeView& runtime,
    const InputEnvironment& input,
    std::int64_t now_ns) {
    if (!state.selected_key.empty() &&
        (!configured(runtime, state.selected_key) ||
         !profile_enabled(runtime, state.selected_key))) {
        state.selected_key.clear();
    }

    // 0x1403F1D95..0x1403F2270: periodic reconciliation every 20,000,000 ns.
    if (now_ns - state.last_reconcile_ns >= 20'000'000) {
        state.last_reconcile_ns = now_ns;
        const std::vector<std::string> snapshot = state.active_keys;
        for (const std::string& key : snapshot) {
            if (!pressed_and_eligible(runtime, input, state.active_key_set, key)) {
                remove_active(state, key);
            }
        }
        for (const std::string& raw : runtime.configured_aim_keys) {
            const std::string key = normalize_key_expression(raw);
            if (is_none(key) || state.active_key_set.contains(key)) continue;
            if (pressed_and_eligible(runtime, input, state.active_key_set, key)) {
                insert_active(state, key);
            }
        }
    }

    // 0x1403F22A3..0x1403F276C: frame-rate path and rising-edge toggle.
    for (const std::string& raw : runtime.configured_aim_keys) {
        const std::string key = normalize_key_expression(raw);
        if (is_none(key)) continue;

        const bool down =
            pressed_and_eligible(runtime, input, state.active_key_set, key);
        const bool enabled = profile_enabled(runtime, key);
        bool& previous = state.previous_down[key];

        if (enabled && down && !previous) {
            if (state.selected_key == key) {
                state.selected_key.clear();
            } else {
                state.selected_key = key;
            }
        }
        previous = down;

        if (down) {
            insert_active(state, key);
        } else {
            remove_active(state, key);
        }
    }

    const bool any_held = !state.active_keys.empty();
    const bool selected_valid =
        !state.selected_key.empty() && configured(runtime, state.selected_key) &&
        profile_enabled(runtime, state.selected_key);

    AimKeySelectionResult result;
    result.has_active_profile = any_held || selected_valid;
    result.resolved_profile = result.has_active_profile;
    result.result_valid = true;
    result.selected_profile_valid = selected_valid;
    if (any_held) {
        result.active_profile_key = choose_active_aim_key(
            runtime, state.active_keys, state.active_key_set);
    } else if (selected_valid) {
        result.active_profile_key = state.selected_key;
    }
    if (selected_valid) result.selected_profile_key = state.selected_key;
    return result;
}

}  // namespace ava::hotkey
