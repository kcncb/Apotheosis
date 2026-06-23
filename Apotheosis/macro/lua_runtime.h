#ifndef MACRO_LUA_RUNTIME_H
#define MACRO_LUA_RUNTIME_H

#include <string>
#include <vector>

// G HUB-compatible Lua macro runtime. One process-wide instance, owned by
// main(): call runtime_start() once after config has loaded, and
// runtime_stop() before exit. Reload the active script via
// runtime_load_script(); pass an empty path to runtime_unload_script() to
// drop the current script without touching the threads.
//
// Threading:
//   - Worker thread:  drains events, calls OnEvent(name, arg) under the
//                     state mutex. One OnEvent at a time, just like G HUB.
//   - Poller thread:  ~2 ms loop watching mouse buttons 1..5 + lock keys.
//                     Emits MOUSE_BUTTON_PRESSED / MOUSE_BUTTON_RELEASED.
//
// User scripts copy-pasted from G HUB / LGS run unmodified provided they
// only use the API surface implemented in ghub_api.cpp.

namespace macro {

// Lifecycle. Both are idempotent; calling start() twice is a no-op,
// calling stop() before start() is a no-op.
void runtime_start();
void runtime_stop();

// Script management. load_script() returns true on success; on error the
// message is written through *err (if non-null) and is also accessible
// via runtime_last_error(). A successful load fires PROFILE_ACTIVATED 1.
bool runtime_load_script(const std::string& path, std::string* err);
void runtime_unload_script();
bool runtime_is_loaded();
std::string runtime_current_script_path();
std::string runtime_last_error();

// Force fire an event. Useful for the UI to push PROFILE_ACTIVATED /
// PROFILE_DEACTIVATED on user toggle.
void runtime_inject_event(const std::string& name, int arg);

// Append a one-line message into the runtime's log ring. Used by
// OutputLogMessage / OutputDebugMessage and by error reporting.
void runtime_log(const std::string& line);
std::vector<std::string> runtime_log_snapshot(size_t max_lines = 200);
void runtime_log_clear();

// Monotonic milliseconds since runtime_start(). Drives GetRunningTime().
unsigned long long runtime_time_ms();

// Mirrors EnablePrimaryMouseButtonEvents(). Defaults false to match G HUB.
bool runtime_primary_button_events_enabled();
void runtime_set_primary_button_events_enabled(bool enabled);

// Fired by macro_input.cpp whenever the macro itself injects a button
// transition, so the poller can suppress the synthetic edge it'd otherwise
// re-emit back into OnEvent. button is 1..5.
void note_synthetic_button(int button, bool pressed);

// Returns true if the underlying Lua state is currently busy executing
// OnEvent. Used by the UI to grey out controls while the script is mid-
// callback.
bool runtime_busy();

// Master enable. Mirrors config.macro_enabled, but kept as a runtime
// atomic so the worker can react without re-reading the config struct.
bool runtime_enabled();
void runtime_set_enabled(bool enabled);

// True after runtime_stop() has been requested but before threads have
// fully joined. Sleep / busy-wait helpers in ghub_api inspect this so
// they can bail early instead of pinning shutdown.
bool runtime_stop_requested();

// True while a reload / unload is racing the worker. The Lua hook and
// cancellable Sleep poll this so an in-flight OnEvent can be torn down
// without waiting for the script to return on its own. Auto-cleared once
// the new VM is up.
bool runtime_abort_pending();

} // namespace macro

#endif // MACRO_LUA_RUNTIME_H
