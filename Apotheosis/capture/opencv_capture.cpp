#include "opencv_capture.h"

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>
#include <dshow.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

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

std::vector<OpenCVCaptureDeviceInfo> EnumerateDirectShowDevices()
{
    std::vector<OpenCVCaptureDeviceInfo> devices;
    const HRESULT co_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool should_uninit = SUCCEEDED(co_init);

    ICreateDevEnum* dev_enum = nullptr;
    IEnumMoniker* enum_moniker = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dev_enum));
    if (SUCCEEDED(hr) && dev_enum)
    {
        hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
    }

    if (hr == S_OK && enum_moniker)
    {
        IMoniker* moniker = nullptr;
        ULONG fetched = 0;
        int index = 0;
        while (enum_moniker->Next(1, &moniker, &fetched) == S_OK)
        {
            IPropertyBag* prop_bag = nullptr;
            std::string name;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&prop_bag))) && prop_bag)
            {
                VARIANT friendly_name;
                VariantInit(&friendly_name);
                if (SUCCEEDED(prop_bag->Read(L"FriendlyName", &friendly_name, nullptr)) &&
                    friendly_name.vt == VT_BSTR)
                {
                    name = WideToUtf8(friendly_name.bstrVal);
                }
                VariantClear(&friendly_name);
                prop_bag->Release();
            }

            OpenCVCaptureDeviceInfo info;
            info.index = index;
            info.name = name.empty() ? ("设备 #" + std::to_string(index)) : name;
            devices.push_back(std::move(info));
            moniker->Release();
            moniker = nullptr;
            ++index;
        }
    }

    if (enum_moniker)
        enum_moniker->Release();
    if (dev_enum)
        dev_enum->Release();
    if (should_uninit)
        CoUninitialize();

    return devices;
}
}

int OpenCVCapture::ResolveApiPreference(const std::string& name)
{
    if (name == "DSHOW")   return cv::CAP_DSHOW;
    if (name == "MSMF")    return cv::CAP_MSMF;
    if (name == "FFMPEG")  return cv::CAP_FFMPEG;
    if (name == "V4L2")    return cv::CAP_V4L2;
    return cv::CAP_ANY;
}

int OpenCVCapture::ResolveFourCC(const std::string& name)
{
    if (name == "NV12") return cv::VideoWriter::fourcc('N', 'V', '1', '2');
    if (name == "MJPG") return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    if (name == "YUY2") return cv::VideoWriter::fourcc('Y', 'U', 'Y', '2');
    if (name == "YUYV") return cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V');
    if (name == "RGB3") return cv::VideoWriter::fourcc('R', 'G', 'B', '3');
    if (name == "BGR3") return cv::VideoWriter::fourcc('B', 'G', 'R', '3');
    return 0;
}

std::vector<OpenCVCaptureDeviceInfo> OpenCVCapture::EnumerateDevices(const std::string& api_preference,
                                                                     int max_devices)
{
    std::vector<OpenCVCaptureDeviceInfo> devices;
    if (api_preference == "DSHOW" || api_preference == "ANY")
    {
        devices = EnumerateDirectShowDevices();
        if (!devices.empty())
            return devices;
    }

    const int api = ResolveApiPreference(api_preference);
    max_devices = std::clamp(max_devices, 1, 32);

    for (int index = 0; index < max_devices; ++index)
    {
        cv::VideoCapture probe;
        if (!probe.open(index, api))
            continue;

        cv::Mat frame;
        const bool has_frame = probe.read(frame) && !frame.empty();
        probe.release();

        if (!has_frame)
            continue;

        OpenCVCaptureDeviceInfo info;
        info.index = index;
        info.name = "设备 #" + std::to_string(index);
        devices.push_back(std::move(info));
    }

    return devices;
}

