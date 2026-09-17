#pragma once

// ③ 滤波 —— ★★ 全链路唯一一处滤波（D7）
//
// ★★★ 硬约束（docs/generic-controller-layer.md §3.5.1）：
//   Kalman **不是**"再加一道滤波"，而是 α-β 的**替代品**。
//   二者必须二选一，不允许串联。理由不是审美，是实测结论：
//   串联滤波各自引入滞后、参数还互相耦合，而它们能滤掉的东西完全一样。
//   而 α-β 本身就是"位置+速度"两状态的稳态 Kalman ⇒ "开 Kalman"的正确含义是
//   **用完整版换掉简化版**，不是"在简化版前面再加一个完整版"。
//
//   ⇒ 实现形态：同一个接口、同一个位置、只允许激活一个。
//     运行时若两个同时激活，**必须拒绝并报错**，不许静默叠加。
//
// ★ 平滑对象：稳定后框的**中心点**（2 个数），不是四个角/宽高。
//   点数越少越不容易调乱。

#include "types.h"

namespace control {

// 滤波器统一接口。两种实现：AlphaBeta（默认）/ KalmanFilter（可选）。
class IFilter
{
public:
    virtual ~IFilter() = default;

    // 喂入一个观测中心点（检测像素）。
    // ★ dtSeconds：与上一帧的间隔（秒）。首帧可传任意值（实现应忽略）。
    virtual void observe(const Vec2& center, double dtSeconds) = 0;

    // 当前滤波后的位置。未初始化时返回最近一次的观测。
    virtual Vec2 position() const = 0;

    // ★ 速度只是滤波器内部状态，**不外传**（control-architecture.md:74）。
    //   这里刻意【不提供】velocity() 接口 —— 曾经有过，就是靠它把速度接进
    //   控制器才出的问题。要读速度请先读 §3.2.2 再改这里。

    // 是否已经初始化。
    virtual bool initialized() const = 0;

    // ★ 硬重置：换目标 / 突变时必须调用。
    //   Kalman 状态残留会让新旧目标之间来回振荡 —— 这正是当年换掉 Kalman 的原因之一。
    virtual void reset() = 0;
};

} // namespace control
