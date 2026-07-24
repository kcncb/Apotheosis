#pragma once

#include "hotkey_eval_exact.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ava::hotkey {

struct AimKeyRuntimeView {
    // Runtime +0x3F0/+0x3F8 (1008/1016).
    std::vector<std::string> configured_aim_keys;

    // Runtime +0x408 (1032). Value 1 selects by configured order; all other
    // values use the most recently appended active key.
    int key_selection_mode = 0;

    // sub_1403F0900: normalized key -> profile node +0x166 (358).
    std::function<bool(std::string_view)> profile_enabled;

    // sub_1403EA9F0. A true result blocks a newly observed press, but an
    // already tracked held key remains eligible until release.
    std::function<bool(std::string_view)> block_new_press;
};

struct AimKeySelectionState {
    // a4 in sub_1403F1CC0: insertion-ordered normalized keys currently held.
    std::vector<std::string> active_keys;

    // a5: membership set paired with active_keys.
    std::unordered_set<std::string> active_key_set;

    // a6: per-key previous down state used for rising-edge detection.
    std::unordered_map<std::string, bool> previous_down;

    // a7: persistent toggle-selected profile key.
    std::string selected_key;

    // a8: timestamp of the last 20 ms reconciliation pass.
    std::int64_t last_reconcile_ns = 0;
};

// Exact 72-byte logical return layout of sub_1403F1CC0 after replacing the two
// MSVC std::string objects with portable C++ strings.
struct AimKeySelectionResult {
    bool has_active_profile = false;       // original +0
    bool resolved_profile = false;         // original +1
    bool result_valid = true;              // original +2
    bool selected_profile_valid = false;   // original +3
    std::string active_profile_key;         // original +8
    std::string selected_profile_key;       // original +40
};

// sub_1403F1060: remove simple keys shadowed by active Combo operands, dedupe,
// then select by configured order (mode 1) or latest active key (other modes).
std::string choose_active_aim_key(const AimKeyRuntimeView& runtime,
                                  const std::vector<std::string>& active_keys,
                                  const std::unordered_set<std::string>&
                                      active_key_set);

// sub_1403F1CC0 semantic reconstruction.
AimKeySelectionResult update_aim_key_selection(
    AimKeySelectionState& state,
    const AimKeyRuntimeView& runtime,
    const InputEnvironment& input,
    std::int64_t now_ns);

}  // namespace ava::hotkey
