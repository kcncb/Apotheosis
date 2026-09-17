#ifndef RUNTIME_AIM_LOOP_H
#define RUNTIME_AIM_LOOP_H

// 通用控制器层的运行期接线（2026-09-17 第三轮重建）
//
// ★★ 这是把 Apotheosis/control/ 那套纯计算接进程序的唯一入口。
//    控制层本身零依赖（不引 OpenCV / Windows / Qt），本文件是它的"外壳"：
//    负责取检测、取准星、算 dt、把结果交给驱动。
//
// ── 节拍（用户决定 D4-d）────────────────────────────────────────────────
//   ★【检测线程：每出一批检测就跑一拍】。
//   dt 就是实际检测间隔，不是采集间隔 —— 因为控制误差来自检测，
//   重复喂同一个框会让滤波器误判"目标停住了"。
//
// ── 线程 ────────────────────────────────────────────────────────────────
//   tick() 由【检测线程】在发布 detectionBuffer 之后调用。
//   它读 crosshair_runtime 的快照（另一线程写），读 config 快照（原子）。
//
// ── 单位（铁律，docs/generic-controller-layer.md §1）─────────────────────
//   控制层内部只允许【检测图像素】与【整数鼠标计数】。
//   本文件在边界上做单位换算，**换算系数全部为 1**：
//   检测分辨率就是控制坐标系，不引入任何缩放。

#include <utility>
#include <vector>

#include "control/aim_controller.h"

// ★★ 这两个前向声明必须在【全局命名空间】—— 不能写在下面的
//    namespace runtime::aim_loop 里面。
//    写在里面就变成 `runtime::aim_loop::HotkeyProfile`, 与 config.h 里的
//    `::HotkeyProfile` 是【两个不同的类型】, 编译报的是
//    "member access into incomplete type"(而且指向 config.h, 很误导)。
//    ★ 这是实测踩到的坑。
struct HotkeyProfile;
struct ClassFilterState;

namespace runtime::aim_loop
{

// 一批检测发布后调用一次。
//
// ★ detections_are_fresh / crosshair_fresh 由调用方判定（detectionBuffer
//   的 staleLocked / 找色的 kFreshnessMs）。这里不做连续降权 ——
//   用户明确决定不做（"无法区分该帧是否是 200ms 前的，就当所有帧都能及时处理"）。
//
// ★★ 返回 true 表示本拍真的下发了位移。
//    返回 false 可能是：控制未启用 / 无目标 / 不新鲜 / 算出来是 0。
bool tick();

// 会话启停时调用：复位滤波/PID/锁定状态。
// ★ 必须在 stop 时调用 —— 否则下次 start 会带着上次的积分与速度估计，
//   表现为"刚开瞄准就冲一下"。
void reset();

// 是否有控制器实例（调试用）。
bool active();

// ── 纯映射（无外部依赖, 供逻辑测试直接钉）──────────────────────────────────
// ★ 为什么单独抽出来: "配置 → 控制器" 的映射漏一处就是【静默失效】
//   (界面能改、跑起来没变), 不报错、不留痕 —— 这类 bug 只能靠测试挡。
// ★ 它的入参刻意是一组扁平数值而不是 HotkeyProfile, 这样本机测试不必
//   拖进 config.h(那份头文件引 OpenCV, 在非 Windows 上编不了)。
struct FlatConfig
{
    // 六个增益（全部分方向）
    double kpX = 35.0, kpY = 35.0;
    double kiX = 0.0, kiY = 0.0;
    double kdX = 0.0, kdY = 0.0;
    // 时间常数与限幅
    double tauUnwindSec = 0.030;
    double tauDerivSec = 0.020;
    double iMax = 0.0;
    int    maxOutputCounts = 200;
    double pFullScalePx = 0.0;
    // 瞄点与滞回
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
    double hysteresisRatio = 1.3;
    double maxDistancePx = 0.0;      // 0 = 不限制
    int    randomSeed = 0;           // 0 = 用内部固定常数

