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

std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);

std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);
#endif // POSTPROCESS_H
