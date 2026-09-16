#pragma once

// =============================================================================
// 调参 agent —— 运行时接入层
// =============================================================================
//
// 职责: 把三样东西接起来
//   ① chainlog 的 SecPid 记录  ->  agent 的样本缓冲(每帧喂一条)
//   ② agent 想要的参数          ->  真实 config + 运行时快照(publish)
//   ③ agent 的开关/历史/状态    ->  Qt 界面
//
// ★ 为什么要有单独一层而不是让 agent 直接碰 config
//   · 全局可访问的单例(界面、瞄准线程、agent 线程都要用)
//   · 读参数要能从【运行时快照】读, 写参数要同时更新 config 与快照,
//     并把配置标脏以便持久化 —— 这些细节不该散在 agent 里
//   · 关掉 agent 时这里保证【不再有任何写入】
//
// ★ 输入采集用【拉】而不是【推】
//   瞄准主循环每帧调用 feed_from_chainlog(), 由这一层去读 chainlog 环形缓冲的
//   新增部分。这样主循环只多一次很轻的调用, 不需要在热路径上做解析之外的事。
// =============================================================================

#include "mouse/autotune_agent.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace boss::autotune {

// 参数进出的抽象 —— 由 mouse_thread_loop / ConfigBridge 实现细节。
//
// ★ 写回为什么走"改配置 + 复用 live_tune"而不是直接改内存
//   程序里已经有一条【完全正确】的重载路径 (ConfigProfiles::load 里的
//   `config.loadConfig()` + `runtime_config::publish()` + `syncFromRuntime()`),
//   live_tune 就是它的触发器。自己再写一套"直接改 config 结构再 publish"会
//   有两份真相: 界面上的输入框不会跟着变, 用户看到的参数和实际在跑的不一致 ——
//   这正是 CLAUDE.md 里记过的"配置文件里是 100, 实际在跑 20"那类事故。
//   所以这里只做两件事: 读当前值; 把新值写成一份完整配置并交给 live_tune。
struct RuntimeHooks
{
    // 从当前生效配置读出一组 Knobs
    std::function<Knobs()> read;
    // 写回: 应把参数写进配置文件, 并触发与界面相同的重载路径。
    //
    // ★★ err 是【出参】, 不是可选的 (2026-09-15 改) ★★
    //   原来这里是 `bool(const Knobs&)` —— 失败只有一个"false", 没有原因。
    //   而这次的事故恰恰是【写盘成功、运行时不采用】: 界面显示"已应用",
    //   参数却停在旧值上, 用户体感"根本没变化"。
    //   把原因带出来之后, agent 才能把"想要 / 文件里 / 实际在跑"三者一起
    //   显示给用户, 让失败可定位而不是只能猜。
    std::function<bool(const Knobs&, std::string& err)> write;
    // 读写【基准框高】(尺度调度的参照点)。单位像素, 0 = 还没学到。
    // ★ 与 Knobs 分开是有意的: 基准不是模型能"调"的参数, 而是 agent 从真实数据
    //   里【学】出来的观测值。让 LLM 去猜"用户当时在多远"是荒谬的。
    std::function<double()> read_base_h;
    std::function<bool(double)> write_base_h;
};

// 单例接入层
class Runtime
{
public:
    static Runtime& instance();

    void install(RuntimeHooks hooks);
    bool installed() const;

    AutoTuner& tuner() { return tuner_; }

    // 瞄准主循环每帧调用: 把 chainlog 里【新增的】 SecPid 行喂给样本缓冲。
    // ★ 必须是廉价操作: 内部只按序号 diff, 没有新记录时立刻返回。
    void feed_from_chainlog();

    // LLM 配置的读写(界面用)
    void set_llm(const LlmConfig& cfg);
    LlmConfig llm() const { return tuner_.llm(); }

    // ── 采样会话 (2026-09-14) ───────────────────────────────────────────────
    //
    // ★★ 从"开个后台线程一直调"改成【开始 / 结束】两个按钮 ★★
    //
    //     开始 → 你打(静止靶/动态靶随你) → 结束 → 出一轮结果
    //
    //   调参的节奏是人的节奏: 连调十轮你根本分不清是哪一轮起的作用, 而且
    //   中间几轮的数据是"你还在适应新参数"的过渡态 —— 拿它做判断本身就是错的。
    //
    // ★★ 边界完全由这两个按钮界定, 【不读任何引擎信号】★★
    //   上一版读"瞄准热键是否按下", 配套的判据又误读了 `target_switched`
    //   (它在 boss_aim.cpp 里的真实含义是"瞄点跳≥25px", 也就是玩家甩枪),
    //   于是几乎每一段都被判成"中途换了目标"。现在改成按按钮打标记、
    //   取两次标记之间的日志 —— agent 层不依赖任何引擎状态, 也不会被误触发。
    void begin_session();                     // 「开始」: 打起始标记
    void end_session();                       // 「结束」: 打结束标记 + 调一轮
    bool session_open() const;
    std::size_t session_sample_count() const; // 本次会话已采到多少条(界面显示)

