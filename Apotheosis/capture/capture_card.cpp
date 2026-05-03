#include "capture_card.h"

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <npp.h>
#include "gpu_color_ops.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

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

    attrs->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    return attrs;
}

bool GetSubType(IMFMediaType* type, GUID* subtype)
{
    return type && subtype && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, subtype));
}

bool MediaTypeMatches(IMFMediaType* type, const GUID& subtype, int width, int height, int fps)
{
    GUID actual{};
    if (!GetSubType(type, &actual) || actual != subtype)
        return false;

    UINT32 w = 0;
    UINT32 h = 0;
    if ((width > 0 || height > 0) && FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)))
        return false;
    if (width > 0 && static_cast<int>(w) != width)
        return false;
    if (height > 0 && static_cast<int>(h) != height)
        return false;

    UINT32 fpsNum = 0;
    UINT32 fpsDen = 0;
    if (fps > 0 && FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &fpsNum, &fpsDen)))
        return false;
    if (fps > 0 && fpsDen != 0)
    {
        const int rounded = static_cast<int>((fpsNum + fpsDen / 2) / fpsDen);
        if (rounded != fps)
            return false;
    }

    return true;
}

ComPtr<IMFMediaType> FindNativeType(IMFSourceReader* reader, const GUID& subtype, int width, int height, int fps)
{
    if (!reader)
        return nullptr;

    ComPtr<IMFMediaType> firstMatch;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> nativeType;
        const HRESULT hr = reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr) || !nativeType)
            continue;

        GUID actual{};
        if (!GetSubType(nativeType.Get(), &actual) || actual != subtype)
            continue;

        if (!firstMatch)
            firstMatch = nativeType;
        if (MediaTypeMatches(nativeType.Get(), subtype, width, height, fps))
            return nativeType;
    }

    return firstMatch;
}

bool TryConfigureFormat(IMFSourceReader* reader, const GUID& subtype, int width, int height, int fps)
{
    if (!reader)
        return false;

    ComPtr<IMFMediaType> requested;
    if (SUCCEEDED(MFCreateMediaType(&requested)) && requested)
    {
        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        requested->SetGUID(MF_MT_SUBTYPE, subtype);
        if (width > 0 && height > 0)
            MFSetAttributeSize(requested.Get(), MF_MT_FRAME_SIZE,
                               static_cast<UINT32>(width), static_cast<UINT32>(height));
        if (fps > 0)
            MFSetAttributeRatio(requested.Get(), MF_MT_FRAME_RATE,
                                static_cast<UINT32>(fps), 1);

        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                  nullptr, requested.Get())))
            return true;
    }

    ComPtr<IMFMediaType> native = FindNativeType(reader, subtype, width, height, fps);
    if (!native)
        return false;

    return SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 nullptr, native.Get()));
}

bool QueryCurrentFrameGeometry(IMFSourceReader* reader, int& width, int& height, int& stride)
{
    width = 0;
    height = 0;
    stride = 0;
    if (!reader)
        return false;

    ComPtr<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) || !current)
        return false;

    UINT32 w = 0;
    UINT32 h = 0;
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

const char* SubtypeName(const GUID& g)
{
    if (g == MFVideoFormat_NV12)  return "NV12";
    if (g == MFVideoFormat_MJPG)  return "MJPG";
    if (g == MFVideoFormat_YUY2)  return "YUY2";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    return "?";
}
} // namespace

CaptureCard::Format CaptureCard::ParseFormat(const std::string& s)
{
    if (s == "NV12")  return Format::Nv12;
    if (s == "MJPG")  return Format::Mjpg;
    if (s == "YUY2" || s == "YUYV") return Format::Yuy2;
    if (s == "RGB32" || s == "RGB3" || s == "BGR3" || s == "RGBA") return Format::Rgb32;
    return Format::Auto;
}

const char* CaptureCard::FormatLabel(Format f)
{
    switch (f)
    {
    case Format::Nv12:  return "NV12";
    case Format::Mjpg:  return "MJPG";
    case Format::Yuy2:  return "YUY2";
    case Format::Rgb32: return "RGB32";
    case Format::Auto:
    default:            return "AUTO";
    }
}

CaptureCard::CaptureCard(int detection_width,
                         int detection_height,
                         int device_index,
                         int capture_width,
                         int capture_height,
                         int capture_fps,
                         int crop_width,
                         int crop_height,
                         Format requested_format)
    : detection_width_(std::max(1, detection_width))
    , detection_height_(std::max(1, detection_height))
    , device_index_(std::max(0, device_index))
    , capture_width_(capture_width)
    , capture_height_(capture_height)
    , capture_fps_(capture_fps)
    , crop_width_(crop_width)
    , crop_height_(crop_height)
    , requested_format_(requested_format)
{
    source_fps_start_time_ = std::chrono::steady_clock::now();
    receive_thread_ = std::thread(&CaptureCard::ReceiveThread, this);
}

