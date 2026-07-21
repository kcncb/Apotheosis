#include "avermedia_capture.h"
#include "avermedia_sdk.h"
#include "../gpu_color_ops.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

AverMediaCapture::AverMediaCapture(int src_width,
                                   int src_height,
                                   int out_side,
                                   bool crop_enabled,
                                   int capture_fps,
                                   uint32_t device_index,
                                   bool prefer_hdmi_source,
                                   bool gpu_decode)
    : src_width_(std::max(0, src_width))
    , src_height_(std::max(0, src_height))
    , out_side_(std::max(1, out_side))
    , crop_enabled_(crop_enabled)
    , capture_fps_(std::max(0, capture_fps))
    , device_index_(device_index)
    , prefer_hdmi_source_(prefer_hdmi_source)
    , gpu_decode_(gpu_decode)
{
    source_fps_start_ = std::chrono::steady_clock::now();
    if (!OpenDevice())
    {
        std::cerr << "[AVerMedia] Failed to open device #" << device_index_ << std::endl;
        return;
    }
    if (gpu_decode_ && !EnsureGpuContext())
    {
        std::cerr << "[AVerMedia] GPU conversion unavailable; using CPU." << std::endl;
        gpu_decode_ = false;
    }
    is_open_.store(true);
    transform_thread_ = std::thread(&AverMediaCapture::TransformLoop, this);
}

AverMediaCapture::~AverMediaCapture()
{
    CloseDevice();
    if (transform_thread_.joinable())
        transform_thread_.join();
    if (gpu_stream_) cudaStreamSynchronize(gpu_stream_);
    for (auto& event : gpu_events_)
        if (event) cudaEventDestroy(event);
    for (auto& slot : raw_pool_)
        if (slot.gpu_done) cudaEventDestroy(slot.gpu_done);
    if (gpu_stream_) cudaStreamDestroy(gpu_stream_);
}

bool AverMediaCapture::EnsureGpuContext()
{
    if (cudaStreamCreateWithFlags(&gpu_stream_, cudaStreamNonBlocking) != cudaSuccess)
        return false;
    int dev = 0;
    cudaGetDevice(&dev);
    cudaDeviceProp props{};
    cudaGetDeviceProperties(&props, dev);
    npp_ctx_.hStream = gpu_stream_;
    npp_ctx_.nCudaDeviceId = dev;
    npp_ctx_.nMultiProcessorCount = props.multiProcessorCount;
    npp_ctx_.nMaxThreadsPerMultiProcessor = props.maxThreadsPerMultiProcessor;
    npp_ctx_.nMaxThreadsPerBlock = props.maxThreadsPerBlock;
    npp_ctx_.nSharedMemPerBlock = props.sharedMemPerBlock;
    npp_ctx_.nCudaDevAttrComputeCapabilityMajor = props.major;
    npp_ctx_.nCudaDevAttrComputeCapabilityMinor = props.minor;
    cudaStreamGetFlags(gpu_stream_, &npp_ctx_.nStreamFlags);
    for (int i = 0; i < GPU_POOL_SIZE; ++i)
    {
        if (!gpu_pool_[i].create(out_side_, out_side_, 3)) return false;
        cudaEventCreateWithFlags(&gpu_events_[i], cudaEventDisableTiming);
    }
    for (auto& slot : raw_pool_)
        cudaEventCreateWithFlags(&slot.gpu_done, cudaEventDisableTiming);
    const int sourceW = src_width_.load();
    const int sourceH = src_height_.load();
    const bool crop = crop_enabled_ && sourceW >= out_side_ && sourceH >= out_side_;
    const int workW = crop ? out_side_ : sourceW;
    const int workH = crop ? out_side_ : sourceH;
    if (workW <= 0 || workH <= 0
        || !scratch_a_.create(workH, workW, nv12_format_ ? 1 : 4)
        || (nv12_format_ && !scratch_b_.create(workH / 2, workW, 1))
        || !scratch_full_.create(workH, workW, 3))
        return false;
    return true;
}

