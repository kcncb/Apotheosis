#ifndef KEYBOARD_LISTENER_H
#define KEYBOARD_LISTENER_H

#include <string>
#include <vector>

void keyboardListener();

// Backend-agnostic "is any of these keys pressed right now" query. Checks
// Mouse buttons come from MAKCU/MAKCUNEW; keyboard keys fall back to
// Win32 GetAsyncKeyState because both devices are mouse-only.
bool isAnyKeyPressed(const std::vector<std::string>& keys);

#endif // KEYBOARD_LISTENER_H
