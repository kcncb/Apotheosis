#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>

#include "Apotheosis.h"
#include "capture.h"
#include "config/config.h"
#include "crosshair/crosshair_detector.h"
#include "detection_buffer.h"
#include "i_detector.h"
#include "preview_window.h"
#include "runtime/active_hotkey.h"
#include "runtime/inference_session.h"

namespace
{
constexpr const char* kWindowName = "Detection Preview";

std::thread g_thread;
std::atomic<bool> g_run{ false };

cv::Scalar bgr(int b, int g, int r) { return cv::Scalar(b, g, r); }

void draw_text_with_bg(cv::Mat& img, const std::string& text, cv::Point org,
                       const cv::Scalar& fg, const cv::Scalar& bg)
{
    int baseline = 0;
    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double scale = 0.45;
    const int thickness = 1;
    cv::Size sz = cv::getTextSize(text, font, scale, thickness, &baseline);
    cv::Point tl(org.x, org.y - sz.height - 3);
    cv::Point br(org.x + sz.width + 4, org.y + 2);
    cv::rectangle(img, tl, br, bg, cv::FILLED);
    cv::putText(img, text, cv::Point(org.x + 2, org.y - 2), font, scale, fg, thickness, cv::LINE_AA);
}

bool window_visible()
{
    try {
        return cv::getWindowProperty(kWindowName, cv::WND_PROP_VISIBLE) >= 1.0;
    } catch (...) {
        return false;
    }
}

void destroy_window_safe()
{
    try { cv::destroyWindow(kWindowName); } catch (...) {}
}

// Snapshot of the bits the preview needs from config in one short critical
// section, so we don't hold configMutex while doing OpenCV drawing.
struct PreviewConfigSnapshot
{
    bool   show_window = false;
    int    detection_resolution = 0;
    int    crosshair_rect_w = 0;
    int    crosshair_rect_h = 0;
    int    crosshair_min_pixel_count = 0;
    int    crosshair_close_radius = 0;
    std::vector<crosshair::CrosshairColorBand> crosshair_colors;
    bool   any_color_enabled = false;

    // Active (or fallback #0) hotkey FOV state.
    int    fov_base_x = 0;
    int    fov_base_y = 0;
    bool   dynamic_fov_enabled = false;
    bool   hotkey_active = false;
};

PreviewConfigSnapshot snapshot_config()
{
    PreviewConfigSnapshot s;
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    s.show_window              = config.show_window;
    s.detection_resolution     = config.detection_resolution;
    s.crosshair_rect_w         = config.crosshair_rect_w;
    s.crosshair_rect_h         = config.crosshair_rect_h;
    s.crosshair_min_pixel_count = config.crosshair_min_pixel_count;
    s.crosshair_close_radius   = config.crosshair_close_radius;
    s.crosshair_colors.reserve(config.crosshair_colors.size());
    for (const auto& c : config.crosshair_colors)
    {
        crosshair::CrosshairColorBand b;
        b.name = c.name;
        b.enabled = c.enabled;
        b.h_low = c.h_low; b.h_high = c.h_high;
        b.s_min = c.s_min; b.s_max = c.s_max;
        b.v_min = c.v_min; b.v_max = c.v_max;
        s.any_color_enabled = s.any_color_enabled || b.enabled;
        s.crosshair_colors.push_back(std::move(b));
    }

    const int idx_active = runtime::g_active_hotkey_index.load();
    const int idx = (idx_active >= 0 && idx_active < static_cast<int>(config.hotkeys.size()))
        ? idx_active
        : (config.hotkeys.empty() ? -1 : 0);
    s.hotkey_active = (idx_active >= 0);
    if (idx >= 0)
    {
        const auto& hk = config.hotkeys[idx];
        s.fov_base_x = hk.fovX;
        s.fov_base_y = hk.fovY;
        s.dynamic_fov_enabled = hk.dynamic_fov_enabled;
    }
    return s;
}

void render_overlays(cv::Mat& canvas, const PreviewConfigSnapshot& cfg)
{
    if (canvas.empty()) return;

    // 1. Detection boxes and class labels.
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    int version = 0;
    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        boxes = detectionBuffer.boxes;
        classes = detectionBuffer.classes;
        version = detectionBuffer.version;
    }

