#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dpapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

#include "auth_state.h"
#include "model_crypto/model_crypto.h"

namespace auth
{
namespace
{
constexpr const char* kTokenFile = "auth_token.bin";

std::unique_ptr<AuthClient> make_client(const std::string& server_url)
{
    return std::make_unique<AuthClient>(server_url);
}

std::filesystem::path token_path()
{
    return std::filesystem::u8path(kTokenFile);
}
} // namespace

AuthState& state()
{
    static AuthState s;
    return s;
}

void AuthState::initialize(const std::string& server_url)
{
    std::lock_guard<std::mutex> lock(mutex_);
    server_url_ = server_url;
    status_text_ = u8"未登录";
    last_error_.clear();
}

bool AuthState::register_user(const std::string& user_name,
                              const std::string& password,
                              const std::string& invite_code)
{
    auto client = make_client(server_url_);
    HttpResult result = client->register_user(user_name, password, invite_code);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!result.ok)
    {
        set_error_locked(result.error.empty() ? u8"注册失败" : result.error);
        return false;
    }
    status_text_ = u8"注册成功，请登录";
    last_error_.clear();
    return true;
}

bool AuthState::login(const std::string& user_name, const std::string& password)
{
    auto client = make_client(server_url_);
    AuthTokens tokens;
    HttpResult result = client->login(user_name, password, tokens);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!result.ok)
    {
        set_error_locked(result.error.empty() ? u8"登录失败" : result.error);
        return false;
    }
    return apply_tokens_locked(tokens);
}

bool AuthState::try_restore_session()
{
    std::string token;
    if (!load_refresh_token(token) || token.empty())
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    refresh_token_ = token;
    return refresh_locked();
}

bool AuthState::ensure_model_key(const std::string& model_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (active_model_id_ == model_id &&
        model_key_expires_at_ > now + std::chrono::seconds(30) &&
        oliver::has_runtime_key())
    {
        return true;
    }

    if (access_token_.empty() || access_expires_at_ <= now + std::chrono::seconds(20))
    {
        if (!refresh_locked())
            return false;
    }

    auto client = make_client(server_url_);
    ModelKeyResponse key_response;
    HttpResult result = client->fetch_model_key(access_token_, model_id, key_response);
    if (!result.ok)
    {
        set_error_locked(result.error.empty() ? u8"获取模型授权失败" : result.error);
        return false;
    }

    oliver::set_runtime_key(key_response.key);
    active_model_id_ = model_id;
    const int ttl = std::max(60, key_response.expires_in_seconds);
    model_key_expires_at_ = now + std::chrono::seconds(ttl);
    status_text_ = user_name_.empty() ? u8"已授权" : (u8"已登录：" + user_name_);
    last_error_.clear();
    return true;
}

bool AuthState::create_model_key(const std::string& model_name, std::string& model_id, std::vector<uint8_t>& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (access_token_.empty() || access_expires_at_ <= std::chrono::steady_clock::now() + std::chrono::seconds(20))
    {
        if (!refresh_locked())
            return false;
    }

    auto client = make_client(server_url_);
    CreateModelResponse response;
    HttpResult result = client->create_model(access_token_, model_name, response);
    if (!result.ok)
    {
        set_error_locked(result.error.empty() ? u8"创建模型失败" : result.error);
        return false;
    }

    model_id = response.model_id;
    key = std::move(response.key);
    last_error_.clear();
    return true;
}

bool AuthState::grant_model(const std::string& model_id, const std::string& target_user)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (access_token_.empty() || access_expires_at_ <= std::chrono::steady_clock::now() + std::chrono::seconds(20))
    {
        if (!refresh_locked())
            return false;
    }

    auto client = make_client(server_url_);
    HttpResult result = client->grant_model(access_token_, model_id, target_user);
    if (!result.ok)
    {
        set_error_locked(result.error.empty() ? u8"模型授权失败" : result.error);
        return false;
    }
    last_error_.clear();
    return true;
}

bool AuthState::heartbeat_if_due()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (access_token_.empty())
        return false;
    if (next_heartbeat_at_ > now)
        return true;
    if (access_expires_at_ <= now + std::chrono::seconds(20) && !refresh_locked())
        return false;

    auto client = make_client(server_url_);
    HttpResult result = client->heartbeat(access_token_);
    if (!result.ok)
    {
        set_error_locked(result.error.empty() ? u8"授权心跳失败" : result.error);
        return false;
    }
    next_heartbeat_at_ = now + std::chrono::seconds(90);
    return true;
}

void AuthState::logout()
{
    std::lock_guard<std::mutex> lock(mutex_);
    access_token_.clear();
    refresh_token_.clear();
    user_name_.clear();
    active_model_id_.clear();
    oliver::clear_runtime_key();
    clear_refresh_token();
    status_text_ = u8"已退出登录";
}

bool AuthState::is_authorized() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !access_token_.empty() && std::chrono::steady_clock::now() < access_expires_at_;
}

std::string AuthState::user_name() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return user_name_;
}

std::string AuthState::status_text() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_text_;
}

std::string AuthState::last_error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string AuthState::server_url() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return server_url_;
}

bool AuthState::apply_tokens_locked(const AuthTokens& tokens)
{
    access_token_ = tokens.access_token;
    refresh_token_ = tokens.refresh_token;
    user_name_ = tokens.user_name;
    int ttl = std::max(60, tokens.expires_in_seconds);
    access_expires_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(ttl);
    next_heartbeat_at_ = std::chrono::steady_clock::now();
    store_refresh_token(refresh_token_);
    status_text_ = user_name_.empty() ? u8"已登录" : (u8"已登录：" + user_name_);
    last_error_.clear();
    return true;
}

bool AuthState::refresh_locked()
{
    if (refresh_token_.empty())
    {
        set_error_locked(u8"请先登录");
        return false;
    }

    auto client = make_client(server_url_);
    AuthTokens tokens;
    HttpResult result = client->refresh(refresh_token_, tokens);
    if (!result.ok)
    {
        access_token_.clear();
        set_error_locked(result.error.empty() ? u8"登录已过期，请重新登录" : result.error);
        return false;
    }
    return apply_tokens_locked(tokens);
}

bool AuthState::store_refresh_token(const std::string& token)
{
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(token.data()));
    input.cbData = static_cast<DWORD>(token.size());

    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Apotheosis refresh token", nullptr, nullptr, nullptr, 0, &output))
        return false;

    std::ofstream f(token_path(), std::ios::binary);
    if (f && output.pbData && output.cbData > 0)
        f.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return static_cast<bool>(f);
}

bool AuthState::load_refresh_token(std::string& token) const
{
    std::ifstream f(token_path(), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;

    DATA_BLOB input{};
    input.pbData = bytes.data();
    input.cbData = static_cast<DWORD>(bytes.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
        return false;
    token.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}

void AuthState::clear_refresh_token() const
{
    std::error_code ec;
    std::filesystem::remove(token_path(), ec);
}

void AuthState::set_error_locked(const std::string& error)
{
    last_error_ = error;
    status_text_ = error;
}

} // namespace auth
