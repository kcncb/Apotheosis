#include "MakcuNew.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
// 自动为当前端口设置 Windows 注册表 / 驱动层最低 LatencyTimer = 1ms
static void autoTuneCh343Latency(const std::string& portName)
{
    // 例如从 COM3 或 \\.\\COM3 提取数字 3
    int portNum = 0;
    size_t pos = portName.find("COM");
    if (pos != std::string::npos) {
        portNum = std::atoi(portName.c_str() + pos + 3);
    }
    if (portNum <= 0) return;

    char regSubKey[256];
    snprintf(regSubKey, sizeof(regSubKey),
             "SYSTEM\\CurrentControlSet\\Services\\CH341SER\\Parameters");
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regSubKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD latencyVal = 1;
        RegSetValueExA(hKey, "LatencyTimer", 0, REG_DWORD,
                       (const BYTE*)&latencyVal, sizeof(latencyVal));
        RegCloseKey(hKey);
    }
}
#endif

namespace
{
constexpr size_t kMaxPayload = 244;
constexpr unsigned int kBootBaud = 115200;   // 固件 setup() 硬编码
constexpr const char* kVersionTag = "MAKCU-PASSTHROUGH";

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
}

MakcuNewConnection::MakcuNewConnection(const std::string& port, unsigned int baudRate)
    : port_(port)
{
    requestedBaud_ = baudRate ? baudRate : kBootBaud;
    if (requestedBaud_ < 115200) requestedBaud_ = kBootBaud;
    if (requestedBaud_ > 6000000) requestedBaud_ = 6000000;
    baudRate_ = requestedBaud_;

#ifdef _WIN32
    autoTuneCh343Latency(port_);
#endif

    wantOpen_.store(true);

    if (!establishSession())
    {
        // 首连失败: 不进入守护状态，由调用方决定是否重建(见 Apotheosis.cpp)。
        wantOpen_.store(false);
        stopReader();
        try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
        std::cerr << "[MakcuNew] Failed to establish passthrough session on "
                  << port_ << std::endl;
        portOpen_.store(false);
        open_.store(false);
    }
    else
    {
        // 断线守护: 只要还 wantOpen 就每 ~1s 尝试恢复会话(含重订阅 0x48)。
        // 没有它的话, 设备重插后 isOpen() 永远为 false, 扳机与位移会静默失效。
        supervisor_ = std::thread(&MakcuNewConnection::supervisorLoop, this);
    }
}

MakcuNewConnection::~MakcuNewConnection()
{
    wantOpen_.store(false);
    if (open_.load())
    {
        // 退订异步按键上报。固件不回 ACK，发完即走。
        subscribeAsync(false);
    }
    if (supervisor_.joinable()) supervisor_.join();
    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    portOpen_.store(false);
    open_.store(false);
}

