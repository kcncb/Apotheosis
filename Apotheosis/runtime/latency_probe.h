#pragma once

// =============================================================================
// 端到端延迟探针 (End-to-end latency probe)
// =============================================================================
//
// 时间边界 (均使用 steady_clock, 随具体帧传递):
//   T0: Media Foundation OnReadSample 回调入口, 早于样本拷贝/解码/转色。
//       markCapture(source_ns) 在后端交帧时接收原始戳, 不重打起点。
//   T1: detector 真正从输入槽取出该帧 (markDetectorConsume)。
//   T2: 该帧推理完成、发布检测结果。
//   T3: 控制环消费该结果。T4: 该帧对应的驱动发送调用完成。
//   capture_wait=T1-T0: 回调交接、样本拷贝、解码、转色、掩码、输入槽等待。
//   inference=T2-T1: 预处理、推理、后处理与 NMS。
//   total=T3-T0; e2e=T4-T0。统计不代表显示画面响应或硬件执行完成。
//
// 设备侧帧龄单列: 驱动 DeviceTimestamp 到 OnReadSample 回调的时间。
// 只有驱动提供可解释的时钟时才显示; 缺失、无效或过期均为 -1。
// 该字段与 total/E2E 的样本和 EMA 窗口不同, 不能简单相加当作逐帧真值。
// 当前探针不覆盖设备打戳之前的 HDMI 流水线, 也不覆盖鼠标硬件/游戏响应。
// 小设备帧龄不能单独证明卡芯片很快或驱动完全没有开销。
//
// 统计由原子量和互斥量保护; 日志线程负责格式化/落盘。实际开销需实机测量。
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace runtime
{
namespace latency
{

using Clock = std::chrono::steady_clock;

inline int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

inline double nsToMs(int64_t ns)
{
    return static_cast<double>(ns) / 1.0e6;
}

// -----------------------------------------------------------------------------
// 帧的采集时刻。采集线程写, detector / aim loop 读。latest-only 语义与
// capture.cpp 的 frameQueue (容量 1) 一致。
// -----------------------------------------------------------------------------
struct CaptureStamp
{
    std::atomic<int64_t>  ns{0};
    std::atomic<uint64_t> seq{0};
};

inline CaptureStamp& captureStamp()
{
    static CaptureStamp s;
    return s;
}

// 后端交帧时传入原始回调戳; 无原始戳的调用方退回当前时刻。
inline int64_t markCapture(int64_t source_ns = 0)
{
    const int64_t ns = source_ns > 0 ? source_ns : nowNs();
    auto& s = captureStamp();
    s.ns.store(ns, std::memory_order_release);
    s.seq.fetch_add(1, std::memory_order_relaxed);
    return ns;
}

inline int64_t loadCaptureNs()
{
    return captureStamp().ns.load(std::memory_order_acquire);
}

inline uint64_t loadCaptureSeq()
{
    return captureStamp().seq.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 设备侧帧龄独立于回调后的 T0..T4。-1 表示缺失/无效/过期。
// -----------------------------------------------------------------------------
inline std::atomic<int>& deviceFrameAgeUs()
{
    static std::atomic<int> v{ -1 };
    return v;
}

inline std::atomic<int64_t>& deviceAgeUpdateNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

inline void noteDeviceFrameAgeUs(int us)
{
    deviceFrameAgeUs().store(us, std::memory_order_relaxed);
    deviceAgeUpdateNs().store(nowNs(), std::memory_order_release);
}

inline int loadDeviceFrameAgeUs()
{
    if (nowNs() - deviceAgeUpdateNs().load(std::memory_order_acquire) > 2'000'000'000)
        return -1;
    return deviceFrameAgeUs().load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// 跨线程的一次性交接量 (detector -> aim loop)
// -----------------------------------------------------------------------------
inline std::atomic<int64_t>& submitNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

// 单线程自测的兼容槽。生产检测器必须使用随帧携带的 SubmitStamp。
inline std::atomic<int64_t>& submittedCaptureNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

inline int64_t takeSubmittedCaptureNs()
{
    return submittedCaptureNs().load(std::memory_order_acquire);
}

// 采集时刻与取帧时刻成对携带。
//
// ★ 必须在 detector【取走该帧的那一次临界区里】读出, 并把结果按槽保存下来。
//
// 旧实现在【发布时】才去读那两个"只存最新"的全局量。而发布发生在取走下一帧
// 之后(双缓冲下必然如此), 所以读到的恒定是【下一帧】的采集戳 —— total 于是
// 系统性地少算整整一个帧间隔(120fps = 8.33ms), 而且越是开双缓冲错得越稳定。
// 按帧、按槽携带即可彻底解耦: 每个 slot 记住自己那一帧的 T0/T1, 发布时取自己那份。
struct SubmitStamp
{
    int64_t capture_ns = 0;   // T0 采集时刻
    int64_t submit_ns  = 0;   // T1 detector 取帧时刻
    uint64_t sequence = 0;
};

// -----------------------------------------------------------------------------
// 阶段统计
// -----------------------------------------------------------------------------
struct Stage
{
    double   last_ms = 0.0;
    double   ema_ms  = 0.0;
    double   max_ms  = 0.0;
    uint64_t n       = 0;

    void push(double ms)
    {
        last_ms = ms;
        // 指数滑动平均: 前 10 帧用算术平均预热, 之后 0.9/0.1 平滑。
        ema_ms = (n < 10) ? (ema_ms * n + ms) / (n + 1)
                          : (ema_ms * 0.9 + ms * 0.1);
        if (ms > max_ms || n == 0) max_ms = ms;
        ++n;
    }

    void reset()
    {
        last_ms = ema_ms = max_ms = 0.0;
        n = 0;
    }
};

enum StageId
{
    kCaptureWait = 0,   // T0 -> T1  采集产出 -> detector 取帧
    kInference,         // T1 -> T2  预处理 + 推理 + NMS
    kPublishToAim,      // T2 -> T3  发布 -> 控制环消费
    kAimToMove,         // T3 -> T4  控制环 -> 位移写出
    kTotal,             // T0 -> T3  ★ 采集 -> aim loop 消费
    kEndToEnd,          // T0 -> T4  采集 -> 位移写出 (下界)
    kStageCount
};

struct Counters
{
    uint64_t frames_consumed   = 0;  // aim loop 结算过的完整帧
    uint64_t detections_seen    = 0;  // aim loop 看到的新检测批数(含未消费的)
    uint64_t capture_frames    = 0;  // 采集产出帧数
    uint64_t dropped_capture   = 0;  // 采集产出但 detector 没跟上的帧数
    uint64_t stale_consumes    = 0;  // aim loop 拿到没有采集戳的旧数据
};

struct Shared
{
    std::mutex mu;
    Stage      stages[kStageCount];
    Counters   counters;
    uint64_t   last_capture_seq_seen = 0;
    int64_t    last_capture_ns_seen  = 0;

    // 采集帧率 (由相邻两次 markCapture 的间隔推算)
    double     capture_interval_ms   = 0.0;
    int64_t    prev_capture_ns       = 0;

    double     engine_inference_ms   = -1.0;
    int64_t    engine_update_ns      = 0;
    int64_t    reset_ns              = 0;
    bool       enabled               = true;
};

inline Shared& shared()
{
    static Shared s;
    return s;
}

// -----------------------------------------------------------------------------
// 采集侧: 由 markCapture() 顺带维护采集节奏, 供 overlap/丢帧统计使用。
// 放在这里而不是 markCapture 里, 是为了让采集热路径只剩两个原子写。
// -----------------------------------------------------------------------------
inline void noteCaptureForStats(int64_t ns)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (sh.prev_capture_ns != 0)
    {
        const double dt = nsToMs(ns - sh.prev_capture_ns);
        if (dt > 0.0 && dt < 2000.0)
            sh.capture_interval_ms = (sh.capture_interval_ms <= 0.0)
                                         ? dt
                                         : sh.capture_interval_ms * 0.9 + dt * 0.1;
    }
    sh.prev_capture_ns = ns;
    sh.counters.capture_frames++;
}

// -----------------------------------------------------------------------------
// detector 侧: 取到帧。返回该帧的采集戳 (由调用方保存并随检测结果发布)。
// -----------------------------------------------------------------------------
inline SubmitStamp markSubmitStamp()
{
    // Called by the capture producer. Counters are updated only when inference
    // actually removes the frame from its latest-only input slot.
    return {loadCaptureNs(), 0, loadCaptureSeq()};
}

inline void markDetectorConsume(SubmitStamp& stamp)
{
    stamp.submit_ns = nowNs();
    submitNs().store(stamp.submit_ns, std::memory_order_release);
    submittedCaptureNs().store(stamp.capture_ns, std::memory_order_release);
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (stamp.capture_ns > 0)
        sh.stages[kCaptureWait].push(nsToMs(stamp.submit_ns - stamp.capture_ns));
    if (stamp.sequence > sh.last_capture_seq_seen + 1)
        sh.counters.dropped_capture += stamp.sequence - sh.last_capture_seq_seen - 1;
    sh.last_capture_seq_seen = stamp.sequence;
    sh.last_capture_ns_seen = stamp.capture_ns;
}

// Single-thread compatibility entry used by the standalone probe tests.
inline int64_t markSubmit()
{
    auto stamp = markSubmitStamp();
    markDetectorConsume(stamp);
    return stamp.capture_ns;
}

// -----------------------------------------------------------------------------
// detector 侧: 推理完成、即将发布检测结果。
//
// submit_ns: 本次发布所依据那一帧的 T1(取帧时刻)。TrtDetector 必须把该帧在
// 取走时记下的 T1 传进来 —— 不能让它自己去读全局槽, 那个槽此时可能已经是
// 下一帧的(见 SubmitStamp 注释)。传 -1 表示"沿用旧的全局槽语义", 仅供单线程
// 自测使用。
// -----------------------------------------------------------------------------
inline void markInferenceDone(int64_t submit_ns = -1)
{
    const int64_t sub = (submit_ns >= 0)
        ? submit_ns
        : submitNs().load(std::memory_order_acquire);
    if (sub == 0) return;
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.stages[kInference].push(nsToMs(nowNs() - sub));
}

// -----------------------------------------------------------------------------
// aim loop 侧: 看到了一批新检测(不论是否被消费)。
//
// 用来把"瞄准键没按下所以没消费"和"采集/推理真的停更了"区分开 —— 旧日志
// 两种情况都只打印同一句"采集或推理已停更", 会把人往错的方向引。
// -----------------------------------------------------------------------------
inline void noteDetectionSeen()
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.counters.detections_seen++;
}

// -----------------------------------------------------------------------------
// T3: return the actual consume instant so the move command can carry it.
// A frame with no movement contributes to total, but not to send latency/E2E.
// -----------------------------------------------------------------------------
inline int64_t markAimConsume(int64_t frame_capture_ns, int64_t publish_ns)
{
    auto& sh = shared();
    const int64_t now = nowNs();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.stages[kPublishToAim].push(publish_ns != 0 ? nsToMs(now - publish_ns) : 0.0);
    if (frame_capture_ns == 0)
    {
        sh.counters.stale_consumes++;
        return now;
    }
    sh.stages[kTotal].push(nsToMs(now - frame_capture_ns));
    sh.counters.frames_consumed++;
    return now;
}

// T4: called only when this command's driver send has completed successfully.
// This does not acknowledge physical mouse execution or display response.
inline void markMoveSent(int64_t frame_capture_ns, int64_t aim_ns, int64_t sent_ns = 0)
{
    if (!sent_ns) sent_ns = nowNs();
    if (frame_capture_ns <= 0 || aim_ns < frame_capture_ns || sent_ns < aim_ns) return;
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    if (frame_capture_ns < sh.reset_ns) return; // a late command from an old session
    sh.stages[kAimToMove].push(nsToMs(sent_ns - aim_ns));
    sh.stages[kEndToEnd].push(nsToMs(sent_ns - frame_capture_ns));
}

// -----------------------------------------------------------------------------
// 快照 (给 overlay / 遥测读取)
// -----------------------------------------------------------------------------
struct Snapshot
{
    bool     enabled             = true;
    uint64_t frames_consumed     = 0;
    uint64_t detections_seen     = 0;
    uint64_t capture_frames      = 0;
    uint64_t dropped_capture     = 0;
    uint64_t stale_consumes      = 0;
    int      device_frame_age_us = -1;
    double   capture_fps         = 0.0;
    double   capture_interval_ms = 0.0;
    Stage    stages[kStageCount];
    double   engine_inference_ms = -1.0;
};

inline void noteEngineInferenceMs(double ms)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.engine_inference_ms = ms;
    sh.engine_update_ns = nowNs();
}

inline Snapshot snapshot()
{
    auto& sh = shared();
    Snapshot out;
    std::lock_guard<std::mutex> lk(sh.mu);
    out.enabled             = sh.enabled;
    out.engine_inference_ms = nowNs() - sh.engine_update_ns <= 2'000'000'000
        ? sh.engine_inference_ms : -1.0;
    out.frames_consumed     = sh.counters.frames_consumed;
    out.detections_seen     = sh.counters.detections_seen;
    out.capture_frames      = sh.counters.capture_frames;
    out.dropped_capture     = sh.counters.dropped_capture;
    out.stale_consumes      = sh.counters.stale_consumes;
    out.device_frame_age_us = loadDeviceFrameAgeUs();
    out.capture_interval_ms = sh.capture_interval_ms;
    out.capture_fps = (sh.capture_interval_ms > 0.0)
                          ? 1000.0 / sh.capture_interval_ms
                          : 0.0;
    for (int i = 0; i < kStageCount; ++i)
        out.stages[i] = sh.stages[i];
    return out;
}

inline void reset()
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    for (int i = 0; i < kStageCount; ++i)
        sh.stages[i].reset();
    sh.counters = Counters{};
    sh.last_capture_seq_seen = loadCaptureSeq();
    sh.last_capture_ns_seen  = 0;
    sh.capture_interval_ms   = 0.0;
    sh.prev_capture_ns       = 0;
    sh.engine_inference_ms   = -1.0;
    sh.engine_update_ns      = 0;
    sh.reset_ns              = nowNs();
    noteDeviceFrameAgeUs(-1);
}

inline void setEnabled(bool on)
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.enabled = on;
}

