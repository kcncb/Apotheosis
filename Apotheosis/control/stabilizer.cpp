#include "stabilizer.h"

#include <algorithm>
#include <cmath>

namespace control {

bool aspectRatioPlausible(const Box& box, const StabilizerConfig& cfg)
{
    if (!box.valid())
        return false;
    const double aspect = box.w / box.h;
    return aspect >= cfg.minAspect && aspect <= cfg.maxAspect;
}

StabilizerResult stabilize(const Candidate& candidate,
                           const StabilizerConfig& cfg,
                           StabilizerState& state)
{
    StabilizerResult result;

    // ── 形状检查（最便宜的一道，先做） ──────────────────────────────────
    if (!aspectRatioPlausible(candidate.box, cfg))
    {
        result.verdict = StabilizerVerdict::Rejected;
        result.accepted = false;
        // ★ 刻意【不】更新 lastBox：一个形状离谱的框不该成为下一个基准，
        //   否则误检会把基准带偏，下一帧真目标反而被判成 Snap。
        return result;
    }

    result.box = candidate.box;   // ★ 原样透传：不做位置平滑（D6）
    result.accepted = true;

    // ── 第一帧 / 刚复位 ────────────────────────────────────────────────
    if (!state.hasLast)
    {
        result.verdict = StabilizerVerdict::NoHistory;
        state.lastBox = candidate.box;
        state.hasLast = true;
        return result;
    }

    // ── 认目标 + 突变检测 ──────────────────────────────────────────────
    // 判据一：中心是否还在附近
    const double centerDist = (candidate.box.center() - state.lastBox.center()).norm();
    const double lastDiag = state.lastBox.diagonal();

    // 判据二：尺寸是否还相似
    const double areaA = candidate.box.area();
    const double areaB = state.lastBox.area();
    const double ratio = (areaB > 0.0) ? (areaA / areaB) : 0.0;
    const bool sizeOk = ratio >= (1.0 / cfg.areaRatioTol) && ratio <= cfg.areaRatioTol;

    // 突变：尺寸跳变超过 lastDiag × kSnapMult ⇒ 判为瞬移/换目标。
    // ★ 注意量纲：这里比的是"位移量"与"框对角线 × 系数"，两者都是检测像素。
    const bool snapped = (centerDist > lastDiag * cfg.kSnapMult) || !sizeOk;

    if (snapped)
    {
        result.verdict = StabilizerVerdict::Snap;
        // ★ 这个 verdict 就是给下游的信号：滤波状态必须【硬重置】，
        //   而不是"慢慢追过去"。不重置的话，换目标时会出现一段
        //   "从旧位置滑到新位置"的过渡，而控制器会把这段当成"目标在高速移动"。
    }
    else
    {
        // 认出来了。★ 仍然只更新基准，不改动输出框。
        const bool nearEnough = (centerDist <= lastDiag * cfg.matchCenterRatio);
        result.verdict = nearEnough ? StabilizerVerdict::Ok : StabilizerVerdict::Snap;
    }

    // 基准始终跟到当前帧（这是"认目标"的记忆，不是滤波）。
    state.lastBox = candidate.box;
    state.hasLast = true;

    return result;
}

} // namespace control
