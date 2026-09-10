// ============================================================
// makcu_proto.h - MAKCU直通透传固件 C++ 对接头文件 (单头文件, 零依赖)
// 协议规范见 docs/proto.md  |  用法:
//   MakcuLink link("COM4", 115200);        // 或串口句柄自行注入
//   link.move(dx, dy);                     // 直通位移
//   link.setButtons(MAKCU_BTN_L);          // 绝对态按键
//   link.click(MAKCU_BTN_L, 45);           // 固件内点击
// 编译: C++11 及以上, Windows/Linux 通用; 串口读写函数需按平台实现
// ============================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace makcu {

// ---- 命令码 (与 docs/proto.md §2 一致) ----
enum Cmd : uint8_t {
    CMD_MOVE            = 0x01,
    CMD_MOVE_RAW        = 0x02,
    CMD_MOVE_BATCH      = 0x03,
    CMD_MOVETO          = 0x04,
    CMD_BUTTON_MASK     = 0x10,
    CMD_BUTTON_MASK_EX  = 0x11,
    CMD_CLICK           = 0x12,
    CMD_WHEEL           = 0x20,
    CMD_KEY_MASK        = 0x21,
    CMD_KEY_TAP         = 0x22,
    CMD_GET_VERSION     = 0x40,
    CMD_GET_STATS       = 0x41,
    CMD_SET_BAUD        = 0x42,
    CMD_GHOST_MODE      = 0x43,
    CMD_PANIC           = 0x44,
    CMD_REBOOT          = 0x45,
    CMD_SUB_ASYNC       = 0x48,   // uint8 enable: 订阅按键异步上报(会话级)
    CMD_ACK             = 0x80,
    CMD_NAK             = 0x81,
    // 异步帧: 0x84 ASYNC_BUTTON {uint8 real_mask, uint8 inj_mask} 需订阅
};

// 按键位定义
enum Btn : uint8_t {
    MAKCU_BTN_L  = 1 << 0,
    MAKCU_BTN_R  = 1 << 1,
    MAKCU_BTN_M  = 1 << 2,
    MAKCU_BTN_S1 = 1 << 3,
    MAKCU_BTN_S2 = 1 << 4,
};

constexpr uint8_t FRAME_MAGIC0 = 0xA5;
constexpr uint8_t FRAME_MAGIC1 = 0x5C;

// ---- CRC-16/MODBUS ----
inline uint16_t crc16_modbus(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

// ---- 帧构造器 ----
inline std::vector<uint8_t> build_frame(uint8_t cmd, const uint8_t* payload, size_t len) {
    std::vector<uint8_t> f;
    f.reserve(len + 7);
    f.push_back(FRAME_MAGIC0);
    f.push_back(FRAME_MAGIC1);
    f.push_back(static_cast<uint8_t>(len));
    f.push_back(0);                       // SEQ占位(可由Link层填)
    f.push_back(cmd);
    if (len) f.insert(f.end(), payload, payload + len);
    uint16_t crc = crc16_modbus(f.data() + 2, len + 3);  // LEN..PAYLOAD
    f.push_back(crc & 0xFF);              // 低字节在前
    f.push_back((crc >> 8) & 0xFF);
    return f;
}

// ---- 常用payload打包辅助 (小端) ----
inline void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
}
inline void put_i16(std::vector<uint8_t>& v, int16_t x) { put_u16(v, (uint16_t)x); }
inline void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}

// ---- 高层API: 组帧示例(直接可用) ----
inline std::vector<uint8_t> frame_move(int16_t dx, int16_t dy) {
    std::vector<uint8_t> p; put_i16(p, dx); put_i16(p, dy);
    return build_frame(CMD_MOVE, p.data(), p.size());
}
inline std::vector<uint8_t> frame_move_batch(const int16_t* dxdy, size_t points) {
    std::vector<uint8_t> p; p.reserve(points * 4);
    for (size_t i = 0; i < points * 2; ++i) put_i16(p, dxdy[i]);
    return build_frame(CMD_MOVE_BATCH, p.data(), p.size());
}
inline std::vector<uint8_t> frame_button_mask(uint8_t mask) {
    return build_frame(CMD_BUTTON_MASK, &mask, 1);
}
inline std::vector<uint8_t> frame_click(uint8_t btn_bits, uint16_t down_ms) {
    std::vector<uint8_t> p; p.push_back(btn_bits); put_u16(p, down_ms);
    return build_frame(CMD_CLICK, p.data(), p.size());
}
inline std::vector<uint8_t> frame_wheel(int8_t delta) {
    return build_frame(CMD_WHEEL, reinterpret_cast<const uint8_t*>(&delta), 1);
}
inline std::vector<uint8_t> frame_ghost(bool on) {
    uint8_t v = on ? 1 : 0; return build_frame(CMD_GHOST_MODE, &v, 1);
}
inline std::vector<uint8_t> frame_sub_async(bool on) {
    uint8_t v = on ? 1 : 0; return build_frame(CMD_SUB_ASYNC, &v, 1);
}
inline std::vector<uint8_t> frame_panic() { return build_frame(CMD_PANIC, nullptr, 0); }

// ---- 串口抽象: 按平台替换实现即可 ----
struct ISerial {
    virtual ~ISerial() {}
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual int  read(uint8_t* buf, size_t cap) = 0;   // 返回实际读取数, 无数据返回0
};

// ---- 链路封装(可选): 自动SEQ/ACK等待 ----
class Link {
public:
    explicit Link(ISerial& s) : _s(s) {}

    bool send(uint8_t cmd, const std::vector<uint8_t>& frame, bool wait_ack = false) {
        _seq = (_seq + 1) & 0xFF;
        if (frame.size() >= 4) const_cast<std::vector<uint8_t>&>(frame)[3] = _seq;
        if (!_s.write(frame.data(), frame.size())) return false;
        _tx_count++;
        return wait_ack ? wait_ack_for(_seq) : true;
    }

    bool move(int16_t dx, int16_t dy)        { auto f = frame_move(dx, dy); return send(CMD_MOVE, f); }
    bool buttons(uint8_t mask)               { auto f = frame_button_mask(mask); return send(CMD_BUTTON_MASK, f, true); }
    bool ghost(bool on)                      { auto f = frame_ghost(on); return send(CMD_GHOST_MODE, f, true); }
    bool panic()                             { auto f = frame_panic(); return send(CMD_PANIC, f, true); }

private:
    bool wait_ack_for(uint8_t seq);

    ISerial& _s;
    uint8_t  _seq = 0;
    uint32_t _tx_count = 0, _rx_count = 0, _crc_err = 0;
};

} // namespace makcu