inline bool enabled()
{
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    return sh.enabled;
}

// -----------------------------------------------------------------------------
// 文本报告 —— overlay 直接画, 也可原样贴日志。
// -----------------------------------------------------------------------------
inline std::string formatLines(bool detail)
{
    const Snapshot s = snapshot();
    char buf[192];
    std::string out;

    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof(buf), fmt, args...);
        out += buf;
        out += '\n';
    };
    auto addLine = [&](const char* text) {
        out += text;
        out += '\n';
    };

    if (!s.enabled)
        return u8"延迟探针: 已关闭\n";

    if (s.frames_consumed == 0)
        return u8"延迟探针: 等待数据...\n";

    add(u8"采集 %.1f fps (%.2f ms/帧)", s.capture_fps, s.capture_interval_ms);
    add(u8"总延迟 %.1f ms  (均 %.1f / 峰 %.1f)",
        s.stages[kTotal].last_ms, s.stages[kTotal].ema_ms,
        s.stages[kTotal].max_ms);

    if (detail)
    {
        add(u8"  采集->取帧  %5.1f ms", s.stages[kCaptureWait].last_ms);
        add(u8"  推理        %5.1f ms", s.stages[kInference].last_ms);
        add(u8"  发布->消费  %5.1f ms", s.stages[kPublishToAim].last_ms);
        add(u8"  消费->写出  %5.1f ms", s.stages[kAimToMove].last_ms);
        if (s.stages[kEndToEnd].n > 0)
            add(u8"  全链路      %5.1f ms", s.stages[kEndToEnd].last_ms);
        else
            addLine(u8"  全链路      -- (暂无成功发送样本)");
        // 独立诊断值; 不计入上述 T0..T4, 也不是卡芯片延迟。
        if (s.device_frame_age_us >= 0)
            add(u8"  设备侧帧龄  %5.1f ms  (驱动/MF 内部)", s.device_frame_age_us / 1000.0);
        else
            addLine(u8"  设备侧帧龄  --      (时间戳缺失、无效或过期)");
        if (s.dropped_capture > 0)
            add(u8"  !! detector 跟不上, 已丢 %llu 帧",
                static_cast<unsigned long long>(s.dropped_capture));
        addLine(u8"  (以上均不含采集卡芯片内部 HDMI->USB, 当前探针未覆盖)");
    }
    else
    {
        add(u8"  推理 %.1f | 消费 %.1f | 写出 %.1f",
            s.stages[kInference].last_ms, s.stages[kPublishToAim].last_ms,
            s.stages[kAimToMove].last_ms);
    }

    return out;
}