bool AverMediaCapture::OpenDevice()
{
    auto& sdk = avermedia::SdkLoader::Instance();
    if (!sdk.IsUsable())
    {
        std::cerr << "[AVerMedia] SDK not usable — cannot construct AverMediaCapture."
                  << std::endl;
        return false;
    }
    const avermedia::ApiTable& api = sdk.Api();

    uint32_t num = 0;
    if (api.GetDeviceNum && api.GetDeviceNum(&num) == avermedia::AVER_ERR_SUCCESS)
        std::cout << "[AVerMedia] " << num << " device(s) reported by SDK." << std::endl;

    if (api.CreateEngine(device_index_, &device_handle_) != avermedia::AVER_ERR_SUCCESS
        || !device_handle_)
    {
        std::cerr << "[AVerMedia] CreateEngine(" << device_index_ << ") failed." << std::endl;
        device_handle_ = nullptr;
        return false;
    }

    // 选 HDMI 源(项目接的就是 HDMI 信号)。失败不致命 — 一些设备只有一个源,
    // SetVideoSource 直接 NOT_SUPPORTED。
    if (prefer_hdmi_source_ && api.SetVideoSource)
        api.SetVideoSource(device_handle_, avermedia::AVER_SRC_HDMI);

    // 像素格式优先 RGB32(BGRA)。若设备只支持 NV12，worker 也能正确转换；不能
    // 把 NV12 字节流继续按 BGRA 解释，否则不仅丢帧还可能越界读取。
    bool fmt_set = false;
    if (api.SetVideoPixelFormat)
    {
        if (api.SetVideoPixelFormat(device_handle_, avermedia::AVER_PIX_RGB32)
            == avermedia::AVER_ERR_SUCCESS)
        {
            nv12_format_ = false;
            fmt_set = true;
        }
        else if (api.SetVideoPixelFormat(device_handle_, avermedia::AVER_PIX_NV12)
            == avermedia::AVER_ERR_SUCCESS)
        {
            nv12_format_ = true;
            std::cerr << "[AVerMedia] RGB32 unsupported; using NV12." << std::endl;
            fmt_set = true;
        }
    }
    if (!fmt_set)
        std::cerr << "[AVerMedia] SetVideoPixelFormat unavailable; using device default."
                  << std::endl;

    if (src_width_.load() > 0 && src_height_.load() > 0 && api.SetVideoResolution)
        api.SetVideoResolution(device_handle_,
                               static_cast<uint32_t>(src_width_.load()),
                               static_cast<uint32_t>(src_height_.load()));

    if (capture_fps_ > 0 && api.SetVideoInputFrameRate)
        api.SetVideoInputFrameRate(device_handle_,
                                   static_cast<uint32_t>(capture_fps_ * 100));

    source_frame_count_ = 0;
    source_fps_smoothed_ = 0.0;
    source_fps_start_ = std::chrono::steady_clock::now();

    // 启流。
    const uint32_t rc = api.StartStreaming(device_handle_, &AverMediaCapture::FrameThunk, this);
    if (rc != avermedia::AVER_ERR_SUCCESS)
    {
        std::cerr << "[AVerMedia] StartStreaming failed: rc=" << rc << std::endl;
        api.ReleaseEngine(device_handle_);
        device_handle_ = nullptr;
        return false;
    }
    streaming_ = true;

    // 探一次信号 / 实际协商参数。
    if (api.GetSignalPresence)
    {
        uint32_t present = 0;
        api.GetSignalPresence(device_handle_, &present);
        std::cout << "[AVerMedia] Signal present=" << present << std::endl;
    }
    if (api.GetVideoInfo)
    {
        uint32_t w = 0, h = 0, fps_x100 = 0, interlaced = 0;
        api.GetVideoInfo(device_handle_, &w, &h, &fps_x100, &interlaced);
        if (w > 0 && h > 0)
        {
            src_width_.store(static_cast<int>(w));
            src_height_.store(static_cast<int>(h));
        }
        std::cout << "[AVerMedia] Negotiated: " << w << "x" << h
                  << " @ " << (fps_x100 / 100.0)
                  << " fps (interlaced=" << interlaced << ")" << std::endl;
    }
    std::cout << "[AVerMedia] Streaming started; out=" << out_side_ << "x" << out_side_
              << (crop_enabled_ ? " (center-crop)" : " (scaled)") << std::endl;
    return true;
}

