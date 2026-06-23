#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "ghub_api.h"
#include "lua_runtime.h"
#include "macro_input.h"
#include "macro_keymap.h"

namespace macro {

namespace {

// Resolve "vk number, key name string, or scancode-style hex" from arg i.
// Returns -1 if the value at the index can't be turned into a key code.
int resolve_arg_to_vk(lua_State* L, int idx)
{
    if (lua_isnumber(L, idx))
    {
        int v = static_cast<int>(lua_tointeger(L, idx));
        if (v > 0 && v <= 0xFF) return v;
        return -1;
    }
    if (lua_isstring(L, idx))
    {
        const char* s = lua_tostring(L, idx);
        if (!s) return -1;
        return resolve_key_name_or_code(s);
    }
    return -1;
}

// Cancellable sleep — yields in small chunks so a pending runtime_stop()
// can interrupt a long Sleep() instead of pinning shutdown. On detection
// of stop_requested we throw a Lua error so the OnEvent stack unwinds
// out of the worker.
void cancellable_sleep_ms(lua_State* L, int ms)
{
    if (ms <= 0) return;
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(ms);
    while (steady_clock::now() < deadline)
    {
        if (runtime_stop_requested() || runtime_abort_pending())
        {
            luaL_error(L, "macro runtime aborting");
            return; // unreachable
        }
        auto remaining = deadline - steady_clock::now();
        auto chunk = (std::min)(duration_cast<milliseconds>(remaining), milliseconds(5));
        if (chunk.count() <= 0) break;
        std::this_thread::sleep_for(chunk);
    }
}

// All registered Lua callbacks must have C linkage to match
// lua_CFunction's signature under /permissive-. The wrapping extern "C"
// block gives them that linkage while keeping them at internal-linkage
// (the surrounding `namespace { }` strips them from the global symbol
// table). They still call C++ helpers above freely — calling C++ from
// C-linkage functions is well-defined as long as no exception escapes,
// which Lua callbacks never do (errors propagate via setjmp/longjmp).
extern "C" {

// ---------------- API: motion ----------------

int api_MoveMouseRelative(lua_State* L)
{
    int dx = static_cast<int>(luaL_optnumber(L, 1, 0));
    int dy = static_cast<int>(luaL_optnumber(L, 2, 0));
    dispatch_move(dx, dy);
    return 0;
}

int api_MoveMouseTo(lua_State* L)
{
    // G HUB normalises both axes to 0..65535 across the virtual screen.
    double xn = luaL_optnumber(L, 1, 0);
    double yn = luaL_optnumber(L, 2, 0);
    if (xn < 0) xn = 0; if (xn > 65535) xn = 65535;
    if (yn < 0) yn = 0; if (yn > 65535) yn = 65535;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = static_cast<int>(xn / 65535.0 * (sw - 1));
    int y = static_cast<int>(yn / 65535.0 * (sh - 1));
    dispatch_move_abs(x, y);
    return 0;
}

int api_MoveMouseWheel(lua_State* L)
{
    int clicks = static_cast<int>(luaL_optnumber(L, 1, 0));
    dispatch_wheel(clicks);
    return 0;
}

// ---------------- API: buttons ----------------

int api_PressMouseButton(lua_State* L)
{
    int btn = static_cast<int>(luaL_checkinteger(L, 1));
    dispatch_button_down(btn);
    return 0;
}

int api_ReleaseMouseButton(lua_State* L)
{
    int btn = static_cast<int>(luaL_checkinteger(L, 1));
    dispatch_button_up(btn);
    return 0;
}

int api_PressAndReleaseMouseButton(lua_State* L)
{
    int btn = static_cast<int>(luaL_checkinteger(L, 1));
    dispatch_button_down(btn);
    // Plain sleep here (not cancellable) so a runtime abort can't longjmp
    // out between down/up and leave the button latched.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    dispatch_button_up(btn);
    return 0;
}

// ---------------- API: keys (variadic) ----------------

int api_PressKey(lua_State* L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i)
    {
        int vk = resolve_arg_to_vk(L, i);
        if (vk > 0) dispatch_key_down(vk);
    }
    return 0;
}

int api_ReleaseKey(lua_State* L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i)
    {
        int vk = resolve_arg_to_vk(L, i);
        if (vk > 0) dispatch_key_up(vk);
    }
    return 0;
}

int api_PressAndReleaseKey(lua_State* L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i)
    {
        int vk = resolve_arg_to_vk(L, i);
        if (vk > 0) dispatch_key_down(vk);
    }
    // See api_PressAndReleaseMouseButton — non-cancellable so a key can't
    // be left latched if the runtime is asked to abort mid-tap.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    for (int i = 1; i <= n; ++i)
    {
        int vk = resolve_arg_to_vk(L, i);
        if (vk > 0) dispatch_key_up(vk);
    }
    return 0;
}

