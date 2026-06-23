#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "lua_runtime.h"
#include "macro_keymap.h"

#include "ghub_api.h"

namespace macro {
namespace {

struct Event
{
    std::string name; // e.g. "MOUSE_BUTTON_PRESSED"
    int         arg = 0;
};

struct State
{
    std::atomic<bool> started{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> abort_pending{false};

    // Master enable mirrors config.macro_enabled. Used by the worker to
    // skip dispatching when toggled off; the worker thread itself stays
    // alive so toggling back on doesn't need a thread restart.
    std::atomic<bool> enabled{false};

    // EnablePrimaryMouseButtonEvents flag. Default false to mirror G HUB.
    std::atomic<bool> primary_events{false};

    // Lua VM. Protected by vm_mtx — the only thread that touches the VM
    // outside of vm_mtx is the worker, and it grabs the lock before each
    // OnEvent dispatch.
    std::mutex   vm_mtx;
    lua_State*   L = nullptr;
    std::atomic<bool> busy{false};

    // Event queue.
    std::mutex              q_mtx;
    std::condition_variable q_cv;
    std::deque<Event>       q;

    // Poller-tracked input state. Indexed 1..5.
    std::atomic<bool>     btn_state[6];      // last observed physical state
    std::atomic<int>      synth_pending[6];  // synthetic-edge suppression counter

    // Logs.
    std::mutex             log_mtx;
    std::deque<std::string> log;

    // Status.
    std::mutex   status_mtx;
    std::string  current_script_path;
    std::string  last_error;
    bool         loaded = false;

    std::chrono::steady_clock::time_point start_time;

    std::thread worker_thread;
    std::thread poller_thread;
};

State g_state;

constexpr size_t kLogMax = 500;

void log_locked(State& s, const std::string& line)
{
    s.log.push_back(line);
    while (s.log.size() > kLogMax)
        s.log.pop_front();
}

void log_line_internal(const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_state.log_mtx);
    log_locked(g_state, line);
}

// ---------------- VM lifecycle (must be called with vm_mtx held) ----------

void vm_close_locked(State& s)
{
    if (s.L)
    {
        lua_close(s.L);
        s.L = nullptr;
    }
    s.loaded = false;
}

// Lua hook installed on every loaded script. Fires periodically (every
// kHookInstr instructions) and tears the script down via luaL_error when
// the runtime is shutting down. This is what lets us interrupt an infinite
// `while true do ... end` from the script.
constexpr int kHookInstr = 100000;

// extern "C" linkage to match the lua_Hook function-pointer type defined
// in lua.h. /permissive- keeps language linkage strict.
extern "C" {
static void lua_count_hook(lua_State* L, lua_Debug* /*ar*/)
{
    if (g_state.stop_requested.load() || g_state.abort_pending.load())
        luaL_error(L, "macro runtime aborting");
}
} // extern "C"

// Load a Lua chunk from a UTF-8 path. Replaces luaL_loadfile, which goes
// through fopen and on Windows uses the system ANSI codepage — paths
// with Chinese / Cyrillic / emoji characters silently fail there. We
// convert UTF-8 → UTF-16 → _wfopen, slurp the bytes, hand them to
// luaL_loadbuffer with a "@<path>" chunkname so error tracebacks still
// reference the file location. On error returns a non-zero Lua code with
// the error message already pushed onto the stack.
int load_chunk_utf8(lua_State* L, const std::string& utf8_path)
{
    const int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                         utf8_path.c_str(), -1,
                                         nullptr, 0);
    if (wlen <= 0)
    {
        lua_pushfstring(L, "MultiByteToWideChar failed for %s",
                        utf8_path.c_str());
        return LUA_ERRFILE;
    }
    std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0,
                        utf8_path.c_str(), -1,
                        wpath.data(), wlen);

    FILE* f = nullptr;
    const errno_t open_err = _wfopen_s(&f, wpath.data(), L"rb");
    if (open_err != 0 || !f)
    {
        lua_pushfstring(L, "cannot open %s (errno=%d)",
                        utf8_path.c_str(), static_cast<int>(open_err));
        return LUA_ERRFILE;
    }

    if (std::fseek(f, 0, SEEK_END) != 0)
    {
        std::fclose(f);
        lua_pushfstring(L, "fseek failed on %s", utf8_path.c_str());
        return LUA_ERRFILE;
    }
    const long size = std::ftell(f);
    std::rewind(f);
    if (size < 0)
    {
        std::fclose(f);
        lua_pushfstring(L, "ftell failed on %s", utf8_path.c_str());
        return LUA_ERRFILE;
    }

    std::vector<char> buf(size > 0 ? static_cast<size_t>(size) : 1u);
    const size_t got = (size > 0)
        ? std::fread(buf.data(), 1, static_cast<size_t>(size), f)
        : 0;
    std::fclose(f);

    if (size > 0 && got != static_cast<size_t>(size))
    {
        lua_pushfstring(L, "short read on %s (%d/%d)",
                        utf8_path.c_str(), static_cast<int>(got),
                        static_cast<int>(size));
        return LUA_ERRFILE;
    }

