#pragma once

// ① 筛选 + 选靶
//
// 流程：
//   按 Delete / Filter / Aim 桶筛 → 只留 Aim → 选一个"距准星最近"的
//   ★ 带滞回：已锁定 T 时，新候选 C 必须比 T 近 k 倍才切换
//
// ★★ 滞回不是优化，是"滤波能不能起作用"的前提：
//   两个目标交替成为"最近"时，无滞回会每帧换目标 ⇒ ② 稳定器认目标反复失败、
//   ③ 的滤波状态每帧重置 ⇒ 滤波等于不存在。
//
// ★ 本文件零依赖（不引 OpenCV / Windows / Qt）。

#include "types.h"

#include <vector>

namespace control {

// 类别桶。★ 三者的语义必须区分（旧实现里 Filter 是死的：
// applyDeleteBucketFilter 只处理 Delete，Filter 配了没反应）：
//   Delete = 永远不考虑（队友、场景物件）
//   Filter = 参与检测但不瞄准（想看见但不想锁）
//   Aim    = 可瞄候选
enum class Bucket
{
    Aim,      // 可瞄
    Filter,   // 可见但不瞄
    Delete,   // 完全排除
};

// 类别 → 桶 的映射。调用方（逐游戏配置 L0）负责填。
struct ClassBuckets
{
    // 类别的桶。下标是 classId；越界视为 Delete（安全默认：未知类别不瞄）。
    std::vector<Bucket> byClassId;

    Bucket bucketOf(int classId) const
    {
        if (classId < 0 || static_cast<size_t>(classId) >= byClassId.size())
            return Bucket::Delete;
        return byClassId[static_cast<size_t>(classId)];
    }
};

struct SelectorConfig
{
    // 滞回倍数（D9）。已锁定目标 T 时，候选 C 必须满足
    //   dist(C) * hysteresisRatio < dist(T)
    // 才切换。= 1.0 表示无滞回（每帧选最近）。
    // ★ 取值待实测（方案 §7 第 5 条，建议从 1.3 起）。
    double hysteresisRatio = 1.3;

    // 距准星超过这个距离（检测像素）的候选不参与选择。
    // <= 0 表示不限制。
    // ★ 这是 FOV 椭圆的粗筛；真正的 FOV 判断在 L0 做，这里是兜底。
    double maxDistancePx = 0.0;
};

// 选靶状态（跨帧保持，滞回需要它）。
struct SelectorState
{
    // 当前锁定的目标框（检测像素）。locked == false 时无意义。
    Box lockedBox;
    int lockedClassId = -1;
    bool locked = false;
    // 锁定了多久（帧）。仅用于调试/日志。
    int lockedFrames = 0;

    void reset()
    {
        locked = false;
        lockedFrames = 0;
        lockedClassId = -1;
        lockedBox = Box{};
    }
};

// 选靶结果。
struct TargetSelection
{
    bool found = false;         // false ⇒ 本帧没有可瞄目标
    size_t index = 0;           // 在传入的 candidates 里的下标
    Box box;
    int classId = -1;
    double confidence = 0.0;
    double distancePx = 0.0;    // 到准星的距离（检测像素）
};

// 从推理输出里筛出 Aim 候选。
// ★ 保留 Filter 的判定入口：Filter 类别"可见但不瞄"，所以不进结果，
//   但仍会被 L0 用于显示（调用方自行取用）。
std::vector<size_t> filterAimCandidates(const std::vector<Candidate>& candidates,
                                        const ClassBuckets& buckets);

// 选靶 + 滞回。
//   cross  : 准星位置（检测像素）
//   state  : 跨帧状态，会被更新
// ★ 若上一帧锁定的目标仍在候选里，且没有别的候选比它"明显更近"，
//   则继续锁定它 —— 这就是滞回。
TargetSelection selectTarget(const std::vector<Candidate>& candidates,
                             const std::vector<size_t>& aimIndices,
                             const Vec2& cross,
                             const SelectorConfig& cfg,
                             SelectorState& state);

} // namespace control