// ---------------- API: timing ----------------

int api_Sleep(lua_State* L)
{
    int ms = static_cast<int>(luaL_optnumber(L, 1, 0));
    cancellable_sleep_ms(L, ms);
    return 0;
}

int api_GetRunningTime(lua_State* L)
{
    lua_pushnumber(L, static_cast<lua_Number>(runtime_time_ms()));
    return 1;
}

// ---------------- API: state queries ----------------

int api_IsMouseButtonPressed(lua_State* L)
{
    int btn = static_cast<int>(luaL_checkinteger(L, 1));
    int vk = mouse_button_to_vk(btn);
    bool pressed = (vk != 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0);
    lua_pushboolean(L, pressed ? 1 : 0);
    return 1;
}

int api_IsModifierPressed(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    if (!name) { lua_pushboolean(L, 0); return 1; }

    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    bool pressed = false;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };

    if (n == "ctrl" || n == "lctrl") pressed = down(VK_LCONTROL);
    else if (n == "rctrl")           pressed = down(VK_RCONTROL);
    else if (n == "shift" || n == "lshift") pressed = down(VK_LSHIFT);
    else if (n == "rshift")          pressed = down(VK_RSHIFT);
    else if (n == "alt" || n == "lalt") pressed = down(VK_LMENU);
    else if (n == "ralt")            pressed = down(VK_RMENU);

    lua_pushboolean(L, pressed ? 1 : 0);
    return 1;
}

int api_IsKeyLockOn(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    if (!name) { lua_pushboolean(L, 0); return 1; }

    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    int vk = 0;
    if (n == "capslock" || n == "caps") vk = VK_CAPITAL;
    else if (n == "numlock") vk = VK_NUMLOCK;
    else if (n == "scrolllock" || n == "scroll") vk = VK_SCROLL;

    bool on = (vk != 0) && ((GetKeyState(vk) & 0x0001) != 0);
    lua_pushboolean(L, on ? 1 : 0);
    return 1;
}

int api_GetMousePosition(lua_State* L)
{
    POINT p{};
    GetCursorPos(&p);
    lua_pushinteger(L, p.x);
    lua_pushinteger(L, p.y);
    return 2;
}

// ---------------- API: log ----------------

// Replicates G HUB's printf-style API. Accepts a single string OR a format
// string + arguments; the latter delegates to string.format inside Lua so
// %s / %d / %.2f all work without us reimplementing printf.
int api_OutputLogMessage(lua_State* L)
{
    int n = lua_gettop(L);
    if (n == 0) return 0;

    if (n == 1)
    {
        const char* s = lua_tostring(L, 1);
        if (s) runtime_log(s);
        return 0;
    }

    // Defer to string.format for the multi-arg form.
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "format");
    lua_remove(L, -2);
    lua_insert(L, 1);

    if (lua_pcall(L, n, 1, 0) != 0)
    {
        const char* err = lua_tostring(L, -1);
        runtime_log(std::string("[Lua] OutputLogMessage format failure: ") +
                    (err ? err : "(unknown)"));
        lua_pop(L, 1);
        return 0;
    }

    const char* result = lua_tostring(L, -1);
    if (result) runtime_log(result);
    return 0;
}

int api_OutputDebugMessage(lua_State* L)
{
    return api_OutputLogMessage(L);
}

int api_ClearLog(lua_State* /*L*/)
{
    runtime_log_clear();
    return 0;
}

// ---------------- API: configuration ----------------

int api_EnablePrimaryMouseButtonEvents(lua_State* L)
{
    bool v = lua_toboolean(L, 1) != 0;
    runtime_set_primary_button_events_enabled(v);
    return 0;
}

// G HUB's M-keys / G-keys are physical only on Logitech keyboards. Our
// implementation maintains a software-only state slot so scripts that
// poll/set it don't crash.
int g_mkey_state = 1;
int api_GetMKeyState(lua_State* L) { lua_pushinteger(L, g_mkey_state); return 1; }
int api_SetMKeyState(lua_State* L) {
    int v = static_cast<int>(luaL_checkinteger(L, 1));
    if (v >= 1 && v <= 3) g_mkey_state = v;
    return 0;
}