// -----------------------------------------------------------------------------
// ASCII 版: OpenCV 的 putText 无法渲染 CJK, preview overlay 必须用这个。
// 每行一条, 交给调用方逐行绘制。
// -----------------------------------------------------------------------------
inline std::vector<std::string> formatLinesAscii(bool detail)
{
    const Snapshot s = snapshot();
    std::vector<std::string> out;
    char buf[192];

    if (!s.enabled) { out.push_back("latency probe: OFF"); return out; }
    if (s.frames_consumed == 0) { out.push_back("latency probe: waiting..."); return out; }

    std::snprintf(buf, sizeof(buf), "E2E %.1f ms  (avg %.1f / pk %.1f)",
                  s.stages[kEndToEnd].n > 0 ? s.stages[kEndToEnd].last_ms : -1.0, s.stages[kEndToEnd].ema_ms,
                  s.stages[kEndToEnd].max_ms);
    out.push_back(s.stages[kEndToEnd].n > 0 ? buf : "E2E n/a (no successful send)");

    std::snprintf(buf, sizeof(buf), "  cap->det %5.1f  infer %5.1f",
                  s.stages[kCaptureWait].last_ms, s.stages[kInference].last_ms);
    out.push_back(buf);

    std::snprintf(buf, sizeof(buf), "  pub->aim %5.1f  aim->mv %5.1f",
                  s.stages[kPublishToAim].last_ms, s.stages[kAimToMove].last_ms);
    out.push_back(buf);

    std::snprintf(buf, sizeof(buf), "  T0->T3   %5.1f   [src %.0f fps]",
                  s.stages[kTotal].last_ms, s.capture_fps);
    out.push_back(buf);

    if (detail)
    {
        std::snprintf(buf, sizeof(buf), "  frames %llu  dropped %llu",
                      static_cast<unsigned long long>(s.frames_consumed),
                      static_cast<unsigned long long>(s.dropped_capture));
        out.push_back(buf);
        if (s.device_frame_age_us >= 0)
        {
            std::snprintf(buf, sizeof(buf), "  devq %.1f ms (driver/MF, pre-hand-off)",
                          s.device_frame_age_us / 1000.0);
            out.push_back(buf);
        }
        else
        {
            out.push_back("  devq n/a (timestamp unavailable/stale)");
        }
        out.push_back("  (excl. card-internal HDMI->USB)");
    }
    return out;
}


