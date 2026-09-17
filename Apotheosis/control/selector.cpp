#include "selector.h"

#include <algorithm>
#include <cmath>

namespace control {

std::vector<size_t> filterAimCandidates(const std::vector<Candidate>& candidates,
                                        const ClassBuckets& buckets,
                                        const SelectorConfig& cfg)
{
    std::vector<size_t> out;
    out.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (!candidates[i].box.valid())
            continue;
        if (buckets.bucketOf(candidates[i].classId) != Bucket::Aim)
            continue;
        // ★★ 逐类别最低置信度（准入）。<= 0 表示该类不限。
        //   ★ 这里是【额外收紧】, 不替代 AI 页的全局阈值 —— 全局阈值在下游
        //     仍然生效, 所以两者是"都要过"的关系, 这里不做 max 合并。
        const double need = cfg.minConfOf(candidates[i].classId);
        if (need > 0.0 && candidates[i].confidence < need)
            continue;
        out.push_back(i);
    }
    return out;
}

namespace {

// 候选中心到准星的距离。
double distanceTo(const Candidate& c, const Vec2& cross)
{
    const Vec2 d = c.box.center() - cross;
    return d.norm();
}

} // namespace

TargetSelection selectTarget(const std::vector<Candidate>& candidates,
                             const std::vector<size_t>& aimIndices,
                             const Vec2& cross,
                             const SelectorConfig& cfg,
                             SelectorState& state)
{
    TargetSelection result;

    // ── 1. 找出"最近"的候选（同时受 maxDistancePx 粗筛） ────────────────
    bool haveNearest = false;
    size_t nearestIdx = 0;
    double nearestDist = 0.0;

    for (size_t idx : aimIndices)
    {
        const Candidate& c = candidates[idx];
        const double d = distanceTo(c, cross);
        if (cfg.maxDistancePx > 0.0 && d > cfg.maxDistancePx)
            continue;
        if (!haveNearest || d < nearestDist)
        {
            haveNearest = true;
            nearestIdx = idx;
            nearestDist = d;
        }
    }

    if (!haveNearest)
    {
        // 本帧没有可瞄目标 —— 锁定失效，下游必须复位滤波状态。
        state.reset();
        result.found = false;
        return result;
    }

    // ── 2. 滞回：上一帧锁定的目标还在，且没有明显更近的就保持 ────────────
    size_t chosenIdx = nearestIdx;
    if (state.locked)
    {
        // 在候选里找"和锁定框是同一个目标"的那个。
        // ★ 判据用中心距离 + 尺寸相似度，与 ② 稳定器的"认目标"同源。
        //   这里先用一个宽松的判定：中心距离小于锁定框对角线的一半，
        //   且尺寸比在 [0.5, 2.0] 内。
        //   ★ 阈值本身待实测（方案 §7 第 6 条），此处是占位实现。
        bool lockedStillPresent = false;
        size_t lockedIdx = 0;
        double lockedDistToCross = 0.0;
        for (size_t idx : aimIndices)
        {
            const Candidate& c = candidates[idx];
            const double cd = (c.box.center() - state.lockedBox.center()).norm();
            const double lockDiag = state.lockedBox.diagonal();
            if (lockDiag <= 0.0) break;
            if (cd > lockDiag * 0.5) continue;

            const double areaA = c.box.area();
            const double areaB = state.lockedBox.area();
            if (areaA <= 0.0 || areaB <= 0.0) continue;
            const double ratio = areaA / areaB;
            if (ratio < 0.5 || ratio > 2.0) continue;

            lockedStillPresent = true;
            lockedIdx = idx;
            lockedDistToCross = distanceTo(c, cross);
            break;
        }

        if (lockedStillPresent)
        {
            // 切换条件是"新目标明显更近"。k=1 就是纯最近（无滞回）。
            if (nearestDist * cfg.hysteresisRatio < lockedDistToCross)
                chosenIdx = nearestIdx;      // 明显更近 ⇒ 切换
            else
                chosenIdx = lockedIdx;       // 否则保持锁定 ⇒ 这就是滞回
        }
        // lockedStillPresent == false ⇒ 锁定目标已经没了，走最近的那个。
    }

    // ── 3. 落定 ─────────────────────────────────────────────────────────
    const Candidate& chosen = candidates[chosenIdx];
    result.found = true;
    result.index = chosenIdx;
    result.box = chosen.box;
    result.classId = chosen.classId;
    result.confidence = chosen.confidence;
    result.distancePx = distanceTo(chosen, cross);

    state.locked = true;
    state.lockedBox = chosen.box;
    state.lockedClassId = chosen.classId;
    ++state.lockedFrames;

    return result;
}

} // namespace control
