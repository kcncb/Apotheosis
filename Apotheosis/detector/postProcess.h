#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include <chrono>
#include <vector>
#include <opencv2/opencv.hpp>

struct Detection
{
    cv::Rect box;
    float confidence;
    int classId;
};

void NMS(
    std::vector<Detection>& detections,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);

// Drop detections whose classId is in the config's "Delete" bucket. Takes
// configMutex internally so callers do not need to.
void applyDeleteBucketFilter(std::vector<Detection>& detections);

// Small-target recall enhancement. When config.small_target_enabled is true,
// boxes whose area is below small_target_area_frac × detection_resolution² keep
// the lower small_target_confidence threshold, while larger boxes keep the
// regular confidence_threshold. This struct carries the values derived from the
// live config so the GPU decode floor and the CPU area-adaptive filter stay
// consistent.
//   decodeFloor   — confidence floor for GPU/CPU decode (lets weak small
//                   targets through). Equals confidence_threshold when disabled.
//   baseConf      — large-target keep threshold; a negative value disables the
//                   area-adaptive filter (feature off).
//   areaThreshPx  — area (in detection pixels) below which a box is "small".
struct SmallTargetDecode
{
    float  decodeFloor = 0.0f;   // CPU decode 门槛(GPU-cands 路径用,=min(base,small))
    float  baseConf = -1.0f;     // CPU 面积过滤的大目标阈值(raw/DML 路径);-1 禁用
    float  smallConf = -1.0f;    // GPU 内核小目标阈值;-1 禁用
    double areaThreshPx = 0.0;
};

struct DetectorRuntimeSettings
{
    float confidenceThreshold = 0.25f;
    float nmsThreshold = 0.45f;
    int maxDetections = 100;
    int detectionResolution = 320;
};

// 在 configMutex 内一次性生成帧级只读快照。
DetectorRuntimeSettings detectorRuntimeSettings();

// Read the small-target config and derive the decode floor / filter params.
SmallTargetDecode computeSmallTargetDecode();

// Cap detections to the top-K (maxDetections) by confidence, dropping the
// weakest beyond the budget. No-op when maxDetections <= 0 or already within
// budget. Downstream safety valve so dense scenes / a low decode floor can't
// flood the tracker and overlay.
void capDetectionsToMax(std::vector<Detection>& detections, int maxDetections);

std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr,
    float smallTargetBaseConf = -1.0f,
    double smallTargetAreaThreshPx = 0.0
);

std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr,
    float smallTargetBaseConf = -1.0f,
    double smallTargetAreaThreshPx = 0.0
);
#endif // POSTPROCESS_H
