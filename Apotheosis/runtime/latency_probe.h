#pragma once

// =============================================================================
// 端到端延迟探针 (End-to-end latency probe)
// =============================================================================
//
// 为什么需要它
// ------------
// 原来的 DetectionBuffer::stamp 打的是【推理完成、结果发布】的时刻, 不是
// 【像素被采集】的时刻。于是整条链路里最贵的几段 —— 采集卡、H264 解码、
// 预处理、推理本体 —— 从未被测量过, 系统对它们完全是瞎的。
//
// 后果: "预测速度"(lr_x/lr_y) 只能按经验手调。采集卡/分辨率/模型/fps 一变,
// 该值就不再正确 —— 欠补则准星恒定落后移动目标 (v × L), 过补则来回荡。
//
// 本探针给帧打上【采集时刻】时间戳, 一路带到 aim loop, 把总延迟按阶段拆开:
//
//   T0 markCapture()          采集后端产出一帧 (最早可观测时刻)
//   T1 markSubmit()           detector 取走该帧
//   T2 markInferenceDone()    推理完成、检测结果发布
//   T3 markAimConsume()       aim loop 消费该检测
//   T4 markMoveSent()         位移真正写出到鼠标
//
//   capture_wait   = T1 - T0   帧在采集侧排队/下载/掩码的耗时
//   inference      = T2 - T1   预处理 + 推理 + NMS
//   publish_to_aim = T3 - T2   发布 -> 控制环唤醒消费
//   aim_to_move    = T4 - T3   控制环 -> HID 写出
//   total          = T3 - T0   ★ 真正决定"准星落后多少"的那个数
//   e2e            = T4 - T0   全链路下界
//
// 不可观测的部分 (必须知道, 否则会误判)
// ------------------------------------
// 采集卡内部 HDMI -> USB 的流水线延迟 (典型 20-60ms) 在 PC 侧【物理上不可
// 测量】: 帧进到进程时, 卡里那段已经花掉了。本探针给出的 total/e2e 是【下界】。
// 真值需要用 240/480fps 手机同时拍屏幕与鼠标 LED, 数帧差 ÷ 帧率。
// 那个差额是一个近乎恒定的常数, 就是你在配置里需要手工兜住的部分。
//
// 开销
// ----
// 全无锁、零分配。每帧写 3-4 个原子量 + 一次短临界区, 量级 ~100ns/帧,
// 相对 5-30ms 的帧预算可以忽略。可长期常开。
//
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

