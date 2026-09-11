#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
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
#include "crosshair/crosshair_runtime.h"
#include "detection_buffer.h"
#include "i_detector.h"
#include "preview_window.h"
#include "runtime/active_hotkey.h"
#include "runtime/inference_session.h"
#include "runtime/aim_telemetry.h"
#include "runtime/latency_probe.h"

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
    // 当前热键是否勾选了准星找色。用来区分"没命中"和"压根没开/没色带"。
    bool   crosshair_hotkey_enabled = false;

    // Active (or fallback #0) hotkey FOV state.
    int    fov_base_x = 0;
    int    fov_base_y = 0;
    bool   dynamic_fov_enabled = false;
    bool   hotkey_active = false;
    bool   show_fps = false;
    float  replay_playback_speed = 0.25f;
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

    s.show_fps                     = config.show_fps;
    s.replay_playback_speed        = config.replay_playback_speed;

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
        s.crosshair_hotkey_enabled = hk.crosshair_detect_enabled;
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
        char label[64];
        std::snprintf(label, sizeof(label), "#%d", cls);
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
    // the centre line). Must match the runtime ROI in
    // crosshair/crosshair_runtime.cpp::process_gpu_frame
    // (roi_y = rows/2 - roi_h + 10) — that is the window the live finder
    // actually searches; a frame drawn from any other formula would lie to
    // whoever is tuning the ROI.
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

    // 4. 运行期准星找色结果 —— 直接读 crosshair_runtime 发布的那份 pivot。
    //
    // 这里必须显示运行期快照, 不能在本线程里另跑一次 CPU 检测器: 实战走的是
    // GPU 路径 (cpu_path_active() 恒为 false), 而 CPU 检测器的 ROI
    // (crosshair_detector.cpp dynamic_center_roi, cy - 0.6*h) 和接受窗口都跟
    // 运行期不一样, 画出来的命中点会"看着生效、实际无效"。判断压枪有没有参考
    // 点, 只看这一行。
    // 注: 预览画布用 Hershey 字体, 渲染不了中文, 所以这一行保持英文 —— 与上方
    // "Infer FPS | Lat" 一栏的既有约定一致; 中文文案在 Qt/ImGui 面板里。
    {
        const auto snap = crosshair_runtime::read();
        const auto ref  = crosshair_runtime::read_static_ref();
        const auto ref_age_ms = ref.valid
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - ref.ts).count()
            : -1;
        // 参考点只在新鲜时展示: 松开瞄准热键后它会停在上次的值, 画出来会误导。
        const bool ref_fresh = ref_age_ms >= 0 && ref_age_ms <= 250;
        const long long age_ms = snap.valid
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - snap.ts).count()
            : -1;

        char ref_text[48];
        if (ref_fresh)
            std::snprintf(ref_text, sizeof(ref_text), "ref (%d,%d)",
                          static_cast<int>(std::lround(ref.x)),
                          static_cast<int>(std::lround(ref.y)));
        else
            std::snprintf(ref_text, sizeof(ref_text), "ref -");

        char line[200];
        if (!cfg.any_color_enabled || !cfg.crosshair_hotkey_enabled)
        {
            std::snprintf(line, sizeof(line), "Xhair: OFF (hotkey/palette off)");
        }
        else if (snap.valid)
        {
            // 原始命中点 + 真正进控制环的静态参考点一起显示: 参考点乱跳就是
            // 控制环被灌了抖动, 参考点稳而命中点跳说明限速/惯性在起作用。
            std::snprintf(line, sizeof(line),
                          "Xhair: HIT (%d,%d) age=%lldms%s | %s",
                          static_cast<int>(std::lround(snap.x)),
                          static_cast<int>(std::lround(snap.y)),
                          age_ms,
                          (age_ms > crosshair_runtime::kFreshnessMs) ? " STALE" : "",
                          ref_text);
        }
        else
        {
            std::snprintf(line, sizeof(line), "Xhair: MISS | %s", ref_text);
        }

        const cv::Scalar col = snap.valid ? bgr(80, 255, 80) : bgr(150, 150, 150);
        draw_text_with_bg(canvas, line, cv::Point(6, canvas.rows - 6),
                          bgr(245, 245, 245), bgr(0, 0, 0));

        if (snap.valid)
        {
            const cv::Point p(static_cast<int>(std::lround(snap.x)),
                              static_cast<int>(std::lround(snap.y)));
            cv::drawMarker(canvas, p, col, cv::MARKER_CROSS, 14, 1, cv::LINE_AA);
            cv::circle(canvas, p, 6, col, 1, cv::LINE_AA);
        }

        // 静态参考点 = 找色对瞄准环的唯一输出, 用方块表示。
        if (ref_fresh)
        {
            const cv::Point r(static_cast<int>(std::lround(ref.x)),
                              static_cast<int>(std::lround(ref.y)));
            cv::rectangle(canvas, cv::Rect(r.x - 7, r.y - 7, 15, 15),
                          bgr(255, 120, 240), 1, cv::LINE_AA);
        }

        // 画面几何中心 = 找色关闭/参考点回收后回落的位置。两个标记重合就说明
        // 找色当前没有提供任何有效参考(等于没压枪), 分开才是真的在跟随准星。
        cv::drawMarker(canvas, cv::Point(canvas.cols / 2, canvas.rows / 2),
                       bgr(0, 200, 255), cv::MARKER_TILTED_CROSS, 10, 1, cv::LINE_AA);
    }


    // 5. Optional top-left status banner with inference FPS + latency.
    if (!cfg.show_fps)
        return;
    const auto timing = runtime::latency::snapshot();
    const float infer_ms = static_cast<float>(timing.engine_inference_ms);

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

    // 端到端延迟分解面板。
    //
    // E2E pairs the sample callback timestamp with this command's completed
    // driver send. No send sample means unavailable, not a zero-latency write.
    // It does not acknowledge physical HID execution or the display response.
    int y = 34;
    for (const auto& line : runtime::latency::formatLinesAscii(true))
    {
        draw_text_with_bg(canvas, line, cv::Point(6, y),
                          bgr(120, 255, 160), bgr(0, 0, 0));
        y += 16;
    }
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
        if (crosshair::SampleRegionHSV(clean, x, y, crosshair::PickHalf(), h, s, v))
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
    const int half = crosshair::PickHalf();
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

    // Exact sample footprint selected by the active picker.
    cv::Rect foot(cx - half, cy - half, 2 * half + 1, 2 * half + 1);
    foot &= cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (foot.area() > 0)
        cv::rectangle(canvas, foot, bgr(255, 255, 255), 1, cv::LINE_AA);
}

