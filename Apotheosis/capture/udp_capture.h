#ifndef UDP_CAPTURE_H
#define UDP_CAPTURE_H

#include "capture.h"
#include "gpu_jpeg_decoder.h"

#include <opencv2/opencv.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cuda_runtime.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
    int GetSourceFpsEstimate() const override { return source_fps_.load(); }
    bool WaitFrame(int timeoutMs) override;
    bool SupportsEventWait() const override { return true; }

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
    void TickInputFps();

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
    std::atomic<int> source_fps_{ 0 };
    int source_frame_count_{ 0 };
    std::chrono::steady_clock::time_point source_fps_start_{};

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;

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

    // Pre-allocated ring of nvJPEG output buffers. Each frame decodes into the
    // next slot; GpuImage::create() reuses a slot's device buffer when its
    // shape matches and no consumer still holds a reference (use_count()==1).
    // This eliminates the per-frame cudaMalloc/cudaFree that the old "fresh
    // GpuImage every frame" pattern incurred. That matters enormously here:
    // cudaMalloc and cudaFree synchronize the WHOLE device, so at 240fps the
    // churn was fencing the detector's high-priority inference stream hundreds
    // of times a second — inflating measured inference time and capping
    // throughput. The ring must be deeper than the max frames in flight (GPU
    // queue depth + the consumer that is mid-inference) so a slot is always
    // back to use_count()==1 by the time we cycle around to it; otherwise
    // create() falls back to a fresh malloc for that slot (still correct).
    // resize_pool_ mirrors decode_pool_ for the rare case the sender ships a
    // non-target resolution and we have to GPU-resize.
    static const int DECODE_POOL_SIZE = 10;
    std::array<GpuImage, DECODE_POOL_SIZE> decode_pool_{};
    std::array<GpuImage, DECODE_POOL_SIZE> resize_pool_{};
    size_t decode_pool_index_{ 0 };

    // One CUDA event per decode slot. After decode(+resize) on decode_stream_
    // we record the matching event (no CPU sync) and hand it to the consumer
    // via GpuImage::setReadyEvent; the detector waits on it from its own stream.
    // Created lazily alongside decode_stream_ on the receive thread.
    std::array<cudaEvent_t, DECODE_POOL_SIZE> decode_events_{};

    static const int MAX_FRAME_SIZE = 1024 * 1024;
    static const int MAX_QUEUE_SIZE = 1;
};

#endif // UDP_CAPTURE_H
