#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <iostream>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include "nvinf.h"
#include "Apotheosis.h"
#include "trt_monitor.h"

Logger gLogger;

void Logger::log(nvinfer1::ILogger::Severity severity, const char* msg) noexcept
{
    if (severity <= nvinfer1::ILogger::Severity::kWARNING)
    {
        std::string devMsg = msg;

        std::string magicTag = "Serialization assertion plan->header.magicTag == rt::kPLAN_MAGIC_TAG failed.";
        std::string old_deserialization = "Using old deserialization call on a weight-separated plan file.";
        if (devMsg.find(magicTag) != std::string::npos || devMsg.find(old_deserialization) != std::string::npos)
        {
            std::cout << "[TensorRT] ERROR: This engine model is not suitable for execution. Please delete this engine model and set the ONNX version of this model in the settings. The program will export the model automatically." << std::endl;
        }
        else
        {
            std::cout << "[TensorRT] " << severityLevelName(severity) << ": " << msg << std::endl;
        }
    }
}

const char* Logger::severityLevelName(nvinfer1::ILogger::Severity severity)
{
    switch (severity)
    {
        case nvinfer1::ILogger::Severity::kINTERNAL_ERROR: return "INTERNAL_ERROR";
        case nvinfer1::ILogger::Severity::kERROR:          return "ERROR";
        case nvinfer1::ILogger::Severity::kWARNING:        return "WARNING";
        case nvinfer1::ILogger::Severity::kINFO:           return "INFO";
        case nvinfer1::ILogger::Severity::kVERBOSE:        return "VERBOSE";
        default:                                           return "UNKNOWN";
    }
}

nvinfer1::IBuilder* createInferBuilder()
{
    return nvinfer1::createInferBuilder(gLogger);
}

nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder)
{
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    return builder->createNetworkV2(explicitBatch);
}

nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder)
{
    return builder->createBuilderConfig();
}

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime)
{
    std::ifstream file(std::filesystem::u8path(engineFile), std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] Error opening the engine file: " << engineFile << std::endl;
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> engineData(size);
    file.read(engineData.data(), size);
    file.close();

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(engineData.data(), size);
    if (!engine)
    {
        std::cerr << "[TensorRT] Engine deserialization error from file: " << engineFile << std::endl;
        return nullptr;
    }

    if (config.verbose)
    {
        std::cout << "[TensorRT] The engine was successfully loaded from the file: " << engineFile << std::endl;
    }
    return engine;
}

nvinfer1::ICudaEngine* loadEngineFromMemory(const void* data, size_t size, nvinfer1::IRuntime* runtime)
{
    if (!data || size == 0 || !runtime)
    {
        std::cerr << "[TensorRT] Invalid engine memory buffer" << std::endl;
        return nullptr;
    }

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(data, size);
    if (!engine)
    {
        std::cerr << "[TensorRT] Engine deserialization error from memory" << std::endl;
        return nullptr;
    }
    return engine;
}

namespace
{
std::unique_ptr<nvinfer1::IHostMemory> buildSerializedEngine(nvinfer1::INetworkDefinition* network,
                                                            nvinfer1::IBuilder* builder,
                                                            nvinfer1::IBuilderConfig* cfg)
{
    nvinfer1::ITensor* inputTensor = network->getInput(0);
    if (!inputTensor)
    {
        std::cerr << "[TensorRT] ERROR: ONNX model has no input tensor" << std::endl;
        return nullptr;
    }
    const char* inName = inputTensor->getName();

    // FP16 IO contract: pin every network input AND output tensor to kHALF.
    // The CUDA preprocess kernel writes __half directly into the input
    // binding, and the GPU decode kernel reads outputs through a __half
    // template specialization — without these pins TRT is free to keep
    // input/output at FP32 (which is what was happening before: an engine
    // built with kFP16 still shipped an FP32 output tensor, so the kernel
    // dispatched its float branch and read the wrong stride).
    //
    // We deliberately do NOT force per-layer precision: shape tensors must
    // be integer, Constant layers carry FP32 weights and refuse a kHALF
    // pin, and reductions intentionally stay at higher precision. kFP16
    // flag below already lets TRT pick FP16 for all the heavy compute; we
    // only care that the IO surface matches our kernels.
    for (int i = 0; i < network->getNbInputs(); ++i)
    {
        nvinfer1::ITensor* t = network->getInput(i);
        if (t) t->setType(nvinfer1::DataType::kHALF);
    }
    for (int i = 0; i < network->getNbOutputs(); ++i)
    {
        nvinfer1::ITensor* t = network->getOutput(i);
        if (t) t->setType(nvinfer1::DataType::kHALF);
    }

    nvinfer1::Dims inDims = inputTensor->getDimensions();
    auto dimension_to_int = [](std::int64_t value) -> int {
        if (value < -1 || value > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
            return -1;
        return static_cast<int>(value);
    };
    int H = (inDims.nbDims >= 4) ? dimension_to_int(inDims.d[2]) : -1;
    int W = (inDims.nbDims >= 4) ? dimension_to_int(inDims.d[3]) : -1;

    bool fixedByModel = (H > 0 && W > 0);
    bool fixedByConfig = config.fixed_input_size;
    bool makeStatic = fixedByModel || fixedByConfig;

    if (fixedByConfig && (H <= 0 || W <= 0))
        H = W = config.detection_resolution;

    nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
    if (makeStatic)
    {
        nvinfer1::Dims4 d{ 1, 3, H, W };
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMIN, d);
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kOPT, d);
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMAX, d);
        if (config.verbose)
            std::cout << "[TensorRT] Static profile " << H << "x" << W << std::endl;
    }
    else
    {
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4{ 1, 3, 160, 160 });
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4{ 1, 3, 320, 320 });
        profile->setDimensions(inName, nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4{ 1, 3, 640, 640 });
        if (config.verbose)
            std::cout << "[TensorRT] Dynamic profile 160/320/640" << std::endl;
    }

    cfg->addOptimizationProfile(profile);


    // FP16 build policy. kFP16 lets TRT pick FP16 implementations for every
    // op that supports it. Combined with the input+output kHALF pins above
    // this gives us a fully FP16 IO surface (which the preprocess and GPU
    // decode kernels assume), while leaving shape/Constant ops to their
    // native dtype so the build doesn't fail. Disable kTF32 too so any
    // residual FP32 reductions don't slide into TF32 silently.
    std::cout << "[TensorRT] FP16 build: kFP16 (IO pinned to kHALF, kTF32 disabled)" << std::endl;
    cfg->setFlag(nvinfer1::BuilderFlag::kFP16);
    cfg->clearFlag(nvinfer1::BuilderFlag::kTF32);

    // Builder optimization level controls how exhaustively TRT searches kernel
    // tactics. Default is 3; level 5 spends more build time probing tactics and
    // usually yields a measurably faster engine — a one-time cost since the
    // engine is cached on disk. Requires TRT 8.6+.
