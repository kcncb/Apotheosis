#include "eth_capture.h"
#include "Apotheosis.h"  // for global `config` (Config struct)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pcap.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

// ---- ProSexy wire protocol (must stay byte-identical to ProSexy/src/Protocol.h) ----
#pragma pack(push, 1)
struct PxEthHeader {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t etherType; // big-endian
};
struct PxFragHeader {
    uint8_t  magic[2];   // 'P','X'
    uint8_t  version;    // 1
    uint8_t  flags;
    uint32_t frameId;
    uint16_t width;
    uint16_t height;
    uint32_t totalSize;
    uint32_t fragOffset;
    uint16_t fragLen;
    uint16_t fragIndex;
    uint16_t fragCount;
};
#pragma pack(pop)

static constexpr uint8_t PX_MAGIC0 = 'P';
static constexpr uint8_t PX_MAGIC1 = 'X';

std::vector<std::pair<std::string, std::string>> EthCapture::ListAdapters()
{
    std::vector<std::pair<std::string, std::string>> out;
    pcap_if_t* all = nullptr;
    char errbuf[PCAP_ERRBUF_SIZE] = { 0 };
    if (pcap_findalldevs(&all, errbuf) != 0 || !all)
    {
        std::cerr << "[EthCapture] pcap_findalldevs failed: " << errbuf << std::endl;
        return out;
    }
    for (pcap_if_t* d = all; d; d = d->next)
    {
        std::string name = d->name ? d->name : "";
        std::string desc = d->description ? d->description : name;
        out.emplace_back(std::move(name), std::move(desc));
    }
    pcap_freealldevs(all);
    return out;
}

EthCapture::EthCapture(int width, int height, const std::string& adapter, int ethertype)
    : width_(width)
    , height_(height)
    , adapter_(adapter)
    , ethertype_(static_cast<uint16_t>(ethertype))
    , is_connected_(false)
    , should_stop_(false)
    , received_frames_(0)
    , dropped_frames_(0)
{
    Initialize();
}

EthCapture::~EthCapture()
{
    Cleanup();
}

bool EthCapture::Initialize()
{
    if (adapter_.empty())
    {
        std::cerr << "[EthCapture] No adapter configured" << std::endl;
        return false;
    }

    char errbuf[PCAP_ERRBUF_SIZE] = { 0 };

    // pcap_create + immediate mode: deliver each captured frame to userland as
    // soon as it arrives instead of batching, which is essential for the
    // low-latency 240fps target.
    pcap_t* p = pcap_create(adapter_.c_str(), errbuf);
    if (!p)
    {
        std::cerr << "[EthCapture] pcap_create failed: " << errbuf << std::endl;
        return false;
    }
    pcap_set_snaplen(p, 65536);
    pcap_set_promisc(p, 1);
    pcap_set_timeout(p, 1);           // 1ms read timeout
    pcap_set_immediate_mode(p, 1);
    // Sender uses jumbo frames (MTU 8900),每帧 ~4 包。pcap 内核缓冲是真正的
    // 抗抖动池——之前 8MB 实测在 239fps 高负载下还是会被填满+drop(尤其当
    // receive thread 阻塞在 NVDEC sync 那阵子),直接造成"产帧 ≈ 190fps"的
    // ~20% wire loss。这里直接拉到 64MB,内核缓冲 ≈ 7.4 秒;配合接收线程
    // dispCb 已经异步化(receive thread per-frame 工作 < 300us),稳态下缓冲
    // 始终空,延迟不增,只在 spike 时吃下去抗。
    pcap_set_buffer_size(p, 64 * 1024 * 1024);

    int act = pcap_activate(p);
    if (act < 0)
    {
        std::cerr << "[EthCapture] pcap_activate failed: " << pcap_geterr(p) << std::endl;
        pcap_close(p);
        return false;
    }

    // 只取入向流量,出向(本机发出去的)直接由内核 BPF 丢掉,省一遍 userland
    // 解析。对 ProSexy 这种我们只收的链路来说是纯收益,packet rate 直接减半。
    pcap_setdirection(p, PCAP_D_IN);

    // Only deliver our EtherType frames; the kernel filter keeps unrelated LAN
    // traffic out of the receive loop entirely.
    char filter[64];
    std::snprintf(filter, sizeof(filter), "ether proto 0x%04X", ethertype_);
    bpf_program prog{};
    if (pcap_compile(p, &prog, filter, 1, PCAP_NETMASK_UNKNOWN) == 0)
    {
        pcap_setfilter(p, &prog);
        pcap_freecode(&prog);
    }
    else
    {
        std::cerr << "[EthCapture] pcap_compile failed: " << pcap_geterr(p)
                  << " (continuing unfiltered)" << std::endl;
    }

    handle_ = p;
    should_stop_ = false;
    is_connected_ = true;
    received_frames_ = 0;
    dropped_frames_ = 0;
    have_frame_ = false;

    decode_thread_ = std::thread(&EthCapture::DecodeThread, this);
    receive_thread_ = std::thread(&EthCapture::ReceiveThread, this);

    std::cout << "[EthCapture] Listening on " << adapter_
              << " ethertype 0x" << std::hex << ethertype_ << std::dec << std::endl;
    return true;
}

