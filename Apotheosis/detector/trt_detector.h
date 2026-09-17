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
    void processFrame(const cv::Mat& frame, runtime::FrameContext context = runtime::FrameContext{}) override;
    void processFrameGpu(GpuImage frame, runtime::FrameContext context = runtime::FrameContext{}) override;
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
    // True once the (single) graph has been captured + instantiated.
    // ★ 2026-09-17: 双缓冲整条移除后只剩单槽, 所以只需要一张图。
    //   下面这些数组形式上保留 2 个元素以免改动面过大, 实际只用下标 0。
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
    // ★ 2026-09-17: numSlots 成员已删除 —— 单缓冲固定单槽。
    std::unordered_map<std::string, void*>& pinnedSlot(int s) {
        return s == 0 ? pinnedOutputBuffers : pinnedOutputBuffersB;
    }
    void allocatePinnedOutputs();
    void freePinnedOutputs();

    // 等待一个 CUDA event 完成。
    //
    // 默认走自旋 (cudaEventQuery 轮询), 完成即返回; 这在推理线程上比
    // cudaEventSynchronize 的内核态阻塞唤醒快 10~40us, 而且不会因为 CPU
    // 抢占被推迟 —— 消的是 p99 尾部而非均值。
    //
    // 自旋超过 spin_wait_timeout_ms 后回退到阻塞式同步, 避免 GPU 掉卡 /
    // 上下文丢失时把推理线程永久占死。
    void waitForEvent(cudaEvent_t ev);
    // 自旋统计: 用于确认这条路径真的生效 (chain log 里打出来)。
    double lastSyncSpinMs = 0.0;
    bool   lastSyncUsedSpin = false;
    long   syncFallbackCount = 0;

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

    // ── 延迟探针: 按帧携带的时间戳 (T0 采集 / T1 取帧) ──
    //
    // pending*: processFrame*/processFrameGpu 写入(持 inferenceMutex), 推理线程
    //   在【取走该帧的那一刻】读走, 存进自己的 per-slot 数组。
    // publish*: 当前正在发布的那一帧的 T0/T1, postProcess() 发布时使用。
    //
    // 为什么不直接读探针的全局槽: 那是"只存最新"的量, 而发布发生在取走下一帧
    // 之后(双缓冲下必然如此) —— 在那里读到的恒定是下一帧的戳, 于是 total 系统性
    // 地少算整整一个帧间隔(120fps = 8.33ms)。按帧、按槽携带才不会有这个错位。
    //
    // 这里刻意存两个 int64_t 而不是探针的 SubmitStamp, 免得头文件为了一个
    // POD 去包含整个 latency_probe.h。
    runtime::FrameContext pendingContext, publishContext;
    int64_t publishCaptureNs = 0;
    int64_t publishSubmitNs  = 0;

    void loadEngine(const std::string& engineFile);

    void preProcess(const cv::Mat& frame);
    void preProcess(const GpuImage& frame);

    GpuImage gpuFrameBuffer;

    // ★ 2026-09-17: 签名改了 —— 原来是 `const float* output`。
    //   现在直接传 pinned 缓冲区原始指针 + 数据类型, 由本函数按需读取:
    //   模型固定 FP16 I/O, 旧实现在调用前先把整块输出逐元素 __half2float 成
    //   一个 float 阵列(纯 CPU 开销), 而 end2end 只需要读 N 行 × 6 个数。
    void postProcess(
        const void* output,
        const std::string& outputName,
        nvinfer1::DataType dtype,
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

    // ★ 2026-09-17: 下面这些成员全部删除 —— 它们只服务"raw YOLO 输出 +
    //   GPU 转置 + 候选块"那条路径, 而本程序现在只接受 end2end 输出 [1,N,6]:
    //     fp16OutputScratch   (CPU 侧 __half2float 整块转换的落地缓冲)
    //     transposedDeviceBuffers / transposedSizes / outputNeedsTranspose
    //     outputCnLayout / outputC / outputN  (raw 布局判定)
    //   freeTransposedBuffers() 保留为空实现, 因为 initialize()/析构仍调用它
    //   (调用序列保持不变, 少一处"看起来还有东西要做"的误导)。
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
