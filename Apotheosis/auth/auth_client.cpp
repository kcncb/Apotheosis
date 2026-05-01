#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

#include "auth_client.h"

namespace auth
{
namespace
{
struct UrlParts
{
    std::wstring host;
    INTERNET_PORT port = 0;
    bool https = false;
    std::wstring base_path;
};

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool parse_url(const std::string& url, UrlParts& parts, std::string& error)
{
    std::wstring wide = utf8_to_wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &uc))
    {
        error = u8"授权服务器地址格式不正确";
        return false;
    }

    parts.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
    {
        error = u8"授权服务器仅支持 http 或 https";
        return false;
    }

    parts.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    parts.port = uc.nPort;
    parts.base_path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (parts.base_path == L"/") parts.base_path.clear();
    return true;
}

std::string win32_error(const char* prefix)
{
    DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string msg = prefix;
    msg += ": ";
    msg += buffer ? wide_to_utf8(buffer) : std::to_string(code);
    if (buffer) LocalFree(buffer);
    return msg;
}

HttpResult post_json(const std::string& server_url,
                     const std::string& path,
                     const std::string& body,
                     const std::string& bearer = {})
{
    HttpResult result;
    UrlParts url;
    if (!parse_url(server_url, url, result.error))
        return result;

    HINTERNET session = WinHttpOpen(L"ApotheosisAuth/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session)
    {
        result.error = win32_error("WinHttpOpen failed");
        return result;
    }

    HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (!connect)
    {
        result.error = win32_error("WinHttpConnect failed");
        WinHttpCloseHandle(session);
        return result;
    }

    std::wstring full_path = url.base_path + utf8_to_wide(path);
    DWORD flags = url.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", full_path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request)
    {
        result.error = win32_error("WinHttpOpenRequest failed");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!bearer.empty())
        headers += L"Authorization: Bearer " + utf8_to_wide(bearer) + L"\r\n";

    BOOL sent = WinHttpSendRequest(request,
                                   headers.c_str(),
                                   static_cast<DWORD>(headers.size()),
                                   const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()),
                                   0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        result.error = win32_error("授权服务器请求失败");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD status_size = sizeof(result.status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &result.status,
                        &status_size,
                        WINHTTP_NO_HEADER_INDEX);

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
            break;
        chunk.resize(read);
        result.body += chunk;
    }

    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok && result.error.empty())
    {
        std::string message;
        if (json_get_string(result.body, "detail", message) || json_get_string(result.body, "error", message))
            result.error = message;
        else
            result.error = u8"授权服务器返回错误: HTTP " + std::to_string(result.status);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

std::string sha256_hex(const std::string& input)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::array<BYTE, 32> digest{};
    DWORD digest_size = static_cast<DWORD>(digest.size());

    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return {};
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
    {
        CryptReleaseContext(provider, 0);
        return {};
    }
    CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()), static_cast<DWORD>(input.size()), 0);
    CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);

    std::ostringstream oss;
    for (BYTE b : digest)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
}

std::string read_registry_string(HKEY root, const wchar_t* subkey, const wchar_t* name)
{
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, &type, buffer, &size) != ERROR_SUCCESS)
        return {};
    return wide_to_utf8(buffer);
}

std::string auth_body_credentials(const std::string& user_name,
                                  const std::string& password,
                                  const std::string& invite_code = {})
{
    std::string body = "{\"user_name\":\"" + json_escape(user_name) +
        "\",\"password\":\"" + json_escape(password) +
        "\",\"device_hash\":\"" + json_escape(machine_fingerprint()) + "\"";
    if (!invite_code.empty())
        body += ",\"invite_code\":\"" + json_escape(invite_code) + "\"";
    body += "}";
    return body;
}
} // namespace

AuthClient::AuthClient(std::string server_url)
    : server_url_(std::move(server_url))
{
}

HttpResult AuthClient::register_user(const std::string& user_name,
                                     const std::string& password,
                                     const std::string& invite_code)
{
    return post_json(server_url_, "/api/auth/register",
                     auth_body_credentials(user_name, password, invite_code));
}

HttpResult AuthClient::login(const std::string& user_name,
                             const std::string& password,
                             AuthTokens& tokens)
{
    HttpResult result = post_json(server_url_, "/api/auth/login",
                                  auth_body_credentials(user_name, password));
    if (!result.ok) return result;
    json_get_string(result.body, "access_token", tokens.access_token);
    json_get_string(result.body, "refresh_token", tokens.refresh_token);
    json_get_string(result.body, "user_name", tokens.user_name);
    json_get_int(result.body, "expires_in", tokens.expires_in_seconds);
    if (tokens.access_token.empty() || tokens.refresh_token.empty())
    {
        result.ok = false;
        result.error = u8"授权响应缺少令牌";
    }
    return result;
}

