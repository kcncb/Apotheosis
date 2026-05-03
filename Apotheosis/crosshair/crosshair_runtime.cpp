#include "crosshair_runtime.h"

#include <mutex>

#include "Apotheosis.h"
#include "config.h"

namespace crosshair_runtime
{

namespace
{

std::mutex g_mtx;
PivotSnapshot g_snap{};

crosshair::CrosshairDetector g_detector;

} // namespace

PivotSnapshot read()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void publish(const PivotSnapshot& snap)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_snap = snap;
}

void process_frame(const cv::Mat& bgrFrame)
{
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        publish(PivotSnapshot{});
        return;
    }

    crosshair::CrosshairDetectorSettings settings;
    bool any_hotkey_opt_in = false;
    bool any_color_enabled = false;

    {
        std::lock_guard<std::recursive_mutex> cfg(configMutex);
        for (const auto& hk : config.hotkeys)
        {
            if (hk.crosshair_detect_enabled)
            {
                any_hotkey_opt_in = true;
                break;
            }
        }
        if (!any_hotkey_opt_in)
        {
            // Cheap exit; no detector cost when nobody opted in.
            publish(PivotSnapshot{});
            return;
        }

        settings.enabled         = true;
        settings.rect_w          = config.crosshair_rect_w;
        settings.rect_h          = config.crosshair_rect_h;
        settings.min_pixel_count = config.crosshair_min_pixel_count;
        settings.close_radius    = config.crosshair_close_radius;
        settings.colors.reserve(config.crosshair_colors.size());
        for (const auto& c : config.crosshair_colors)
        {
            crosshair::CrosshairColorBand b;
            b.name    = c.name;
            b.enabled = c.enabled;
            b.h_low   = c.h_low;
            b.h_high  = c.h_high;
            b.s_min   = c.s_min;
            b.s_max   = c.s_max;
            b.v_min   = c.v_min;
            b.v_max   = c.v_max;
            any_color_enabled = any_color_enabled || b.enabled;
            settings.colors.push_back(std::move(b));
        }
    }

    if (!any_color_enabled)
    {
        publish(PivotSnapshot{});
        return;
    }

    auto hit = g_detector.detect(bgrFrame, settings);
    PivotSnapshot snap;
    snap.ts = std::chrono::steady_clock::now();
    if (hit)
    {
        snap.x = static_cast<double>(hit->x);
        snap.y = static_cast<double>(hit->y);
        snap.valid = true;
    }
    else
    {
        snap.valid = false;
    }
    publish(snap);
}

} // namespace crosshair_runtime