    // Strip a UTF-8 BOM if the user's editor planted one. Lua 5.1
    // doesn't recognise it natively and would treat it as a parse error.
    size_t off = 0;
    if (size >= 3 &&
        static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF)
    {
        off = 3;
    }

    const std::string chunkname = "@" + utf8_path;
    return luaL_loadbuffer(L, buf.data() + off, size - off,
                           chunkname.c_str());
}

bool vm_open_locked(State& s, const std::string& path, std::string* err_out)
{
    vm_close_locked(s);

    lua_State* L = luaL_newstate();
    if (!L)
    {
        if (err_out) *err_out = "luaL_newstate() returned nullptr";
        return false;
    }
    luaL_openlibs(L);

    // Stash the runtime pointer in the registry so API helpers can find
    // it without depending on the file-static.
    lua_pushlightuserdata(L, &s);
    lua_setfield(L, LUA_REGISTRYINDEX, "macro_runtime_state");

    register_ghub_api(L);

    lua_sethook(L, lua_count_hook, LUA_MASKCOUNT, kHookInstr);

    int rc = load_chunk_utf8(L, path);
    if (rc != 0)
    {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(load failure)";
        if (err_out) *err_out = msg;
        log_line_internal("[Lua] load: " + msg);
        lua_close(L);
        return false;
    }

    rc = lua_pcall(L, 0, 0, 0);
    if (rc != 0)
    {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(run failure)";
        if (err_out) *err_out = msg;
        log_line_internal("[Lua] init: " + msg);
        lua_close(L);
        return false;
    }

    s.L = L;
    s.loaded = true;
    return true;
}

// Push event (locks q_mtx).
void enqueue_event(const std::string& name, int arg)
{
    {
        std::lock_guard<std::mutex> lk(g_state.q_mtx);
        g_state.q.push_back({ name, arg });
    }
    g_state.q_cv.notify_one();
}

// Pull current physical button state for index 1..5.
bool read_button_pressed(int btn)
{
    int vk = mouse_button_to_vk(btn);
    if (vk == 0) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void poller_loop()
{
    using namespace std::chrono;

    // Initialize state to current values without firing events — we don't
    // want a button that's currently held when the script loads to look
    // like a fresh press.
    for (int i = 1; i <= 5; ++i)
    {
        g_state.btn_state[i].store(read_button_pressed(i));
        g_state.synth_pending[i].store(0);
    }

    while (!g_state.stop_requested.load())
    {
        std::this_thread::sleep_for(milliseconds(2));

        if (!g_state.enabled.load())
            continue;

        for (int i = 1; i <= 5; ++i)
        {
            const bool now_pressed = read_button_pressed(i);
            const bool was_pressed = g_state.btn_state[i].load();
            if (now_pressed == was_pressed)
                continue;

            g_state.btn_state[i].store(now_pressed);

            // If we recently injected this exact transition, swallow the
            // synthetic echo. synth_pending is decremented per matching
            // edge so a press+release pair is suppressed independently.
            int pending = g_state.synth_pending[i].load();
            if (pending > 0)
            {
                g_state.synth_pending[i].fetch_sub(1);
                continue;
            }

            // Primary-button gating. G HUB suppresses LMB events unless
            // the script opts in; mirror that behaviour for arg==1.
            if (i == 1 && !g_state.primary_events.load())
                continue;

            const char* ev = now_pressed ? "MOUSE_BUTTON_PRESSED" : "MOUSE_BUTTON_RELEASED";
            enqueue_event(ev, i);
        }
    }
}

void worker_loop()
{
    using namespace std::chrono;

    while (!g_state.stop_requested.load())
    {
        Event ev;
        {
            std::unique_lock<std::mutex> lk(g_state.q_mtx);
            g_state.q_cv.wait_for(lk, milliseconds(100), [] {
                return g_state.stop_requested.load() || !g_state.q.empty();
            });
            if (g_state.stop_requested.load())
                break;
            if (g_state.q.empty())
                continue;
            ev = std::move(g_state.q.front());
            g_state.q.pop_front();
        }

        if (!g_state.enabled.load())
            continue;

        std::lock_guard<std::mutex> vmlk(g_state.vm_mtx);
        if (!g_state.L || !g_state.loaded)
            continue;

        lua_State* L = g_state.L;
        lua_getglobal(L, "OnEvent");
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 1);
            continue;
        }

        lua_pushstring(L, ev.name.c_str());
        lua_pushinteger(L, ev.arg);

        g_state.busy.store(true);
        int rc = lua_pcall(L, 2, 0, 0);
        g_state.busy.store(false);

        if (rc != 0)
        {
            const char* msg = lua_tostring(L, -1);
            log_line_internal(std::string("[Lua] OnEvent: ") + (msg ? msg : "(error)"));
            lua_pop(L, 1);

            // luaL_error from our cancellation hook lands here — bail
            // out of the worker if a stop was requested.
            if (g_state.stop_requested.load())
                break;
        }
    }
}

} // namespace

