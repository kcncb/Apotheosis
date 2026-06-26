#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <dml_provider_factory.h>
#include <wrl/client.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>
#include <algorithm>
#include <sstream>
#include <dxgi.h>

#include "dml_detector.h"
#include "Apotheosis.h"
#include "postProcess.h"
#include "capture.h"
#include "other_tools.h"
#include "model_inspector.h"
#include "model_crypto/model_crypto.h"
#include "runtime/active_hotkey.h"

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;

namespace
{
void publishDmlModelMetadata(const DirectMLDetector& detector)
{
    detector::ModelMetadata md;
    md.class_count = detector.numberOfClasses();
    md.class_names = detector.classNames();
    detector::pad_class_names(md, md.class_count);
    if (md.source == detector::ClassNamesSource::None && !md.class_names.empty())
        md.source = detector::ClassNamesSource::OnnxCustomMetadata;

    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.sync_class_filters_from_model(md.class_count, md.class_names);
    }

    {
        std::lock_guard<std::mutex> lock(runtime::g_model_metadata_mutex);
        runtime::g_model_metadata = std::move(md);
    }
}
} // namespace

namespace
{
std::string codeToHex(unsigned long code)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << code;
    return oss.str();
}

struct ProviderAppendResult
{
    OrtStatus* status = nullptr;
    unsigned long seh_code = 0;
};

ProviderAppendResult appendDirectMLProviderSafely(Ort::SessionOptions& options, int deviceId)
{
    __try
    {
        return { OrtSessionOptionsAppendExecutionProvider_DML(options, deviceId), 0 };
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return { nullptr, GetExceptionCode() };
    }
}

std::string consumeOrtStatusMessage(OrtStatus* status)
{
    if (!status)
        return {};

    std::string message = Ort::GetApi().GetErrorMessage(status);
    Ort::GetApi().ReleaseStatus(status);
    return message;
}

bool tryInt64ToInt(int64_t value, int* out)
{
    if (!out)
    {
        return false;
    }

    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}
}

std::string GetDMLDeviceName(int deviceId)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))))
        return "Unknown";

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(dxgiFactory->EnumAdapters1(deviceId, &adapter)))
        return "Invalid device ID";

    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(adapter->GetDesc1(&desc)))
        return "Failed to get description";

    std::wstring wname(desc.Description);
    return WideToUtf8(wname);
}

std::vector<DmlAdapterInfo> EnumerateDMLAdapters()
{
    std::vector<DmlAdapterInfo> adapters;

    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))))
        return adapters;

    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(dxgiFactory->EnumAdapters1(i, &adapter)))
            break;

        DXGI_ADAPTER_DESC1 desc;
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;

        std::wstring wname(desc.Description);
        adapters.push_back({ static_cast<int>(i), WideToUtf8(wname) });
    }

    return adapters;
}

DirectMLDetector::DirectMLDetector()
    :
    env(ORT_LOGGING_LEVEL_WARNING, "DML_Detector"),
    memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
}

DirectMLDetector::DirectMLDetector(const std::string& model_path)
    : DirectMLDetector()
{
    initialize(model_path);
}

DirectMLDetector::~DirectMLDetector()
{
    shouldExit = true;
    inferenceCV.notify_all();
}

