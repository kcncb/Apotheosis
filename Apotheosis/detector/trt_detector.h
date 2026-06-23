#ifndef TRT_DETECTOR_H
#define TRT_DETECTOR_H

#include <opencv2/opencv.hpp>
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
#include <cuda_runtime_api.h>

#include "i_detector.h"
#include "postProcess.h"
#include "../mem/gpu_image.h"

class TrtDetector : public IDetector
{
public:
    TrtDetector();
    ~TrtDetector() override;

    DetectorBackend backend() const noexcept override { return DetectorBackend::TensorRT; }
    const char* backendName() const noexcept override { return "TRT"; }

    bool initialize(const std::string& model_path) override;
    void processFrame(const cv::Mat& frame) override;
    void processFrameGpu(GpuImage frame) override;
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
    // True once *every* active slot has a captured + instantiated graph. When
    // numSlots==2 we capture one graph per slot, each writing to its own pinned
    // dst. Per-slot graphs lets graph capture coexist with double_buffer
    // pipelining instead of being mutually exclusive.
    bool cudaGraphCaptured;
    std::array<cudaGraph_t, 2> cudaGraphs{ nullptr, nullptr };
    std::array<cudaGraphExec_t, 2> cudaGraphExecs{ nullptr, nullptr };
    // Per-slot staging buffer for the preprocess kernel input. Each captured
    // graph reads from a fixed device pointer, so we copy the per-frame source
    // image (GpuImage from capture, or uploaded cv::Mat) into the slot's
    // staging buffer right before launching that slot's graph. The graph
    // itself then runs preprocess -> enqueue -> decode -> D2H without any
    // host-visible launch overhead.
    std::array<GpuImage, 2> graphInputBuffers;
    // Shape that the currently captured graphs are bound to. When the next
    // frame's shape doesn't match we destroy + recapture both graphs.
    int graphInputRows = 0;
    int graphInputCols = 0;
    int graphInputChannels = 0;
    size_t graphInputStep = 0;
    bool captureCudaGraph(int slot);
    void launchCudaGraph(int slot);
    void destroyCudaGraph();
    // Ensure each slot's staging buffer is allocated for (rows, cols, ch) and
    // matches what the currently captured graph expects. Returns true if the
    // graph needs to be rebuilt (shape changed or first-time alloc).
    bool ensureGraphStaging(int rows, int cols, int channels);

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
    GpuImage currentFrameGpu;
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
    void preProcess(const GpuImage& frame);

    GpuImage gpuFrameBuffer;

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
    // Per-output cached YOLO layout. cnLayout=true means model output is
    // [1, C, N] (Ultralytics default), false means [1, N, C] (transposed
    // export). outputC / outputN store the resolved channel and anchor counts
    // so the inference loop and CUDA Graph capture both feed the kernel
    // identical, layout-correct values.
    std::unordered_map<std::string, bool> outputCnLayout;
    std::unordered_map<std::string, int> outputC;
    std::unordered_map<std::string, int> outputN;
    void freeTransposedBuffers();

    // CUDA Events
    // 每 slot 一组计时 event。提交某 slot 的 GPU 工作时在该 slot 的 event 上打点,
    // 等下一帧(或单缓冲下本帧 sync 后)该 slot 完成再读 elapsedTime——否则在 CUDA
    // Graph + double_buffer 下,提交后立刻读会拿到尚未完成的 event(NotReady),值恒
    // 为 0,这正是"开 graph 后推理/拷贝耗时显示 0"的根因。
    std::array<cudaEvent_t, 2> preprocessStartEvent{ nullptr, nullptr };
    std::array<cudaEvent_t, 2> inferenceStartEvent{ nullptr, nullptr };
    std::array<cudaEvent_t, 2> inferenceCompleteEvent{ nullptr, nullptr };
    std::array<cudaEvent_t, 2> copyCompleteEvent{ nullptr, nullptr };
    bool asyncInferenceInProgress = false;
};

#endif // TRT_DETECTOR_H
