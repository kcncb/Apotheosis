#ifndef DETECTOR_MODEL_INSPECTOR_H
#define DETECTOR_MODEL_INSPECTOR_H

#include <string>
#include <vector>

namespace detector
{

// Source of class-name discovery. Mostly for debug / UI hints.
enum class ClassNamesSource
{
    None = 0,
    OnnxCustomMetadata,
    TrtEngineHeader,
    SidecarJson,
    SidecarNames,
    Fallback
};

struct ModelMetadata
{
    int class_count = 0;
    std::vector<std::string> class_names;
    ClassNamesSource source = ClassNamesSource::None;
    bool fixed_input_size = false;
    bool fixed_input_size_known = false;

    // 模型第 0 个输入张量的 H / W (NCHW 的 [2] / [3])。
    //
    // 采集侧的中心裁切边长恒等于模型输入边长, 所以这两个值是
    // detection_resolution 的唯一来源 —— 用户不再手填尺寸, 填错就会出现
    // 裁切尺寸和模型输入对不上、检测框与鼠标坐标空间错位。
    //
    // 任一为 0 表示动态形状或读取失败, 调用方应保持原值。
    int input_width  = 0;
    int input_height = 0;
};

// Lightweight ONNX inspection used by the launcher UI before inference starts.
// It reads tensor shapes and class metadata without appending any execution
// provider, so model/category UI can be populated as soon as ImGui opens.
ModelMetadata inspect_onnx_model(const std::string& model_path, bool verbose = false);

// Parsers exposed for reuse by backend implementations.
std::vector<std::string> parse_python_dict_names(const std::string& blob);
std::vector<std::string> parse_json_names(const std::string& blob);

// Read the 4-byte little-endian length prefix + JSON metadata that Ultralytics
// prepends to TensorRT engine files. Returns empty on any failure.
std::string read_ultralytics_engine_header(const std::string& engine_path);

// Try sidecar files next to the model: "<stem>.names" (one per line) or
// "<stem>.json" (JSON object with a "names" field).
std::vector<std::string> read_sidecar_class_names(const std::string& model_path,
                                                  ClassNamesSource* source_out);

// Pad `md.class_names` up to at least `expected_count` using synthetic
// "class_<i>" placeholders so UI code can always index by class_id.
void pad_class_names(ModelMetadata& md, int expected_count);

} // namespace detector

#endif // DETECTOR_MODEL_INSPECTOR_H
