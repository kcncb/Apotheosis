#pragma once

// =============================================================================
// 调参 agent —— 主循环 (靶场模式)
// =============================================================================
//
// 用户定义的用法 (2026-09-14 原话):
//   "就是我在靶场开agent调参改好，然后再关agent调参实战"
//
// 所以这是一个【显式开关的、只在靶场用的】后台线程:
//   开 -> 每隔 N 秒抓一段日志 -> 算指标 -> 问 LLM -> 夹取 -> 写参数 -> 重复
//   关 -> 线程停止, 参数保持不动, 完全回到普通模式
//
// ★ 六条安全设计 (这一层的存在理由就是"不让 agent 把参数搞坏"):
//   ① 独立线程, 不碰瞄准主循环 —— agent 卡住/网络超时都不影响瞄准
//   ② 只在开关打开时运行; 关掉后连日志采集都不做
//   ③ 参数落地前必过 autotune_safety 的夹取(单步 ±20%, beta 上调 ±10%)
//   ④ 发散/抖动/明显变差 -> 自动回滚到上一组, 不问模型
//   ⑤ 每一轮都记录(时间/参数/指标/模型原话), 事后可审计、可复现
//   ⑥ 提供"恢复初始参数"一键回退
//
// ★ 数据只用于【本机调参】: 发给 LLM 的是指标摘要与参数, 不含画面。
// =============================================================================

#include "mouse/autotune_llm.h"
#include "mouse/autotune_metrics.h"
#include "mouse/autotune_safety.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace boss::autotune {

// ── 调参模式 (2026-09-14) ───────────────────────────────────────────────────
//
// 用户原话: "加个模式选择 1.静态目标调参 2.动态目标调参 3.体感调参 也就是
// 用户输入自己的体感来调参, 每次只调一轮
//
// ★ 三个模式的【数据来源完全相同】: 都是"两次按钮点击之间的那段日志"。
//   区别只在【怎么判断这段数据好不好用】以及【给模型什么额外输入】。
//
// ★ 为什么坚持"一次只调一轮": 这是【人的节奏】而不是机器的节奏。
//   调完一轮你要自己甩几枪、跟几下, 用肌肉去判断变好还是变坏。连调十轮你
//   根本分不清是哪一轮起的作用 —— 而且中间几轮的数据是"你还在适应新参数"
//   的过渡态, 拿它做判断本身就是错的。所以这里没有定时循环, 只有
//   "开始采样 → 你打 → 结束 → 出一轮结果"。
enum class TuneMode
{
    Static = 0,      // 静态目标: 只判收敛、过冲、抖动
    Moving = 1,      // 动态目标: 判跟随滞后、机动后的再咬合
    Feel   = 2,      // 体感: 用户用文字描述感受, 模型结合数字一起判
};

// 把模式编成给模型看的中文名
const char* mode_name(TuneMode m);

// ── 一次采样会话 ────────────────────────────────────────────────────────────
//
// ★★ 2026-09-14 第二次改版: 采样范围由【用户按的两个按钮】界定 ★★
//
//   点「开始采样」 → 在 chainlog 里【打一条起始标记】
//   你去打(静止靶 / 动态靶, 随你)
//   点「结束并调一轮」 → 打一条结束标记, 取【两条标记之间】的全部日志
//
// ★★ 为什么不按"按住瞄准热键"切段 ★★
//   ① 实测发现旧实现的判据根本用不了 —— 见下面 TrackSegment 的注释:
//      `target_switched` 的真实含义是"瞄点跳变≥25px", 也就是【玩家甩枪】,
//      正常跟枪里每次甩都会触发, 于是"中途换目标"这条判据把几乎每一段
//      都判成脏数据, 用户侧的表现就是"一直提示我中途换了目标"。
//   ② 更根本的原因: 用【我的意图】当边界, 比用【程序的信号】当边界可靠得多。
//      "我按了开始, 现在开始算"是一个不需要任何解释的约定。
struct TrackSegment
{
    std::vector<Sample> samples;
    double t_begin = 0.0;        // 段内首条样本的时刻
    double t_end   = 0.0;        // 段内末条样本的时刻
    double duration() const { return t_end - t_begin; }

