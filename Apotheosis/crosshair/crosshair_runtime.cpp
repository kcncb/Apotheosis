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
PivotSnapshot g_static_ref{};

crosshair::CrosshairDetector  g_detector;

// ── 【2026-09-13 删除】准星枢轴的自适应(One-Euro)低通 ────────────
// 删掉的代码: LowPass / OneEuro2D 结构体 + g_cross_filter + g_filter_mtx, 以及两处调用点
// (各自的 process_frame 路径与 CUDA 路径)。
//
// 为什么: 现在瞄点已经有一道 α-β 框平滑(mouse/anchor_filter.h)在 PID 之前。
// 准星这里再叠一层就是两道串联滤波: 各自引入滞后、参数还互相耦合。
// 保留 α-β 的理由: 它就在控制器门口, 位置/速度状态对 PID 直接可用,
// 而且对"匀速目标不落后"有解析保证。
//
// 准星找色检测本身保留, 只是输出不再被时间平滑。
// 旧 ini 里的 crosshair_smooth 键会被忽略。


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
    if (snap.ts.time_since_epoch().count() && g_snap.ts.time_since_epoch().count() && snap.ts < g_snap.ts) return;
    g_snap = snap;
}

PivotSnapshot read_static_ref()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_static_ref;
}

void publish_static_ref(const PivotSnapshot& ref)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_static_ref = ref;
}

void process_frame(const cv::Mat& bgrFrame, int64_t captured_ns)
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
        // (a slow frame), the buffer keeps reporting a
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

    bool cross_enabled = false;
    bool cross_has_color = false;
    // (cross_smooth 已随 One-Euro 滤波一起删除 2026-09-13)

    crosshair::CrosshairDetectorSettings cross_settings;

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
        if (!cross_enabled)
        {
            publish(PivotSnapshot{});
            return;
        }

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
    }

    std::optional<cv::Point2f> hit;
    if (cross_enabled && cross_has_color)
        hit = g_detector.detect(bgrFrame, cross_settings);

    // ── 【2026-09-13 删除】准星枢轴的自适应平滑 (One-Euro, crosshair_smooth) ──────
    // 原代码在这里对 hit 做一次 g_cross_filter.filter()。删掉的理由:
    //
    //   现在瞄点已经有一道 α-β 框平滑(anchor_filter.h)在 PID 之前。准星这里再叠一层
    //   就是【两道串联滤波】: 各自引入滞后、参数还互相耦合(调了一个另一个就不对了),
    //   而两道滤波能滤掉的东西是一样的 —— 没有理由串两个。
    //
    //   留哪一道: 留 α-β。因为它就在控制器门口, 位置/速度状态对 PID 直接可用, 而且
    //   它对"匀速目标不落后"这件事有解析保证(见 anchor_filter.h 的说明)。准星找色
    //   本身【保留】, 只是它的输出现在是原始值, 不再被时间平滑。
    //
    // 顺带: 原来还有一条"配置 <=0 时强制按 0.5, 不许关闭"的逻辑。那道防线存在的
    // 前提是"这是唯一一道滤波"; 前提没了, 这条特例也一起删。

    PivotSnapshot snap;
    snap.ts = captured_ns > 0 ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(captured_ns))
                              : std::chrono::steady_clock::now();
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
    return snapshot->hotkeys[active_idx].crosshair_detect_enabled;
}

bool cpu_path_active()
{
    return false;
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
    // ROI 垂直位置【故意偏上】: 全自动射击时准星是向上弹跳的, 采样框必须多留上方
    // 空间, 否则后坐力抬枪会把准星顶出框外。CPU 侧 crosshair_detector.cpp 的
    // dynamic_center_roi() 用 y = cy - h*0.6 表达同一件事(向上 60% / 向下 40%);
    // 这里的 rows/2 - roi_h + 10 是"框底在中心线下方 10px", 即向上留了约 100%
    // 的余量, 比 CPU 侧更宽松。★ 不要顺手把它改成居中 —— 那会在连发时丢准星。
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
    out.ts = frame.captureNs() > 0 ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(frame.captureNs()))
                                  : std::chrono::steady_clock::now();
    const int count = state.host_result[1];
    if (count >= std::max(1, snapshot->crosshair_min_pixel_count))
    {
        cv::Point2f hit(
            static_cast<float>(state.host_result[2]) / static_cast<float>(count),
            static_cast<float>(state.host_result[3]) / static_cast<float>(count));
        // 接受窗口 = ROI 本体, 不再叠加"离画面中心 <= 0.5*min(roi_w,roi_h)"
        // 的圆形门限。那个半径比 ROI 小得多(rect 32x66 时只有 16px), 会把 ROI
        // 特意向上扩展出来的后坐力抬枪带整段判掉: 准星一旦离开画面中心 16px 就
        // 判无效, pivot 随即回落成画面几何中心(见 mouse_thread_loop.cpp 的
        // resolve_crosshair_pivot), 控制器以为已经对准目标, 于是不再下压 ——
        // 表现就是"开了找色也没有压枪效果"。ROI 尺寸本身就是用户设定的接受范围,
        // 假命中由内核的邻近度排序 + ROI 边界约束。
        const float roi_left   = static_cast<float>(roi_x);
        const float roi_top    = static_cast<float>(roi_y);
        const float roi_right  = roi_left + static_cast<float>(roi_w);
        const float roi_bottom = roi_top + static_cast<float>(roi_h);
        if (hit.x >= roi_left && hit.x <= roi_right
            && hit.y >= roi_top && hit.y <= roi_bottom)
        {
            // 【2026-09-13 删除】这里原来是 g_cross_filter 的自适应平滑。
            // 准星枢轴现在直接用原始质心 —— 平滑统一交给 PID 之前的 anchor_filter。
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
    }
    publish(out);
}

} // namespace crosshair_runtime
