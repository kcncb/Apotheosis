#include "crosshair_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

#include "Apotheosis.h"
#include "config.h"
#include "runtime/config_snapshot.h"
#include "laser_detector.h"
#include "runtime/active_hotkey.h"
#include "capture/gpu_color_ops.h"
#include "mem/gpu_image.h"

#include <cuda_runtime.h>

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
std::mutex g_filter_mtx;

constexpr int kMaxGpuBands = 16;

struct GpuDetectorState
{
    cudaStream_t stream = nullptr;
    GpuHsvBand* device_bands = nullptr;
    int* device_result = nullptr;
    int* host_result = nullptr;
    bool initialized = false;

    bool ensure()
    {
        if (initialized) return true;
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
            return false;
        if (cudaMalloc(reinterpret_cast<void**>(&device_bands),
                       sizeof(GpuHsvBand) * kMaxGpuBands) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&device_result), sizeof(int) * 4) != cudaSuccess
            || cudaMallocHost(reinterpret_cast<void**>(&host_result), sizeof(int) * 4) != cudaSuccess)
            return false;
        initialized = true;
        return true;
    }
};

GpuDetectorState& gpu_state()
{
    // Intentionally process-lifetime: avoids CUDA-runtime teardown ordering
    // hazards during application shutdown. The allocation is under 1 KiB.
    static GpuDetectorState* state = new GpuDetectorState();
    return *state;
}

bool target_gate_open()
{
    std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
    return !detectionBuffer.boxes.empty() && !detectionBuffer.staleLocked();
}

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
        const auto snapshot = runtime_config::read();
        const auto& cfg = *snapshot;
        if (active_idx >= static_cast<int>(cfg.hotkeys.size()))
        {
            publish(PivotSnapshot{});
            return;
        }
        const auto& hk = cfg.hotkeys[active_idx];
        cross_enabled = hk.crosshair_detect_enabled;
        laser_enabled = hk.laser_detect_enabled;
        if (!cross_enabled && !laser_enabled)
        {
            // This hotkey opted into neither colour mode.
            publish(PivotSnapshot{});
            return;
        }

        cross_smooth = cfg.crosshair_smooth;
        laser_smooth = cfg.laser_smooth;

        cross_settings.enabled         = true;
        cross_settings.rect_w          = cfg.crosshair_rect_w;
        cross_settings.rect_h          = cfg.crosshair_rect_h;
        cross_settings.min_pixel_count = cfg.crosshair_min_pixel_count;
        cross_settings.close_radius    = cfg.crosshair_close_radius;
        cross_settings.colors.reserve(cfg.crosshair_colors.size());
        for (const auto& c : cfg.crosshair_colors)
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
        laser_settings.rect_w          = cfg.laser_rect_w;
        laser_settings.rect_h          = cfg.laser_rect_h;
        laser_settings.center_x        = cfg.laser_center_x;
        laser_settings.center_y        = cfg.laser_center_y;
        laser_settings.min_pixel_count = cfg.laser_min_pixel_count;
        laser_settings.close_radius    = cfg.laser_close_radius;
        laser_settings.min_elongation  = cfg.laser_min_elongation;
        laser_settings.target_center_x = cfg.laser_target_center_x;
        laser_settings.target_center_y = cfg.laser_target_center_y;
        laser_settings.target_rect_w   = cfg.laser_target_rect_w;
        laser_settings.target_rect_h   = cfg.laser_target_rect_h;
        laser_settings.colors.reserve(cfg.laser_colors.size());
        for (const auto& c : cfg.laser_colors)
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
    {
        std::lock_guard<std::mutex> filter_lock(g_filter_mtx);
        if (cross_used && cross_smooth > 0.001f)
        {
            g_cross_filter.configure(cross_smooth);
            *hit = g_cross_filter.filter(*hit, tsec);
        }
        else if (!cross_used)
        {
            g_cross_filter.reset();
        }
    }

    PivotSnapshot snap;
    snap.ts = std::chrono::steady_clock::now();
    static int s_lost_frames = 0;
    static cv::Point2f s_last_valid_hit(0, 0);

    if (hit)
    {
        snap.x = static_cast<double>(hit->x);
        snap.y = static_cast<double>(hit->y);
        snap.valid = true;
        s_last_valid_hit = *hit;
        s_lost_frames = 0;
    }
    else
    {
        // 开火火光/烟雾瞬间遮挡的丢帧惯性接力保护：
        // 如果前几帧有明确命中，在接下来的 1~3 帧内丢失时，保持上一有效准星位置，
        // 绝不让准星突变断崖式跳回屏幕物理正中心，防止压枪抽搐
        if (s_lost_frames < 3 && s_last_valid_hit.x > 1.0f)
        {
            snap.x = static_cast<double>(s_last_valid_hit.x);
            snap.y = static_cast<double>(s_last_valid_hit.y);
            snap.valid = true;
            s_lost_frames++;
        }
        else
        {
            snap.valid = false;
        }
    }
    publish(snap);
}

bool gpu_path_active()
{
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0) return false;
    const auto snapshot = runtime_config::read();
    if (active_idx >= static_cast<int>(snapshot->hotkeys.size())) return false;
    const auto& hk = snapshot->hotkeys[active_idx];
    // When laser is also enabled, keep the established CPU combined path so
    // its line fitter can consume the ordinary crosshair hit as a snap hint.
    return hk.crosshair_detect_enabled && !hk.laser_detect_enabled;
}