    // 覆盖率: 段内【真正连续的那部分】占的比例。
    //
    // ★★ 2026-09-14 第三次修正 —— 这个量之前算错过两次, 记在这里 ★★
    //
    //   旧公式: coverage = n ÷ ( 跨度 ÷ 平均dt )
    //   两个缺陷:
    //   ① **分母把"程序没在跑"的时间也算进去了。** 实测用户的日志里有
    //      9.5s / 9.9s / 12s / 18s / 29.6s 这些空洞(那段时间瞄准循环没有
    //      输出帧), 跨度和平均 dt 相除就把这些死时间算成"本该有的样本数",
    //      于是覆盖率被判成 32%, 每一段都过不了 50% 的门限。
    //   ② **平均 dt 会偏大。** 实测 mean(dt_ms)=9.26ms 而典型步进只有
    //      8.3ms(慢帧把均值拉高了), 所以即使一段完全连续, 算出来也是
    //      111% 而不是 100%。
    //
    //   更根本的问题: 那个公式在【用一个可能错的模型去推测本该有多少条】。
    //   dt_ms 是引擎自己报的每拍间隔, 本来就记录了"这一拍之前空了多久"
    //   —— 直接看它就够了, 不需要推测。
    //
    //   新定义: coverage = 正常间隔的样本数 ÷ 总样本数。
    //   "正常间隔" = dt_ms ≤ 中位数的 4 倍。丢帧和空洞都会被排除在分子外,
    //   于是它直接回答"这段数据里有多大比例是连续采到的"。
    double coverage = 1.0;

    // 段内的【位置突变】次数(>40px)。
    //
    // ★★ 注意这里统计的是 `jump_px`, **不是** `target_switched`。
    //   `boss_aim.cpp` 里 `target_switched = motion_suppressed && anchor_jump>=25px`,
    //   即"瞄点跳 ≥25px" —— 那是【玩家在甩枪】, 不是"换了目标"。
    //   拿它当"数据不干净"的判据会误杀几乎每一个真实采样。
    //   真正该关心的是野值: 检测框瞬间跳到完全无关的位置(会污染误差统计)。
    int jumps = 0;

    // 框中途丢失的次数。
    // ★ 用 `coverage` 之外再单独看它: 用户可能中途松了热键(没检测行),
    //   那段时间段内没有样本, coverage 会掉但 doesn't 需要拒绝 ——
    //   只要剩下的样本还够、还连续就行。
    int gaps = 0;

    // 框高是否稳定。★ 用它区分"静态靶"和"动态靶": 动态靶在画面里远近变化,
    // 框高会跟着变; 静态靶站着不动, 框高基本恒定。也用来判"你是不是在拉近拉远"。
    double bbox_h_median = 0.0;
    double bbox_h_p10 = 0.0;
    double bbox_h_p90 = 0.0;
    // 相对离散度 = (p90-p10)/median。0.35 以内认为尺寸稳定。
    double bbox_h_spread() const
    {
        return bbox_h_median > 1e-6 ? (bbox_h_p90 - bbox_h_p10) / bbox_h_median : 0.0;
    }
};

// 一段数据能不能用来调参 —— 独立于 LLM 的硬判据。
// ★ 不满足就不发请求, 直接告诉用户"请重新采样"。
//   为什么必须硬判: 拿一段"根本没锁到目标"的数据去调参, 模型会看到一堆
//   无意义的误差, 然后合理地给出错误的调整方向。宁可不出结果。
//
// ★★ 2026-09-14 第二次改版: 判据大幅放宽 ★★
//   旧版有一条"中途换目标必须为 0", 而它读的是 `target_switched`
//   (= 瞄点跳≥25px = 玩家甩枪) —— 正常跟枪里每次甩都触发, 于是几乎每一段
//   都被判成脏数据。**判据比被测现象更容易出错**, 这条被删掉了。
//   现在只查真正会导致指标失真的事: 样本够不够、连不连续、有没有野值。
struct SegmentVerdict
{
    bool ok = false;
    std::string reason;          // 不通过时的中文原因(直接显示给用户)
};

SegmentVerdict judge_segment(const TrackSegment& seg, TuneMode mode);

// ── 一轮的记录(给 UI 显示 + 事后翻账)
struct Round
{
    int index = 0;
    double wall_s = 0.0;             // 相对 agent 启动的时刻
    TuneMode mode = TuneMode::Static;
    // 这一轮用户选的改动幅度。★ 记进历史里, 因为"改了多少"和"结果如何"
    // 必须一起看才能判断: 同一组指标, ±10% 和 ±50% 得到的结果完全不同。
    StepStyle style = StepStyle::Steady;
    Metrics metrics;                 // 本轮实测指标
    TrackSegment segment;            // 本轮用的原始数据(供 UI 显示覆盖度等)
    std::string feel_text;           // 体感模式下用户的原话
    Knobs requested;                 // 模型想要的
    Knobs applied;                   // 实际生效的(夹取后)
    std::string clamp_notes;         // 夹取说明
    std::string model_reply;         // 模型原话(截断)
    std::string error;               // 本轮失败原因(空 = 成功)
    bool rolled_back = false;
    std::string rollback_reason;
};

