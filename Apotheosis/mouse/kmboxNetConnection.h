#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

#include "kmbox_net/kmboxNet.h"

class KmboxNetConnection
{
public:
    KmboxNetConnection(const std::string& ip, const std::string& port, const std::string& uuid);
    ~KmboxNetConnection();

    void monitorThread();
    bool isOpen() const { return is_open_; }

    void move(int x, int y);
    void moveAuto(int x, int y, int ms);
    void moveBezier(int x, int y, int ms, int x1, int y1, int x2, int y2);
    void leftDown();
    void leftUp();
    void rightDown();
    void rightUp();
    void middleDown();
    void middleUp();
    // Side buttons. The kmboxNet protocol expresses them through the
    // button bitmask of the full report (cmd_mouse_all), so the wrapper
    // mirrors a tracked mask and submits the whole report each time.
    // 0x08 = X1 (back / mouse4), 0x10 = X2 (forward / mouse5).
    void side1Down();
    void side1Up();
    void side2Down();
    void side2Up();
    void wheel(int wheel);
    void mouseAll(int button, int x, int y, int wheel);

    void keyDown(int vkey);
    void keyUp(int vkey);

    void monitor(short port);
    int monitorMouseLeft();
    int monitorMouseRight();
    int monitorMouseMiddle();
    int monitorMouseSide1();
    int monitorMouseSide2();
    int monitorKeyboard(short vkey);

    void maskMouseLeft(bool enable);
    void maskMouseRight(bool enable);
    void maskMouseMiddle(bool enable);
    void maskMouseSide1(bool enable);
    void maskMouseSide2(bool enable);
    void maskMouseX(bool enable);
    void maskMouseY(bool enable);
    void maskMouseWheel(bool enable);
    void maskKeyboard(short vkey);
    void unmaskKeyboard(short vkey);
    void unmaskAll();

    void reboot();
    void setConfig(const std::string& ip, unsigned short port);
    void debug(short port, char enable);

    void lcdColor(unsigned short rgb565);
    void lcdPictureBottom(unsigned char* buff_128_80);
    void lcdPicture(unsigned char* buff_128_160);

    std::atomic<bool> aiming_active;
    std::atomic<bool> shooting_active;
    std::atomic<bool> zooming_active;

private:
    std::mutex io_mutex_;
    bool is_open_;
    bool monitor_;
    std::thread monitor_thread_;
    std::atomic<bool> monitor_running_{ true };
    std::string ip_, port_, uuid_;

    // Mirror of the kmbox firmware's pressed-button bitmap, kept in sync
    // by every button helper above. Required by the side1/side2 path so
    // the cmd_mouse_all call we issue for X1/X2 doesn't clobber the
    // currently-held left/right/middle state. Guarded by io_mutex_.
    int button_mask_ = 0;
};
