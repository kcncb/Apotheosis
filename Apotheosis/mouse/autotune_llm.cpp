// =============================================================================
// 调参 agent —— LLM 客户端实现 (WinHTTP + 手写 JSON 拼装/提取)
// =============================================================================
//
// 依赖: 只用 Windows 自带的 WinHTTP + 标准库。不引入任何第三方库。
// 链接: 需要 winhttp.lib (见 CMakeLists.txt 的 target_link_libraries)。
// =============================================================================

#include "mouse/autotune_llm.h"

#include <cctype>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace boss::autotune
{

namespace
{

// ── 极简 JSON 字符串转义 ────────────────────────────────────────────────────
// 只处理必须转义的字符。中文保持原样(UTF-8 直接发)。
std::string json_escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 16);
    for (const char ch : in)
    {
        switch (ch)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out += ch;
            }
        }
    }
    return out;
}

#if defined(_WIN32)

std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                         static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), need);
    return w;
}

std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                         static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), need, nullptr, nullptr);
    return s;
}

// 把一个 URL 拆成 (是否https, host, path)。解析失败返回 false。
bool split_url(const std::string& url, bool& https, std::wstring& host,
               std::wstring& path, INTERNET_PORT& port)
{
    const std::wstring w = to_wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host_buf[256]{};
    wchar_t path_buf[1024]{};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host_buf));
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path_buf));

    if (!WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc))
        return false;

    https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    host = uc.lpszHostName;
    path = uc.lpszUrlPath;
    if (uc.nPort != 0)
        port = uc.nPort;
    else
        port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    return true;
}

#endif // _WIN32

// 粗取 HTTP 状态码之外, 找 "content":"..." 里的正文。
// ★ 手写而不是引 JSON 库: 只需要抽一个字段。但必须正确处理【转义】,
//   否则模型回答里的 \" 会把解析带偏。
std::string extract_content_field(const std::string& body)
{
    // 找 "content" 后面第一个冒号, 再跳过空白到引号
    const std::string key = "\"content\"";
    std::size_t pos = 0;
    while ((pos = body.find(key, pos)) != std::string::npos)
    {
        std::size_t i = pos + key.size();
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
            ++i;
        if (i >= body.size() || body[i] != ':') { pos = i; continue; }
        ++i;
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
            ++i;
        if (i >= body.size()) return {};
        // 兼容 "content": null (推理模型有时先给空 content)
        if (body.compare(i, 4, "null") == 0) { pos = i + 4; continue; }
        if (body[i] != '"') { pos = i; continue; }

        ++i;
        std::string out;
        while (i < body.size())
        {
            const char ch = body[i];
            if (ch == '\\' && i + 1 < body.size())
            {
                const char nx = body[i + 1];
                switch (nx)
                {
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'u':
                {
                    // \uXXXX -> 转成 UTF-8
                    if (i + 6 <= body.size())
                    {
                        auto hex = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                            return 0;
                        };
                        const int cp = (hex(body[i+2]) << 12) | (hex(body[i+3]) << 8)
                                     | (hex(body[i+4]) << 4)  |  hex(body[i+5]);
                        if (cp < 0x80)
                            out += static_cast<char>(cp);
                        else if (cp < 0x800)
                        {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        else
                        {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                        continue;
                    }
                    break;
                }
                default: out += nx; break;
                }
                i += 2;
                continue;
            }
            if (ch == '"') break;    // 字符串结束
            out += ch;
            ++i;
        }
        return out;
    }
    return {};
}

// 从响应体里抠 "error":{"message":"..."} 的内容, 用于把失败原因显示给用户。
// (parse_completion_body 在同一个编译单元里, 直接调用。)
std::string extract_error_message(const std::string& body)
{
    const std::string key = "\"message\"";
    const std::size_t p = body.find(key);
    if (p == std::string::npos) return {};
    std::size_t i = p + key.size();
    while (i < body.size() && body[i] != '"') ++i;
    if (i >= body.size()) return {};
    ++i;
    std::string out;
    while (i < body.size() && body[i] != '"')
    {
        if (body[i] == '\\' && i + 1 < body.size()) ++i;
        out += body[i++];
    }
    return out;
}

// ── 诊断用的字段提取 (2026-09-14) ───────────────────────────────────────────
//
// ★ 为什么需要这些: 用户报"测试连接成功, 但显示没 content"。
//   只报一句"没有 content 字段"用户没法判断原因(被截断? 模型不支持?
//   字段名不同?), 所以要把【上游到底说了什么】如实带回来:
//   · finish_reason = "length"  -> 预算被吃光了(推理模型 + max_tokens 太小)
//   · reasoning_len > 0         -> 模型是带思维链的, 正文可能被挤掉
//   · completion_tokens         -> 实际用量, 跟 max_tokens 一比就知道
//   · raw_snippet               -> 前面一段原文, 兜底排查
//
// ★ 这些辅助函数留在本文件的匿名 namespace 里; parse_completion_body 定义在
//   同一编译单元的后面, 所以可以直接调用它们(不需要暴露到头文件)。

// 抠任意 "key":"value" 里的字符串值。转义只处理最常见的几种 ——
// 这些诊断字段(finish_reason 等)本身不会含复杂转义。
std::string extract_string_field(const std::string& body, const char* field)
{
    const std::string key = std::string("\"") + field + "\"";
    const std::size_t p = body.find(key);
    if (p == std::string::npos) return {};
    std::size_t i = p + key.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != ':') return {};
    ++i;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size()) return {};
    if (body.compare(i, 4, "null") == 0) return {};      // "finish_reason": null
    if (body[i] != '"') return {};
    ++i;
    std::string out;
    while (i < body.size() && body[i] != '"')
    {
        if (body[i] == '\\' && i + 1 < body.size())
        {
            switch (body[i + 1])
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            default:  out += body[i + 1]; break;
            }
            i += 2;
            continue;
        }
        out += body[i++];
    }
    return out;
}