// ── 日志采集 ────────────────────────────────────────────────────────────────
// 与其他线程解耦: 外部每帧把 chainlog 的最新若干行丢进来, 这里只保留
// 【最近 kMaxSamples 条】的样本。
//
// ★ 为什么要有 push() 的返回值: 采样会话要记住"按下热键那一刻"对应哪条样本,
//   而环形缓冲会不断淘汰旧样本。返回一个【永不重复的绝对序号】, 会话拿它当
//   下标的锚点, 就能在结束时准确切出"按下到松开"那一段 —— 即使中间淘汰了
//   很多旧样本也不会切错。
class SampleBuffer
{
public:
    // 返回这条样本的绝对序号(单调递增, 不因淘汰而变)。
    long long push(const Sample& s)
    {
        std::lock_guard<std::mutex> g(mutex_);
        buf_.push_back(s);
        ++pushed_;
        while (buf_.size() > kMaxSamples) buf_.pop_front();
        return pushed_ - 1;
    }

    std::size_t size() const { std::lock_guard<std::mutex> g(mutex_); return buf_.size(); }
    void clear() { std::lock_guard<std::mutex> g(mutex_); buf_.clear(); pushed_ = 0; }

    // 当前已推入的绝对序号(下一条将是这个值)
    long long pushed() const { std::lock_guard<std::mutex> g(mutex_); return pushed_; }
    // 缓冲里最旧一条的绝对序号。若请求的区间已被淘汰, 调用方据此收敛范围。
    long long oldest() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return pushed_ - static_cast<long long>(buf_.size());
    }

    // 取 [abs_begin, abs_end) 这段绝对序号的样本。
    // ★ 区间被淘汰的部分会被自动裁掉(返回的段可能比请求的短), 而不是报错 ——
    //   采样太久导致最早的样本被淘汰时, 我们尽量利用剩余数据, 够不够
    //   由 judge_segment 去决定。
    std::vector<Sample> range(long long abs_begin, long long abs_end) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        std::vector<Sample> out;
        const long long lo = pushed_ - static_cast<long long>(buf_.size());
        long long b = abs_begin < lo ? lo : abs_begin;
        long long e = abs_end   > pushed_ ? pushed_ : abs_end;
        if (b < 0) b = 0;
        if (e <= b) return out;
        out.reserve(static_cast<std::size_t>(e - b));
        for (long long i = b; i < e; ++i)
            out.push_back(buf_[static_cast<std::size_t>(i - lo)]);
        return out;
    }

private:
    static constexpr std::size_t kMaxSamples = 8192;   // 120fps 下约 68 秒
    mutable std::mutex mutex_;
    std::deque<Sample> buf_;
    long long pushed_ = 0;
};

// ── agent ───────────────────────────────────────────────────────────────────
class AutoTuner
{
public:
    // 取当前参数 / 落地新参数 —— 由接入层(运行时)提供, 这样 agent 不直接依赖 config。
    using KnobsGetter = std::function<Knobs()>;
    // ★ setter 返回"是否真的生效了", 并把失败原因写进 err。
    //   见 RuntimeHooks::write 的说明: "写进文件"不等于"运行中用上了",
    //   而后者才是用户体感得到的那个。
    using KnobsSetter = std::function<bool(const Knobs&, std::string& err)>;
    // 拿到"这一轮用的指标", 用于 UI 显示
    using RoundCallback = std::function<void(const Round&)>;

    AutoTuner() = default;
    ~AutoTuner();

    AutoTuner(const AutoTuner&) = delete;
    AutoTuner& operator=(const AutoTuner&) = delete;

    void set_llm(LlmConfig cfg) { llm_ = std::move(cfg); }
    LlmConfig llm() const { return llm_; }
    void set_getter(KnobsGetter g) { get_ = std::move(g); }
    void set_setter(KnobsSetter s) { set_ = std::move(s); }
    void set_callback(RoundCallback c) { on_round_ = std::move(c); }

    // ★ 注入"模型回答"的方式 (仅供逻辑测试)。
    //
    //   为什么需要它: `run_one_round()` 里【真正决定成败】的那一段在 LLM 之后
    //   —— 夹取、落地、以及"没生效就不算成功"的判定。而网络不可重复, 所以
    //   测不到那一段就只能去测周边的小函数, 抓不到"判据本身写错了"这种 bug
    //   (实际发生过: 无视 set_ 返回值时, 周边测试全绿)。
    //   注入之后, 提示词构造、JSON 解析、夹取、落地确认全部走真实代码路径。
    using ReplyFn = std::function<LlmResult(const LlmConfig&,
                                            const std::string& sys,
                                            const std::string& usr)>;
    void set_reply_fn(ReplyFn f) { reply_fn_ = std::move(f); }