bool DirectMLDetector::initialize(const std::string& model_path)
{
    try
    {
        session_options = Ort::SessionOptions{};
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        session_options.SetIntraOpNumThreads(1);
        session_options.SetInterOpNumThreads(1);

        bool using_dml_provider = true;
        const ProviderAppendResult dml_status = appendDirectMLProviderSafely(session_options, config.dml_device_id);
        if (dml_status.status || dml_status.seh_code != 0)
        {
            using_dml_provider = false;
            const std::string reason = dml_status.status
                ? consumeOrtStatusMessage(dml_status.status)
                : ("SEH exception " + codeToHex(dml_status.seh_code));
            std::cerr << "[DirectML] provider initialization failed on adapter "
                      << config.dml_device_id << " (" << GetDMLDeviceName(config.dml_device_id)
                      << "): " << reason << ". Falling back to ONNX Runtime CPU provider." << std::endl;
            session_options = Ort::SessionOptions{};
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            session_options.SetIntraOpNumThreads(1);
            session_options.SetInterOpNumThreads(1);
        }

        if (config.verbose)
        {
            if (using_dml_provider)
                std::cout << "[DirectML] Using adapter: " << GetDMLDeviceName(config.dml_device_id) << std::endl;
            else
                std::cout << "[DirectML] Running with ONNX Runtime CPU provider fallback." << std::endl;
        }

        if (oliver::is_oliver_path(model_path))
        {
            oliver::Payload payload;
            std::string error;
            if (!oliver::decrypt_file(model_path, payload, error))
            {
                std::cerr << "[DirectML] oliver 模型解密失败: " << error << std::endl;
                return false;
            }
            if (payload.type != oliver::PayloadType::Onnx)
            {
                std::cerr << "[DirectML] oliver 文件不是 ONNX 模型，无法使用 DML 后端。" << std::endl;
                return false;
            }
            initializeModelFromBytes(payload.bytes, model_path);
        }
        else
        {
            initializeModel(model_path);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[DirectML] initialize failed: " << e.what() << std::endl;
        return false;
    }
    return true;
}

void DirectMLDetector::requestExit()
{
    {
        std::lock_guard<std::mutex> lock(inferenceMutex);
        shouldExit = true;
        frameReady = false;
        currentFrame.release();
    }
    inferenceCV.notify_all();
}

void DirectMLDetector::initializeModel(const std::string& model_path)
{
    std::wstring model_path_wide = Utf8ToWide(model_path);
    session = Ort::Session(env, model_path_wide.c_str(), session_options);
    readModelMetadata(model_path);
}

void DirectMLDetector::initializeModelFromBytes(const std::vector<uint8_t>& model_bytes, const std::string& display_path)
{
    session = Ort::Session(env,
                           model_bytes.data(),
                           model_bytes.size(),
                           session_options);
    readModelMetadata(display_path);
}

void DirectMLDetector::readModelMetadata(const std::string& model_path)
{

    input_name = session.GetInputNameAllocated(0, allocator).get();
    output_name = session.GetOutputNameAllocated(0, allocator).get();

    Ort::TypeInfo input_type_info = session.GetInputTypeInfo(0);
    auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
    input_shape = input_tensor_info.GetShape();

    bool isStatic = true;
    for (auto d : input_shape) if (d <= 0) isStatic = false;

    if (isStatic != config.fixed_input_size)
    {
        config.fixed_input_size = isStatic;
        detector_model_changed.store(true);
        std::cout << "[DML] Automatically set fixed_input_size = " << (isStatic ? "true" : "false") << std::endl;
    }

    {
        Ort::TypeInfo output_type_info = session.GetOutputTypeInfo(0);
        auto tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> output_shape = tensor_info.GetShape();
        num_classes_ = -1;
        if (output_shape.size() == 3)
        {
            int channels = 0;
            int trailing = 0;
            if (tryInt64ToInt(output_shape[1], &channels) && channels > 4 &&
                (!tryInt64ToInt(output_shape[2], &trailing) || channels <= trailing))
            {
                num_classes_ = channels - 4;
            }
            else if (tryInt64ToInt(output_shape[2], &channels) && channels > 4)
            {
                num_classes_ = channels - 4;
            }
            else
            {
                std::cerr << "[DirectMLDetector] Output tensor channel dimension is invalid." << std::endl;
            }
        }
        else
        {
            std::cerr << "[DirectMLDetector] Unexpected output tensor shape." << std::endl;
        }
    }

    // Class names are parsed below; if the metadata is richer than the
    // channel-derived count, trust it. Handles NMS-decoded outputs ([N, 6]
    // = x1,y1,x2,y2,score,classId) where channels-4 == 2 regardless of how
    // many classes the model was trained on. We re-apply this after the
    // names parse runs so both sources agree before publishing.
    auto reconcile_class_count = [this]()
    {
        if (!class_names_.empty()
            && static_cast<int>(class_names_.size()) > num_classes_)
        {
            num_classes_ = static_cast<int>(class_names_.size());
        }
    };

    // Pull class names out of the ONNX custom metadata. Ultralytics exports
    // stash a Python dict repr under the "names" key; we tolerate both dict
    // repr and JSON array formats.
    class_names_.clear();
    try
    {
        Ort::ModelMetadata md = session.GetModelMetadata();
        Ort::AllocatorWithDefaultOptions meta_alloc;
        auto val = md.LookupCustomMetadataMapAllocated("names", meta_alloc);
        if (val)
        {
            std::string blob = val.get();
            class_names_ = detector::parse_python_dict_names(blob);
            if (class_names_.empty())
                class_names_ = detector::parse_json_names(blob);
        }
    }
    catch (const std::exception& e)
    {
        if (config.verbose)
            std::cerr << "[DirectMLDetector] Reading ONNX metadata failed: " << e.what() << std::endl;
    }

    if (class_names_.empty())
    {
        detector::ClassNamesSource src = detector::ClassNamesSource::None;
        auto sidecar = detector::read_sidecar_class_names(model_path, &src);
        if (!sidecar.empty())
            class_names_ = std::move(sidecar);
    }

    reconcile_class_count();
}

std::vector<Detection> DirectMLDetector::detect(const cv::Mat& input_frame)
{
    std::vector<cv::Mat> batch = { input_frame };
    auto batchResult = detectBatch(batch);
    if (!batchResult.empty())
        return batchResult[0];
    else
        return {};
}

std::vector<std::vector<Detection>> DirectMLDetector::detectBatch(const std::vector<cv::Mat>& frames)
{
    std::vector<std::vector<Detection>> empty;
    if (frames.empty()) return empty;
    const size_t batch_size = frames.size();

    int model_h = -1;
    int model_w = -1;
    if (input_shape.size() > 2)
    {
        int converted = 0;
        if (tryInt64ToInt(input_shape[2], &converted))
        {
            model_h = converted;
        }
    }
    if (input_shape.size() > 3)
    {
        int converted = 0;
        if (tryInt64ToInt(input_shape[3], &converted))
        {
            model_w = converted;
        }
    }
    const bool useFixed = config.fixed_input_size && model_h > 0 && model_w > 0;

    const int target_h = useFixed ? model_h : config.detection_resolution;
    const int target_w = useFixed ? model_w : config.detection_resolution;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> input_tensor_values(
        batch_size * static_cast<size_t>(3) * static_cast<size_t>(target_h) * static_cast<size_t>(target_w));

    for (size_t b = 0; b < batch_size; ++b)
    {
        cv::Mat bgrFrame;
        switch (frames[b].channels())
        {
        case 4:
            cv::cvtColor(frames[b], bgrFrame, cv::COLOR_BGRA2BGR);
            break;
        case 3:
            bgrFrame = frames[b];
            break;
        case 1:
            cv::cvtColor(frames[b], bgrFrame, cv::COLOR_GRAY2BGR);
            break;
        default:
            bgrFrame = cv::Mat::zeros(frames[b].size(), CV_8UC3);
            break;
        }

        cv::Mat resized;
        cv::resize(bgrFrame, resized, cv::Size(target_w, target_h));
        cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);
        resized.convertTo(resized, CV_32FC3, 1.0f / 255.0f);

        const float* src = reinterpret_cast<const float*>(resized.data);
        for (int h = 0; h < target_h; ++h)
            for (int w = 0; w < target_w; ++w)
                for (int c = 0; c < 3; ++c)
                {
                    size_t dstIdx = b * static_cast<size_t>(3) * static_cast<size_t>(target_h) * static_cast<size_t>(target_w)
                        + static_cast<size_t>(c) * static_cast<size_t>(target_h) * static_cast<size_t>(target_w)
                        + static_cast<size_t>(h) * static_cast<size_t>(target_w)
                        + static_cast<size_t>(w);
                    input_tensor_values[dstIdx] = src[(h * target_w + w) * 3 + c];
                }
    }
    auto t1 = std::chrono::steady_clock::now();

    std::vector<int64_t> ort_input_shape{
        static_cast<int64_t>(batch_size),
        3,
        static_cast<int64_t>(target_h),
        static_cast<int64_t>(target_w)
    };
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, input_tensor_values.data(), input_tensor_values.size(),
        ort_input_shape.data(), ort_input_shape.size());

    const char* input_names[] = { input_name.c_str() };
    const char* output_names[] = { output_name.c_str() };

    auto t2 = std::chrono::steady_clock::now();
    auto output_tensors = session.Run(Ort::RunOptions{ nullptr },
        input_names, &input_tensor, 1,
        output_names, 1);
    auto t3 = std::chrono::steady_clock::now();

    float* outData = output_tensors.front().GetTensorMutableData<float>();
    Ort::TensorTypeAndShapeInfo outInfo = output_tensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> outShape = outInfo.GetShape(); // [B, rows, cols]
    if (outShape.size() < 3)
    {
        std::cerr << "[DirectMLDetector] Unexpected output tensor rank." << std::endl;
        return empty;
    }

    int rows = 0;
    int cols = 0;
    if (!tryInt64ToInt(outShape[1], &rows) || !tryInt64ToInt(outShape[2], &cols) || rows <= 0 || cols <= 0)
    {
        std::cerr << "[DirectMLDetector] Output tensor dimensions are invalid." << std::endl;
        return empty;
    }
    const int num_classes = rows - 4;

    std::vector<std::vector<Detection>> batchDetections(batch_size);
    const SmallTargetDecode st = computeSmallTargetDecode();
    float conf_thr = st.decodeFloor;
    float nms_thr = config.nms_threshold;

    auto t4 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> nmsTimeTmp{ 0 };

    for (size_t b = 0; b < batch_size; ++b)
    {
        const float* ptr = outData + b * rows * cols;
        std::vector<Detection> detections;

        std::vector<int64_t> shp = { static_cast<int64_t>(rows), static_cast<int64_t>(cols) };
        detections = postProcessYoloDML(ptr, shp, num_classes, conf_thr, nms_thr, &nmsTimeTmp,
                                        st.baseConf, st.areaThreshPx);

        if (useFixed && (target_w != config.detection_resolution))
        {
            float scale = static_cast<float>(config.detection_resolution) / target_w;
            for (auto& d : detections)
            {
                d.box.x = static_cast<int>(d.box.x * scale);
                d.box.y = static_cast<int>(d.box.y * scale);
                d.box.width = static_cast<int>(d.box.width * scale);
                d.box.height = static_cast<int>(d.box.height * scale);
            }
        }

        batchDetections[b] = std::move(detections);
    }
    auto t5 = std::chrono::steady_clock::now();

    lastPreprocessTimeValue = t1 - t0;
    lastInferenceTimeValue = t3 - t2;
    lastCopyTimeValue = t4 - t3;
    lastPostprocessTimeValue = t5 - t4;
    lastNmsTimeValue = nmsTimeTmp;

    return batchDetections;
}


