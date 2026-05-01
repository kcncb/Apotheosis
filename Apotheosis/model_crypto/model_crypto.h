#ifndef MODEL_CRYPTO_H
#define MODEL_CRYPTO_H

#include <cstdint>
#include <string>
#include <vector>

namespace oliver
{

enum class PayloadType : uint32_t
{
    Unknown = 0,
    Onnx = 1,
    TensorRtEngine = 2
};

struct Payload
{
    PayloadType type = PayloadType::Unknown;
    std::string model_id;
    std::vector<uint8_t> bytes;
};

void set_runtime_key(const std::vector<uint8_t>& key);
void clear_runtime_key();
bool has_runtime_key();

bool is_oliver_path(const std::string& path);
const char* payload_type_name(PayloadType type);
PayloadType payload_type_from_extension(const std::string& path);

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& bytes, std::string& error);
bool write_file_bytes(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error);

bool read_model_id_from_file(const std::string& input_path, std::string& model_id, std::string& error);

bool encrypt_bytes(const std::vector<uint8_t>& plaintext,
                   PayloadType type,
                   const std::string& model_id,
                   std::vector<uint8_t>& output,
                   std::string& error);

bool decrypt_bytes(const std::vector<uint8_t>& encrypted,
                   Payload& payload,
                   std::string& error);

bool encrypt_file(const std::string& input_path,
                  const std::string& output_path,
                  PayloadType type,
                  const std::string& model_id,
                  std::string& error);

bool decrypt_file(const std::string& input_path,
                  Payload& payload,
                  std::string& error);

} // namespace oliver

#endif // MODEL_CRYPTO_H
