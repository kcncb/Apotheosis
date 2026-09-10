// ============================================================
// makcu_proto.h - MAKCU定制固件 C++ 对接头文件 (单头文件, 零依赖)
// 协议规范见 docs/proto.md  |  用法:
//   makcu::Link link(serial);                // serial实现makcu::ISerial
//   link.pid_move_latest(dx, dy, 8);       // 原版MAKCU式latest-value热路径
//   link.buttons(makcu::MAKCU_BTN_L);       // 绝对态按键(根治卡枪)
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
    CMD_MOVETO          = 0x04,   // 已退役: NAK/UNSUPPORTED
    CMD_PID_MOVE_LATEST = 0x05,   // latest-value单槽；新值覆盖尚未发送的旧值
    CMD_BUTTON_MASK     = 0x10,
    CMD_BUTTON_MASK_EX  = 0x11,
    CMD_CLICK           = 0x12,
    CMD_WHEEL           = 0x20,
    CMD_KEY_MASK        = 0x21,
    CMD_KEY_TAP         = 0x22,
    CMD_KEY_RELEASE_ALL = 0x23,
    CMD_KB_BLOCK        = CMD_KEY_RELEASE_ALL, // 旧名称兼容；不阻断真实键盘
    CMD_RESERVED_30     = 0x30,   // 0x30~0x36均已退役: NAK/UNSUPPORTED
    CMD_RESERVED_31     = 0x31,
    CMD_RESERVED_32     = 0x32,
    CMD_RESERVED_33     = 0x33,
    CMD_RESERVED_34     = 0x34,
    CMD_RESERVED_35     = 0x35,
    CMD_RESERVED_36     = 0x36,
    CMD_GET_VERSION     = 0x40,
    CMD_GET_STATS       = 0x41,
    CMD_SET_BAUD        = 0x42,
    CMD_RESERVED_43     = 0x43,   // 已退役: NAK/UNSUPPORTED
    CMD_PANIC           = 0x44,
    CMD_REBOOT          = 0x45,
    CMD_DESC_APPLY      = 0x46,
    CMD_DESC_COMMIT     = 0x47,
    CMD_SUB_ASYNC       = 0x48,   // uint8 enable: 订阅按键异步上报(会话级)
    CMD_GET_STATE       = 0x49,
    CMD_GET_PIPELINE    = 0x4A,   // 查询HID移动队列实时状态
    CMD_GET_HID_CAPS    = 0x4B,
    CMD_GET_TIMING      = 0x4C,
    CMD_START_PID       = 0x4D,   // ACK后自动切换到4M
    CMD_ACK             = 0x80,
    CMD_NAK             = 0x81,
    CMD_VERSION_RESP    = 0x82,
    CMD_STATS_RESP      = 0x83,
    CMD_ASYNC_BUTTON    = 0x84,
    CMD_STATE_RESP      = 0x85,
    CMD_PIPELINE_RESP   = 0x86,
    CMD_HID_CAPS_RESP   = 0x87,
    CMD_TIMING_RESP     = 0x88,
};

enum NakError : uint8_t {
    NAK_BAD_LENGTH = 1,
    NAK_BAD_VALUE = 2,
    NAK_UNSUPPORTED = 3,
    NAK_UNKNOWN_CMD = 4,
    NAK_BUSY = 5,                 // 可靠输入事件缓冲繁忙；PID latest槽不会返回此错误
};

enum HidCapsFlag : uint8_t {
    HID_CAP_MOUNTED       = 1 << 0,
    HID_CAP_BOOT_PROTOCOL = 1 << 1,
    HID_CAP_SUSPENDED     = 1 << 2,
    HID_CAP_PRIMARY_READY = 1 << 3,
};

enum TimingUsbFlag : uint8_t {
    TIMING_USB_SUSPENDED = 1 << 0,
    TIMING_HID_READY     = 1 << 1,
    TIMING_TASK_VALID    = 1 << 2,
};