CaptureCard::~CaptureCard()
{
    should_stop_.store(true);
    if (receive_thread_.joinable())
        receive_thread_.join();

    if (pinned_jpeg_buffer_)
    {
        cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;
    }
    if (gpu_stream_)
    {
        cudaStreamDestroy(gpu_stream_);
        gpu_stream_ = nullptr;
    }
}

std::vector<CaptureCardDeviceInfo> CaptureCard::EnumerateDevices()
{
    std::vector<CaptureCardDeviceInfo> devices;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);
    const HRESULT mfStart = MFStartup(MF_VERSION, MFSTARTUP_LITE);

    if (SUCCEEDED(mfStart))
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
                wchar_t* symbolicLink = nullptr;
                UINT32 symbolicLinkLen = 0;
                std::string label;
                std::string detail;
                if (SUCCEEDED(activates[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen)))
                {
                    label = WideToUtf8(name);
                    CoTaskMemFree(name);
                }
                if (SUCCEEDED(activates[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        &symbolicLink,
                        &symbolicLinkLen)))
                {
                    detail = WideToUtf8(symbolicLink);
                    CoTaskMemFree(symbolicLink);
                }

                CaptureCardDeviceInfo info;
                info.index = static_cast<int>(i);
                std::ostringstream display;
                display << u8"设备 #" << i;
                if (!label.empty())
                    display << " - " << label;
                if (!detail.empty())
                    display << " [" << detail << "]";
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

cv::Mat CaptureCard::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (cpu_frame_queue_.empty())
        return cv::Mat();

    cv::Mat frame = std::move(cpu_frame_queue_.front());
    cpu_frame_queue_.pop();
    return frame;
}

GpuImage CaptureCard::GetNextFrameGpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (gpu_frame_queue_.empty())
        return GpuImage();

    GpuImage frame = std::move(gpu_frame_queue_.front());
    gpu_frame_queue_.pop();
    return frame;
}

void CaptureCard::ReceiveThread()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(coInit);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        std::cerr << "[CaptureCard] MFStartup failed." << std::endl;
        if (shouldUninit)
            CoUninitialize();
        return;
    }

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    ComPtr<IMFAttributes> deviceAttrs = CreateVideoDeviceAttributes();
    if (!deviceAttrs || FAILED(MFEnumDeviceSources(deviceAttrs.Get(), &activates, &count)) || count == 0)
    {
        std::cerr << "[CaptureCard] No video capture devices found." << std::endl;
        goto cleanup;
    }

    if (device_index_ >= static_cast<int>(count))
        device_index_ = 0;

    if (FAILED(activates[device_index_]->ActivateObject(IID_PPV_ARGS(&source))))
    {
        std::cerr << "[CaptureCard] Failed to activate capture device." << std::endl;
        goto cleanup;
    }

    {
        // Disable MF's built-in converters so the device delivers raw frames
        // (NV12 stays NV12, MJPG stays MJPG) instead of being silently
        // re-encoded by the source reader on the CPU.
        ComPtr<IMFAttributes> readerAttrs;
        MFCreateAttributes(&readerAttrs, 2);
        if (readerAttrs)
        {
            readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
        }

        if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader)))
        {
            std::cerr << "[CaptureCard] Failed to create source reader." << std::endl;
            goto cleanup;
        }
    }

    {
        struct Candidate
        {
            Format format;
            const GUID* subtype;
            ActiveFormat active;
        };
        const std::array<Candidate, 4> kAll = { {
            { Format::Nv12,  &MFVideoFormat_NV12,  ActiveFormat::Nv12   },
            { Format::Mjpg,  &MFVideoFormat_MJPG,  ActiveFormat::MjpgGpu },
            { Format::Yuy2,  &MFVideoFormat_YUY2,  ActiveFormat::Yuy2   },
            { Format::Rgb32, &MFVideoFormat_RGB32, ActiveFormat::Rgb32  },
        } };

        std::vector<Candidate> trial;
        if (requested_format_ == Format::Auto)
        {
            trial.assign(kAll.begin(), kAll.end());
        }
        else
        {
            for (const auto& c : kAll)
                if (c.format == requested_format_) { trial.push_back(c); break; }
        }

        for (const auto& c : trial)
        {
            if (!TryConfigureFormat(reader.Get(), *c.subtype, capture_width_, capture_height_, capture_fps_) ||
                !QueryCurrentFrameGeometry(reader.Get(), frame_width_, frame_height_, frame_stride_))
                continue;

            if (!gpu_stream_)
            {
                if (cudaStreamCreateWithFlags(&gpu_stream_, cudaStreamNonBlocking) != cudaSuccess)
                {
                    std::cerr << "[CaptureCard] CUDA stream creation failed; aborting." << std::endl;
                    goto cleanup;
                }

                // Build NppStreamContext once. NPP 12 dropped the global
                // nppSetStream() API, so every NPP call we make has to
                // route through a _Ctx variant that takes this struct.
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
            }

            if (c.active == ActiveFormat::MjpgGpu)
            {
                gpu_decoder_ = std::make_unique<capture::GpuJpegDecoder>();
                if (!gpu_decoder_->init())
                {
                    std::cerr << "[CaptureCard] nvJPEG unavailable; trying next format." << std::endl;
                    gpu_decoder_.reset();
                    continue;
                }
            }

            active_format_ = c.active;
            std::cout << "[CaptureCard] Active mode " << SubtypeName(*c.subtype)
                      << " @ " << frame_width_ << "x" << frame_height_
                      << " stride=" << frame_stride_ << " (GPU)" << std::endl;
            break;
        }

        if (active_format_ == ActiveFormat::None)
        {
            std::cerr << "[CaptureCard] Device did not expose any supported pixel format." << std::endl;
            goto cleanup;
        }
    }

    is_open_.store(true);
    while (!should_stop_.load())
    {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0,
            &streamIndex,
            &flags,
            &timestamp,
            &sample);

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
        DWORD maxLen = 0;
        DWORD currentLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &currentLen)))
            continue;

        if (data && currentLen > 0)
        {
            switch (active_format_)
            {
            case ActiveFormat::Nv12:
                PushNv12(data, frame_width_, frame_height_, frame_stride_);
                break;
            case ActiveFormat::MjpgGpu:
                PushMjpgGpu(data, static_cast<size_t>(currentLen));
                break;
            case ActiveFormat::Yuy2:
                PushYuy2(data, frame_width_, frame_height_, frame_stride_);
                break;
            case ActiveFormat::Rgb32:
                PushRgb32(data, frame_width_, frame_height_, frame_stride_);
                break;
            default:
                break;
            }
        }

        buffer->Unlock();
    }

