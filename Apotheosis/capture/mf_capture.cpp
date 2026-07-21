#include "mf_capture.h"

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "gpu_color_ops.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
std::string WideToUtf8(const wchar_t* text)
{
    if (!text || !*text)
        return std::string();
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return std::string();
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    return result;
}

ComPtr<IMFAttributes> CreateVideoDeviceAttributes()
{
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)))
        return nullptr;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    return attrs;
}

const GUID& SubtypeFor(MFCapture::Format f)
{
    switch (f)
    {
    case MFCapture::Format::Nv12:  return MFVideoFormat_NV12;
    case MFCapture::Format::Mjpg:  return MFVideoFormat_MJPG;
    case MFCapture::Format::Yuy2:  return MFVideoFormat_YUY2;
    case MFCapture::Format::Rgb32:
    default:                       return MFVideoFormat_RGB32;
    }
}

bool GetSubType(IMFMediaType* type, GUID* subtype)
{
    return type && subtype && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, subtype));
}

// 跨多个 subtype 一次性枚举设备的所有原生媒体类型,综合打分挑选最优 type,然后
// 直接 SetCurrentMediaType 选中的原生类型(设备一定接受)。
//
// 关键设计:
//   1) 不再像旧版那样锁死单一请求 subtype 才挑候选 —— 圆刚 / AVerMedia 改 EDID 后,
//      用户填 NV12 但设备真正暴露的可能是 YUY2/MJPG/低 fps NV12;旧版协商必败或锁到
//      30fps 的 NV12。这里把 4 种已知 subtype 都拿来一起评分,把"接近请求 fps"放到
//      首要位次,"用户偏好的 subtype"作次序。
//   2) MF_MT_FRAME_RATE 在部分驱动上是 0,真实帧率写在 MF_MT_FRAME_RATE_RANGE_MAX —
//      读不到 FRAME_RATE 时回退到 RANGE_MAX,避免把这类条目当成 0fps 错排。
//   3) 设备没有 size 属性时不强行按面积比对,纯靠 fps + 偏好定阶,让 EDID 怪卡也能开。
//
// preferred: 用户偏好的 subtype 顺序(第 0 个是 UI 里选的格式),后续元素打分次序低
// chosenSubtype: 出参,实际选用的 subtype(可能不是用户偏好的第 0 个)
bool SelectAndApplyBestType(IMFSourceReader* reader,
                             const std::vector<GUID>& preferred,
                             int wantW, int wantH, int wantFps,
                             GUID& chosenSubtype, double& outFps,
                             bool log_enumeration)
{
    outFps = 0.0;
    chosenSubtype = GUID_NULL;
    if (!reader || preferred.empty())
        return false;

    struct TypeInfo
    {
        ComPtr<IMFMediaType> type;
        GUID  sub{};
        int   w = 0;
        int   h = 0;
        double fps = 0.0;
        int   prefIdx = INT_MAX;   // 在 preferred 里的下标(越小越优先)
    };

    auto subtypeShortName = [](const GUID& g) -> const char*
    {
        if (g == MFVideoFormat_NV12)  return "NV12";
        if (g == MFVideoFormat_MJPG)  return "MJPG";
        if (g == MFVideoFormat_YUY2)  return "YUY2";
        if (g == MFVideoFormat_RGB32) return "RGB32";
        return "OTHER";
    };

    std::vector<TypeInfo> all;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr) || !t)
            continue;

        GUID sub{};
        if (!GetSubType(t.Get(), &sub))
            continue;

        int pref = -1;
        for (size_t p = 0; p < preferred.size(); ++p)
            if (preferred[p] == sub) { pref = static_cast<int>(p); break; }
        if (pref < 0)
            continue;   // 不在用户允许的 subtype 集合里,跳过

        TypeInfo info;
        info.type = t;
        info.sub = sub;
        info.prefIdx = pref;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h)))
        {
            info.w = static_cast<int>(w);
            info.h = static_cast<int>(h);
        }
        // 优先取声明的 frame rate;若该属性缺失或分母为 0,回退到 RANGE_MAX。
        // 一些 AVerMedia/EDID 改装驱动只在 RANGE_MAX 里写真实帧率。
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den)) && den != 0 && num != 0)
            info.fps = static_cast<double>(num) / den;
        else if (SUCCEEDED(MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MAX, &num, &den)) && den != 0 && num != 0)
            info.fps = static_cast<double>(num) / den;
        all.push_back(std::move(info));
    }

    if (log_enumeration)
    {
        std::cerr << "[MFCapture] Enumerated " << all.size() << " supported native type(s):" << std::endl;
        for (const auto& t : all)
            std::cerr << "  - " << subtypeShortName(t.sub)
                      << " " << t.w << "x" << t.h
                      << " @ " << t.fps << "fps (prefIdx=" << t.prefIdx << ")" << std::endl;
    }

    if (all.empty())
        return false;

    // 评分:先按"分辨率组"过滤一遍,再综合 fps 接近度 + subtype 偏好排序。
    //   resScore:分辨率精确匹配 = 0;否则取与请求面积的差值(未指定分辨率时 = 0,不影响)。
    //   fpsScore:|fps - wantFps|;wantFps<=0 时取 -fps(等价"越高越优")。
    //   prefScore:preferred 下标,小者优先。
    // 三者按 (resScore, fpsScore, prefScore) 字典序比较。这样:
    //   * 圆刚 EDID 改装 + 用户选 NV12:NV12 不存在但 YUY2/MJPG 存在 → 自动落到后者
    //   * GC553G2 + 用户选 NV12 + wantFps=240:NV12@30 vs MJPG@120 → MJPG@120 胜出
    //     (因为 fps 差更小);若用户保留 NV12 偏好,但 fps 差异更大,以 fps 为主。
    auto resScore = [&](const TypeInfo& t) -> double
    {
        if (wantW <= 0 || wantH <= 0) return 0.0;
        if (t.w == wantW && t.h == wantH) return 0.0;
        return std::abs(static_cast<double>(t.w) * t.h
                        - static_cast<double>(wantW) * wantH);
    };
    auto fpsScore = [&](const TypeInfo& t) -> double
    {
        if (wantFps <= 0) return -t.fps;   // 取最高 fps
        return std::abs(t.fps - static_cast<double>(wantFps));
    };

    const TypeInfo* chosen = nullptr;
    for (const auto& t : all)
    {
        if (!chosen)
        {
            chosen = &t;
            continue;
        }
        const double r1 = resScore(*chosen), r2 = resScore(t);
        if (r2 < r1) { chosen = &t; continue; }
        if (r2 > r1) continue;
        const double f1 = fpsScore(*chosen), f2 = fpsScore(t);
        if (f2 < f1) { chosen = &t; continue; }
        if (f2 > f1) continue;
        if (t.prefIdx < chosen->prefIdx) chosen = &t;
    }
    if (!chosen)
        return false;

    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, chosen->type.Get())))
        return false;

    chosenSubtype = chosen->sub;
    outFps = chosen->fps;
    return true;
}