// 抠 "key":123 里的整数值(找不到返回 -1)。
int extract_int_field(const std::string& body, const char* field)
{
    const std::string key = std::string("\"") + field + "\"";
    const std::size_t p = body.find(key);
    if (p == std::string::npos) return -1;
    std::size_t i = p + key.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != ':') return -1;
    ++i;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || !std::isdigit(static_cast<unsigned char>(body[i]))) return -1;
    return std::atoi(body.c_str() + i);
}

} // namespace

LlmResult chat_completion(const LlmConfig& cfg,
                          const std::string& system_prompt,
                          const std::string& user_prompt)
{
    LlmResult r;

    if (cfg.base_url.empty() || cfg.model.empty())
    {
        r.error = u8"未配置 base_url 或 model";
        return r;
    }

#if !defined(_WIN32)
    r.error = u8"当前平台未实现 LLM 客户端(仅 Windows/WinHTTP)";
    return r;
#else
    // 拼 URL: 用户可能给 https://api.deepseek.com 或带 /v1
    std::string url = cfg.base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.find("/chat/completions") == std::string::npos)
    {
        if (url.size() >= 3 && url.compare(url.size() - 3, 3, "/v1") != 0)
            url += "/v1";
        url += "/chat/completions";
    }

    bool https = true;
    std::wstring host, path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!split_url(url, https, host, path, port))
    {
        r.error = u8"无法解析 base_url: " + cfg.base_url;
        return r;
    }

    // 请求体
    std::string body = "{\"model\":\"" + json_escape(cfg.model) + "\",";
    char tb[64];
    std::snprintf(tb, sizeof(tb), "\"temperature\":%.2f,", cfg.temperature);
    body += tb;
    std::snprintf(tb, sizeof(tb), "\"max_tokens\":%d,", cfg.max_tokens);
    body += tb;
    body += "\"messages\":[{\"role\":\"system\",\"content\":\""
          + json_escape(system_prompt) + "\"},{\"role\":\"user\",\"content\":\""
          + json_escape(user_prompt) + "\"}]}";

    const auto t0 = GetTickCount64();

    HINTERNET session = WinHttpOpen(L"Apotheosis-AutoTune/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        r.error = u8"WinHttpOpen 失败";
        return r;
    }
    // 超时必须设, 否则网络卡住会把 agent 线程一直挂住
    WinHttpSetTimeouts(session, cfg.timeout_ms, cfg.timeout_ms,
                       cfg.timeout_ms, cfg.timeout_ms);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (connect == nullptr)
    {
        WinHttpCloseHandle(session);
        r.error = u8"无法连接主机(检查 base_url 与网络)";
        return r;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        r.error = u8"WinHttpOpenRequest 失败";
        return r;
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!cfg.api_key.empty())
        headers += L"Authorization: Bearer " + to_wide(cfg.api_key) + L"\r\n";

    const BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(-1L),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        const DWORD err = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        // ★ 把 WinHTTP 错误码翻译成人话。只显示 "12007" 用户查不出所以然,
        //   这几个是最常见的: 域名解析不了 / 连不上 / 超时 / TLS 失败。
        const char* why = u8"请求失败";
        switch (err)
        {
        case 12007: why = u8"域名解析失败(地址拼错了?)"; break;
        case 12029: why = u8"无法连接(服务没起? 端口错了? 防火墙?)"; break;
        case 12002: why = u8"请求超时"; break;
        case 12017: why = u8"连接被中断"; break;
        case 12175: why = u8"TLS/证书错误(本地 http 请勿用 https)"; break;
        case 12006: why = u8"连接被拒绝"; break;
        default: break;
        }
        char buf[192];
        std::snprintf(buf, sizeof(buf), u8"%s (WinHTTP %lu)", why,
                      static_cast<unsigned long>(err));
        r.error = buf;
        return r;
    }

    // 状态码
    {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        r.http_status = static_cast<int>(status);
    }

    // 读正文
    std::string resp;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
        chunk.resize(read);
        resp += chunk;
        if (resp.size() > 4u * 1024u * 1024u) break;   // 防御性上限
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    r.elapsed_ms = static_cast<double>(GetTickCount64() - t0);

    // ★ 响应解释全部交给 parse_completion_body —— 那部分不碰网络,
    //   所以可以在逻辑测试里拿真实响应体直接断言(见头文件的说明)。
    LlmResult parsed = parse_completion_body(resp, r.http_status, cfg.max_tokens);
    parsed.elapsed_ms = r.elapsed_ms;
    return parsed;