void AverMediaCapture::CloseDevice()
{
    should_stop_.store(true);
    raw_cv_.notify_all();
    if (!device_handle_)
        return;
    auto& sdk = avermedia::SdkLoader::Instance();
    if (sdk.IsLoaded())
    {
        const avermedia::ApiTable& api = sdk.Api();
        if (streaming_ && api.StopStreaming)
            api.StopStreaming(device_handle_);
        streaming_ = false;
        if (api.ReleaseEngine)
            api.ReleaseEngine(device_handle_);
    }
    device_handle_ = nullptr;
    is_open_.store(false);

    // 唤醒任何等在 WaitFrame 上的消费线程,让它们看到 IsOpen=false 退出。
    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        has_frame_ = false;
        has_gpu_frame_ = false;
    }
    frame_cv_.notify_all();
}

void __stdcall AverMediaCapture::FrameThunk(uint8_t* buffer,
                                            uint32_t length,
                                            uint64_t timestamp_100ns,
                                            void* user_ctx)
{
    if (!user_ctx) return;
    static_cast<AverMediaCapture*>(user_ctx)->OnFrame(buffer, length, timestamp_100ns);
}

void AverMediaCapture::OnFrame(uint8_t* buffer, uint32_t length, uint64_t /*ts*/)
{
    if (!buffer || length == 0)
        return;

    const int w = src_width_.load();
    const int h = src_height_.load();
    const uint64_t expected = nv12_format_
        ? static_cast<uint64_t>(w) * h * 3 / 2
        : static_cast<uint64_t>(w) * h * 4;
    if (w <= 0 || h <= 0 || expected != length)
    {
        static bool warned = false;
        if (!warned)
        {
            std::cerr << "[AVerMedia] Unexpected frame size: " << length
                      << " bytes for negotiated " << w << "x" << h
                      << (nv12_format_ ? " NV12." : " BGRA.") << std::endl;
            warned = true;
        }
        return;
    }

    // 这里是 SDK 的实时回调。设备内存会在返回后失效，所以必须复制；但只复制
    // 最终需要的中心 ROI，并把 cvtColor/resize 留给 worker，回调本身尽快返回。
    TickFps();

    bool cropped = false;
    {
        std::lock_guard<std::mutex> lk(raw_mutex_);
        // If a not-yet-consumed latest slot exists, overwrite it in place.
        // Otherwise take a free slot; all slots busy means transform is behind,
        // so drop this callback frame instead of blocking the SDK thread.
        int slotIndex = queued_raw_slot_;
        if (slotIndex < 0)
        {
            for (int i = 0; i < RAW_POOL_SIZE; ++i)
            {
                RawSlot& candidate = raw_pool_[i];
                if (candidate.busy && candidate.gpu_pending && candidate.gpu_done
                    && cudaEventQuery(candidate.gpu_done) == cudaSuccess)
                {
                    candidate.busy = false;
                    candidate.gpu_pending = false;
                }
                if (!candidate.busy) { slotIndex = i; break; }
            }
        }
        if (slotIndex < 0)
            return;

        RawSlot& slot = raw_pool_[slotIndex];
        if (nv12_format_)
        {
            if (gpu_decode_ && crop_enabled_ && w >= out_side_ && h >= out_side_)
            {
                const int left = ((w - out_side_) / 2) & ~1;
                const int top = ((h - out_side_) / 2) & ~1;
                slot.image.create(out_side_ + out_side_ / 2, out_side_, CV_8UC1);
                for (int row = 0; row < out_side_; ++row)
                    std::memcpy(slot.image.data + static_cast<size_t>(row) * slot.image.step,
                                buffer + static_cast<size_t>(top + row) * w + left,
                                static_cast<size_t>(out_side_));
                const uint8_t* srcUv = buffer + static_cast<size_t>(w) * h;
                uint8_t* dstUv = slot.image.data
                    + static_cast<size_t>(out_side_) * slot.image.step;
                for (int row = 0; row < out_side_ / 2; ++row)
                    std::memcpy(dstUv + static_cast<size_t>(row) * slot.image.step,
                                srcUv + static_cast<size_t>(top / 2 + row) * w + left,
                                static_cast<size_t>(out_side_));
                cropped = true;
            }
            else
            {
                cv::Mat src(h + h / 2, w, CV_8UC1, buffer);
                src.copyTo(slot.image);  // create() only during pool warm-up
            }
        }
        else
        {
            cv::Mat bgra(h, w, CV_8UC4, buffer);
            if (crop_enabled_ && w >= out_side_ && h >= out_side_)
            {
                const int left = (w - out_side_) / 2;
                const int top = (h - out_side_) / 2;
                bgra(cv::Rect(left, top, out_side_, out_side_)).copyTo(slot.image);
                cropped = true;
            }
            else
            {
                bgra.copyTo(slot.image);
            }
        }
        slot.cropped = cropped;
        slot.busy = true;
        queued_raw_slot_ = slotIndex;
    }
    raw_cv_.notify_one();
}