void DirectMLDetector::processFrame(const cv::Mat& frame)
{
    std::unique_lock<std::mutex> lock(inferenceMutex);
    currentFrame = frame;
    frameReady = true;
    inferenceCV.notify_one();
}

void DirectMLDetector::inferenceThread()
{
    try
    {
        while (!shouldExit)
        {
            if (detector_model_changed.load())
            {
                initializeModel("models/" + config.ai_model);
                publishDmlModelMetadata(*this);
                detection_resolution_changed.store(true);
                detector_model_changed.store(false);
                std::cout << "[DML] Detector reloaded: " << config.ai_model << std::endl;
            }


            cv::Mat frame;
            bool hasNewFrame = false;
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                if (!frameReady && !shouldExit)
                    inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

                if (shouldExit) break;

                if (frameReady)
                {
                    frame = std::move(currentFrame);
                    frameReady = false;
                    hasNewFrame = true;
                }
            }

            if (hasNewFrame && !frame.empty())
            {
                std::vector<cv::Mat> batchFrames = { frame };
                auto detectionsBatch = detectBatch(batchFrames);
                if (detectionsBatch.empty())
                {
                    continue;
                }
                const std::vector<Detection>& detections = detectionsBatch.back();
                std::vector<Detection> filteredDetections = detections;
                capDetectionsToMax(filteredDetections, config.max_detections);

                std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
                detectionBuffer.boxes.clear();
                detectionBuffer.classes.clear();
                detectionBuffer.confidences.clear();
                for (const auto& d : filteredDetections) {
                    detectionBuffer.boxes.push_back(d.box);
                    detectionBuffer.classes.push_back(d.classId);
                    detectionBuffer.confidences.push_back(d.confidence);
                }
                detectionBuffer.bumpVersionLocked();
                detectionBuffer.cv.notify_all();
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[DML] Inference thread crashed: " << e.what() << std::endl;
        shouldExit = true;
        inferenceCV.notify_all();
        detectionBuffer.cv.notify_all();
    }
    catch (...)
    {
        std::cerr << "[DML] Inference thread crashed: unknown exception." << std::endl;
        shouldExit = true;
        inferenceCV.notify_all();
        detectionBuffer.cv.notify_all();
    }
}

