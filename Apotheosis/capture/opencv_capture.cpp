#include "opencv_capture.h"

#include <algorithm>
#include <iostream>
#include <utility>

int OpenCVCapture::ResolveApiPreference(const std::string& name)
{
    if (name == "DSHOW")   return cv::CAP_DSHOW;
    if (name == "MSMF")    return cv::CAP_MSMF;
    if (name == "FFMPEG")  return cv::CAP_FFMPEG;
    if (name == "V4L2")    return cv::CAP_V4L2;
    return cv::CAP_ANY;
}

int OpenCVCapture::ResolveFourcc(const std::string& format)
{
    // RGB32 has no portable FOURCC; cv::VideoCapture delivers BGR regardless,
    // so we leave the device on its default mode for that case (returns 0).
    if (format == "MJPG") return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    if (format == "NV12") return cv::VideoWriter::fourcc('N', 'V', '1', '2');
    if (format == "YUY2") return cv::VideoWriter::fourcc('Y', 'U', 'Y', '2');
    return 0;
}

OpenCVCapture::OpenCVCapture(int src_width,
                             int src_height,
                             int out_side,
                             bool crop_enabled,
                             int capture_fps,
                             const std::string& format,
                             int device_index,
                             const std::string& api_preference,
                             const std::string& connection_url)
    : src_width_(std::max(0, src_width))
    , src_height_(std::max(0, src_height))
    , out_side_(std::max(1, out_side))
    , crop_enabled_(crop_enabled)
    , capture_fps_(std::max(0, capture_fps))
    , format_(format)
    , device_index_(std::max(0, device_index))
    , api_preference_name_(api_preference)
    , api_preference_(ResolveApiPreference(api_preference))
    , connection_url_(connection_url)
{
    source_fps_start_ = std::chrono::steady_clock::now();
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
    if (grab_thread_.joinable())
        grab_thread_.join();
    CloseDevice();
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

    // FOURCC must be set before the resolution on most Windows backends,
    // otherwise the device stays on its default (often low-fps YUY2) mode.
    const int fourcc = ResolveFourcc(format_);
    if (fourcc != 0)
        cap_.set(cv::CAP_PROP_FOURCC, fourcc);

    if (src_width_ > 0)
        cap_.set(cv::CAP_PROP_FRAME_WIDTH, src_width_);
    if (src_height_ > 0)
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, src_height_);
    if (capture_fps_ > 0)
        cap_.set(cv::CAP_PROP_FPS, capture_fps_);

    cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // Read back what the backend actually negotiated. cv::VideoCapture has weak
    // control over capture cards: if the requested FOURCC didn't stick the
    // device usually falls back to YUY2/raw and gets bandwidth-locked to a low
    // fps over USB. Surface that here so a "locked fps" is diagnosable instead
    // of silent — the MF backend is the way out when this warns.
    const double actualFps = cap_.get(cv::CAP_PROP_FPS);
    const int actualFourcc = static_cast<int>(cap_.get(cv::CAP_PROP_FOURCC));
    char fourccStr[5] = { 0 };
    for (int i = 0; i < 4; ++i)
        fourccStr[i] = static_cast<char>((actualFourcc >> (8 * i)) & 0xFF);

    std::cout << "[Capture][OpenCV] Opened "
              << (connection_url_.empty()
                    ? ("device #" + std::to_string(device_index_))
                    : connection_url_)
              << " via " << api_preference_name_
              << " req-fmt=" << format_ << " got-fourcc=" << fourccStr
              << " @ " << cap_.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
              << cap_.get(cv::CAP_PROP_FRAME_HEIGHT)
              << " " << actualFps << "fps -> "
              << out_side_ << "x" << out_side_
              << (crop_enabled_ ? " (center-crop)" : " (scaled)") << std::endl;

    const int wantFourcc = ResolveFourcc(format_);
    if (wantFourcc != 0 && actualFourcc != wantFourcc)
        std::cerr << "[Capture][OpenCV] WARNING: requested format " << format_
                  << " not honored (got '" << fourccStr << "'); the device may be "
                     "bandwidth-locked to a low fps. Try the MF backend or another format."
                  << std::endl;
    if (capture_fps_ > 0 && actualFps > 0.0 && actualFps < capture_fps_ * 0.5)
        std::cerr << "[Capture][OpenCV] WARNING: negotiated " << actualFps
                  << "fps is well below requested " << capture_fps_
                  << "fps (cv::VideoCapture fps control is unreliable; consider the MF backend)."
                  << std::endl;
    return true;
}

void OpenCVCapture::CloseDevice()
{
    if (cap_.isOpened())
        cap_.release();
    is_open_.store(false);
}

void OpenCVCapture::TickFps()
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

void OpenCVCapture::GrabLoop()
{
    while (!should_stop_.load())
    {
        cv::Mat frame;
        if (!cap_.read(frame) || frame.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        cv::Mat out;
        if (crop_enabled_ && frame.cols >= out_side_ && frame.rows >= out_side_)
        {
            // Center-crop the square region of interest. The decoded frame is
            // refcounted, so cropROI is a view; clone() detaches it into a
            // tightly-packed buffer the consumer can own safely.
            const int left = (frame.cols - out_side_) / 2;
            const int top = (frame.rows - out_side_) / 2;
            out = frame(cv::Rect(left, top, out_side_, out_side_)).clone();
        }
        else
        {
            // crop disabled, or the source is smaller than the requested crop:
            // fall back to scaling the whole frame to the square output.
            cv::resize(frame, out, cv::Size(out_side_, out_side_));
        }

        TickFps();

        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_ = std::move(out);
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