void AverMediaCapture::TransformLoop()
{
    while (!should_stop_.load())
    {
        cv::Mat bgra;
        bool cropped = false;
        int slotIndex = -1;
        {
            std::unique_lock<std::mutex> lk(raw_mutex_);
            raw_cv_.wait(lk, [this] { return should_stop_.load() || queued_raw_slot_ >= 0; });
            if (should_stop_.load())
                break;
            slotIndex = queued_raw_slot_;
            queued_raw_slot_ = -1;
            bgra = raw_pool_[slotIndex].image; // shared header; slot stays busy
            cropped = raw_pool_[slotIndex].cropped;
        }

        if (gpu_decode_)
        {
            TransformGpu(bgra, cropped);
            std::lock_guard<std::mutex> lk(raw_mutex_);
            RawSlot& slot = raw_pool_[slotIndex];
            if (slot.gpu_done)
            {
                cudaEventRecord(slot.gpu_done, gpu_stream_);
                slot.gpu_pending = true;
            }
            else
            {
                cudaStreamSynchronize(gpu_stream_);
                slot.busy = false;
            }
            continue;
        }

        cv::Mat out;
        if (nv12_format_)
        {
            cv::Mat full;
            cv::cvtColor(bgra, full, cv::COLOR_YUV2BGR_NV12);
            if (crop_enabled_ && full.cols >= out_side_ && full.rows >= out_side_)
            {
                const int left = (full.cols - out_side_) / 2;
                const int top = (full.rows - out_side_) / 2;
                out = full(cv::Rect(left, top, out_side_, out_side_)).clone();
            }
            else if (full.cols == out_side_ && full.rows == out_side_)
                out = std::move(full);
            else
                cv::resize(full, out, cv::Size(out_side_, out_side_));
        }
        else
        {
            cv::Mat sized;
            if (cropped || (bgra.cols == out_side_ && bgra.rows == out_side_))
                sized = std::move(bgra);
            else
                cv::resize(bgra, sized, cv::Size(out_side_, out_side_));
            cv::cvtColor(sized, out, cv::COLOR_BGRA2BGR);
        }
        {
            std::lock_guard<std::mutex> lk(frame_mutex_);
            latest_ = std::move(out);
            has_frame_ = true;
        }
        {
            std::lock_guard<std::mutex> lk(raw_mutex_);
            raw_pool_[slotIndex].busy = false;
        }
        frame_cv_.notify_one();
    }
}