enum HidSendTaskState : uint8_t {
    HID_SEND_IDLE       = 0,
    HID_SEND_RAW        = 1,
    HID_SEND_INJECTED   = 2,
    HID_SEND_NOT_READY  = 3,
    HID_SEND_RECOVERING = 4,
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
constexpr uint16_t PROTOCOL_SPEC_VERSION = 0x0107;
constexpr uint16_t PROTOCOL_WIRE_COMPAT_VERSION = 0x0103; // 帧和命令编号保持兼容
constexpr char EXPECTED_FIRMWARE_VERSION[] = "PID_MIRROR_V3R5";
constexpr char EXPECTED_KEYBOARD_FIRMWARE_VERSION[] = "KBD_MIRROR_V3R2";
constexpr size_t KEYBOARD_KEY_SLOTS = 6;
constexpr uint8_t PID_LATEST_CAPACITY = 1;
constexpr uint8_t PID_PENDING_CAPACITY = PID_LATEST_CAPACITY;
constexpr uint8_t PID_ACCUMULATOR_CAPACITY = PID_LATEST_CAPACITY; // 旧名称兼容
constexpr uint16_t PID_TTL_MIN_MS = 1;
constexpr uint16_t PID_TTL_MAX_MS = 1000;
constexpr uint16_t PID_TTL_DEFAULT_MS = 8;
constexpr size_t STATE_RESP_PAYLOAD_SIZE = 19;
constexpr size_t ASYNC_BUTTON_PAYLOAD_SIZE = 2;
constexpr size_t VERSION_RESP_PAYLOAD_SIZE = 18;
constexpr size_t PIPELINE_RESP_PAYLOAD_SIZE = 12;
constexpr size_t HID_CAPS_RESP_PAYLOAD_SIZE = 24;
constexpr size_t TIMING_RESP_LEGACY_PAYLOAD_SIZE = 28;
constexpr size_t TIMING_RESP_V3R2_PAYLOAD_SIZE = 36;
constexpr size_t TIMING_RESP_V3R3_PAYLOAD_SIZE = 44;
constexpr size_t TIMING_RESP_V3R5_PAYLOAD_SIZE = 60;
constexpr size_t TIMING_RESP_PAYLOAD_SIZE = TIMING_RESP_V3R5_PAYLOAD_SIZE;

struct AsyncButtonState {
    uint8_t real_button_mask = 0;
    uint8_t injected_button_mask = 0;
};

struct VersionInfo {
    char firmware[17] = {};       // VERSION_RESP前16字节，保证本地NUL结尾
};

struct PipelineState {
    uint16_t queued_reports = 0;  // latest槽：0=空，1=存在待发送修正
    uint16_t capacity = 0;        // V3R5固定为PID_LATEST_CAPACITY
    uint32_t rejected_reports = 0;// 兼容字段：当前返回单次USB提交失败数
    uint32_t emitted_reports = 0; // 已被USB端点接受的PID报告数
};

struct HidCaps {
    uint8_t protocol_version = 0;
    uint8_t poll_interval_ms = 0;
    uint8_t report_id = 0;
    uint8_t report_bytes = 0;
    uint8_t x_bits = 0;
    uint8_t y_bits = 0;
    bool mounted = false;
    bool boot_protocol = false;
    bool suspended = false;
    bool primary_ready = false;
    int32_t x_min = 0;
    int32_t x_max = 0;
    int32_t y_min = 0;
    int32_t y_max = 0;
};

struct TimingState {
    uint32_t last_hid_send_us = 0; // 最近一次USB HID完成回调时间(micros)
    uint32_t pid_received = 0;
    uint32_t pid_emitted = 0;
    uint32_t pid_overwritten = 0; // 尚未取出的旧修正被更新修正覆盖
    uint32_t pid_expired = 0;     // 兼容字段名：单次USB提交失败数
    uint32_t raw_dropped = 0;
    uint32_t serial_crc_errors = 0;
    uint16_t raw_queue_depth = 0;      // 当前等待透传的实体HID报告数
    uint16_t raw_queue_high_water = 0; // 本次PID会话内最高深度
    uint32_t raw_oldest_age_us = 0;    // 当前队首报告等待时间；空队列为0
    uint32_t raw_received = 0;          // 收到并尝试入队的实体复合HID报告数
    uint32_t raw_emitted = 0;           // 已被USB端点接受的实体复合HID报告数
    bool usb_suspended = false;
    bool hid_ready = false;
    bool send_task_valid = false;
    uint8_t send_task_state = 0;
    uint8_t send_instance = 0xFF;
    uint32_t hid_recoveries = 0;
    uint32_t primary_fail_streak = 0;
    uint32_t primary_no_progress_us = 0;
};

struct DeviceState {
    bool enabled = false;              // 兼容字段，当前固定false
    uint8_t reserved_mode = 0;
    uint8_t tempo_jitter_pct = 0;
    uint8_t tremor_hz = 0;
    uint8_t tremor_amp = 0;
    uint8_t jerk_segments = 0;
    uint8_t overshoot_pct = 0;
    int8_t max_dx = 0;
    int8_t max_dy = 0;
    uint16_t click_min_ms = 0;
    uint16_t click_max_ms = 0;
    uint16_t btn_watchdog_ms = 0;
    uint8_t real_button_mask = 0;
    uint8_t injected_button_mask = 0;
    uint16_t cpu_mhz = 0;
};

inline uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 解码0x84 ASYNC_BUTTON。该事件是设备主动帧，SEQ固定为0。
inline bool decode_async_button_payload(const uint8_t* p, size_t len,
                                        AsyncButtonState& out) {
    if (!p || len != ASYNC_BUTTON_PAYLOAD_SIZE) return false;
    out.real_button_mask = p[0] & 0x1F;
    out.injected_button_mask = p[1] & 0x1F;
    return true;
}

inline bool decode_state_payload(const uint8_t* p, size_t len, DeviceState& out) {
    if (!p || len != STATE_RESP_PAYLOAD_SIZE) return false;
    out.reserved_mode = p[0];
    out.enabled = p[1] != 0;
    out.tempo_jitter_pct = p[2];
    out.tremor_hz = p[3];
    out.tremor_amp = p[4];
    out.jerk_segments = p[5];
    out.overshoot_pct = p[6];
    out.max_dx = (int8_t)p[7];
    out.max_dy = (int8_t)p[8];
    out.click_min_ms = get_u16(p + 9);
    out.click_max_ms = get_u16(p + 11);
    out.btn_watchdog_ms = get_u16(p + 13);
    out.real_button_mask = p[15];
    out.injected_button_mask = p[16];
    out.cpu_mhz = get_u16(p + 17);
    return true;
}

inline bool decode_pipeline_payload(const uint8_t* p, size_t len, PipelineState& out) {
    if (!p || len != PIPELINE_RESP_PAYLOAD_SIZE) return false;
    out.queued_reports = get_u16(p);
    out.capacity = get_u16(p + 2);
    out.rejected_reports = get_u32(p + 4);
    out.emitted_reports = get_u32(p + 8);
    return true;
}

inline bool decode_version_payload(const uint8_t* p, size_t len, VersionInfo& out) {
    if (!p || len != VERSION_RESP_PAYLOAD_SIZE) return false;
    for (size_t i = 0; i < 16; ++i) out.firmware[i] = (char)p[i];
    out.firmware[16] = '\0';
    return true;
}

inline bool decode_hid_caps_payload(const uint8_t* p, size_t len, HidCaps& out) {
    if (!p || len != HID_CAPS_RESP_PAYLOAD_SIZE) return false;
    out.protocol_version = p[0];
    out.poll_interval_ms = p[1];
    out.report_id = p[2];
    out.report_bytes = p[3];
    out.x_bits = p[4];
    out.y_bits = p[5];
    out.mounted = (p[6] & HID_CAP_MOUNTED) != 0;
    out.boot_protocol = (p[6] & HID_CAP_BOOT_PROTOCOL) != 0;
    out.suspended = (p[6] & HID_CAP_SUSPENDED) != 0;
    out.primary_ready = (p[6] & HID_CAP_PRIMARY_READY) != 0;
    out.x_min = (int32_t)get_u32(p + 8);
    out.x_max = (int32_t)get_u32(p + 12);
    out.y_min = (int32_t)get_u32(p + 16);
    out.y_max = (int32_t)get_u32(p + 20);
    return out.protocol_version == 1 && out.poll_interval_ms != 0;
}

inline bool decode_timing_payload(const uint8_t* p, size_t len, TimingState& out) {
    if (!p || (len != TIMING_RESP_LEGACY_PAYLOAD_SIZE &&
               len != TIMING_RESP_V3R2_PAYLOAD_SIZE &&
               len != TIMING_RESP_V3R3_PAYLOAD_SIZE &&
               len != TIMING_RESP_PAYLOAD_SIZE)) return false;
    out = TimingState{};
    out.last_hid_send_us = get_u32(p);
    out.pid_received = get_u32(p + 4);
    out.pid_emitted = get_u32(p + 8);
    out.pid_overwritten = get_u32(p + 12);
    out.pid_expired = get_u32(p + 16);
    out.raw_dropped = get_u32(p + 20);
    out.serial_crc_errors = get_u32(p + 24);
    if (len >= TIMING_RESP_V3R2_PAYLOAD_SIZE) {
        out.raw_queue_depth = get_u16(p + 28);
        out.raw_queue_high_water = get_u16(p + 30);
        out.raw_oldest_age_us = get_u32(p + 32);
    }
    if (len >= TIMING_RESP_V3R3_PAYLOAD_SIZE) {
        out.raw_received = get_u32(p + 36);
        out.raw_emitted = get_u32(p + 40);
    }
    if (len == TIMING_RESP_PAYLOAD_SIZE) {
        out.usb_suspended = (p[44] & TIMING_USB_SUSPENDED) != 0;
        out.hid_ready = (p[44] & TIMING_HID_READY) != 0;
        out.send_task_valid = (p[44] & TIMING_TASK_VALID) != 0;
        out.send_task_state = p[45];
        out.send_instance = p[46];
        out.hid_recoveries = get_u32(p + 48);
        out.primary_fail_streak = get_u32(p + 52);
        out.primary_no_progress_us = get_u32(p + 56);
    }
    return true;
}

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
    if (len > 244 || (len && !payload)) return f;
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
inline std::vector<uint8_t> frame_pid_move_latest(int16_t dx, int16_t dy,
                                                   uint16_t ttl_ms) {
    if (ttl_ms < PID_TTL_MIN_MS || ttl_ms > PID_TTL_MAX_MS) return {};
    std::vector<uint8_t> p;
    put_i16(p, dx); put_i16(p, dy); put_u16(p, ttl_ms);
    return build_frame(CMD_PID_MOVE_LATEST, p.data(), p.size());
}
inline std::vector<uint8_t> frame_pid_move_accumulated(int16_t dx, int16_t dy,
                                                        uint16_t ttl_ms) {
    return frame_pid_move_latest(dx, dy, ttl_ms);
}
inline std::vector<uint8_t> frame_move_batch(const int16_t* dxdy, size_t points) {
    if (points == 0 || points > 61 || !dxdy) return {};
    std::vector<uint8_t> p; p.reserve(points * 4);
    for (size_t i = 0; i < points; ++i) {
        const int16_t dx = dxdy[i * 2], dy = dxdy[i * 2 + 1];
        put_i16(p, dx); put_i16(p, dy);
    }
    return build_frame(CMD_MOVE_BATCH, p.data(), p.size());
}
inline std::vector<uint8_t> frame_button_mask(uint8_t mask) {
    return build_frame(CMD_BUTTON_MASK, &mask, 1);
}
inline std::vector<uint8_t> frame_click(uint8_t btn_bits, uint16_t down_ms) {
    // down_ms由固件原样采用，不做最短/最长钳制或随机化。
    std::vector<uint8_t> p; p.push_back(btn_bits); put_u16(p, down_ms);
    return build_frame(CMD_CLICK, p.data(), p.size());
}
inline std::vector<uint8_t> frame_wheel(int8_t delta) {
    return build_frame(CMD_WHEEL, reinterpret_cast<const uint8_t*>(&delta), 1);
}
inline std::vector<uint8_t> frame_key_mask(uint8_t modifiers,
                                           const uint8_t keys[KEYBOARD_KEY_SLOTS]) {
    if (!keys) return {};
    uint8_t payload[1 + KEYBOARD_KEY_SLOTS]{};
    payload[0] = modifiers;
    for (size_t i = 0; i < KEYBOARD_KEY_SLOTS; ++i) payload[i + 1] = keys[i];
    return build_frame(CMD_KEY_MASK, payload, sizeof(payload));
}
inline std::vector<uint8_t> frame_key_tap(uint8_t modifiers, uint8_t key,
                                          uint16_t down_ms) {
    if (key < 0x04 || !down_ms) return {};
    std::vector<uint8_t> payload;
    payload.push_back(modifiers);
    payload.push_back(key);
    put_u16(payload, down_ms);
    return build_frame(CMD_KEY_TAP, payload.data(), payload.size());
}
inline std::vector<uint8_t> frame_key_release_all() {
    return build_frame(CMD_KEY_RELEASE_ALL, nullptr, 0);
}
inline std::vector<uint8_t> frame_sub_async(bool on) {
    uint8_t v = on ? 1 : 0; return build_frame(CMD_SUB_ASYNC, &v, 1);
}
inline std::vector<uint8_t> frame_get_state() {
    return build_frame(CMD_GET_STATE, nullptr, 0);
}
inline std::vector<uint8_t> frame_get_version() {
    return build_frame(CMD_GET_VERSION, nullptr, 0);
}
inline std::vector<uint8_t> frame_get_pipeline() {
    return build_frame(CMD_GET_PIPELINE, nullptr, 0);
}
inline std::vector<uint8_t> frame_get_hid_caps() {
    return build_frame(CMD_GET_HID_CAPS, nullptr, 0);
}
inline std::vector<uint8_t> frame_get_timing() {
    return build_frame(CMD_GET_TIMING, nullptr, 0);
}
inline std::vector<uint8_t> frame_start_pid_session() {
    return build_frame(CMD_START_PID, nullptr, 0);
}
inline std::vector<uint8_t> frame_panic() { return build_frame(CMD_PANIC, nullptr, 0); }

// ---- 串口抽象: 按你的工程替换实现即可 ----
struct ISerial {
    virtual ~ISerial() {}
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual int  read(uint8_t* buf, size_t cap) = 0;   // 返回实际读取数, 无数据返回0
};

// ---- 链路封装(可选): 自动SEQ/ACK等待 ----
class Link {
public:
    explicit Link(ISerial& s) : _s(s) {}

