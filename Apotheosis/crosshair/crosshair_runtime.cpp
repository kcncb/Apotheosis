#include "crosshair_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

#include "Apotheosis.h"
#include "config.h"
#include "flashlight_runtime.h"
#include "laser_detector.h"
#include "runtime/active_hotkey.h"

namespace crosshair_runtime
{

namespace
{

std::mutex g_mtx;
PivotSnapshot g_snap{};

crosshair::CrosshairDetector  g_detector;
crosshair::LaserDetector      g_laser_detector;

// --- Anti-jitter: adaptive (One-Euro) low-pass on the pivot --------------
// Heavy smoothing when the point is nearly still (kills detection jitter),
// light smoothing when it moves fast (so flicks / recoil stay responsive and
// un-laggy). Stateful; only touched on the capture thread inside process_frame.
struct LowPass
{
    double y = 0.0;
    bool   init = false;
    double filter(double x, double a)
    {
        if (!init) { y = x; init = true; }
        else       { y = a * x + (1.0 - a) * y; }
        return y;
    }
    void reset() { init = false; }
};

struct OneEuro2D
{
    double mincutoff = 4.0; // Hz; lower = smoother when still
    double beta = 0.04;     // responsiveness vs speed
    double dcutoff = 1.0;
    LowPass xf, yf, dxf, dyf;
    double lastT = -1.0;

    static double alpha(double cutoff, double dt)
    {
        constexpr double kPi = 3.14159265358979323846;
        const double tau = 1.0 / (2.0 * kPi * cutoff);
        return 1.0 / (1.0 + tau / dt);
    }

    // Map smoothing strength s in (0,1] to a min-cutoff: more s => lower
    // cutoff => stronger steady-state smoothing.
    void configure(double s)
    {
        mincutoff = std::max(0.5, (1.0 - s) * 8.0);
        beta = 0.04;
        dcutoff = 1.0;
    }

    cv::Point2f filter(cv::Point2f p, double t)
    {
        if (lastT < 0.0)
        {
            lastT = t;
            xf.filter(p.x, 1.0);
            yf.filter(p.y, 1.0);
            return p;
        }
        double dt = t - lastT;
        if (!(dt > 1e-5)) dt = 1.0 / 240.0;
        lastT = t;

        const double dx = (p.x - xf.y) / dt;
        const double dy = (p.y - yf.y) / dt;
        const double edx = dxf.filter(dx, alpha(dcutoff, dt));
        const double edy = dyf.filter(dy, alpha(dcutoff, dt));
        const double speed = std::hypot(edx, edy);
        const double cutoff = mincutoff + beta * speed;
        const double a = alpha(cutoff, dt);
        return cv::Point2f(static_cast<float>(xf.filter(p.x, a)),
                           static_cast<float>(yf.filter(p.y, a)));
    }
    void reset() { xf.reset(); yf.reset(); dxf.reset(); dyf.reset(); lastT = -1.0; }
};

OneEuro2D g_cross_filter;
OneEuro2D g_laser_filter;

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
    // Flashlight halo is a SEPARATE pipeline (virtual target injection, not
    // crosshair-pivot publication). Drive it from the same capture hook so
    // we don't pay for a second per-frame ROI grab — the work itself is
    // independent and lives in its own snapshot.
    flashlight_runtime::process_frame(bgrFrame);

    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3)
    {
        publish(PivotSnapshot{});
        return;
    }

