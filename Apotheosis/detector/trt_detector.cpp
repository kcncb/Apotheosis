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
#include <cctype>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <mutex>
#include <sstream>
#include <cstring>

#include "trt_detector.h"
#include "runtime/config_snapshot.h"
#include "nvinf.h"
#include "Apotheosis.h"
#include "other_tools.h"
#include "postProcess.h"
#include "model_inspector.h"
#include "model_crypto/model_crypto.h"
#include "cuda_preprocess.h"
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
} // namespace

TrtDetector::TrtDetector()
    : frameReady(false),
    shouldExit(false),
    useCudaGraph(false),
    cudaGraphCaptured(false),
    inputBufferDevice(nullptr),
    img_scale(1.0f),
    numClasses(0)
{
    stream = nullptr;
    // Run inference on a high-priority stream so the GPU scheduler favors it
    // over the capture-side nvJPEG decode stream, which runs continuously at
    // the full capture rate (~240fps of 1080p MJPEG) and otherwise steals SMs
    // mid-inference, inflating and destabilizing inference wall-time. Falls
    // back to a default-priority stream if stream priorities are unsupported.
    {
        // cudaStreamNonBlocking (NOT cudaStreamDefault): a default-flag stream
        // implicitly synchronizes with the legacy NULL stream, so any NULL-
        // stream op elsewhere (e.g. the capture thread's synchronous D2H
        // download for the preview window) would serialize against inference.
        // The capture/decode streams are already non-blocking; this makes the
        // inference stream consistent so it truly runs concurrently.
        int priLow = 0, priHigh = 0;
        if (cudaDeviceGetStreamPriorityRange(&priLow, &priHigh) == cudaSuccess &&
            priHigh != priLow &&
            cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, priHigh) == cudaSuccess)
        {
            // high-priority non-blocking stream created
        }
        else
        {
            cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
        }
    }
    // cudaGraphs / cudaGraphExecs are std::array<...,2>, default-init to
    // {nullptr,nullptr} via the in-class initializer in trt_detector.h.
}

TrtDetector::~TrtDetector()
{
    destroyCudaGraph();
    freePinnedOutputs();
    freeTransposedBuffers();

    for (auto& binding : inputBindings) if (binding.second) cudaFree(binding.second);
    for (auto& binding : outputBindings) if (binding.second) cudaFree(binding.second);
    if (inputBufferDevice) cudaFree(inputBufferDevice);
    for (int s = 0; s < 2; ++s)
    {
        if (preprocessStartEvent[s]) cudaEventDestroy(preprocessStartEvent[s]);
        if (inferenceStartEvent[s]) cudaEventDestroy(inferenceStartEvent[s]);
        if (inferenceCompleteEvent[s]) cudaEventDestroy(inferenceCompleteEvent[s]);
        if (copyCompleteEvent[s]) cudaEventDestroy(copyCompleteEvent[s]);
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

        if (runtime_config::read()->verbose)
        {
            std::cout << "[Detector] Allocated " << numSlots << " pinned host buffer(s) for output "
                << name << ": " << bytes << " bytes each" << std::endl;
        }
    }
}

void TrtDetector::destroyCudaGraph()
{
    for (int s = 0; s < 2; ++s)
    {
        if (cudaGraphExecs[s])
        {
            cudaGraphExecDestroy(cudaGraphExecs[s]);
            cudaGraphExecs[s] = nullptr;
        }
        if (cudaGraphs[s])
        {
            cudaGraphDestroy(cudaGraphs[s]);
            cudaGraphs[s] = nullptr;
        }
    }
    cudaGraphCaptured = false;
    graphInputRows = 0;
    graphInputCols = 0;
    graphInputChannels = 0;
    graphInputStep = 0;
}

