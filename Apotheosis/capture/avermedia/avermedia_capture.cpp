#include "avermedia_capture.h"
#include "avermedia_sdk.h"

#include <algorithm>
#include <chrono>
#include <iostream>

AverMediaCapture::AverMediaCapture(int src_width,
                                   int src_height,
                                   int out_side,
                                   bool crop_enabled,
                                   int capture_fps,
                                   uint32_t device_index,
                                   bool prefer_hdmi_source)
    : src_width_(std::max(0, src_width))
    , src_height_(std::max(0, src_height))
    , out_side_(std::max(1, out_side))
    , crop_enabled_(crop_enabled)
    , capture_fps_(std::max(0, capture_fps))
    , device_index_(device_index)
    , prefer_hdmi_source_(prefer_hdmi_source)
{
    source_fps_start_ = std::chrono::steady_clock::now();
    if (!OpenDevice())
    {
        std::cerr << "[AVerMedia] Failed to open device #" << device_index_ << std::endl;
        return;
    }
    is_open_.store(true);
    transform_thread_ = std::thread(&AverMediaCapture::TransformLoop, this);
}

AverMediaCapture::~AverMediaCapture()
{
    CloseDevice();
    if (transform_thread_.joinable())
        transform_thread_.join();
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

    // 像素格式优先 RGB32(BGRA),最省得在帧回调里做颜色转换。SDK 不支持就回 NV12 兜底。
    bool fmt_set = false;
    if (api.SetVideoPixelFormat)
    {
        if (api.SetVideoPixelFormat(device_handle_, avermedia::AVER_PIX_RGB32)
            == avermedia::AVER_ERR_SUCCESS)
            fmt_set = true;
        else if (api.SetVideoPixelFormat(device_handle_, avermedia::AVER_PIX_NV12)
            == avermedia::AVER_ERR_SUCCESS)
        {
            std::cerr << "[AVerMedia] RGB32 unsupported, fell back to NV12 — "
                         "TODO: add NV12->BGR conversion in OnFrame()." << std::endl;
            fmt_set = true;
        }
    }
    if (!fmt_set)
        std::cerr << "[AVerMedia] SetVideoPixelFormat unavailable; using device default."
                  << std::endl;

    if (src_width_ > 0 && src_height_ > 0 && api.SetVideoResolution)
        api.SetVideoResolution(device_handle_,
                               static_cast<uint32_t>(src_width_),
                               static_cast<uint32_t>(src_height_));

    if (capture_fps_ > 0 && api.SetVideoInputFrameRate)
        api.SetVideoInputFrameRate(device_handle_,
                                   static_cast<uint32_t>(capture_fps_ * 100));

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

    // 当前路径假设 SDK 已协商到 RGB32 (BGRA8888)。src_width × src_height × 4 = length。
    // 协商不上时,length 与 BGRA 假设对不上 — 直接丢这帧,日志一次。NV12 / YUY2 等
    // 路径后续需要时再补(走 cv::cvtColor 或 gpu_color_ops 即可)。
    const int w = src_width_ > 0 ? src_width_ : 0;
    const int h = src_height_ > 0 ? src_height_ : 0;
    if (w <= 0 || h <= 0 || static_cast<uint32_t>(w * h * 4) != length)
    {
        static bool warned = false;
        if (!warned)
        {
            std::cerr << "[AVerMedia] Unexpected frame size: " << length
                      << " bytes for assumed " << w << "x" << h
                      << " BGRA. Dropping frames until format is renegotiated." << std::endl;
            warned = true;
        }
        return;
    }

    // 这里是 SDK 的实时回调。设备内存会在返回后失效，所以必须复制；但只复制
    // 最终需要的中心 ROI，并把 cvtColor/resize 留给 worker，回调本身尽快返回。
    cv::Mat bgra(h, w, CV_8UC4, buffer);
    TickFps();

    cv::Mat raw;
    bool cropped = false;
    if (crop_enabled_ && w >= out_side_ && h >= out_side_)
    {
        const int left = (w - out_side_) / 2;
        const int top = (h - out_side_) / 2;
        raw = bgra(cv::Rect(left, top, out_side_, out_side_)).clone();
        cropped = true;
    }
    else
    {
        raw = bgra.clone();
    }

    {
        std::lock_guard<std::mutex> lk(raw_mutex_);
        raw_bgra_latest_ = std::move(raw);
        raw_is_cropped_ = cropped;
        has_raw_frame_ = true;
    }
    raw_cv_.notify_one();
}

void AverMediaCapture::TransformLoop()
{
    while (!should_stop_.load())
    {
        cv::Mat bgra;
        bool cropped = false;
        {
            std::unique_lock<std::mutex> lk(raw_mutex_);
            raw_cv_.wait(lk, [this] { return should_stop_.load() || has_raw_frame_; });
            if (should_stop_.load())
                break;
            bgra = std::move(raw_bgra_latest_);
            cropped = raw_is_cropped_;
            has_raw_frame_ = false;
        }

        cv::Mat sized;
        if (cropped || (bgra.cols == out_side_ && bgra.rows == out_side_))
            sized = std::move(bgra);
        else
            cv::resize(bgra, sized, cv::Size(out_side_, out_side_));

        cv::Mat out;
        cv::cvtColor(sized, out, cv::COLOR_BGRA2BGR);
        {
            std::lock_guard<std::mutex> lk(frame_mutex_);
            latest_ = std::move(out);
            has_frame_ = true;
        }
        frame_cv_.notify_one();
    }
}

void AverMediaCapture::TickFps()
{
    ++source_frame_count_;
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - source_fps_start_;
    if (elapsed.count() >= 1.0)
    {
        source_fps_.store(static_cast<int>(source_frame_count_ / elapsed.count()));
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

bool AverMediaCapture::WaitFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(frame_mutex_);
    if (has_frame_)
        return true;
    if (timeoutMs <= 0)
        return false;
    frame_cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                       [&]{ return has_frame_ || !is_open_.load(); });
    return has_frame_;
}