bool MakcuNewConnection::establishSession()
{
    const unsigned int requestedBaud = requestedBaud_;

    const auto closeSession = [this] {
        stopReader();
        try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
        portOpen_.store(false);
        open_.store(false);
        receiveBuffer_.clear();
        receiveOffset_ = 0;
        std::lock_guard<std::mutex> lk(asciiMutex_);
        asciiAccum_.clear();
        asciiLine_.clear();
    };

    // 尝试在指定波特率建立并验证一个可用会话
    const auto tryConnectAt = [&](unsigned int baud, const char* note) -> bool {
        if (!openSerial(baud)) return false;
        startReader();
        if (!probeAscii("km.version", kVersionTag, 500))
        {
            closeSession();
            return false;
        }
        baudRate_ = baud;
        open_.store(true);          // 探活通过, 会话才算真正建立
        initializeProtocolSession();
        std::cout << "[MakcuNew] Connected on " << port_ << " @ " << baudRate_
                  << " bps" << (note ? note : "") << std::endl;
        return true;
    };

    // 固件上电一定在 115200 (main.cpp 硬编码，且不持久化)，因此先探 115200，
    // 再决定是否需要切速。也允许设备恰好已处于目标波特率 (同一次上电内重连)。
    std::vector<unsigned int> candidates;
    if (requestedBaud != kBootBaud) candidates.push_back(kBootBaud);
    candidates.push_back(requestedBaud);

    try
    {
        for (unsigned int candidate : candidates)
        {
            if (!openSerial(candidate)) continue;
            startReader();

            const bool alive = probeAscii("km.version", kVersionTag, 500);
            if (!alive)
            {
                closeSession();
                continue;
            }

            // 已经处于目标波特率，直接进入会话。
            if (candidate == requestedBaud)
            {
                baudRate_ = candidate;
                open_.store(true);      // 探活通过, 会话才算真正建立
                initializeProtocolSession();
                std::cout << "[MakcuNew] Connected to MAKCU passthrough device on "
                          << port_ << " @ " << baudRate_ << " bps." << std::endl;
                return true;
            }

            // 当前在 115200，需要切到目标波特率。
            // 两条路径最终都落到固件 cbSetBaud():
            //   A) A5 5C 帧 0x42 SET_BAUD  —— 带 CRC、有长度校验，优先
            //   B) DE AD 转义帧 0xA5 <u32> —— proto_parser.h 注明的
            //      "Apotheosis 动态高速切波特率"硬件级路径，无 CRC
            // 两者都不会回 ACK (sendFrame 从未接线)，只能发完即走、自行重连复验。
            std::cout << "[MakcuNew] Requesting baud switch to " << requestedBaud
                      << " bps ..." << std::endl;

            // ---- 方案 A ----
            {
                uint8_t payload[4];
                payload[0] = static_cast<uint8_t>(requestedBaud & 0xFF);
                payload[1] = static_cast<uint8_t>((requestedBaud >> 8) & 0xFF);
                payload[2] = static_cast<uint8_t>((requestedBaud >> 16) & 0xFF);
                payload[3] = static_cast<uint8_t>((requestedBaud >> 24) & 0xFF);
                sendFrame(makcu::CMD_SET_BAUD, payload, sizeof(payload));
            }
            // 固件内部 end() + 50ms delay + begin()，留足 250ms 余量。
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            closeSession();

            if (tryConnectAt(requestedBaud, " (SET_BAUD)")) return true;

            // ---- 方案 B ----
            closeSession();
            if (openSerial(kBootBaud))
            {
                startReader();
                if (probeAscii("km.version", kVersionTag, 500))
                {
                    sendDeadBaud(requestedBaud);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    closeSession();
                    if (tryConnectAt(requestedBaud, " (DE AD)")) return true;
                }
            }

            // 切速失败 -> 退回 115200 保证至少能用 (只是慢)。
            closeSession();
            if (tryConnectAt(kBootBaud, ""))
            {
                std::cerr << "[MakcuNew] Baud switch failed, fell back to 115200 bps."
                          << std::endl;
                return true;
            }
            closeSession();
            break;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MakcuNew] Connection error: " << e.what() << std::endl;
    }

    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    portOpen_.store(false);
    open_.store(false);
    return false;
}

