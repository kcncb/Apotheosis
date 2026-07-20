#ifndef MF_CAPTURE_H
#define MF_CAPTURE_H

#include "capture.h"
#include "gpu_jpeg_decoder.h"

#include <cuda_runtime.h>
#include <npp.h>
#include <opencv2/opencv.hpp>

#include "../mem/gpu_image.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct MFDeviceInfo
{
    int index = 0;
    std::string name;
};

// Self-written capture-card backend talking to the device directly through
// Media Foundation (IMFSourceReader). No cv::VideoCapture involved.
//
// The caller picks the device, source geometry, fps, an exact pixel format
// (NV12 / MJPG / YUY2 / RGB32 — no AUTO negotiation), and a decode location:
//
//   GPU mode  -> output is a BGR GpuImage (nvJPEG for MJPG, NPP / a small
//                CUDA kernel for the raw formats). The frame stays on the GPU
//                for the zero-copy TensorRT detector path.
//   CPU mode  -> output is a BGR cv::Mat (cv::imdecode for MJPG, cv::cvtColor
//                for the raw formats); the existing pipeline uploads it.
//
// Pipeline contract: the frame handed to inference is the centered square
// region of interest, never the whole downscaled frame. When cropping is
// enabled the centered out_side x out_side region is taken; on the GPU raw
// paths only that region is uploaded, so the per-frame PCIe transfer (the
// bottleneck on a Gen1-capped card) is bounded by the crop, not the source.
class MFCapture : public IScreenCapture
{
public:
    enum class Format
    {
        Nv12,
        Mjpg,
        Yuy2,
        Rgb32
    };

    MFCapture(int src_width,
              int src_height,
              int out_side,
              bool crop_enabled,
              int capture_fps,
              const std::string& format,
              int device_index,
              bool gpu_decode);
    ~MFCapture() override;

    MFCapture(const MFCapture&) = delete;
    MFCapture& operator=(const MFCapture&) = delete;

    cv::Mat GetNextFrameCpu() override;
    GpuImage GetNextFrameGpu() override;
    int GetSourceFpsEstimate() const override { return source_fps_.load(); }
    bool WaitFrame(int timeoutMs) override;
    bool SupportsEventWait() const override { return true; }
    void SetTargetFps(int fps) override;
    bool HandlesTargetFps() const override { return true; }

    bool IsOpen() const { return is_open_.load(); }

    static std::vector<MFDeviceInfo> EnumerateDevices();
    static Format ParseFormat(const std::string& s);
    static const char* FormatLabel(Format f);

private:
    void ReceiveThread();
    bool EnsureGpuContext();
    void TickFps();

    // MJPG-GPU 并行解码 worker 池(说明见下方 DecodeJob 成员处)。
    void StartDecodeWorkers();
    void StopDecodeWorkers();
    void DecodeWorkerLoop();
    void EnqueueJpegJob(const uint8_t* data, size_t size);
    bool EnqueueGpuOrdered(GpuImage&& frame, uint64_t seq);
    bool ShouldDispatchFrame();   // 按 target_fps_ 节流:返回 false 表示这帧应丢弃(不解码)

    // GPU decode paths (output BGR GpuImage on gpu_stream_). On a PCIe Gen1 card
    // the goal is to move and reconstruct as LITTLE as possible:
    //   raw  -> upload only the centered ROI (crop-before-upload) so the per-
    //           frame PCIe transfer is bounded by out_side, not the source.
    //   MJPG -> nvJPEG ROI decode reconstructs only the centered out_side region
    //           (decodeCropped), avoiding a full-frame decode the weak GPU can't
    //           sustain at the source fps.
    // The raw paths keep a per-frame cudaStreamSynchronize (their ROI upload +
    // NPP convert is tiny). The MJPG path does NOT sync per frame — it records a
    // completion event so the next frame's host-side entropy decode overlaps
    // this frame's GPU reconstruction (this is what closes the gap to the source
    // fps); the detector waits on that event from its own stream.
    bool PushNv12Gpu(const uint8_t* data, int width, int height, int stride);
    bool PushYuy2Gpu(const uint8_t* data, int width, int height, int stride);
    bool PushRgb32Gpu(const uint8_t* data, int width, int height, int stride);
    bool PushMjpgGpu(const uint8_t* data, size_t size);

    // Center-crop geometry within a width x height source. When cropping is
    // enabled and the source is big enough, returns the centered out_side square
    // (origin forced even so NV12/YUY2 chroma stays aligned); otherwise returns
    // the full frame so the caller resizes to out_side instead.
    void ResolveRoi(int width, int height, int& left, int& top, int& roiW, int& roiH) const;
    // Next output ring slot (records its index in current_out_idx_); the slot
    // keeps its own reference so its device buffer is reused once the consumer
    // releases it, and create() allocates fresh while a consumer still holds it.
    GpuImage& nextOutSlot();
    // Bilinear-resize src into the next ring slot (same-size = pixel copy).
    GpuImage ResizeToOut(const GpuImage& src);

