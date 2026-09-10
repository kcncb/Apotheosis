#include "MakcuNew.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>

namespace
{
constexpr size_t kMaxPayload = 244;
constexpr unsigned int kStartupBaud = 115200;
constexpr unsigned int kFastSessionBaud = 4000000;
void putI16(std::vector<uint8_t>& output, int value)
{
    const auto v = static_cast<uint16_t>(static_cast<int16_t>(value));
    output.push_back(static_cast<uint8_t>(v));
    output.push_back(static_cast<uint8_t>(v >> 8));
}
}

MakcuNewConnection::MakcuNewConnection(const std::string& port, unsigned int baudRate)
    : port_(port), baudRate_(baudRate ? baudRate : kStartupBaud)
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
        if (requestedBaud == kFastSessionBaud) {
            // 只有用户明确配置 4M 才执行 START_PID 自动切速流程。
            if (openSerial(kStartupBaud)) {
                startReader();
                if (sendAndWaitAck(makcu::CMD_START_PID, nullptr, 0, 500)) {
                    stopReader();
                    serial_.setBaudrate(kFastSessionBaud);
                    baudRate_ = kFastSessionBaud;
                    receiveBuffer_.clear();
                    receiveOffset_ = 0;
                    startReader();
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    if (initializeProtocolSession()) {
                        std::cout << "[MakcuNew] V3R5 mouse firmware connected! PORT: "
                                  << port_ << " @ " << baudRate_ << std::endl;
                        return;
                    }
                }
                closeSession();
            }

            // 仅在配置就是 4M 时尝试接管已经切速的固件会话。
            if (openSerial(kFastSessionBaud)) {
                baudRate_ = kFastSessionBaud;
                startReader();
                if (initializeProtocolSession()) {
                    std::cout << "[MakcuNew] Rejoined V3R5 mouse session! PORT: "
                              << port_ << " @ " << baudRate_ << std::endl;
                    return;
                }
                closeSession();
            }
        } else {
            // 非4M配置不发送START_PID，也不进行任何隐式切速。
            if (openSerial(requestedBaud)) {
                baudRate_ = requestedBaud;
                startReader();
                if (initializeProtocolSession()) {
                    std::cout << "[MakcuNew] V3R5 mouse firmware connected! PORT: "
                              << port_ << " @ " << baudRate_ << std::endl;
                    return;
                }
                closeSession();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[MakcuNew] Error: " << e.what() << std::endl;
    }
    std::cerr << "[MakcuNew] Unable to establish protocol session on "
              << port_ << std::endl;
    open_.store(false);
}

MakcuNewConnection::~MakcuNewConnection()
{
    if (open_) {
        const uint8_t disable = 0;
        sendAndWaitAck(makcu::CMD_SUB_ASYNC, &disable, 1, 150);
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
        if (serial_.isOpen()) serial_.close();
        serial_.setPort(port_);
        serial_.setBaudrate(baudRate);
        serial::Timeout timeout(5, 10, 0, 100, 0);
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
    output[frameLength - 2] = static_cast<uint8_t>(crc);
    output[frameLength - 1] = static_cast<uint8_t>(crc >> 8);
    if (sentSequence) *sentSequence = sequence;
    return frameLength;
}

bool MakcuNewConnection::sendFrame(
    uint8_t command, const uint8_t* payload, size_t length, uint8_t* sentSequence)
{
    if (!open_ || length > kMaxPayload || (length && !payload)) return false;
    std::lock_guard<std::mutex> lock(writeMutex_);
    std::array<uint8_t, kMaxPayload + 7> frame{};
    const size_t frameLength = encodeFrameLocked(
        frame.data(), frame.size(), command, payload, length, sentSequence);
    if (!frameLength) return false;
    try {
        const size_t written = serial_.write(frame.data(), frameLength);
        if (written != frameLength)
            std::cerr << "[MakcuNew] partial serial write: " << written
                      << "/" << frameLength << std::endl;
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
            return ackReceived_ && ackSequence_ == sequence
                && ackCommand_ == command;
        });
    return received && ackAccepted_;
}

bool MakcuNewConnection::refreshState(unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(stateMutex_);
    stateReceived_ = false;
    uint8_t sequence = 0;
    if (!sendFrame(makcu::CMD_GET_STATE, nullptr, 0, &sequence)) return false;
    return stateCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this, sequence] {
            return stateReceived_ && stateSequence_ == sequence;
        });
}

bool MakcuNewConnection::refreshHidCaps(unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(hidCapsMutex_);
    hidCapsReceived_ = false;
    uint8_t sequence = 0;
    if (!sendFrame(makcu::CMD_GET_HID_CAPS, nullptr, 0, &sequence)) return false;
    return hidCapsCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this, sequence] {
            return hidCapsReceived_ && hidCapsSequence_ == sequence;
        });
}

bool MakcuNewConnection::queryVersion(
    makcu::VersionInfo& out, unsigned int timeoutMs)
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
        out = versionInfo_;
    return received;
}

