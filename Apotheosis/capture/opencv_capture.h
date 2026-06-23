#ifndef OPENCV_CAPTURE_H
#define OPENCV_CAPTURE_H

#include "capture.h"

#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

// Capture-card backend backed by cv::VideoCapture. Targets USB / PCIe capture
// cards exposed as a DirectShow (CAP_DSHOW) or Media Foundation (CAP_MSMF)
// device on Windows, identified by integer index or by a connection URL
// (e.g. an RTSP / file path).
//
// Pipeline (matches the project contract: the frame fed to inference is the
// centered region of interest, never the whole downscaled frame):
//   1. negotiate the device at src_width x src_height @ fps in `format`
//   2. cv::VideoCapture decodes each frame to BGR internally
//   3. produce a square out_side x out_side BGR frame:
//        crop_enabled  -> center-crop out_side x out_side from the decoded frame
//        !crop_enabled -> resize the whole decoded frame to out_side x out_side
//
// cv::VideoCapture owns the decode step, so cropping happens after decode here;
// the resulting pixels are identical to crop-before-decode for raw formats.
class OpenCVCapture : public IScreenCapture
{
public:
    OpenCVCapture(int src_width,
                  int src_height,
                  int out_side,
                  bool crop_enabled,
                  int capture_fps,
                  const std::string& format,
                  int device_index,
                  const std::string& api_preference,
                  const std::string& connection_url = std::string());
    ~OpenCVCapture() override;

    OpenCVCapture(const OpenCVCapture&) = delete;
    OpenCVCapture& operator=(const OpenCVCapture&) = delete;

    cv::Mat GetNextFrameCpu() override;
    int GetSourceFpsEstimate() const override { return source_fps_.load(); }

    bool IsOpen() const { return is_open_.load(); }

private:
    void GrabLoop();
    bool OpenDevice();
    void CloseDevice();
    void TickFps();
    static int ResolveApiPreference(const std::string& name);
    static int ResolveFourcc(const std::string& format);

    int src_width_;
    int src_height_;
    int out_side_;
    bool crop_enabled_;
    int capture_fps_;
    std::string format_;
    int device_index_;
    std::string api_preference_name_;
    int api_preference_;
    std::string connection_url_;

    cv::VideoCapture cap_;

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };
    std::atomic<int> source_fps_{ 0 };
    int source_frame_count_{ 0 };
    std::chrono::steady_clock::time_point source_fps_start_;

    std::thread grab_thread_;
    std::mutex frame_mutex_;
    cv::Mat latest_;
    bool has_frame_{ false };
};

#endif // OPENCV_CAPTURE_H