    // 检测稳定器（②）5 项 —— 此前没有配置槽位，等于写死在代码里
    double matchCenterRatio = 0.5;
    double areaRatioTol = 2.0;
    double kSnapMult = 1.15;
    double minAspect = 0.2;
    double maxAspect = 5.0;
    // 类别桶的来源。★ 两个来源【合并】成一个桶数组（见 toControllerConfig）:
    //   aimClassIds  —— hotkeys[].aim_classes, 逐热键的"瞄准优先级列表"
    //   classFilters —— config.class_filters, TargetPage 维护的【全局】类别桶
    //
    // ★★ 为什么必须合并（这是"前后端对不上"的核心）:
    //   TargetPage 写的是 class_filters, 而控制器原来只读 aim_classes ——
    //   于是【用户在界面上设的类别, 控制器完全看不到】。
    //   两个来源都读, 界面改的东西才会真的作用到控制器上。
    std::vector<int> aimClassIds;

    // 全局类别桶: 每个元素 {classId, bucket}, bucket ∈ {0=Delete, 1=Filter, 2=Aim}
    // ★ 与 config.h 的 ClassBucket 数值一致（Delete=0 / Filter=1 / Aim=2）。
    //   这里用 int 而不是那个枚举, 是为了让本文件不依赖 config.h（那是 OpenCV 侧）。
    std::vector<std::pair<int, int>> classFilters;
    // 检测分辨率（用于找色失效时退回画面中心）
    int detectionResolution = 320;
};

// 扁平配置 → 控制器配置。★ 纯函数, 无副作用。
control::ControllerConfig toControllerConfig(const FlatConfig& flat);

// ── HotkeyProfile → FlatConfig ─────────────────────────────────────────────
// ★★ 为什么放在【这个】TU（而不是 aim_loop.cpp）:
//   它是 22 条逐字段赋值 + class_filters 搬运, 而"漏一条"的表现是
//   【静默失效】—— 界面上那个参数改了没反应, 不报错、不留痕。
//   aim_loop.cpp 要 include OpenCV/Windows, 本机编不了 ⇒ 放那里等于零覆盖。
//   ★ 实测: 把 flattenProfile 留在 aim_loop.cpp 时做过反向变异
//     ("不读 classFilters" / "不回填 maxDistancePx"), 结果【全绿】——
//     因为测试根本没编到那个文件。搬到本 TU 后同样两处变异都会变红。
FlatConfig flattenProfile(const HotkeyProfile& hk, int detectionResolution,
                          const std::vector<ClassFilterState>& classFilters);

// 由 aim_classes 的 classId 列表算桶数组（下标 = classId, 1 = Aim / 0 = Delete）。
std::vector<int> buildClassBuckets(const std::vector<int>& aimClassIds);

// ── dt 合法性门禁 ──────────────────────────────────────────────────────────
//
// ★★ 为什么把判据放在这里（而不是 aim_loop.cpp 里的一个 inline if）:
//   aim_loop.cpp 编不了（它 include OpenCV），而"dt 不合规时怎么办"是一条
//   【真实行为】—— 判据留在编不了的文件里就等于没覆盖。
//
// ★ 上限 250ms: 超过它说明"上一拍是陈年旧事"。拿它当 dt 会让积分项拿到
//   一个荒谬的大 dt，微分项则被压平 —— 两个方向都是错的。
//   宁可不输出，也不要一次错误的大跳。
// ★ 下限 1ms: 两个检测几乎同时发布时防除零。
inline constexpr double kMinDtSec = 0.001;
inline constexpr double kMaxDtSec = 0.250;

// dt 是否可信（可用来算积分/微分）。
inline bool dtIsUsable(double dtSec)
{
    return dtSec >= kMinDtSec && dtSec <= kMaxDtSec;
}

} // namespace runtime::aim_loop

#endif // RUNTIME_AIM_LOOP_H
