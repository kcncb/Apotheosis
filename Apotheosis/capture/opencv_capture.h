#ifndef OPENCV_CAPTURE_H
#define OPENCV_CAPTURE_H

#include "capture.h"

#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OpenCVCaptureDeviceInfo
{
    int index = 0;
    std::string name;
};

// Capture-card / generic video-input backend backed by cv::VideoCapture.
// Targets USB / PCIe capture cards exposed as a DirectShow (CAP_DSHOW) or
// Media Foundation (CAP_MSMF) device on Windows. The device is identified
// either by integer index or by a connection URL (e.g. an RTSP / file path).
class OpenCVCapture : public IScreenCapture
{
public:
    OpenCVCapture(int detection_width,
                  int detection_height,
                  int device_index,
                  const std::string& api_preference,
                  int capture_width,
                  int capture_height,
                  int capture_fps,
                  const std::string& pixel_format,
                  int crop_width,
                  int crop_height,
                  const std::string& connection_url = std::string());
    ~OpenCVCapture() override;

    OpenCVCapture(const OpenCVCapture&) = delete;
    OpenCVCapture& operator=(const OpenCVCapture&) = delete;

    cv::Mat GetNextFrameCpu() override;

    bool IsOpen() const { return is_open_.load(); }

    static std::vector<OpenCVCaptureDeviceInfo> EnumerateDevices(const std::string& api_preference,
                                                                 int max_devices = 10);

private:
    void GrabLoop();
    bool OpenDevice();
    void CloseDevice();
    static int ResolveApiPreference(const std::string& name);
    static int ResolveFourCC(const std::string& name);
    cv::Mat CropFrameCenter(const cv::Mat& input) const;

    int detection_width_;
    int detection_height_;
    int device_index_;
    std::string api_preference_name_;
    int api_preference_;
    int capture_width_;
    int capture_height_;
    int capture_fps_;
    std::string pixel_format_;
    int crop_width_;
    int crop_height_;
    std::string connection_url_;

    cv::VideoCapture cap_;

    std::atomic<bool> is_open_{ false };
    std::atomic<bool> should_stop_{ false };

    std::thread grab_thread_;
    std::mutex frame_mutex_;
    cv::Mat latest_;
    bool has_frame_{ false };
};

#endif // OPENCV_CAPTURE_H