void EthCapture::Cleanup()
{
    should_stop_ = true;
    is_connected_ = false;
    frame_cv_.notify_all();  // wake any consumer blocked in WaitFrame
    decode_cv_.notify_all();

    // pcap_breakloop unblocks a pcap_next_ex that is mid-timeout.
    if (handle_)
        pcap_breakloop(handle_);

    if (receive_thread_.joinable())
        receive_thread_.join();
    decode_cv_.notify_all();
    if (decode_thread_.joinable())
        decode_thread_.join();

    if (handle_)
    {
        pcap_close(handle_);
        handle_ = nullptr;
    }
}

cv::Mat EthCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_queue_.empty())
        return cv::Mat();
    // Low-latency: always hand the consumer the FRESHEST frame and discard any
    // older ones still queued. A FIFO that returns front() lets a transient
    // burst settle into a persistent N-frame backlog (steady-state latency =
    // N * frame interval), which is the dominant glass-to-glass delay. Draining
    // to the newest caps receiver-side buffering at a single frame.
    while (frame_queue_.size() > 1)
    {
        frame_queue_.pop();
        dropped_frames_++;
    }
    cv::Mat frame = frame_queue_.front();
    frame_queue_.pop();
    return frame;
}

GpuImage EthCapture::GetNextFrameGpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (gpu_frame_queue_.empty())
        return GpuImage();
    // See GetNextFrameCpu: drop stale frames, keep only the newest.
    while (gpu_frame_queue_.size() > 1)
    {
        gpu_frame_queue_.pop();
        dropped_frames_++;
    }
    GpuImage frame = std::move(gpu_frame_queue_.front());
    gpu_frame_queue_.pop();
    return frame;
}

