#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>

#include "model_crypto.h"

#pragma comment(lib, "bcrypt.lib")

namespace oliver
{
namespace
{
constexpr std::array<uint8_t, 8> kMagic{ 'O', 'L', 'I', 'V', 'E', 'R', 'K', 'C' };
constexpr uint32_t kVersionLegacy = 1;
constexpr uint32_t kVersionWithModelId = 2;
constexpr uint32_t kKdfIterations = 200000;
constexpr size_t kSaltSize = 16;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kKeySize = 32;
constexpr size_t kLegacyHeaderSize = 8 + 4 + 4 + 4 + 8 + kSaltSize + kNonceSize + kTagSize;

std::mutex g_key_mutex;
std::array<uint8_t, kKeySize> g_runtime_key{};
bool g_has_runtime_key = false;

struct AlgHandle
{
    BCRYPT_ALG_HANDLE value = nullptr;
    ~AlgHandle() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
};

struct KeyHandle
{
    BCRYPT_KEY_HANDLE value = nullptr;
    ~KeyHandle() { if (value) BCryptDestroyKey(value); }
};

std::string wide_to_utf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring status_message(NTSTATUS status)
{
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, static_cast<DWORD>(status), 0,
                               reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = len ? std::wstring(buffer, len) : L"BCrypt error";
    if (buffer) LocalFree(buffer);
    return message;
}

void set_error(std::string& error, const char* context, NTSTATUS status)
{
    error = std::string(context) + ": " + wide_to_utf8(status_message(status));
}

void write_u32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void write_u64(std::vector<uint8_t>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
}

uint32_t read_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

bool random_bytes(uint8_t* data, size_t size, std::string& error)
{
    NTSTATUS status = BCryptGenRandom(nullptr, data, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
    {
        set_error(error, "BCryptGenRandom failed", status);
        return false;
    }
    return true;
}

bool get_runtime_key(std::array<uint8_t, kKeySize>& key, std::string& error)
{
    std::lock_guard<std::mutex> lock(g_key_mutex);
    if (!g_has_runtime_key)
    {
        error = u8"尚未获取模型授权，请先登录并授权模型。";
        return false;
    }
    key = g_runtime_key;
    return true;
}

bool open_aes_gcm(AlgHandle& alg, std::string& error)
{
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg.value, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status < 0)
    {
        set_error(error, "BCryptOpenAlgorithmProvider(AES) failed", status);
        return false;
    }
    status = BCryptSetProperty(alg.value,
                               BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                               static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)),
                               0);
    if (status < 0)
    {
        set_error(error, "BCryptSetProperty(GCM) failed", status);
        return false;
    }
    return true;
}

bool create_key(BCRYPT_ALG_HANDLE alg, const std::array<uint8_t, kKeySize>& key, KeyHandle& handle, std::string& error)
{
    NTSTATUS status = BCryptGenerateSymmetricKey(alg,
                                                 &handle.value,
                                                 nullptr,
                                                 0,
                                                 const_cast<PUCHAR>(key.data()),
                                                 static_cast<ULONG>(key.size()),
                                                 0);
    if (status < 0)
    {
        set_error(error, "BCryptGenerateSymmetricKey failed", status);
        return false;
    }
    return true;
}

std::filesystem::path u8path_from_string(const std::string& path)
{
    return std::filesystem::u8path(path);
}

bool parse_header(const std::vector<uint8_t>& encrypted,
                  PayloadType& type,
                  std::string& model_id,
                  uint64_t& plain_size,
                  size_t& salt_offset,
                  size_t& nonce_offset,
                  size_t& tag_offset,
                  size_t& ciphertext_offset,
                  std::string& error)
{
    if (encrypted.size() < kLegacyHeaderSize)
    {
        error = u8"oliver 文件过小或格式不正确。";
        return false;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), encrypted.begin()))
    {
        error = u8"不是有效的 oliver 加密模型。";
        return false;
    }

    size_t off = kMagic.size();
    const uint32_t version = read_u32(encrypted.data() + off); off += 4;
    type = static_cast<PayloadType>(read_u32(encrypted.data() + off)); off += 4;
    const uint32_t iterations = read_u32(encrypted.data() + off); off += 4;
    plain_size = read_u64(encrypted.data() + off); off += 8;

    if (version != kVersionLegacy && version != kVersionWithModelId)
    {
        error = u8"不支持的 oliver 文件版本。";
        return false;
    }
    if (iterations != kKdfIterations)
    {
        error = u8"不支持的 oliver KDF 参数。";
        return false;
    }
    if (type != PayloadType::Onnx && type != PayloadType::TensorRtEngine)
    {
        error = u8"oliver 文件内的模型类型不受支持。";
        return false;
    }
    if (plain_size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
    {
        error = u8"oliver 文件声明的模型大小过大。";
        return false;
    }

    model_id.clear();
    if (version == kVersionWithModelId)
    {
        if (encrypted.size() < off + 4)
        {
            error = u8"oliver 文件缺少模型编号。";
            return false;
        }
        const uint32_t model_id_size = read_u32(encrypted.data() + off);
        off += 4;
        if (model_id_size == 0 || model_id_size > 128 || encrypted.size() < off + model_id_size)
        {
            error = u8"oliver 文件模型编号不正确。";
            return false;
        }
        model_id.assign(reinterpret_cast<const char*>(encrypted.data() + off), model_id_size);
        off += model_id_size;
    }

    if (encrypted.size() < off + kSaltSize + kNonceSize + kTagSize)
    {
        error = u8"oliver 文件头不完整。";
        return false;
    }
    salt_offset = off; off += kSaltSize;
    nonce_offset = off; off += kNonceSize;
    tag_offset = off; off += kTagSize;
    ciphertext_offset = off;
    return true;
}
} // namespace

