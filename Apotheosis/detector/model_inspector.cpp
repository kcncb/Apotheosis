#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "model_inspector.h"
#include "include/other_tools.h"
#include "model_crypto/model_crypto.h"

namespace detector
{

namespace
{

std::string trim(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

// Minimal tokenizer for name blobs. Both Python dict repr and JSON objects look
// similar enough that the same scanner handles both: we walk the blob, collect
// every quoted string (single- or double-quoted), and assume the ordered
// sequence of strings matches the class order. Integer keys are ignored.
// This is intentionally tolerant; upstream metadata is not strictly typed.
std::vector<std::string> extract_quoted_strings(const std::string& blob)
{
    std::vector<std::string> out;
    const size_t n = blob.size();
    size_t i = 0;
    while (i < n)
    {
        const char c = blob[i];
        if (c == '"' || c == '\'')
        {
            const char quote = c;
            ++i;
            std::string acc;
            while (i < n)
            {
                const char ch = blob[i];
                if (ch == '\\' && i + 1 < n)
                {
                    // skip escape
                    acc.push_back(blob[i + 1]);
                    i += 2;
                    continue;
                }
                if (ch == quote)
                {
                    ++i;
                    break;
                }
                acc.push_back(ch);
                ++i;
            }
            out.push_back(std::move(acc));
        }
        else
        {
            ++i;
        }
    }
    return out;
}

} // namespace

std::vector<std::string> parse_python_dict_names(const std::string& blob)
{
    return extract_quoted_strings(blob);
}

std::vector<std::string> parse_json_names(const std::string& blob)
{
    // Look for a "names" key and limit extraction to its value scope. If not
    // found, just take every quoted string.
    const std::string key = "\"names\"";
    const size_t at = blob.find(key);
    if (at == std::string::npos)
        return extract_quoted_strings(blob);

    size_t i = at + key.size();
    while (i < blob.size() && (std::isspace(static_cast<unsigned char>(blob[i])) || blob[i] == ':'))
        ++i;
    if (i >= blob.size())
        return {};

    const char open = blob[i];
    if (open != '{' && open != '[')
        return extract_quoted_strings(blob.substr(i));

    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    size_t j = i;
    for (; j < blob.size(); ++j)
    {
        if (blob[j] == open)
            ++depth;
        else if (blob[j] == close)
        {
            --depth;
            if (depth == 0)
            {
                ++j;
                break;
            }
        }
    }
    return extract_quoted_strings(blob.substr(i, j - i));
}

std::string read_ultralytics_engine_header(const std::string& engine_path)
{
    std::ifstream f(std::filesystem::u8path(engine_path), std::ios::binary);
    if (!f)
        return {};
    unsigned char len_bytes[4]{};
    f.read(reinterpret_cast<char*>(len_bytes), 4);
    if (!f)
        return {};
    const uint32_t meta_len =
        static_cast<uint32_t>(len_bytes[0]) |
        (static_cast<uint32_t>(len_bytes[1]) << 8) |
        (static_cast<uint32_t>(len_bytes[2]) << 16) |
        (static_cast<uint32_t>(len_bytes[3]) << 24);
    // Sanity-check the length; a raw engine would not be expected to start
    // with a plausible JSON-length prefix. Ultralytics metadata caps at a few
    // kilobytes in practice.
    if (meta_len == 0 || meta_len > 64 * 1024)
        return {};
    std::string blob(meta_len, '\0');
    f.read(blob.data(), static_cast<std::streamsize>(meta_len));
    if (!f || blob.empty() || (blob.front() != '{' && blob.front() != '['))
        return {};
    return blob;
}

std::vector<std::string> read_sidecar_class_names(const std::string& model_path,
                                                  ClassNamesSource* source_out)
{
    std::error_code ec;
    std::filesystem::path path(std::filesystem::u8path(model_path));
    const std::filesystem::path stem = path.parent_path() / path.stem();
    const std::filesystem::path json_path = std::filesystem::u8path(stem.u8string() + ".json");
    const std::filesystem::path names_path = std::filesystem::u8path(stem.u8string() + ".names");

    if (std::filesystem::exists(json_path, ec))
    {
        std::ifstream f(json_path);
        if (f)
        {
            std::stringstream ss;
            ss << f.rdbuf();
            auto names = parse_json_names(ss.str());
            if (!names.empty())
            {
                if (source_out) *source_out = ClassNamesSource::SidecarJson;
                return names;
            }
        }
    }

    if (std::filesystem::exists(names_path, ec))
    {
        std::ifstream f(names_path);
        if (f)
        {
            std::vector<std::string> names;
            std::string line;
            while (std::getline(f, line))
            {
                auto trimmed = trim(line);
                if (!trimmed.empty())
                    names.push_back(trimmed);
            }
            if (!names.empty())
            {
                if (source_out) *source_out = ClassNamesSource::SidecarNames;
                return names;
            }
        }
    }

    return {};
}

void pad_class_names(ModelMetadata& md, int expected_count)
{
    if (expected_count < 0)
        expected_count = 0;

    if (expected_count == 0 && !md.class_names.empty())
        expected_count = static_cast<int>(md.class_names.size());

    md.class_count = (std::max)(md.class_count, expected_count);

    if (md.class_count < 0)
        md.class_count = expected_count;

    while (static_cast<int>(md.class_names.size()) < md.class_count)
    {
        md.class_names.push_back("class_" + std::to_string(md.class_names.size()));
    }
    // If we got more names than the model's class count, trim them back so UI
    // only exposes what the model actually produces.
    if (static_cast<int>(md.class_names.size()) > md.class_count)
        md.class_names.resize(static_cast<size_t>(md.class_count));

    if (md.class_names.empty() && md.source == ClassNamesSource::None)
        md.source = ClassNamesSource::Fallback;
}

ModelMetadata inspect_onnx_model(const std::string& model_path, bool verbose)
{
    ModelMetadata out;

    try
    {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ModelInspector");
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);

        Ort::Session session(nullptr);
        oliver::Payload payload;
        if (oliver::is_oliver_path(model_path))
        {
            std::string error;
            if (!oliver::decrypt_file(model_path, payload, error))
            {
                if (verbose)
                    std::cerr << "[ModelInspector] oliver 模型解密失败: " << error << std::endl;
                return out;
            }
            if (payload.type != oliver::PayloadType::Onnx)
            {
                if (verbose)
                    std::cerr << "[ModelInspector] oliver 文件不是 ONNX 模型。" << std::endl;
                return out;
            }
            session = Ort::Session(env, payload.bytes.data(), payload.bytes.size(), options);
        }
        else
        {
            std::wstring wide_path = Utf8ToWide(model_path);
            session = Ort::Session(env, wide_path.c_str(), options);
        }

        Ort::TypeInfo input_type_info = session.GetInputTypeInfo(0);
        auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> input_shape = input_tensor_info.GetShape();

        out.fixed_input_size = true;
        out.fixed_input_size_known = true;
        for (int64_t dim : input_shape)
        {
            if (dim <= 0)
            {
                out.fixed_input_size = false;
                break;
            }
        }

        Ort::TypeInfo output_type_info = session.GetOutputTypeInfo(0);
        auto output_tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> output_shape = output_tensor_info.GetShape();
        if (output_shape.size() == 3)
        {
            const int64_t dim1 = output_shape[1];
            const int64_t dim2 = output_shape[2];
            const int64_t max_int = static_cast<int64_t>((std::numeric_limits<int>::max)());

            if (dim1 > 4 && dim1 <= max_int && (dim2 <= 0 || dim1 <= dim2))
            {
                out.class_count = static_cast<int>(dim1) - 4;
            }
            else if (dim2 > 4 && dim2 <= max_int)
            {
                out.class_count = static_cast<int>(dim2) - 4;
            }
        }

        try
        {
            Ort::ModelMetadata metadata = session.GetModelMetadata();
            Ort::AllocatorWithDefaultOptions allocator;
            auto val = metadata.LookupCustomMetadataMapAllocated("names", allocator);
            if (val)
            {
                std::string blob = val.get();
                out.class_names = parse_python_dict_names(blob);
                if (out.class_names.empty())
                    out.class_names = parse_json_names(blob);
                if (!out.class_names.empty())
                    out.source = ClassNamesSource::OnnxCustomMetadata;
            }
        }
        catch (const std::exception& e)
        {
            if (verbose)
                std::cerr << "[ModelInspector] Reading ONNX metadata failed: " << e.what() << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        if (verbose)
            std::cerr << "[ModelInspector] ONNX inspection failed: " << e.what() << std::endl;
    }

    if (out.class_names.empty())
    {
        ClassNamesSource source = ClassNamesSource::None;
        auto sidecar = read_sidecar_class_names(model_path, &source);
        if (!sidecar.empty())
        {
            out.class_names = std::move(sidecar);
            out.source = source;
        }
    }

    pad_class_names(out, out.class_count);
    return out;
}

} // namespace detector



