#ifndef MACRO_GHUB_API_H
#define MACRO_GHUB_API_H

struct lua_State;

namespace macro {

// Register the G HUB-compatible API on the given Lua state. Called once
// per VM by lua_runtime.cpp after luaL_openlibs(). Pushes nothing to the
// stack on return; all entries land as globals.
void register_ghub_api(lua_State* L);

} // namespace macro

#endif // MACRO_GHUB_API_H
