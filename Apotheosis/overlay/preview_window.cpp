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
#include "crosshair/color_picker.h"
#include "crosshair/crosshair_detector.h"
#include "crosshair/flashlight_detector.h"
#include "crosshair/laser_detector.h"
#include "depth/depth_mask.h"
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

// Eyedropper ("取色") plumbing. The OpenCV mouse callback runs on THIS preview
// thread (HighGUI dispatches it from cv::pollKey below), so the cursor state is
// only ever touched here and needs no locking against the render loop. The
// clean frame, however, is what the callback samples: we stash a copy of each
// shown frame BEFORE overlays are drawn so a pick never reads the boxes / ROI
// lines painted on top. The arm flag and the picked result cross to the Qt
// thread via crosshair/color_picker (its own synchronisation).
std::mutex g_clean_mutex;
cv::Mat    g_clean_frame;          // last frame shown, sans overlays
int        g_pick_cursor_x = -1;   // preview-thread only
int        g_pick_cursor_y = -1;
bool       g_pick_cursor_inside = false;

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

    // Laser color-find (independent module). The ROI is always drawn (yellow);
    // when a beam is found the fitted line is overlaid.
    int    laser_rect_w = 0;
    int    laser_rect_h = 0;
    int    laser_center_x = 0;
    int    laser_center_y = 0;
    int    laser_min_pixel_count = 0;
    int    laser_close_radius = 0;
    float  laser_min_elongation = 3.0f;
    int    laser_target_center_x = 0;
    int    laser_target_center_y = 0;
    int    laser_target_rect_w = 0;
    int    laser_target_rect_h = 0;
    std::vector<crosshair::CrosshairColorBand> laser_colors;
    bool   any_laser_color_enabled = false;

    // Flashlight halo (debug preview). When `flashlight_show_preview` is on
    // the preview thread runs the detector against `canvas` directly and
    // overlays a yellow ring + centre cross + confidence label for every
    // matching halo (the mouse loop separately consumes the runtime-side
    // snapshot, so what's drawn here is purely diagnostic).
    bool   flashlight_show_preview = false;
    int    flashlight_brightness_threshold = 220;
    int    flashlight_min_radius = 5;
    int    flashlight_max_radius = 100;
    float  flashlight_min_circularity = 0.60f;
    int    flashlight_open_radius = 1;
    int    flashlight_min_local_contrast = 30;

    // Active (or fallback #0) hotkey FOV state.
    int    fov_base_x = 0;
    int    fov_base_y = 0;
    bool   dynamic_fov_enabled = false;
    bool   hotkey_active = false;

    // Depth heatmap overlay.
    bool   depth_inference_enabled = false;
    bool   depth_show_heatmap = false;
    bool   depth_show_bbox_distance = false;
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
    s.depth_inference_enabled  = config.depth_inference_enabled;
    s.depth_show_heatmap       = config.depth_show_heatmap;
    s.depth_show_bbox_distance = config.depth_show_bbox_distance;
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

    s.laser_rect_w          = config.laser_rect_w;
    s.laser_rect_h          = config.laser_rect_h;
    s.laser_center_x        = config.laser_center_x;
    s.laser_center_y        = config.laser_center_y;
    s.laser_min_pixel_count = config.laser_min_pixel_count;
    s.laser_close_radius    = config.laser_close_radius;
    s.laser_min_elongation  = config.laser_min_elongation;
    s.laser_target_center_x = config.laser_target_center_x;
    s.laser_target_center_y = config.laser_target_center_y;
    s.laser_target_rect_w   = config.laser_target_rect_w;
    s.laser_target_rect_h   = config.laser_target_rect_h;

    s.flashlight_show_preview         = config.flashlight_show_preview;
    s.flashlight_brightness_threshold = config.flashlight_brightness_threshold;
    s.flashlight_min_radius           = config.flashlight_min_radius;
    s.flashlight_max_radius           = config.flashlight_max_radius;
    s.flashlight_min_circularity      = config.flashlight_min_circularity;
    s.flashlight_open_radius          = config.flashlight_open_radius;
    s.flashlight_min_local_contrast   = config.flashlight_min_local_contrast;
    s.laser_colors.reserve(config.laser_colors.size());
    for (const auto& c : config.laser_colors)
    {
        crosshair::CrosshairColorBand b;
        b.name = c.name;
        b.enabled = c.enabled;
        b.h_low = c.h_low; b.h_high = c.h_high;
        b.s_min = c.s_min; b.s_max = c.s_max;
        b.v_min = c.v_min; b.v_max = c.v_max;
        s.any_laser_color_enabled = s.any_laser_color_enabled || b.enabled;
        s.laser_colors.push_back(std::move(b));
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

    // 0. Optional depth heatmap replacement. Done first so boxes/FOV/banner
    //    are drawn on top of it.
    if (cfg.depth_show_heatmap && cfg.depth_inference_enabled && canvas.type() == CV_8UC3)
    {
        cv::Mat heat = depth_anything::GetDepthMaskGenerator().getColormap();
        if (!heat.empty())
        {
            if (heat.size() != canvas.size())
                cv::resize(heat, heat, canvas.size(), 0, 0, cv::INTER_LINEAR);
            if (heat.type() == canvas.type())
                heat.copyTo(canvas);
        }
    }

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

    // Snapshot the latest normalized depth once for per-bbox sampling.
    cv::Mat depthForBbox;
    if (cfg.depth_show_bbox_distance && cfg.depth_inference_enabled)
    {
        depthForBbox = depth_anything::GetDepthMaskGenerator().getDepthNormalized();
    }

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const cv::Rect& r = boxes[i];
        const cv::Rect clipped = r & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() <= 0) continue;
        cv::rectangle(canvas, clipped, boxColor, 1, cv::LINE_AA);

        const int cls = (i < classes.size()) ? classes[i] : -1;
        char label[64];
        if (!depthForBbox.empty()
            && depthForBbox.type() == CV_8UC1
            && depthForBbox.size() == canvas.size())
        {
            // Sample depth at the box centre. 255 = nearest in frame, 0 =
            // farthest. Show as 0..1 with 1 = nearest so users intuit
            // "bigger = closer".
            const cv::Point pc(clipped.x + clipped.width / 2,
                               clipped.y + clipped.height / 2);
            const uint8_t d = depthForBbox.at<uint8_t>(pc);
            std::snprintf(label, sizeof(label), "#%d  d=%.2f", cls, d / 255.0f);
        }
        else
        {
            std::snprintf(label, sizeof(label), "#%d", cls);
        }
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
    // Bottom-edge midpoint anchored at the frame centre (square sits above
    // the centre line). Must match clipped_center_roi() in
    // crosshair/crosshair_detector.cpp so the preview matches detection.
    if (cfg.crosshair_rect_w > 0 && cfg.crosshair_rect_h > 0)
    {
        const int rw = std::max(4, cfg.crosshair_rect_w);
        const int rh = std::max(4, cfg.crosshair_rect_h);
        constexpr int kVerticalOffset = 10;
        const cv::Rect roi(canvas.cols / 2 - rw / 2, canvas.rows / 2 - rh + kVerticalOffset, rw, rh);
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

    // 4b. Laser color-find ROI (YELLOW, always drawn so the user can see /
    //     position the detection region) + the detected beam line on top.
    if (cfg.laser_rect_w > 0 && cfg.laser_rect_h > 0)
    {
        const cv::Scalar laserCol = bgr(0, 255, 255); // yellow
        const int rw = std::max(4, cfg.laser_rect_w);
        const int rh = std::max(4, cfg.laser_rect_h);
        const cv::Rect roi(cfg.laser_center_x - rw / 2, cfg.laser_center_y - rh / 2, rw, rh);
        const cv::Rect clipped = roi & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() > 0)
        {
            cv::rectangle(canvas, clipped, laserCol, 1, cv::LINE_AA);
            draw_text_with_bg(canvas, "Laser ROI",
                              cv::Point(clipped.x, std::max(12, clipped.y)),
                              laserCol, bgr(0, 0, 0));
        }

        // Target box (BROWN): where the fitted line is projected to estimate
        // the endpoint. Always drawn so the user can position it near centre.
        if (cfg.laser_target_rect_w > 0 && cfg.laser_target_rect_h > 0)
        {
            const cv::Scalar brown = bgr(40, 100, 170); // B,G,R -> brown/orange
            const int tw = std::max(4, cfg.laser_target_rect_w);
            const int th = std::max(4, cfg.laser_target_rect_h);
            const cv::Rect troi(cfg.laser_target_center_x - tw / 2,
                                cfg.laser_target_center_y - th / 2, tw, th);
            const cv::Rect tclip = troi & cv::Rect(0, 0, canvas.cols, canvas.rows);
            if (tclip.area() > 0)
            {
                cv::rectangle(canvas, tclip, brown, 1, cv::LINE_AA);
                draw_text_with_bg(canvas, "Laser tip zone",
                                  cv::Point(tclip.x, std::max(12, tclip.y)),
                                  brown, bgr(0, 0, 0));
            }
        }

        // Detected beam: overlay a yellow line on the laser, dot at the tip.
        if (cfg.any_laser_color_enabled && canvas.type() == CV_8UC3)
        {
            crosshair::LaserDetectorSettings ls;
            ls.enabled = true;
            ls.rect_w = cfg.laser_rect_w;
            ls.rect_h = cfg.laser_rect_h;
            ls.center_x = cfg.laser_center_x;
            ls.center_y = cfg.laser_center_y;
            ls.min_pixel_count = cfg.laser_min_pixel_count;
            ls.close_radius = cfg.laser_close_radius;
            ls.min_elongation = cfg.laser_min_elongation;
            ls.target_center_x = cfg.laser_target_center_x;
            ls.target_center_y = cfg.laser_target_center_y;
            ls.target_rect_w = cfg.laser_target_rect_w;
            ls.target_rect_h = cfg.laser_target_rect_h;
            ls.colors = cfg.laser_colors;

            static crosshair::LaserDetector laser_detector;
            const crosshair::LaserResult lr = laser_detector.detectLine(canvas, ls);
            if (lr.found)
            {
                const cv::Point m(static_cast<int>(lr.muzzle.x), static_cast<int>(lr.muzzle.y));
                const cv::Point t(static_cast<int>(lr.tip.x), static_cast<int>(lr.tip.y));
                // Beam line (muzzle -> extended tip), dark underlay for contrast.
                cv::line(canvas, m, t, bgr(0, 0, 0), 4, cv::LINE_AA);
                cv::line(canvas, m, t, laserCol, 2, cv::LINE_AA);
                // Tip marker.
                cv::circle(canvas, t, 5, bgr(0, 0, 0), 3, cv::LINE_AA);
                cv::circle(canvas, t, 5, laserCol, 1, cv::LINE_AA);
            }
        }
    }

    // 4c. Flashlight halo preview. Independent of crosshair / laser / hotkey
    //     gates — when the user enables it on the Flashlight page we draw
    //     every bright contour the detector evaluated, green when accepted,
    //     red when rejected with the reason printed. This is purely visual
    //     diagnostics; the runtime path consumes its own snapshot.
    if (cfg.flashlight_show_preview && canvas.type() == CV_8UC3)
    {
        crosshair::FlashlightDetectorSettings fs;
        fs.enabled              = true;
        fs.brightness_threshold = cfg.flashlight_brightness_threshold;
        fs.min_radius           = cfg.flashlight_min_radius;
        fs.max_radius           = cfg.flashlight_max_radius;
        fs.min_circularity      = cfg.flashlight_min_circularity;
        fs.open_radius          = cfg.flashlight_open_radius;
        fs.min_local_contrast   = cfg.flashlight_min_local_contrast;

        static crosshair::FlashlightDetector flashlight_detector;
        const auto cands = flashlight_detector.detectVerbose(canvas, fs);

        const cv::Scalar ringOk    = bgr(0, 255, 0);     // green = accepted
        const cv::Scalar ringBad   = bgr(60, 60, 255);   // red   = rejected
        const cv::Scalar centerCol = bgr(255, 255, 255);
        for (const auto& cd : cands)
        {
            const cv::Point c(static_cast<int>(std::lround(cd.center.x)),
                              static_cast<int>(std::lround(cd.center.y)));
            const int r = std::max(2, static_cast<int>(std::lround(cd.radius)));
            const cv::Scalar col = cd.accepted ? ringOk : ringBad;

            // Dark underlay + coloured ring on top.
            cv::circle(canvas, c, r, bgr(0, 0, 0), 3, cv::LINE_AA);
            cv::circle(canvas, c, r, col,          1, cv::LINE_AA);

            // Centre cross (4 px arms) to mark the picked centroid.
            cv::line(canvas, cv::Point(c.x - 4, c.y), cv::Point(c.x + 4, c.y), centerCol, 1, cv::LINE_AA);
            cv::line(canvas, cv::Point(c.x, c.y - 4), cv::Point(c.x, c.y + 4), centerCol, 1, cv::LINE_AA);

            // Label: "circ r=12" when accepted, "r=12 deformed" etc. when not.
            char lbuf[48];
            if (cd.accepted)
                std::snprintf(lbuf, sizeof(lbuf), "OK c=%.2f r=%d", cd.circularity, r);
            else
                std::snprintf(lbuf, sizeof(lbuf), "%s c=%.2f r=%d",
                              cd.reject_reason ? cd.reject_reason : "rejected",
                              cd.circularity, r);
            const cv::Point label_org(c.x + r + 4, std::max(12, c.y - r));
            draw_text_with_bg(canvas, lbuf, label_org, col, bgr(0, 0, 0));
        }
    }

    // 5. Top-left status banner with inference FPS + latency.
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

    static int    s_last_version  = -1;
    static int    s_frames_seen   = 0;
    static auto   s_window_start  = std::chrono::steady_clock::now();
    static float  s_inference_fps = 0.0f;
    const auto now = std::chrono::steady_clock::now();
    if (version != s_last_version)
    {
        if (s_last_version >= 0)
            s_frames_seen += std::max(0, version - s_last_version);
        s_last_version = version;
    }
    const double elapsed_s =
        std::chrono::duration<double>(now - s_window_start).count();
    if (elapsed_s >= 0.5)
    {
        s_inference_fps = static_cast<float>(s_frames_seen / elapsed_s);
        s_frames_seen = 0;
        s_window_start = now;
    }

    float displayed_fps = s_inference_fps;
    if (displayed_fps <= 0.01f && infer_ms > 0.01f)
        displayed_fps = 1000.0f / infer_ms;

    char buf[96];
    std::snprintf(buf, sizeof(buf), "Infer %.1f FPS | Lat %.1f ms",
                  displayed_fps, infer_ms);
    draw_text_with_bg(canvas, buf, cv::Point(6, 16), bgr(245, 245, 245), bgr(0, 0, 0));
}

