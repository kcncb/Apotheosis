#ifndef GLASS_RUNTIME_H
#define GLASS_RUNTIME_H

#include <chrono>
#include <vector>

#include <opencv2/opencv.hpp>

namespace glass_runtime
{

// 单个 box 的玻璃判定结果(给预览叠图用)。`box` 已经裁切到帧内,坐标在
// 检测图像素空间。`is_glass=true` 表示该 box 已经从 aim 候选里抹掉。
// `evaluated=false` 表示这帧没参与过滤(框太小 / 禁用 / 越界),overlay
// 应该用中性色绘制,避免误导用户。
struct BoxJudgement
{
    cv::Rect box{};
    float    coverage     = 0.0f;
    bool     is_glass     = false;
    bool     evaluated    = false;
};

struct Snapshot
{
    std::vector<BoxJudgement> judgements;
    std::chrono::steady_clock::time_point ts{};
};

// 同步语义同 flashlight_runtime:单写多读。mouse_thread_loop 在过滤完每帧
// 检测后写,overlay 预览线程读最新一帧。
Snapshot read();
void publish(Snapshot snap);

inline constexpr int kFreshnessMs = 200;

} // namespace glass_runtime

#endif // GLASS_RUNTIME_H