bool TrtDetector::ensureGraphStaging(int rows, int cols, int channels)
{
    // Returns true when the caller should (re)capture the graph: either no
    // graph exists yet, or the staging shape needs to grow/shrink to match a
    // new frame.
    const bool shapeChanged =
        (rows != graphInputRows) ||
        (cols != graphInputCols) ||
        (channels != graphInputChannels);

    bool needsCapture = !cudaGraphCaptured || shapeChanged;

    for (int s = 0; s < numSlots; ++s)
    {
        auto& staging = graphInputBuffers[s];
        if (staging.empty() || staging.rows() != rows
            || staging.cols() != cols || staging.channels() != channels)
        {
            if (!staging.create(rows, cols, channels))
            {
                std::cerr << "[Detector] Failed to allocate graph staging buffer ("
                          << rows << "x" << cols << "x" << channels << ")" << std::endl;
                return false;
            }
            needsCapture = true;
        }
    }

    if (shapeChanged && cudaGraphCaptured)
    {
        // Pinned dst pointers stay the same across recapture; only the
        // preprocess kernel's source view (rows/cols/channels/step) changed.
        destroyCudaGraph();
    }

    graphInputRows = rows;
    graphInputCols = cols;
    graphInputChannels = channels;
    graphInputStep = graphInputBuffers[0].step();

    return needsCapture;
}

bool TrtDetector::captureCudaGraph(int slot)
{
    if (!useCudaGraph) return false;
    if (slot < 0 || slot >= numSlots) return false;
    if (graphInputBuffers[slot].empty()) return false;

    if (cudaGraphExecs[slot])
    {
        cudaGraphExecDestroy(cudaGraphExecs[slot]);
        cudaGraphExecs[slot] = nullptr;
    }
    if (cudaGraphs[slot])
    {
        cudaGraphDestroy(cudaGraphs[slot]);
        cudaGraphs[slot] = nullptr;
    }

    // Warm up the exact preprocess+enqueue chain ONCE outside capture before
    // recording it. TensorRT performs lazy per-context setup on its first
    // enqueueV3 — Cask convolution kernel selection plus internal scratch
    // allocation — and those operations are illegal while a stream is
    // capturing. Without this warmup the capture aborts with
    // "Cask ... Error Code 1" + cudaErrorStreamCaptureUnsupported(229) at
    // EndCapture. After one warm run the captured pass only replays
    // already-initialized work. The warmup reads this slot's staging buffer
    // (garbage on the first frame is fine — the result is discarded).
    {
        void* warmInput = inputBindings[inputName];
        if (warmInput)
        {
            nvinfer1::Dims wd = context->getTensorShape(inputName.c_str());
            int warmH = 0;
            int warmW = 0;
            if (wd.nbDims >= 4
                && tryGetPositiveDimInt(wd.d[2], &warmH)
                && tryGetPositiveDimInt(wd.d[3], &warmW))
            {
                launch_resize_bgr_u8_to_chw_rgb_f16(
                    graphInputBuffers[slot].view(),
                    reinterpret_cast<__half*>(warmInput),
                    warmW,
                    stream
                );
            }
        }
        context->enqueueV3(stream);
    }

    cudaStreamSynchronize(stream);

    // ThreadLocal, not Global: other threads run their own GPU work during the
    // capture window — most importantly the capture-card nvJPEG decode worker,
    // which issues kernels and a cudaStreamSynchronize every frame on its own
    // stream. Under cudaStreamCaptureModeGlobal any such cross-thread GPU
    // operation invalidates this capture (also surfaces as 229). ThreadLocal
    // scopes capture safety checks to the calling thread only.
    cudaError_t st = cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] BeginCapture(slot=" << slot << ") failed: "
            << cudaGetErrorString(st) << std::endl;
        return false;
    }

    // 1) Fused preprocess: read from this slot's staging buffer (fixed device
    // address), write half CHW directly into the engine's input binding.
    void* inputBuffer = inputBindings[inputName];
    if (inputBuffer)
    {
        nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
        int sideH = 0;
        int sideW = 0;
        if (dims.nbDims >= 4
            && tryGetPositiveDimInt(dims.d[2], &sideH)
            && tryGetPositiveDimInt(dims.d[3], &sideW))
        {
            launch_resize_bgr_u8_to_chw_rgb_f16(
                graphInputBuffers[slot].view(),
                reinterpret_cast<__half*>(inputBuffer),
                sideW,
                stream
            );
        }
    }

    // 2) TRT enqueue.
    context->enqueueV3(stream);

    // 3) Decode + filter + D2H to *this slot's* pinned buffers.
    auto& pinned = pinnedSlot(slot);
    for (const auto& name : outputNames)
    {
        const auto itPinned = pinned.find(name);
        if (itPinned == pinned.end() || !itPinned->second) continue;

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

            const SmallTargetDecode st = computeSmallTargetDecode();
            launch_decode_and_filter(
                outputBindings[name], C, N, numClasses, isHalf,
                runtime_config::read()->confidence_threshold, st.smallConf,
                static_cast<float>(st.areaThreshPx), img_scale,
                kMaxCandidates, cnLayout, devCounter, devCandidates, stream
            );

            cudaMemcpyAsync(itPinned->second, devBlock,
                transposedSizes[name], cudaMemcpyDeviceToHost, stream);
        }
        else
        {
            cudaMemcpyAsync(itPinned->second,
                outputBindings[name],
                outputSizes[name],
                cudaMemcpyDeviceToHost,
                stream);
        }
    }

    st = cudaStreamEndCapture(stream, &cudaGraphs[slot]);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] EndCapture(slot=" << slot << ") failed: "
            << cudaGetErrorString(st) << std::endl;
        return false;
    }

    st = cudaGraphInstantiate(&cudaGraphExecs[slot], cudaGraphs[slot], 0);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] GraphInstantiate(slot=" << slot << ") failed: "
            << cudaGetErrorString(st) << std::endl;
        cudaGraphDestroy(cudaGraphs[slot]);
        cudaGraphs[slot] = nullptr;
        return false;
    }

    // Mark as fully captured only once every active slot is done.
    cudaGraphCaptured = true;
    for (int s = 0; s < numSlots; ++s)
    {
        if (!cudaGraphExecs[s]) { cudaGraphCaptured = false; break; }
    }
    return true;
}