cleanup:
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

bool CaptureCard::TickFps()
{
    const int produced = source_frame_count_.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - source_fps_start_time_;
    if (elapsed.count() >= 1.0)
    {
        source_fps_.store(static_cast<int>(std::lround(produced / elapsed.count())));
        source_frame_count_.store(0);
        source_fps_start_time_ = now;
    }
    return true;
}

GpuImage CaptureCard::ApplyCropGpu(const GpuImage& src) const
{
    if (src.empty())
        return src;
    const int cropW = crop_width_ > 0 ? std::min(crop_width_, src.cols()) : src.cols();
    const int cropH = crop_height_ > 0 ? std::min(crop_height_, src.rows()) : src.rows();
    if (cropW > 0 && cropH > 0 && (cropW < src.cols() || cropH < src.rows()))
    {
        const int left = std::max(0, (src.cols() - cropW) / 2);
        const int top = std::max(0, (src.rows() - cropH) / 2);
        return src.subRect(left, top, cropW, cropH);
    }
    return src;
}

namespace
{
// Upload an MF buffer (with possibly padded stride) into a freshly-allocated
// GpuImage via the given CUDA stream. cudaMemcpy2D handles the non-natural
// source stride; the destination uses natural step.
GpuImage UploadHostBytes(const uint8_t* data, int rows, int cols, int channels, int stride,
                         cudaStream_t stream)
{
    GpuImage gpu;
    gpu.upload(data, rows, cols, channels, static_cast<size_t>(stride), stream);
    return gpu;
}

bool PushGpuFrame(std::queue<GpuImage>& q, std::mutex& m, std::atomic<int>& dropped,
                  GpuImage&& frame, int max_size)
{
    if (frame.empty())
        return false;
    std::lock_guard<std::mutex> lock(m);
    while (static_cast<int>(q.size()) >= max_size)
    {
        q.pop();
        dropped++;
    }
    q.push(std::move(frame));
    return true;
}
} // namespace

