#ifndef ETH_CAPTURE_H
#define ETH_CAPTURE_H

#include "capture.h"
#include "gpu_h264_decoder.h"

#include <opencv2/opencv.hpp>
#include <cuda_runtime.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Receiver for the ProSexy sender (../../ProSexy): raw Ethernet (layer-2)
// frames carrying a fragmented MJPEG stream, captured via npcap. This is the
// counterpart of ProSexy/src/PcapSender.cpp + Protocol.h. We bypass the IP/UDP
// stack entirely, reassemble the per-frame fragments, then decode the JPEG on
// the GPU (nvJPEG) — reusing the exact same decode/ring-pool machinery as
// UDPCapture so the hot path keeps the same zero-malloc, event-pipelined cost.
//
// Forward-declared pcap_t so npcap headers stay out of this header (they pull
// in winsock and conflict easily); the .cpp owns <pcap.h>.
struct pcap;

class EthCapture : public IScreenCapture
{
public:
    // adapter: npcap device name, e.g. "\\Device\\NPF_{GUID}". ethertype must
    // match the sender (ProSexy default 0x88B5).
    EthCapture(int width, int height, const std::string& adapter, int ethertype = 0x88B5);
    ~EthCapture();

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;

    // Event-driven frame pickup: block until a frame is enqueued (or timeout),
    // so the consumer doesn't fall back to its fixed 1ms empty-queue sleep —
    // that sleep was the real ~180fps cap. SupportsEventWait()==true also makes
    // the consumer skip its fixed-cadence frame limiter for this backend.
    bool WaitFrame(int timeoutMs) override;
    bool SupportsEventWait() const override { return true; }

    bool Initialize();
    void Cleanup();

    // Enumerate npcap capture devices as {device_name, friendly_description}.
    // device_name is what gets stored in config.eth_adapter / passed to the
    // constructor. Implemented in the .cpp so npcap/pcap.h stays out of the UI.
    static std::vector<std::pair<std::string, std::string>> ListAdapters();

    bool IsConnected() const { return is_connected_.load(); }
    int GetReceivedFrames() const { return received_frames_.load(); }
    int GetDroppedFrames() const { return dropped_frames_.load(); }

    // 真实产帧速率(receive thread 每秒成功解码+入队的帧数)。让上层 captureSourceFps
    // 显示"网络送达 + NVDEC 解出"的真实速度,而不是消费侧的 dequeue 计数——这样能
    // 把"消费循环慢"和"产帧不够"两类瓶颈区分开。
    int GetSourceFpsEstimate() const override { return source_fps_.load(); }

    // 接收侧诊断面板(供 Qt UI 查询)。所有计数都是"每秒滚动窗口"。
    // sender_span_fps_: 这一秒看到的 sender frameId 跨度,接近 sender 的真实
    //     发包速率;比它低意味着 sender 端就少了。
    // wire_lost_fps_  : sender 发了但 receive thread 一个 fragment 都没收到的帧
    //     数(senderSpan - started)。基本上就是 wire / pcap kernel drop。
    // partial_lost_fps_: 收到 fragment 但凑不齐整帧的数量(started - decoded)。
    //     reassembly 失败,通常是某个 fragment 在 pcap 队列尾被丢。
    // pcap_kernel_dropped_fps_: pcap_stats() 报告的内核 drop(ps_drop)增量。
    //     基本就是 pcap 内核 ring 满了来不及消费时的丢弃数。
    // pcap_ifdropped_fps_: NIC/driver 层 drop(ps_ifdrop)增量,网卡/驱动级丢弃。
    int GetSenderSpanFps()        const override { return sender_span_fps_.load(); }
    int GetWireLostFps()          const override { return wire_lost_fps_.load(); }
    int GetPartialLostFps()       const override { return partial_lost_fps_.load(); }
    int GetPcapKernelDroppedFps() const override { return pcap_kernel_dropped_fps_.load(); }
    int GetPcapIfDroppedFps()     const override { return pcap_ifdropped_fps_.load(); }

private:
    void ReceiveThread();
    // Push one fully-reassembled H.264 IDR NALU through NVDEC and enqueue the
    // resulting GpuImage (BGR8). ProSexy sender ships all-intra so every NALU
    // is self-contained — no reorder buffer needed.
    void DecodeAndEnqueue(const uint8_t* nalu, size_t nalu_size);

    int width_;
    int height_;
    std::string adapter_;
    uint16_t ethertype_;

    pcap* handle_{ nullptr };

    std::atomic<bool> is_connected_;
    std::atomic<bool> should_stop_;
    std::atomic<int> received_frames_;
    std::atomic<int> dropped_frames_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;  // signaled when a frame is enqueued

    std::queue<cv::Mat> frame_queue_;
    std::queue<GpuImage> gpu_frame_queue_;

    // --- fragment reassembly (single in-flight frame; ProSexy ships a whole
    //     frame's fragments back-to-back, so one slot is enough) ---
    std::vector<uint8_t> asm_buf_;       // accumulates the current JPEG
    std::vector<uint8_t> frag_received_; // per-fragment dedupe flags
    uint32_t cur_frame_id_{ 0 };
    uint32_t expected_size_{ 0 };
    uint16_t frag_count_{ 0 };
    uint16_t frags_got_{ 0 };
    bool have_frame_{ false };

    // --- GPU decode (NVDEC H.264). ProSexy sends all-intra so the parser
    //     decode latency is single-frame; no gate ring or inflight buffer
    //     needed (NVDEC silicon decodes faster than the wire delivers). The
    //     decoder sink writes the BGR8 GpuImage straight into the queue
    //     under frame_mutex_. ---
    std::unique_ptr<capture::GpuH264Decoder> gpu_decoder_;
    cudaStream_t decode_stream_{ nullptr };
    cudaEvent_t  decode_event_{ nullptr };  // signals decoded BGR ready

    std::atomic<int> last_dec_w_{ 0 };
    std::atomic<int> last_dec_h_{ 0 };
    std::atomic<int> source_fps_{ 0 };
    std::atomic<int> sender_span_fps_{ 0 };
    std::atomic<int> wire_lost_fps_{ 0 };
    std::atomic<int> partial_lost_fps_{ 0 };
    std::atomic<int> pcap_kernel_dropped_fps_{ 0 };
    std::atomic<int> pcap_ifdropped_fps_{ 0 };

    static const int MAX_FRAME_SIZE = 4 * 1024 * 1024;
    // Small ready-queue. The GPU-readiness gate already bounds decodes in flight
    // to ~1, and GetNextFrame* drains to the newest, so latency stays ~one
    // decode regardless. 2 gives the receive thread room to publish a fresh
    // frame while the consumer still holds the previous one (no stall), without
    // letting a backlog accumulate.
    static const int MAX_QUEUE_SIZE = 2;
};

#endif // ETH_CAPTURE_H
