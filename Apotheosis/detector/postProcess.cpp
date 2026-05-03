#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <limits>
#include <unordered_set>

#include "postProcess.h"
#include "Apotheosis.h"
#include "trt_detector.h"

void applyDeleteBucketFilter(std::vector<Detection>& detections)
{
    if (detections.empty())
        return;

    std::unordered_set<int> deleted;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        for (const auto& cf : config.class_filters)
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
    std::chrono::duration<double, std::milli>* nmsTime
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
                detection.box.x = toInt(cx * img_scale);
                detection.box.y = toInt(cy * img_scale);
                detection.box.width = toInt((dx - cx) * img_scale);
                detection.box.height = toInt((dy - cy) * img_scale);
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
            det.box.x = toInt((cx - half_ow) * img_scale);
            det.box.y = toInt((cy - half_oh) * img_scale);
            det.box.width = toInt(ow * img_scale);
            det.box.height = toInt(oh * img_scale);
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
            det.box.x = toInt((cx - half_ow) * img_scale);
            det.box.y = toInt((cy - half_oh) * img_scale);
            det.box.width = toInt(ow * img_scale);
            det.box.height = toInt(oh * img_scale);
            det.confidence = maxScore;
            det.classId = maxClassId;

            detections.push_back(det);
        }
    }

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
    std::chrono::duration<double, std::milli>* nmsTime
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

                cv::Rect box;
                box.x = static_cast<int>(cx);
                box.y = static_cast<int>(cy);
                box.width = static_cast<int>(dx - cx);
                box.height = static_cast<int>(dy - cy);
                detections.push_back(Detection{ box, confidence, classId });
            }
        }
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
            cv::Rect box;
            box.x = static_cast<int>(cx - half_ow);
            box.y = static_cast<int>(cy - half_oh);
            box.width = static_cast<int>(ow);
            box.height = static_cast<int>(oh);
            detections.push_back(Detection{ box, static_cast<float>(score), class_id_point.y });
        }
    }
    if (!detections.empty())
    {
        NMS(detections, nmsThreshold, nmsTime);
    }
    applyDeleteBucketFilter(detections);
    return detections;
}