bool QueryCurrentFrameGeometry(IMFSourceReader* reader, int& width, int& height, int& stride)
{
    width = height = stride = 0;
    if (!reader)
        return false;

    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) || !current)
        return false;

    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
        return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);

    UINT32 declaredStride = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &declaredStride)) && declaredStride > 0)
        stride = static_cast<int>(declaredStride);
    else
        stride = width;
    return true;
}
} // namespace

MFCapture::Format MFCapture::ParseFormat(const std::string& s)
{
    if (s == "MJPG")  return Format::Mjpg;
    if (s == "YUY2" || s == "YUYV") return Format::Yuy2;
    if (s == "RGB32" || s == "RGBA" || s == "BGRA") return Format::Rgb32;
    return Format::Nv12;
}

const char* MFCapture::FormatLabel(Format f)
{
    switch (f)
    {
    case Format::Mjpg:  return "MJPG";
    case Format::Yuy2:  return "YUY2";
    case Format::Rgb32: return "RGB32";
    case Format::Nv12:
    default:            return "NV12";
    }
}

MFCapture::MFCapture(int src_width,
                     int src_height,
                     int out_side,
                     bool crop_enabled,
                     int capture_fps,
                     const std::string& format,
                     int device_index,
                     bool gpu_decode)
    : src_width_(std::max(0, src_width))
    , src_height_(std::max(0, src_height))
    , out_side_(std::max(1, out_side))
    , crop_enabled_(crop_enabled)
    , capture_fps_(std::max(0, capture_fps))
    , format_(ParseFormat(format))
    , device_index_(std::max(0, device_index))
    , gpu_decode_(gpu_decode)
{
    source_fps_start_ = std::chrono::steady_clock::now();
    target_fps_.store(capture_fps_);   // 初始采集帧率上限(0 = 不限速)
    receive_thread_ = std::thread(&MFCapture::ReceiveThread, this);
}

MFCapture::~MFCapture()
{
    should_stop_.store(true);
    if (receive_thread_.joinable())
        receive_thread_.join();

    // The MJPG path doesn't sync per frame, so the last frame's async work may
    // still be on the stream. Drain before freeing buffers / events / decoder.
    if (gpu_stream_)
        cudaStreamSynchronize(gpu_stream_);

    if (pinned_jpeg_buffer_)
    {
        cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;
    }
    for (auto& e : out_events_)
    {
        if (e)
            cudaEventDestroy(e);
        e = nullptr;
    }
    if (gpu_stream_)
    {
        cudaStreamDestroy(gpu_stream_);
        gpu_stream_ = nullptr;
    }
}

std::vector<MFDeviceInfo> MFCapture::EnumerateDevices()
{
    std::vector<MFDeviceInfo> devices;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        ComPtr<IMFAttributes> attrs = CreateVideoDeviceAttributes();
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (attrs && SUCCEEDED(MFEnumDeviceSources(attrs.Get(), &activates, &count)))
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                wchar_t* name = nullptr;
                UINT32 nameLen = 0;
                std::string label;
                if (SUCCEEDED(activates[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
                {
                    label = WideToUtf8(name);
                    CoTaskMemFree(name);
                }

                MFDeviceInfo info;
                info.index = static_cast<int>(i);
                std::ostringstream display;
                display << u8"设备 #" << i;
                if (!label.empty())
                    display << " - " << label;
                info.name = display.str();
                devices.push_back(std::move(info));
                activates[i]->Release();
            }
            CoTaskMemFree(activates);
        }
        MFShutdown();
    }
    if (shouldUninit)
        CoUninitialize();
    return devices;
}

cv::Mat MFCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (cpu_frame_queue_.empty())
        return cv::Mat();
    cv::Mat frame = std::move(cpu_frame_queue_.front());
    cpu_frame_queue_.pop();
    return frame;
}

GpuImage MFCapture::GetNextFrameGpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (gpu_frame_queue_.empty())
        return GpuImage();
    GpuImage frame = std::move(gpu_frame_queue_.front());
    gpu_frame_queue_.pop();
    return frame;
}