void EthCapture::ReceiveThread()
{
    try
    {
        const int ethHdr = (int)sizeof(PxEthHeader);
        const int fragHdr = (int)sizeof(PxFragHeader);

        // Diagnostic: once a second, report where the throughput goes —
        //   decoded/s  : frames actually decoded+enqueued (producer rate)
        // decoded/s is produced by the independent NVDEC thread; the remaining
        // counters isolate sender, wire, reassembly and Npcap losses.
        auto statT0 = std::chrono::steady_clock::now();
        int lastRecv = received_frames_.load();
        // Per-window sender-rate / loss tracking. The sender stamps a
        // monotonically increasing frameId per frame, so the span of frameIds
        // seen == how many frames the sender actually emitted, while `started`
        // == frames we got at least one fragment for. senderSpan-started ==
        // frames lost whole on the wire; started-completed == frames that lost a
        // fragment (incomplete). This pinpoints whether the last few fps are
        // wire loss vs the sender simply not emitting a full 240.
        uint64_t winStarted = 0;
        uint64_t winCompleted = 0;
        uint64_t winPartialLost = 0;
        uint32_t winFirstId = 0, winLastId = 0;
        bool winHaveId = false;

        // pcap_stats() 的两个计数器是累计值(自打开 handle 起),每秒做差得
        // 增量塞到 atomics 里。ps_drop 是 pcap 内核 ring 满 drop,ps_ifdrop
        // 是 NIC/驱动层 drop。
        unsigned int lastPsDrop   = 0;
        unsigned int lastPsIfDrop = 0;
        {
            pcap_stat ps0{};
            if (pcap_stats(handle_, &ps0) == 0)
            {
                lastPsDrop   = ps0.ps_drop;
                lastPsIfDrop = ps0.ps_ifdrop;
            }
        }

        while (!should_stop_)
        {
            // 诊断:统计窗口每秒滚动一次,把"真实产帧 fps"推给 source_fps_,UI
            // 通过 GetSourceFpsEstimate 拿到。verbose 时再打印到 stderr。
            {
                auto nowT = std::chrono::steady_clock::now();
                double el = std::chrono::duration<double>(nowT - statT0).count();
                if (el >= 1.0)
                {
                    int rec = received_frames_.load();
                    const int decFps = static_cast<int>(std::lround((rec - lastRecv) / el));
                    const uint32_t spanRaw = winHaveId ? (winLastId - winFirstId + 1) : 0;
                    const int spanFps = static_cast<int>(std::lround(spanRaw / el));
                    const int startedFps = static_cast<int>(std::lround(winStarted / el));
                    // wire loss = sender 发了但一个 frag 都没到的;partial = 收到
                    // frag 但凑不齐的(reassembly 失败)。
                    const int wireLost    = std::max(0, spanFps - startedFps);
                    const int completedFps = static_cast<int>(std::lround(winCompleted / el));
                    // Count only frames that were actually superseded while
                    // incomplete. A started-completed subtraction jitters when
                    // one frame crosses the statistics-window boundary.
                    const int partialLost = static_cast<int>(std::lround(winPartialLost / el));

                    pcap_stat ps{};
                    int psDropInc = 0, psIfDropInc = 0;
                    if (pcap_stats(handle_, &ps) == 0)
                    {
                        psDropInc   = (int)((ps.ps_drop   - lastPsDrop)   / el);
                        psIfDropInc = (int)((ps.ps_ifdrop - lastPsIfDrop) / el);
                        lastPsDrop   = ps.ps_drop;
                        lastPsIfDrop = ps.ps_ifdrop;
                    }

                    source_fps_.store(decFps);
                    sender_span_fps_.store(spanFps);
                    wire_lost_fps_.store(wireLost);
                    partial_lost_fps_.store(partialLost);
                    pcap_kernel_dropped_fps_.store(psDropInc);
                    pcap_ifdropped_fps_.store(psIfDropInc);

                    if (config.verbose)
                    {
                        std::cerr << "[EthCapture] decoded/s=" << decFps
                                  << " senderSpan/s=" << spanFps
                                  << " started/s=" << startedFps
                                  << " completed/s=" << completedFps
                                  << " wireLost/s=" << wireLost
                                  << " partial/s=" << partialLost
                                  << " psDrop/s=" << psDropInc
                                  << " psIfDrop/s=" << psIfDropInc
                                  << " decQueueDrop/s=" << decode_queue_dropped_fps_.load()
                                  << " queueP95=" << (decode_queue_p95_us_.load() / 1000.0) << "ms"
                                  << " submitP95=" << (decode_submit_p95_us_.load() / 1000.0) << "ms"
                                  << " gpuAllocs=" << GpuImage::allocationCount()
                                  << " dec=" << last_dec_w_.load() << "x" << last_dec_h_.load()
                                  << std::endl;
                    }
                    lastRecv = rec;
                    statT0 = nowT;
                    winStarted = 0;
                    winCompleted = 0;
                    winPartialLost = 0;
                    winHaveId = false;
                }
            }

            pcap_pkthdr* header = nullptr;
            const u_char* data = nullptr;
            int r = pcap_next_ex(handle_, &header, &data);
            if (r == 0)
                continue;            // read timeout, no packet
            if (r < 0)
                break;               // PCAP_ERROR or breakloop

            if (header->caplen < (bpf_u_int32)(ethHdr + fragHdr))
                continue;

            const PxFragHeader* fh =
                reinterpret_cast<const PxFragHeader*>(data + ethHdr);
            if (fh->magic[0] != PX_MAGIC0 || fh->magic[1] != PX_MAGIC1)
                continue;

            const uint32_t totalSize = fh->totalSize;
            const uint16_t fragCount = fh->fragCount;
            const uint16_t fragIndex = fh->fragIndex;
            const uint32_t fragOffset = fh->fragOffset;
            const uint16_t fragLen = fh->fragLen;

            if (totalSize == 0 || totalSize > (uint32_t)MAX_FRAME_SIZE ||
                fragCount == 0 || fragIndex >= fragCount)
                continue;
            // Payload must actually be present in the captured bytes.
            if ((bpf_u_int32)(ethHdr + fragHdr + fragLen) > header->caplen)
                continue;
            if ((size_t)fragOffset + fragLen > totalSize)
                continue;

            // New frame? Start reassembly fresh (dropping any incomplete prior).
            if (!have_frame_ || fh->frameId != cur_frame_id_)
            {
                if (have_frame_ && frags_got_ < frag_count_)
                {
                    dropped_frames_++;
                    winPartialLost++;
                }
                // diagnostic: track sender frameId span + frames started
                if (!winHaveId) { winFirstId = fh->frameId; winHaveId = true; }
                winLastId = fh->frameId;
                winStarted++;
                cur_frame_id_ = fh->frameId;
                expected_size_ = totalSize;
                frag_count_ = fragCount;
                frags_got_ = 0;
                // Every completed frame is fully overwritten by its fragments;
                // resize preserves the allocation and avoids zero-filling the
                // entire encoded frame 240 times per second.
                asm_buf_.resize(totalSize);
                frag_received_.assign(fragCount, 0);
                have_frame_ = true;
            }

            const u_char* payload = data + ethHdr + fragHdr;
            std::memcpy(asm_buf_.data() + fragOffset, payload, fragLen);

            if (!frag_received_[fragIndex])
            {
                frag_received_[fragIndex] = 1;
                frags_got_++;
            }

            if (frags_got_ == frag_count_)
            {
                winCompleted++;
                EnqueueDecode(asm_buf_.data(), expected_size_);
                have_frame_ = false;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[EthCapture] Receive thread crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[EthCapture] Receive thread crashed: unknown exception." << std::endl;
    }
}

void EthCapture::EnqueueDecode(const uint8_t* nalu, size_t nalu_size)
{
    if (!nalu || nalu_size == 0 || should_stop_.load())
        return;

    DecodeJob job;
    {
        std::lock_guard<std::mutex> lock(decode_mutex_);
        if (!decode_free_buffers_.empty())
        {
            job.nalu = std::move(decode_free_buffers_.back());
            decode_free_buffers_.pop_back();
        }
        while (static_cast<int>(decode_queue_.size()) >= MAX_DECODE_QUEUE)
        {
            DecodeJob stale = std::move(decode_queue_.front());
            decode_queue_.pop();
            decode_free_buffers_.push_back(std::move(stale.nalu));
            dropped_frames_++;
            decode_queue_dropped_total_.fetch_add(1, std::memory_order_relaxed);
        }
        job.nalu.resize(nalu_size);
        std::memcpy(job.nalu.data(), nalu, nalu_size);
        job.assembled_at = std::chrono::steady_clock::now();
        decode_queue_.push(std::move(job));
    }
    decode_cv_.notify_one();
}

void EthCapture::DecodeThread()
{
    gpu_decoder_ = std::make_unique<capture::GpuH264Decoder>();
    if (!gpu_decoder_->init()
        || cudaStreamCreateWithFlags(&decode_stream_, cudaStreamNonBlocking) != cudaSuccess)
    {
        std::cerr << "[EthCapture] NVDEC/CUDA init failed; stopping capture" << std::endl;
        should_stop_.store(true);
        is_connected_.store(false);
        if (handle_) pcap_breakloop(handle_);
        decode_cv_.notify_all();
    }
    else
    {
        // Parser callbacks execute on this decode thread. GpuH264Decoder puts a
        // per-output-slot completion event on each image before this sink runs.
        gpu_decoder_->setSink([this](GpuImage&& img) {
            if (img.empty()) return;
            last_dec_w_.store(img.cols());
            last_dec_h_.store(img.rows());
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                while (gpu_frame_queue_.size() >= MAX_QUEUE_SIZE)
                {
                    gpu_frame_queue_.pop();
                    dropped_frames_++;
                }
                gpu_frame_queue_.push(std::move(img));
                received_frames_++;
            }
            frame_cv_.notify_one();
        });

        auto statsStart = std::chrono::steady_clock::now();
        uint64_t lastQueueDrops = decode_queue_dropped_total_.load();
        std::vector<int> queueWaitSamples;
        std::vector<int> submitSamples;
        queueWaitSamples.reserve(512);
        submitSamples.reserve(512);

        while (!should_stop_.load())
        {
            DecodeJob job;
            {
                std::unique_lock<std::mutex> lock(decode_mutex_);
                decode_cv_.wait(lock, [this] {
                    return should_stop_.load() || !decode_queue_.empty();
                });
                if (should_stop_.load())
                    break;
                job = std::move(decode_queue_.front());
                decode_queue_.pop();
            }

            const auto submitStart = std::chrono::steady_clock::now();
            queueWaitSamples.push_back(static_cast<int>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    submitStart - job.assembled_at).count()));
            DecodeAndEnqueue(job.nalu.data(), job.nalu.size());
            const auto submitEnd = std::chrono::steady_clock::now();
            submitSamples.push_back(static_cast<int>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    submitEnd - submitStart).count()));

            if (submitEnd - statsStart >= std::chrono::seconds(1))
            {
                auto p95 = [](std::vector<int>& samples) -> int {
                    if (samples.empty()) return 0;
                    const size_t index = (samples.size() - 1) * 95 / 100;
                    std::nth_element(samples.begin(), samples.begin() + index, samples.end());
                    return samples[index];
                };
                decode_queue_p95_us_.store(p95(queueWaitSamples));
                decode_submit_p95_us_.store(p95(submitSamples));
                const uint64_t drops = decode_queue_dropped_total_.load();
                decode_queue_dropped_fps_.store(static_cast<int>(drops - lastQueueDrops));
                lastQueueDrops = drops;
                queueWaitSamples.clear();
                submitSamples.clear();
                statsStart = submitEnd;
            }

            std::lock_guard<std::mutex> lock(decode_mutex_);
            if (decode_free_buffers_.size() < MAX_DECODE_QUEUE + 1)
                decode_free_buffers_.push_back(std::move(job.nalu));
        }
    }

    if (decode_stream_)
        cudaStreamSynchronize(decode_stream_);
    gpu_decoder_.reset();
    if (decode_stream_) { cudaStreamDestroy(decode_stream_); decode_stream_ = nullptr; }

    std::lock_guard<std::mutex> lock(decode_mutex_);
    std::queue<DecodeJob> empty;
    decode_queue_.swap(empty);
    decode_free_buffers_.clear();
}