inline void TrtDetector::launchCudaGraph(int slot)
{
    if (slot < 0 || slot >= numSlots) return;
    if (!cudaGraphExecs[slot]) return;
    auto err = cudaGraphLaunch(cudaGraphExecs[slot], stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] GraphLaunch(slot=" << slot << ") failed: "
            << cudaGetErrorString(err) << std::endl;
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
            if (runtime_config::read()->verbose)
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

            if (runtime_config::read()->verbose)
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
                if (runtime_config::read()->verbose)
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
                if (runtime_config::read()->verbose)
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
        std::string resolved_path = model_path;
        std::filesystem::path maybe(std::filesystem::u8path(model_path));
        if (!maybe.is_absolute())
        {
            std::error_code ec;
            auto abs = std::filesystem::absolute(maybe, ec);
            if (!ec) resolved_path = abs.u8string();
        }

        // ONNX-source models carry their class names in the ONNX "names"
        // custom metadata. An engine we build ourselves from .onnx has NO
        // Ultralytics JSON header, so read_ultralytics_engine_header() returns
        // nothing and numClasses falls back to (channels - 4). That fallback
        // is wrong for end2end / NMS outputs shaped [1, N, 6], where
        // channels-4 == 2 regardless of the real class count — the reason a
        // freshly built YOLOv10-style engine only ever produced classes 0 and
        // 1. Read the ONNX metadata here so TRT matches the DML path.
        std::string ext = std::filesystem::u8path(model_path).extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".onnx" || ext == ".oliver")
        {
            detector::ModelMetadata md = detector::inspect_onnx_model(model_path, runtime_config::read()->verbose);
            if (!md.class_names.empty())
                class_names_ = std::move(md.class_names);
        }

        // Real .engine files exported by Ultralytics carry a 4-byte length +
        // JSON metadata header; fall back to it for engine-source models.
        if (class_names_.empty())
        {
            auto blob = detector::read_ultralytics_engine_header(resolved_path);
            if (!blob.empty())
                class_names_ = detector::parse_json_names(blob);
        }

        // Last resort: "<stem>.json" / "<stem>.names" sidecar next to the model.
        if (class_names_.empty())
        {
            detector::ClassNamesSource src = detector::ClassNamesSource::None;
            auto sidecar = detector::read_sidecar_class_names(resolved_path, &src);
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

    if (isStatic != runtime_config::read()->fixed_input_size)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            config.fixed_input_size = isStatic;
        }
        runtime_config::publish();
        detector_model_changed.store(true);
        std::cout << "[Detector] Automatically set fixed_input_size = " << (isStatic ? "true" : "false") << std::endl;
    }

    const int target = runtime_config::read()->detection_resolution;
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

        // NMS-decoded engines (EfficientNMS plugin etc.) emit [batch, K, 6]
        // where 6 = x1,y1,x2,y2,score,classId — the channel count tells us
        // nothing about the actual class count of the underlying model. Same
        // is true for any "cols==6" layout: subtracting 4 gives a misleading
        // 2 regardless of how many classes the model was trained on. When the
        // engine ships richer names metadata, trust that instead.
        if (!class_names_.empty() && static_cast<int>(class_names_.size()) > classes)
        {
            classes = static_cast<int>(class_names_.size());
        }
        numClasses = classes;

        if (runtime_config::read()->verbose)
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
    // avoid a silent truncation/extension mismatch. Delete the incompatible
    // cache so the fixed FP16-I/O builder policy can rebuild it.
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

    // CUDA Graph + double_buffer now coexist: we capture one graph per slot,
    // each writing to its own pinned dst. The two are no longer mutually
    // exclusive — pipelining (CPU post-process on prev slot while GPU runs
    // curr slot's graph) stacks on top of the launch-overhead savings from
    // collapsing N kernel launches into one cudaGraphLaunch.
    numSlots = (runtime_config::read()->use_double_buffer ? 2 : 1);

    for (int s = 0; s < 2; ++s)
    {
        if (slotDoneEvent[s]) { cudaEventDestroy(slotDoneEvent[s]); slotDoneEvent[s] = nullptr; }
    }
    for (int s = 0; s < numSlots; ++s)
    {
        cudaEventCreateWithFlags(&slotDoneEvent[s], cudaEventDisableTiming);
    }

    allocatePinnedOutputs();

    img_scale = static_cast<float>(runtime_config::read()->detection_resolution) / w;

    for (const auto& n : inputNames)
        context->setTensorAddress(n.c_str(), inputBindings[n]);
    for (const auto& n : outputNames)
        context->setTensorAddress(n.c_str(), outputBindings[n]);

    for (int s = 0; s < 2; ++s)
    {
        if (preprocessStartEvent[s]) cudaEventDestroy(preprocessStartEvent[s]);
        if (inferenceStartEvent[s]) cudaEventDestroy(inferenceStartEvent[s]);
        if (inferenceCompleteEvent[s]) cudaEventDestroy(inferenceCompleteEvent[s]);
        if (copyCompleteEvent[s]) cudaEventDestroy(copyCompleteEvent[s]);
        preprocessStartEvent[s] = nullptr;
        inferenceStartEvent[s] = nullptr;
        inferenceCompleteEvent[s] = nullptr;
        copyCompleteEvent[s] = nullptr;
        cudaEventCreate(&preprocessStartEvent[s]);
        cudaEventCreate(&inferenceStartEvent[s]);
        cudaEventCreate(&inferenceCompleteEvent[s]);
        cudaEventCreate(&copyCompleteEvent[s]);
    }

    useCudaGraph = runtime_config::read()->use_cuda_graph;
    // Graph capture itself is deferred to the first frame: we need that
    // frame's rows/cols/channels to size the per-slot staging buffer that the
    // captured preprocess kernel will read from.

    if (runtime_config::read()->verbose)
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
    if (runtime_config::read()->backend == "DML") return;

    std::unique_lock<std::mutex> lock(inferenceMutex);
    currentFrame = frame;
    currentFrameGpu.release();
    pendingFrameType = PendingFrameType::Cpu;
    frameReady = true;
    inferenceCV.notify_one();
}