bool MFCapture::EnsureGpuContext()
{
    if (gpu_stream_)
        return true;
    if (cudaStreamCreateWithFlags(&gpu_stream_, cudaStreamNonBlocking) != cudaSuccess)
    {
        std::cerr << "[MFCapture] CUDA stream creation failed." << std::endl;
        return false;
    }

    int dev = 0;
    cudaGetDevice(&dev);
    cudaDeviceProp props{};
    cudaGetDeviceProperties(&props, dev);
    npp_ctx_.hStream                            = gpu_stream_;
    npp_ctx_.nCudaDeviceId                      = dev;
    npp_ctx_.nMultiProcessorCount               = props.multiProcessorCount;
    npp_ctx_.nMaxThreadsPerMultiProcessor       = props.maxThreadsPerMultiProcessor;
    npp_ctx_.nMaxThreadsPerBlock                = props.maxThreadsPerBlock;
    npp_ctx_.nSharedMemPerBlock                 = props.sharedMemPerBlock;
    npp_ctx_.nCudaDevAttrComputeCapabilityMajor = props.major;
    npp_ctx_.nCudaDevAttrComputeCapabilityMinor = props.minor;
    cudaStreamGetFlags(gpu_stream_, &npp_ctx_.nStreamFlags);

    // One timing-free event per out slot. The MJPG path records it (no per-frame
    // sync) and hands it to the consumer; failure leaves a null event and that
    // path falls back to a CPU sync, so correctness never depends on these.
    for (auto& e : out_events_)
        cudaEventCreateWithFlags(&e, cudaEventDisableTiming);
    return true;
}

void MFCapture::ReceiveThread()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        std::cerr << "[MFCapture] MFStartup failed." << std::endl;
        if (shouldUninit)
            CoUninitialize();
        return;
    }

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    double negotiatedFps = 0.0;  // declared before any goto so cleanup stays in scope

    ComPtr<IMFAttributes> deviceAttrs = CreateVideoDeviceAttributes();
    if (!deviceAttrs || FAILED(MFEnumDeviceSources(deviceAttrs.Get(), &activates, &count)) || count == 0)
    {
        std::cerr << "[MFCapture] No video capture devices found." << std::endl;
        goto cleanup;
    }

    if (device_index_ >= static_cast<int>(count))
        device_index_ = 0;

    if (FAILED(activates[device_index_]->ActivateObject(IID_PPV_ARGS(&source))))
    {
        std::cerr << "[MFCapture] Failed to activate capture device." << std::endl;
        goto cleanup;
    }

    {
        // 构造偏好顺序:UI 选的格式排第 0,其余 3 种依次跟在后面作为兜底。这样
        // 圆刚 / EDID 改装等卡上,即便用户填的 subtype 在驱动里被砍掉,也能落到
        // 设备真实暴露的格式;同时若用户偏好的 subtype 帧率远低于请求(GC553G2
        // 上的 NV12@30 vs MJPG@120),按 fps 接近度优先而非锁死偏好。
        auto buildPreferred = [&]() -> std::vector<GUID>
        {
            std::vector<GUID> p;
            p.reserve(4);
            p.push_back(SubtypeFor(format_));
            const Format chain[] = { Format::Mjpg, Format::Nv12, Format::Yuy2, Format::Rgb32 };
            for (Format f : chain)
            {
                const GUID& g = SubtypeFor(f);
                bool dup = false;
                for (const auto& q : p) if (q == g) { dup = true; break; }
                if (!dup) p.push_back(g);
            }
            return p;
        };

        // 第一次尝试:禁用 MF 内置 converter,要求设备直送原生帧格式。这是稳态路径,
        // 帧率/带宽不会被 source reader 在 CPU 上偷偷转一手。
        auto createReader = [&](bool disable_converters) -> bool
        {
            reader.Reset();
            ComPtr<IMFAttributes> readerAttrs;
            MFCreateAttributes(&readerAttrs, 2);
            if (readerAttrs)
            {
                readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, disable_converters ? TRUE : FALSE);
                readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, disable_converters ? FALSE : TRUE);
            }
            return SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader));
        };

        if (!createReader(true))
        {
            std::cerr << "[MFCapture] Failed to create source reader." << std::endl;
            goto cleanup;
        }

        const std::vector<GUID> preferred = buildPreferred();
        GUID chosenSubtype = GUID_NULL;
        bool ok = SelectAndApplyBestType(reader.Get(), preferred, src_width_, src_height_,
                                          capture_fps_, chosenSubtype, negotiatedFps, /*log_enumeration=*/false);

        // 严格路径失败:某些 EDID 改装驱动只在启用 converter 的 reader 上才把
        // 真实 type 列出来。重建 reader 再试一次,仍限制在 4 种已知 subtype 内,
        // 不会让 MF 把帧格式悄悄换成不认识的东西。
        if (!ok)
        {
            std::cerr << "[MFCapture] Strict negotiation failed; retrying with MF converters enabled." << std::endl;
            if (!createReader(false))
            {
                std::cerr << "[MFCapture] Failed to recreate source reader for fallback." << std::endl;
                goto cleanup;
            }
            ok = SelectAndApplyBestType(reader.Get(), preferred, src_width_, src_height_,
                                         capture_fps_, chosenSubtype, negotiatedFps, /*log_enumeration=*/true);
        }

        if (!ok || !QueryCurrentFrameGeometry(reader.Get(), frame_width_, frame_height_, frame_stride_))
        {
            std::cerr << "[MFCapture] Device does not expose any of the requested formats "
                         "(NV12/MJPG/YUY2/RGB32). See enumeration above." << std::endl;
            // 上面 log_enumeration=true 的那次失败已经打印了实际枚举,这里再补一行总结。
            goto cleanup;
        }

        // 把"实际选用的格式"回写到 format_,这样后续 switch(format_) 走到正确的解码路径。
        // 用户填的是 NV12 但实际拿到 MJPG 的情况下,这一步至关重要,否则会用 NV12 解码器
        // 去喂 JPEG 字节流。
        const Format previous = format_;
        if      (chosenSubtype == MFVideoFormat_MJPG)  format_ = Format::Mjpg;
        else if (chosenSubtype == MFVideoFormat_NV12)  format_ = Format::Nv12;
        else if (chosenSubtype == MFVideoFormat_YUY2)  format_ = Format::Yuy2;
        else if (chosenSubtype == MFVideoFormat_RGB32) format_ = Format::Rgb32;
        if (format_ != previous)
        {
            std::cerr << "[MFCapture] Requested " << FormatLabel(previous)
                      << " unavailable / suboptimal; using " << FormatLabel(format_) << " instead." << std::endl;
        }
    }

    if (gpu_decode_)
    {
        if (!EnsureGpuContext())
            goto cleanup;
        // Allocate steady-state raw-path storage before streaming. cudaMalloc
        // may synchronize the device, so letting it occur while 240Hz samples
        // are arriving creates visible frame-time spikes.
        for (auto& slot : out_pool_)
            if (!slot.create(out_side_, out_side_, 3))
                goto cleanup;
        if (format_ != Format::Mjpg)
        {
            int left = 0, top = 0, roiW = 0, roiH = 0;
            ResolveRoi(frame_width_, frame_height_, left, top, roiW, roiH);
            const int sourceChannels = format_ == Format::Rgb32 ? 4
                                     : format_ == Format::Yuy2 ? 2 : 1;
            if (!scratch_a_.create(roiH, roiW, sourceChannels)
                || (format_ == Format::Nv12
                    && !scratch_b_.create(roiH / 2, roiW, 1))
                || !scratch_full_.create(roiH, roiW, 3))
                goto cleanup;
        }
        if (format_ == Format::Mjpg)
        {
            gpu_decoder_ = std::make_unique<capture::GpuJpegDecoder>();
            if (!gpu_decoder_->init())
            {
                std::cerr << "[MFCapture] nvJPEG unavailable; MJPG falls back to CPU decode." << std::endl;
                gpu_decoder_.reset();
                mjpg_cpu_fallback_ = true;
            }
        }
    }

    std::cout << "[MFCapture] " << FormatLabel(format_)
              << " @ " << frame_width_ << "x" << frame_height_
              << "@" << negotiatedFps << "fps"
              << " stride=" << frame_stride_
              << " -> " << out_side_ << "x" << out_side_
              << (crop_enabled_ ? " (center-crop)" : " (scaled)")
              << " [" << (gpu_decode_ ? "GPU" : "CPU") << "]" << std::endl;
    negotiated_fps_.store(static_cast<int>(std::lround(negotiatedFps)));

    source_frame_count_ = 0;
    source_fps_smoothed_ = 0.0;
    source_fps_start_ = std::chrono::steady_clock::now();
    is_open_.store(true);
    StartDecodeWorkers();   // 仅 crop_enabled 的 MJPG-GPU 模式内部生效
    StartProcessWorker();
    while (!should_stop_.load())
    {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
            continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0, currentLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &currentLen)))
            continue;

        if (data && currentLen > 0)
        {
            // Count device delivery immediately after ReadSample/Lock.  Decode,
            // color conversion and queue pressure must not masquerade as a
            // lower camera/source frame rate in diagnostics.
            TickFps();
            // Apply the requested processing cap once, before every CPU/GPU
            // decode path.  The device is still drained at full speed and the
            // source counter above remains truthful, but frames above the cap
            // consume no conversion/decode work.
            if (ShouldDispatchFrame())
            {
                if (format_ == Format::Mjpg && gpu_decode_
                    && !mjpg_cpu_fallback_ && crop_enabled_)
                    EnqueueJpegJob(data, static_cast<size_t>(currentLen));
                else
                    EnqueueProcessJob(data, static_cast<size_t>(currentLen));
            }
        }

        buffer->Unlock();
    }