bool MakcuNewConnection::queryPipeline(
    makcu::PipelineState& out, unsigned int timeoutMs)
{
    std::unique_lock<std::mutex> lock(pipelineMutex_);
    pipelineReceived_ = false;
    uint8_t sequence = 0;
    if (!sendFrame(makcu::CMD_GET_PIPELINE, nullptr, 0, &sequence))
        return false;
    const bool received = pipelineCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this, sequence] {
            return pipelineReceived_ && pipelineSequence_ == sequence;
        });
    if (received)
        out = pipelineState_;
    return received;
}

bool MakcuNewConnection::initializeProtocolSession()
{
    makcu::VersionInfo version{};
    if (!queryVersion(version))
        return false;
    if (std::strcmp(version.firmware, makcu::EXPECTED_FIRMWARE_VERSION) != 0) {
        std::cerr << "[MakcuNew] Unsupported firmware: " << version.firmware
                  << ", expected " << makcu::EXPECTED_FIRMWARE_VERSION
                  << "." << std::endl;
        return false;
    }

    if (!refreshHidCaps()) return false;
    {
        std::lock_guard<std::mutex> lock(hidCapsMutex_);
        const unsigned int pollMs = std::max<unsigned int>(1, hidCaps_.poll_interval_ms);
        hidPollMs_.store(pollMs, std::memory_order_release);
        pidTtlMs_.store(static_cast<uint16_t>(std::max<unsigned int>(
            makcu::PID_TTL_DEFAULT_MS, 2 * pollMs)));
        axisMinX_.store(std::clamp(hidCaps_.x_min, -32768, 0), std::memory_order_release);
        axisMaxX_.store(std::clamp(hidCaps_.x_max, 0, 32767), std::memory_order_release);
        axisMinY_.store(std::clamp(hidCaps_.y_min, -32768, 0), std::memory_order_release);
        axisMaxY_.store(std::clamp(hidCaps_.y_max, 0, 32767), std::memory_order_release);
        if (!hidCaps_.mounted) {
            std::cerr << "[MakcuNew] HID mirror is not mounted yet." << std::endl;
        }
    }

    const uint8_t enable = 1;
    return sendAndWaitAck(makcu::CMD_SUB_ASYNC, &enable, 1)
        && refreshState();
}

uint8_t MakcuNewConnection::buttonBit(int button)
{
    return button >= 1 && button <= 5
        ? static_cast<uint8_t>(1u << (button - 1)) : 0;
}

bool MakcuNewConnection::move(int x, int y)
{
    if (!open_.load(std::memory_order_acquire)) return false;
    const int dx = std::clamp(x, -32768, 32767);
    const int dy = std::clamp(y, -32768, 32767);
    if (dx == 0 && dy == 0) return true;

    // V3R5 与原版 MAKCU 一样使用 latest-value 单槽。每个真实检测结果
    // 只发送一次；PC 不插值、不保留余量，也不等待成功 ACK。
    std::vector<uint8_t> payload;
    payload.reserve(6);
    putI16(payload, dx);
    putI16(payload, dy);
    const uint16_t ttl = pidTtlMs_.load(std::memory_order_acquire);
    payload.push_back(static_cast<uint8_t>(ttl));
    payload.push_back(static_cast<uint8_t>(ttl >> 8));
    return sendFrame(makcu::CMD_PID_MOVE_LATEST,
                     payload.data(), payload.size());
}