    SampleBuffer& buffer() { return buffer_; }
    const SampleBuffer& buffer() const { return buffer_; }

    void set_mode(TuneMode m) { mode_ = m; }
    TuneMode mode() const { return mode_; }
    void set_feel_text(std::string t) { feel_text_ = std::move(t); }
    const std::string& feel_text() const { return feel_text_; }

    // 本轮改动幅度(保守/平稳/激进)。★ 同时作用于【提示词】和【硬夹取】,
    // 见 autotune_safety.h 里 StepStyle 的说明 —— 两处必须一致。
    void set_step_style(StepStyle s) { step_style_ = s; }
    StepStyle step_style() const { return step_style_; }

    // ── 采样会话 ────────────────────────────────────────────────────────────
    //
    // ★ 调参的节奏是人的节奏: 点开始 → 你打 → 点结束 → 出一轮。
    //   这里【没有】后台循环, 也没有"自动连调"。
    //
    // ★★ 边界完全由用户的两个按钮决定, 不读任何引擎信号 ★★
    //   `begin_session()` 记下当前日志位置作起点, `end_session()` 记终点,
    //   取两者之间的全部样本。这样 agent 层【不依赖任何引擎状态】——
    //   它要能在纯逻辑测试里编译、也能在任何接入方式下工作。
    void begin_session();                 // 「开始」按钮: 打起始标记
    void end_session();                   // 「结束」按钮: 打结束标记 + 调一轮
    bool session_open() const { return session_open_.load(); }

    // 会话已采到的样本数(供界面显示"正在采集中 · 已 N 条")
    std::size_t session_sample_count() const;

    // 拿最近一次采到的段(供 UI 显示"采了多少 / 覆盖度多少"), 没有则 false
    bool last_segment_info(TrackSegment& out) const;

    // 历史(供 UI 显示)
    std::vector<Round> history() const;
    void clear_history();

    // ★ 仅供逻辑测试: 拿一段给定的数据跑一整轮(含 LLM/夹取/落地确认),
    //   并把这一轮的记录返回。生产路径用 end_session() —— 它从样本缓冲取段。
    //   存在的理由见 set_reply_fn(): 真正决定成败的判据在网络之后, 不注入
    //   回答就永远测不到那一段。
    Round end_session_for_test(const TrackSegment& seg);

private:
    // ★ 同步执行一轮。调用者是 UI 线程(「结束」按钮), 会在网络请求期间
    //   阻塞。这是【有意】的: 一轮只发一次请求, 而且用户刚按完「结束」,
    //   本来就在等结果。比开个后台线程再回 UI 要简单可靠得多。
    Round run_one_round(int index, const TrackSegment& seg);

    // 把 [begin,end) 区间封成段
    TrackSegment close_segment() const;

    LlmConfig llm_;
    ReplyFn reply_fn_;          // 空 = 走真实网络 (生产默认)

    KnobsGetter get_;
    KnobsSetter set_;
    RoundCallback on_round_;

    SampleBuffer buffer_;

    // ── 会话状态 ────────────────────────────────────────────────────────────
    std::atomic<bool> session_open_{false};
    // ★ 用【绝对序号】而不是缓冲区下标: 环形缓冲会淘汰旧样本, 下标会漂。
    //   绝对序号单调递增、不因淘汰而变, 所以切出来的永远是"标记之间的那一段"。
    long long seg_begin_idx_ = -1;
    long long seg_end_idx_ = -1;

    TuneMode mode_ = TuneMode::Static;
    StepStyle step_style_ = StepStyle::Steady;
    std::string feel_text_;

    mutable std::mutex seg_mutex_;
    TrackSegment last_seg_;              // 最近一次封好的段(供 UI 显示)
    bool have_last_seg_ = false;

    mutable std::mutex hist_mutex_;
    std::vector<Round> history_;
    Knobs last_good_;            // 上一组"已知可用"的参数(回滚目标)
    bool have_last_good_ = false;
    Metrics prev_metrics_;
    bool have_prev_metrics_ = false;
};

// 把 Knobs 编成给模型看的文本(由实现提供, 便于集中改提示词)
std::string build_system_prompt();
// ★ 带模式与段信息。feel_text 只在体感模式下非空。
// ★ style = 用户选的"本轮改动幅度", 会写进提示词告诉模型这一轮允许改多大。
std::string build_user_prompt(const Knobs& current, const Metrics& m,
                              const TrackSegment& seg, TuneMode mode,
                              const std::string& feel_text,
                              int round_index, const std::string& last_note,
                              StepStyle style = StepStyle::Steady);
// 从模型回答里解析出想要的参数。失败返回 false。
bool parse_knobs_json(const std::string& json, const Knobs& current, Knobs& out,
                      std::string& err);

} // namespace boss::autotune