cleanup:
    StopProcessWorker();
    StopDecodeWorkers();    // 幂等:未启动时直接返回
    is_open_.store(false);
    if (activates)
    {
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
    }
    if (source)
        source->Shutdown();
    MFShutdown();
    if (shouldUninit)
        CoUninitialize();
}

void MFCapture::StartProcessWorker()
{
    process_stop_.store(false);
    process_thread_ = std::thread(&MFCapture::ProcessWorkerLoop, this);
}

void MFCapture::StopProcessWorker()
{
    process_stop_.store(true);
    process_cv_.notify_all();
    if (process_thread_.joinable())
        process_thread_.join();
    std::lock_guard<std::mutex> lock(process_mutex_);
    std::queue<ProcessJob> empty;
    process_queue_.swap(empty);
    process_free_buffers_.clear();
}

void MFCapture::EnqueueProcessJob(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return;
    ProcessJob job;
    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        if (!process_free_buffers_.empty())
        {
            job.bytes = std::move(process_free_buffers_.back());
            process_free_buffers_.pop_back();
        }
    }

    job.width = frame_width_;
    job.height = frame_height_;
    job.stride = frame_stride_;
    bool copied = false;
    if (format_ != Format::Mjpg && frame_width_ > 0 && frame_height_ > 0)
    {
        int left = 0, top = 0, roiW = 0, roiH = 0;
        ResolveRoi(frame_width_, frame_height_, left, top, roiW, roiH);
        const int srcStride = frame_stride_ > 0 ? frame_stride_
            : frame_width_ * (format_ == Format::Rgb32 ? 4
                            : format_ == Format::Yuy2 ? 2 : 1);
        const int channels = format_ == Format::Rgb32 ? 4
                           : format_ == Format::Yuy2 ? 2 : 1;
        const size_t rowBytes = static_cast<size_t>(roiW) * channels;
        const size_t yEnd = static_cast<size_t>(top + roiH - 1) * srcStride
                          + static_cast<size_t>(left) * channels + rowBytes;

        if (roiW > 0 && roiH > 0 && yEnd <= size)
        {
            size_t total = rowBytes * roiH;
            if (format_ == Format::Nv12)
                total += static_cast<size_t>(roiW) * (roiH / 2);
            job.bytes.resize(total);
            for (int row = 0; row < roiH; ++row)
                std::memcpy(job.bytes.data() + static_cast<size_t>(row) * rowBytes,
                            data + static_cast<size_t>(top + row) * srcStride
                                 + static_cast<size_t>(left) * channels,
                            rowBytes);

            if (format_ == Format::Nv12)
            {
                const size_t uvBase = static_cast<size_t>(srcStride) * frame_height_;
                if (roiH >= 2)
                {
                    const size_t uvEnd = uvBase
                        + static_cast<size_t>(top / 2 + roiH / 2 - 1) * srcStride
                        + left + roiW;
                    if (uvEnd <= size)
                    {
                        uint8_t* dstUv = job.bytes.data() + rowBytes * roiH;
                        for (int row = 0; row < roiH / 2; ++row)
                            std::memcpy(dstUv + static_cast<size_t>(row) * roiW,
                                        data + uvBase
                                             + static_cast<size_t>(top / 2 + row) * srcStride + left,
                                        static_cast<size_t>(roiW));
                        copied = true;
                    }
                }
            }
            else
            {
                copied = true;
            }

            if (copied)
            {
                job.width = roiW;
                job.height = roiH;
                job.stride = static_cast<int>(rowBytes);
            }
        }
    }
    if (!copied)
    {
        job.bytes.resize(size);
        std::memcpy(job.bytes.data(), data, size);
    }

    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        while (static_cast<int>(process_queue_.size()) >= MAX_PROCESS_QUEUE)
        {
            ProcessJob stale = std::move(process_queue_.front());
            process_queue_.pop();
            if (process_free_buffers_.size() < MAX_PROCESS_QUEUE + 8)
                process_free_buffers_.push_back(std::move(stale.bytes));
        }
        process_queue_.push(std::move(job));
    }
    process_cv_.notify_one();
}

