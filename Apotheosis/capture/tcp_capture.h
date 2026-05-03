#ifndef TCP_CAPTURE_H
#define TCP_CAPTURE_H

#include "capture.h"
#include "gpu_jpeg_decoder.h"

#include <cuda_runtime.h>
#include <opencv2/opencv.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

class TCPCapture : public IScreenCapture
{
public:
    TCPCapture(int width, int height, const std::string& ip = "0.0.0.0", int port = 1234);
    ~TCPCapture();

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;

    bool Initialize();
    void Cleanup();

    void SetTCPParams(const std::string& ip, int port);
    bool IsConnected() const { return is_connected_.load(); }
    int GetReceivedFrames() const { return received_frames_.load(); }
    int GetDroppedFrames() const { return dropped_frames_.load(); }

private:
    void ReceiveThread();
    bool ParseMJPEGFrame(std::vector<uint8_t>& data, cv::Mat& frame);
    bool DecodeJpegGpu(const uint8_t* jpeg, size_t jpeg_size);

    int width_;
    int height_;
    std::string ip_;
    int port_;

    SOCKET listen_socket_;
    SOCKET client_socket_;

    std::atomic<bool> is_connected_;
    std::atomic<bool> should_stop_;
    std::atomic<int> received_frames_;
    std::atomic<int> dropped_frames_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::queue<cv::Mat> frame_queue_;
    std::queue<GpuImage> gpu_frame_queue_;

    uint8_t* pinned_jpeg_buffer_{ nullptr };
    size_t pinned_jpeg_capacity_{ 0 };
    std::unique_ptr<capture::GpuJpegDecoder> gpu_decoder_;
    cudaStream_t decode_stream_{ nullptr };

    static const int MAX_FRAME_SIZE = 8 * 1024 * 1024;
    static const int MAX_QUEUE_SIZE = 5;
};

#endif // TCP_CAPTURE_H