bool cpu_path_active()
{
    const int active_idx = runtime::g_active_hotkey_index.load();
    if (active_idx < 0) return false;
    const auto snapshot = runtime_config::read();
    if (active_idx >= static_cast<int>(snapshot->hotkeys.size())) return false;
    return snapshot->hotkeys[active_idx].laser_detect_enabled;
}

void process_gpu_frame(const GpuImage& frame)
{
    if (frame.empty() || frame.channels() != 3 || !gpu_path_active()
        || !target_gate_open())
    {
        publish(PivotSnapshot{});
        return;
    }

    const int active_idx = runtime::g_active_hotkey_index.load();
    const auto snapshot = runtime_config::read();
    if (active_idx < 0 || active_idx >= static_cast<int>(snapshot->hotkeys.size()))
    {
        publish(PivotSnapshot{});
        return;
    }

    std::vector<GpuHsvBand> bands;
    bands.reserve(std::min<size_t>(snapshot->crosshair_colors.size(), kMaxGpuBands));
    for (const auto& c : snapshot->crosshair_colors)
    {
        if (!c.enabled || bands.size() >= kMaxGpuBands) continue;
        bands.push_back(GpuHsvBand{
            std::clamp(c.h_low, 0, 179), std::clamp(c.h_high, 0, 179),
            std::clamp(c.s_min, 0, 255), std::clamp(c.s_max, 0, 255),
            std::clamp(c.v_min, 0, 255), std::clamp(c.v_max, 0, 255) });
    }
    if (bands.empty())
    {
        publish(PivotSnapshot{});
        return;
    }

    const int roi_w = std::min(frame.cols(), std::max(4, snapshot->crosshair_rect_w));
    const int roi_h = std::min(frame.rows(), std::max(4, snapshot->crosshair_rect_h));
    const int roi_x = std::clamp(frame.cols() / 2 - roi_w / 2, 0, frame.cols() - roi_w);
    const int roi_y = std::clamp(frame.rows() / 2 - roi_h + 10, 0, frame.rows() - roi_h);

    auto& state = gpu_state();
    if (!state.ensure())
    {
        publish(PivotSnapshot{});
        return;
    }
    if (frame.readyEvent())
        cudaStreamWaitEvent(state.stream, frame.readyEvent(), 0);
    if (cudaMemcpyAsync(state.device_bands, bands.data(),
                        bands.size() * sizeof(GpuHsvBand), cudaMemcpyHostToDevice,
                        state.stream) != cudaSuccess
        || cudaMemsetAsync(state.device_result, 0, sizeof(int) * 4, state.stream) != cudaSuccess)
    {
        publish(PivotSnapshot{});
        return;
    }

    launch_crosshair_hsv_reduce_bgr_u8(
        frame.data(), frame.step(), frame.cols(), frame.rows(),
        roi_x, roi_y, roi_w, roi_h,
        state.device_bands, static_cast<int>(bands.size()),
        state.device_result, state.stream);
    if (cudaGetLastError() != cudaSuccess
        || cudaMemcpyAsync(state.host_result, state.device_result, sizeof(int) * 4,
                           cudaMemcpyDeviceToHost, state.stream) != cudaSuccess
        || cudaStreamSynchronize(state.stream) != cudaSuccess)
    {
        publish(PivotSnapshot{});
        return;
    }

    PivotSnapshot out;
    out.ts = std::chrono::steady_clock::now();
    const int count = state.host_result[1];
    if (count >= std::max(1, snapshot->crosshair_min_pixel_count))
    {
        cv::Point2f hit(
            static_cast<float>(state.host_result[2]) / static_cast<float>(count),
            static_cast<float>(state.host_result[3]) / static_cast<float>(count));
        const float cx = frame.cols() * 0.5f, cy = frame.rows() * 0.5f;
        const float tolerance = 0.5f * static_cast<float>(std::min(roi_w, roi_h));
        if (std::hypot(hit.x - cx, hit.y - cy) <= tolerance)
        {
            const float smooth = snapshot->crosshair_smooth;
            if (smooth > 0.001f)
            {
                const double tsec = std::chrono::duration<double>(
                    out.ts.time_since_epoch()).count();
                std::lock_guard<std::mutex> filter_lock(g_filter_mtx);
                g_cross_filter.configure(smooth);
                hit = g_cross_filter.filter(hit, tsec);
            }
            out.x = hit.x;
            out.y = hit.y;
            out.valid = true;
        }
    }
    if (!out.valid)
    {
        // A tiny, nearly white reticle can disappear for one compressed frame.
        // Keep the last genuine hit only for the normal freshness window so a
        // one- or two-frame colour dropout does not snap the pivot to centre.
        const PivotSnapshot previous = read();
        if (previous.valid
            && out.ts - previous.ts <= std::chrono::milliseconds(kFreshnessMs))
        {
            publish(previous);
            return;
        }
        std::lock_guard<std::mutex> filter_lock(g_filter_mtx);
        g_cross_filter.reset();
    }
    publish(out);
}

} // namespace crosshair_runtime
