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
    // 保留模型解码得到的浮点框供 AVA selector/tracker 使用。旧的整数框
    // 继续提供给 overlay 和其它既有消费者，避免在显示链中扩散改动。
    cv::Rect2f preciseBox;
};

// ★ 2026-09-17: NMS 声明已删除 —— end2end 模型的 NMS 在计算图内完成,
//   CPU 侧再抑制会把"两个真实目标靠得很近"的情况误删。

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

// ★ 2026-09-17 删除: postProcessYolo (raw YOLO 解码 + NMS) 与 postProcessYoloDML。
//   本程序现在只接受 end2end 模型 [1,N,6], 解码/NMS 都在图内; 那段处理已经
//   内联进 TrtDetector::postProcess。DML 版本随 DirectML 后端一起删除。
#endif // POSTPROCESS_H