#if NV_TENSORRT_MAJOR > 8 || (NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR >= 6)
    cfg->setBuilderOptimizationLevel(5);
    std::cout << "[TensorRT] Builder optimization level: 5" << std::endl;
#endif

    cudaStream_t stream;
    cudaStreamCreate(&stream);

    std::cout << "[TensorRT] Building engine (this may take several minutes)..." << std::endl;

    auto plan = builder->buildSerializedNetwork(*network, *cfg);
    if (!plan)
    {
        std::cerr << "[TensorRT] ERROR: Could not build the engine" << std::endl;
        return nullptr;
    }

    cudaStreamSynchronize(stream);
    cudaStreamDestroy(stream);

    return std::unique_ptr<nvinfer1::IHostMemory>(plan);
}

struct ScopedExportState
{
    ScopedExportState()
    {
        TrtExportResetState();
        gIsTrtExporting = true;
    }

    ~ScopedExportState()
    {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        gProgressPhases.clear();
        gIsTrtExporting = false;
        gTrtExportCancelRequested = false;
        gTrtExportLastUpdateMs = TrtNowMs();
    }
};
} // namespace

std::unique_ptr<nvinfer1::IHostMemory> buildSerializedEngineFromOnnxMemory(const void* data, size_t size, nvinfer1::ILogger& logger)
{
    if (!data || size == 0)
    {
        std::cerr << "[TensorRT] ERROR: Empty ONNX memory buffer" << std::endl;
        return nullptr;
    }
    if (size > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
        std::cerr << "[TensorRT] ERROR: ONNX memory buffer is too large" << std::endl;
        return nullptr;
    }

    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(logger);
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    nvinfer1::INetworkDefinition* network = builder ? builder->createNetworkV2(explicitBatch) : nullptr;
    nvinfer1::IBuilderConfig* cfg = builder ? builder->createBuilderConfig() : nullptr;
    nvonnxparser::IParser* parser = network ? nvonnxparser::createParser(*network, logger) : nullptr;

    if (!builder || !network || !cfg || !parser)
    {
        std::cerr << "[TensorRT] ERROR: Could not create TensorRT builder objects" << std::endl;
        delete parser;
        delete network;
        delete cfg;
        delete builder;
        return nullptr;
    }

    TrtProgressMonitor progressMonitor;
    cfg->setProgressMonitor(&progressMonitor);
    ScopedExportState exportState;

    if (!parser->parse(data, static_cast<size_t>(size)))
    {
        std::cerr << "[TensorRT] ERROR: Error parsing ONNX model from memory" << std::endl;
        delete parser;
        delete network;
        delete cfg;
        delete builder;
        return nullptr;
    }

    auto plan = buildSerializedEngine(network, builder, cfg);

    delete parser;
    delete network;
    delete cfg;
    delete builder;

    return plan;
}

nvinfer1::ICudaEngine* buildEngineFromOnnxMemory(const void* data, size_t size, nvinfer1::ILogger& logger)
{
    auto plan = buildSerializedEngineFromOnnxMemory(data, size, logger);
    if (!plan)
        return nullptr;

    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(logger);
    nvinfer1::ICudaEngine* engine = runtime ? runtime->deserializeCudaEngine(plan->data(), plan->size()) : nullptr;
    if (!engine)
    {
        std::cerr << "[TensorRT] ERROR: Could not create engine" << std::endl;
        delete runtime;
        return nullptr;
    }

    delete runtime;
    std::cout << "[TensorRT] The FP16 engine was built successfully." << std::endl;
    return engine;
}

nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger)
{
    std::ifstream file(std::filesystem::u8path(onnxFile), std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] ERROR: Error opening the ONNX file: " << onnxFile << std::endl;
        return nullptr;
    }
    file.seekg(0, std::ios::end);
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> onnxData(size);
    if (!onnxData.empty())
        file.read(reinterpret_cast<char*>(onnxData.data()), static_cast<std::streamsize>(onnxData.size()));
    if (!file && !onnxData.empty())
    {
        std::cerr << "[TensorRT] ERROR: Error reading the ONNX file: " << onnxFile << std::endl;
        return nullptr;
    }
    return buildEngineFromOnnxMemory(onnxData.data(), onnxData.size(), logger);
}