void render_replay_frame(cv::Mat& canvas,
                         const std::vector<runtime::ReplayFrame>& frames,
                         size_t frame_index,
                         float playback_speed)
{
    if (canvas.empty() || frames.empty()) return;
    frame_index = std::min(frame_index, frames.size() - 1);
    const auto& frame = frames[frame_index];

    for (size_t i = 0; i < frame.boxes.size(); ++i)
    {
        const cv::Rect clipped = frame.boxes[i] & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (clipped.area() <= 0) continue;
        const int track_id = i < frame.track_ids.size() ? frame.track_ids[i] : -1;
        const bool locked = track_id >= 0 && track_id == frame.locked_track_id;
        const cv::Scalar color = locked ? bgr(70, 90, 255) : bgr(110, 220, 80);
        cv::rectangle(canvas, clipped, color, locked ? 3 : 1, cv::LINE_AA);
        const int cls = i < frame.class_ids.size() ? frame.class_ids[i] : -1;
        char label[64];
        std::snprintf(label, sizeof(label), locked ? "LOCK #%d" : "#%d", cls);
        draw_text_with_bg(canvas, label, cv::Point(clipped.x, std::max(12, clipped.y)),
                          bgr(250, 245, 240), bgr(0, 0, 0));
    }

    std::vector<cv::Point> trail;
    const size_t first = frame_index > 90 ? frame_index - 90 : 0;
    trail.reserve(frame_index - first + 1);
    for (size_t i = first; i <= frame_index; ++i)
    {
        if (frames[i].locked_track_id >= 0)
            trail.emplace_back(static_cast<int>(std::lround(frames[i].pivot_x)),
                               static_cast<int>(std::lround(frames[i].pivot_y)));
    }
    if (trail.size() >= 2)
        cv::polylines(canvas, trail, false, bgr(255, 190, 70), 2, cv::LINE_AA);
    if (!trail.empty())
    {
        const cv::Point p = trail.back();
        cv::circle(canvas, p, 5, bgr(0, 0, 0), 3, cv::LINE_AA);
        cv::circle(canvas, p, 5, bgr(255, 220, 80), 1, cv::LINE_AA);
    }

    char banner[128];
    std::snprintf(banner, sizeof(banner), "REPLAY %.2fx | %zu / %zu",
                  playback_speed, frame_index + 1, frames.size());
    draw_text_with_bg(canvas, banner, cv::Point(6, 18),
                      bgr(245, 245, 245), bgr(0, 0, 0));
}

void preview_loop()
{
    bool window_open = false;
    bool replay_was_active = false;
    std::vector<runtime::ReplayFrame> replay_frames;
    size_t replay_index = 0;
    auto replay_next_tick = std::chrono::steady_clock::now();

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

        bool replay_active = g_replay_playback_active.load();
        if (replay_active && !replay_was_active)
        {
            replay_frames = runtime::ReplayBuffer::instance().snapshot();
            replay_index = 0;
            replay_next_tick = std::chrono::steady_clock::now();
            if (replay_frames.empty())
            {
                replay_active = false;
                g_replay_playback_active.store(false);
            }
        }
        replay_was_active = replay_active;

        if (replay_active && !replay_frames.empty())
        {
            const float speed = std::clamp(cfg.replay_playback_speed, 0.05f, 2.0f);
            const auto now = std::chrono::steady_clock::now();
            while (replay_index + 1 < replay_frames.size() && now >= replay_next_tick)
            {
                const auto source_delta = std::chrono::duration_cast<std::chrono::microseconds>(
                    replay_frames[replay_index + 1].ts - replay_frames[replay_index].ts);
                const auto scaled_us = std::clamp<long long>(
                    static_cast<long long>(source_delta.count() / speed), 1000, 500000);
                replay_next_tick = now + std::chrono::microseconds(scaled_us);
                ++replay_index;
            }
            g_replay_playback_frame.store(static_cast<int>(replay_index));

            const int dr = std::max(64, cfg.detection_resolution);
            cv::Mat replayCanvas(dr, dr, CV_8UC3, cv::Scalar(18, 20, 24));
            render_replay_frame(replayCanvas, replay_frames, replay_index, speed);
            cv::imshow(kWindowName, replayCanvas);

            if (replay_index + 1 >= replay_frames.size() && now >= replay_next_tick)
            {
                g_replay_playback_active.store(false);
                replay_was_active = false;
            }
            cv::pollKey();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
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