    bool send(uint8_t cmd, std::vector<uint8_t> frame, bool wait_ack = false) {
        _seq = (_seq + 1) & 0xFF;
        if (frame.size() < 7) return false;
        frame[3] = _seq;
        uint16_t crc = crc16_modbus(frame.data() + 2, frame.size() - 4);
        frame[frame.size() - 2] = crc & 0xFF;
        frame[frame.size() - 1] = (crc >> 8) & 0xFF;
        if (!_s.write(frame.data(), frame.size())) return false;
        _tx_count++;
        return wait_ack ? wait_ack_for(_seq, cmd) : true;
    }

    bool move(int16_t dx, int16_t dy)        { auto f = frame_move(dx, dy); return send(CMD_MOVE, f); }
    // dx/dy是本次PID相对修正。新命令覆盖尚未取出的旧命令；每个被取出
    // 的值只提交一次，不累加、不补发报告限幅后的余量，成功热路径无ACK。
    bool pid_move_latest(int16_t dx, int16_t dy,
                         uint16_t ttl_ms = PID_TTL_DEFAULT_MS) {
        auto f = frame_pid_move_latest(dx, dy, ttl_ms);
        return send(CMD_PID_MOVE_LATEST, f);
    }
    // 旧API名称只为源码兼容；V3R5不再提供累加语义。
    bool pid_move_accumulated(int16_t dx, int16_t dy,
                              uint16_t ttl_ms = PID_TTL_DEFAULT_MS) {
        return pid_move_latest(dx, dy, ttl_ms);
    }
    bool buttons(uint8_t mask)               { auto f = frame_button_mask(mask); return send(CMD_BUTTON_MASK, f); }
    bool click(uint8_t bits, uint16_t ms)    { auto f = frame_click(bits, ms); return send(CMD_CLICK, f, true); }
    bool keyboard(uint8_t modifiers, const uint8_t keys[KEYBOARD_KEY_SLOTS]) {
        auto f = frame_key_mask(modifiers, keys);
        return send(CMD_KEY_MASK, f, true);
    }
    bool key_tap(uint8_t modifiers, uint8_t key, uint16_t ms) {
        auto f = frame_key_tap(modifiers, key, ms);
        return send(CMD_KEY_TAP, f, true);
    }
    bool release_all_keys() {
        auto f = frame_key_release_all();
        return send(CMD_KEY_RELEASE_ALL, f, true);
    }
    bool panic()                             { auto f = frame_panic(); return send(CMD_PANIC, f, true); }
    bool request_version()                   { auto f = frame_get_version(); return send(CMD_GET_VERSION, f); }
    bool request_pipeline_state()            { auto f = frame_get_pipeline(); return send(CMD_GET_PIPELINE, f); }
    bool request_hid_caps()                  { auto f = frame_get_hid_caps(); return send(CMD_GET_HID_CAPS, f); }
    bool request_timing()                    { auto f = frame_get_timing(); return send(CMD_GET_TIMING, f); }
    // baud_switch必须立即把本机CH343端口切到target_baud。固件在ACK后保留
    // 200ms切换窗口，本函数随后等待链路稳定，调用方无需再手工切速。
    bool start_pid_session(bool (*baud_switch)(uint32_t, void*),
                           void (*sleep_ms)(uint32_t, void*),
                           void* ctx = nullptr) {
        if (!baud_switch || !sleep_ms) return false;
        auto f = frame_start_pid_session();
        if (!send(CMD_START_PID, f, true)) return false;
        if (!baud_switch(4000000, ctx)) return false;
        sleep_ms(250, ctx);
        return true;
    }
    // 订阅命令ACK后设备会紧接着主动发送首帧0x84快照。这里不在Link内部
    // 等ACK，避免wait_ack_for的临时接收缓冲吞掉同批到达的首帧事件；调用方
    // 应在自己的持续接收循环中同时处理ACK与ASYNC_BUTTON。
    bool subscribe_buttons(bool on)          { auto f = frame_sub_async(on); return send(CMD_SUB_ASYNC, f, false); }

private:
    bool wait_ack_for(uint8_t seq, uint8_t cmd) {
        std::vector<uint8_t> rx;
        rx.reserve(255);
        uint8_t tmp[64];
        // ISerial是非阻塞抽象；调用方应在read实现里承担等待/超时。
        for (unsigned attempts = 0; attempts < 1024; ++attempts) {
            int n = _s.read(tmp, sizeof(tmp));
            if (n < 0) return false;
            if (n == 0) continue;
            rx.insert(rx.end(), tmp, tmp + n);
            while (rx.size() >= 7) {
                size_t start = 0;
                while (start + 1 < rx.size() &&
                       !(rx[start] == FRAME_MAGIC0 && rx[start + 1] == FRAME_MAGIC1)) ++start;
                if (start) rx.erase(rx.begin(), rx.begin() + start);
                if (rx.size() < 7) break;
                size_t frame_len = (size_t)rx[2] + 7;
                if (frame_len > 255) { rx.erase(rx.begin()); continue; }
                if (rx.size() < frame_len) break;
                uint16_t got = (uint16_t)rx[frame_len - 2] |
                               ((uint16_t)rx[frame_len - 1] << 8);
                uint16_t want = crc16_modbus(rx.data() + 2, frame_len - 4);
                if (got == want) {
                    _rx_count++;
                    if (rx[3] == seq && rx[2] >= 2 && rx[5] == cmd) {
                        if (rx[4] == CMD_ACK) return rx[6] == 0;
                        if (rx[4] == CMD_NAK) return false;
                    }
                } else {
                    _crc_err++;
                }
                rx.erase(rx.begin(), rx.begin() + frame_len);
            }
        }
        return false;
    }

    ISerial& _s;
    uint8_t  _seq = 0;
    uint32_t _tx_count = 0, _rx_count = 0, _crc_err = 0;
};

} // namespace makcu
