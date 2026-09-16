// =============================================================================
// 调参 agent —— 运行时接入层实现
// =============================================================================

#include "mouse/autotune_runtime.h"
#include "runtime/chain_log.h"

#include <algorithm>
#include <cstdio>

namespace boss::autotune
{

Runtime& Runtime::instance()
{
    static Runtime inst;
    return inst;
}

void Runtime::install(RuntimeHooks hooks)
{
    std::lock_guard<std::mutex> g(mutex_);
    hooks_ = std::move(hooks);
    have_hooks_ = (hooks_.read && hooks_.write) ? true : false;

    // 接线: agent 从这里读/写参数。
    // ★ setter 会把"没生效"的原因带回去(见 RuntimeHooks::write)。
    tuner_.set_getter([this]() -> Knobs {
        std::lock_guard<std::mutex> lg(mutex_);
        return hooks_.read ? hooks_.read() : Knobs{};
    });
    tuner_.set_setter([this](const Knobs& k, std::string& err) -> bool {
        std::lock_guard<std::mutex> lg(mutex_);
        if (!hooks_.write) { err = u8"未接入参数写入(内部错误)"; return false; }
        return hooks_.write(k, err);
    });
}

bool Runtime::installed() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return have_hooks_;
}

void Runtime::feed_from_chainlog()
{
    // 取序号与可读条数。★ 这一段与 agent 开关无关 —— 基准框高的环形缓冲
    // 需要【一直】有数据, 因为"以当前距离设为基准"这个按钮随时可能被按下,
    // 而按下那一刻之前几秒的框高必须已经在手上。
    auto& s = runtime::chainlog::sink();
    std::int64_t seq = 0;
    std::int64_t total = 0;
    {
        std::lock_guard<std::mutex> g(s.mutex);
        seq = s.sequence;
        total = (seq < runtime::chainlog::kCapacity) ? seq : runtime::chainlog::kCapacity;
    }
    if (seq == last_seq_) return;

    // 第一次接入: 从"当前缓冲的最旧一条"开始, 不要把历史全灌进来
    if (last_seq_ == 0)
    {
        last_seq_ = seq - total;
        if (last_seq_ < 0) last_seq_ = 0;
    }
    // 序号被环形缓冲甩掉太远(比如中途关过 agent): 直接跳到最旧可读处
    if (seq - last_seq_ > total)
        last_seq_ = seq - total;

    // ★ 会话没开时也要走完这一段(为了喂框高环), 但【不做解析之外的任何事】:
    //   会话关闭 = 不收样本、不写参数。这里只把框高喂进环。
    //   ★ 采样会话开着时才把样本喂给 tuner 的样本缓冲。
    const bool collecting = tuner_.session_open();

    for (std::int64_t i = last_seq_; i < seq; ++i)
    {
        const std::int64_t slot = i % runtime::chainlog::kCapacity;
        char line[runtime::chainlog::kLineSize];
        {
            std::lock_guard<std::mutex> g(s.mutex);
            std::snprintf(line, sizeof(line), "%s", s.lines[slot]);
        }
        if (line[0] == '\0') continue;
        Sample sample;
        if (!parse_pid_line(line, sample)) continue;

        // ① agent 的样本缓冲: 只在采样会话开着时喂。
        //    ★ 会话没开就不收 —— 缓冲只装"用户明确要采的那一段", 结束之后
        //      紧接着进来的帧不会污染它。(end_session 里是用【绝对序号】
        //      切的区间, 所以即使后面又推进了也不会串味。)
        if (collecting)
        {
            // chainlog 里的 t 是【全局单调时钟秒】(now_seconds 的原点),
            // 而样本窗口是按 t 的差值算的 —— 直接沿用即可。
            tuner_.buffer().push(sample);
        }

        // ② 基准框高的环形缓冲: 【一直】喂, 与开关无关。
        //    采【原始框高】而不是平滑后的 —— 中位数本身对离群值稳健。
        sample_target_height(sample.bbox_h);
    }
    last_seq_ = seq;
}

void Runtime::set_llm(const LlmConfig& cfg)
{
    tuner_.set_llm(cfg);
}

