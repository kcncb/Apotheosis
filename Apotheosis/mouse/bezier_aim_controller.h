#ifndef MOUSE_BEZIER_AIM_CONTROLLER_H
#define MOUSE_BEZIER_AIM_CONTROLLER_H

#include <utility>

namespace aim
{

struct BezierCurve
{
    // 归一化控制点。P0=(0,0), P3=(1,0) 固定。P1/P2 可拖。
    double cx1 = 0.30;
    double cy1 = 0.25;
    double cx2 = 0.70;
    double cy2 = -0.15;
};

struct BezierParams
{
    BezierCurve curve{};
    double speed_x = 0.6;
    double speed_y = 0.6;
    // 一阶低通跟随系数。越大越快地把锚定 target 拉到最新观测,越小越保形。
    double follow_alpha = 0.15;
    // 锚定 target 与最新 target 距离超过该阈值时强制重锚。
    double reanchor_threshold_px = 60.0;
};

// 不直接包含 mouse.h,父对象自己持有该控制器并通过 step() 拿到原始 errX/errY,
// 复用现有 AimController 的 speed/residual 路径输出整数像素步长。
class BezierTrajectoryController
{
public:
    void setParams(const BezierParams& p) noexcept { params_ = p; }
    const BezierParams& params() const noexcept { return params_; }

    // 重置:清空锚定状态。下一次 step() 会以当前 pivot/target 重新 engage。
    void reset() noexcept
    {
        engaged_ = false;
        anchor_target_x_ = 0.0;
        anchor_target_y_ = 0.0;
    }

    // 输入: 当前 target / pivot 像素坐标 (检测图空间)。
    // 输出: 替代 (target - pivot) 的 (errX, errY)。父调用者把它喂给已有的
    // P 控制器残差累积逻辑得到整数像素步。target_id 用来检测锁定切换:
    // 不同 ID 强制重锚。传 -1 表示未知,基于距离阈值兜底。
    std::pair<double, double> computeError(double targetX, double targetY,
                                           double pivotX, double pivotY,
                                           int target_id = -1) noexcept;

private:
    BezierParams params_{};
    bool   engaged_ = false;
    int    last_target_id_ = -1;
    // 锚定: 起点 + 当前(平滑后)目标。轴方向每帧从这俩点重算。
    double anchor_start_x_ = 0.0;
    double anchor_start_y_ = 0.0;
    double anchor_target_x_ = 0.0;
    double anchor_target_y_ = 0.0;
};

} // namespace aim

#endif // MOUSE_BEZIER_AIM_CONTROLLER_H