    // Only search for the reticle colour while the user is actively aiming:
    // a hotkey must be held AND that specific hotkey must have opted into
    // crosshair-colour AND there must be at least one detected target. This
    // keeps the detector cost off the capture thread the rest of the time.
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0)
    {
        // No aim hotkey held.
        publish(PivotSnapshot{});
        return;
    }

    {
        // Cheap target-presence gate: skip the detector when nothing is on
        // screen to aim at.
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        if (detectionBuffer.boxes.empty())
        {
            publish(PivotSnapshot{});
            return;
        }

        // Detection-freshness gate. This runs on the capture thread against the
        // LIVE frame, but detectionBuffer reflects whatever the detector last
        // finished, which lags by the inference time. When inference stalls
        // (a slow frame, GPU busy with depth), the buffer keeps reporting a
        // target that has already left the live frame. Emitting a pivot then
        // lets the mouse loop's crosshair-feedback re-push (Path B) keep
        // driving toward that departed target, oscillating around it — the
        // "aim circles with nothing on screen" symptom. Suppress the pivot
        // when the backing detection is stale relative to the detector's own
        // recent cadence; the aim loop then falls through to "no fresh event"
        // and settles instead of chasing a ghost.
        if (detectionBuffer.staleLocked())
        {
            publish(PivotSnapshot{});
            return;
        }
    }

    // Crosshair-colour and laser-colour are INDEPENDENT and may both be on.
    // Read each hotkey's two toggles + the two separate palettes/params here.
    bool cross_enabled = false;
    bool laser_enabled = false;
    bool cross_has_color = false;
    bool laser_has_color = false;
    float cross_smooth = 0.0f;
    float laser_smooth = 0.0f;

    crosshair::CrosshairDetectorSettings cross_settings;
    crosshair::LaserDetectorSettings     laser_settings;

    {
        std::lock_guard<std::recursive_mutex> cfg(configMutex);
        if (active_idx >= static_cast<int>(config.hotkeys.size()))
        {
            publish(PivotSnapshot{});
            return;
        }
        const auto& hk = config.hotkeys[active_idx];
        cross_enabled = hk.crosshair_detect_enabled;
        laser_enabled = hk.laser_detect_enabled;
        // 智能扳机默认接入准星找色:启用 smart_trigger 时即使用户未在 UI
        // 中显式打开 crosshair_detect_enabled,也隐式开启 crosshair 通路。
        // 这样扳机判定能用真实准星色点而不是几何中心,无需新增 UI 开关。
        // (镭射通路 laser_detect_enabled 仍需用户显式打开,不被扳机隐式启用。)
        if (hk.smart_trigger_enabled)
            cross_enabled = true;
        if (!cross_enabled && !laser_enabled)
        {
            // This hotkey opted into neither colour mode.
            publish(PivotSnapshot{});
            return;
        }

        cross_smooth = config.crosshair_smooth;
        laser_smooth = config.laser_smooth;

        cross_settings.enabled         = true;
        cross_settings.rect_w          = config.crosshair_rect_w;
        cross_settings.rect_h          = config.crosshair_rect_h;
        cross_settings.min_pixel_count = config.crosshair_min_pixel_count;
        cross_settings.close_radius    = config.crosshair_close_radius;
        cross_settings.colors.reserve(config.crosshair_colors.size());
        for (const auto& c : config.crosshair_colors)
        {
            crosshair::CrosshairColorBand b;
            b.name = c.name; b.enabled = c.enabled;
            b.h_low = c.h_low; b.h_high = c.h_high;
            b.s_min = c.s_min; b.s_max = c.s_max;
            b.v_min = c.v_min; b.v_max = c.v_max;
            cross_has_color = cross_has_color || b.enabled;
            cross_settings.colors.push_back(std::move(b));
        }

        laser_settings.enabled         = true;
        laser_settings.rect_w          = config.laser_rect_w;
        laser_settings.rect_h          = config.laser_rect_h;
        laser_settings.center_x        = config.laser_center_x;
        laser_settings.center_y        = config.laser_center_y;
        laser_settings.min_pixel_count = config.laser_min_pixel_count;
        laser_settings.close_radius    = config.laser_close_radius;
        laser_settings.min_elongation  = config.laser_min_elongation;
        laser_settings.target_center_x = config.laser_target_center_x;
        laser_settings.target_center_y = config.laser_target_center_y;
        laser_settings.target_rect_w   = config.laser_target_rect_w;
        laser_settings.target_rect_h   = config.laser_target_rect_h;
        laser_settings.colors.reserve(config.laser_colors.size());
        for (const auto& c : config.laser_colors)
        {
            crosshair::CrosshairColorBand b;
            b.name = c.name; b.enabled = c.enabled;
            b.h_low = c.h_low; b.h_high = c.h_high;
            b.s_min = c.s_min; b.s_max = c.s_max;
            b.v_min = c.v_min; b.v_max = c.v_max;
            laser_has_color = laser_has_color || b.enabled;
            laser_settings.colors.push_back(std::move(b));
        }
    }

    // Wall-clock seconds for the adaptive filters.
    const double tsec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Detection order:
    //   1. Run crosshair-colour first (cheap centroid).  Its (unsmoothed)
    //      raw hit becomes the LASER "snap hint" when both are enabled.
    //   2. Run the laser fit.  When both detectors are on AND the laser
    //      line passes through the crosshair-colour point, the laser tip
    //      snaps to that exact point — best of both: stable line direction
    //      from the beam + precise reticle location from the crosshair colour.
    //   3. Publication priority when BOTH on:  laser tip → crosshair point.
    //      When only one is on it stays the sole source.
    //
    // Each source is smoothed by its OWN adaptive filter; the unused
    // source's filter is reset so it never carries a stale position into a
    // later hand-off (no glide on switch).

    std::optional<cv::Point2f> raw_cross;   // pre-smoothing — used as laser hint
    if (cross_enabled && cross_has_color)
        raw_cross = g_detector.detect(bgrFrame, cross_settings);

    std::optional<cv::Point2f> hit;

    bool laser_used = false;
    if (laser_enabled && laser_has_color)
    {
        if (raw_cross)
        {
            laser_settings.use_crosshair_hint  = true;
            laser_settings.crosshair_hint_x    = raw_cross->x;
            laser_settings.crosshair_hint_y    = raw_cross->y;
        }

        auto lh = g_laser_detector.detect(bgrFrame, laser_settings);
        if (lh)
        {
            if (laser_smooth > 0.001f)
            {
                g_laser_filter.configure(laser_smooth);
                *lh = g_laser_filter.filter(*lh, tsec);
            }
            hit = lh;
            laser_used = true;
        }
    }
    if (!laser_used)
        g_laser_filter.reset();

    bool cross_used = false;
    if (!hit && raw_cross)
    {
        hit = raw_cross;
        cross_used = true;
    }
    if (cross_used && cross_smooth > 0.001f)
    {
        g_cross_filter.configure(cross_smooth);
        *hit = g_cross_filter.filter(*hit, tsec);
    }
    else if (!cross_used)
    {
        g_cross_filter.reset();
    }

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