// ── 采样会话 (2026-09-14 改版) ──────────────────────────────────────────────
//
// ★ 从"开个后台线程一直调"改成"点开始 → 你打 → 点结束出一轮"。
//   理由见 AutoTuner 头文件: 调参的节奏是人的节奏, 连调十轮你分不清是
//   哪一轮起的作用, 中间几轮的数据还是"你正在适应新参数"的过渡态。
//
// ★★ 边界 = 用户按的两个按钮, 【不读任何引擎信号】★★
//   上一版读"瞄准热键是否按下", 配套的"中途换目标"判据又误读了
//   `target_switched`(它其实是"瞄点跳≥25px"= 玩家甩枪), 于是几乎每一段
//   都被判成脏数据。现在改成: 按「开始」打标记, 按「结束」取两者之间的日志。

void Runtime::begin_session()
{
    // 记录"开始调参之前"的参数, 供一键回退。
    // ★ 只在第一次记, 不是每次开始都覆盖 —— 否则连调几轮后"恢复最初"
    //   会恢复到上一轮而不是真正的初始参数。
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (hooks_.read && !have_initial_)
        {
            initial_ = hooks_.read();
            have_initial_ = true;
        }
    }
    // ★★ 这里【不】重置 last_seq_ ★★
    //
    //   曾经这里有一行 `last_seq_ = 0;`, 那是 bug, 症状是"这段数据只有 0 条"。
    //
    //   原因: feed_from_chainlog() 里有这么一段 ——
    //
    //       if (last_seq_ == 0)
    //       {
    //           last_seq_ = seq - total;      // 跳到"当前缓冲最旧一条"
    //           ...
    //       }
    //
    //   它的用意是"第一次接入时不要把历史全灌进来"。但把 last_seq_ 归零之后,
    //   下一次 feed 会认为自己是"第一次接入", 于是把起点【重新设到当前最旧】,
    //   而 seq 也被赋成同一个值 —— 循环 `for (i = last_seq_; i < seq; ++i)`
    //   一条都不跑。结果: 点「开始」之后, 永远采不到任何样本。
    //
    //   ★ 教训: 一个变量如果有"哨兵值"(这里是 0 = 尚未初始化), 就不要拿它
    //     当"重置"用。重置和未初始化是两件不同的事, 复用同一个值会让
    //     "重新开始"被误读成"第一次开始"。
    //
    //   现在 last_seq_ 保持不动: 它本来就一直指向"已消费到哪一条",
    //   点「开始」只是让 tuner 在【当前绝对序号】上打个起点标记
    //   (AutoTuner::begin_session 里的 seg_begin_idx_ = buffer_.pushed()),
    //   两者互不干扰, 也不需要动 last_seq_。
    tuner_.begin_session();
}

void Runtime::end_session()
{
    // ★ 调用者是 UI 线程, 这里会阻塞到一轮跑完(含网络请求)。
    //   这是有意的: 用户刚按完「结束」, 本来就在等结果。见 AutoTuner 头文件。
    tuner_.end_session();
}

bool Runtime::session_open() const
{
    return tuner_.session_open();
}

std::size_t Runtime::session_sample_count() const
{
    return tuner_.session_sample_count();
}

void Runtime::set_mode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    tuner_.set_mode(static_cast<TuneMode>(mode));
}

int Runtime::mode() const
{
    return static_cast<int>(tuner_.mode());
}

// ── 本轮改动幅度 (2026-09-15) ───────────────────────────────────────────────
// ★ 越界一律夹到合法档, 不抛异常 —— 这个值来自 UI, 而 UI 的持久化数据
//   可能来自更老的版本(比如键不存在或值被手改过)。
void Runtime::set_step_style(int style)
{
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    tuner_.set_step_style(static_cast<StepStyle>(style));
}

int Runtime::step_style() const
{
    return static_cast<int>(tuner_.step_style());
}

void Runtime::set_feel_text(const std::string& t)
{
    tuner_.set_feel_text(t);
}

// ── 基准框高 H₀ 的学习 (手动按钮) ──────────────────────────────────────────
//
// ★ 2026-09-14 改版: 从"开 agent 时自动学"改成手动按钮。
//   原因见头文件 —— 关键是不开 agent 就永远学不到, 而"参数在哪个距离调的"
//   跟"agent 有没有在跑"是两件独立的事。