// 采集后端刚产出一帧时调用。这是我们在 PC 侧能打的最早的时间点。
// 返回采集时刻 (纳秒), 调用方顺手喂给 noteCaptureForStats()。
inline int64_t markCapture()
{
    const int64_t ns = nowNs();
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
// 跨线程的一次性交接量 (detector -> aim loop)
// -----------------------------------------------------------------------------
inline std::atomic<int64_t>& submitNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

// detector 取帧时把该帧的采集戳一并存下, 推理线程发布时取回写进
// DetectionBuffer::frame_stamp_ns。走独立的原子量是为了不必给
// TrtDetector / DirectMLDetector 的类定义加成员 (跨线程, 但 processFrame
// 与推理线程之间已有 inferenceMutex/frameReady 的 happens-before)。
inline std::atomic<int64_t>& submittedCaptureNs()
{
    static std::atomic<int64_t> v{0};
    return v;
}

inline int64_t takeSubmittedCaptureNs()
{
    return submittedCaptureNs().load(std::memory_order_acquire);
}

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

inline const char* stageName(int id)
{
    switch (id)
    {
    case kCaptureWait:  return "采集->取帧";
    case kInference:    return "推理";
    case kPublishToAim: return "发布->消费";
    case kAimToMove:    return "消费->写出";
    case kTotal:        return "总延迟(T0->T3)";
    case kEndToEnd:     return "全链路(T0->T4)";
    default:            return "?";
    }
}

struct Counters
{
    uint64_t frames_consumed   = 0;  // aim loop 结算过的完整帧
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
inline int64_t markSubmit()
{
    const int64_t cap = loadCaptureNs();
    submitNs().store(nowNs(), std::memory_order_release);
    submittedCaptureNs().store(cap, std::memory_order_release);

    auto& sh = shared();
    auto& st = sh.stages[kCaptureWait];
    double wait_ms = 0.0;
    if (cap != 0) wait_ms = nsToMs(nowNs() - cap);
    st.push(wait_ms);

    // 采集序号跳变 => detector 没跟上, 中间有帧被覆盖丢弃。
    {
        std::lock_guard<std::mutex> lk(sh.mu);
        const uint64_t seq = loadCaptureSeq();
        if (seq > sh.last_capture_seq_seen + 1)
            sh.counters.dropped_capture += (seq - sh.last_capture_seq_seen - 1);
        sh.last_capture_seq_seen = seq;
        sh.last_capture_ns_seen  = cap;
    }
    return cap;
}

// -----------------------------------------------------------------------------
// detector 侧: 推理完成、即将发布检测结果。
// -----------------------------------------------------------------------------
inline void markInferenceDone()
{
    const int64_t sub = submitNs().load(std::memory_order_acquire);
    if (sub == 0) return;
    auto& sh = shared();
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.stages[kInference].push(nsToMs(nowNs() - sub));
}

// -----------------------------------------------------------------------------
// aim loop 侧: 消费了一条检测。
//   frame_capture_ns - DetectionBuffer::frame_stamp_ns (可能为 0 = 无戳)
//   publish_ns       - DetectionBuffer::stamp
//   queue_ms         - 现有 g_mouse_queue_latency_ms, 用于结算 T4
// -----------------------------------------------------------------------------
inline void markAimConsume(int64_t frame_capture_ns, int64_t publish_ns,
                           double queue_ms)
{
    auto& sh = shared();
    const int64_t now = nowNs();

    std::lock_guard<std::mutex> lk(sh.mu);
    sh.stages[kPublishToAim].push(
        publish_ns != 0 ? nsToMs(now - publish_ns) : 0.0);

    if (frame_capture_ns == 0)
    {
        sh.counters.stale_consumes++;
        return;
    }

    const double total_ms = nsToMs(now - frame_capture_ns);
    sh.stages[kTotal].push(total_ms);
    sh.stages[kAimToMove].push(queue_ms);
    if (queue_ms > 0.0)
        sh.stages[kEndToEnd].push(total_ms + queue_ms);
    sh.counters.frames_consumed++;
}

// -----------------------------------------------------------------------------
// 快照 (给 overlay / 遥测读取)
// -----------------------------------------------------------------------------
struct Snapshot
{
    bool     enabled             = true;
    uint64_t frames_consumed     = 0;
    uint64_t capture_frames      = 0;
    uint64_t dropped_capture     = 0;
    uint64_t stale_consumes      = 0;
    double   capture_fps         = 0.0;
    double   capture_interval_ms = 0.0;
    Stage    stages[kStageCount];
};

inline Snapshot snapshot()
{
    auto& sh = shared();
    Snapshot out;
    std::lock_guard<std::mutex> lk(sh.mu);
    out.enabled             = sh.enabled;
    out.frames_consumed     = sh.counters.frames_consumed;
    out.capture_frames      = sh.counters.capture_frames;
    out.dropped_capture     = sh.counters.dropped_capture;
    out.stale_consumes      = sh.counters.stale_consumes;
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
        return "延迟探针: 已关闭\n";

    if (s.frames_consumed == 0)
        return "延迟探针: 等待数据...\n";

    add("采集 %.1f fps (%.2f ms/帧)", s.capture_fps, s.capture_interval_ms);
    add("总延迟 %.1f ms  (均 %.1f / 峰 %.1f)",
        s.stages[kTotal].last_ms, s.stages[kTotal].ema_ms,
        s.stages[kTotal].max_ms);

    if (detail)
    {
        add("  采集->取帧  %5.1f ms", s.stages[kCaptureWait].last_ms);
        add("  推理        %5.1f ms", s.stages[kInference].last_ms);
        add("  发布->消费  %5.1f ms", s.stages[kPublishToAim].last_ms);
        add("  消费->写出  %5.1f ms", s.stages[kAimToMove].last_ms);
        add("  全链路      %5.1f ms",
            s.stages[kEndToEnd].last_ms > 0.0 ? s.stages[kEndToEnd].last_ms
                                              : s.stages[kTotal].last_ms);
        if (s.dropped_capture > 0)
            add("  !! detector 跟不上, 已丢 %llu 帧",
                static_cast<unsigned long long>(s.dropped_capture));
        addLine("  (不含采集卡内部延迟, 典型 +20~60 ms)");
    }
    else
    {
        add("  推理 %.1f | 消费 %.1f | 写出 %.1f",
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
                  s.stages[kEndToEnd].last_ms, s.stages[kEndToEnd].ema_ms,
                  s.stages[kEndToEnd].max_ms);
    out.push_back(buf);

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
        out.push_back("  (excl. cap-card internal 20-60ms)");
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
    char buf[320];
    std::snprintf(
        buf, sizeof(buf),
        "E2E=%.2f avg=%.2f pk=%.2f | cap2det=%.2f infer=%.2f pub2aim=%.2f "
        "aim2mv=%.2f total=%.2f | src=%.1ffps frames=%llu dropped=%llu stale=%llu",
        s.stages[kEndToEnd].last_ms, s.stages[kEndToEnd].ema_ms,
        s.stages[kEndToEnd].max_ms,
        s.stages[kCaptureWait].ema_ms, s.stages[kInference].ema_ms,
        s.stages[kPublishToAim].ema_ms, s.stages[kAimToMove].ema_ms,
        s.stages[kTotal].ema_ms,
        s.capture_fps, static_cast<unsigned long long>(s.frames_consumed),
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
                          "-> detector 跟不上, 中间帧被覆盖丢弃",
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
                          "-> 控制环读到了无采集戳的数据",
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
                logLine("IDLE no new frames (采集或推理已停更)");
            }
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
          << "# T0=frame enters process(capture) T1=detector picks up "
             "T2=inference published T3=aim loop consumes T4=move sent" << '\n'
          << "# total = T3-T0   <- THIS is the L in lag = v * L" << '\n'
          << "# e2e   = T4-T0" << '\n'
          << "# stages: cap2det=T1-T0  infer=T2-T1  pub2aim=T3-T2  aim2mv=T4-T3" << '\n'
          << "# unit: ms. EXCLUDES capture-card internal HDMI->USB (typ 20-60ms), "
             "so this is a LOWER BOUND." << '\n'
          << "# line kinds: periodic summary | EVENT drop/stale | SPIKE | IDLE" << '\n'
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
