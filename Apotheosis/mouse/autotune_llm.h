#pragma once

// =============================================================================
// 调参 agent —— LLM 客户端 (WinHTTP, OpenAI 兼容接口)
// =============================================================================
//
// 为什么用 WinHTTP (2026-09-14)
//   项目目前没有任何网络依赖(整个链路是 采集卡->推理->HID)。为了一个调参
//   功能引入 libcurl 不划算, 而 WinHTTP 是 Windows 自带的, 零新依赖。
//   ★ 兼容 [OpenAI /v1/chat/completions] 协议, 所以 DeepSeek、Kimi、通义、
//     本地 ollama 都能直接用, 只改 base_url 和 model 两个设置。
//
// 调用形态
//   一次性请求-响应(非流式)。调参每轮要等几秒到几十秒, 但那是【靶场模式】下
//   的等待, 不影响实战 —— agent 关掉之后这条路径完全不执行。
//
// ★ 超时与失败处理
//   网络请求绝不允许拖住瞄准主循环。本客户端只在【agent 自己的线程】里被调用,
//   且有硬超时; 任何失败都返回 ok=false + 原因, 由上层决定"这一轮跳过"。
// =============================================================================

#include <string>

namespace boss::autotune {

struct LlmConfig
{
    // 例: https://api.deepseek.com  (不带 /v1, 内部会拼)
    std::string base_url;
    std::string api_key;
    std::string model;
    int timeout_ms = 60000;
    double temperature = 0.2;    // 调参要稳定, 不要发散
    // ★★ 2026-09-14 从 800 提到 4096 —— 原因见下 ★★
    //
    //   用户实测: "测试连接成功, 但显示没 content"。
    //   根因是【推理模型 + max_tokens 太小】: 带思维链的模型(deepseek-reasoner、
    //   Qwen-QwQ、各种 -thinking 变体, 以及很多网关自带的思考模式)会先产出
    //   一大段 reasoning_content, 而**推理 token 也算在 max_tokens 里**。
    //   800 的预算很容易被思考吃光, 于是 finish_reason="length"、content=""。
    //
    //   为什么"测试连接"却是好的: 那个测试用的提示词极短("请只回复一个 JSON"),
    //   模型不用想太多, 所以有 content。真调参时的提示词长得多(指标+参数+
    //   数据来历), 思考量大一个量级 —— 于是同一个配置, 测试通过、调参失败。
    //   ★ 这就是"能连上但没内容"这类症状的典型形态: 不是连不上, 是预算不够。
    //
    //   4096 给"思考 + 正文"都留了余量。仍然可以调: 页面上有输入框。
    int max_tokens = 4096;
};

struct LlmResult
{
    bool ok = false;
    std::string text;       // 模型的回答正文
    std::string error;      // ok=false 时的原因(中文, 便于直接显示在 UI 上)
    int http_status = 0;
    double elapsed_ms = 0.0;

    // ── 诊断字段 (2026-09-14 新增) ─────────────────────────────────────────
    // ★ 为什么要把这些带出来: 之前 content 为空时只报一句"响应里没有 content
    //   字段(模型可能只返回了 reasoning)", 用户拿这句话查不出所以然 ——
    //   到底是被截断了? 还是模型不支持? 还是字段名不一样?
    //   现在把"上游到底说了什么"如实带回来, 让错误信息本身就能定位问题。
    std::string finish_reason;        // "stop" / "length" / "content_filter" ...
    std::size_t reasoning_len = 0;    // reasoning_content 的长度(0 = 没有)
    int completion_tokens = -1;       // 实际用了多少 token(-1 = 上游没给)
    std::string raw_snippet;          // 响应体开头一段(排查用)
};

// 发一次 chat/completions 请求。
// ★ 这是唯一的网络出口。内容里没有任何真实日志以外的隐私数据 ——
//   发出去的是【指标摘要 + 当前参数】, 不含画面。
LlmResult chat_completion(const LlmConfig& cfg,
                          const std::string& system_prompt,
                          const std::string& user_prompt);

// 把 /chat/completions 的响应体解释成 LlmResult (不联网)。
//
// ★★ 为什么把它从 chat_completion 里拆出来 ★★
//   真实网络不可重复, 所以"联网测试"没法覆盖解析逻辑; 但"上游返回了这样的
//   JSON, 我们该怎么理解它"是完全可以判定的 —— 而出问题的恰恰是这一层:
//   用户报"测试连接成功但显示没 content", 根因是【推理模型把 max_tokens
//   全花在思维链上】, 而当时的报错只说了一句"没有 content 字段", 定位不了。
//   拆出来之后 tests/autotune_test.cpp §[14] 可以拿真实的响应体断言诊断结论。
//
// http_status 用于区分"HTTP 层就失败了"和"HTTP 通了但内容不对"。
// max_tokens 只用于把"预算用光"这个原因写进错误信息。
LlmResult parse_completion_body(const std::string& resp,
                                int http_status,
                                int max_tokens);

// 从模型回答里抠出第一个 JSON 对象。模型经常会加 ```json 围栏或前后解释,
// 所以这里做一次宽容提取 —— 抠不到就返回空串。
std::string extract_json_object(const std::string& text);

} // namespace boss::autotune
