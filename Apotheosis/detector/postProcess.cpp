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
    out.maxDetections = snapshot->max_detections;
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

// Drop "large" boxes (area >= threshold) whose confidence is below the regular
// base threshold. Small boxes already cleared the lower decode floor and are
// kept as-is. No-op when baseConf < 0 (feature disabled).
static void applySmallTargetConfFilter(std::vector<Detection>& detections,
                                       float baseConf,
                                       double areaThreshPx)
{
    if (detections.empty() || baseConf < 0.0f)
        return;

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&](const Detection& d)
            {
                const double area = static_cast<double>(d.box.area());
                return area >= areaThreshPx && d.confidence < baseConf;
            }),
        detections.end());
}

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

void NMS(std::vector<Detection>& detections, float nmsThreshold, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (detections.empty()) return;

    if (nmsThreshold <= 0.0f)
    {
        if (nmsTime)
        {
            *nmsTime = std::chrono::duration<double, std::milli>(0);
        }
        return;
    }

    auto t0 = std::chrono::steady_clock::now();

    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& a, const Detection& b)
        {
            return a.confidence > b.confidence;
        }
    );

    std::vector<bool> suppress(detections.size(), false);
    std::vector<Detection> result;
    result.reserve(detections.size());

    for (size_t i = 0; i < detections.size(); ++i)
    {
        if (suppress[i]) continue;

        result.push_back(detections[i]);

        const cv::Rect& box_i = detections[i].box;
        const int class_i = detections[i].classId;
        const float area_i = static_cast<float>(box_i.area());

        for (size_t j = i + 1; j < detections.size(); ++j)
        {
            if (suppress[j]) continue;
            // Class-aware NMS: only suppress overlaps within the same class so
            // a hat and a head sitting on top of each other both survive.
            if (detections[j].classId != class_i) continue;

            const cv::Rect& box_j = detections[j].box;
            const cv::Rect intersection = box_i & box_j;

            if (intersection.width > 0 && intersection.height > 0)
            {
                const float intersection_area = static_cast<float>(intersection.area());
                const float union_area = area_i + static_cast<float>(box_j.area()) - intersection_area;

                if (intersection_area / union_area > nmsThreshold)
                {
                    suppress[j] = true;
                }
            }
        }
    }

    detections = std::move(result);

    auto t1 = std::chrono::steady_clock::now();
    if (nmsTime)
    {
        *nmsTime = t1 - t0;
    }
}

std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime,
    float smallTargetBaseConf,
    double smallTargetAreaThreshPx
)
{
    std::vector<Detection> detections;
    detections.reserve(256);

    if (shape.size() < 3) return detections;

    int64_t rows = shape[1];
    int64_t cols = shape[2];
    const float img_scale = trt_detector.img_scale;

    auto toInt = [](float v) { return static_cast<int>(std::lround(v)); };

    if (cols == 6)
    {
        int64_t numDetections = rows;
        for (int i = 0; i < numDetections; ++i)
        {
            const float* det = output + i * cols;
            float confidence = det[4];

            if (confidence > confThreshold)
            {
                int classId = static_cast<int>(det[5]);

                float cx = det[0];
                float cy = det[1];
                float dx = det[2];
                float dy = det[3];

                Detection detection;
                detection.preciseBox = cv::Rect2f(
                    cx * img_scale, cy * img_scale,
                    (dx - cx) * img_scale, (dy - cy) * img_scale);
                detection.box.x = toInt(detection.preciseBox.x);
                detection.box.y = toInt(detection.preciseBox.y);
                detection.box.width = toInt(detection.preciseBox.width);
                detection.box.height = toInt(detection.preciseBox.height);
                detection.confidence = confidence;
                detection.classId = classId;

                detections.push_back(detection);
            }
        }
    }
    else if (rows == numClasses + 4)
    {
        // Raw YOLOv8 layout [1, C, N], same layout as ONNX Runtime returns.
        // DML already decodes this path; keep TRT compatible when we copy the
        // native TensorRT output instead of using the GPU pre-decode fast path.
        const int64_t N = cols;
        const int64_t C = rows;
        for (int64_t i = 0; i < N; ++i)
        {
            float maxScore = 0.0f;
            int maxClassId = 0;
            for (int c = 0; c < numClasses; ++c)
            {
                const float score = output[(4 + c) * N + i];
                if (score > maxScore)
                {
                    maxScore = score;
                    maxClassId = c;
                }
            }

            if (maxScore <= confThreshold) continue;

            const float cx = output[0 * N + i];
            const float cy = output[1 * N + i];
            const float ow = output[2 * N + i];
            const float oh = output[3 * N + i];
            const float half_ow = 0.5f * ow;
            const float half_oh = 0.5f * oh;

            Detection det;
            det.preciseBox = cv::Rect2f(
                (cx - half_ow) * img_scale, (cy - half_oh) * img_scale,
                ow * img_scale, oh * img_scale);
            det.box.x = toInt(det.preciseBox.x);
            det.box.y = toInt(det.preciseBox.y);
            det.box.width = toInt(det.preciseBox.width);
            det.box.height = toInt(det.preciseBox.height);
            det.confidence = maxScore;
            det.classId = maxClassId;

            detections.push_back(det);
        }
    }
    else
    {
        // Row-major [N=rows, C=cols] layout (GPU-transposed upstream). Each
        // row is: cx, cy, w, h, score_0 .. score_{nc-1}. Reading anchors with
        // unit stride is massively more cache-friendly than the old CxN scan.
        const int64_t N = rows;
        const int64_t C = cols;
        for (int64_t i = 0; i < N; ++i)
        {
            const float* row = output + i * C;

            float maxScore = 0.0f;
            int maxClassId = 0;
            for (int c = 0; c < numClasses; ++c)
            {
                const float score = row[4 + c];
                if (score > maxScore)
                {
                    maxScore = score;
                    maxClassId = c;
                }
            }

            if (maxScore <= confThreshold) continue;

            const float cx = row[0];
            const float cy = row[1];
            const float ow = row[2];
            const float oh = row[3];
            const float half_ow = 0.5f * ow;
            const float half_oh = 0.5f * oh;

            Detection det;
            det.preciseBox = cv::Rect2f(
                (cx - half_ow) * img_scale, (cy - half_oh) * img_scale,
                ow * img_scale, oh * img_scale);
            det.box.x = toInt(det.preciseBox.x);
            det.box.y = toInt(det.preciseBox.y);
            det.box.width = toInt(det.preciseBox.width);
            det.box.height = toInt(det.preciseBox.height);
            det.confidence = maxScore;
            det.classId = maxClassId;

            detections.push_back(det);
        }
    }

    applySmallTargetConfFilter(detections, smallTargetBaseConf, smallTargetAreaThreshPx);
    NMS(detections, nmsThreshold, nmsTime);
    applyDeleteBucketFilter(detections);
    return detections;
}