void EthCapture::DecodeAndEnqueue(const uint8_t* nalu, size_t nalu_size)
{
    if (!gpu_decoder_ || !decode_stream_) return;
    // Sender is all-intra, so each NALU bundle is self-contained (SPS+PPS+IDR).
    // Runs only on DecodeThread; parser callbacks never execute on the Npcap
    // receive thread.
    if (!gpu_decoder_->parse(nalu, nalu_size, decode_stream_))
    {
        std::cerr << "[EthCapture] NVDEC parse failed (size=" << nalu_size << ")" << std::endl;
        dropped_frames_++;
        return;
    }

    if (last_dec_w_.load() != 0 &&
        (last_dec_w_.load() != width_ || last_dec_h_.load() != height_))
    {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::cerr << "[EthCapture] WARNING: sender crop " << last_dec_w_.load()
                      << "x" << last_dec_h_.load() << " != detection_resolution "
                      << width_ << "x" << height_
                      << " -> set ProSexy crop = detection_resolution to avoid model-scale mismatch" << std::endl;
        }
    }
}

bool EthCapture::WaitFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(frame_mutex_);
    if (!gpu_frame_queue_.empty() || !frame_queue_.empty())
        return true;
    // Event-driven: sleep until the receive thread enqueues a frame (or we stop)
    // instead of the consumer's fixed 1ms poll. This removes the per-frame sleep
    // that was capping throughput and also minimizes pickup latency.
    frame_cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return should_stop_.load() || !gpu_frame_queue_.empty() || !frame_queue_.empty();
    });
    return !gpu_frame_queue_.empty() || !frame_queue_.empty();
}