void MFCapture::ProcessWorkerLoop()
{
    // Keep several source byte buffers alive until their asynchronous H2D copy
    // has passed on gpu_stream_. Waiting only when a ring slot is reused gives
    // CUDA multiple frames of runway without risking use-after-free of pageable
    // IMFSample copies.
    static constexpr size_t HOST_RING = 8;
    std::array<ProcessJob, HOST_RING> hostRing;
    std::array<cudaEvent_t, HOST_RING> hostDone{};
    std::array<bool, HOST_RING> hostPending{};
    size_t hostIdx = 0;
    const bool rawGpu = gpu_decode_ && format_ != Format::Mjpg;
    if (rawGpu)
        for (auto& event : hostDone)
            cudaEventCreateWithFlags(&event, cudaEventDisableTiming);

    for (;;)
    {
        ProcessJob job;
        {
            std::unique_lock<std::mutex> lock(process_mutex_);
            process_cv_.wait(lock, [this] {
                return process_stop_.load() || !process_queue_.empty();
            });
            if (process_stop_.load())
                break;
            job = std::move(process_queue_.front());
            process_queue_.pop();
        }

        const uint8_t* data = job.bytes.data();
        size_t rawSlot = 0;
        if (rawGpu)
        {
            rawSlot = hostIdx;
            hostIdx = (hostIdx + 1) % HOST_RING;
            if (hostPending[rawSlot] && hostDone[rawSlot])
                cudaEventSynchronize(hostDone[rawSlot]);
            if (!hostRing[rawSlot].bytes.empty())
            {
                std::lock_guard<std::mutex> lock(process_mutex_);
                if (process_free_buffers_.size() < MAX_PROCESS_QUEUE + HOST_RING)
                    process_free_buffers_.push_back(std::move(hostRing[rawSlot].bytes));
            }
            hostRing[rawSlot] = std::move(job);
            data = hostRing[rawSlot].bytes.data();
        }
        const ProcessJob& activeJob = rawGpu ? hostRing[rawSlot] : job;
        switch (format_)
        {
        case Format::Nv12:
            gpu_decode_ ? PushNv12Gpu(data, activeJob.width, activeJob.height, activeJob.stride)
                        : PushNv12Cpu(data, activeJob.width, activeJob.height, activeJob.stride);
            break;
        case Format::Yuy2:
            gpu_decode_ ? PushYuy2Gpu(data, activeJob.width, activeJob.height, activeJob.stride)
                        : PushYuy2Cpu(data, activeJob.width, activeJob.height, activeJob.stride);
            break;
        case Format::Rgb32:
            gpu_decode_ ? PushRgb32Gpu(data, activeJob.width, activeJob.height, activeJob.stride)
                        : PushRgb32Cpu(data, activeJob.width, activeJob.height, activeJob.stride);
            break;
        case Format::Mjpg:
            if (gpu_decode_ && !mjpg_cpu_fallback_)
                PushMjpgGpu(data, job.bytes.size());
            else
                PushMjpgCpu(data, job.bytes.size());
            break;
        }

        if (rawGpu)
        {
            if (hostDone[rawSlot])
            {
                cudaEventRecord(hostDone[rawSlot], gpu_stream_);
                hostPending[rawSlot] = true;
            }
            else
            {
                // Event creation failure is rare; preserve correctness even if
                // this degraded path loses overlap.
                cudaStreamSynchronize(gpu_stream_);
                hostRing[rawSlot].bytes.clear();
            }
        }
        else if (!job.bytes.empty())
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            if (process_free_buffers_.size() < MAX_PROCESS_QUEUE + HOST_RING)
                process_free_buffers_.push_back(std::move(job.bytes));
        }
    }

    // ProcessJob owns the host bytes used by asynchronous H2D copies. Drain the
    // stream before the last local job is destroyed during shutdown.
    if (gpu_stream_)
        cudaStreamSynchronize(gpu_stream_);
    for (auto& event : hostDone)
        if (event) cudaEventDestroy(event);
}

