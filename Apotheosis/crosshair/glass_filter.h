#ifndef GLASS_FILTER_H
#define GLASS_FILTER_H

#include <opencv2/opencv.hpp>
#include <vector>

#include "crosshair_detector.h" // CrosshairColorBand

namespace crosshair
{

// 玻璃过滤(三角洲穿不透玻璃后的人形抑制)。
//
// 模型只看轮廓,玻璃后的目标仍然能被识别为人 → 锁过去打不穿,纯浪费。
// 本模块在 mouse 循环里、target tracker 之前,对每个 detection box 的"边
// 缘环"采样玻璃膜特征色(浅蓝/浅绿薄膜,低饱和度,高亮度)。命中率超过阈
// 值的 box 视为"被玻璃罩住",直接从候选里抹掉。
//
// 为什么是边缘环、不是整框:目标人形会占满 box 中心,如果整框算颜色覆盖
// 率,人物本身的衣服 / 皮肤会稀释玻璃膜的信号;反过来,box 边缘最能反映
// "目标和摄像机之间隔着什么"——玻璃膜是均匀薄膜,沿框四周都能见到;空
// 气前的目标边缘是杂乱背景,玻璃色覆盖率天然很低。
//
// 计算只对单 box ROI 做 cvtColor + inRange,每框 < 0.2 ms。N 框总开销远低
// 于一次 PCIe Gen1 D2H,放心放在 CPU 线程上跑。
struct GlassFilterSettings
{
    bool enabled = false;

    // 玻璃膜 HSV 色带。和准星 / 镭射共用同一套 CrosshairColorBand 结构,
    // 用户在 UI 里加色;默认值给两条浅蓝 + 浅绿低饱和高亮度带。
    std::vector<CrosshairColorBand> colors;

    // 边缘环厚度,占 bbox 短边的比例 [0.05, 0.45]。
    // 0.15 = 短边的 15 %,典型 60 px 框采 9 px 宽的环。
    // 太薄:玻璃信号点少、噪声主导;太厚:吃进框中心的人物,coverage 被稀释。
    float edge_ring_frac = 0.15f;

    // 命中率阈值。环内被任一色带命中的像素 ÷ 环总像素 ≥ 此值 → 判玻璃。
    // 0.45 = 默认,对薄膜场景比较稳。误伤多了往上调,玻璃漏过多往下调。
    float coverage_threshold = 0.45f;

    // 太小的框不参与过滤(信号不可靠 + 容易把远处真目标误杀)。框短边
    // < 此值 (检测图像素) 时直接 pass。
    int min_box_short_side = 20;
};

// 单 box 过滤结果。`is_behind_glass` 用来决定是否丢弃,其他字段供预览叠图
// 显示:用户能看到具体哪个框被判玻璃、覆盖率多少,方便调色 / 调阈值。
struct GlassResult
{
    bool  is_behind_glass = false;
    float coverage        = 0.0f;
    int   ring_pixels     = 0;
    int   matched_pixels  = 0;
    // false = 该 box 没参与过滤(太小 / 越界 / 色带空 / disabled)。供 UI
    // 区分"过滤判定通过"和"压根没判过"两种 not-glass 情况。
    bool  evaluated       = false;
};

// 无状态 filter。一次实例可被并发使用(没有可变成员)。
class GlassFilter
{
public:
    GlassResult check(const cv::Mat& bgrFrame,
                      const cv::Rect& bbox,
                      const GlassFilterSettings& settings) const;
};

// 默认色带:浅蓝薄膜 + 浅绿薄膜(三角洲常见玻璃色)。低 S、高 V 是关键。
std::vector<CrosshairColorBand> default_glass_bands();

} // namespace crosshair

#endif // GLASS_FILTER_H