bool AverMediaCapture::TransformGpu(const cv::Mat& raw, bool cropped)
{
    if (!gpu_stream_ || raw.empty())
        return false;

    const int sourceW = nv12_format_ && cropped ? raw.cols : src_width_.load();
    const int sourceH = nv12_format_ && cropped ? raw.rows * 2 / 3 : src_height_.load();
    int roiW = nv12_format_ ? sourceW : raw.cols;
    int roiH = nv12_format_ ? sourceH : raw.rows;
    int left = 0, top = 0;
    if (nv12_format_ && !cropped && crop_enabled_
        && sourceW >= out_side_ && sourceH >= out_side_)
    {
        roiW = roiH = out_side_;
        left = ((sourceW - out_side_) / 2) & ~1;
        top = ((sourceH - out_side_) / 2) & ~1;
    }

    const bool direct = roiW == out_side_ && roiH == out_side_;
    const size_t poolIndex = gpu_pool_index_;
    gpu_pool_index_ = (gpu_pool_index_ + 1) % GPU_POOL_SIZE;
    GpuImage& dst = direct ? gpu_pool_[poolIndex] : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;

    if (nv12_format_)
    {
        if (!scratch_a_.upload(raw.data + static_cast<size_t>(top) * raw.step + left,
                               roiH, roiW, 1, raw.step, gpu_stream_))
            return false;
        const uint8_t* uv = raw.data + static_cast<size_t>(sourceH) * raw.step;
        if (!scratch_b_.upload(uv + static_cast<size_t>(top / 2) * raw.step + left,
                               roiH / 2, roiW, 1, raw.step, gpu_stream_))
            return false;
        const Npp8u* planes[2] = { scratch_a_.data(), scratch_b_.data() };
        NppiSize roi{ roiW, roiH };
        if (nppiNV12ToBGR_8u_P2C3R_Ctx(
                planes, static_cast<int>(scratch_a_.step()),
                dst.data(), static_cast<int>(dst.step()), roi, npp_ctx_) != NPP_SUCCESS)
            return false;
    }
    else
    {
        if (!scratch_a_.upload(raw.data, raw.rows, raw.cols, 4, raw.step, gpu_stream_))
            return false;
        launch_bgra_to_bgr_u8(scratch_a_.data(), scratch_a_.step(),
                              dst.data(), dst.step(), roiW, roiH, gpu_stream_);
    }

    GpuImage out = dst;
    if (!direct)
    {
        GpuImage& resized = gpu_pool_[poolIndex];
        if (!resized.create(out_side_, out_side_, 3))
            return false;
        launch_resize_bgr_u8_bilinear(dst.data(), dst.step(), roiW, roiH,
                                      resized.data(), resized.step(), out_side_, out_side_,
                                      gpu_stream_);
        out = resized;
    }

    cudaEvent_t ready = gpu_events_[poolIndex];
    if (ready)
    {
        cudaEventRecord(ready, gpu_stream_);
        out.setReadyEvent(ready);
    }
    else
    {
        cudaStreamSynchronize(gpu_stream_);
    }
    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        latest_gpu_ = std::move(out);
        has_gpu_frame_ = true;
    }
    frame_cv_.notify_one();
    return true;
}

void AverMediaCapture::TickFps()
{
    ++source_frame_count_;
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - source_fps_start_;
    if (elapsed.count() >= 0.5)
    {
        const double instant = source_frame_count_ / elapsed.count();
        source_fps_smoothed_ = source_fps_smoothed_ <= 0.0
            ? instant
            : source_fps_smoothed_ * 0.75 + instant * 0.25;
        source_fps_.store(static_cast<int>(std::lround(source_fps_smoothed_)));
        source_frame_count_ = 0;
        source_fps_start_ = now;
    }
}

cv::Mat AverMediaCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lk(frame_mutex_);
    if (!has_frame_ || latest_.empty())
        return cv::Mat();
    has_frame_ = false;
    return std::move(latest_);
}

GpuImage AverMediaCapture::GetNextFrameGpu()
{
    std::lock_guard<std::mutex> lk(frame_mutex_);
    if (!has_gpu_frame_ || latest_gpu_.empty())
        return GpuImage();
    has_gpu_frame_ = false;
    return std::move(latest_gpu_);
}

bool AverMediaCapture::WaitFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(frame_mutex_);
    if (has_frame_ || has_gpu_frame_)
        return true;
    if (timeoutMs <= 0)
        return false;
    frame_cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                       [&]{ return has_frame_ || has_gpu_frame_ || !is_open_.load(); });
    return has_frame_ || has_gpu_frame_;
}