void set_runtime_key(const std::vector<uint8_t>& key)
{
    std::lock_guard<std::mutex> lock(g_key_mutex);
    std::fill(g_runtime_key.begin(), g_runtime_key.end(), 0);
    if (key.size() == kKeySize)
    {
        std::copy(key.begin(), key.end(), g_runtime_key.begin());
        g_has_runtime_key = true;
    }
    else
    {
        g_has_runtime_key = false;
    }
}

void clear_runtime_key()
{
    std::lock_guard<std::mutex> lock(g_key_mutex);
    std::fill(g_runtime_key.begin(), g_runtime_key.end(), 0);
    g_has_runtime_key = false;
}

bool has_runtime_key()
{
    std::lock_guard<std::mutex> lock(g_key_mutex);
    return g_has_runtime_key;
}

bool is_oliver_path(const std::string& path)
{
    std::filesystem::path p = u8path_from_string(path);
    std::string ext = p.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".oliver" || ext == ".olivercache";
}

const char* payload_type_name(PayloadType type)
{
    switch (type)
    {
    case PayloadType::Onnx: return "onnx";
    case PayloadType::TensorRtEngine: return "trt_engine";
    default: return "unknown";
    }
}

PayloadType payload_type_from_extension(const std::string& path)
{
    std::filesystem::path p = u8path_from_string(path);
    std::string ext = p.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".onnx") return PayloadType::Onnx;
    if (ext == ".engine" || ext == ".trt") return PayloadType::TensorRtEngine;
    return PayloadType::Unknown;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& bytes, std::string& error)
{
    std::ifstream f(u8path_from_string(path), std::ios::binary);
    if (!f)
    {
        error = u8"无法打开文件: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len < 0)
    {
        error = u8"无法读取文件大小: " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(len));
    if (!bytes.empty())
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f && !bytes.empty())
    {
        error = u8"读取文件失败: " + path;
        return false;
    }
    return true;
}

bool write_file_bytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error)
{
    std::filesystem::path out_path = u8path_from_string(path);
    std::error_code ec;
    if (!out_path.parent_path().empty())
        std::filesystem::create_directories(out_path.parent_path(), ec);
    if (ec)
    {
        error = u8"创建输出目录失败: " + ec.message();
        return false;
    }

    std::ofstream f(out_path, std::ios::binary);
    if (!f)
    {
        error = u8"无法写入文件: " + path;
        return false;
    }
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f)
    {
        error = u8"写入文件失败: " + path;
        return false;
    }
    return true;
}

bool read_model_id_from_file(const std::string& input_path, std::string& model_id, std::string& error)
{
    std::vector<uint8_t> encrypted;
    if (!read_file_bytes(input_path, encrypted, error))
        return false;
    PayloadType type = PayloadType::Unknown;
    uint64_t plain_size = 0;
    size_t salt = 0, nonce = 0, tag = 0, ciphertext = 0;
    return parse_header(encrypted, type, model_id, plain_size, salt, nonce, tag, ciphertext, error);
}

