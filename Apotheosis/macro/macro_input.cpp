#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "macro_input.h"

#include <atomic>
#include <mutex>

#include "Apotheosis.h"
#include "Makcu.h"
#include "MakcuNew.h"
#include "config.h"
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

} // namespace

void dispatch_move(int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
        makcuSerial->move(dx, dy);
    else if (config.input_method == "MAKCUNEW" && makcuNewSerial && makcuNewSerial->isOpen())
        makcuNewSerial->move(dx, dy);
}

void dispatch_move_abs(int x, int y)
{
    POINT current{};
    if (!GetCursorPos(&current)) return;
    dispatch_move(x - current.x, y - current.y);
}

void dispatch_button_down(int button)
{
    if (button < 1 || button > 5) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    note_synthetic_button(button, true);

    if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
    {
        // Makcu enumerates 1..5 using the same convention we use.
        makcuSerial->press(button);
        return;
    }
    else if (config.input_method == "MAKCUNEW" && makcuNewSerial && makcuNewSerial->isOpen())
    {
        makcuNewSerial->press(button);
        return;
    }
}

void dispatch_button_up(int button)
{
    if (button < 1 || button > 5) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    note_synthetic_button(button, false);

    if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
    {
        makcuSerial->release(button);
        return;
    }
    else if (config.input_method == "MAKCUNEW" && makcuNewSerial && makcuNewSerial->isOpen())
    {
        makcuNewSerial->release(button);
        return;
    }
}

void dispatch_key_down(int vk)
{
    if (vk <= 0 || vk > 0xFF) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    // MAKCU和MAKCUNEW均为纯鼠标设备；宏键盘事件由系统键盘API发送。
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    keybd_event(static_cast<BYTE>(vk), static_cast<BYTE>(scan), 0, 0);
}

void dispatch_key_up(int vk)
{
    if (vk <= 0 || vk > 0xFF) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    keybd_event(static_cast<BYTE>(vk), static_cast<BYTE>(scan), KEYEVENTF_KEYUP, 0);
}

void dispatch_wheel(int clicks)
{
    if (clicks == 0) return;
    std::lock_guard<std::recursive_mutex> lk(dispatch_mutex());

    if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
    {
        makcuSerial->wheel(clicks);
        return;
    }
    if (config.input_method == "MAKCUNEW" && makcuNewSerial && makcuNewSerial->isOpen())
    {
        makcuNewSerial->wheel(clicks);
        return;
    }

}

} // namespace macro