HttpResult AuthClient::refresh(const std::string& refresh_token, AuthTokens& tokens)
{
    std::string body = "{\"refresh_token\":\"" + json_escape(refresh_token) +
        "\",\"device_hash\":\"" + json_escape(machine_fingerprint()) + "\"}";
    HttpResult result = post_json(server_url_, "/api/auth/refresh", body);
    if (!result.ok) return result;
    json_get_string(result.body, "access_token", tokens.access_token);
    json_get_string(result.body, "refresh_token", tokens.refresh_token);
    json_get_string(result.body, "user_name", tokens.user_name);
    json_get_int(result.body, "expires_in", tokens.expires_in_seconds);
    if (tokens.access_token.empty() || tokens.refresh_token.empty())
    {
        result.ok = false;
        result.error = u8"刷新登录状态失败";
    }
    return result;
}

HttpResult AuthClient::heartbeat(const std::string& access_token)
{
    std::string body = "{\"device_hash\":\"" + json_escape(machine_fingerprint()) + "\"}";
    return post_json(server_url_, "/api/auth/heartbeat", body, access_token);
}

HttpResult AuthClient::create_model(const std::string& access_token,
                                    const std::string& model_name,
                                    CreateModelResponse& model_response)
{
    std::string body = "{\"model_name\":\"" + json_escape(model_name) + "\"}";
    HttpResult result = post_json(server_url_, "/api/model/create", body, access_token);
    if (!result.ok) return result;

    std::string key_b64;
    json_get_string(result.body, "model_id", model_response.model_id);
    json_get_string(result.body, "model_key", key_b64);
    if (model_response.model_id.empty() || !base64_decode(key_b64, model_response.key) || model_response.key.size() != 32)
    {
        result.ok = false;
        result.error = u8"创建模型授权失败";
    }
    return result;
}

HttpResult AuthClient::grant_model(const std::string& access_token,
                                   const std::string& model_id,
                                   const std::string& target_user)
{
    std::string body = "{\"model_id\":\"" + json_escape(model_id) +
        "\",\"target_user\":\"" + json_escape(target_user) + "\"}";
    return post_json(server_url_, "/api/model/grant", body, access_token);
}

HttpResult AuthClient::fetch_model_key(const std::string& access_token,
                                       const std::string& model_id,
                                       ModelKeyResponse& key_response)
{
    std::string body = "{\"model_id\":\"" + json_escape(model_id) +
        "\",\"device_hash\":\"" + json_escape(machine_fingerprint()) + "\"}";
    HttpResult result = post_json(server_url_, "/api/model/key", body, access_token);
    if (!result.ok) return result;

    std::string key_b64;
    json_get_string(result.body, "model_id", key_response.model_id);
    json_get_string(result.body, "model_key", key_b64);
    json_get_int(result.body, "expires_in", key_response.expires_in_seconds);
    if (!base64_decode(key_b64, key_response.key) || key_response.key.size() != 32)
    {
        result.ok = false;
        result.error = u8"模型授权响应不正确";
    }
    return result;
}

std::string machine_fingerprint()
{
    std::string seed;
    seed += read_registry_string(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");

    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD computer_len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computer, &computer_len))
        seed += "|" + wide_to_utf8(computer);

    wchar_t windows_dir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows_dir, MAX_PATH))
        seed += "|" + wide_to_utf8(windows_dir);

    return sha256_hex(seed);
}

std::string json_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text)
    {
        switch (ch)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                char buf[7]{};
                sprintf_s(buf, "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

bool json_get_string(const std::string& json, const std::string& key, std::string& value)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    ++pos;

    std::string out;
    for (; pos < json.size(); ++pos)
    {
        char ch = json[pos];
        if (ch == '"')
        {
            value = out;
            return true;
        }
        if (ch == '\\' && pos + 1 < json.size())
        {
            char esc = json[++pos];
            switch (esc)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(esc); break;
            }
        }
        else
        {
            out.push_back(ch);
        }
    }
    return false;
}

bool json_get_int(const std::string& json, const std::string& key, int& value)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    size_t end = pos;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) ++end;
    if (end == pos) return false;
    value = std::stoi(json.substr(pos, end - pos));
    return true;
}

std::string base64_encode(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) return {};
    DWORD out_len = 0;
    CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &out_len);
    std::string out(out_len, '\0');
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &out_len))
        return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& bytes)
{
    DWORD out_len = 0;
    if (!CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &out_len, nullptr, nullptr))
        return false;
    bytes.resize(out_len);
    return CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, bytes.data(), &out_len, nullptr, nullptr) != FALSE;
}

} // namespace auth