bool encrypt_bytes(const std::vector<uint8_t>& plaintext,
                   PayloadType type,
                   const std::string& model_id,
                   std::vector<uint8_t>& output,
                   std::string& error)
{
    if (type == PayloadType::Unknown)
    {
        error = u8"未知模型类型，无法加密。";
        return false;
    }
    if (model_id.empty() || model_id.size() > 128)
    {
        error = u8"模型编号不正确。";
        return false;
    }
    if (plaintext.size() > static_cast<size_t>((std::numeric_limits<ULONG>::max)()))
    {
        error = u8"模型文件过大，当前加密实现不支持。";
        return false;
    }

    std::array<uint8_t, kSaltSize> salt{};
    std::array<uint8_t, kNonceSize> nonce{};
    std::array<uint8_t, kTagSize> tag{};
    std::array<uint8_t, kKeySize> key{};
    if (!random_bytes(salt.data(), salt.size(), error) ||
        !random_bytes(nonce.data(), nonce.size(), error) ||
        !get_runtime_key(key, error))
    {
        return false;
    }

    AlgHandle alg;
    KeyHandle key_handle;
    if (!open_aes_gcm(alg, error) || !create_key(alg.value, key, key_handle, error))
        return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = nonce.data();
    auth_info.cbNonce = static_cast<ULONG>(nonce.size());
    auth_info.pbTag = tag.data();
    auth_info.cbTag = static_cast<ULONG>(tag.size());

    std::vector<uint8_t> ciphertext(plaintext.size());
    ULONG result = 0;
    NTSTATUS status = BCryptEncrypt(key_handle.value,
                                    const_cast<PUCHAR>(plaintext.data()),
                                    static_cast<ULONG>(plaintext.size()),
                                    &auth_info,
                                    nullptr,
                                    0,
                                    ciphertext.data(),
                                    static_cast<ULONG>(ciphertext.size()),
                                    &result,
                                    0);
    if (status < 0)
    {
        set_error(error, "BCryptEncrypt failed", status);
        return false;
    }

    output.clear();
    output.reserve(kLegacyHeaderSize + 4 + model_id.size() + ciphertext.size());
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    write_u32(output, kVersionWithModelId);
    write_u32(output, static_cast<uint32_t>(type));
    write_u32(output, kKdfIterations);
    write_u64(output, static_cast<uint64_t>(plaintext.size()));
    write_u32(output, static_cast<uint32_t>(model_id.size()));
    output.insert(output.end(), model_id.begin(), model_id.end());
    output.insert(output.end(), salt.begin(), salt.end());
    output.insert(output.end(), nonce.begin(), nonce.end());
    output.insert(output.end(), tag.begin(), tag.end());
    output.insert(output.end(), ciphertext.begin(), ciphertext.end());
    return true;
}

bool decrypt_bytes(const std::vector<uint8_t>& encrypted,
                   Payload& payload,
                   std::string& error)
{
    PayloadType type = PayloadType::Unknown;
    std::string model_id;
    uint64_t plain_size = 0;
    size_t salt_offset = 0, nonce_offset = 0, tag_offset = 0, ciphertext_offset = 0;
    if (!parse_header(encrypted, type, model_id, plain_size, salt_offset, nonce_offset, tag_offset, ciphertext_offset, error))
        return false;

    (void)salt_offset;
    const size_t ciphertext_size = encrypted.size() - ciphertext_offset;
    if (ciphertext_size != static_cast<size_t>(plain_size))
    {
        error = u8"oliver 文件大小校验失败。";
        return false;
    }
    if (ciphertext_size > static_cast<size_t>((std::numeric_limits<ULONG>::max)()))
    {
        error = u8"模型文件过大，当前解密实现不支持。";
        return false;
    }

    std::array<uint8_t, kKeySize> key{};
    if (!get_runtime_key(key, error))
        return false;

    AlgHandle alg;
    KeyHandle key_handle;
    if (!open_aes_gcm(alg, error) || !create_key(alg.value, key, key_handle, error))
        return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(encrypted.data() + nonce_offset);
    auth_info.cbNonce = static_cast<ULONG>(kNonceSize);
    auth_info.pbTag = const_cast<PUCHAR>(encrypted.data() + tag_offset);
    auth_info.cbTag = static_cast<ULONG>(kTagSize);

    std::vector<uint8_t> plaintext(static_cast<size_t>(plain_size));
    ULONG result = 0;
    NTSTATUS status = BCryptDecrypt(key_handle.value,
                                    const_cast<PUCHAR>(encrypted.data() + ciphertext_offset),
                                    static_cast<ULONG>(ciphertext_size),
                                    &auth_info,
                                    nullptr,
                                    0,
                                    plaintext.data(),
                                    static_cast<ULONG>(plaintext.size()),
                                    &result,
                                    0);
    if (status < 0)
    {
        error = u8"oliver 模型解密失败，文件可能被篡改或授权不匹配。";
        return false;
    }

    payload.type = type;
    payload.model_id = std::move(model_id);
    payload.bytes = std::move(plaintext);
    return true;
}

bool encrypt_file(const std::string& input_path,
                  const std::string& output_path,
                  PayloadType type,
                  const std::string& model_id,
                  std::string& error)
{
    std::vector<uint8_t> plaintext;
    std::vector<uint8_t> encrypted;
    if (!read_file_bytes(input_path, plaintext, error))
        return false;
    if (!encrypt_bytes(plaintext, type, model_id, encrypted, error))
        return false;
    return write_file_bytes(output_path, encrypted, error);
}

bool decrypt_file(const std::string& input_path,
                  Payload& payload,
                  std::string& error)
{
    std::vector<uint8_t> encrypted;
    if (!read_file_bytes(input_path, encrypted, error))
        return false;
    return decrypt_bytes(encrypted, payload, error);
}

} // namespace oliver