void runtime_start()
{
    bool expected = false;
    if (!g_state.started.compare_exchange_strong(expected, true))
        return;

    g_state.stop_requested.store(false);
    g_state.start_time = std::chrono::steady_clock::now();

    // 1ms timer resolution so Sleep(7) actually sleeps ~7ms instead of
    // rounding up to the system tick (~15.6 ms by default). Released in
    // runtime_stop().
    timeBeginPeriod(1);

    g_state.worker_thread = std::thread(worker_loop);
    g_state.poller_thread = std::thread(poller_loop);
}

void runtime_stop()
{
    if (!g_state.started.load())
        return;

    g_state.stop_requested.store(true);
    g_state.q_cv.notify_all();

    if (g_state.worker_thread.joinable())
        g_state.worker_thread.join();
    if (g_state.poller_thread.joinable())
        g_state.poller_thread.join();

    {
        std::lock_guard<std::mutex> lk(g_state.vm_mtx);
        vm_close_locked(g_state);
    }

    timeEndPeriod(1);
    g_state.started.store(false);
    g_state.stop_requested.store(false);
}

bool runtime_load_script(const std::string& path, std::string* err)
{
    std::string err_local;

    // Tell any in-flight OnEvent to abort so we don't wait for it to
    // finish — the hook + cancellable_sleep both check abort_pending and
    // bail out via luaL_error.
    g_state.abort_pending.store(true);

    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_state.vm_mtx);
        ok = vm_open_locked(g_state, path, &err_local);
        g_state.abort_pending.store(false);
    }

    {
        std::lock_guard<std::mutex> lk(g_state.status_mtx);
        g_state.current_script_path = path;
        g_state.last_error = ok ? std::string() : err_local;
    }

    if (err) *err = err_local;

    if (ok)
    {
        runtime_log("[macro] script loaded: " + path);
        // Drop any queued events from the previous script so the new one
        // starts clean. Then synthesize PROFILE_ACTIVATED 1.
        {
            std::lock_guard<std::mutex> lk(g_state.q_mtx);
            g_state.q.clear();
        }
        enqueue_event("PROFILE_ACTIVATED", 1);
    }
    else
    {
        runtime_log("[macro] load failed: " + err_local);
    }
    return ok;
}

void runtime_unload_script()
{
    enqueue_event("PROFILE_DEACTIVATED", 1);

    // Give the worker a beat to dispatch PROFILE_DEACTIVATED before we
    // close the VM out from under it.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    g_state.abort_pending.store(true);
    {
        std::lock_guard<std::mutex> lk(g_state.vm_mtx);
        vm_close_locked(g_state);
        g_state.abort_pending.store(false);
    }

    std::lock_guard<std::mutex> sk(g_state.status_mtx);
    g_state.current_script_path.clear();
    runtime_log("[macro] script unloaded");
}

bool runtime_is_loaded()
{
    std::lock_guard<std::mutex> lk(g_state.vm_mtx);
    return g_state.loaded;
}

std::string runtime_current_script_path()
{
    std::lock_guard<std::mutex> lk(g_state.status_mtx);
    return g_state.current_script_path;
}

std::string runtime_last_error()
{
    std::lock_guard<std::mutex> lk(g_state.status_mtx);
    return g_state.last_error;
}

void runtime_inject_event(const std::string& name, int arg)
{
    enqueue_event(name, arg);
}

void runtime_log(const std::string& line)
{
    log_line_internal(line);
}

std::vector<std::string> runtime_log_snapshot(size_t max_lines)
{
    std::lock_guard<std::mutex> lk(g_state.log_mtx);
    if (g_state.log.size() <= max_lines)
        return std::vector<std::string>(g_state.log.begin(), g_state.log.end());
    return std::vector<std::string>(g_state.log.end() - max_lines, g_state.log.end());
}

void runtime_log_clear()
{
    std::lock_guard<std::mutex> lk(g_state.log_mtx);
    g_state.log.clear();
}

unsigned long long runtime_time_ms()
{
    using namespace std::chrono;
    if (!g_state.started.load())
        return 0;
    auto delta = steady_clock::now() - g_state.start_time;
    return static_cast<unsigned long long>(duration_cast<milliseconds>(delta).count());
}

bool runtime_primary_button_events_enabled()
{
    return g_state.primary_events.load();
}

void runtime_set_primary_button_events_enabled(bool enabled)
{
    g_state.primary_events.store(enabled);
}

void note_synthetic_button(int button, bool /*pressed*/)
{
    if (button < 1 || button > 5) return;
    g_state.synth_pending[button].fetch_add(1);
}

bool runtime_busy()
{
    return g_state.busy.load();
}

bool runtime_enabled()
{
    return g_state.enabled.load();
}

void runtime_set_enabled(bool enabled)
{
    g_state.enabled.store(enabled);
}

bool runtime_stop_requested()
{
    return g_state.stop_requested.load();
}

bool runtime_abort_pending()
{
    return g_state.abort_pending.load();
}

} // namespace macro
