#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <mutex>
#include <sstream>
#include <cstring>

#include "trt_detector.h"
#include "nvinf.h"
#include "Apotheosis.h"
#include "other_tools.h"
#include "postProcess.h"
#include "model_inspector.h"
#include "model_crypto/model_crypto.h"
#include "cuda_preprocess.h"
#include "depth/depth_mask.h"
#include "capture.h"
#include "runtime/active_hotkey.h"

int model_quant;
std::vector<float> outputData;

// Max candidates the GPU decode+filter kernel will emit before CPU NMS.
// Per-output device buffer layout is: [int counter (16B aligned) | float
// candidates[kMaxCandidates * 6]]. 1024 is ample for YOLOv8 at the common
// confidence thresholds (typical real scenes yield 20-200 kept).
static constexpr int kMaxCandidates = 1024;
static constexpr size_t kDecodeHeaderBytes = 16;
static constexpr size_t kDecodeCandidatesBytes = static_cast<size_t>(kMaxCandidates) * 6 * sizeof(float);
static constexpr size_t kDecodeBlockBytes = kDecodeHeaderBytes + kDecodeCandidatesBytes;

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;

static bool error_logged = false;

namespace {
bool tryGetDimInt(int64_t value, int* out)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    *out = static_cast<int>(value);
    return true;
}

void publishTrtModelMetadata(const TrtDetector& detector)
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

bool isAsciiPath(const std::string& path)
{
    return std::all_of(path.begin(), path.end(), [](unsigned char ch) {
        return ch < 0x80;
    });
}

std::string makeAsciiEngineStem(const std::filesystem::path& modelPath)
{
    const std::string stem = modelPath.stem().u8string();
    if (isAsciiPath(stem))
        return stem;

    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : stem)
    {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }

    std::ostringstream oss;
    oss << "model_" << std::hex << hash;
    return oss.str();
}
std::string makeTensorRtParserPath(const std::filesystem::path& onnxPath,
                                   const std::filesystem::path& engineCacheDir,
                                   std::filesystem::path* temporaryPath)
{
    const std::string original = onnxPath.u8string();
    if (isAsciiPath(original))
        return original;

    std::error_code ec;
    const std::filesystem::path asciiDir = engineCacheDir / "_trt_parser_tmp";
    std::filesystem::create_directories(asciiDir, ec);
    if (ec)
    {
        std::cerr << "[Detector] Failed to create TensorRT parser temp dir: "
                  << ec.message() << std::endl;
        return original;
    }

    const std::filesystem::path asciiPath = asciiDir / "model.onnx";
    std::filesystem::copy_file(onnxPath, asciiPath,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec)
    {
        std::cerr << "[Detector] Failed to copy ONNX to ASCII TensorRT parser path: "
                  << ec.message() << std::endl;
        return original;
    }

    if (temporaryPath)
        *temporaryPath = asciiPath;

    std::cout << "[Detector] Copied non-ASCII ONNX path for TensorRT parser: "
              << asciiPath.u8string() << std::endl;
    return asciiPath.u8string();
}
bool tryGetPositiveDimInt(int64_t value, int* out)
{
    if (value <= 0)
        return false;
    return tryGetDimInt(value, out);
}