void TrtDetector::processFrameGpu(GpuImage frame)
{
    if (runtime_config::read()->backend == "DML") return;

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

    // Latches true if CUDA graph capture fails this session. Without it a
    // failing capture is retried every frame (each retry costs a full stream
    // sync + begin/end capture), which silently caps throughput. Once set we
    // run the direct enqueue path instead; cleared only on a model reload.
    bool graphCaptureGivenUp = false;

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
            initialize("models/" + runtime_config::read()->ai_model);
            publishTrtModelMetadata(*this);
            detection_resolution_changed.store(true);
            detector_model_changed.store(false);
            curr_slot = 0;
            prev_slot = -1;
            graphCaptureGivenUp = false;
        }

        if (useCudaGraph != runtime_config::read()->use_cuda_graph)
        {
            useCudaGraph = runtime_config::read()->use_cuda_graph;
            if (!useCudaGraph)
            {
                destroyCudaGraph();
            }
            // Re-capture is deferred to the next frame via the
            // ensureGraphStaging path — that lets capture see the actual
            // rows/cols/channels of the next inbound frame so the graph's
            // baked-in preprocess kernel parameters match.
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
                // The capture thread decodes on its own stream and hands us a
                // completion event instead of CPU-syncing. Make our stream wait
                // for it so the D2D copy / preprocess below never reads a buffer
                // whose decode is still in flight. No-op when no event is set.
                if (hasGpuFrame)
                {
                    cudaEvent_t frameReadyEvent = frameGpu.readyEvent();
                    if (frameReadyEvent)
                        cudaStreamWaitEvent(stream, frameReadyEvent, 0);
                }

                cudaEventRecord(preprocessStartEvent[curr_slot], stream);

                // Try the CUDA Graph fast path first when enabled. Capture is
                // lazy on the first frame (or whenever input shape changes) so
                // the baked-in preprocess kernel sees the correct rows/cols/
                // channels. Each slot has its own graph + pinned dst, so this
                // stacks cleanly on top of double_buffer pipelining.
                bool usedGraph = false;
                if (useCudaGraph && !graphCaptureGivenUp)
                {
                    int frameRows = 0;
                    int frameCols = 0;
                    int frameChannels = 0;
                    if (hasGpuFrame)
                    {
                        frameRows = frameGpu.rows();
                        frameCols = frameGpu.cols();
                        frameChannels = frameGpu.channels();
                    }
                    else
                    {
                        frameRows = frame.rows;
                        frameCols = frame.cols;
                        frameChannels = frame.channels();
                    }

                    if (frameRows > 0 && frameCols > 0 && frameChannels > 0)
                    {
                        bool needsCapture = ensureGraphStaging(frameRows, frameCols, frameChannels);
                        if (needsCapture)
                        {
                            bool captureOk = true;
                            for (int s = 0; s < numSlots; ++s)
                            {
                                if (!captureCudaGraph(s)) { captureOk = false; break; }
                            }
                            if (!captureOk)
                            {
                                // Give up for this session instead of retrying
                                // every frame; fall through to direct enqueue.
                                std::cerr << "[Detector] CUDA graph capture failed; "
                                             "falling back to direct enqueue for this session."
                                          << std::endl;
                                destroyCudaGraph();
                                graphCaptureGivenUp = true;
                            }
                        }
                    }

                    if (cudaGraphCaptured)
                    {
                        auto& staging = graphInputBuffers[curr_slot];
                        if (hasGpuFrame)
                        {
                            const size_t widthBytes =
                                static_cast<size_t>(frameCols) * static_cast<size_t>(frameChannels);
                            cudaMemcpy2DAsync(
                                staging.data(), staging.step(),
                                frameGpu.data(), frameGpu.step(),
                                widthBytes, static_cast<size_t>(frameRows),
                                cudaMemcpyDeviceToDevice, stream
                            );
                        }
                        else
                        {
                            staging.upload(frame.data, frameRows, frameCols, frameChannels,
                                           frame.step, stream);
                        }

                        cudaEventRecord(inferenceStartEvent[curr_slot], stream);
                        launchCudaGraph(curr_slot);
                        cudaEventRecord(inferenceCompleteEvent[curr_slot], stream);
                        cudaEventRecord(copyCompleteEvent[curr_slot], stream);
                        cudaEventRecord(slotDoneEvent[curr_slot], stream);
                        usedGraph = true;

                        if (numSlots == 1)
                        {
                            cudaEventSynchronize(copyCompleteEvent[curr_slot]);
                        }
                    }
                }

                if (!usedGraph)
                {
                    if (hasGpuFrame)
                        preProcess(frameGpu);
                    else
                        preProcess(frame);
                    cudaEventRecord(inferenceStartEvent[curr_slot], stream);
                    context->enqueueV3(stream);
                    cudaEventRecord(inferenceCompleteEvent[curr_slot], stream);

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

                            const SmallTargetDecode st = computeSmallTargetDecode();
                            const DetectorRuntimeSettings runtime = detectorRuntimeSettings();
                            launch_decode_and_filter(
                                outputBindings[name], C, N, numClasses, isHalf,
                                runtime.confidenceThreshold, st.smallConf,
                                static_cast<float>(st.areaThreshPx), img_scale,
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

                    cudaEventRecord(copyCompleteEvent[curr_slot], stream);
                    cudaEventRecord(slotDoneEvent[curr_slot], stream);

                    if (numSlots == 1)
                    {
                        // Single-slot: block here as before.
                        cudaEventSynchronize(copyCompleteEvent[curr_slot]);
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
                            const SmallTargetDecode st = computeSmallTargetDecode();
                            const DetectorRuntimeSettings runtime = detectorRuntimeSettings();
                            // GPU 内核已做面积自适应过滤,这里只让候选通过 conf 门槛并跑 NMS,
                            // 不再重复 CPU 面积过滤(传 -1 关闭)。
                            std::vector<Detection> detections = postProcessYolo(
                                cands, shape, numClasses,
                                st.decodeFloor,
                                runtime.nmsThreshold,
                                &lastNmsTimeValue,
                                -1.0f, 0.0
                            );
                            capDetectionsToMax(detections, runtime.maxDetections);

                            {
                                std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
                                detectionBuffer.boxes.clear();
                                detectionBuffer.precise_boxes.clear();
                                detectionBuffer.classes.clear();
                                detectionBuffer.confidences.clear();
                                for (const auto& det : detections)
                                {
                                    detectionBuffer.boxes.push_back(det.box);
                                    detectionBuffer.precise_boxes.push_back(det.preciseBox);
                                    detectionBuffer.classes.push_back(det.classId);
                                    detectionBuffer.confidences.push_back(det.confidence);
                                }
                                detectionBuffer.bumpVersionLocked();
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

                // 读"已完成那帧(post_slot)"的 GPU 计时:单缓冲下 post_slot=本帧且
                // 已 sync;双缓冲下 post_slot=上一帧且已等过其 slotDoneEvent。这样即便
                // CUDA Graph 把工作异步打包,也能拿到真实的推理/拷贝耗时,而不是读到
                // 尚未完成的 event(值恒为 0)。post_slot<0 仅出现在双缓冲第一帧。
                if (post_slot >= 0)
                {
                    cudaEventElapsedTime(&preprocessMs, preprocessStartEvent[post_slot], inferenceStartEvent[post_slot]);
                    cudaEventElapsedTime(&inferenceMs, inferenceStartEvent[post_slot], inferenceCompleteEvent[post_slot]);
                    cudaEventElapsedTime(&copyMs, inferenceCompleteEvent[post_slot], copyCompleteEvent[post_slot]);
                }

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

    if (runtime_config::read()->verbose)
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

    const SmallTargetDecode st = computeSmallTargetDecode();
    detections = postProcessYolo(
        output,
        shape,
        numClasses,
        st.decodeFloor,
        detectorRuntimeSettings().nmsThreshold,
        nmsTime,
        st.baseConf, st.areaThreshPx
    );
    capDetectionsToMax(detections, detectorRuntimeSettings().maxDetections);

    {
        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        detectionBuffer.boxes.clear();
        detectionBuffer.precise_boxes.clear();
        detectionBuffer.classes.clear();
        detectionBuffer.confidences.clear();

        for (const auto& det : detections)
        {
            detectionBuffer.boxes.push_back(det.box);
            detectionBuffer.precise_boxes.push_back(det.preciseBox);
            detectionBuffer.classes.push_back(det.classId);
            detectionBuffer.confidences.push_back(det.confidence);
        }

        detectionBuffer.bumpVersionLocked();
        detectionBuffer.cv.notify_all();
    }
}








