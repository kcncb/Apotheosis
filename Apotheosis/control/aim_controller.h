#pragma once

// L1+L2 编排器 —— 把 ①~⑥ 串成一条链
//
//  ① 筛选 + 选靶（最近 + 滞回）
//  ② 检测稳定器（认目标 + 剔除异常框，★ 不滤波）
//  ③ 滤波（α-β 默认 ┃ Kalman 可选）★ 全链路唯一一处滤波
//  ④ 瞄点 = 滤波后中心点 + y偏移；④b xy 中轴
//  ⑤ PID（六增益分方向）
//  ⑥ 限幅 → 单次量化 + 余量结转
//
// ★★ 本层【不碰驱动、不碰采集、不碰找色】—— cross 由调用方传进来。
//    这样整条链在 macOS 上可编译、可单测。

#include "anchor.h"
#include "filter.h"
#include "pid_controller.h"
#include "selector.h"
#include "stabilizer.h"
#include "types.h"

#include <memory>
#include <vector>

namespace control {

// ★★ 逐类别的瞄点覆盖（2026-09-17 第四轮续）。
//
//   背景：旧界面按【每个类别】各设一个 Y 锁点范围（默认 0.65 = 上半身/头颈），
//   而本轮重建时把它压成了热键级一对 ctl_y_offset。那会丢掉一个真实需求 ——
//   同一个热键同时瞄 head 和 body 时，两者该瞄的框内位置本来就不一样。
//
//   ★ 语义：按下表查到的类别【覆盖】热键级的 yOffset/yOffsetMax；
//     查不到就退回热键级。这样"只在需要时逐类覆盖"，不是两套并列真相。
//   ★ 只带 y_offset 两项：min_conf 不在这里 —— 它管的是【选靶门槛】，
//     属于 selector 的职责，塞进瞄点结构会让两个阶段的责任混在一起。
struct ClassAimPoint
{
    int    classId = -1;
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
};

struct ControllerConfig
{
    ClassBuckets buckets;
    SelectorConfig selector;
    StabilizerConfig stabilizer;
    AimPointConfig aimPoint;
    PidConfig pid;

    // 逐类别瞄点覆盖。空 = 全部走 aimPoint。
    // ★ 用 vector 而不是 map：类别数只有几十个，线性查找比哈希快且无分配抖动。
    std::vector<ClassAimPoint> classAimPoints;

    // 新鲜度门禁（二值，§3.3 第 1/2 条）。调用方每帧把 "是否新鲜" 传进来。
    // ★ 这里不做"连续降权" —— 用户明确决定不做（"无法区分该帧是否是 200ms 前的"）。
    bool requireFreshDetection = true;
    bool requireFreshCrosshair = true;
};

// 一拍的输入。
struct ControlInput
{
    std::vector<Candidate> candidates;   // 推理输出（未经筛选）
    Vec2 cross;                          // 准星（检测像素）
    double dtSec = 0.0;                  // 与上一拍的间隔（秒）
    uint64_t frameIndex = 0;             // 用于 y_offset 随机与可复现
    bool detectionFresh = true;          // 检测是否新鲜（调用方判定）
    bool crosshairFresh = true;          // 找色是否新鲜（调用方判定）
};

// 一拍的输出。
struct ControlOutput
{
    bool engaged = false;        // 是否真的在控制（false ⇒ counts 恒为 0）
    Counts counts;
    Vec2 anchor;
    Vec2 cross;
    Vec2 error;
    // ★ 本拍锁定目标的框（检测像素）。★ 2026-09-17 新增: 自动扳机要用它算
    //   命中区 —— 命中区的判据是"准星是否落在【框】的某个区间里", 所以
    //   下游必须有这个框。engaged=false 或无目标时 hasTarget=false。
    Box targetBox{};
    bool hasTarget = false;
    // ★ 目标身份的稳定编号。选靶层的"锁定目标"在跨帧延续时这个值不变,
    //   换目标才变 —— 扳机的转火冷却靠它判定。
    //   （选靶层没有 track_id 概念, 这里由本层按"锁定框是否延续"生成。）
    int targetId = -1;
    // 诊断：为什么没出力。调参时最需要的就是这个。
    enum class IdleReason
    {
        None = 0,            // 正常输出
        NoCandidates,        // 没有可瞄目标
        StaleDetection,      // 检测不新鲜
        StaleCrosshair,      // 找色不新鲜
        RejectedByStabilizer,// 被稳定器剔除（形状离谱）
        BadDt,               // dt 非法
    };
    IdleReason idleReason = IdleReason::None;
};

class AimController
{
public:
    AimController();
    ~AimController();

    // ★★ 这个方法必须【同时】把 PID 部分转交给 pid_ ——
    //    只赋值 cfg_ 是不够的: pid_ 是独立对象, 它持有自己的配置副本。
    //    实测事故(2026-09-17 接线时抓到): 只写 cfg_ = cfg 时, 六个增益
    //    全部被忽略, PID 一直用默认值跑 —— 配置改了【完全没反应】,
    //    不报错、不留痕。回归: tests/control_wiring_test.cpp 的
    //    "Kp 大 ⇒ 输出大" 那条就是钉它的。
    void setConfig(const ControllerConfig& cfg)
    {
        cfg_ = cfg;
        pid_.setConfig(cfg.pid);
    }
    const ControllerConfig& config() const { return cfg_; }

    // ★ 滤波器注入点：默认 α-β。要换 Kalman 就传一个 IFilter 进来。
    //   传 nullptr 恢复默认 α-β。
    void setFilter(std::unique_ptr<IFilter> filter);

    // ★★ 二选一保护：若调用方试图"串联"（例如既保留内部 α-β 又外挂一个
    //    Kalman 并期望两者都生效），本接口的设计就是【只保留一个实例】——
    //   没有"追加"这个操作。这是刻意的（§3.5.1 禁止串联）。
    IFilter* filter() const { return filter_.get(); }

    ControlOutput update(const ControlInput& in);

    // 换目标 / 外部事件时手动复位。
    void reset();

    // 调试读数透传。
    const ControlTelemetry& telemetry() const { return pid_.telemetry(); }
    const SelectorState& selectorState() const { return selectorState_; }
    const StabilizerState& stabilizerState() const { return stabilizerState_; }

private:
    ControllerConfig cfg_;
    std::unique_ptr<IFilter> filter_;
    SelectorState selectorState_;
    StabilizerState stabilizerState_;
    PidController pid_;

    // 上一次实际喂给滤波器的框（用于判断"框变了没"）。
    bool hasLastBox_ = false;
    Box lastBox_;

    // ★ 目标身份计数器。选靶层没有 track_id 概念, 这里按稳定器的判决生成:
    //   Snap / NoHistory = 换目标(或首次) ⇒ 推进;  Ok = 延续 ⇒ 不变。
    //   自动扳机的转火冷却挂在这个编号的变化上。
    int targetIdCounter_ = 0;
};

} // namespace control