bool MakcuNewConnection::moveConfirmed(int x, int y)
{
    if (!open_.load(std::memory_order_acquire))
        return false;

    std::lock_guard<std::mutex> confirmedLock(confirmedMoveMutex_);

    // 独占移动先等 V3R5 latest槽变空，防止上一份普通瞄准
    // 修正的 USB 报告被误当成本段回执。

    int remainingX = std::clamp(x, -32768, 32767);
    int remainingY = std::clamp(y, -32768, 32767);
    const unsigned int pollMs = std::max<unsigned int>(
        1, hidPollMs_.load(std::memory_order_acquire));
    const unsigned int queryTimeoutMs = std::max(50u, pollMs * 8u);
    const uint16_t ttl = static_cast<uint16_t>(std::clamp<unsigned int>(
        std::max(50u, pollMs * 4u),
        makcu::PID_TTL_MIN_MS, makcu::PID_TTL_MAX_MS));

    while (remainingX != 0 || remainingY != 0)
    {
        makcu::PipelineState baseline{};
        const auto idleDeadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(queryTimeoutMs);
        do
        {
            if (!queryPipeline(baseline, queryTimeoutMs))
                return false;
            if (baseline.queued_reports == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        } while (std::chrono::steady_clock::now() < idleDeadline);
        if (baseline.queued_reports != 0)
            return false;

        const int minX = axisMinX_.load(std::memory_order_acquire);
        const int maxX = axisMaxX_.load(std::memory_order_acquire);
        const int minY = axisMinY_.load(std::memory_order_acquire);
        const int maxY = axisMaxY_.load(std::memory_order_acquire);
        const int dx = std::clamp(remainingX, minX, maxX);
        const int dy = std::clamp(remainingY, minY, maxY);
        if ((remainingX != 0 && dx == 0) || (remainingY != 0 && dy == 0))
            return false;

        bool emitted = false;
        for (int attempt = 0; attempt < 3 && !emitted; ++attempt)
        {
            std::vector<uint8_t> payload;
            payload.reserve(6);
            putI16(payload, dx);
            putI16(payload, dy);
            payload.push_back(static_cast<uint8_t>(ttl));
            payload.push_back(static_cast<uint8_t>(ttl >> 8));
            if (!sendFrame(makcu::CMD_PID_MOVE_LATEST,
                           payload.data(), payload.size()))
                return false;

            const auto emitDeadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(queryTimeoutMs);
            bool submissionFailureReported = false;
            while (std::chrono::steady_clock::now() < emitDeadline)
            {
                makcu::PipelineState current{};
                if (!queryPipeline(current, queryTimeoutMs))
                    continue;
                if (current.emitted_reports != baseline.emitted_reports)
                {
                    emitted = true;
                    break;
                }
                if (current.rejected_reports != baseline.rejected_reports)
                {
                    baseline = current;
                    submissionFailureReported = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
            }
            // V3R5 的 rejected_reports 表示单次 USB 提交失败。只有设备
            // 明确报告失败才重发；查询超时不能盲目重发，避免双倍移动。
            if (!emitted && !submissionFailureReported)
                return false;
        }
        if (!emitted)
            return false;

        remainingX -= dx;
        remainingY -= dy;
    }
    return true;
}

void MakcuNewConnection::cancelMove()
{
    // V3R5 上位机不再维护移动队列或余量；固件槽中的最新值由发送
    // 任务立即取走，因此这里没有可取消的本地状态。
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
    const uint8_t payload[3]{bit, 45, 0};
    sendAndWaitAck(makcu::CMD_CLICK, payload, sizeof(payload));
}

void MakcuNewConnection::wheel(int delta)
{
    const int8_t value = static_cast<int8_t>(std::clamp(delta, -127, 127));
    sendFrame(makcu::CMD_WHEEL,
              reinterpret_cast<const uint8_t*>(&value), 1);
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
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
                    && receiveBuffer_[receiveOffset_ + 1] == makcu::FRAME_MAGIC1))
            ++receiveOffset_;
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
            if (command == makcu::CMD_ASYNC_BUTTON) {
                makcu::AsyncButtonState buttons;
                if (makcu::decode_async_button_payload(payload, payloadLength, buttons)) {
                    applyPhysicalButtons(buttons.real_button_mask);
                    injectedButtons_.store(
                        buttons.injected_button_mask, std::memory_order_release);
                }
            } else if (command == makcu::CMD_STATE_RESP) {
                makcu::DeviceState state;
                if (makcu::decode_state_payload(payload, payloadLength, state)) {
                    applyPhysicalButtons(state.real_button_mask);
                    injectedButtons_.store(
                        state.injected_button_mask, std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lock(stateMutex_);
                        deviceState_ = state;
                        stateSequence_ = sequence;
                        stateReceived_ = true;
                    }
                    stateCv_.notify_all();
                }
            } else if (command == makcu::CMD_VERSION_RESP) {
                makcu::VersionInfo version;
                if (makcu::decode_version_payload(payload, payloadLength, version)) {
                    {
                        std::lock_guard<std::mutex> lock(versionMutex_);
                        versionInfo_ = version;
                        versionSequence_ = sequence;
                        versionReceived_ = true;
                    }
                    versionCv_.notify_all();
                }
            } else if (command == makcu::CMD_HID_CAPS_RESP) {
                makcu::HidCaps caps;
                if (makcu::decode_hid_caps_payload(payload, payloadLength, caps)) {
                    {
                        std::lock_guard<std::mutex> lock(hidCapsMutex_);
                        hidCaps_ = caps;
                        hidCapsSequence_ = sequence;
                        hidCapsReceived_ = true;
                    }
                    hidCapsCv_.notify_all();
                }
            } else if (command == makcu::CMD_PIPELINE_RESP) {
                makcu::PipelineState state;
                if (makcu::decode_pipeline_payload(payload, payloadLength, state)) {
                    {
                        std::lock_guard<std::mutex> lock(pipelineMutex_);
                        pipelineState_ = state;
                        pipelineSequence_ = sequence;
                        pipelineReceived_ = true;
                    }
                    pipelineCv_.notify_all();
                }
            } else if ((command == makcu::CMD_ACK || command == makcu::CMD_NAK)
                       && payloadLength >= 2) {
                {
                    std::lock_guard<std::mutex> lock(ackMutex_);
                    ackSequence_ = sequence;
                    ackCommand_ = payload[0];
                    ackAccepted_ = command == makcu::CMD_ACK && payload[1] == 0;
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