void MakcuNewConnection::supervisorLoop()
{
    while (wantOpen_.load())
    {
        for (int i = 0; i < 40 && wantOpen_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (!wantOpen_.load()) break;
        if (open_.load()) continue;
        tryReconnect();
    }
}

void MakcuNewConnection::tryReconnect()
{
    std::lock_guard<std::mutex> lk(reconnectMutex_);
    if (open_.load() || !wantOpen_.load()) return;

    std::cerr << "[MakcuNew] Link lost on " << port_ << ", reconnecting ..." << std::endl;
    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    receiveBuffer_.clear();
    receiveOffset_ = 0;
    {
        std::lock_guard<std::mutex> al(asciiMutex_);
        asciiAccum_.clear();
        asciiLine_.clear();
    }

    if (establishSession())
        std::cout << "[MakcuNew] Reconnected on " << port_
                  << " @ " << baudRate_ << " bps." << std::endl;
}

bool MakcuNewConnection::isOpen() const
{
    return open_.load();
}

bool MakcuNewConnection::openSerial(unsigned int baudRate)
{
    try
    {
        if (serial_.isOpen()) serial_.close();
        serial_.setPort(port_);
        serial_.setBaudrate(baudRate);
        serial::Timeout timeout(5, 10, 0, 50, 0);
        serial_.setTimeout(timeout);
        serial_.open();
        const bool opened = serial_.isOpen();
        portOpen_.store(opened);
        if (opened)
        {
            std::lock_guard<std::mutex> lk(asciiMutex_);
            asciiAccum_.clear();
            asciiLine_.clear();
            asciiSeq_ = 0;
        }
        return opened;
    }
    catch (...)
    {
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
}

void MakcuNewConnection::startReader()
{
    stopping_.store(false);
    if (!reader_.joinable())
        reader_ = std::thread(&MakcuNewConnection::readerLoop, this);
}

void MakcuNewConnection::stopReader()
{
    stopping_.store(true);
    if (reader_.joinable()) reader_.join();
}

size_t MakcuNewConnection::encodeFrameLocked(
    uint8_t* output, size_t capacity, uint8_t command,
    const uint8_t* payload, size_t length)
{
    const size_t frameLength = length + 7;
    if (!output || capacity < frameLength || length > kMaxPayload
        || (length && !payload)) return 0;
    output[0] = makcu::FRAME_MAGIC0;
    output[1] = makcu::FRAME_MAGIC1;
    output[2] = static_cast<uint8_t>(length);
    output[3] = ++sequence_;
    output[4] = command;
    if (length) std::copy(payload, payload + length, output + 5);
    const uint16_t crc = makcu::crc16_modbus(output + 2, length + 3);
    output[frameLength - 2] = static_cast<uint8_t>(crc & 0xFF);
    output[frameLength - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return frameLength;
}

bool MakcuNewConnection::sendFrame(
    uint8_t command, const uint8_t* payload, size_t length)
{
    if (!portOpen_.load(std::memory_order_acquire)
        || length > kMaxPayload || (length && !payload)) return false;

    uint8_t frame[kMaxPayload + 7];
    std::lock_guard<std::mutex> lock(writeMutex_);
    const size_t frameLength = encodeFrameLocked(
        frame, sizeof(frame), command, payload, length);
    if (!frameLength) return false;
    try
    {
        const size_t written = serial_.write(frame, frameLength);
        return written == frameLength;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MakcuNew] serial write failed: " << e.what() << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
    catch (...)
    {
        std::cerr << "[MakcuNew] serial write failed: unknown exception." << std::endl;
        portOpen_.store(false);
        open_.store(false);
        return false;
    }
}

void MakcuNewConnection::sendDeadBaud(unsigned int baud)
{
    // DE AD | LEN_LO LEN_HI | CMD | PAYLOAD[LEN-1]   (LEN 含 CMD 字节, 无 CRC)
    // 固件 proto_parser.cpp::dispatchDead(): CMD 0xA5 + uint32 baud -> onSetBaud()
    // 6 Mbps => DE AD 05 00 A5 80 8D 5B 00
    uint8_t frame[9];
    frame[0] = 0xDE;
    frame[1] = 0xAD;
    frame[2] = 0x05;                             // LEN = 1 (CMD) + 4 (baud)
    frame[3] = 0x00;
    frame[4] = 0xA5;                             // CMD: 切换波特率
    frame[5] = static_cast<uint8_t>(baud & 0xFF);
    frame[6] = static_cast<uint8_t>((baud >> 8) & 0xFF);
    frame[7] = static_cast<uint8_t>((baud >> 16) & 0xFF);
    frame[8] = static_cast<uint8_t>((baud >> 24) & 0xFF);

    std::lock_guard<std::mutex> lock(writeMutex_);
    if (!portOpen_.load(std::memory_order_acquire)) return;
    try { serial_.write(frame, sizeof(frame)); }
    catch (...) { portOpen_.store(false);
 open_.store(false); }
}

void MakcuNewConnection::subscribeAsync(bool on)
{
    // 固件 0x48: onSubAsync(pl[0] != 0) 置 g_async_sub，之后主动上报 0x84。
    const uint8_t enable = on ? 1 : 0;
    sendFrame(makcu::CMD_SUB_ASYNC, &enable, 1);
}

void MakcuNewConnection::feedAsciiByte(uint8_t b)
{
    // 行结束符: 提交整行 (固件 km.* 回包 / 上电横幅)
    if (b == '\n')
    {
        std::lock_guard<std::mutex> lk(asciiMutex_);
        if (!asciiAccum_.empty())
        {
            asciiLine_ = asciiAccum_;
            asciiAccum_.clear();
            ++asciiSeq_;
            asciiCv_.notify_all();
        }
        return;
    }

    if (b == '\r') return;   // 固件 println 会发 \r\n

    if (b >= 0x20 && b < 0x7F)
    {
        std::lock_guard<std::mutex> lk(asciiMutex_);
        if (asciiAccum_.size() < 256) asciiAccum_.push_back(static_cast<char>(b));
        return;
    }

    // 其余控制字节 (< 0x20, 已排除 CR/LF): 固件 ASCII 模式 km.buttons(1) 下
    // 的"单字节快速按键掩码流" (< 32)。本客户端默认走 0x48 SUB_ASYNC 的
    // 0x84 结构化帧，不会同时开启该模式，这里仅作兼容接收。
    if (b < 0x20)
    {
        applyPhysicalButtons(b);
        return;
    }

    // 其余不可打印字节: 认为不是 ASCII 行，清空避免污染
    std::lock_guard<std::mutex> lk(asciiMutex_);
    asciiAccum_.clear();
}

bool MakcuNewConnection::probeAscii(const std::string& command,
                                    const std::string& expect,
                                    unsigned int timeoutMs)
{
    // 固件只有 ASCII km.* 命令会回包 (Serial0.println)，用它做探活。
    std::unique_lock<std::mutex> lk(asciiMutex_);
    asciiLine_.clear();
    asciiAccum_.clear();          // 丢弃上一条命令残留的半行
    const uint64_t baseSeq = asciiSeq_;
    lk.unlock();

    std::string wire = command;
    wire += "\r\n";
    {
        std::lock_guard<std::mutex> wl(writeMutex_);
        if (!portOpen_.load(std::memory_order_acquire)) return false;
        try
        {
            if (serial_.write(reinterpret_cast<const uint8_t*>(wire.data()), wire.size())
                != wire.size())
                return false;
        }
        catch (...)
        {
            return false;
        }
    }

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    std::unique_lock<std::mutex> lock(asciiMutex_);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (asciiSeq_ != baseSeq
            && asciiLine_.find(expect) != std::string::npos)
        {
            std::cout << "[MakcuNew] probe reply: " << asciiLine_ << std::endl;
            return true;
        }
        asciiCv_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return false;
}

bool MakcuNewConnection::initializeProtocolSession()
{
    // 新会话: 清掉上一会话残留的按键影子，避免断线重连后状态串味。
    realButtons_.store(0, std::memory_order_release);
    injectedButtons_.store(0, std::memory_order_release);

    // 订阅 0x84 异步按键上报(real/inj 双掩码)。固件不回 ACK，
    // 但收到订阅后会【立刻补发一帧当前按键态】，所以这里等一小会儿，
    // 否则上位机接入时若用户已经按着键，会一直误判为未按下。
    subscribeAsync(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return true;
}

uint8_t MakcuNewConnection::buttonBit(int button)
{
    return button >= 1 && button <= 5
        ? static_cast<uint8_t>(1u << (button - 1)) : 0;
}

bool MakcuNewConnection::move(int x, int y)
{
    if (!open_.load(std::memory_order_acquire)) return false;
    const int cx = std::clamp(x, -32768, 32767);
    const int cy = std::clamp(y, -32768, 32767);
    if (cx == 0 && cy == 0) return true;

    // 热路径: 11 字节定长帧，固件 protoOnMove 会走 USB 就绪直发旁路，
    // 跳过 FreeRTOS 任务唤醒与上下文切换。
    uint8_t payload[4];
    payload[0] = static_cast<uint8_t>(cx & 0xFF);
    payload[1] = static_cast<uint8_t>((cx >> 8) & 0xFF);
    payload[2] = static_cast<uint8_t>(cy & 0xFF);
    payload[3] = static_cast<uint8_t>((cy >> 8) & 0xFF);
    return sendFrame(makcu::CMD_MOVE, payload, sizeof(payload));
}

void MakcuNewConnection::cancelMove()
{
    // 固件 0x05 MOVE_CANCEL: 清空设备侧 s_pending_dx/dy 与滚轮积压。
    // 直通透传下上位机确实没有本地队列，但设备侧在 USB 未就绪时会攒位移，
    // 停火时那份积压也必须丢掉，否则会继续"吐"出去。
    if (!open_.load(std::memory_order_acquire)) return;
    sendFrame(makcu::CMD_MOVE_CANCEL, nullptr, 0);
}

void MakcuNewConnection::press(int button)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    const uint8_t state = static_cast<uint8_t>(outputButtons_.fetch_or(bit) | bit);
    sendFrame(makcu::CMD_BUTTON_MASK, &state, 1);
}

void MakcuNewConnection::release(int button)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    const uint8_t inverse = static_cast<uint8_t>(~bit);
    const uint8_t state = static_cast<uint8_t>(outputButtons_.fetch_and(inverse) & inverse);
    sendFrame(makcu::CMD_BUTTON_MASK, &state, 1);
}

void MakcuNewConnection::click(int button)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    // 固件 0x12 CLICK: payload = {btn_bits, down_ms_lo, down_ms_hi}
    // 固件内定时弹起 (clickTick 每 1ms 扫描)，上位机不等回执。
    uint8_t payload[3];
    payload[0] = bit;
    payload[1] = 45;
    payload[2] = 0;
    sendFrame(makcu::CMD_CLICK, payload, sizeof(payload));
}

void MakcuNewConnection::wheel(int delta)
{
    if (!open_.load(std::memory_order_acquire)) return;
    const int8_t value = static_cast<int8_t>(std::clamp(delta, -127, 127));
    sendFrame(makcu::CMD_WHEEL, reinterpret_cast<const uint8_t*>(&value), 1);
}

bool MakcuNewConnection::physicalButtonPressed(int button) const
{
    const uint8_t bit = buttonBit(button);
    return bit && (realButtons_.load(std::memory_order_acquire) & bit) != 0;
}

void MakcuNewConnection::readerLoop()
{
    uint8_t chunk[512];
    while (!stopping_.load())
    {
        try
        {
            const size_t readable = serial_.available();
            if (readable == 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            const size_t count = serial_.read(
                chunk, std::min(readable, sizeof(chunk)));
            if (count)
            {
                receiveBuffer_.insert(receiveBuffer_.end(), chunk, chunk + count);
                consumeFrames();
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[MakcuNew] serial reader failed: " << e.what() << std::endl;
            portOpen_.store(false);
            open_.store(false);
            break;
        }
        catch (...)
        {
            std::cerr << "[MakcuNew] serial reader failed: unknown exception." << std::endl;
            portOpen_.store(false);
            open_.store(false);
            break;
        }
    }
}

void MakcuNewConnection::consumeFrames()
{
    for (;;)
    {
        const size_t avail = receiveBuffer_.size() - receiveOffset_;
        if (avail == 0) break;

        const uint8_t* p = receiveBuffer_.data() + receiveOffset_;

        // 只有 0xA5 可能是帧头首字节。其余任何字节都不可能是帧的一部分，
        // 必须【立刻】消费掉: 否则行尾 '\n' 或单字节按键掩码会永久滞留
        // 在缓冲里，导致 km.version 探活永远收不到回包。
        if (p[0] != makcu::FRAME_MAGIC0)
        {
            feedAsciiByte(p[0]);
            ++receiveOffset_;
            continue;
        }

        if (avail < 2) break;                   // 等第二个字节判断 0x5C

        if (p[1] != makcu::FRAME_MAGIC1)
        {
            feedAsciiByte(p[0]);                // 孤立的 0xA5
            ++receiveOffset_;
            continue;
        }

        if (avail < 7) break;                   // 帧头还没收全
        const size_t payloadLength = p[2];
        if (payloadLength > kMaxPayload)
        {
            // 长度非法: 把首字节当普通数据丢掉，避免死等
            feedAsciiByte(p[0]);
            ++receiveOffset_;
            continue;
        }
        const size_t frameLength = payloadLength + 7;
        if (avail < frameLength) break;         // 帧体还没收全

        const uint16_t receivedCrc = static_cast<uint16_t>(
            p[frameLength - 2] | (p[frameLength - 1] << 8));
        const uint16_t expectedCrc = makcu::crc16_modbus(p + 2, payloadLength + 3);

        if (receivedCrc == expectedCrc)
        {
            const uint8_t command = p[4];
            const uint8_t* payload = p + 5;

            if (command == 0x84)                // ASYNC_BUTTON {real, inj}
            {
                if (payloadLength >= 1)
                {
                    applyPhysicalButtons(payload[0]);
                    if (payloadLength >= 2)
                        injectedButtons_.store(payload[1],
                                               std::memory_order_release);
                }
            }
            // 固件当前不会回 ACK/NAK/VERSION/STATS，未知帧直接忽略。
        }
        receiveOffset_ += frameLength;
    }

    if (receiveOffset_
        && (receiveOffset_ >= 4096 || receiveOffset_ * 2 >= receiveBuffer_.size()))
    {
        receiveBuffer_.erase(
            receiveBuffer_.begin(), receiveBuffer_.begin() + receiveOffset_);
        receiveOffset_ = 0;
    }
}

void MakcuNewConnection::applyPhysicalButtons(uint8_t mask)
{
    realButtons_.store(static_cast<uint8_t>(mask & 0x1F),
                       std::memory_order_release);
}
