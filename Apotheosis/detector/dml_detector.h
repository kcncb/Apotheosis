#ifndef DIRECTML_DETECTOR_H
#define DIRECTML_DETECTOR_H

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "i_detector.h"
#include "postProcess.h"

struct DmlAdapterInfo
{
    int device_id = 0;
    std::string name;
};

std::string GetDMLDeviceName(int deviceId);
std::vector<DmlAdapterInfo> EnumerateDMLAdapters();

class DirectMLDetector : public IDetector
{
public:
    DirectMLDetector();
    explicit DirectMLDetector(const std::string& model_path);
    ~DirectMLDetector() override;

    DetectorBackend backend() const noexcept override { return DetectorBackend::DirectML; }
    const char* backendName() const noexcept override { return "DML"; }

    bool initialize(const std::string& model_path) override;

    std::vector<Detection> detect(const cv::Mat& input_frame);
    std::vector<std::vector<Detection>> detectBatch(const std::vector<cv::Mat>& frames);

    void inferenceThread() override;
    void processFrame(const cv::Mat& frame) override;

    int numberOfClasses() const override { return num_classes_; }
    std::vector<std::string> classNames() const override { return class_names_; }

    std::chrono::duration<double, std::milli> lastPreprocessTime() const override { return lastPreprocessTimeValue; }
    std::chrono::duration<double, std::milli> lastInferenceTime() const override { return lastInferenceTimeValue; }
    std::chrono::duration<double, std::milli> lastCopyTime() const override { return lastCopyTimeValue; }
    std::chrono::duration<double, std::milli> lastPostprocessTime() const override { return lastPostprocessTimeValue; }
    std::chrono::duration<double, std::milli> lastNmsTime() const override { return lastNmsTimeValue; }

    void requestExit() override;

    std::chrono::duration<double, std::milli> lastInferenceTimeValue{};
    std::chrono::duration<double, std::milli> lastPreprocessTimeValue{};
    std::chrono::duration<double, std::milli> lastCopyTimeValue{};
    std::chrono::duration<double, std::milli> lastPostprocessTimeValue{};
    std::chrono::duration<double, std::milli> lastNmsTimeValue{};

    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit = false;

private:
    Ort::Env env;
    Ort::Session session{ nullptr };
    Ort::SessionOptions session_options;
    Ort::AllocatorWithDefaultOptions allocator;

    std::string input_name;
    std::string output_name;
    std::vector<int64_t> input_shape;
    int num_classes_ = 0;
    std::vector<std::string> class_names_;

    std::mutex inferenceMutex;
    cv::Mat currentFrame;
    bool frameReady = false;

    void initializeModel(const std::string& model_path);
    void initializeModelFromBytes(const std::vector<uint8_t>& model_bytes, const std::string& display_path);
    void readModelMetadata(const std::string& model_path);
    Ort::MemoryInfo memory_info;
};

#endif // DIRECTML_DETECTOR_H
