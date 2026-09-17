#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <limits>
#include <unordered_set>

#include "postProcess.h"
#include "Apotheosis.h"
#include "trt_detector.h"
#include "runtime/config_snapshot.h"

void applyDeleteBucketFilter(std::vector<Detection>& detections)
{
    if (detections.empty())
        return;

    std::unordered_set<int> deleted;
    {
        const auto snapshot = runtime_config::read();
        for (const auto& cf : snapshot->class_filters)
        {
            if (cf.bucket == ClassBucket::Delete)
                deleted.insert(cf.class_id);
        }
    }

    if (deleted.empty())
        return;

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&deleted](const Detection& d) { return deleted.count(d.classId) > 0; }),
        detections.end());
}

DetectorRuntimeSettings detectorRuntimeSettings()
{
    DetectorRuntimeSettings out;
    const auto snapshot = runtime_config::read();
    out.confidenceThreshold = snapshot->confidence_threshold;
    out.nmsThreshold = snapshot->nms_threshold;
    // ★ 2026-09-17: maxDetections 恒为 kFixedMaxDetections(=20), 不再从快照读 ——
    //   配置层已经把它钉死, 但"读一个理论上可变的字段"是将来被改回去的入口。
    //   直接取常量, 让"候选数固定"这件事在代码里也是硬约束而不只是约定。
    out.maxDetections = kFixedMaxDetections;
    out.detectionResolution = snapshot->detection_resolution;
    return out;
}

SmallTargetDecode computeSmallTargetDecode()
{
    SmallTargetDecode out;
    float base = 0.25f;
    bool enabled = false;
    float small_conf = 0.15f;
    int resolution = 320;
    float area_frac = 0.0025f;
    const auto snapshot = runtime_config::read();
    base = snapshot->confidence_threshold;
    enabled = snapshot->small_target_enabled;
    small_conf = snapshot->small_target_confidence;
    resolution = snapshot->detection_resolution;
    area_frac = snapshot->small_target_area_frac;
    if (!enabled)
    {
        out.decodeFloor = base;
        out.baseConf = -1.0f;   // disables the CPU area-adaptive filter
        out.smallConf = -1.0f;  // disables the GPU-side area threshold
        out.areaThreshPx = 0.0;
        return out;
    }
    const float smallConf = small_conf;
    const double res = static_cast<double>(resolution);
    out.decodeFloor = std::min(base, smallConf);
    out.baseConf = base;
    out.smallConf = smallConf;
    out.areaThreshPx = static_cast<double>(area_frac) * res * res;
    return out;
}

// ★ 2026-09-17: applySmallTargetConfFilter 已删除 —— 它是 raw 解码路径的召回
//   补偿(GPU 粗筛放宽门槛后再按面积二次过滤)。end2end 模型自己决定保留哪些框,
//   再按面积卡一遍只会与模型的选择打架。
//   注意: computeSmallTargetDecode / SmallTargetDecode 结构体保留 —— 配置项与
//   界面仍在, 且 delete-bucket 等路径引用它; 若要彻底清掉需另开一次改动。

void capDetectionsToMax(std::vector<Detection>& detections, int maxDetections)
{
    if (maxDetections <= 0)
        return;
    if (detections.size() <= static_cast<size_t>(maxDetections))
        return;
    // Partial sort: move the top-K highest-confidence detections to the front,
    // then drop the tail. O(n) average, cheaper than a full sort.
    std::nth_element(
        detections.begin(),
        detections.begin() + maxDetections,
        detections.end(),
        [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    detections.resize(static_cast<size_t>(maxDetections));
}

// ★ 2026-09-17: NMS / postProcessYolo(raw 解码) / postProcessYoloDML 三个函数
//   整段删除。本程序现在只接受 end2end 模型(输出 [1,N,6], NMS 与解码已烘进
//   计算图), 这三者都是 raw YOLO 输出形态的 CPU 后处理:
//     · postProcessYolo 的 cols==6 分支已内联进 TrtDetector::postProcess;
//     · 它的 raw 分支([1,C,N]/[1,N,C] 解码) 与 NMS 一起失去消费者;
//     · postProcessYoloDML 随 DirectML 后端一起删除。
//   保留在这条路径上的只有: capDetectionsToMax / applyDeleteBucketFilter /
//   detectorRuntimeSettings / computeSmallTargetDecode(后者仅剩少量消费者)。