    const cv::Scalar boxColor = bgr(110, 220, 80);  // greenish
    const cv::Scalar textFg   = bgr(250, 245, 240);
    const cv::Scalar textBg   = bgr(0, 0, 0);

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const cv::Rect& r = boxes[i];
        const cv::Rect clipped = r & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() <= 0) continue;
        cv::rectangle(canvas, clipped, boxColor, 1, cv::LINE_AA);

        const int cls = (i < classes.size()) ? classes[i] : -1;
        std::string label = "#" + std::to_string(cls);
        draw_text_with_bg(canvas, label,
                          cv::Point(clipped.x, std::max(12, clipped.y)),
                          textFg, textBg);
    }

    // 2. FOV ellipses (base + dynamic gate).
    if (cfg.fov_base_x > 0 && cfg.fov_base_y > 0)
    {
        const cv::Point center(canvas.cols / 2, canvas.rows / 2);
        const cv::Size baseAxes(std::max(1, cfg.fov_base_x / 2),
                                std::max(1, cfg.fov_base_y / 2));

        const float dynRx = g_dynamic_fov_radius_x_px.load();
        const float dynRy = g_dynamic_fov_radius_y_px.load();
        const bool dynActive = cfg.dynamic_fov_enabled && dynRx > 0.0f && dynRy > 0.0f;

        const cv::Scalar baseCol = dynActive ? bgr(60, 200, 255) : bgr(60, 200, 255); // amber
        cv::ellipse(canvas, center, baseAxes, 0, 0, 360, baseCol, 1, cv::LINE_AA);

        if (dynActive)
        {
            const cv::Size dynAxes(std::max(1, static_cast<int>(dynRx)),
                                   std::max(1, static_cast<int>(dynRy)));
            const cv::Scalar dynCol = bgr(255, 220, 80); // cyan-blue
            cv::ellipse(canvas, center, dynAxes, 0, 0, 360, dynCol, 2, cv::LINE_AA);
        }
    }

    // 3. Crosshair colour-find ROI rectangle.
    if (cfg.crosshair_rect_w > 0 && cfg.crosshair_rect_h > 0)
    {
        const int rw = std::max(4, cfg.crosshair_rect_w);
        const int rh = std::max(4, cfg.crosshair_rect_h);
        const cv::Rect roi(canvas.cols / 2 - rw / 2, canvas.rows / 2 - rh / 2, rw, rh);
        const cv::Rect clipped = roi & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() > 0)
            cv::rectangle(canvas, clipped, bgr(255, 190, 0), 1, cv::LINE_AA);
    }

    // 4. Live crosshair colour hit marker.
    if (cfg.any_color_enabled && canvas.type() == CV_8UC3)
    {
        crosshair::CrosshairDetectorSettings settings;
        settings.enabled = true;
        settings.rect_w = cfg.crosshair_rect_w;
        settings.rect_h = cfg.crosshair_rect_h;
        settings.min_pixel_count = cfg.crosshair_min_pixel_count;
        settings.close_radius = cfg.crosshair_close_radius;
        settings.colors = cfg.crosshair_colors;

        static crosshair::CrosshairDetector detector;
        auto hit = detector.detect(canvas, settings);
        if (hit)
        {
            const cv::Point p(static_cast<int>(hit->x), static_cast<int>(hit->y));
            cv::circle(canvas, p, 5, bgr(0, 0, 0), 3, cv::LINE_AA);
            cv::circle(canvas, p, 5, bgr(60, 60, 255), 1, cv::LINE_AA);
            cv::line(canvas, cv::Point(p.x - 8, p.y), cv::Point(p.x + 8, p.y), bgr(60, 60, 255), 1, cv::LINE_AA);
            cv::line(canvas, cv::Point(p.x, p.y - 8), cv::Point(p.x, p.y + 8), bgr(60, 60, 255), 1, cv::LINE_AA);
        }
    }

    // 5. Top-left status banner with detection FPS / count.
    runtime::InferenceSession* session = g_inference_session;
    float infer_ms = 0.0f;
    if (session && session->detector())
    {
        infer_ms = static_cast<float>(
            session->detector()->lastPreprocessTime().count()
            + session->detector()->lastInferenceTime().count()
            + session->detector()->lastCopyTime().count()
            + session->detector()->lastPostprocessTime().count()
            + session->detector()->lastNmsTime().count());
    }

    char buf[96];
    std::snprintf(buf, sizeof(buf), "v%d  %d det  %.1fms",
                  version, static_cast<int>(boxes.size()), infer_ms);
    draw_text_with_bg(canvas, buf, cv::Point(6, 16), bgr(245, 245, 245), bgr(0, 0, 0));
}

void preview_loop()
{
    bool window_open = false;

    while (g_run.load())
    {
        const PreviewConfigSnapshot cfg = snapshot_config();

        if (!cfg.show_window)
        {
            if (window_open)
            {
                destroy_window_safe();
                window_open = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        // Toggle is on. If the window isn't currently visible (either never
        // opened, or the user just clicked the OS X to close it), recreate it.
        const bool visible_now = window_open && window_visible();
        if (!visible_now)
        {
            destroy_window_safe();
            try {
                cv::namedWindow(kWindowName, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
                cv::setWindowProperty(kWindowName, cv::WND_PROP_TOPMOST, 0);
                window_open = true;
            } catch (...) {
                window_open = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
        }

        cv::Mat frameCopy;
        {
            std::lock_guard<std::mutex> lk(frameMutex);
            if (!latestFrame.empty()) latestFrame.copyTo(frameCopy);
        }

        if (frameCopy.empty())
        {
            // No frame yet — show a small placeholder so the OS window keeps
            // its handle and `getWindowProperty` reports visible.
            const int dr = std::max(64, cfg.detection_resolution);
            cv::Mat placeholder(dr, dr, CV_8UC3, cv::Scalar(20, 20, 20));
            draw_text_with_bg(placeholder, "Waiting for capture...",
                              cv::Point(10, dr / 2),
                              bgr(220, 220, 220), bgr(0, 0, 0));
            cv::imshow(kWindowName, placeholder);
        }
        else
        {
            render_overlays(frameCopy, cfg);
            cv::imshow(kWindowName, frameCopy);
        }

        // pollKey pumps the OpenCV window's event loop. 1ms is enough to
        // process drag / resize / close events without blocking the loop.
        cv::pollKey();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (window_open)
        destroy_window_safe();
}
} // namespace

void PreviewWindow_Start()
{
    if (g_run.exchange(true))
        return;
    g_thread = std::thread(preview_loop);
}

void PreviewWindow_Stop()
{
    if (!g_run.exchange(false))
        return;
    if (g_thread.joinable())
        g_thread.join();
}