// All four GPU-conversion paths share the same shape:
//   1) upload MF buffer (with possibly padded stride) into a GpuImage
//   2) NPP _Ctx (NV12 / YUY2) or hand-written kernel (RGB32 BGRA->BGR) on
//      gpu_stream_; nvJPEG for MJPG
//   3) ApplyCropGpu — zero-copy sub-rectangle view
//   4) cudaStreamSynchronize, then push the GpuImage to gpu_frame_queue_
bool CaptureCard::PushNv12(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width;

    // NV12 = Y plane (H rows) followed by interleaved UV plane (H/2 rows),
    // both at the same row-stride. Upload as one (H+H/2) x W single-channel
    // image, then point NPP at the two sub-planes.
    GpuImage src = UploadHostBytes(data, height + height / 2, width, 1, stride, gpu_stream_);
    if (src.empty())
        return false;

    GpuImage bgr;
    if (!bgr.create(height, width, 3))
        return false;
    const Npp8u* pSrc[2] = {
        src.data(),
        src.data() + src.step() * static_cast<size_t>(height)
    };
    NppiSize roi = { width, height };
    if (nppiNV12ToBGR_8u_P2C3R_Ctx(pSrc, static_cast<int>(src.step()),
                                    bgr.data(), static_cast<int>(bgr.step()),
                                    roi, npp_ctx_) != NPP_SUCCESS)
        return false;

    GpuImage cropped = ApplyCropGpu(bgr);
    cudaStreamSynchronize(gpu_stream_);
    if (cropped.empty())
        return false;

    TickFps();
    return PushGpuFrame(gpu_frame_queue_, frame_mutex_, dropped_frames_,
                        std::move(cropped), MAX_QUEUE_SIZE);
}

bool CaptureCard::PushYuy2(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 2;

    GpuImage src = UploadHostBytes(data, height, width, 2, stride, gpu_stream_);
    if (src.empty())
        return false;

    GpuImage bgr;
    if (!bgr.create(height, width, 3))
        return false;
    NppiSize roi = { width, height };
    if (nppiYCbCr422ToBGR_8u_C2C3R_Ctx(src.data(), static_cast<int>(src.step()),
                                        bgr.data(), static_cast<int>(bgr.step()),
                                        roi, npp_ctx_) != NPP_SUCCESS)
        return false;

    GpuImage cropped = ApplyCropGpu(bgr);
    cudaStreamSynchronize(gpu_stream_);
    if (cropped.empty())
        return false;

    TickFps();
    return PushGpuFrame(gpu_frame_queue_, frame_mutex_, dropped_frames_,
                        std::move(cropped), MAX_QUEUE_SIZE);
}

bool CaptureCard::PushRgb32(const uint8_t* data, int width, int height, int stride)
{
    if (!gpu_stream_ || !data || width <= 0 || height <= 0)
        return false;
    if (stride <= 0)
        stride = width * 4;

    GpuImage src = UploadHostBytes(data, height, width, 4, stride, gpu_stream_);
    if (src.empty())
        return false;

    // NPP 12 removed nppiCopy_8u_AC4C3R (the variant that drops the alpha
    // channel into a packed BGR destination). Hand-rolled kernel does the
    // same on gpu_stream_ and avoids pulling cv::cuda back in.
    GpuImage bgr;
    if (!bgr.create(height, width, 3))
        return false;
    launch_bgra_to_bgr_u8(src.data(), src.step(), bgr.data(), bgr.step(),
                          width, height, gpu_stream_);

    GpuImage cropped = ApplyCropGpu(bgr);
    cudaStreamSynchronize(gpu_stream_);
    if (cropped.empty())
        return false;

    TickFps();
    return PushGpuFrame(gpu_frame_queue_, frame_mutex_, dropped_frames_,
                        std::move(cropped), MAX_QUEUE_SIZE);
}

bool CaptureCard::PushMjpgGpu(const uint8_t* data, size_t size)
{
    if (!gpu_decoder_ || !gpu_stream_ || !data || size == 0)
        return false;

    if (pinned_jpeg_capacity_ < size)
    {
        if (pinned_jpeg_buffer_)
            cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;

        const size_t want = std::max<size_t>(size, 256 * 1024);
        if (cudaHostAlloc(reinterpret_cast<void**>(&pinned_jpeg_buffer_),
                          want, cudaHostAllocDefault) == cudaSuccess)
        {
            pinned_jpeg_capacity_ = want;
        }
    }

    const uint8_t* decodeSrc = data;
    if (pinned_jpeg_buffer_ && pinned_jpeg_capacity_ >= size)
    {
        std::memcpy(pinned_jpeg_buffer_, data, size);
        decodeSrc = pinned_jpeg_buffer_;
    }

    GpuImage decoded;
    if (!gpu_decoder_->decode(decodeSrc, size, decoded, gpu_stream_))
        return false;

    GpuImage cropped = ApplyCropGpu(decoded);
    cudaStreamSynchronize(gpu_stream_);
    if (cropped.empty())
        return false;

    TickFps();

    std::lock_guard<std::mutex> lock(frame_mutex_);
    while (gpu_frame_queue_.size() >= MAX_QUEUE_SIZE)
    {
        gpu_frame_queue_.pop();
        dropped_frames_++;
    }
    gpu_frame_queue_.push(std::move(cropped));
    return true;
}
