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