// =============================================================================
// 落盘日志
// =============================================================================
//
// 周期性把阶段统计写成一行 (默认 1Hz), 并单独记录超阈值的尖峰、丢帧与陈旧
// 消费事件。目的是让"跑一局之后回看"成为可能 —— overlay 只能看到瞬时值,
// 而台阶式的延迟劣化 (某个阶段偶尔飙一下) 恰恰是毁手感的主因, 必须落盘才抓得到。
//
// 行格式 (空格分隔, 便于 awk / python 解析):
//   <时间> | E2E=x avg=x pk=x | cap2det=x infer=x pub2aim=x aim2mv=x total=x
//          | src=x fps frames=n dropped=n stale=n
//
// 所有耗时为毫秒。total = T3-T0 (决定准星落后 v*total); e2e = T4-T0。
// 不含采集卡内部 HDMI->USB 延迟 (典型 20-60ms), 因此是【下界】。

struct FileLogConfig
{
    std::string directory   = "logs";     // 相对工作目录
    std::string basename    = "latency";  // 实际文件 latency_YYYY-mm-dd_HHMMSS.log
    int         interval_ms = 1000;       // 周期摘要间隔
    double      spike_ms    = 25.0;       // total 峰值超过该值 -> 记一行; <=0 关闭
    bool        flush_each  = true;       // 每行 fflush, 崩溃时也保得住
};

