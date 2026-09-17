#pragma once

// ② 检测稳定器 —— ★ 不是滤波（D6）
//
// 职责只有两条：
//   1. 认目标：这个框和上一帧的那个是不是同一个（中心距离 + 尺寸相似度）
//   2. 剔除异常框：形状离谱的（宽高比）、尺寸突变的
//
// ★★ 它【不做位置平滑】：只重画当前帧的框，不参考上一帧的位置。
//   所以它不是滤波，不违反"全链路只有一道滤波"（D7）。
//   这一条是用户明确要求的，改动前先想清楚。
//
// ★ 突变检测归这里（原放在 Kalman 专属节，是结构错误）：
//   kSnapMult = 1.15 × 平滑后 bbox 对角线。
//   瞬移（威龙式推进）/ 被换目标时，必须【硬重置下游的滤波状态】。
//   它与卡尔曼无关 —— 无论开不开卡尔曼都需要。

#include "types.h"

namespace control {

struct StabilizerConfig
{
    // 认目标的判据：中心距离 < 上一帧框对角线 × 这个系数。
    // ★ 待实测（方案 §7 第 6 条）。
    double matchCenterRatio = 0.5;
    // 认目标的判据：面积比必须在 [1/areaRatioTol, areaRatioTol] 内。
    double areaRatioTol = 2.0;

    // 突变检测：尺寸跳变超过"上一帧对角线 × kSnapMult" ⇒ 判为瞬移/换目标。
    // ★ 1.15 来自 art-design.md:204（历史实测值）。
    double kSnapMult = 1.15;

    // 宽高比合理性检查：超出 [minAspect, maxAspect] 的框视为模型误检。
    // 人形目标大致在 0.2 ~ 5 之间（横躺/竖直都允许）。
    // ★ 这是"便宜的检查比让滤波器消化一个错框划算"。
    double minAspect = 0.2;
    double maxAspect = 5.0;
};

enum class StabilizerVerdict
{
    Ok,          // 认出来了，尺寸正常 ⇒ 正常处理
    NoHistory,   // 第一帧（或刚复位）⇒ 建立基准，下游滤波需初始化
    Snap,        // 尺寸突变/瞬移 ⇒ ★ 下游滤波必须硬重置
    Rejected,    // 形状离谱 ⇒ 本帧不可用
};

struct StabilizerState
{
    Box lastBox;
    bool hasLast = false;

    void reset()
    {
        hasLast = false;
        lastBox = Box{};
    }
};

struct StabilizerResult
{
    StabilizerVerdict verdict = StabilizerVerdict::NoHistory;
    Box box;                 // 重画后的框（★ 就是当前帧的框，未平滑）
    bool accepted = false;   // verdict 不是 Rejected 且形状合法
};

// 处理一个候选框。
// ★ 输出框【就是输入框】（可能经过宽高比检查），不含任何位置平滑 —— 见文件头。
StabilizerResult stabilize(const Candidate& candidate,
                           const StabilizerConfig& cfg,
                           StabilizerState& state);

// 形状合理性检查（独立导出，方便单测与复用）。
bool aspectRatioPlausible(const Box& box, const StabilizerConfig& cfg);

} // namespace control
