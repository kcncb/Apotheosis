#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "Arduino.h"
#include "capture.h"
#include "config.h"
#include "keyboard_listener.h"
#include "keycodes.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
#include "mouse.h"
#include "runtime/active_hotkey.h"
#include "Apotheosis.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;

namespace
{

// Only the user-configured aim hotkey is honored at runtime. All other
// hardcoded hotkeys (F2 exit, HOME overlay toggle, etc.) have been removed.
bool win32_key_pressed(int vk_code)
{
    return (GetAsyncKeyState(vk_code) & 0x8000) != 0;
}

} // namespace

bool isAnyKeyPressed(const std::vector<std::string>& keys)
{
    for (const auto& key_name : keys)
    {
        int key_code = KeyCodes::getKeyCode(key_name);
        bool pressed = false;
        bool handled_by_device = false;

        if (config.input_method == "KMBOX_NET")
        {
            handled_by_device = true;
            if (kmboxNetSerial && kmboxNetSerial->isOpen())
            {
                int state = -1;
                if (key_name == "LeftMouseButton")        state = kmboxNetSerial->monitorMouseLeft();
                else if (key_name == "RightMouseButton")  state = kmboxNetSerial->monitorMouseRight();
                else if (key_name == "MiddleMouseButton") state = kmboxNetSerial->monitorMouseMiddle();
                else if (key_name == "X1MouseButton")     state = kmboxNetSerial->monitorMouseSide1();
                else if (key_name == "X2MouseButton")     state = kmboxNetSerial->monitorMouseSide2();
                else handled_by_device = false;

                pressed = (state == 1);
            }
        }

        if (!pressed && config.input_method == "MAKCU")
        {
            handled_by_device = true;
            if (makcuSerial && makcuSerial->isOpen())
            {
                if (key_name == "LeftMouseButton")       pressed = makcuSerial->shooting_active;
                else if (key_name == "RightMouseButton") pressed = makcuSerial->zooming_active;
                else if (key_name == "X2MouseButton")    pressed = makcuSerial->aiming_active;
                else handled_by_device = false;
            }
        }

        if (!pressed && config.input_method == "ARDUINO" && config.arduino_enable_keys)
        {
            handled_by_device = true;
            if (arduinoSerial && arduinoSerial->isOpen())
            {
                if (key_name == "LeftMouseButton")       pressed = arduinoSerial->shooting_active;
                else if (key_name == "RightMouseButton") pressed = arduinoSerial->zooming_active;
                else if (key_name == "X2MouseButton")    pressed = arduinoSerial->aiming_active;
                else handled_by_device = false;
            }
        }

        if (!pressed && !handled_by_device && key_code != -1)
        {
            pressed = win32_key_pressed(key_code);
        }

        if (pressed) return true;
    }
    return false;
}

void keyboardListener()
{
    while (!shouldExit)
    {
        // Aim hotkey dispatch. Walk through configured profiles in order;
        // the first one with any key held wins. Snapshot under configMutex
        // so the UI can edit config.hotkeys concurrently. This is the only
        // hotkey honored at runtime.
        int next_active = -1;
        {
            std::lock_guard<std::recursive_mutex> cfg(configMutex);
            for (size_t i = 0; i < config.hotkeys.size(); ++i)
            {
                if (isAnyKeyPressed(config.hotkeys[i].keys))
                {
                    next_active = static_cast<int>(i);
                    break;
                }
            }
        }
        runtime::g_active_hotkey_index.store(next_active);
        aiming.store(next_active >= 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