bool engineHasHalfInput(const std::filesystem::path& enginePath)
{
    std::unique_ptr<nvinfer1::IRuntime> probeRuntime(nvinfer1::createInferRuntime(gLogger));
    if (!probeRuntime)
        return false;

    std::unique_ptr<nvinfer1::ICudaEngine> probeEngine(
        loadEngineFromFile(enginePath.u8string(), probeRuntime.get()));
    if (!probeEngine)
        return false;

    for (int i = 0; i < probeEngine->getNbIOTensors(); ++i)
    {
        const char* name = probeEngine->getIOTensorName(i);
        if (probeEngine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
            return probeEngine->getTensorDataType(name) == nvinfer1::DataType::kHALF;
    }
    return false;
}

// Engine is "fully fp16" only if every IO tensor (inputs AND outputs) is
// kHALF. The strict FP16 build pipeline produces engines that satisfy this;
// pre-existing engines that were cached before the strict FP16 policy will
// have FP32 outputs and must be rebuilt so the GPU decode kernel doesn't
// have to run a fp16/fp32 dispatch on each frame and so we don't pay an
// implicit precision cast at the engine boundary.
bool engineIsFullyHalf(const std::filesystem::path& enginePath)
{
    std::unique_ptr<nvinfer1::IRuntime> probeRuntime(nvinfer1::createInferRuntime(gLogger));
    if (!probeRuntime)
        return false;

    std::unique_ptr<nvinfer1::ICudaEngine> probeEngine(
        loadEngineFromFile(enginePath.u8string(), probeRuntime.get()));
    if (!probeEngine)
        return false;

    for (int i = 0; i < probeEngine->getNbIOTensors(); ++i)
    {
        const char* name = probeEngine->getIOTensorName(i);
        if (probeEngine->getTensorDataType(name) != nvinfer1::DataType::kHALF)
            return false;
    }
    return true;
}

// Same FP16-everywhere check but on an in-memory serialized engine — used
// for the .oliver encrypted-engine cache where we never write the engine
// to disk in plaintext.
bool engineBytesAreFullyHalf(const void* data, size_t size)
{
    if (!data || size == 0)
        return false;

    std::unique_ptr<nvinfer1::IRuntime> probeRuntime(nvinfer1::createInferRuntime(gLogger));
    if (!probeRuntime)
        return false;

    std::unique_ptr<nvinfer1::ICudaEngine> probeEngine(
        probeRuntime->deserializeCudaEngine(data, size));
    if (!probeEngine)
        return false;

    for (int i = 0; i < probeEngine->getNbIOTensors(); ++i)
    {
        const char* name = probeEngine->getIOTensorName(i);
        if (probeEngine->getTensorDataType(name) != nvinfer1::DataType::kHALF)
            return false;
    }
    return true;
}

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : value)
    {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(uint64_t value)
{
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

// See dml_detector.cpp:intersectsDepthMask for the rationale. Both detectors
// keep their own copies because they live behind a runtime polymorphic
// boundary and we don't want to drag a shared utility into the public
// detector header just for this one helper.
bool intersectsDepthMask(const cv::Rect& box, const cv::Mat& mask, float ratio_threshold)
{
    if (box.width <= 0 || box.height <= 0 || mask.empty() || mask.type() != CV_8UC1)
        return false;

    const cv::Rect imageBounds(0, 0, mask.cols, mask.rows);
    const cv::Rect clipped = box & imageBounds;
    if (clipped.width <= 0 || clipped.height <= 0)
        return false;

    const cv::Mat roi = mask(clipped);
    const int occluded = cv::countNonZero(roi);
    if (occluded <= 0)
        return false;

    const int total = clipped.area();
    if (total <= 0)
        return false;

    const float ratio = static_cast<float>(occluded) / static_cast<float>(total);
    const float threshold = std::clamp(ratio_threshold, 0.0f, 1.0f);
    return ratio >= threshold;
}

void filterDetectionsByDepthMask(std::vector<Detection>& detections)
{
    static cv::Mat holdTtl;

    if (detections.empty())
        return;

    if (!config.depth_inference_enabled || !config.depth_mask_enabled)
    {
        holdTtl.release();
        return;
    }

    const int holdFrames = std::clamp(config.depth_mask_hold_frames, 0, 120);
    cv::Mat currentMask = getCurrentDetectionSuppressionMask();
    cv::Mat suppressionMask;

    if (holdFrames <= 0)
    {
        holdTtl.release();
        suppressionMask = currentMask;
    }
    else
    {
        if (!currentMask.empty() && currentMask.type() == CV_8UC1)
        {
            if (holdTtl.empty() || holdTtl.size() != currentMask.size())
                holdTtl = cv::Mat::zeros(currentMask.size(), CV_16UC1);
            cv::subtract(holdTtl, cv::Scalar(1), holdTtl);
            holdTtl.setTo(cv::Scalar(static_cast<uint16_t>(holdFrames)), currentMask);
        }
        else if (!holdTtl.empty())
        {
            cv::subtract(holdTtl, cv::Scalar(1), holdTtl);
        }

        if (!holdTtl.empty() && cv::countNonZero(holdTtl) > 0)
        {
            cv::compare(holdTtl, cv::Scalar(0), suppressionMask, cv::CMP_GT);
        }
        else
        {
            suppressionMask.release();
        }
    }

    if (suppressionMask.empty() || suppressionMask.type() != CV_8UC1)
        return;

    const float ratio = std::clamp(config.depth_mask_suppression_ratio, 0.0f, 1.0f);

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&suppressionMask, ratio](const Detection& det) { return intersectsDepthMask(det.box, suppressionMask, ratio); }),
        detections.end());
}
} // namespace

TrtDetector::TrtDetector()
    : frameReady(false),
    shouldExit(false),
    useCudaGraph(false),
    cudaGraphCaptured(false),
    cudaGraph(nullptr),
    cudaGraphExec(nullptr),
    inputBufferDevice(nullptr),
    img_scale(1.0f),
    numClasses(0)
{
    stream = nullptr;
    cudaStreamCreate(&stream);
}

TrtDetector::~TrtDetector()
{
    destroyCudaGraph();
    freePinnedOutputs();
    freeTransposedBuffers();

    for (auto& binding : inputBindings) if (binding.second) cudaFree(binding.second);
    for (auto& binding : outputBindings) if (binding.second) cudaFree(binding.second);
    if (inputBufferDevice) cudaFree(inputBufferDevice);
    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);
    for (int s = 0; s < 2; ++s)
    {
        if (slotDoneEvent[s]) { cudaEventDestroy(slotDoneEvent[s]); slotDoneEvent[s] = nullptr; }
    }
    if (stream) cudaStreamDestroy(stream);
}

void TrtDetector::freePinnedOutputs()
{
    for (auto& kv : pinnedOutputBuffers)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffers.clear();
    for (auto& kv : pinnedOutputBuffersB)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffersB.clear();
}

void TrtDetector::freeTransposedBuffers()
{
    for (auto& kv : transposedDeviceBuffers)
    {
        if (kv.second) cudaFree(kv.second);
    }
    transposedDeviceBuffers.clear();
    transposedSizes.clear();
    outputNeedsTranspose.clear();
}

void TrtDetector::allocatePinnedOutputs()
{
    freePinnedOutputs();

    for (const auto& name : outputNames)
    {
        // Pinned buffer size matches what the stream D2Hs: for GPU-decoded
        // outputs that's [counter | candidates] (transposedSizes[name]); for
        // raw outputs that's the native engine tensor (outputSizes[name]).
        size_t bytes = outputSizes[name];
        auto tsIt = transposedSizes.find(name);
        if (tsIt != transposedSizes.end() && tsIt->second > 0)
            bytes = tsIt->second;
        if (bytes == 0) continue;

        for (int slot = 0; slot < numSlots; ++slot)
        {
            void* hostPtr = nullptr;
            cudaError_t err = cudaHostAlloc(&hostPtr, bytes, cudaHostAllocDefault);
            if (err != cudaSuccess)
            {
                std::cerr << "[Detector] cudaHostAlloc failed for output " << name
                    << " slot " << slot << " (" << bytes << " bytes): "
                    << cudaGetErrorString(err) << std::endl;
                continue;
            }
            pinnedSlot(slot)[name] = hostPtr;
        }

        if (config.verbose)
        {
            std::cout << "[Detector] Allocated " << numSlots << " pinned host buffer(s) for output "
                << name << ": " << bytes << " bytes each" << std::endl;
        }
    }
}

void TrtDetector::destroyCudaGraph()
{
    if (cudaGraphExec)
    {
        cudaGraphExecDestroy(cudaGraphExec);
        cudaGraphExec = nullptr;
    }
    if (cudaGraph)
    {
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
    }
    cudaGraphCaptured = false;
}