std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime,
    float smallTargetBaseConf,
    double smallTargetAreaThreshPx
)
{
    std::vector<Detection> detections;
    if (shape.size() != 2) return detections;

    int64_t rows = shape[0];
    int64_t cols = shape[1];
    if (rows <= 0 || cols <= 0) return detections;
    if (rows > std::numeric_limits<int>::max() || cols > std::numeric_limits<int>::max()) return detections;
    const int rows_i = static_cast<int>(rows);
    const int cols_i = static_cast<int>(cols);

    if (cols_i == 6)
    {
        const int numDetections = rows_i;
        detections.reserve(static_cast<size_t>(numDetections));
        for (int i = 0; i < numDetections; ++i)
        {
            const float* det = output + i * cols_i;
            float confidence = det[4];
            if (confidence > confThreshold)
            {
                int classId = static_cast<int>(det[5]);
                float cx = det[0];
                float cy = det[1];
                float dx = det[2];
                float dy = det[3];

                Detection detection;
                detection.preciseBox = cv::Rect2f(cx, cy, dx - cx, dy - cy);
                detection.box.x = static_cast<int>(cx);
                detection.box.y = static_cast<int>(cy);
                detection.box.width = static_cast<int>(dx - cx);
                detection.box.height = static_cast<int>(dy - cy);
                detection.confidence = confidence;
                detection.classId = classId;
                detections.push_back(detection);
            }
        }
        applySmallTargetConfFilter(detections, smallTargetBaseConf, smallTargetAreaThreshPx);
        NMS(detections, nmsThreshold, nmsTime);
        applyDeleteBucketFilter(detections);
        return detections;
    }

    cv::Mat det_output(rows_i, cols_i, CV_32F, (void*)output);
    for (int i = 0; i < cols_i; ++i) {
        cv::Mat classes_scores = det_output.col(i).rowRange(4, 4 + numClasses);
        cv::Point class_id_point;
        double score;
        cv::minMaxLoc(classes_scores, nullptr, &score, nullptr, &class_id_point);
        if (score > confThreshold) {
            float cx = det_output.at<float>(0, i);
            float cy = det_output.at<float>(1, i);
            float ow = det_output.at<float>(2, i);
            float oh = det_output.at<float>(3, i);
            const float half_ow = 0.5f * ow;
            const float half_oh = 0.5f * oh;
            Detection detection;
            detection.preciseBox = cv::Rect2f(cx - half_ow, cy - half_oh, ow, oh);
            detection.box.x = static_cast<int>(cx - half_ow);
            detection.box.y = static_cast<int>(cy - half_oh);
            detection.box.width = static_cast<int>(ow);
            detection.box.height = static_cast<int>(oh);
            detection.confidence = static_cast<float>(score);
            detection.classId = class_id_point.y;
            detections.push_back(detection);
        }
    }
    applySmallTargetConfFilter(detections, smallTargetBaseConf, smallTargetAreaThreshPx);
    if (!detections.empty())
    {
        NMS(detections, nmsThreshold, nmsTime);
    }
    applyDeleteBucketFilter(detections);
    return detections;
}
