#ifndef MACRO_KEYMAP_H
#define MACRO_KEYMAP_H

#include <string>

namespace macro {

// Resolve a Logitech-style key name to a Win32 VK code. Accepts the LGS /
// G HUB string-name set (e.g. "lalt", "f1", "spacebar") plus single-letter
// shortcuts ("a".."z", "0".."9"). Case-insensitive. Returns -1 when the
// name is unknown.
int resolve_key_name(const std::string& name);

// Same as above but also accepts an integer (already a VK code). Pulled out
// so the Lua API functions can do `if lua_isnumber → int else lookup`.
// Returns -1 when unrecognized.
int resolve_key_name_or_code(const std::string& name);

// Mouse-button index 1..5 → corresponding VK_LBUTTON etc. for polling. The
// G HUB convention is 1=left, 2=right, 3=middle, 4=X1 (back), 5=X2 (forward).
// Returns 0 on out-of-range.
int mouse_button_to_vk(int button);

} // namespace macro

#endif // MACRO_KEYMAP_H
