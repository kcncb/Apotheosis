#include "MakcuNew.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
// 自动为当前端口设置 Windows 注册表 / 驱动层最低 LatencyTimer = 1ms
static void autoTuneCh343Latency(const std::string& portName)
{
    // 例如从 COM3 或 \\.\COM3 提取数字 3
    int portNum = 0;
    size_t pos = portName.find("COM");
    if (pos != std::string::npos) {
        portNum = std::atoi(portName.c_str() + pos + 3);
    }
    if (portNum <= 0) return;

    // 扫描注册表中的 FTDI / CH34x 串口参数键
    // HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\FTDIBUS / USB 等
    // 并尝试打开当前端口的注册表设置项
    char regSubKey[256];
    snprintf(regSubKey, sizeof(regSubKey), "SYSTEM\\CurrentControlSet\\Services\\CH341SER\\Parameters");
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regSubKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD latencyVal = 1;
        RegSetValueExA(hKey, "LatencyTimer", 0, REG_DWORD, (const BYTE*)&latencyVal, sizeof(latencyVal));
        RegCloseKey(hKey);
    }
}
#endif

namespace
{
constexpr size_t kMaxPayload = 244;
constexpr unsigned int kDefaultBaud = 4000000;
constexpr unsigned int kFallbackBaud = 115200;

void putI16(std::vector<uint8_t>& output, int value)
{
    const auto v = static_cast<uint16_t>(static_cast<int16_t>(value));
    output.push_back(static_cast<uint8_t>(v & 0xFF));
    output.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void putU16(std::vector<uint8_t>& output, uint16_t value)
{
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void putU32(std::vector<uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}
}

MakcuNewConnection::MakcuNewConnection(const std::string& port, unsigned int baudRate)
    : port_(port), baudRate_(baudRate ? baudRate : kDefaultBaud)
{
    const unsigned int requestedBaud = baudRate_;

    const auto closeSession = [this] {
        stopReader();
        try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
        open_.store(false);
        receiveBuffer_.clear();
        receiveOffset_ = 0;
    };

    try {
        // 1. 首先尝试直接使用目标波特率连接 (例如 4000000)
        if (openSerial(requestedBaud)) {
            startReader();
            if (initializeProtocolSession()) {
                std::cout << "[MakcuNew] Connected to MAKCU device on "
                          << port_ << " @ " << baudRate_ << " bps." << std::endl;
                return;
            }
            closeSession();
        }

        // 2. 如果目标波特率不是 115200 且直连未通，尝试通过 115200 发送 SET_BAUD 指令进行高速切换
        if (requestedBaud != kFallbackBaud) {
            if (openSerial(kFallbackBaud)) {
                startReader();
                std::vector<uint8_t> baudPayload;
                putU32(baudPayload, requestedBaud);

                // 发送 SET_BAUD (0x42)
                if (sendAndWaitAck(makcu::CMD_SET_BAUD, baudPayload.data(), baudPayload.size(), 300)) {
                    stopReader();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    serial_.setBaudrate(requestedBaud);
                    baudRate_ = requestedBaud;
                    startReader();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (initializeProtocolSession()) {
                        std::cout << "[MakcuNew] Switched and connected on "
                                  << port_ << " @ " << baudRate_ << " bps." << std::endl;
                        return;
                    }
                }
                closeSession();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[MakcuNew] Connection error: " << e.what() << std::endl;
    }

    std::cerr << "[MakcuNew] Failed to establish protocol session on " << port_ << std::endl;
    open_.store(false);
}

MakcuNewConnection::~MakcuNewConnection()
{
    if (open_) {
        const uint8_t disable = 0;
        sendAndWaitAck(makcu::CMD_SUB_ASYNC, &disable, 1, 100);
    }
    stopReader();
    try { if (serial_.isOpen()) serial_.close(); } catch (...) {}
    open_.store(false);
}

bool MakcuNewConnection::isOpen() const
{
    return open_.load();
}

bool MakcuNewConnection::openSerial(unsigned int baudRate)
{
    try {
#ifdef _WIN32
        autoTuneCh343Latency(port_);
#endif
        if (serial_.isOpen()) serial_.close();
        serial_.setPort(port_);
        serial_.setBaudrate(baudRate);
        // 激进的零等待超时: 读超时立即返回，写超时 0 (由底层硬件驱动处理，绝不阻塞)
        serial::Timeout timeout = serial::Timeout::simpleTimeout(0);
        serial_.setTimeout(timeout);
        serial_.open();
        const bool opened = serial_.isOpen();
        open_.store(opened);
        return opened;
    } catch (...) {
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
    const uint8_t* payload, size_t length, uint8_t* sentSequence)
{
    const size_t frameLength = length + 7;
    if (!output || capacity < frameLength || length > kMaxPayload
        || (length && !payload)) return 0;
    output[0] = makcu::FRAME_MAGIC0;
    output[1] = makcu::FRAME_MAGIC1;
    output[2] = static_cast<uint8_t>(length);
    const uint8_t sequence = ++sequence_;
    output[3] = sequence;
    output[4] = command;
    if (length) std::copy(payload, payload + length, output + 5);
    const uint16_t crc = makcu::crc16_modbus(output + 2, length + 3);
    output[frameLength - 2] = static_cast<uint8_t>(crc & 0xFF);
    output[frameLength - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    if (sentSequence) *sentSequence = sequence;
    return frameLength;
}

bool MakcuNewConnection::sendFrame(
    uint8_t command, const uint8_t* payload, size_t length, uint8_t* sentSequence)
{
    if (!open_.load(std::memory_order_acquire) || length > kMaxPayload || (length && !payload)) return false;
    std::lock_guard<std::mutex> lock(writeMutex_);
    std::array<uint8_t, kMaxPayload + 7> frame{};
    const size_t frameLength = encodeFrameLocked(
        frame.data(), frame.size(), command, payload, length, sentSequence);
    if (!frameLength) return false;
    try {
        const size_t written = serial_.write(frame.data(), frameLength);
        return written == frameLength;
    } catch (const std::exception& e) {
        std::cerr << "[MakcuNew] serial write failed: " << e.what() << std::endl;
        open_.store(false);
        return false;
    } catch (...) {
        std::cerr << "[MakcuNew] serial write failed: unknown exception." << std::endl;
        open_.store(false);
        return false;
    }
}

bool MakcuNewConnection::sendAndWaitAck(
    uint8_t command, const uint8_t* payload, size_t length, unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(ackMutex_);
    ackReceived_ = false;
    uint8_t sequence = 0;
    if (!sendFrame(command, payload, length, &sequence)) return false;
    const bool received = ackCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this, sequence, command] {
            return ackReceived_ && ackSequence_ == sequence && ackCommand_ == command;
        });
    return received && ackAccepted_;
}

bool MakcuNewConnection::queryVersion(std::string& outVersion, unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(versionMutex_);
    versionReceived_ = false;
    uint8_t sequence = 0;
    if (!sendFrame(makcu::CMD_GET_VERSION, nullptr, 0, &sequence))
        return false;
    const bool received = versionCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this, sequence] {
            return versionReceived_ && versionSequence_ == sequence;
        });
    if (received)
        outVersion = versionStr_;
    return received;
}

bool MakcuNewConnection::initializeProtocolSession()
{
    std::string ver;
    if (!queryVersion(ver, 300)) {
        // 如果固件未回版本响应，尝试订阅按键测试握手
        const uint8_t enable = 1;
        if (!sendAndWaitAck(makcu::CMD_SUB_ASYNC, &enable, 1, 300))
            return false;
    } else {
        std::cout << "[MakcuNew] Firmware Version: " << ver << std::endl;
        const uint8_t enable = 1;
        sendAndWaitAck(makcu::CMD_SUB_ASYNC, &enable, 1, 300);
    }
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
    const int16_t dx = static_cast<int16_t>(std::clamp(x, -32768, 32767));
    const int16_t dy = static_cast<int16_t>(std::clamp(y, -32768, 32767));
    if (dx == 0 && dy == 0) return true;

    // 热路径：直接通过 CMD_MOVE 组帧发送，单帧 11 字节，零等待直通
    uint8_t payload[4];
    payload[0] = static_cast<uint8_t>(dx & 0xFF);
    payload[1] = static_cast<uint8_t>((dx >> 8) & 0xFF);
    payload[2] = static_cast<uint8_t>(dy & 0xFF);
    payload[3] = static_cast<uint8_t>((dy >> 8) & 0xFF);
    return sendFrame(makcu::CMD_MOVE, payload, sizeof(payload));
}

bool MakcuNewConnection::moveConfirmed(int x, int y)
{
    return move(x, y);
}

void MakcuNewConnection::cancelMove()
{
    // 直通透传模式，无本地队列堆积
}

void MakcuNewConnection::press(int button)
{
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    const uint8_t state = static_cast<uint8_t>(outputButtons_.fetch_or(bit) | bit);
    sendFrame(makcu::CMD_BUTTON_MASK, &state, 1);
}

void MakcuNewConnection::release(int button)
{
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    const uint8_t inverse = static_cast<uint8_t>(~bit);
    const uint8_t state = static_cast<uint8_t>(outputButtons_.fetch_and(inverse) & inverse);
    sendFrame(makcu::CMD_BUTTON_MASK, &state, 1);
}

void MakcuNewConnection::click(int button)
{
    const uint8_t bit = buttonBit(button);
    if (!bit) return;
    std::vector<uint8_t> payload;
    payload.push_back(bit);
    putU16(payload, 45);
    sendAndWaitAck(makcu::CMD_CLICK, payload.data(), payload.size(), 300);
}

void MakcuNewConnection::wheel(int delta)
{
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
    uint8_t chunk[256];
    while (!stopping_) {
        try {
            const size_t readable = serial_.available();
            if (readable == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            const size_t count = serial_.read(
                chunk, std::min(readable, sizeof(chunk)));
            if (count) {
                receiveBuffer_.insert(receiveBuffer_.end(), chunk, chunk + count);
                consumeFrames();
            }
        } catch (const std::exception& e) {
            std::cerr << "[MakcuNew] serial reader failed: " << e.what() << std::endl;
            open_.store(false);
            break;
        } catch (...) {
            std::cerr << "[MakcuNew] serial reader failed: unknown exception." << std::endl;
            open_.store(false);
            break;
        }
    }
}

void MakcuNewConnection::consumeFrames()
{
    while (receiveBuffer_.size() - receiveOffset_ >= 7) {
        while (receiveOffset_ + 1 < receiveBuffer_.size()
               && !(receiveBuffer_[receiveOffset_] == makcu::FRAME_MAGIC0
                    && receiveBuffer_[receiveOffset_ + 1] == makcu::FRAME_MAGIC1)) {
            // 兼容固件单字节快速按键流 (< 32 掩码)
            const uint8_t b = receiveBuffer_[receiveOffset_];
            if (b < 32) {
                applyPhysicalButtons(b);
            }
            ++receiveOffset_;
        }
        if (receiveBuffer_.size() - receiveOffset_ < 7) break;

        const size_t payloadLength = receiveBuffer_[receiveOffset_ + 2];
        if (payloadLength > kMaxPayload) { ++receiveOffset_; continue; }
        const size_t frameLength = payloadLength + 7;
        if (receiveBuffer_.size() - receiveOffset_ < frameLength) break;
        const size_t end = receiveOffset_ + frameLength;
        const uint16_t receivedCrc = static_cast<uint16_t>(
            receiveBuffer_[end - 2] | (receiveBuffer_[end - 1] << 8));
        const uint16_t expectedCrc = makcu::crc16_modbus(
            receiveBuffer_.data() + receiveOffset_ + 2, payloadLength + 3);

        if (receivedCrc == expectedCrc) {
            const uint8_t sequence = receiveBuffer_[receiveOffset_ + 3];
            const uint8_t command = receiveBuffer_[receiveOffset_ + 4];
            const uint8_t* payload = receiveBuffer_.data() + receiveOffset_ + 5;

            if (command == 0x84) { // ASYNC_BUTTON
                if (payloadLength >= 1) {
                    applyPhysicalButtons(payload[0]);
                    if (payloadLength >= 2)
                        injectedButtons_.store(payload[1], std::memory_order_release);
                }
            } else if (command == makcu::CMD_VERSION_RESP) {
                if (payloadLength > 0) {
                    std::string ver(reinterpret_cast<const char*>(payload),
                                    std::min<size_t>(payloadLength, 16));
                    // trim trailing nulls
                    ver.erase(std::find(ver.begin(), ver.end(), '\0'), ver.end());
                    {
                        std::lock_guard<std::mutex> lock(versionMutex_);
                        versionStr_ = ver;
                        versionSequence_ = sequence;
                        versionReceived_ = true;
                    }
                    versionCv_.notify_all();
                }
            } else if ((command == makcu::CMD_ACK || command == makcu::CMD_NAK)
                       && payloadLength >= 2) {
                {
                    std::lock_guard<std::mutex> lock(ackMutex_);
                    ackSequence_ = sequence;
                    ackCommand_ = payload[0];
                    ackAccepted_ = (command == makcu::CMD_ACK && payload[1] == 0);
                    ackReceived_ = true;
                }
                ackCv_.notify_all();
            }
        }
        receiveOffset_ = end;
    }

    if (receiveOffset_
        && (receiveOffset_ >= 4096 || receiveOffset_ * 2 >= receiveBuffer_.size())) {
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
