#ifndef UDP_CAPTURE_H
#define UDP_CAPTURE_H

#include "capture.h"
#include "gpu_jpeg_decoder.h"

#include <opencv2/opencv.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cuda_runtime.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

class UDPCapture : public IScreenCapture
{
public:
    UDPCapture(int width, int height, const std::string& ip = "0.0.0.0", int port = 1234);
    ~UDPCapture();

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;

    bool Initialize();
    void Cleanup();

    void SetUDPParams(const std::string& ip, int port);
    bool IsConnected() const { return is_connected_.load(); }
    int GetReceivedFrames() const { return received_frames_.load(); }
    int GetDroppedFrames() const { return dropped_frames_.load(); }

private:
    void ReceiveThread();
    // Locate SOI (FFD8) / EOI (FFD9) markers in a byte stream buffered from
    // fragmented UDP datagrams. Returns false if no complete JPEG is present.
    bool FindJpegBounds(const std::vector<uint8_t>& data, size_t& start_pos, size_t& end_pos);

    int width_;
    int height_;
    std::string ip_;
    int port_;

    SOCKET socket_;
    sockaddr_in server_addr_;

    std::atomic<bool> is_connected_;
    std::atomic<bool> should_stop_;
    std::atomic<int> received_frames_;
    std::atomic<int> dropped_frames_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;

    // Legacy CPU queue (used when nvJPEG init failed and we fall back to
    // cv::imdecode), plus the preferred GPU queue populated by the nvJPEG
    // path. captureThread drains the GPU queue first; CPU queue is a fallback.
    std::queue<cv::Mat> frame_queue_;
    std::queue<GpuImage> gpu_frame_queue_;

    // Pinned staging buffer for the pending JPEG bytes. Using pinned memory
    // avoids a page-fault/copy inside nvjpegDecode's internal host->device
    // transfers. Lazily grown up to MAX_FRAME_SIZE.
    uint8_t* pinned_jpeg_buffer_{ nullptr };
    size_t pinned_jpeg_capacity_{ 0 };

    // GPU decode owned by the receive thread (nvjpegJpegState_t is not
    // thread-safe, so keep it pinned to that thread).
    std::unique_ptr<capture::GpuJpegDecoder> gpu_decoder_;
    cudaStream_t decode_stream_{ nullptr };

    static const int MAX_FRAME_SIZE = 1024 * 1024;
    static const int MAX_QUEUE_SIZE = 5;
};

#endif // UDP_CAPTURE_H