void Runtime::sample_target_height(double bbox_h)
{
    // ★ 这个函数在瞄准热路径上每帧被调用, 所以:
    //   · 不加锁(单写者: 只有瞄准线程写; 读的是 UI 线程, 靠 atomic 槽位保证
    //     不会读到半个 double。最坏只是取到略旧的值, 而"最近 7 秒的中位数"
    //     对这种误差完全不敏感)
    //   · 不做分配(环形缓冲)
    //   · 非法值直接跳过
    if (!(bbox_h > 0.0) || bbox_h > 4000.0) return;
    height_ring_[height_ring_pos_].store(bbox_h, std::memory_order_relaxed);
    height_ring_pos_ = (height_ring_pos_ + 1) % kHeightRingSize;
    const std::size_t f = height_ring_filled_.load(std::memory_order_relaxed);
    if (f < kHeightRingSize)
        height_ring_filled_.store(f + 1, std::memory_order_relaxed);
}

bool Runtime::set_baseline_from_recent()
{
    const std::size_t n = height_ring_filled_.load(std::memory_order_acquire);
    if (n < kMinBaseSamples)
        return false;                 // 数据太少: 不凭几帧就定基准

    std::vector<double> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n && i < kHeightRingSize; ++i)
    {
        const double h = height_ring_[i].load(std::memory_order_relaxed);
        if (h > 0.0) v.push_back(h);
    }
    if (v.size() < kMinBaseSamples) return false;

    std::sort(v.begin(), v.end());
    // ★ 中位数而不是均值: 遮挡/出画会产生离群的小框, 均值会被拉偏。
    const double h = v[v.size() / 2];
    if (!(h > 0.0) || h > 4000.0) return false;

    learned_base_.store(h, std::memory_order_release);
    // 落盘。★ 走 hooks 而不是直接改 config —— 这一层不依赖 config。
    if (hooks_.write_base_h)
        hooks_.write_base_h(h);
    return true;
}

double Runtime::learned_baseline() const
{
    const double local = learned_base_.load(std::memory_order_acquire);
    if (local > 0.0) return local;
    return hooks_.read_base_h ? hooks_.read_base_h() : 0.0;
}

int Runtime::recent_height_samples() const
{
    return static_cast<int>(height_ring_filled_.load(std::memory_order_acquire));
}

Knobs Runtime::current_knobs() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return hooks_.read ? hooks_.read() : Knobs{};
}

void Runtime::restore_initial()
{
    std::lock_guard<std::mutex> g(mutex_);
    if (!have_initial_ || !hooks_.write) return;
    std::string err;
    // ★ 回退同样要验证: "点了一键回退但参数没动"比不回退更让人迷惑。
    hooks_.write(initial_, err);
}

// ★ 仅供逻辑测试: 验证"写回失败的原因能穿过这一层传出去"。
bool Runtime::write_knobs_for_test(const Knobs& k, std::string& err)
{
    std::lock_guard<std::mutex> g(mutex_);
    if (!hooks_.write) { err = u8"未接入参数写入(内部错误)"; return false; }
    return hooks_.write(k, err);
}

std::string Runtime::status_text() const
{
    if (!installed()) return u8"未接入(内部错误)";

    const auto hist = tuner_.history();
    char buf[320];

    if (tuner_.session_open())
    {
        std::snprintf(buf, sizeof(buf),
            u8"● 正在采样(模式: %s) · 已记录 %zu 条 · 历史 %zu 轮\n"
            u8"  去打吧(甩枪/跟枪/换目标都随意), 打完了点「结束并调一轮」。",
            mode_name(tuner_.mode()), tuner_.session_sample_count(), hist.size());
        return std::string(buf);
    }

    if (hist.empty())
        std::snprintf(buf, sizeof(buf),
            u8"○ 空闲 · 已完成 0 轮\n"
            u8"  选好模式 → 点「开始采样」→ 正常去打 → 点「结束并调一轮」。");
    else
        std::snprintf(buf, sizeof(buf),
            u8"○ 空闲 · 已完成 %zu 轮 · 上一次: %s",
            hist.size(),
            hist.back().error.empty()
                ? u8"成功(参数已更新)"
                : u8"未出结果(见下方详情)");
    return std::string(buf);
}

} // namespace boss::autotune
