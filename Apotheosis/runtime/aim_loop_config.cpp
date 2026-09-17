// 通用控制器层 —— "配置 → 控制器" 的纯映射
//
// ★★ 为什么从 aim_loop.cpp 拆出来:
//   aim_loop.cpp 要 include OpenCV / Windows / Qt（它读 detectionBuffer、
//   找色快照、MouseThread），所以在非 Windows 上【编不了】。
//   而本文件的映射逻辑是纯函数 —— 拆出来就能进逻辑测试。
//   ★ 漏映射的表现是"界面能改、跑起来没变"，不报错不留痕，
//     这类 bug 只能靠测试挡，所以它必须可测。
//


#include "runtime/aim_loop.h"

#include "config/config.h"   // HotkeyProfile / ClassFilterState

#include <algorithm>

namespace runtime::aim_loop
{

// ── 纯映射（实现见 aim_loop.h 的说明）────────────────────────────────────

std::vector<int> buildClassBuckets(const std::vector<int>& aimClassIds)
{
    int maxClassId = -1;
    for (int id : aimClassIds)
        maxClassId = std::max(maxClassId, id);
    if (maxClassId < 0)
        return {};

    // ★ 默认全 Delete —— "未知类别不瞄"是安全默认。
    std::vector<int> buckets(static_cast<size_t>(maxClassId) + 1, 0);
    for (int id : aimClassIds)
    {
        if (id >= 0)
            buckets[static_cast<size_t>(id)] = 1;
    }
    return buckets;
}

control::ControllerConfig toControllerConfig(const FlatConfig& flat)
{
    control::ControllerConfig cfg;

    // ── 类别桶（两源合并）────────────────────────────────────────────
    // ★ 三桶语义（方案 §3.4）: Aim 可瞄 / Filter 可见不瞄 / Delete 排除。
    //
    // ★★ 两个来源【都读】, 这是修掉"前后端对不上"的关键:
    //   · flat.classFilters —— TargetPage 维护的【全局】类别桶(class_filters)
    //   · flat.aimClassIds   —— 逐热键的 aim_classes 优先级列表
    //   合并规则（顺序很重要, 见下）:
    //     1. 先按 classFilters 铺全局桶
    //     2. 再用 aimClassIds 把其中出现的类别【提升】为 Aim
    //        （★ 只提升, 不降级 —— 逐热键写"Aim"是更具体的意图, 不该被全局 Delete 吃掉）
    //     3. aimClassIds 里超出 classFilters 范围的类别, 补进桶数组
    //     4. 两个来源都没有的类别 ⇒ Delete（安全默认: 未知类别不瞄）
    {
        int maxId = -1;
        for (const auto& cf : flat.classFilters)
            maxId = std::max(maxId, cf.first);
        for (int id : flat.aimClassIds)
            maxId = std::max(maxId, id);

        if (maxId >= 0)
        {
            cfg.buckets.byClassId.assign(static_cast<size_t>(maxId) + 1,
                                         control::Bucket::Delete);

            // 1) 全局桶
            for (const auto& cf : flat.classFilters)
            {
                if (cf.first < 0)
                    continue;
                switch (cf.second)
                {
                case 2: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Aim;    break;
                case 1: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Filter; break;
                default: cfg.buckets.byClassId[static_cast<size_t>(cf.first)] = control::Bucket::Delete; break;
                }
            }

            // 2) 逐热键只做"提升为 Aim"
            for (int id : flat.aimClassIds)
            {
                if (id >= 0)
                    cfg.buckets.byClassId[static_cast<size_t>(id)] = control::Bucket::Aim;
            }
        }
    }

    // ── 选靶 ──────────────────────────────────────────────────────────
    cfg.selector.hysteresisRatio = flat.hysteresisRatio;
    // ★ 0 = 不限制。距离门控目前由 FOV 椭圆承担，不在这里重复设第二道。
    cfg.selector.maxDistancePx = flat.maxDistancePx;

    // ── 稳定器（②）───────────────────────────────────────────────────
    // ★ 这 5 项此前【写死在 control/ 的默认值里、没有配置槽位】——
    //   等于谁都调不了。现在可调（⚠️ 数值仍全是待实测的占位，方案 §7 第 6 条）。
    cfg.stabilizer.matchCenterRatio = flat.matchCenterRatio;
    cfg.stabilizer.areaRatioTol     = flat.areaRatioTol;
    cfg.stabilizer.kSnapMult        = flat.kSnapMult;
    cfg.stabilizer.minAspect        = flat.minAspect;
    cfg.stabilizer.maxAspect        = flat.maxAspect;

    // ── 瞄点 ──────────────────────────────────────────────────────────
    cfg.aimPoint.yOffset = flat.yOffset;
    cfg.aimPoint.yOffsetMax = flat.yOffsetMax;
    cfg.aimPoint.randomSeed = flat.randomSeed;   // 0 = 用内部固定常数

    // ── PID ───────────────────────────────────────────────────────────
    cfg.pid.kpX = flat.kpX;
    cfg.pid.kpY = flat.kpY;
    cfg.pid.kiX = flat.kiX;
    cfg.pid.kiY = flat.kiY;
    cfg.pid.kdX = flat.kdX;
    cfg.pid.kdY = flat.kdY;
    cfg.pid.tauUnwindSec = flat.tauUnwindSec;
    cfg.pid.tauDerivSec = flat.tauDerivSec;
    cfg.pid.iMax = flat.iMax;
    cfg.pid.maxOutputCounts = flat.maxOutputCounts;
    cfg.pid.pFullScalePx = flat.pFullScalePx;
    // ★★ 这里【没有死区字段】—— 死区已整项删除（方案 §5.1）。
    //    不要因为"少了点什么"就加回来。

    // ── 新鲜度门禁 ────────────────────────────────────────────────────
    cfg.requireFreshDetection = true;
    cfg.requireFreshCrosshair = true;

    return cfg;
}

// HotkeyProfile → FlatConfig。这一层只做"搬字段", 不做任何判断 ——
// 判断全在 toControllerConfig 里（那样才测得动）。
FlatConfig flattenProfile(const HotkeyProfile& hk, int detectionResolution,
                          const std::vector<ClassFilterState>& classFilters)
{
    FlatConfig flat;
    flat.kpX = hk.ctl_kp_x;
    flat.kpY = hk.ctl_kp_y;
    flat.kiX = hk.ctl_ki_x;
    flat.kiY = hk.ctl_ki_y;
    flat.kdX = hk.ctl_kd_x;
    flat.kdY = hk.ctl_kd_y;
    flat.tauUnwindSec = hk.ctl_tau_unwind_sec;
    flat.tauDerivSec = hk.ctl_tau_deriv_sec;
    flat.iMax = hk.ctl_i_max;
    flat.maxOutputCounts = hk.ctl_max_output_counts;
    flat.pFullScalePx = hk.ctl_p_full_scale_px;
    flat.yOffset = hk.ctl_y_offset;
    flat.yOffsetMax = hk.ctl_y_offset_max;
    flat.hysteresisRatio = hk.ctl_hysteresis_ratio;
    flat.maxDistancePx = hk.ctl_max_distance_px;
    flat.randomSeed = hk.ctl_random_seed;
    flat.matchCenterRatio = hk.ctl_match_center_ratio;
    flat.areaRatioTol = hk.ctl_area_ratio_tol;
    flat.kSnapMult = hk.ctl_k_snap_mult;
    flat.minAspect = hk.ctl_min_aspect;
    flat.maxAspect = hk.ctl_max_aspect;
    flat.detectionResolution = detectionResolution;
    flat.aimClassIds.reserve(hk.aim_classes.size());
    for (const auto& ac : hk.aim_classes)
        flat.aimClassIds.push_back(ac.class_id);

    // ★★ 全局类别桶: TargetPage 写的就是它。
    //   不读它 ⇒ 用户在界面上设的类别对控制器【完全无效】(这就是原来的断层)。
    //   ★ ClassBucket 的数值(Delete=0/Filter=1/Aim=2)与 toControllerConfig 里
    //     的 case 标签一致, 所以直接 static_cast, 不做映射表。
    //   ★★ 但"一致"这件事是【隐含契约】—— 若有人给 ClassBucket 重排序,
    //      编译照样通过, 而 Filter 会静默变成 Aim(或反过来)。
    //      下面这组 static_assert 把契约变成编译期错误: 重排序会直接编不过。
    static_assert(static_cast<int>(ClassBucket::Delete) == 0, "ClassBucket::Delete 必须 = 0");
    static_assert(static_cast<int>(ClassBucket::Filter) == 1, "ClassBucket::Filter 必须 = 1");
    static_assert(static_cast<int>(ClassBucket::Aim)    == 2, "ClassBucket::Aim 必须 = 2");
    flat.classFilters.reserve(classFilters.size());
    for (const auto& cf : classFilters)
        flat.classFilters.emplace_back(cf.class_id, static_cast<int>(cf.bucket));

    return flat;
}

} // namespace runtime::aim_loop