// G HUB scripts occasionally call these for visual feedback on Logitech
// LightSync hardware. We don't drive any LEDs; accept and ignore.
int api_SetBacklightColor(lua_State* /*L*/)    { return 0; }
int api_RestoreBacklightColor(lua_State* /*L*/) { return 0; }

// G HUB's PlayMacro plays a sequence recorded through the G HUB UI by
// name. We don't have a recorder, but ported scripts in the wild almost
// always pass the name of a Lua function the script itself defined for
// the same effect (a top-level function that issues PressKey / Sleep /
// MoveMouseRelative calls). So we honour that contract: look up
// _G[name], and if it's a function, call it synchronously. Anything
// else is silently a no-op so legitimate "macro doesn't exist" calls
// don't spam the log on every keypress.
int api_PlayMacro(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;

    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return 0;
    }

    if (lua_pcall(L, 0, 0, 0) != 0)
    {
        const char* err = lua_tostring(L, -1);
        runtime_log(std::string("[macro] PlayMacro(") + name + "): "
                    + (err ? err : "(unknown error)"));
        lua_pop(L, 1);
    }
    return 0;
}

// True async macros aren't supported — PlayMacro runs synchronously on
// the worker thread, so by the time another OnEvent could call
// AbortMacro the macro has already returned. Provided as a no-op so
// scripts using the API don't error out.
int api_AbortMacro(lua_State* /*L*/) { return 0; }

// G HUB allows scripts to react to lock keys without polling.
int api_IsKeyDown(lua_State* L)
{
    int vk = resolve_arg_to_vk(L, 1);
    bool pressed = (vk > 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0);
    lua_pushboolean(L, pressed ? 1 : 0);
    return 1;
}

} // extern "C"

// ---------------- Registration ----------------

const luaL_Reg kApi[] = {
    {"MoveMouseRelative",            api_MoveMouseRelative},
    {"MoveMouseTo",                  api_MoveMouseTo},
    {"MoveMouseWheel",               api_MoveMouseWheel},
    {"PressMouseButton",             api_PressMouseButton},
    {"ReleaseMouseButton",           api_ReleaseMouseButton},
    {"PressAndReleaseMouseButton",   api_PressAndReleaseMouseButton},
    {"PressKey",                     api_PressKey},
    {"ReleaseKey",                   api_ReleaseKey},
    {"PressAndReleaseKey",           api_PressAndReleaseKey},
    {"Sleep",                        api_Sleep},
    {"GetRunningTime",               api_GetRunningTime},
    {"IsMouseButtonPressed",         api_IsMouseButtonPressed},
    {"IsModifierPressed",            api_IsModifierPressed},
    {"IsKeyLockOn",                  api_IsKeyLockOn},
    {"IsKeyDown",                    api_IsKeyDown},
    {"GetMousePosition",             api_GetMousePosition},
    {"OutputLogMessage",             api_OutputLogMessage},
    {"OutputDebugMessage",           api_OutputDebugMessage},
    {"ClearLog",                     api_ClearLog},
    {"EnablePrimaryMouseButtonEvents", api_EnablePrimaryMouseButtonEvents},
    {"GetMKeyState",                 api_GetMKeyState},
    {"SetMKeyState",                 api_SetMKeyState},
    {"SetBacklightColor",            api_SetBacklightColor},
    {"RestoreBacklightColor",        api_RestoreBacklightColor},
    {"PlayMacro",                    api_PlayMacro},
    {"AbortMacro",                   api_AbortMacro},
    {nullptr, nullptr},
};

// Tiny Lua-side prelude. Aliases print() onto OutputLogMessage so scripts
// that use `print` for debugging still land in our log panel, and exposes
// GetDate as a thin os.date wrapper because the G HUB signature matches.
const char kPrelude[] =
    "_G.print = function(...)\n"
    "  local t = {...}\n"
    "  for i = 1, select('#', ...) do t[i] = tostring(t[i]) end\n"
    "  OutputLogMessage(table.concat(t, '\\t'))\n"
    "end\n"
    "if not _G.GetDate then _G.GetDate = function(...) return os.date(...) end end\n";

} // namespace

void register_ghub_api(lua_State* L)
{
    for (const luaL_Reg* p = kApi; p->name; ++p)
    {
        lua_pushcfunction(L, p->func);
        lua_setglobal(L, p->name);
    }

    if (luaL_dostring(L, kPrelude) != 0)
    {
        const char* err = lua_tostring(L, -1);
        runtime_log(std::string("[macro] prelude failure: ") +
                    (err ? err : "(unknown)"));
        lua_pop(L, 1);
    }
}

} // namespace macro