void MFCapture::TickFps()
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

void MFCapture::EnqueueCpu(cv::Mat&& frame)
{
    if (frame.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        while (static_cast<int>(cpu_frame_queue_.size()) >= MAX_QUEUE_SIZE)
            cpu_frame_queue_.pop();
        cpu_frame_queue_.push(std::move(frame));
    }
    frame_cv_.notify_one();
}

void MFCapture::EnqueueGpu(GpuImage&& frame)
{
    if (frame.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        while (static_cast<int>(gpu_frame_queue_.size()) >= MAX_QUEUE_SIZE)
            gpu_frame_queue_.pop();
        gpu_frame_queue_.push(std::move(frame));
    }
    frame_cv_.notify_one();
}

// 消费线程在队列取空时调用:阻塞到下一帧入队(被 Enqueue* 的 notify 唤醒)或
// 超时。这样消费精确贴着产帧节奏走,而不是睡满一个固定节拍——后者会和产帧时钟
// 相位漂移而踏空,在队列容量 1 下丢帧。返回 true 表示队列里已有帧,可立即重试。
bool MFCapture::WaitFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(frame_mutex_);
    return frame_cv_.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [this] { return !gpu_frame_queue_.empty() || !cpu_frame_queue_.empty(); });
}

void MFCapture::StartDecodeWorkers()
{
    // 仅 crop_enabled 的 MJPG-GPU 模式才用并行 ROI 解码;其余路径保持 ReceiveThread
    // 直接 Push(单线程,不受影响)。
    if (!(format_ == Format::Mjpg && gpu_decode_ && crop_enabled_))
        return;
    workers_stop_.store(false);
    enqueued_seq_.store(0);
    job_seq_ = 0;
    decode_workers_.clear();
    for (int i = 0; i < DECODE_WORKERS; ++i)
        decode_workers_.emplace_back(&MFCapture::DecodeWorkerLoop, this);
}

void MFCapture::StopDecodeWorkers()
{
    if (decode_workers_.empty())
        return;
    workers_stop_.store(true);
    job_cv_.notify_all();
    for (auto& t : decode_workers_)
        if (t.joinable())
            t.join();
    decode_workers_.clear();
    std::lock_guard<std::mutex> lock(job_mutex_);
    std::queue<DecodeJob> empty;
    job_queue_.swap(empty);
}

void MFCapture::EnqueueJpegJob(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return;
    DecodeJob job;
    job.jpeg.assign(data, data + size);
    job.seq = ++job_seq_;   // ReceiveThread 单线程,普通递增即可
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        while (static_cast<int>(job_queue_.size()) >= MAX_JOB_QUEUE)
            job_queue_.pop();   // 丢最旧、保最新
        job_queue_.push(std::move(job));
    }
    job_cv_.notify_one();
}

void MFCapture::SetTargetFps(int fps)
{
    target_fps_.store(std::max(0, fps));
}

// 按 target_fps_ 节流:命中目标时刻返回 true(放行去解码),否则 false(丢弃,不解码)。
// 用"下一个目标时刻"递进而非"距上次",让放行节奏贴近目标帧率;落后过多则重锚定,
// 避免卡顿/暂停后 burst 补帧。仅 ReceiveThread 单线程调用,next_dispatch_ 无需加锁。
bool MFCapture::ShouldDispatchFrame()
{
    const int fps = target_fps_.load();
    if (fps <= 0)
        return true;   // 0 = 不限速,全部放行
    const int sourceFps = negotiated_fps_.load();
    const int tolerance = std::max(2, fps / 100);
    if (sourceFps > 0 && sourceFps <= fps + tolerance)
        return true;   // 240Hz 源请求 240 时绝不能再用第二个 240Hz 时钟抽帧
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(1000.0 / fps));
    if (next_dispatch_.time_since_epoch().count() == 0)
    {
        next_dispatch_ = now + interval;
        return true;
    }
    if (now >= next_dispatch_)
    {
        next_dispatch_ += interval;
        if (next_dispatch_ < now)
            next_dispatch_ = now + interval;   // 落后过多(卡顿/暂停),重锚定
        return true;
    }
    return false;
}

void MFCapture::DecodeWorkerLoop()
{
    // 每个 worker 自带独立 nvjpeg 解码器、CUDA 流、输出环 + 完成事件:nvjpegJpegState
    // 非线程安全必须 per-thread;输出环让消费者持有某帧时 create() 仍能复用别的 slot
    // 的设备缓冲(GpuImage 引用计数)。
    capture::GpuJpegDecoder decoder;
    if (!decoder.init())
    {
        std::cerr << "[MFCapture] decode worker: GpuJpegDecoder init 失败" << std::endl;
        return;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return;

    constexpr int RING = 3;
    std::array<GpuImage, RING> outRing;
    std::array<cudaEvent_t, RING> evRing{};
    for (auto& e : evRing)
        cudaEventCreateWithFlags(&e, cudaEventDisableTiming);
    size_t ringIdx = 0;

    for (;;)
    {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lock(job_mutex_);
            job_cv_.wait(lock, [this] { return !job_queue_.empty() || workers_stop_.load(); });
            if (workers_stop_.load())
                break;   // 停止:残留 job 由 StopDecodeWorkers 清空,不再处理
            job = std::move(job_queue_.front());
            job_queue_.pop();
        }

        const size_t slot = ringIdx;
        ringIdx = (ringIdx + 1) % RING;
        GpuImage& dst = outRing[slot];
        // ROI 中心 out_side_ 解码(与原单线程路径一致,FOV 不变)。
        if (!decoder.decodeCropped(job.jpeg.data(), job.jpeg.size(), out_side_, out_side_, dst, stream))
            continue;   // ROI 解码失败(罕见),丢弃该帧

        cudaEvent_t e = evRing[slot];
        cudaEventRecord(e, stream);
        GpuImage out = dst;          // 引用计数共享;slot 保留以便消费者释放后复用
        out.setReadyEvent(e);
        EnqueueGpuOrdered(std::move(out), job.seq);
    }

    cudaStreamSynchronize(stream);
    for (auto& e : evRing)
        if (e) cudaEventDestroy(e);
    cudaStreamDestroy(stream);
}

