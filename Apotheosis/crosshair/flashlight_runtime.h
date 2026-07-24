#ifndef FLASHLIGHT_RUNTIME_H
#define FLASHLIGHT_RUNTIME_H

#include <chrono>
#include <vector>

#include <opencv2/opencv.hpp>

namespace flashlight_runtime
{

// One candidate flashlight halo, in detection-image pixel space, AFTER the
// appearance/depth/temporal/model-box gates have run on a YOLO result tick.
// The snapshot contains at most one stable selected core.
struct Spot
{
    cv::Rect    box{};                 // synthesized bbox (centre±radius), clamped
    cv::Point2f center{};              // halo centre (det-img px)
    float       radius       = 0.0f;
    float       confidence   = 0.0f;   // adjusted score, [0,1]
    int         track_id     = -1;
    bool        passed_depth = true;   // false only when depth-gate hard-rejected it
    bool        confirmed    = false;  // temporal tracker reached confirm_frames
    bool        onset        = false;  // light just turned on this frame
    bool        associated_with_model = false; // 有框时只瞄模型框，不注入光斑
    bool        independent_aimable = false;   // 无框且深度+连续三帧均通过
};

// Latest flashlight detection. `valid` means the latest YOLO result produced
// one recognized core. Single-writer (mouse inference consumer), multi-reader.
struct Snapshot
{
    bool              valid = false;
    std::vector<Spot> spots;            // sorted by descending confidence
    std::chrono::steady_clock::time_point ts{};
};

inline constexpr int kFreshnessMs = 150;

Snapshot read();
void publish(const Snapshot& snap);

// 每次 YOLO 发布新结果时调用一次。这样确认帧、立即丢失与 YOLO 使用同一时钟，
// 不再受采集 FPS 或预览线程丢帧影响。modelBoxes 是本轮经过玻璃过滤后的模型框；
// 若白核位于框内或距框边缘不超过半个框宽，只发布预览结果，不生成独立目标。
void process_inference_frame(const cv::Mat& bgrFrame,
                             const std::vector<cv::Rect>& modelBoxes);

} // namespace flashlight_runtime

#endif // FLASHLIGHT_RUNTIME_H
