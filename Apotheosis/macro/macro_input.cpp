#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "macro_input.h"

#include <atomic>
#include <mutex>

#include "Apotheosis.h"
#include "Arduino.h"
#include "KmboxAConnection.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
#include "config.h"
#include "ghub.h"
#include "mouse.h"

namespace macro {
namespace {

// Fallback mutex for when no MouseThread exists yet (config phase, or
// before a session starts). Once globalMouseThread is non-null we route
// through its recursive mutex so we serialize against the aim loop too.
std::recursive_mutex g_local_mutex;

std::recursive_mutex& dispatch_mutex()
{
    if (globalMouseThread)
        return globalMouseThread->input_method_mutex;
    return g_local_mutex;
}

DWORD button_mouse_event_down(int button)
{
    switch (button) {
        case 1: return MOUSEEVENTF_LEFTDOWN;
        case 2: return MOUSEEVENTF_RIGHTDOWN;
        case 3: return MOUSEEVENTF_MIDDLEDOWN;
        case 4: case 5: return MOUSEEVENTF_XDOWN;
        default: return 0;
    }
}

DWORD button_mouse_event_up(int button)
{
    switch (button) {
        case 1: return MOUSEEVENTF_LEFTUP;
        case 2: return MOUSEEVENTF_RIGHTUP;
        case 3: return MOUSEEVENTF_MIDDLEUP;
        case 4: case 5: return MOUSEEVENTF_XUP;
        default: return 0;
    }
}

void win32_mouse_button(int button, bool down)
{
    DWORD flags = down ? button_mouse_event_down(button) : button_mouse_event_up(button);
    if (flags == 0) return;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    if (button == 4) in.mi.mouseData = XBUTTON1;
    else if (button == 5) in.mi.mouseData = XBUTTON2;
    SendInput(1, &in, sizeof(INPUT));
}

} // namespace

void dispatch_move(int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
        kmboxNetSerial->move(dx, dy);
    else if (config.input_method == "KMBOX_A" && kmboxASerial && kmboxASerial->isOpen())
        kmboxASerial->move(dx, dy);
    else if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
        makcuSerial->move(dx, dy);
    else if (config.input_method == "ARDUINO" && arduinoSerial && arduinoSerial->isOpen())
        arduinoSerial->move(dx, dy);
    else if (config.input_method == "GHUB" && gHub)
        gHub->mouse_xy(dx, dy);
    else
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(INPUT));
    }
}

void dispatch_move_abs(int x, int y)
{
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());
    SetCursorPos(x, y);
}

void dispatch_button_down(int button)
{
    if (button < 1 || button > 5) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    note_synthetic_button(button, true);

    if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
    {
        switch (button) {
            case 1: kmboxNetSerial->leftDown();   return;
            case 2: kmboxNetSerial->rightDown();  return;
            case 3: kmboxNetSerial->middleDown(); return;
            case 4: kmboxNetSerial->side1Down();  return;
            case 5: kmboxNetSerial->side2Down();  return;
        }
    }
    else if (config.input_method == "KMBOX_A" && kmboxASerial && kmboxASerial->isOpen())
    {
        switch (button) {
            case 1: kmboxASerial->leftDown();   return;
            case 2: kmboxASerial->rightDown();  return;
            case 3: kmboxASerial->middleDown(); return;
            case 4: kmboxASerial->side1Down();  return;
            case 5: kmboxASerial->side2Down();  return;
        }
    }
    else if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
    {
        // Makcu enumerates 1..5 using the same convention we use.
        makcuSerial->press(button);
        return;
    }
    else if (config.input_method == "ARDUINO" && arduinoSerial && arduinoSerial->isOpen())
    {
        // Arduino bridge currently only supports the primary button.
        if (button == 1) { arduinoSerial->press(); return; }
    }
    else if (config.input_method == "GHUB" && gHub)
    {
        gHub->mouse_down(button);
        return;
    }

    win32_mouse_button(button, true);
}

void dispatch_button_up(int button)
{
    if (button < 1 || button > 5) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    note_synthetic_button(button, false);

    if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
    {
        switch (button) {
            case 1: kmboxNetSerial->leftUp();   return;
            case 2: kmboxNetSerial->rightUp();  return;
            case 3: kmboxNetSerial->middleUp(); return;
            case 4: kmboxNetSerial->side1Up();  return;
            case 5: kmboxNetSerial->side2Up();  return;
        }
    }
    else if (config.input_method == "KMBOX_A" && kmboxASerial && kmboxASerial->isOpen())
    {
        switch (button) {
            case 1: kmboxASerial->leftUp();   return;
            case 2: kmboxASerial->rightUp();  return;
            case 3: kmboxASerial->middleUp(); return;
            case 4: kmboxASerial->side1Up();  return;
            case 5: kmboxASerial->side2Up();  return;
        }
    }
    else if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
    {
        makcuSerial->release(button);
        return;
    }
    else if (config.input_method == "ARDUINO" && arduinoSerial && arduinoSerial->isOpen())
    {
        if (button == 1) { arduinoSerial->release(); return; }
    }
    else if (config.input_method == "GHUB" && gHub)
    {
        gHub->mouse_up(button);
        return;
    }

    win32_mouse_button(button, false);
}

void dispatch_key_down(int vk)
{
    if (vk <= 0 || vk > 0xFF) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
    {
        kmboxNetSerial->keyDown(vk);
        return;
    }

    // The other backends don't expose keyboard injection (Arduino/Makcu/
    // KmboxA wrappers in this tree are mouse-only). Fall back to keybd_event
    // for everything else, including GHUB — the GHUB DLL doesn't expose
    // key APIs in the version we ship.
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    keybd_event(static_cast<BYTE>(vk), static_cast<BYTE>(scan), 0, 0);
}

void dispatch_key_up(int vk)
{
    if (vk <= 0 || vk > 0xFF) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
    {
        kmboxNetSerial->keyUp(vk);
        return;
    }

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    keybd_event(static_cast<BYTE>(vk), static_cast<BYTE>(scan), KEYEVENTF_KEYUP, 0);
}

void dispatch_wheel(int clicks)
{
    if (clicks == 0) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
    {
        kmboxNetSerial->wheel(clicks);
        return;
    }
    if (config.input_method == "KMBOX_A" && kmboxASerial && kmboxASerial->isOpen())
    {
        kmboxASerial->wheel(clicks);
        return;
    }

    // Win32: WHEEL_DELTA is 120 per notch.
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(clicks * WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));
}

} // namespace macro
