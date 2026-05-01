#ifndef NVINF_H
#define NVINF_H

#include "NvInfer.h"
#include "Apotheosis.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class Logger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) noexcept override;
    static const char* severityLevelName(Severity severity);
};

extern Logger gLogger;

inline nvinfer1::IBuilder* createInferBuilder();
inline nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder);
inline nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder);

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime);
nvinfer1::ICudaEngine* loadEngineFromMemory(const void* data, size_t size, nvinfer1::IRuntime* runtime);
nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger);
std::unique_ptr<nvinfer1::IHostMemory> buildSerializedEngineFromOnnxMemory(const void* data, size_t size, nvinfer1::ILogger& logger);
nvinfer1::ICudaEngine* buildEngineFromOnnxMemory(const void* data, size_t size, nvinfer1::ILogger& logger);

#endif // NVINF_H
