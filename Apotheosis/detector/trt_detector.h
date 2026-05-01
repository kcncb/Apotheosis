#ifndef TRT_DETECTOR_H
#define TRT_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <NvInfer.h>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cuda_fp16.h>
#include <memory>
#include <thread>
#include <chrono>
#include <functional>
#include <opencv2/cudawarping.hpp>
#include <opencv2/cudaarithm.hpp>
#include <cuda_runtime_api.h>

#include "i_detector.h"
#include "postProcess.h"

class TrtDetector : public IDetector
{
public:
    TrtDetector();
    ~TrtDetector() override;

    DetectorBackend backend() const noexcept override { return DetectorBackend::TensorRT; }
    const char* backendName() const noexcept override { return "TRT"; }

    bool initialize(const std::string& model_path) override;
    void processFrame(const cv::Mat& frame) override;
    void processFrameGpu(const cv::cuda::GpuMat& frame) override;
    void inferenceThread() override;

    int numberOfClasses() const override { return numClasses; }
    std::vector<std::string> classNames() const override { return class_names_; }

    std::chrono::duration<double, std::milli> lastPreprocessTime() const override { return lastPreprocessTimeValue; }
    std::chrono::duration<double, std::milli> lastInferenceTime() const override { return lastInferenceTimeValue; }
    std::chrono::duration<double, std::milli> lastCopyTime() const override { return lastCopyTimeValue; }
    std::chrono::duration<double, std::milli> lastPostprocessTime() const override { return lastPostprocessTimeValue; }
    std::chrono::duration<double, std::milli> lastNmsTime() const override { return lastNmsTimeValue; }

    void requestExit() override;

    float img_scale;

    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::unordered_map<std::string, size_t> outputSizes;

    std::chrono::duration<double, std::milli> lastPreprocessTimeValue{};
    std::chrono::duration<double, std::milli> lastInferenceTimeValue{};
    std::chrono::duration<double, std::milli> lastCopyTimeValue{};
    std::chrono::duration<double, std::milli> lastPostprocessTimeValue{};
    std::chrono::duration<double, std::milli> lastNmsTimeValue{};

private:
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream;

    bool useCudaGraph;
    bool cudaGraphCaptured;
    cudaGraph_t cudaGraph;
    cudaGraphExec_t cudaGraphExec;
    void captureCudaGraph();
    void launchCudaGraph();
    void destroyCudaGraph();

    // Primary pinned output buffers (slot 0). When double_buffer is enabled a
    // second slot is also allocated below and the inference thread pipelines:
    // submit GPU work for curr_slot on the stream, then post-process the
    // previously-completed slot on the CPU while the GPU chews on curr. The
    // device output bindings themselves are shared across frames — stream
    // ordering on a single stream guarantees sequential consistency — only
    // the pinned host buffers (CPU-read during post-process) need duplicating.
    std::unordered_map<std::string, void*> pinnedOutputBuffers;
    std::unordered_map<std::string, void*> pinnedOutputBuffersB;
    std::array<cudaEvent_t, 2> slotDoneEvent{ nullptr, nullptr };
    int numSlots = 1;
    std::unordered_map<std::string, void*>& pinnedSlot(int s) {
        return s == 0 ? pinnedOutputBuffers : pinnedOutputBuffersB;
    }
    void allocatePinnedOutputs();
    void freePinnedOutputs();

    std::mutex inferenceMutex;
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit;
    cv::Mat currentFrame;
    cv::cuda::GpuMat currentFrameGpu;
    bool frameReady;

    enum class PendingFrameType
    {
        None = 0,
        Cpu = 1,
        Gpu = 2
    };
    PendingFrameType pendingFrameType = PendingFrameType::None;

    void loadEngine(const std::string& engineFile);

    void preProcess(const cv::Mat& frame);
    void preProcess(const cv::cuda::GpuMat& frame);

    cv::cuda::GpuMat gpuFrameBuffer;
    cv::cuda::GpuMat gpuResizedBuffer;
    cv::cuda::GpuMat gpuFloatBuffer;
    std::vector<cv::cuda::GpuMat> gpuChannelBuffers;

    cv::cuda::Stream cvStream;

    void postProcess(
        const float* output,
        const std::string& outputName,
        std::chrono::duration<double, std::milli>* nmsTime
    );

    void getInputNames();
    void getOutputNames();
    void getBindings();

    std::unordered_map<std::string, size_t> inputSizes;
    std::unordered_map<std::string, void*> inputBindings;
    std::unordered_map<std::string, void*> outputBindings;
    std::unordered_map<std::string, std::vector<int64_t>> outputShapes;
    int numClasses;
    std::vector<std::string> class_names_;

    size_t getSizeByDim(const nvinfer1::Dims& dims);
    size_t getElementSize(nvinfer1::DataType dtype);

    std::string inputName;
    void* inputBufferDevice;

    std::unordered_map<std::string, nvinfer1::DataType> outputTypes;
    std::unordered_map<std::string, std::vector<float>> fp16OutputScratch;

    // Per-output device buffer that holds the post-transpose [N, C] float32
    // tensor (YOLOv8-style outputs only). Pre-transpose on the GPU fixes the
    // stride-unfriendly access pattern in the old CPU decode loop and folds in
    // the fp16->fp32 cast that used to run per-element on the CPU.
    std::unordered_map<std::string, void*> transposedDeviceBuffers;
    std::unordered_map<std::string, size_t> transposedSizes;
    std::unordered_map<std::string, bool> outputNeedsTranspose;
    void freeTransposedBuffers();

    cv::cuda::GpuMat resizedBuffer;
    cv::cuda::GpuMat floatBuffer;
    std::vector<cv::cuda::GpuMat> channelBuffers;

    // CUDA Events
    cudaEvent_t preprocessStartEvent = nullptr;
    cudaEvent_t inferenceStartEvent = nullptr;
    cudaEvent_t inferenceCompleteEvent = nullptr;
    cudaEvent_t copyCompleteEvent = nullptr;
    bool asyncInferenceInProgress = false;
};

#endif // TRT_DETECTOR_H