namespace detail
{

struct FileLogState
{
    std::mutex        mu;
    std::thread       worker;
    std::atomic<bool> running{false};
    std::string       path;   // 当前日志文件绝对路径
    std::string       error;  // 启动失败原因 (供 UI 显示)
    FileLogConfig     cfg;
};

// 故意不析构: 避免静态销毁顺序问题 (与 crosshair_runtime 同款做法)。
inline FileLogState& fileLogState()
{
    static FileLogState* s = new FileLogState();
    return *s;
}

// C++20 下 path::u8string() 返回 std::u8string (char8_t), 需要显式转回 char。
// MSVC 与 clang/gcc 行为一致, 所以统一走这个helper。
inline std::string pathToUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

inline std::string timestampNow()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _MSC_VER
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

inline std::string summaryLine()
{
    const Snapshot s = snapshot();
    char buf[400];
    std::snprintf(
        buf, sizeof(buf),
        "E2E=%.2f avg=%.2f pk=%.2f | cap2det=%.2f infer=%.2f pub2aim=%.2f "
        "aim2mv=%.2f total=%.2f | devq=%.2f | src=%.1ffps frames=%llu seen=%llu "
        "dropped=%llu stale=%llu",
        s.stages[kEndToEnd].n > 0 ? s.stages[kEndToEnd].last_ms : -1.0, s.stages[kEndToEnd].ema_ms,
        s.stages[kEndToEnd].max_ms,
        s.stages[kCaptureWait].ema_ms, s.stages[kInference].ema_ms,
        s.stages[kPublishToAim].ema_ms, s.stages[kAimToMove].ema_ms,
        s.stages[kTotal].ema_ms,
        // devq = 设备侧帧龄(驱动/MF 在把帧交给我们之前花掉的时间)。-1 = 该驱动
        // 不提供 sample 时间戳; 它是"卡本身慢"与"对接方式慢"的分界线。
        s.device_frame_age_us >= 0 ? s.device_frame_age_us / 1000.0 : -1.0,
        s.capture_fps, static_cast<unsigned long long>(s.frames_consumed),
        static_cast<unsigned long long>(s.detections_seen),
        static_cast<unsigned long long>(s.dropped_capture),
        static_cast<unsigned long long>(s.stale_consumes));
    return buf;
}

inline void logLine(const std::string& text)
{
    auto& st = fileLogState();
    if (st.path.empty()) return;
    std::ofstream f(st.path, std::ios::out | std::ios::app);
    if (!f) return;
    f << timestampNow() << " | " << text << '\n';
    if (st.cfg.flush_each) f.flush();
}

inline void logWorker(FileLogConfig cfg)
{
    auto& st = fileLogState();

    double   last_max[kStageCount] = {0};
    uint64_t last_dropped = 0;
    uint64_t last_stale   = 0;
    uint64_t last_frames  = 0;
    uint64_t last_seen    = 0;

    const int step_ms = 50;
    int elapsed = 0;

    while (st.running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        elapsed += step_ms;

        const Snapshot s = snapshot();

        // ---- 事件: 丢帧 / 陈旧消费 (一发生就记, 与周期无关) ----
        if (s.dropped_capture != last_dropped)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "EVENT dropped_detector +%llu (total %llu, capture %llu) "
                          u8"-> detector 跟不上, 中间帧被覆盖丢弃",
                          static_cast<unsigned long long>(s.dropped_capture - last_dropped),
                          static_cast<unsigned long long>(s.dropped_capture),
                          static_cast<unsigned long long>(s.capture_frames));
            logLine(buf);
            last_dropped = s.dropped_capture;
        }
        if (s.stale_consumes != last_stale)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "EVENT stale_consume +%llu (total %llu) "
                          u8"-> 控制环读到了无采集戳的数据",
                          static_cast<unsigned long long>(s.stale_consumes - last_stale),
                          static_cast<unsigned long long>(s.stale_consumes));
            logLine(buf);
            last_stale = s.stale_consumes;
        }

        // ---- 事件: 尖峰 ----
        if (cfg.spike_ms > 0.0 && s.stages[kTotal].max_ms > last_max[kTotal] &&
            s.stages[kTotal].max_ms >= cfg.spike_ms)
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "SPIKE total=%.2f (avg %.2f) cap2det=%.2f infer=%.2f "
                          "pub2aim=%.2f aim2mv=%.2f",
                          s.stages[kTotal].max_ms, s.stages[kTotal].ema_ms,
                          s.stages[kCaptureWait].ema_ms, s.stages[kInference].ema_ms,
                          s.stages[kPublishToAim].ema_ms, s.stages[kAimToMove].ema_ms);
            logLine(buf);
        }

        // ---- 周期摘要 ----
        if (elapsed >= cfg.interval_ms)
        {
            elapsed = 0;
            if (s.frames_consumed != last_frames)
            {
                logLine(summaryLine());
                last_frames = s.frames_consumed;
            }
            else if (s.frames_consumed > 0)
            {
                // 区分两种完全不同的"没消费":
                //   seen 还在涨 -> 检测一直在产, 只是瞄准键没按下;
                //   seen 也不涨 -> 采集或推理确实停更了。
                // 旧版两种情况都只写"采集或推理已停更", 会误导排查方向。
                char buf[224];
                const uint64_t seen_delta = s.detections_seen - last_seen;
                std::snprintf(
                    buf, sizeof(buf),
                    "IDLE consumed=+0 seen=+%llu -> %s",
                    static_cast<unsigned long long>(seen_delta),
                    seen_delta > 0
                        ? u8"检测仍在产帧, 仅是瞄准键未按下 (采集/推理正常)"
                        : u8"采集或推理确实已停更");
                logLine(buf);
            }
            last_seen = s.detections_seen;
        }

        for (int i = 0; i < kStageCount; ++i)
            if (s.stages[i].max_ms > last_max[i]) last_max[i] = s.stages[i].max_ms;
    }

    logLine("=== latency log stop ===");
}

} // namespace detail