#endif
}

// ── 响应体解释 (不联网, 可离线测试) ─────────────────────────────────────────
LlmResult parse_completion_body(const std::string& resp,
                                int http_status,
                                int max_tokens)
{
    LlmResult r;
    r.http_status = http_status;

    if (http_status < 200 || http_status >= 300)
    {
        char buf[256];
        const std::string msg = extract_error_message(resp);
        std::snprintf(buf, sizeof(buf), u8"HTTP %d", http_status);
        r.error = buf;
        if (!msg.empty()) r.error += u8": " + msg;
        return r;
    }

    r.text = extract_content_field(resp);

    // ── 诊断字段: 无论成功与否都带回去 ──────────────────────────────────────
    r.finish_reason     = extract_string_field(resp, "finish_reason");
    r.completion_tokens = extract_int_field(resp, "completion_tokens");
    {
        const std::string rc = extract_string_field(resp, "reasoning_content");
        r.reasoning_len = rc.size();
    }
    r.raw_snippet = resp.substr(0, 600);

    if (r.text.empty())
    {
        // ★★ content 为空时, 必须说清【为什么】。 ★★
        //
        //   之前这里只有一句"响应里没有 content 字段(模型可能只返回了
        //   reasoning)", 用户拿这句话查不出所以然。现在按最可能的原因
        //   分情况说, 并且把上游的原文证据一起给出来。
        //
        //   最常见的成因(实测 + 上游 issue 都证实): 推理模型把 max_tokens
        //   全花在思维链上了, finish_reason = "length", content = ""。
        //   典型症状就是"测试连接是好的, 真调参却没内容" —— 因为测试的
        //   提示词极短、不用思考, 真调参的提示词长得多。
        if (r.finish_reason == "length")
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                u8"模型的思考过程把输出预算用光了, 正文被截断。\n\n"
                u8"当前 max_tokens = %d, 实际用掉 %d 个 token, 思考内容 %zu 字符。\n"
                u8"带思维链的模型(deepseek-reasoner / QwQ / 各种 thinking 变体)\n"
                u8"会把思考也算进这个预算, 不够时就会只剩思考、没有正文。\n\n"
                u8"解决办法: 把页面上的「最大输出」调大(建议 4096 或更大), "
                u8"或换一个非推理模型。",
                max_tokens, r.completion_tokens, r.reasoning_len);
            r.error = buf;
        }
        else if (r.reasoning_len > 0)
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                u8"模型只返回了思考内容, 没有正文。\n"
                u8"(思考 %zu 字符, finish_reason=%s, 用掉 %d token / 上限 %d)\n\n"
                u8"可能是 max_tokens 不够(把「最大输出」调大试试),\n"
                u8"也可能这个模型不支持输出正文。",
                r.reasoning_len,
                r.finish_reason.empty() ? "?" : r.finish_reason.c_str(),
                r.completion_tokens, max_tokens);
            r.error = buf;
        }
        else
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                u8"响应里没有 content 字段。\n"
                u8"(HTTP %d, finish_reason=%s, 用掉 %d token)\n\n"
                u8"接口通了但返回体不是预期的 OpenAI 格式 —— 检查一下模型名是否正确。\n"
                u8"响应开头: %.300s",
                http_status,
                r.finish_reason.empty() ? "(无)" : r.finish_reason.c_str(),
                r.completion_tokens, r.raw_snippet.c_str());
            r.error = buf;
        }
        return r;
    }
    r.ok = true;
    return r;
}

std::string extract_json_object(const std::string& text)
{
    // 跳过 ```json 围栏
    std::size_t begin = text.find('{');
    if (begin == std::string::npos) return {};
    // 花括号配对(不处理字符串内的花括号 —— 够用了, 因为我们的 schema 里
    // 没有嵌套字符串含括号的情况)
    int depth = 0;
    for (std::size_t i = begin; i < text.size(); ++i)
    {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}')
        {
            --depth;
            if (depth == 0) return text.substr(begin, i - begin + 1);
        }
    }
    return {};
}

} // namespace boss::autotune