bool MFCapture::EnqueueGpuOrdered(GpuImage&& frame, uint64_t seq)
{
    if (frame.empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        // 序号守卫:乱序到达的旧帧直接丢弃,避免覆盖队列里更新的帧。
        if (seq <= enqueued_seq_.load(std::memory_order_relaxed))
            return false;
        enqueued_seq_.store(seq, std::memory_order_relaxed);
        while (static_cast<int>(gpu_frame_queue_.size()) >= MAX_QUEUE_SIZE)
            gpu_frame_queue_.pop();
        gpu_frame_queue_.push(std::move(frame));

    }
    frame_cv_.notify_one();
    return true;
}

void MFCapture::ResolveRoi(int width, int height, int& left, int& top, int& roiW, int& roiH) const
{
    if (crop_enabled_ && width >= out_side_ && height >= out_side_)
    {
        roiW = roiH = out_side_;
        left = ((width - out_side_) / 2) & ~1;   // even origin keeps NV12/YUY2 chroma aligned
        top  = ((height - out_side_) / 2) & ~1;
    }
    else
    {
        left = 0;
        top = 0;
        roiW = width;
        roiH = height;
    }
}

GpuImage& MFCapture::nextOutSlot()
{
    current_out_idx_ = out_pool_idx_;
    GpuImage& slot = out_pool_[out_pool_idx_];
    out_pool_idx_ = (out_pool_idx_ + 1) % OUT_POOL_SIZE;
    return slot;
}

GpuImage MFCapture::ResizeToOut(const GpuImage& src)
{
    if (src.empty())
        return GpuImage();
    GpuImage& slot = nextOutSlot();
    if (!slot.create(out_side_, out_side_, 3))
        return GpuImage();
    // Same-size bilinear (pixel-center sampling) is an identity copy, so this
    // doubles as the "copy a scratch/view into an owned ring slot" primitive.
    launch_resize_bgr_u8_bilinear(src.data(), src.step(), src.cols(), src.rows(),
                                  slot.data(), slot.step(), out_side_, out_side_, gpu_stream_);
    return slot;
}

cv::Mat MFCapture::FinalizeCpu(cv::Mat bgr)
{
    if (bgr.empty())
        return cv::Mat();
    if (bgr.cols == out_side_ && bgr.rows == out_side_)
        return std::move(bgr);

    if (crop_enabled_ && bgr.cols >= out_side_ && bgr.rows >= out_side_)
    {
        const int left = (bgr.cols - out_side_) / 2;
        const int top = (bgr.rows - out_side_) / 2;
        return bgr(cv::Rect(left, top, out_side_, out_side_)).clone();
    }

    cv::Mat out;
    cv::resize(bgr, out, cv::Size(out_side_, out_side_));
    return out;
}

// ---------------- GPU decode paths ----------------
// Raw formats: upload ONLY the centered ROI (crop-before-upload) so the per-
// frame PCIe transfer is bounded by out_side — critical on a Gen1-capped card.
// NPP converts the ROI to BGR straight into an output slot. Completion events
// let conversion overlap the next device read without exposing partial frames.