// Mouse callback for the preview window. HighGUI runs this on the preview
// thread during cv::pollKey. Tracks the cursor (to draw the sample marker) and,
// when the eyedropper is armed, turns a left click into an HSV sample taken
// from the stashed clean frame. Right click cancels. WINDOW_NORMAL already maps
// (x, y) into image-pixel space, so no manual de-scaling is needed.
void on_mouse(int event, int x, int y, int /*flags*/, void* /*userdata*/)
{
    if (event == cv::EVENT_MOUSEMOVE)
    {
        g_pick_cursor_x = x;
        g_pick_cursor_y = y;
        g_pick_cursor_inside = true;
        return;
    }

    if (!crosshair::IsColorPickArmed())
        return;

    if (event == cv::EVENT_RBUTTONDOWN)
    {
        crosshair::CancelColorPick();
        return;
    }

    if (event == cv::EVENT_LBUTTONDOWN)
    {
        cv::Mat clean;
        {
            std::lock_guard<std::mutex> lk(g_clean_mutex);
            if (!g_clean_frame.empty()) g_clean_frame.copyTo(clean);
        }
        int h = 0, s = 0, v = 0;
        if (crosshair::SampleRegionHSV(clean, x, y, crosshair::kPickHalf, h, s, v))
            crosshair::SubmitPickedColor(h, s, v);
        // If sampling failed (no frame yet) stay armed so the next click retries.
    }
}

