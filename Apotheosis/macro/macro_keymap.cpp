#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "macro_keymap.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace macro {
namespace {

// Lowercased Logitech-style key name → VK_* code. Mirrors the documented
// LGS / G HUB API:
//   https://douile.github.io/logitech-toggle-keys/APIDocs.html
// Synonyms ("ctrl"/"lctrl"/"leftctrl") are all included so users can paste
// any historical script and it just works. Names are stored lowercase; the
// resolver lowercases input before lookup.
const std::unordered_map<std::string, int>& name_table()
{
    static const std::unordered_map<std::string, int> t = {
        // Modifiers
        {"lshift", VK_LSHIFT},  {"leftshift", VK_LSHIFT},
        {"rshift", VK_RSHIFT},  {"rightshift", VK_RSHIFT},
        {"lctrl",  VK_LCONTROL},{"leftctrl",  VK_LCONTROL}, {"ctrl", VK_LCONTROL},
        {"rctrl",  VK_RCONTROL},{"rightctrl", VK_RCONTROL},
        {"lalt",   VK_LMENU},   {"leftalt",   VK_LMENU},    {"alt", VK_LMENU},
        {"ralt",   VK_RMENU},   {"rightalt",  VK_RMENU},
        {"lgui",   VK_LWIN},    {"leftgui",   VK_LWIN},
        {"rgui",   VK_RWIN},    {"rightgui",  VK_RWIN},
        {"lwin",   VK_LWIN},    {"rwin",      VK_RWIN},

        // Special
        {"escape", VK_ESCAPE},  {"esc",       VK_ESCAPE},
        {"tab",    VK_TAB},
        {"capslock", VK_CAPITAL}, {"caps",    VK_CAPITAL},
        {"spacebar", VK_SPACE},   {"space",   VK_SPACE},
        {"backspace", VK_BACK},   {"bksp",    VK_BACK},
        {"enter",  VK_RETURN},  {"return",    VK_RETURN},
        {"insert", VK_INSERT},  {"ins",       VK_INSERT},
        {"delete", VK_DELETE},  {"del",       VK_DELETE},
        {"home",   VK_HOME},
        {"end",    VK_END},
        {"pageup", VK_PRIOR},   {"pgup",      VK_PRIOR},
        {"pagedown", VK_NEXT},  {"pgdn",      VK_NEXT},
        {"printscreen", VK_SNAPSHOT}, {"prtsc", VK_SNAPSHOT},
        {"scrolllock",  VK_SCROLL},   {"scroll",VK_SCROLL},
        {"pause",  VK_PAUSE},   {"break",     VK_PAUSE},
        {"numlock",VK_NUMLOCK},

        // Arrows
        {"up",     VK_UP},      {"uparrow",   VK_UP},
        {"down",   VK_DOWN},    {"downarrow", VK_DOWN},
        {"left",   VK_LEFT},    {"leftarrow", VK_LEFT},
        {"right",  VK_RIGHT},   {"rightarrow",VK_RIGHT},

        // F1..F24
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"f13", VK_F13}, {"f14", VK_F14}, {"f15", VK_F15}, {"f16", VK_F16},
        {"f17", VK_F17}, {"f18", VK_F18}, {"f19", VK_F19}, {"f20", VK_F20},
        {"f21", VK_F21}, {"f22", VK_F22}, {"f23", VK_F23}, {"f24", VK_F24},

        // Numpad
        {"num0", VK_NUMPAD0}, {"num1", VK_NUMPAD1}, {"num2", VK_NUMPAD2},
        {"num3", VK_NUMPAD3}, {"num4", VK_NUMPAD4}, {"num5", VK_NUMPAD5},
        {"num6", VK_NUMPAD6}, {"num7", VK_NUMPAD7}, {"num8", VK_NUMPAD8},
        {"num9", VK_NUMPAD9},
        {"numpad0", VK_NUMPAD0}, {"numpad1", VK_NUMPAD1}, {"numpad2", VK_NUMPAD2},
        {"numpad3", VK_NUMPAD3}, {"numpad4", VK_NUMPAD4}, {"numpad5", VK_NUMPAD5},
        {"numpad6", VK_NUMPAD6}, {"numpad7", VK_NUMPAD7}, {"numpad8", VK_NUMPAD8},
        {"numpad9", VK_NUMPAD9},
        {"numplus", VK_ADD},      {"numpadadd", VK_ADD},
        {"numminus", VK_SUBTRACT},{"numpadsub", VK_SUBTRACT},
        {"nummult", VK_MULTIPLY}, {"numpadmult",VK_MULTIPLY},
        {"numdiv",  VK_DIVIDE},   {"numpaddiv", VK_DIVIDE},
        {"numdot",  VK_DECIMAL},  {"numpaddot", VK_DECIMAL},
        {"numenter",VK_RETURN},

        // OEM punctuation (US layout) — what most G HUB scripts assume.
        {";",  VK_OEM_1}, {":", VK_OEM_1},
        {"=",  VK_OEM_PLUS}, {"+", VK_OEM_PLUS},
        {",",  VK_OEM_COMMA},
        {"-",  VK_OEM_MINUS}, {"_", VK_OEM_MINUS},
        {".",  VK_OEM_PERIOD},
        {"/",  VK_OEM_2}, {"?", VK_OEM_2},
        {"`",  VK_OEM_3}, {"~", VK_OEM_3},
        {"[",  VK_OEM_4}, {"{", VK_OEM_4},
        {"\\", VK_OEM_5}, {"|", VK_OEM_5},
        {"]",  VK_OEM_6}, {"}", VK_OEM_6},
        {"'",  VK_OEM_7}, {"\"",VK_OEM_7},
    };
    return t;
}

std::string lower_copy(const std::string& s)
{
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return out;
}

} // namespace

int resolve_key_name(const std::string& raw_name)
{
    if (raw_name.empty()) return -1;

    const std::string n = lower_copy(raw_name);

    // Single letter a..z
    if (n.size() == 1)
    {
        char c = n[0];
        if (c >= 'a' && c <= 'z')
            return 'A' + (c - 'a');
        if (c >= '0' && c <= '9')
            return '0' + (c - '0');
    }

    const auto& t = name_table();
    auto it = t.find(n);
    if (it != t.end())
        return it->second;

    return -1;
}

int resolve_key_name_or_code(const std::string& s)
{
    if (s.empty()) return -1;

    // Numeric form ("0x41" or "65")?
    bool numeric = true;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)) && c != 'x' && c != 'X') {
            numeric = false; break;
        }
    }
    if (numeric)
    {
        try {
            int base = (s.find('x') != std::string::npos || s.find('X') != std::string::npos) ? 16 : 10;
            int v = std::stoi(s, nullptr, base);
            if (v > 0 && v <= 0xFF)
                return v;
        } catch (...) { /* fall through */ }
    }

    return resolve_key_name(s);
}

int mouse_button_to_vk(int button)
{
    switch (button) {
        case 1: return VK_LBUTTON;
        case 2: return VK_RBUTTON;
        case 3: return VK_MBUTTON;
        case 4: return VK_XBUTTON1;
        case 5: return VK_XBUTTON2;
        default: return 0;
    }
}

} // namespace macro