bool MFCapture::PushNv12Gpu(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width;

    int left, top, roiW, roiH;
    ResolveRoi(width, height, left, top, roiW, roiH);

    // Upload only the ROI of the Y and UV planes.
    if (!scratch_a_.upload(data + static_cast<size_t>(top) * stride + left,
                           roiH, roiW, 1, stride, gpu_stream_))
        return false;
    const uint8_t* uvBase = data + static_cast<size_t>(stride) * height;
    if (!scratch_b_.upload(uvBase + static_cast<size_t>(top / 2) * stride + left,
                           roiH / 2, roiW, 1, stride, gpu_stream_))
        return false;

    const Npp8u* pSrc[2] = { scratch_a_.data(), scratch_b_.data() };  // both step == roiW
    NppiSize roi = { roiW, roiH };
    const bool direct = crop_enabled_ && roiW == out_side_ && roiH == out_side_;
    GpuImage& dst = direct ? nextOutSlot() : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;
    if (nppiNV12ToBGR_8u_P2C3R_Ctx(pSrc, static_cast<int>(scratch_a_.step()),
                                   dst.data(), static_cast<int>(dst.step()),
                                   roi, npp_ctx_) != NPP_SUCCESS)
        return false;

    GpuImage out = direct ? dst : ResizeToOut(scratch_full_);
    if (out.empty())
        return false;
    cudaEvent_t e = out_events_[current_out_idx_];
    if (e) { cudaEventRecord(e, gpu_stream_); out.setReadyEvent(e); }
    else cudaStreamSynchronize(gpu_stream_);
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushYuy2Gpu(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 2;

    int left, top, roiW, roiH;
    ResolveRoi(width, height, left, top, roiW, roiH);

    if (!scratch_a_.upload(data + static_cast<size_t>(top) * stride + static_cast<size_t>(left) * 2,
                           roiH, roiW, 2, stride, gpu_stream_))
        return false;

    NppiSize roi = { roiW, roiH };
    const bool direct = crop_enabled_ && roiW == out_side_ && roiH == out_side_;
    GpuImage& dst = direct ? nextOutSlot() : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;
    if (nppiYCbCr422ToBGR_8u_C2C3R_Ctx(scratch_a_.data(), static_cast<int>(scratch_a_.step()),
                                       dst.data(), static_cast<int>(dst.step()),
                                       roi, npp_ctx_) != NPP_SUCCESS)
        return false;

    GpuImage out = direct ? dst : ResizeToOut(scratch_full_);
    if (out.empty())
        return false;
    cudaEvent_t e = out_events_[current_out_idx_];
    if (e) { cudaEventRecord(e, gpu_stream_); out.setReadyEvent(e); }
    else cudaStreamSynchronize(gpu_stream_);
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushRgb32Gpu(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 4;

    int left, top, roiW, roiH;
    ResolveRoi(width, height, left, top, roiW, roiH);

    if (!scratch_a_.upload(data + static_cast<size_t>(top) * stride + static_cast<size_t>(left) * 4,
                           roiH, roiW, 4, stride, gpu_stream_))
        return false;

    const bool direct = crop_enabled_ && roiW == out_side_ && roiH == out_side_;
    GpuImage& dst = direct ? nextOutSlot() : scratch_full_;
    if (!dst.create(roiH, roiW, 3))
        return false;
    launch_bgra_to_bgr_u8(scratch_a_.data(), scratch_a_.step(), dst.data(), dst.step(),
                          roiW, roiH, gpu_stream_);

    GpuImage out = direct ? dst : ResizeToOut(scratch_full_);
    if (out.empty())
        return false;
    cudaEvent_t e = out_events_[current_out_idx_];
    if (e) { cudaEventRecord(e, gpu_stream_); out.setReadyEvent(e); }
    else cudaStreamSynchronize(gpu_stream_);
    EnqueueGpu(std::move(out));
    return true;
}

bool MFCapture::PushMjpgGpu(const uint8_t* data, size_t size)
{
    if (!gpu_decoder_ || !gpu_stream_ || !data || size == 0)
        return false;

    // Stage the JPEG into pinned host memory so nvJPEG's host->device copy is
    // efficient. A single buffer is safe here even without a per-frame sync:
    // decodeCropped's host (parse + entropy) phase consumes these bytes
    // synchronously before it returns; only the decoder's INTERNAL scratch is
    // read asynchronously, and that is ring-buffered inside GpuJpegDecoder.
    if (pinned_jpeg_capacity_ < size)
    {
        if (pinned_jpeg_buffer_)
            cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;

        const size_t want = std::max<size_t>(size, 256 * 1024);
        if (cudaHostAlloc(reinterpret_cast<void**>(&pinned_jpeg_buffer_), want, cudaHostAllocDefault) == cudaSuccess)
            pinned_jpeg_capacity_ = want;
    }

    const uint8_t* decodeSrc = data;
    if (pinned_jpeg_buffer_ && pinned_jpeg_capacity_ >= size)
    {
        std::memcpy(pinned_jpeg_buffer_, data, size);
        decodeSrc = pinned_jpeg_buffer_;
    }

    GpuImage out;
    // ROI decode straight into an output slot — only the centered out_side
    // region is reconstructed on the GPU.
    if (crop_enabled_)
    {
        GpuImage& slot = nextOutSlot();
        if (gpu_decoder_->decodeCropped(decodeSrc, size, out_side_, out_side_, slot, gpu_stream_))
            out = slot;
    }

    if (out.empty())
    {
        // Fallback / no-crop: full-frame decode into scratch, then crop-or-resize
        // into an output slot.
        if (!gpu_decoder_->decode(decodeSrc, size, scratch_full_, gpu_stream_))
            return false;
        if (crop_enabled_ && scratch_full_.cols() >= out_side_ && scratch_full_.rows() >= out_side_)
        {
            const int left = (scratch_full_.cols() - out_side_) / 2;
            const int top = (scratch_full_.rows() - out_side_) / 2;
            out = ResizeToOut(scratch_full_.subRect(left, top, out_side_, out_side_));
        }
        else
        {
            out = ResizeToOut(scratch_full_);
        }
    }

    if (out.empty())
        return false;

    // No per-frame sync: record this frame's completion on gpu_stream_ and hand
    // the event to the consumer (the detector waits on it from its own stream,
    // and download() waits via cudaEventSynchronize). This lets the next frame's
    // host-side entropy decode overlap this frame's GPU reconstruction. Fall
    // back to a sync if the event is missing so a consumer never reads an
    // incomplete frame.
    cudaEvent_t e = out_events_[current_out_idx_];
    if (e)
    {
        cudaEventRecord(e, gpu_stream_);
        out.setReadyEvent(e);
    }
    else
    {
        cudaStreamSynchronize(gpu_stream_);
    }
    EnqueueGpu(std::move(out));
    return true;
}

// ---------------- CPU decode paths ----------------
// Color-convert (or imdecode) the full frame, then center-crop / resize to the
// square output. For raw formats crop-after-convert is pixel-identical to
// crop-before-convert; there is no PCIe transfer to save on the CPU path.

bool MFCapture::PushNv12Cpu(const uint8_t* data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width;

    cv::Mat nv12(height + height / 2, width, CV_8UC1, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}

bool MFCapture::PushYuy2Cpu(const uint8_t* data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 2;

    cv::Mat yuy2(height, width, CV_8UC2, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(yuy2, bgr, cv::COLOR_YUV2BGR_YUY2);
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}

bool MFCapture::PushRgb32Cpu(const uint8_t* data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 4;

    cv::Mat bgra(height, width, CV_8UC4, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}

bool MFCapture::PushMjpgCpu(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return false;

    cv::Mat raw(1, static_cast<int>(size), CV_8UC1, const_cast<uint8_t*>(data));
    cv::Mat bgr = cv::imdecode(raw, cv::IMREAD_COLOR);
    if (bgr.empty())
        return false;
    cv::Mat out = FinalizeCpu(bgr);
    if (out.empty())
        return false;
    EnqueueCpu(std::move(out));
    return true;
}
