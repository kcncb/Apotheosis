#include "crosshair/color_picker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace crosshair
{
namespace
{
std::atomic<bool> g_armed{ false };

std::mutex g_result_mutex;
bool g_result_ready = false;
int  g_result_h = 0;
int  g_result_s = 0;
int  g_result_v = 0;
} // namespace

void ArmColorPick()
{
    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_result_ready = false;
    }
    g_armed.store(true);
}

void CancelColorPick()
{
    g_armed.store(false);
}

bool IsColorPickArmed()
{
    return g_armed.load();
}

void SubmitPickedColor(int h, int s, int v)
{
    {
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_result_h = h;
        g_result_s = s;
        g_result_v = v;
        g_result_ready = true;
    }
    g_armed.store(false);
}

bool TakePickedColor(int& h, int& s, int& v)
{
    std::lock_guard<std::mutex> lk(g_result_mutex);
    if (!g_result_ready)
        return false;
    h = g_result_h;
    s = g_result_s;
    v = g_result_v;
    g_result_ready = false;
    return true;
}

bool SampleRegionHSV(const cv::Mat& bgr, int cx, int cy, int half,
                     int& h, int& s, int& v)
{
    if (bgr.empty() || bgr.type() != CV_8UC3)
        return false;
    if (cx < 0 || cy < 0 || cx >= bgr.cols || cy >= bgr.rows)
        return false;

    half = std::max(0, half);
    const int x0 = std::max(0, cx - half);
    const int y0 = std::max(0, cy - half);
    const int x1 = std::min(bgr.cols - 1, cx + half);
    const int y1 = std::min(bgr.rows - 1, cy + half);
    const cv::Rect roi(x0, y0, x1 - x0 + 1, y1 - y0 + 1);

    cv::Mat hsv;
    cv::cvtColor(bgr(roi), hsv, cv::COLOR_BGR2HSV); // H 0..179, S/V 0..255

    std::vector<int> sVals;
    std::vector<int> vVals;
    sVals.reserve(roi.area());
    vVals.reserve(roi.area());

    // H is angular: averaging raw hues breaks across the red 0/179 seam (e.g.
    // hues 2 and 178 are 4 apart, not 176). Accumulate unit vectors and take
    // the circular mean instead. 0..179 maps to 0..2pi at 2 deg/unit, i.e.
    // angle = hue * (pi / 90).
    double sumSin = 0.0;
    double sumCos = 0.0;
    for (int y = 0; y < hsv.rows; ++y)
    {
        const cv::Vec3b* row = hsv.ptr<cv::Vec3b>(y);
        for (int x = 0; x < hsv.cols; ++x)
        {
            const double ang = row[x][0] * (CV_PI / 90.0);
            sumCos += std::cos(ang);
            sumSin += std::sin(ang);
            sVals.push_back(row[x][1]);
            vVals.push_back(row[x][2]);
        }
    }
    if (sVals.empty())
        return false;

    double meanAng = std::atan2(sumSin, sumCos); // -pi..pi
    if (meanAng < 0.0)
        meanAng += 2.0 * CV_PI;
    h = static_cast<int>(std::lround(meanAng * (90.0 / CV_PI))) % 180;

    auto median = [](std::vector<int>& a) {
        const size_t mid = a.size() / 2;
        std::nth_element(a.begin(), a.begin() + mid, a.end());
        return a[mid];
    };
    s = median(sVals);
    v = median(vVals);
    return true;
}

} // namespace crosshair