    // CPU decode paths (output BGR cv::Mat).
    bool PushNv12Cpu(const uint8_t* data, int width, int height, int stride);
    bool PushYuy2Cpu(const uint8_t* data, int width, int height, int stride);
    bool PushRgb32Cpu(const uint8_t* data, int width, int height, int stride);
    bool PushMjpgCpu(const uint8_t* data, size_t size);
    cv::Mat FinalizeCpu(const cv::Mat& bgr);

    void EnqueueCpu(cv::Mat&& frame);
    void EnqueueGpu(GpuImage&& frame);

    int src_width_;
    int src_height_;
    int out_side_;
    bool crop_enabled_;
    int capture_fps_;
    Format format_;
    int device_index_;
    bool gpu_decode_;

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };
    std::atomic<int> source_fps_{ 0 };
    std::atomic<int> negotiated_fps_{ 0 };
    int source_frame_count_{ 0 };
    std::chrono::steady_clock::time_point source_fps_start_;

    int frame_width_{ 0 };
    int frame_height_{ 0 };
    int frame_stride_{ 0 };

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;   // 产帧入队后唤醒等待的消费线程
    std::queue<cv::Mat> cpu_frame_queue_;
    std::queue<GpuImage> gpu_frame_queue_;

    // MJPG-GPU 并行解码 worker 池。host 端 Huffman 熵解码(单帧 ~3.6ms)是产帧瓶颈
    // 且 CPU-bound,单线程顶不到源帧率。ReceiveThread 只读样本、把 JPEG 字节投进
    // job 队列;每个 worker 持独立 GpuJpegDecoder(nvjpegJpegState 非线程安全,必须
    // per-thread)并行解中心 ROI,吞吐随 worker 数翻倍。每帧带单调序号,
    // EnqueueGpuOrdered 只接受比已入队最大序号更新的帧,保住"队列容量 1 = 最新帧"
    // 不被乱序的旧帧覆盖。仅在 crop_enabled 的 MJPG-GPU 模式启用。
    struct DecodeJob { std::vector<uint8_t> jpeg; uint64_t seq = 0; };
    static constexpr int DECODE_WORKERS = 2;
    static constexpr int MAX_JOB_QUEUE = 3;
    std::mutex job_mutex_;
    std::condition_variable job_cv_;
    std::queue<DecodeJob> job_queue_;
    std::vector<std::thread> decode_workers_;
    std::atomic<bool> workers_stop_{ false };
    uint64_t job_seq_{ 0 };                    // ReceiveThread 单线程递增
    std::atomic<uint64_t> enqueued_seq_{ 0 };  // 已入队最大序号(保序守卫)

    // 采集帧率上限(降采样限速)。worker 满速解码会吃 CPU;只要 N fps 时,在分发解码
    // 前按目标间隔丢多余帧——不睡眠(不会像 limiter 过冲压低),且丢的帧不解码省 CPU。
    // 0 或 ≥源帧率 = 满帧不丢。运行时由 SetTargetFps 更新。
    std::atomic<int> target_fps_{ 0 };
    std::chrono::steady_clock::time_point next_dispatch_{};  // ReceiveThread 单线程访问,无需锁

    // GPU state (only created in GPU mode).
    cudaStream_t gpu_stream_{ nullptr };
    NppStreamContext npp_ctx_{};
    std::unique_ptr<capture::GpuJpegDecoder> gpu_decoder_;
    uint8_t* pinned_jpeg_buffer_{ nullptr };  // pinned host staging for the MJPG H->D copy
    size_t pinned_jpeg_capacity_{ 0 };

    // Reused intermediate GPU buffers (never enqueued, so use_count()==1 and
    // create() reuses them every frame). For the raw paths a per-frame
    // cudaStreamSynchronize makes single-buffer reuse safe: frame N+1's upload
    // into a scratch cannot begin until frame N's read of it has completed.
    GpuImage scratch_a_;      // Y / packed-source ROI upload
    GpuImage scratch_b_;      // NV12 UV ROI upload
    GpuImage scratch_full_;   // full-frame BGR (resize source / MJPG full-decode fallback)

    // Output ring. Enqueued frames (or zero-copy views) reference a slot, so the
    // ring must be deeper than the frames in flight for a slot's refcount to be
    // back to 1 by the time we cycle to it. out_events_ pairs one CUDA event per
    // slot: the MJPG path records it (no per-frame sync) so the consumer waits
    // on it; the raw paths sync per frame and leave the event unused (null
    // readyEvent), which the detector/download treat as "already complete".
    static constexpr int OUT_POOL_SIZE = 8;
    std::array<GpuImage, OUT_POOL_SIZE> out_pool_;
    std::array<cudaEvent_t, OUT_POOL_SIZE> out_events_{};
    size_t out_pool_idx_{ 0 };
    size_t current_out_idx_{ 0 };  // slot index returned by the last nextOutSlot()

    // Latest-frame semantics: the producer runs at source fps and the consumer
    // throttles to capture_fps, so a deep queue just feeds the detector stale
    // frames (every backlogged frame is pure end-to-end latency for aiming).
    // Depth 1 means Enqueue drops the previous frame and keeps only the newest,
    // so the consumer always pulls the freshest frame the device has produced.
    static constexpr int MAX_QUEUE_SIZE = 1;
};

#endif // MF_CAPTURE_H
