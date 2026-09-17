#include "runtime/aim_loop.h"

// ★★ 通用控制器层的运行期外壳。设计见 docs/generic-controller-layer.md。
//
// 职责边界（刻意划清楚）:
//   本文件【只做接线】—— 取检测、取准星、算 dt、调 control::AimController、
//   把整数计数交给驱动。**任何控制逻辑都不写在这里**，一律放 Apotheosis/control/。
//   理由: control/ 是零依赖的、能在 macOS 上单测的；把逻辑写进本文件就等于
//   把它移出可测范围。

#include "control/aim_controller.h"

#include "Apotheosis.h"   // 设备指针 (makcuSerial / makcuNewSerial / kmboxNetSerial)
#include "config/config.h"
#include "crosshair/crosshair_runtime.h"
#include "detector/detection_buffer.h"
#include "mouse/mouse.h"
#include "runtime/active_hotkey.h"
#include "runtime/config_snapshot.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace runtime::aim_loop
{

namespace
{

std::mutex g_mtx;
std::unique_ptr<control::AimController> g_controller;

// ── 驱动通道 ────────────────────────────────────────────────────────────
// ★ 这个 MouseThread 由本模块【拥有】。旧控制链删除后程序里已经没有别的
//   MouseThread 实例了(没人再构造它)，所以这里自己建一个 —— 它的全部作用
//   就是提供一条"把整数计数发给盒子"的通道(内部有 LatestMoveSlot 异步队列，
//   慢驱动不会把这个线程拖住)。
// ★ 不用 g_mouse 那种全局: 生命周期跟着本模块走，reset() 时也一起处理。
std::unique_ptr<MouseThread> g_mouse;
std::mutex g_mouse_mtx;

// 惰性构造: 只在真的要下发时才建，避免"控制没开也占着一条线程"。
MouseThread* ensureMouse()
{
    std::lock_guard<std::mutex> lk(g_mouse_mtx);
    if (g_mouse)
        return g_mouse.get();

    MouseRuntimeParams params;
    {
        const auto snap = runtime_config::read();
        params.detection_resolution = snap ? snap->detection_resolution : 320;
    }
    // ★ 设备指针在 inputDeviceMutex 下读 —— 热插拔会换掉它们。
    MakcuConnection* makcu = nullptr;
    MakcuNewConnection* makcuNew = nullptr;
    KmboxNetConnection* kmboxNet = nullptr;
    {
        std::lock_guard<std::mutex> lkDev(inputDeviceMutex);
        makcu = makcuSerial;
        makcuNew = makcuNewSerial;
        kmboxNet = kmboxNetSerial;
    }
    if (!makcu && !makcuNew && !kmboxNet)
        return nullptr;   // 没设备，不建通道

    g_mouse = std::make_unique<MouseThread>(params, makcu, makcuNew, kmboxNet);
    return g_mouse.get();
}
// 上一拍的 steady_clock 时刻。dt = now - 它。
// ★ 初值 epoch: 第一拍 dt 会是个巨大的数, 所以下面显式判 first_tick_。
std::chrono::steady_clock::time_point g_last_tick{};
bool g_first_tick = true;
uint64_t g_frame_index = 0;

// 取准星位置（检测图像素）。
//
// ★ D1 + D4-b: 找色关时用【画面中心】；找色开时用检测到的准星；
//   找色失效时【退回画面中心】（而不是不控制）。
control::Vec2 resolveCrosshair(const Config& cfg, const HotkeyProfile& hk, bool& fresh)
{
    const double center = static_cast<double>(cfg.detection_resolution) * 0.5;

    // 找色是否对这个热键启用？
    if (!hk.crosshair_detect_enabled)
    {
        // 未启用 ⇒ 静态中心，永远新鲜（它不是"测量值"，是常量）。
        fresh = true;
        return control::Vec2{ center, center };
    }

    const auto snap = crosshair_runtime::read();
    const auto now = std::chrono::steady_clock::now();
    const bool usable =
        snap.valid &&
        snap.ts.time_since_epoch().count() != 0 &&
        std::chrono::duration<double, std::milli>(now - snap.ts).count() <=
            static_cast<double>(crosshair_runtime::kFreshnessMs);

    if (usable)
    {
        fresh = true;
        return control::Vec2{ snap.x, snap.y };
    }

    // ★★ D4-b: 找色失效 ⇒ 【退回画面中心】，标记为不新鲜。
    //    fresh=false 会让 AimController 本拍不出力（二值门禁，方案 §3.3 第 2 条）
    //    —— 这是刻意的: 准星位置不可信时不控制，比用旧位置控制安全。
    //    ★ 退回的中心点仍然返回，供遥测显示"如果控制，瞄点在哪"。
    fresh = false;
    return control::Vec2{ center, center };
}

} // namespace

bool tick()
{
    // ── 0. 总开关 + 快照 ──────────────────────────────────────────────
    const auto snapshot = runtime_config::read();
    if (!snapshot)
        return false;
    const Config& cfg = *snapshot;

    const int activeIdx = runtime::g_active_hotkey_index.load();
    if (activeIdx < 0)
        return false;   // 没按住瞄准键
    if (activeIdx >= static_cast<int>(cfg.hotkeys.size()))
        return false;
    const HotkeyProfile& hk = cfg.hotkeys[static_cast<size_t>(activeIdx)];

    // ★★ 未启用直接返回 —— 这是"能真的动鼠标"的唯一闸门。
    //    默认 false，用户显式打开才会下发。
    if (!hk.ctl_enabled)
        return false;

    // ── 1. 取检测（在锁内拷贝出来，别持锁做控制）──────────────────────
    std::vector<control::Candidate> candidates;
    bool detectionFresh = false;
    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        if (detectionBuffer.boxes.empty())
            return false;
        // ★ 二值新鲜度门禁（§3.3 第 1 条）。不做连续降权 —— 用户明确决定。
        detectionFresh = !detectionBuffer.staleLocked();
        const size_t n = detectionBuffer.boxes.size();
        candidates.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            const bool hasPrecise =
                (i < detectionBuffer.precise_boxes.size() &&
                 detectionBuffer.precise_boxes[i].width > 0.0f &&
                 detectionBuffer.precise_boxes[i].height > 0.0f);
            const cv::Rect& r = detectionBuffer.boxes[i];
            cv::Rect2f pr = hasPrecise ? detectionBuffer.precise_boxes[i]
                                       : cv::Rect2f(static_cast<float>(r.x),
                                                    static_cast<float>(r.y),
                                                    static_cast<float>(r.width),
                                                    static_cast<float>(r.height));
            control::Candidate c;
            // ★ 用【浮点框】而不是整数框: 提前取整会把抖动放大成量化噪声，
            //   而那正是稳定器要处理的东西。
            c.box = control::Box{ static_cast<double>(pr.x), static_cast<double>(pr.y),
                                  static_cast<double>(pr.width), static_cast<double>(pr.height) };
            c.classId = (i < detectionBuffer.classes.size()) ? detectionBuffer.classes[i] : -1;
            c.confidence = (i < detectionBuffer.confidences.size())
                ? static_cast<double>(detectionBuffer.confidences[i]) : 0.0;
            candidates.push_back(c);
        }
    }
    if (candidates.empty())
        return false;

    // ── 2. 取准星 ─────────────────────────────────────────────────────
    bool crossFresh = false;
    const control::Vec2 cross = resolveCrosshair(cfg, hk, crossFresh);

    // ── 3. 算 dt ──────────────────────────────────────────────────────
    //
    // ★★ 只有在【真的要控制】的时候才推进 g_last_tick。
    //   若在不控制的分支(下面各种 return)里也推进它, 那些时刻就被"吃掉"了 ——
    //   下一次真控制会拿到一个远小于真实间隔的 dt, 于是微分项被放大、
    //   积分项被缩小, 表现为"偶尔抽一下"。所以耗时读表要放在最后。
    const auto now = std::chrono::steady_clock::now();
    double dtSec = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_first_tick)
        {
            // ★ 首拍不控制: 没有可信的 dt, 也就没有可信的微分/积分。
            //   只记录时刻、建好控制器, 本拍直接返回。
            g_last_tick = now;
            g_first_tick = false;
            if (!g_controller)
                g_controller = std::make_unique<control::AimController>();
            g_controller->setConfig(toControllerConfig(flattenProfile(hk, cfg.detection_resolution, cfg.class_filters)));
            g_controller->reset();
            return false;
        }
        dtSec = std::chrono::duration<double>(now - g_last_tick).count();
    }
    if (!dtIsUsable(dtSec))
    {
        // ★★ dt 不可信 ⇒ 本拍不输出, 【并且把控制器状态清干净】。
        //   不清的话, 上一次的 prevError / 积分 / 余量 会与下一次的真实误差
        //   组合成一个跨越大间隙的"假微分" —— 而那个方向是随机的。
        //   ★ 这与"限幅截掉的不许攒欠账"是同一条原则: 算不出来的东西不要留着。
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller)
            g_controller->reset();
        g_last_tick = now;   // 重新起算, 免得下一拍又超上限
        return false;
    }
    {
        // ★ dt 合法, 才承认这一拍的时间推进。
        std::lock_guard<std::mutex> lk(g_mtx);
        g_last_tick = now;
    }

    // ── 4. 控制 ───────────────────────────────────────────────────────
    control::ControlOutput out;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_controller)
            g_controller = std::make_unique<control::AimController>();
        // ★ 每拍重新下发配置 ⇒ 界面上改参数【立即生效】，不用重启会话。
        //   代价是一次小结构体拷贝，相对本拍的其它工作可忽略。
        g_controller->setConfig(toControllerConfig(flattenProfile(hk, cfg.detection_resolution, cfg.class_filters)));

        control::ControlInput in;
        in.candidates = std::move(candidates);
        in.cross = cross;
        in.dtSec = dtSec;
        in.frameIndex = ++g_frame_index;
        in.detectionFresh = detectionFresh;
        in.crosshairFresh = crossFresh;

        out = g_controller->update(in);
    }

    if (!out.engaged)
        return false;
    if (out.counts.x == 0 && out.counts.y == 0)
        return false;

    // ── 5. 下发 ───────────────────────────────────────────────────────
    // ★ 单位: control/ 的输出就是【整数鼠标计数】，与驱动接口同一单位，
    //   这里【没有换算】—— 任何缩放都会破坏"唯一一次量化"这条铁律。
    //
    // ★ 走 sendRawMove（与旧控制链同一条驱动通道）: 它内部有下标队列
    //   (LatestMoveSlot)，慢驱动不会把这个线程拖住。
    if (MouseThread* mouse = ensureMouse())
        mouse->sendRawMove(out.counts.x, out.counts.y);
    return true;
}

void reset()
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller)
            g_controller->reset();
        g_first_tick = true;
        g_frame_index = 0;
        g_last_tick = std::chrono::steady_clock::time_point{};
    }
    // ★ 会话停止时释放驱动通道: 它的 moveWorker_ 线程要 join 掉，
    //   否则下次 start 会多一条。★ 也要清掉队列里没发完的位移 ——
    //   不然"停了之后还动一下"。
    std::lock_guard<std::mutex> lk(g_mouse_mtx);
    if (g_mouse)
    {
        g_mouse->clearQueuedMoves();
        g_mouse.reset();
    }
}

bool active()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_controller != nullptr;
}

} // namespace runtime::aim_loop
