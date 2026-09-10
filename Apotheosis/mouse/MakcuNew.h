#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "makcu_proto.h"
#include "serial/serial.h"

// MAKCUNEW (fw_device 直通透传固件 v1.0) 上位机链路。
//
// 重要事实 (以固件源码为准):
//   * 固件 protoInit() 从未给 proto::Parser::sendFrame 赋值，因此
//     send_resp()/ack() 全部是空操作 —— 设备【不会】回 ACK/NAK/
//     VERSION_RESP/STATS_RESP 任何二进制响应帧。0x40 GET_VERSION 同样
//     拿不到回包。唯一可靠的回包是 ASCII 命令 km.version，它走
//     sendTrackedResponse() -> Serial0.println()，真正会回字符串。
//   * 固件 setup() 里 Serial0.begin(115200) 是硬编码的，波特率不持久化，
//     所以每次上电设备一定在 115200。要跑 6M 必须先发 0x42 SET_BAUD
//     (失败再补一发 DE AD 转义帧)，设备 cbSetBaud() 执行
//     end() -> delay(50ms) -> begin(new) 后立即切速，
//     上位机必须自行重连复验，不能等 ACK。
//   * 0x48 SUB_ASYNC 会置 g_async_sub，之后设备主动上报 0x84 异步按键帧。
class MakcuNewConnection
{
public:
    MakcuNewConnection(const std::string& port, unsigned int baudRate);
    ~MakcuNewConnection();

    bool isOpen() const;
    unsigned int baudRate() const { return baudRate_; }
    bool move(int x, int y);
    void click(int button);
    void press(int button);
    void release(int button);
    void wheel(int delta);
    void cancelMove();
    bool physicalButtonPressed(int button) const;

private:
    bool openSerial(unsigned int baudRate);
    void startReader();
    void stopReader();
    void readerLoop();
    void consumeFrames();

    // 从帧间隙里捡出的散字节: ASCII 行 (固件 km.* 回包/上电横幅)
    // 与 km.buttons(1) 模式下的单字节按键掩码 (< 32) 都走这里
    void feedAsciiByte(uint8_t b);
    bool probeAscii(const std::string& command,
                    const std::string& expect,
                    unsigned int timeoutMs);

    void sendDeadBaud(unsigned int baud);
    bool establishSession();
    void supervisorLoop();
    void tryReconnect();
    void subscribeAsync(bool on);
    void applyPhysicalButtons(uint8_t mask);
    bool initializeProtocolSession();

    bool sendFrame(uint8_t command, const uint8_t* payload, size_t length);
    size_t encodeFrameLocked(uint8_t* output, size_t capacity, uint8_t command,
                             const uint8_t* payload, size_t length);
    static uint8_t buttonBit(int button);

    serial::Serial serial_;
    std::string port_;
    unsigned int baudRate_{115200};
    std::atomic<bool> portOpen_{false};   // 串口已打开(尚未验证)
    std::atomic<bool> open_{false};       // 会话已验证(探活通过)
    std::atomic<bool> stopping_{false};
    std::thread reader_;
    std::mutex writeMutex_;
    uint8_t sequence_{0};

    // 断线自动重连
    unsigned int requestedBaud_{115200};   // 用户目标速率, 重连始终以它为目标
    std::atomic<bool> wantOpen_{false};
    std::thread supervisor_;
    std::mutex reconnectMutex_;

    std::atomic<uint8_t> outputButtons_{0};
    std::atomic<uint8_t> realButtons_{0};
    std::atomic<uint8_t> injectedButtons_{0};

    // ASCII 探活通道
    std::mutex asciiMutex_;
    std::condition_variable asciiCv_;
    std::string asciiLine_;
    std::string asciiAccum_;
    uint64_t asciiSeq_{0};

    std::vector<uint8_t> receiveBuffer_;
    size_t receiveOffset_{0};
};