OpenCVCapture::OpenCVCapture(int detection_width,
                             int detection_height,
                             int device_index,
                             const std::string& api_preference,
                             int capture_width,
                             int capture_height,
                             int capture_fps,
                             const std::string& pixel_format,
                             int crop_width,
                             int crop_height,
                             const std::string& connection_url)
    : detection_width_(std::max(1, detection_width))
    , detection_height_(std::max(1, detection_height))
    , device_index_(device_index)
    , api_preference_name_(api_preference)
    , api_preference_(ResolveApiPreference(api_preference))
    , capture_width_(capture_width)
    , capture_height_(capture_height)
    , capture_fps_(capture_fps)
    , pixel_format_(pixel_format)
    , crop_width_(crop_width)
    , crop_height_(crop_height)
    , connection_url_(connection_url)
{
    if (!OpenDevice())
    {
        std::cerr << "[Capture][OpenCV] Failed to open capture card (index="
                  << device_index_ << ", api=" << api_preference_name_
                  << (connection_url_.empty() ? "" : (", url=" + connection_url_))
                  << ")" << std::endl;
        return;
    }

    is_open_.store(true);
    grab_thread_ = std::thread(&OpenCVCapture::GrabLoop, this);
}

OpenCVCapture::~OpenCVCapture()
{
    should_stop_.store(true);
    CloseDevice();
    if (grab_thread_.joinable())
        grab_thread_.join();
}

bool OpenCVCapture::OpenDevice()
{
    bool opened = false;
    if (!connection_url_.empty())
        opened = cap_.open(connection_url_, api_preference_);
    else
        opened = cap_.open(device_index_, api_preference_);

    if (!opened)
        return false;

    if (capture_width_ > 0)
        cap_.set(cv::CAP_PROP_FRAME_WIDTH, capture_width_);
    if (capture_height_ > 0)
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, capture_height_);
    if (capture_fps_ > 0)
        cap_.set(cv::CAP_PROP_FPS, capture_fps_);
    const int fourcc = ResolveFourCC(pixel_format_);
    if (fourcc != 0)
        cap_.set(cv::CAP_PROP_FOURCC, fourcc);

    cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);

    std::cout << "[Capture][OpenCV] Opened "
              << (connection_url_.empty()
                    ? ("device #" + std::to_string(device_index_))
                    : connection_url_)
              << " via " << api_preference_name_
              << " @ " << cap_.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
              << cap_.get(cv::CAP_PROP_FRAME_HEIGHT)
              << " " << cap_.get(cv::CAP_PROP_FPS) << "fps"
              << " format=" << (pixel_format_.empty() ? "AUTO" : pixel_format_)
              << " crop=" << crop_width_ << "x" << crop_height_ << std::endl;
    return true;
}

void OpenCVCapture::CloseDevice()
{
    if (cap_.isOpened())
        cap_.release();
    is_open_.store(false);
}

cv::Mat OpenCVCapture::CropFrameCenter(const cv::Mat& input) const
{
    if (input.empty())
        return cv::Mat();

    cv::Mat frame = input;
    const int original_width = frame.cols;
    const int original_height = frame.rows;
    int crop_width = crop_width_ > 0 ? std::min(crop_width_, original_width) : original_width;
    int crop_height = crop_height_ > 0 ? std::min(crop_height_, original_height) : original_height;

    if (crop_width > 0 && crop_height > 0 && (crop_width < original_width || crop_height < original_height))
    {
        const int left = std::max(0, (original_width - crop_width) / 2);
        const int top = std::max(0, (original_height - crop_height) / 2);
        frame = frame(cv::Rect(left, top, crop_width, crop_height));
    }

    return frame.clone();
}

void OpenCVCapture::GrabLoop()
{
    while (!should_stop_.load())
    {
        cv::Mat frame;
        if (!cap_.read(frame) || frame.empty())
        {
            if (should_stop_.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        frame = CropFrameCenter(frame);
        if (frame.empty())
            continue;

        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_ = std::move(frame);
            has_frame_ = true;
        }
    }
}

cv::Mat OpenCVCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_frame_ || latest_.empty())
        return cv::Mat();
    has_frame_ = false;
    return latest_.clone();
}
