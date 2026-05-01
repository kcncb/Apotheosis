#ifndef AUTH_CLIENT_H
#define AUTH_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace auth
{

struct HttpResult
{
    bool ok = false;
    unsigned long status = 0;
    std::string body;
    std::string error;
};

struct AuthTokens
{
    std::string access_token;
    std::string refresh_token;
    std::string user_name;
    int expires_in_seconds = 0;
};

struct ModelKeyResponse
{
    std::string model_id;
    std::vector<uint8_t> key;
    int expires_in_seconds = 0;
};

struct CreateModelResponse
{
    std::string model_id;
    std::vector<uint8_t> key;
};

class AuthClient
{
public:
    explicit AuthClient(std::string server_url);

    HttpResult register_user(const std::string& user_name,
                             const std::string& password,
                             const std::string& invite_code);
    HttpResult login(const std::string& user_name,
                     const std::string& password,
                     AuthTokens& tokens);
    HttpResult refresh(const std::string& refresh_token, AuthTokens& tokens);
    HttpResult heartbeat(const std::string& access_token);
    HttpResult create_model(const std::string& access_token,
                            const std::string& model_name,
                            CreateModelResponse& model_response);
    HttpResult grant_model(const std::string& access_token,
                           const std::string& model_id,
                           const std::string& target_user);
    HttpResult fetch_model_key(const std::string& access_token,
                               const std::string& model_id,
                               ModelKeyResponse& key_response);

private:
    std::string server_url_;
};

std::string machine_fingerprint();
bool json_get_string(const std::string& json, const std::string& key, std::string& value);
bool json_get_int(const std::string& json, const std::string& key, int& value);
std::string base64_encode(const std::vector<uint8_t>& bytes);
bool base64_decode(const std::string& text, std::vector<uint8_t>& bytes);
std::string json_escape(const std::string& text);

} // namespace auth

#endif // AUTH_CLIENT_H
