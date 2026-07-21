#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "capture/auto_capture.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "Apotheosis.h"
#include "capture/capture.h"
#include "config/config.h"
#include "crosshair/flashlight_runtime.h"
#include "detector/detection_buffer.h"
#include "keyboard/keyboard_listener.h"

extern std::atomic<bool> shouldExit;

namespace AutoCapture
{

std::atomic<int>  g_saved_total{0};
std::atomic<int>  g_saved_session{0};
std::atomic<bool> g_force_held{false};
std::atomic<bool> g_running{false};

void reset_session_counter()
{
    g_saved_session.store(0);
}

namespace
{

// Snapshot the configuration once per tick under configMutex so the UI
// can edit live without us tearing.
struct CfgSnap
{
    bool  enabled = false;
    bool  use_high = true;
    float high_conf = 0.85f;
    bool  use_low = false;
    float low_conf = 0.30f;
    bool  any_detection = false;   // 忽略阈值,只要有 YOLO 检测就采集
    bool  use_flashlight = false;  // 寻光命中触发
    int   cooldown_ms = 200;
    std::vector<std::string> force_keys;
    std::string out_dir = "screenshots/auto";
    bool  save_label = true;
    int   det_resolution = 320;
};

CfgSnap snapshot_cfg()
{
    CfgSnap s;
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    s.enabled       = config.auto_capture_enabled;
    s.use_high      = config.auto_capture_use_high;
    s.high_conf     = config.auto_capture_high_conf;
    s.use_low       = config.auto_capture_use_low;
    s.low_conf      = config.auto_capture_low_conf;
    s.any_detection = config.auto_capture_any_detection;
    s.use_flashlight= config.auto_capture_use_flashlight;
    s.cooldown_ms   = std::max(0, config.auto_capture_cooldown_ms);
    s.force_keys    = config.auto_capture_force_keys;
    s.out_dir       = config.auto_capture_output_dir.empty()
                          ? std::string("screenshots/auto")
                          : config.auto_capture_output_dir;
    s.save_label    = config.auto_capture_save_label;
    s.det_resolution = std::max(32, config.detection_resolution);
    return s;
}

std::string make_filename_stem()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "auto_%04d%02d%02d_%02d%02d%02d_%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));
    return buf;
}

bool any_in_zone(const std::vector<float>& confs,
                 bool use_high, float high,
                 bool use_low,  float low)
{
    if (!use_high && !use_low) return false;
    for (const float c : confs)
    {
        if (use_high && c >= high) return true;
        if (use_low  && c <= low && c > 0.0f) return true;
    }
    return false;
}

void write_yolo_label(const std::string& path,
                      const std::vector<cv::Rect>& boxes,
                      const std::vector<int>& classes,
                      double frame_w, double frame_h)
{
    if (boxes.empty() || classes.empty() || frame_w <= 0 || frame_h <= 0)
        return;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "w") != 0 || !fp) return;

    const size_t n = std::min(boxes.size(), classes.size());
    for (size_t i = 0; i < n; ++i)
    {
        const cv::Rect& b = boxes[i];
        if (b.width <= 0 || b.height <= 0) continue;
        const double xc = (b.x + b.width  * 0.5) / frame_w;
        const double yc = (b.y + b.height * 0.5) / frame_h;
        const double w  =  b.width  / frame_w;
        const double h  =  b.height / frame_h;
        std::fprintf(fp, "%d %.6f %.6f %.6f %.6f\n",
                     classes[i], xc, yc, w, h);
    }
    std::fclose(fp);
}

} // namespace

void auto_capture_thread()
{
    g_running.store(true);
    g_saved_session.store(0);

    int last_version = -1;
    auto last_save_ts = std::chrono::steady_clock::time_point::min();

    // NOTE: not gated on session_stop_requested — auto-capture must survive
    // inference start/stop, only the process-wide shouldExit ends it.
    while (!shouldExit.load())
    {
        // Wait for a fresh detection (same idiom as mouse_thread_loop).
        bool fresh = false;
        {
            std::unique_lock<std::mutex> lk(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lk, std::chrono::milliseconds(50),
                [&] { return detectionBuffer.version > last_version
                              || shouldExit.load(); });
            if (shouldExit.load()) break;
            if (detectionBuffer.version > last_version)
            {
                last_version = detectionBuffer.version;
                fresh = true;
            }
        }
        if (!fresh) continue;

        const CfgSnap cfg = snapshot_cfg();
        if (!cfg.enabled) continue;

        // Force-key state (mouse side button typically).
        const bool force_held = isAnyKeyPressed(cfg.force_keys)
                                && !cfg.force_keys.empty();
        g_force_held.store(force_held);

        // Pull detections + the latest CPU frame.
        std::vector<cv::Rect> boxes;
        std::vector<int> classes;
        std::vector<float> confidences;
        int v = -1;
        detectionBuffer.get(boxes, classes, confidences, v);

        // 寻光触发源:独立于 YOLO,只看 flashlight_runtime 是否本轮命中。
        bool flashlight_hit = false;
        if (cfg.use_flashlight)
        {
            const auto snap = flashlight_runtime::read();
            if (snap.valid && !snap.spots.empty())
            {
                // 新鲜度:snap 时间戳与 detection publish 时间同一量级 →
                // 用 kFreshnessMs 判定即可。
                const auto ageMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - snap.ts).count();
                if (ageMs <= flashlight_runtime::kFreshnessMs)
                    flashlight_hit = true;
            }
        }

        // No detections AND no force-hold AND no flashlight → nothing to record.
        if (boxes.empty() && !force_held && !flashlight_hit) continue;

        // Decide save.
        bool should_save = false;
        if (force_held)
            should_save = true;
        else if (flashlight_hit)
            should_save = true;
        else if (!boxes.empty())
        {
            if (cfg.any_detection)
                should_save = true;
            else
                should_save = any_in_zone(confidences,
                                          cfg.use_high, cfg.high_conf,
                                          cfg.use_low,  cfg.low_conf);
        }
        if (!should_save) continue;

        // Cooldown gate.
        const auto now = std::chrono::steady_clock::now();
        const double since_ms = std::chrono::duration<double, std::milli>(
            now - last_save_ts).count();
        if (last_save_ts != std::chrono::steady_clock::time_point::min()
            && since_ms < cfg.cooldown_ms)
        {
            continue;
        }

        // Snapshot the most recent BGR detection-resolution frame.
        cv::Mat frame;
        {
            std::lock_guard<std::mutex> lk(frameMutex);
            if (!latestFrame.empty())
                frame = latestFrame.clone();
        }
        if (frame.empty()) continue;

        // Ensure output dir exists.
        std::error_code ec;
        std::filesystem::create_directories(cfg.out_dir, ec);

        const std::string stem = make_filename_stem();
        const std::string img_path = cfg.out_dir + "/" + stem + ".jpg";
        std::vector<int> jpg_params = { cv::IMWRITE_JPEG_QUALITY, 92 };
        if (!cv::imwrite(img_path, frame, jpg_params))
            continue;

        if (cfg.save_label)
        {
            const std::string lbl_path = cfg.out_dir + "/" + stem + ".txt";
            write_yolo_label(lbl_path, boxes, classes,
                             static_cast<double>(frame.cols),
                             static_cast<double>(frame.rows));
        }

        last_save_ts = now;
        g_saved_total.fetch_add(1);
        g_saved_session.fetch_add(1);
    }

    g_running.store(false);
    g_force_held.store(false);
}

} // namespace AutoCapture