void TrtDetector::captureCudaGraph()
{
    if (!useCudaGraph || cudaGraphCaptured) return;

    destroyCudaGraph();

    cudaStreamSynchronize(stream);

    cudaError_t st = cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] BeginCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    context->enqueueV3(stream);
    cudaEventRecord(inferenceCompleteEvent, stream);

    for (const auto& name : outputNames)
    {
        if (!pinnedOutputBuffers.count(name)) continue;

        const bool needsT = outputNeedsTranspose.count(name) && outputNeedsTranspose[name];
        if (needsT)
        {
            const int C = outputC[name];
            const int N = outputN[name];
            const bool cnLayout = outputCnLayout[name];
            const bool isHalf = (outputTypes[name] == nvinfer1::DataType::kHALF);
            unsigned char* devBlock = reinterpret_cast<unsigned char*>(transposedDeviceBuffers[name]);
            int* devCounter = reinterpret_cast<int*>(devBlock);
            float* devCandidates = reinterpret_cast<float*>(devBlock + kDecodeHeaderBytes);

            launch_decode_and_filter(
                outputBindings[name], C, N, numClasses, isHalf,
                config.confidence_threshold, img_scale,
                kMaxCandidates, cnLayout, devCounter, devCandidates, stream
            );

            cudaMemcpyAsync(pinnedOutputBuffers[name], devBlock,
                transposedSizes[name], cudaMemcpyDeviceToHost, stream);
        }
        else
        {
            cudaMemcpyAsync(pinnedOutputBuffers[name],
                outputBindings[name],
                outputSizes[name],
                cudaMemcpyDeviceToHost,
                stream);
        }
    }

    st = cudaStreamEndCapture(stream, &cudaGraph);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] EndCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    st = cudaGraphInstantiate(&cudaGraphExec, cudaGraph, 0);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] GraphInstantiate failed: "
            << cudaGetErrorString(st) << std::endl;
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
        return;
    }

    cudaGraphCaptured = true;
}

inline void TrtDetector::launchCudaGraph()
{
    auto err = cudaGraphLaunch(cudaGraphExec, stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] GraphLaunch failed: " << cudaGetErrorString(err) << std::endl;
    }
}