    void set_mode(int mode);                  // 0 静态 / 1 动态 / 2 体感
    int mode() const;
    // 本轮改动幅度: 0 保守(±10%) / 1 平稳(±20%, 默认) / 2 激进(±50%)
    // ★ 同时作用于提示词与硬夹取 —— 见 autotune_safety.h 的 StepStyle。
    void set_step_style(int style);
    int step_style() const;
    void set_feel_text(const std::string& t); // 体感模式下用户的原话

    // 把当前运行参数读出来(界面显示)
    Knobs current_knobs() const;

    // ── 基准框高 H₀ 的学习 ─────────────────────────────────────────────────
    //
    // ★★ 2026-09-14 改版: 从"开 agent 时自动学"改成【手动按钮】 ★★
    //
    //   原设计只在开 agent 期间采, 那有个明显漏洞: 【不开 agent 就永远学不到】,
    //   尺度调度恒为中性 —— 用户会以为这个功能坏了。而 H₀ 真正需要的是
    //   "我的参数是在哪个距离上调出来的", 这跟 agent 有没有在跑【是两件事】:
    //     · 你可能手动在某个距离调好了参数, 根本没开 agent
    //     · 你可能先调好参数, 才想起来还有这个功能
    //     · agent 调的只是那几个 PID 值, 与"基准在哪"无关
    //   而且原来只在【关 agent】时才落盘, 开着 agent 直接关程序那次采样就白采了。
    //
    //   现在: 界面上一行「以当前距离设为基准」按钮。程序持续维护一个最近窗口的
    //   框高环形缓冲(与 agent 开关无关), 按下按钮就取那个窗口的中位数存盘。
    //   语义清晰、用户完全控制、跟 agent 解耦。
    void sample_target_height(double bbox_h);   // 每帧调用(与开关无关, 极廉价)
    bool set_baseline_from_recent();            // 按钮: 用最近窗口的中位数
    double learned_baseline() const;            // 当前生效的基准(0 = 未学到)
    int recent_height_samples() const;          // 最近窗口里攒了多少条

    // 一键恢复到【开始调参之前】的参数
    void restore_initial();
    bool has_initial() const { return have_initial_; }

    // 状态文本(界面显示)
    std::string status_text() const;

    // ★ 仅供逻辑测试: 走一次"写回并验证"的接线, 断言失败原因能传出来。
    //   生产路径不调用它 —— 参数写回只发生在 agent 的 run_one_round 里。
    bool write_knobs_for_test(const Knobs& k, std::string& err);

private:
    Runtime() = default;

    AutoTuner tuner_;
    RuntimeHooks hooks_;
    bool have_hooks_ = false;

    std::int64_t last_seq_ = 0;      // 已消费到的 chainlog 序号
    Knobs initial_;
    bool have_initial_ = false;

    // 最近窗口的框高环形缓冲。★ 与 agent 开关无关 —— 每帧都进, 因为按钮随时
    // 可能被按下, 而"按下那一刻之前几秒"的数据必须已经在手上。
    // 用环形缓冲而不是 vector: 它要在瞄准热路径上每帧写一次, 不能有分配。
    //
    // ★ 线程安全: 写者是瞄准线程(每帧), 读者是 UI 线程(按按钮)。
    //   vector<double> 在这种"一边写一边读"下是真正的数据竞争, 所以:
    //     · 每个槽位用 atomic<double> —— 单槽读写本身原子, 不会读到半个 double
    //     · 最坏情况是读到"略微过时"的值(某个槽刚被覆盖), 而我们要的是最近
    //       7 秒的【中位数】, 对个别样本的新旧完全不敏感
    //   这比加锁好: 热路径上不做任何可能阻塞的事。
    static constexpr std::size_t kHeightRingSize = 900;   // 120fps 下约 7.5 秒
    std::array<std::atomic<double>, kHeightRingSize> height_ring_{};
    // filled 用 atomic 是为了让 UI 线程能安全地看到"攒了多少条"。
    std::atomic<std::size_t> height_ring_filled_{0};
    std::size_t height_ring_pos_ = 0;   // 只由瞄准线程写

    std::atomic<double> learned_base_{0.0};
    static constexpr std::size_t kMinBaseSamples = 30;    // 太少不给结论

    mutable std::mutex mutex_;
    std::string last_status_;
};

// 生产实现: 读/写真实 config, 写回走 live_tune 的已验证重载路径。
// 定义在 autotune_hooks.cpp(那一层依赖 Qt 与 config, 不参与纯逻辑测试)。
void install_production_hooks();
void uninstall_production_hooks();

} // namespace boss::autotune