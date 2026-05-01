#ifndef KEYBOARD_LISTENER_H
#define KEYBOARD_LISTENER_H

#include <string>
#include <vector>

void keyboardListener();

// Backend-agnostic "is any of these keys pressed right now" query. Checks
// Kmbox Net / MAKCU / Arduino state first (when a physical device is
// connected and enabled) and falls back to Win32 GetAsyncKeyState.
bool isAnyKeyPressed(const std::vector<std::string>& keys);

#endif // KEYBOARD_LISTENER_H