void TrtDetector::getInputNames()
{
    inputNames.clear();
    inputSizes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
        {
            inputNames.emplace_back(name);
            if (config.verbose)
            {
                std::cout << "[Detector] Detected input: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getOutputNames()
{
    outputNames.clear();
    outputSizes.clear();
    outputTypes.clear();
    outputShapes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kOUTPUT)
        {
            outputNames.emplace_back(name);
            outputTypes[name] = engine->getTensorDataType(name);

            if (config.verbose)
            {
                std::cout << "[Detector] Detected output: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getBindings()
{
    for (auto& binding : inputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    inputBindings.clear();

    for (auto& binding : outputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    outputBindings.clear();

    for (const auto& name : inputNames)
    {
        size_t size = inputSizes[name];
        if (size > 0)
        {
            void* ptr = nullptr;

            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                inputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for input " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate input memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }

    for (const auto& name : outputNames)
    {
        size_t size = outputSizes[name];
        if (size > 0) {
            void* ptr = nullptr;
            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                outputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for output " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate output memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }
}

bool TrtDetector::initialize(const std::string& model_path)
{
    // TrtDetector is used as a global singleton; on a restart after stop() the
    // shouldExit flag would still be true from the previous session and the
    // inference thread would exit immediately. Reset it here so each start is
    // self-contained.
    shouldExit = false;
    frameReady = false;
    pendingFrameType = PendingFrameType::None;

    class_names_.clear();
    {
        // Best-effort probe of Ultralytics-style JSON metadata prepended to
        // the engine file. Failures silently yield an empty class name list;
        // the UI falls back to synthetic "class_<id>" labels.
        std::string engine_path = model_path;
        std::filesystem::path maybe(std::filesystem::u8path(model_path));
        if (!maybe.is_absolute())
        {
            std::error_code ec;
            auto abs = std::filesystem::absolute(maybe, ec);
            if (!ec) engine_path = abs.u8string();
        }
        auto blob = detector::read_ultralytics_engine_header(engine_path);
        if (!blob.empty())
            class_names_ = detector::parse_json_names(blob);

        if (class_names_.empty())
        {
            detector::ClassNamesSource src = detector::ClassNamesSource::None;
            auto sidecar = detector::read_sidecar_class_names(engine_path, &src);
            if (!sidecar.empty())
                class_names_ = std::move(sidecar);
        }
    }

    runtime.reset(nvinfer1::createInferRuntime(gLogger));
    loadEngine(model_path);
    if (!engine)
    {
        std::cerr << "[Detector] Engine loading failed" << std::endl;
        return false;
    }

    context.reset(engine->createExecutionContext());
    if (!context)
    {
        std::cerr << "[Detector] Context creation failed" << std::endl;
        return false;
    }

    getInputNames();
    getOutputNames();
    if (inputNames.empty())
    {
        std::cerr << "[Detector] No input tensors found" << std::endl;
        return false;
    }
    inputName = inputNames[0];

    nvinfer1::Dims inputDims = context->getTensorShape(inputName.c_str());
    bool isStatic = true;
    for (int i = 0; i < inputDims.nbDims; ++i)
        if (inputDims.d[i] <= 0) isStatic = false;

    if (isStatic != config.fixed_input_size)
    {
        config.fixed_input_size = isStatic;
        detector_model_changed.store(true);
        std::cout << "[Detector] Automatically set fixed_input_size = " << (isStatic ? "true" : "false") << std::endl;
    }

    const int target = config.detection_resolution;
    if (!isStatic)
    {
        nvinfer1::Dims4 newShape{ 1, 3, target, target };
        context->setInputShape(inputName.c_str(), newShape);
        if (!context->allInputDimensionsSpecified())
        {
            std::cerr << "[Detector] Failed to set input dimensions" << std::endl;
            return false;
        }
        inputDims = context->getTensorShape(inputName.c_str());
    }

    inputSizes.clear();
    outputSizes.clear();
    outputShapes.clear();
    outputTypes.clear();
    fp16OutputScratch.clear();
    freeTransposedBuffers();

    for (const auto& inName : inputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(inName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(inName.c_str());
        inputSizes[inName] = getSizeByDim(d) * getElementSize(dt);
    }
    for (const auto& outName : outputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(outName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(outName.c_str());
        outputSizes[outName] = getSizeByDim(d) * getElementSize(dt);
        std::vector<int64_t> shape(d.nbDims);
        for (int j = 0; j < d.nbDims; ++j) shape[j] = d.d[j];
        outputShapes[outName] = std::move(shape);
        outputTypes[outName] = dt;
    }

    getBindings();

    if (!outputNames.empty())
    {
        // Auto-detect YOLO output layout. Ultralytics' default export gives
        // [1, C, N] (channels-major: shape[1] = 4 + numClasses, shape[2] =
        // anchor count) but third-party / re-exported ONNXs sometimes ship
        // the transposed [1, N, C] form. DML's detector already auto-detects
        // both; mirror that logic here so a model that "works on DML" never
        // silently returns zero detections on TRT just because of layout.
        // We pick C as the smaller of the two trailing dims that is also > 4
        // (i.e. capable of carrying box+class scores).
        const std::string& mainOut = outputNames[0];
        nvinfer1::Dims outDims = context->getTensorShape(mainOut.c_str());
        int64_t dim1 = (outDims.nbDims >= 2) ? outDims.d[1] : 0;
        int64_t dim2 = (outDims.nbDims >= 3) ? outDims.d[2] : 0;

        int64_t channels64 = 0;
        bool cnLayout = true;
        if (dim1 > 4 && dim2 > 0 && dim1 <= dim2)
        {
            channels64 = dim1;
            cnLayout = true;
        }
        else if (dim2 > 4 && dim1 > 0 && dim2 < dim1)
        {
            channels64 = dim2;
            cnLayout = false;
        }
        else if (dim1 > 4)
        {
            channels64 = dim1;
            cnLayout = true;
        }
        else if (dim2 > 4)
        {
            channels64 = dim2;
            cnLayout = false;
        }

        const int64_t classes64 = (channels64 > 4) ? (channels64 - 4) : 1;
        int classes = 0;
        if (!tryGetDimInt(classes64, &classes) || classes <= 0)
        {
            if (!class_names_.empty())
            {
                classes = static_cast<int>(class_names_.size());
                std::cerr << "[Detector] Invalid output dimensions for classes; using "
                          << classes << " class names from model metadata." << std::endl;
            }
            else
            {
                std::cerr << "[Detector] Invalid output dimensions for classes" << std::endl;
                return false;
            }
        }
        numClasses = classes;

        if (config.verbose)
        {
            std::cout << "[Detector] Output '" << mainOut << "' shape=["
                      << outDims.d[0] << "," << dim1 << "," << dim2 << "]"
                      << " layout=" << (cnLayout ? "[1,C,N]" : "[1,N,C]")
                      << " numClasses=" << numClasses << std::endl;
        }
    }

    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(inputDims.d[1], &c)
        || !tryGetPositiveDimInt(inputDims.d[2], &h)
        || !tryGetPositiveDimInt(inputDims.d[3], &w))
    {
        std::cerr << "[Detector] Invalid input dimensions" << std::endl;
        return false;
    }

    // Square-only policy: the preprocessing path assumes a single scalar
    // scale factor (img_scale) to map from model-space back to capture-space.
    // Reject non-square engines up front so we don't silently produce wrong
    // coordinates via letterbox omission.
    if (h != w)
    {
        std::cerr << "[Detector] Non-square model input not supported (got "
                  << h << "x" << w << "). Use a square detection model." << std::endl;
        return false;
    }

    // FP16-only policy: the preprocess kernel writes __half directly into the
    // engine input binding. Reject engines whose input tensor is not FP16 to
    // avoid a silent truncation/extension mismatch. Rebuild the engine with
    // export_enable_fp16=true (the default) to fix.
    {
        nvinfer1::DataType inDt = engine->getTensorDataType(inputName.c_str());
        if (inDt != nvinfer1::DataType::kHALF)
        {
            std::cerr << "[Detector] FP16-only build: engine input dtype must be kHALF. "
                      << "Delete the cached .engine and rebuild with FP16 enabled."
                      << std::endl;
            return false;
        }
    }

    // Pre-compute decode metadata for YOLOv8/v11-style raw outputs. The
    // tensor can be either [1, C, N] (channels-major, Ultralytics default)
    // or [1, N, C] (transposed export). We resolve C/N per-output and stash
    // the layout flag so the GPU decode kernel reads the correct stride
    // regardless of export style. EfficientNMS plugin output [1, N, 6] is
    // detected via cols==6 and skipped.
    outputCnLayout.clear();
    outputC.clear();
    outputN.clear();
    for (const auto& outName : outputNames)
    {
        const auto& shape = outputShapes[outName];
        bool needs = false;
        size_t bytes = 0;
        bool cnLayout = true;
        int resolvedC = 0;
        int resolvedN = 0;
        if (shape.size() == 3 && shape[0] == 1 && shape[1] > 0 && shape[2] > 0)
        {
            const int64_t dim1 = shape[1];
            const int64_t dim2 = shape[2];
            // EfficientNMS plugin output is [1, N, 6]; do not transpose it.
            const bool isEfficientNms = (dim2 == 6);
            if (!isEfficientNms)
            {
                // Pick the smaller-valid dim (>4) as C. For standard YOLO
                // exports C is far smaller than N (e.g. 84 vs 8400).
                if (dim1 > 4 && dim1 <= dim2)
                {
                    resolvedC = static_cast<int>(dim1);
                    resolvedN = static_cast<int>(dim2);
                    cnLayout = true;
                    needs = true;
                }
                else if (dim2 > 4 && dim2 < dim1)
                {
                    resolvedC = static_cast<int>(dim2);
                    resolvedN = static_cast<int>(dim1);
                    cnLayout = false;
                    needs = true;
                }
                else if (dim1 >= 5)
                {
                    resolvedC = static_cast<int>(dim1);
                    resolvedN = static_cast<int>(dim2);
                    cnLayout = true;
                    needs = true;
                }
                if (needs)
                {
                    // Device buffer layout: [int counter (16B aligned) |
                    // float candidates[kMaxCandidates * 6]]. D2H is one op.
                    bytes = kDecodeBlockBytes;
                }
            }
        }
        outputNeedsTranspose[outName] = needs;
        outputCnLayout[outName] = cnLayout;
        outputC[outName] = resolvedC;
        outputN[outName] = resolvedN;
        if (needs)
        {
            transposedSizes[outName] = bytes;
            void* ptr = nullptr;
            cudaError_t err = cudaMalloc(&ptr, bytes);
            if (err == cudaSuccess)
            {
                transposedDeviceBuffers[outName] = ptr;
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate transposed buffer for "
                          << outName << ": " << cudaGetErrorString(err) << std::endl;
                outputNeedsTranspose[outName] = false;
                transposedSizes.erase(outName);
            }
        }
    }

    // Double-buffer and CUDA Graph are mutually exclusive in this pass: graph
    // capture assumes a single fixed binding set per stream submit, while
    // double-buffer swaps pinned destinations. Prefer double-buffer if both
    // are requested 鈥?it's the bigger throughput win.
    numSlots = (config.use_double_buffer ? 2 : 1);
    if (numSlots > 1 && config.use_cuda_graph)
    {
        std::cout << "[Detector] use_double_buffer=true overrides use_cuda_graph" << std::endl;
    }

    for (int s = 0; s < 2; ++s)
    {
        if (slotDoneEvent[s]) { cudaEventDestroy(slotDoneEvent[s]); slotDoneEvent[s] = nullptr; }
    }
    for (int s = 0; s < numSlots; ++s)
    {
        cudaEventCreateWithFlags(&slotDoneEvent[s], cudaEventDisableTiming);
    }

    allocatePinnedOutputs();

    img_scale = static_cast<float>(config.detection_resolution) / w;

    for (const auto& n : inputNames)
        context->setTensorAddress(n.c_str(), inputBindings[n]);
    for (const auto& n : outputNames)
        context->setTensorAddress(n.c_str(), outputBindings[n]);

    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);

    preprocessStartEvent = nullptr;
    inferenceStartEvent = nullptr;
    inferenceCompleteEvent = nullptr;
    copyCompleteEvent = nullptr;

    cudaEventCreate(&preprocessStartEvent);
    cudaEventCreate(&inferenceStartEvent);
    cudaEventCreate(&inferenceCompleteEvent);
    cudaEventCreate(&copyCompleteEvent);

    useCudaGraph = (numSlots == 1) && config.use_cuda_graph;
    if (useCudaGraph)
    {
        captureCudaGraph();
    }

    if (config.verbose)
    {
        std::cout << "[Detector] Initialized. ModelStatic=" << std::boolalpha << isStatic
            << ", NetInput=" << h << "x" << w << " (scale=" << img_scale << ")" << std::endl;
    }

    return true;
}

void TrtDetector::requestExit()
{
    {
        std::lock_guard<std::mutex> lock(inferenceMutex);
        shouldExit = true;
        frameReady = false;
        pendingFrameType = PendingFrameType::None;
        currentFrame.release();
        currentFrameGpu.release();
    }
    inferenceCV.notify_all();
}

size_t TrtDetector::getSizeByDim(const nvinfer1::Dims& dims)
{
    size_t size = 1;
    for (int i = 0; i < dims.nbDims; ++i)
    {
        if (dims.d[i] < 0) return 0;
        size *= dims.d[i];
    }
    return size;
}

size_t TrtDetector::getElementSize(nvinfer1::DataType dtype)
{
    switch (dtype)
    {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kINT8: return 1;
    default: return 0;
    }
}

void TrtDetector::loadEngine(const std::string& modelFile)
{
    namespace fs = std::filesystem;

    const fs::path modelPath(fs::u8path(modelFile));
    const std::string extension = modelPath.extension().u8string();

    // Centralized cache for compiled TensorRT engines so generated artifacts
    // don't pollute the user's models folder next to their ONNX sources.
    const fs::path engineCacheDir = fs::path("models") / "engines";
    std::error_code ec;
    fs::create_directories(engineCacheDir, ec);
    if (ec)
    {
        std::cerr << "[Detector] Failed to create engine cache dir '"
                  << engineCacheDir.u8string() << "': " << ec.message() << std::endl;
    }

    fs::path engineFilePath;

    if (extension == ".engine")
    {
        engineFilePath = modelPath;
    }
    else if (extension == ".oliver")
    {
        if (!fs::exists(modelPath, ec) || !fs::is_regular_file(modelPath, ec))
        {
            std::cerr << "[Detector] oliver 模型文件不存在: " << modelPath.u8string() << std::endl;
            return;
        }

        const std::string cacheStem = makeAsciiEngineStem(modelPath) + "_" + hex64(fnv1a64(modelPath.u8string()));
        const fs::path encryptedEngineCache = engineCacheDir / (cacheStem + ".engine.olivercache");

        if (fileExists(encryptedEngineCache.u8string()))
        {
            std::cout << "[Detector] Loading encrypted TensorRT engine cache: "
                      << encryptedEngineCache.u8string() << std::endl;

            oliver::Payload cachePayload;
            std::string error;
            bool decrypted = oliver::decrypt_file(encryptedEngineCache.u8string(), cachePayload, error)
                && cachePayload.type == oliver::PayloadType::TensorRtEngine;
            bool acceptCache = false;
            if (decrypted)
            {
                // Strict FP16 policy: only accept the cache if BOTH input and
                // every output is kHALF. An older cache built before this
                // policy may decrypt fine but ship FP32 outputs — that fails
                // our kernel's __half assumption silently (the user just
                // sees zero detections). Detect, drop, and rebuild.
                if (engineBytesAreFullyHalf(cachePayload.bytes.data(), cachePayload.bytes.size()))
                {
                    engine.reset(loadEngineFromMemory(cachePayload.bytes.data(), cachePayload.bytes.size(), runtime.get()));
                    if (engine)
                    {
                        acceptCache = true;
                        return;
                    }
                }
                else
                {
                    std::cerr << "[Detector] Encrypted engine cache is not fully FP16; rebuilding from oliver." << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Encrypted engine cache decrypt failed: " << error << std::endl;
            }

            (void)acceptCache;
            fs::remove(encryptedEngineCache, ec);
            if (ec)
            {
                std::cerr << "[Detector] Failed to delete invalid encrypted engine cache: "
                          << ec.message() << std::endl;
            }
        }

        oliver::Payload modelPayload;
        std::string error;
        std::string sourceModelId;
        oliver::read_model_id_from_file(modelPath.u8string(), sourceModelId, error);
        error.clear();
        if (!oliver::decrypt_file(modelPath.u8string(), modelPayload, error))
        {
            std::cerr << "[Detector] oliver 模型解密失败: " << error << std::endl;
            return;
        }
        if (modelPayload.type != oliver::PayloadType::Onnx)
        {
            std::cerr << "[Detector] oliver 文件不是 ONNX 模型，无法从 TRT 构建。" << std::endl;
            return;
        }

        std::cout << "[Detector] Building engine from encrypted ONNX model -> "
                  << encryptedEngineCache.u8string() << std::endl;
        auto serializedEngine = buildSerializedEngineFromOnnxMemory(modelPayload.bytes.data(), modelPayload.bytes.size(), gLogger);
        if (!serializedEngine)
            return;

        engine.reset(loadEngineFromMemory(serializedEngine->data(), serializedEngine->size(), runtime.get()));
        if (!engine)
            return;

        std::vector<uint8_t> engineBytes(static_cast<size_t>(serializedEngine->size()));
        std::memcpy(engineBytes.data(), serializedEngine->data(), engineBytes.size());
        std::vector<uint8_t> encrypted;
        if (sourceModelId.empty())
            sourceModelId = modelPayload.model_id;
        if (oliver::encrypt_bytes(engineBytes, oliver::PayloadType::TensorRtEngine, sourceModelId, encrypted, error) &&
            oliver::write_file_bytes(encryptedEngineCache.u8string(), encrypted, error))
        {
            std::cout << "[Detector] Encrypted engine cache saved to: "
                      << encryptedEngineCache.u8string() << std::endl;
        }
        else
        {
            std::cerr << "[Detector] Failed to save encrypted engine cache: " << error << std::endl;
        }
        return;
    }
    else if (extension == ".onnx")
    {
        if (!fs::exists(modelPath, ec) || !fs::is_regular_file(modelPath, ec))
        {
            std::cerr << "[Detector] ONNX model file not found: " << modelPath.u8string() << std::endl;
            return;
        }

        engineFilePath = engineCacheDir / (makeAsciiEngineStem(modelPath) + ".engine");

        // Backwards-compat: pick up any legacy engine that sits next to the
        // .onnx file (pre-cache-dir behavior) so users don't have to rebuild.
        const fs::path legacyEnginePath = fs::path(modelPath).replace_extension(".engine");
        if (!fileExists(engineFilePath.u8string()) && fileExists(legacyEnginePath.u8string()))
        {
            engineFilePath = legacyEnginePath;
        }

        if (fileExists(engineFilePath.u8string()) && !engineIsFullyHalf(engineFilePath))
        {
            std::cerr << "[Detector] Cached engine has non-FP16 IO tensor(s); deleting and rebuilding: "
                      << engineFilePath.u8string() << std::endl;
            fs::remove(engineFilePath, ec);
            if (ec)
            {
                std::cerr << "[Detector] Failed to delete incompatible cached engine: "
                          << ec.message() << std::endl;
            }
        }

        if (!fileExists(engineFilePath.u8string()))
        {
            std::cout << "[Detector] Building engine from ONNX model -> "
                      << engineFilePath.u8string() << std::endl;

            std::filesystem::path temporaryParserPath;
            const std::string parserModelFile = makeTensorRtParserPath(modelPath, engineCacheDir, &temporaryParserPath);
            std::unique_ptr<nvinfer1::ICudaEngine> builtEngine(
                buildEngineFromOnnx(parserModelFile, gLogger));
            if (builtEngine)
            {
                std::unique_ptr<nvinfer1::IHostMemory> serializedEngine(
                    builtEngine->serialize());
                if (serializedEngine)
                {
                    std::ofstream engineFile(engineFilePath, std::ios::binary);
                    if (engineFile)
                    {
                        engineFile.write(
                            reinterpret_cast<const char*>(serializedEngine->data()),
                            serializedEngine->size());
                        engineFile.close();

                        std::cout << "[Detector] Engine saved to: "
                                  << engineFilePath.u8string() << std::endl;
                    }
                    else
                    {
                        std::cerr << "[Detector] Could not open engine file for write: "
                                  << engineFilePath.u8string() << std::endl;
                    }
                }
            }

            if (!temporaryParserPath.empty())
            {
                fs::remove(temporaryParserPath, ec);
            }
        }
    }
    else
    {
        std::cerr << "[Detector] Unsupported model format: " << extension << std::endl;
        return;
    }

    std::cout << "[Detector] Loading engine: " << engineFilePath.u8string() << std::endl;
    engine.reset(loadEngineFromFile(engineFilePath.u8string(), runtime.get()));
}

void TrtDetector::processFrame(const cv::Mat& frame)
{
    if (config.backend == "DML") return;

    std::unique_lock<std::mutex> lock(inferenceMutex);
    currentFrame = frame;
    currentFrameGpu.release();
    pendingFrameType = PendingFrameType::Cpu;
    frameReady = true;
    inferenceCV.notify_one();
}

void TrtDetector::processFrameGpu(GpuImage frame)
{
    if (config.backend == "DML") return;

    std::unique_lock<std::mutex> lock(inferenceMutex);
    currentFrame.release();
    currentFrameGpu = std::move(frame);
    pendingFrameType = PendingFrameType::Gpu;
    frameReady = true;
    inferenceCV.notify_one();
}

void TrtDetector::inferenceThread()
{
    // Double-buffer pipeline state. When numSlots==1 prev_slot stays -1 and
    // post-processing runs inline on the just-submitted slot (legacy flow).
    int curr_slot = 0;
    int prev_slot = -1;

    while (!shouldExit)
    {
        if (detector_model_changed.load())
        {
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                destroyCudaGraph();
                context.reset();
                engine.reset();

                freePinnedOutputs();
                freeTransposedBuffers();

                for (auto& binding : inputBindings)
                    if (binding.second) cudaFree(binding.second);
                inputBindings.clear();
                for (auto& binding : outputBindings)
                    if (binding.second) cudaFree(binding.second);
                outputBindings.clear();

                currentFrame.release();
                currentFrameGpu.release();
                frameReady = false;
                pendingFrameType = PendingFrameType::None;
            }
            initialize("models/" + config.ai_model);
            publishTrtModelMetadata(*this);
            detection_resolution_changed.store(true);
            detector_model_changed.store(false);
            curr_slot = 0;
            prev_slot = -1;
        }

        if (useCudaGraph != config.use_cuda_graph)
        {
            useCudaGraph = config.use_cuda_graph;
            if (!useCudaGraph)
            {
                destroyCudaGraph();
            }
            else if (context)
            {
                captureCudaGraph();
            }
        }

        cv::Mat frame;
        GpuImage frameGpu;
        PendingFrameType frameType = PendingFrameType::None;
        bool hasNewFrame = false;

        {
            std::unique_lock<std::mutex> lock(inferenceMutex);
            if (!frameReady && !shouldExit)
                inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

            if (shouldExit) break;

            if (frameReady)
            {
                frameType = pendingFrameType;
                if (frameType == PendingFrameType::Gpu)
                {
                    frameGpu = std::move(currentFrameGpu);
                    currentFrameGpu.release();
                    currentFrame.release();
                }
                else
                {
                    frame = std::move(currentFrame);
                    currentFrameGpu.release();
                }
                pendingFrameType = PendingFrameType::None;
                frameReady = false;
                hasNewFrame = true;
            }
        }

        if (!context)
        {
            if (!error_logged)
            {
                std::cerr << "[Detector] Context not initialized" << std::endl;
                error_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        else
        {
            error_logged = false;
        }

        if (hasNewFrame)
        {
            const bool hasCpuFrame = (frameType == PendingFrameType::Cpu && !frame.empty());
            const bool hasGpuFrame = (frameType == PendingFrameType::Gpu && !frameGpu.empty());
            if (!hasCpuFrame && !hasGpuFrame)
                continue;

            try
            {
                cudaEventRecord(preprocessStartEvent, stream);
                if (hasGpuFrame)
                    preProcess(frameGpu);
                else
                    preProcess(frame);
                cudaEventRecord(inferenceStartEvent, stream);
                bool usedGraph = useCudaGraph && cudaGraphCaptured;
                if (usedGraph)
                {
                    launchCudaGraph();
                    cudaEventRecord(copyCompleteEvent, stream);
                    cudaEventSynchronize(copyCompleteEvent);
                }
                else
                {
                    context->enqueueV3(stream);
                    cudaEventRecord(inferenceCompleteEvent, stream);

                    auto& curPinned = pinnedSlot(curr_slot);
                    for (const auto& name : outputNames)
                    {
                        auto itPinned = curPinned.find(name);
                        if (itPinned == curPinned.end() || !itPinned->second)
                            continue;

                        const bool needsT = outputNeedsTranspose.count(name) && outputNeedsTranspose[name];
                        if (needsT)
                        {
                            const int C = outputC[name];
                            const int N = outputN[name];
                            const bool cnLayout = outputCnLayout[name];
                            const bool isHalf = (outputTypes[name] == nvinfer1::DataType::kHALF);
                            unsigned char* devBlock = reinterpret_cast<unsigned char*>(transposedDeviceBuffers[name]);
                            int* devCounter = reinterpret_cast<int*>(devBlock);
                            float* devCandidates = reinterpret_cast<float*>(devBlock + kDecodeHeaderBytes);

                            launch_decode_and_filter(
                                outputBindings[name], C, N, numClasses, isHalf,
                                config.confidence_threshold, img_scale,
                                kMaxCandidates, cnLayout, devCounter, devCandidates, stream
                            );

                            cudaMemcpyAsync(
                                itPinned->second, devBlock, transposedSizes[name],
                                cudaMemcpyDeviceToHost, stream
                            );
                        }
                        else
                        {
                            cudaMemcpyAsync(
                                itPinned->second, outputBindings[name],
                                outputSizes[name], cudaMemcpyDeviceToHost, stream
                            );
                        }
                    }

                    cudaEventRecord(copyCompleteEvent, stream);
                    cudaEventRecord(slotDoneEvent[curr_slot], stream);

                    if (numSlots == 1)
                    {
                        // Single-slot: block here as before.
                        cudaEventSynchronize(copyCompleteEvent);
                    }
                }

                // Decide which slot to post-process this iteration.
                //   numSlots==1  -> use curr_slot (inline, legacy behavior).
                //   numSlots==2  -> use prev_slot; first frame has no prev, so
                //                   skip post and just advance.
                const int post_slot = (numSlots == 1) ? curr_slot : prev_slot;
                const bool do_post = (post_slot >= 0);

                auto t_post_start = std::chrono::steady_clock::now();

                if (do_post)
                {
                    if (numSlots > 1)
                    {
                        // Wait for this slot's GPU work chain to complete.
                        cudaEventSynchronize(slotDoneEvent[post_slot]);
                    }

                    auto& postPinned = pinnedSlot(post_slot);
                    for (const auto& name : outputNames)
                    {
                        const auto itPinned = postPinned.find(name);
                        if (itPinned == postPinned.end() || !itPinned->second)
                            continue;

                        const bool needsT = outputNeedsTranspose.count(name) && outputNeedsTranspose[name];

                        if (needsT)
                        {
                            // Pinned layout: [int counter (16B aligned) |
                            // float candidates[K*6]]. GPU already decoded +
                            // conf-filtered; all CPU has to do is NMS on the
                            // small kept set via the existing cols==6 path.
                            auto* block = reinterpret_cast<unsigned char*>(itPinned->second);
                            const int kept_raw = *reinterpret_cast<const int*>(block);
                            const int kept = std::min(kept_raw, kMaxCandidates);
                            const float* cands = reinterpret_cast<const float*>(block + kDecodeHeaderBytes);

                            std::vector<int64_t> shape{ 1, kept, 6 };
                            std::vector<Detection> detections = postProcessYolo(
                                cands, shape, numClasses,
                                config.confidence_threshold,
                                config.nms_threshold,
                                &lastNmsTimeValue
                            );
                            filterDetectionsByDepthMask(detections);

                            {
                                std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
                                detectionBuffer.boxes.clear();
                                detectionBuffer.classes.clear();
                                for (const auto& det : detections)
                                {
                                    detectionBuffer.boxes.push_back(det.box);
                                    detectionBuffer.classes.push_back(det.classId);
                                }
                                detectionBuffer.version++;
                                detectionBuffer.cv.notify_all();
                            }
                            continue;
                        }

                        nvinfer1::DataType dtype = outputTypes[name];
                        if (dtype == nvinfer1::DataType::kHALF)
                        {
                            const size_t numElements = outputSizes[name] / sizeof(__half);
                            const __half* halfPtr = reinterpret_cast<const __half*>(itPinned->second);

                            auto& outputDataFloat = fp16OutputScratch[name];
                            if (outputDataFloat.size() != numElements)
                                outputDataFloat.resize(numElements);

                            for (size_t i = 0; i < numElements; ++i)
                                outputDataFloat[i] = __half2float(halfPtr[i]);

                            postProcess(outputDataFloat.data(), name, &lastNmsTimeValue);
                        }
                        else if (dtype == nvinfer1::DataType::kFLOAT)
                        {
                            const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
                            postProcess(floatPtr, name, &lastNmsTimeValue);
                        }
                    }
                }

                auto t_post_end = std::chrono::steady_clock::now();

                if (numSlots > 1)
                {
                    prev_slot = curr_slot;
                    curr_slot = (curr_slot + 1) % numSlots;
                }

                float preprocessMs = 0.0f;
                float inferenceMs = 0.0f;
                float copyMs = 0.0f;

                cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
                cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
                cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

                lastPreprocessTimeValue = std::chrono::duration<double, std::milli>(preprocessMs);
                lastInferenceTimeValue = std::chrono::duration<double, std::milli>(inferenceMs);
                lastCopyTimeValue = std::chrono::duration<double, std::milli>(copyMs);
                lastPostprocessTimeValue = t_post_end - t_post_start;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Detector] Error during inference: " << e.what() << std::endl;
            }
        }
    }
}

void TrtDetector::preProcess(const cv::Mat& frame)
{
    if (frame.empty())
        return;

    if (!gpuFrameBuffer.upload(frame.data, frame.rows, frame.cols, frame.channels(),
                               frame.step, stream))
        return;
    preProcess(gpuFrameBuffer);
}

void TrtDetector::preProcess(const GpuImage& frame)
{
    if (frame.empty())
        return;

    void* inputBuffer = inputBindings[inputName];
    if (!inputBuffer)
        return;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return;
    }

    if (c != 3)
        return;

    const int srcChannels = frame.channels();
    if (srcChannels != 1 && srcChannels != 3 && srcChannels != 4)
        return;

    // Square input invariant is enforced in initialize(); w == h here.
    // One-shot fused kernel: bilinear resize -> BGR(A)->RGB (or GRAY broadcast)
    // -> /255 -> half CHW directly into the engine input binding. No OpenCV
    // CUDA module needed — all work runs on hand-written kernels.
    launch_resize_bgr_u8_to_chw_rgb_f16(
        frame.view(), reinterpret_cast<__half*>(inputBuffer), w, stream
    );

    if (config.verbose)
    {
        auto err = cudaGetLastError();
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] preprocess kernel launch error: " << cudaGetErrorString(err) << std::endl;
        }
    }
}

void TrtDetector::postProcess(const float* output, const std::string& outputName, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (numClasses <= 0) return;

    const auto shapeIt = outputShapes.find(outputName);
    if (shapeIt == outputShapes.end())
        return;

    std::vector<Detection> detections;

    // If this output was GPU-transposed from [1, C, N] to [N, C], tell the
    // decoder about the new row-major layout by swapping the inner dims.
    std::vector<int64_t> shape = shapeIt->second;
    auto itT = outputNeedsTranspose.find(outputName);
    if (itT != outputNeedsTranspose.end() && itT->second && shape.size() == 3)
        std::swap(shape[1], shape[2]);

    detections = postProcessYolo(
        output,
        shape,
        numClasses,
        config.confidence_threshold,
        config.nms_threshold,
        nmsTime
    );
    filterDetectionsByDepthMask(detections);

    {
        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        detectionBuffer.boxes.clear();
        detectionBuffer.classes.clear();

        for (const auto& det : detections)
        {
            detectionBuffer.boxes.push_back(det.box);
            detectionBuffer.classes.push_back(det.classId);
        }

        detectionBuffer.version++;
        detectionBuffer.cv.notify_all();
    }
}








