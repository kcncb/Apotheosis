#ifndef MOUSE_AUTO_STOP_H
#define MOUSE_AUTO_STOP_H

#include <algorithm>
#include <cstdint>

namespace boss
{

// 自动急停（开火时的反向按键抵消）。
//
// 原理：绝大多数 FPS 引擎里【相反方向键同时按下 = 相互抵消 = 立刻停住】。
// 所以不需要松开玩家手上的 W，只要在开火那一拍往盒子里补一个"S"短按，
// 游戏侧就认为 W+S 同时存在，速度归零 —— 这一枪才是站定打出去的。
//
// ★ 只有 MAKCUNEW 能做：它才有键盘注入（0x22 KEY_TAP，固件内定时弹起）。
//   MAKCU（老盒子）只有鼠标通道，调用方必须自己关掉这个功能。
//
// ★ 为什么用 KEY_TAP 而不是 KEY_MASK：KEY_TAP 由固件定时弹起，是【自清】的 ——
//   就算上位机崩了/会话停了，键也会在 ms 级被放开。KEY_MASK 是绝对态，
//   一旦漏发清除帧，玩家的移动键就会永久卡住。
//
// ★ 一次只按一个方向键：前/后轴优先（绝大多数是按住 W 在走），因为 KEY_TAP
//   一次只带一个 key。斜向移动（W+A）只会抵消掉前后轴那一半，横向仍在 ——
//   这一点在文档里写清楚了，不假装它能把斜向也停干净。
class AutoStopController
{
public:
    // HID usage id（KEY_MASK 用的是标准 HID 键盘报表，所以 KEY_TAP 的 key 也用同一码表）。
    static constexpr int kHidA = 0x04;
    static constexpr int kHidD = 0x07;
    static constexpr int kHidS = 0x16;
    static constexpr int kHidW = 0x1A;

    // 固件定时弹起之后, 盒子→USB→系统还可能残留几毫秒。在这段余量里要剔除
    // 我们自己注入的键; 过了就不剔(见 tick() 里的说明)。
    static constexpr int64_t kInjectionGuardMs = 25;

    // 物理按下的方向键（调用方用 GetAsyncKeyState 读 VK 码）。
    struct Keys
    {
        bool forward = false;   // W
        bool back = false;      // S
        bool left = false;      // A
        bool right = false;     // D

        bool any() const { return forward || back || left || right; }
        void clearByHidKey(int hid_key)
        {
            if (hid_key == kHidW) forward = false;
            else if (hid_key == kHidS) back = false;
            else if (hid_key == kHidA) left = false;
            else if (hid_key == kHidD) right = false;
        }
    };

    struct Action
    {
        bool        tap = false;    // 本拍是否要发 KEY_TAP
        int         hid_key = 0;    // 要按的键（HID usage id）
        const char* name = "";      // 诊断用（"S" 等）
    };

    // 每拍调用一次（只在该热键的扳机运行时调）。
    //   fired   本拍扳机是否真的按下了左键（begin_fire 成功那一拍）
    //   raw     物理按下的方向键（未剔除我们自己注入的键）
    //   enabled 功能是否开启（配置开 + 输入方式 = MAKCUNEW）
    //   tap_ms  短按时长
    Action tick(bool fired, const Keys& raw, bool enabled, int tap_ms, int64_t now_ms)
    {
        if (!enabled || tap_ms <= 0)
            return {};

        // 上一发还在按着的话不重复下发：固件那边是定时弹起，重发只会把计时重置，
        // 而且连点模式下每一发都发一次会让"按键"变成"一直按着"。
        if (holdingFor(now_ms, tap_ms) > 0)
            return {};

        if (!fired)
            return {};

        // ★ 剔除"我们自己注入的键"—— 但【只在它可能还没弹起的那一小段里】剔。
        //   GetAsyncKeyState 看到的是"物理 + 注入"的合成态，不剔除会出现自激
        //   （我们要按 S，于是判定玩家在按 S，于是去按 W）。
        //   固件是定时弹起，但盒子→USB→系统还有几毫秒抖动，所以窗口后留一点余量。
        //
        //   ★★ 上一版这里是【无条件剔除】，于是 last_hid_key_ 一旦设上就永久生效：
        //      补过一次 S 之后 keys.back 永远被剔掉 —— 玩家按 S 后退时再也急停不了，
        //      A/D 同理（急停只在"第一次用的那个方向"上有效）。
        //      回归测试 [4b] 专门守这一条：同一个控制器里先补 S、之后还要能补 W。
        Keys keys = raw;
        if (last_hid_key_ != 0 &&
            (now_ms - last_tap_ms_) < static_cast<int64_t>(tap_ms) + kInjectionGuardMs)
        {
            keys.clearByHidKey(last_hid_key_);
        }

        Action action;
        if (keys.forward)      { action = {true, kHidS, "S"}; }
        else if (keys.back)    { action = {true, kHidW, "W"}; }
        else if (keys.left)    { action = {true, kHidD, "D"}; }
        else if (keys.right)   { action = {true, kHidA, "A"}; }
        else                   { return {}; }

        last_hid_key_ = action.hid_key;
        last_tap_ms_ = now_ms;
        return action;
    }

    // 强制结束：不发送任何东西（KEY_TAP 是固件定时弹起，本来就会自己放开），
    // 只清掉我们自己的状态，让下一次开火可以重新判定。
    void reset()
    {
        last_hid_key_ = 0;
        last_tap_ms_ = -1000000;
    }

    bool active(int64_t now_ms, int tap_ms) const { return holdingFor(now_ms, tap_ms) > 0; }
    int  lastHidKey() const { return last_hid_key_; }

private:
    int64_t holdingFor(int64_t now_ms, int tap_ms) const
    {
        if (last_hid_key_ == 0)
            return 0;
        const int64_t left = static_cast<int64_t>(tap_ms) - (now_ms - last_tap_ms_);
        return std::max<int64_t>(0, left);
    }

    int     last_hid_key_ = 0;
    int64_t last_tap_ms_ = -1000000;
};

} // namespace boss

#endif // MOUSE_AUTO_STOP_H
