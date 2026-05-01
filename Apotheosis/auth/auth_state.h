#ifndef AUTH_STATE_H
#define AUTH_STATE_H

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "auth_client.h"

namespace auth
{

class AuthState
{
public:
    void initialize(const std::string& server_url);

    bool register_user(const std::string& user_name,
                       const std::string& password,
                       const std::string& invite_code);
    bool login(const std::string& user_name, const std::string& password);
    bool try_restore_session();
    bool ensure_model_key(const std::string& model_id);
    bool create_model_key(const std::string& model_name, std::string& model_id, std::vector<uint8_t>& key);
    bool grant_model(const std::string& model_id, const std::string& target_user);
    bool heartbeat_if_due();
    void logout();

    bool is_authorized() const;
    std::string user_name() const;
    std::string status_text() const;
    std::string last_error() const;
    std::string server_url() const;

private:
    bool apply_tokens_locked(const AuthTokens& tokens);
    bool refresh_locked();
    bool store_refresh_token(const std::string& token);
    bool load_refresh_token(std::string& token) const;
    void clear_refresh_token() const;
    void set_error_locked(const std::string& error);

    mutable std::mutex mutex_;
    std::string server_url_;
    std::string access_token_;
    std::string refresh_token_;
    std::string user_name_;
    std::string active_model_id_;
    std::string last_error_;
    std::string status_text_ = u8"未登录";
    std::chrono::steady_clock::time_point access_expires_at_{};
    std::chrono::steady_clock::time_point next_heartbeat_at_{};
    std::chrono::steady_clock::time_point model_key_expires_at_{};
};

AuthState& state();

} // namespace auth

#endif // AUTH_STATE_H