// 启动落盘。成功返回 true, 路径见 fileLogPath()。
// 失败 (无写权限等) 返回 false, 原因见 fileLogError(), 不影响主流程。
inline bool startFileLog(const FileLogConfig& cfg = FileLogConfig{})
{
    auto& st = detail::fileLogState();
    std::lock_guard<std::mutex> lk(st.mu);

    if (st.running.load()) return true;

    std::error_code ec;
    std::filesystem::create_directories(cfg.directory, ec);

    std::string stamp = detail::timestampNow();   // 2026-09-10 14:44:01.123
    for (char& c : stamp)
        if (c == ' ' || c == ':') c = '-';
    if (stamp.size() > 19) stamp.resize(19);      // 去掉 .123

    std::filesystem::path p = std::filesystem::path(cfg.directory)
                            / (cfg.basename + "_" + stamp + ".log");

    std::ofstream probe(p, std::ios::out | std::ios::trunc);
    if (!probe)
    {
        st.error = "cannot write " + detail::pathToUtf8(p);
        return false;
    }
    probe << "# Apotheosis end-to-end latency log" << '\n'
          << "# T0=MF sample callback entry T1=detector dequeues frame "
             "T2=inference published T3=aim loop consumes T4=move sent" << '\n'
          << "# total = T3-T0 (software measurement age after MF callback)" << '\n'
          << "# e2e   = T4-T0" << '\n'
          << "# stages: cap2det=T1-T0  infer=T2-T1  pub2aim=T3-T2  aim2mv=T4-T3" << '\n'
          << u8"# devq = 设备侧帧龄: 驱动/MF 把帧交给我们之前花掉的时间. -1 = 驱动未提供"
             u8"/有效/新鲜时间戳. 不用于单独判定卡芯片快慢" << '\n'
          << "# unit: ms. EXCLUDES the capture card's internal HDMI->USB pipeline "
             "(not measurable in software), so this is a LOWER BOUND." << '\n'
          << "# line kinds: periodic summary | EVENT drop/stale | SPIKE | IDLE" << '\n'
          << u8"# frames=aim loop 消费过的批数  seen=aim loop 看到的新检测批数(含未消费)" << '\n'
          << u8"# seen 涨而 frames 不涨 = 瞄准键未按下(正常); 两者都不涨 = 采集/推理停更" << '\n'
          << "# started: " << detail::timestampNow() << '\n';
    probe.flush();
    probe.close();

    st.path = detail::pathToUtf8(std::filesystem::absolute(p, ec));
    st.cfg  = cfg;
    st.error.clear();
    st.running.store(true);
    st.worker = std::thread(detail::logWorker, cfg);

    detail::logLine("=== latency log start ===");
    return true;
}

inline void stopFileLog()
{
    auto& st = detail::fileLogState();
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (!st.running.load()) return;
        st.running.store(false);
        toJoin = std::move(st.worker);
    }
    if (toJoin.joinable()) toJoin.join();
}

inline bool fileLogActive() { return detail::fileLogState().running.load(); }

inline std::string fileLogPath()
{
    auto& st = detail::fileLogState();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.path;
}

inline std::string fileLogError()
{
    auto& st = detail::fileLogState();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.error;
}

} // namespace latency
} // namespace runtime
