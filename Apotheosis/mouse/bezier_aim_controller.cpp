#include "bezier_aim_controller.h"

#include <algorithm>
#include <cmath>

namespace aim
{

namespace
{

inline void sampleCubicBezier(double p, const BezierCurve& c,
                              double& outX, double& outY) noexcept
{
    p = std::clamp(p, 0.0, 1.0);
    const double u  = 1.0 - p;
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double p2 = p * p;
    const double p3 = p2 * p;
    // P0=(0,0), P3=(1,0)
    outX = 3.0 * u2 * p * c.cx1 + 3.0 * u * p2 * c.cx2 + p3;          // P0_x=0, P3_x=1
    outY = 3.0 * u2 * p * c.cy1 + 3.0 * u * p2 * c.cy2;               // P0_y=0, P3_y=0
    (void)u3;
}

} // namespace

std::pair<double, double> BezierTrajectoryController::computeError(
    double targetX, double targetY,
    double pivotX, double pivotY,
    int target_id) noexcept
{
    const double dx_to_target = targetX - pivotX;
    const double dy_to_target = targetY - pivotY;
    const double dist_to_target = std::hypot(dx_to_target, dy_to_target);

    // Engage / re-anchor 条件:
    //  - 第一次 step
    //  - target_id 已知且变化
    //  - 锚定 target 与当前 target 距离 > 阈值
    //  - 锚定段长度 < 1 像素 (退化,无方向可言)
    bool need_engage = !engaged_;
    if (engaged_ && target_id >= 0 && target_id != last_target_id_)
        need_engage = true;
    if (engaged_)
    {
        const double drift = std::hypot(targetX - anchor_target_x_,
                                        targetY - anchor_target_y_);
        if (drift > params_.reanchor_threshold_px)
            need_engage = true;
        const double anchor_len = std::hypot(anchor_target_x_ - anchor_start_x_,
                                             anchor_target_y_ - anchor_start_y_);
        if (anchor_len < 1.0)
            need_engage = true;
    }

    if (need_engage)
    {
        anchor_start_x_ = pivotX;
        anchor_start_y_ = pivotY;
        anchor_target_x_ = targetX;
        anchor_target_y_ = targetY;
        engaged_ = true;
        last_target_id_ = target_id;
    }
    else
    {
        // 一阶低通跟随最新观测,保形不脱靶。
        const double a = std::clamp(params_.follow_alpha, 0.0, 1.0);
        anchor_target_x_ = (1.0 - a) * anchor_target_x_ + a * targetX;
        anchor_target_y_ = (1.0 - a) * anchor_target_y_ + a * targetY;
        last_target_id_ = target_id >= 0 ? target_id : last_target_id_;
    }

    // 锚定段几何。
    const double seg_dx = anchor_target_x_ - anchor_start_x_;
    const double seg_dy = anchor_target_y_ - anchor_start_y_;
    const double seg_len = std::hypot(seg_dx, seg_dy);
    if (seg_len < 1e-3 || dist_to_target < 0.5)
    {
        // 退化:直接走 Direct 兜底。
        return { dx_to_target, dy_to_target };
    }

    const double inv_len = 1.0 / seg_len;
    const double dir_x =  seg_dx * inv_len;
    const double dir_y =  seg_dy * inv_len;
    const double perp_x = -dir_y;
    const double perp_y =  dir_x;

    // 进度 p:把 pivot 投影到锚段。
    const double rel_x = pivotX - anchor_start_x_;
    const double rel_y = pivotY - anchor_start_y_;
    double p = (rel_x * dir_x + rel_y * dir_y) * inv_len;
    p = std::clamp(p, 0.0, 1.0);

    // 采样曲线 (归一化空间)。
    double bx = 0.0, by = 0.0;
    sampleCubicBezier(p, params_.curve, bx, by);

    // 还原到检测图世界坐标。
    const double desired_x = anchor_start_x_
        + bx * dir_x  * seg_len
        + by * perp_x * seg_len;
    const double desired_y = anchor_start_y_
        + bx * dir_y  * seg_len
        + by * perp_y * seg_len;

    return { desired_x - pivotX, desired_y - pivotY };
}

} // namespace aim