// Draw the eyedropper marker: a translucent disc + crisp ring at the cursor and
// the exact 5x5 sample footprint, plus a one-line hint. No-op unless armed.
// ASCII only — OpenCV putText can't render CJK (the Chinese UI lives in Qt).
void draw_pick_overlay(cv::Mat& canvas)
{
    if (canvas.empty() || canvas.type() != CV_8UC3) return;
    if (!crosshair::IsColorPickArmed()) return;

    draw_text_with_bg(canvas, "PICK: click=sample  right-click=cancel",
                      cv::Point(6, canvas.rows - 8), bgr(245, 245, 245), bgr(0, 0, 0));

    if (!g_pick_cursor_inside) return;

    const int cx = g_pick_cursor_x;
    const int cy = g_pick_cursor_y;
    const int half = crosshair::kPickHalf;
    const int ringR = half + 4;

    // Translucent cyan disc so the spot under the cursor is obvious without
    // hiding the pixels being sampled.
    cv::Rect rr(cx - ringR, cy - ringR, 2 * ringR + 1, 2 * ringR + 1);
    rr &= cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (rr.area() > 0)
    {
        cv::Mat patch = canvas(rr).clone();
        cv::circle(patch, cv::Point(cx - rr.x, cy - rr.y), ringR,
                   bgr(0, 220, 255), cv::FILLED, cv::LINE_AA);
        cv::addWeighted(patch, 0.25, canvas(rr), 0.75, 0.0, canvas(rr));
    }

    cv::circle(canvas, cv::Point(cx, cy), ringR, bgr(0, 0, 0), 2, cv::LINE_AA);
    cv::circle(canvas, cv::Point(cx, cy), ringR, bgr(0, 220, 255), 1, cv::LINE_AA);

    // Exact sample footprint (5x5).
    cv::Rect foot(cx - half, cy - half, 2 * half + 1, 2 * half + 1);
    foot &= cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (foot.area() > 0)
        cv::rectangle(canvas, foot, bgr(255, 255, 255), 1, cv::LINE_AA);
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
                // (Re)attach the eyedropper callback: it's lost whenever the
                // window is destroyed/recreated (e.g. user closed the OS X).
                cv::setMouseCallback(kWindowName, on_mouse, nullptr);
                g_pick_cursor_inside = false;
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
            // Stash a clean copy for the eyedropper BEFORE overlays are drawn,
            // so a colour pick samples the real frame, not the boxes/ROI lines.
            {
                std::lock_guard<std::mutex> lk(g_clean_mutex);
                frameCopy.copyTo(g_clean_frame);
            }
            render_overlays(frameCopy, cfg);
            draw_pick_overlay(frameCopy);
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
