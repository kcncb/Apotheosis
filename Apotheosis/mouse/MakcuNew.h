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

class MakcuNewConnection
{
public:
    MakcuNewConnection(const std::string& port, unsigned int baudRate);
    ~MakcuNewConnection();

    bool isOpen() const;
    bool move(int x, int y);
    // 独占动作使用：逐段等待固件确认已被 USB 端点接受，
    // 用于需要精确偿还位移的往返动作。
    bool moveConfirmed(int x, int y);
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
    void applyPhysicalButtons(uint8_t mask);
    bool initializeProtocolSession();
    bool queryVersion(makcu::VersionInfo& out,
                      unsigned int timeoutMs = 500);
    bool refreshState(unsigned int timeoutMs = 300);
    bool refreshHidCaps(unsigned int timeoutMs = 500);
    bool queryPipeline(makcu::PipelineState& out,
                       unsigned int timeoutMs = 100);
    bool sendFrame(uint8_t command, const uint8_t* payload, size_t length,
                   uint8_t* sentSequence = nullptr);
    size_t encodeFrameLocked(uint8_t* output, size_t capacity, uint8_t command,
                             const uint8_t* payload, size_t length,
                             uint8_t* sentSequence = nullptr);
    bool sendAndWaitAck(uint8_t command, const uint8_t* payload, size_t length,
                        unsigned int timeoutMs = 300);
    static uint8_t buttonBit(int button);

    serial::Serial serial_;
    std::string port_;
    unsigned int baudRate_{115200};
    std::atomic<bool> open_{false};
    std::atomic<bool> stopping_{false};
    std::thread reader_;
    std::mutex writeMutex_;
    uint8_t sequence_{0};
    std::atomic<uint8_t> outputButtons_{0};
    std::atomic<uint8_t> realButtons_{0};
    std::atomic<uint8_t> injectedButtons_{0};
    std::atomic<uint16_t> pidTtlMs_{makcu::PID_TTL_DEFAULT_MS};
    std::atomic<unsigned int> hidPollMs_{1};
    std::atomic<int> axisMinX_{-127};
    std::atomic<int> axisMaxX_{127};
    std::atomic<int> axisMinY_{-127};
    std::atomic<int> axisMaxY_{127};

    std::mutex confirmedMoveMutex_;

    std::mutex ackMutex_;
    std::condition_variable ackCv_;
    uint8_t ackSequence_{0};
    uint8_t ackCommand_{0};
    bool ackReceived_{false};
    bool ackAccepted_{false};

    std::mutex stateMutex_;
    std::condition_variable stateCv_;
    makcu::DeviceState deviceState_{};
    uint8_t stateSequence_{0};
    bool stateReceived_{false};

    std::mutex hidCapsMutex_;
    std::condition_variable hidCapsCv_;
    makcu::HidCaps hidCaps_{};
    uint8_t hidCapsSequence_{0};
    bool hidCapsReceived_{false};

    std::mutex versionMutex_;
    std::condition_variable versionCv_;
    makcu::VersionInfo versionInfo_{};
    uint8_t versionSequence_{0};
    bool versionReceived_{false};

    std::mutex pipelineMutex_;
    std::condition_variable pipelineCv_;
    makcu::PipelineState pipelineState_{};
    uint8_t pipelineSequence_{0};
    bool pipelineReceived_{false};

    std::vector<uint8_t> receiveBuffer_;
    size_t receiveOffset_{0};
};
