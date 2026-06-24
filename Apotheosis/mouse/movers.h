#ifndef MOUSE_MOVERS_H
#define MOUSE_MOVERS_H

// =============================================================================
// 鼠标移动控制器二选一 — 在 boss::AimEngine 输出 dx/dy 前接管。
//
//   Kind::Smooth      微澜 — 走 ART/path 原路径(EMA 自适应平滑),controller 不介入,
//                            engine 直接用 ArtResult/AimPathDriver 的 move。
//   Kind::Predictive  疾风 — 独立于 ART 的「带预测 PID」。直接读原始 anchor + bbox,
//                            自己估速、预测领先、位置式 P+D 出 dx/dy。box 占比自适应:
//                            大框(近) 压增益 / 加阻尼 / 大死区 / 单帧硬钳位 → 不过冲;
//                            小框(远) 抬增益 / 满预测领先 → 紧跟。bbox 适配全内部自动,
//                            不占用户旋钮。疾风从不调用 ART。
//
// 用户面 4 旋钮: 速度X (kp_x) / 速度Y (kp_y) / 阻尼 (kd) / 预测 (pred_weight)。
// =============================================================================

#include <algorithm>
#include <cmath>

namespace mover
{

enum class Kind : int
{
    Smooth     = 0,
    Predictive = 1,
};

struct Move
{
    int dx = 0;
    int dy = 0;
};

// 疾风 4 旋钮(沿用现有 config 字段,语义即 UI 标签)。
struct PredictiveParams
{
    double kp_x        = 0.6;   // 速度X — X 轴比例增益(收敛快慢)
    double kp_y        = 0.6;   // 速度Y
    double kd          = 0.10;  // 阻尼   — 微分阻尼(抑过冲 / 抑抖)
    double pred_weight = 0.5;   // 预测   — 速度前瞻领先强度(0 = 纯 PID)
};

// =============================================================================
// 疾风 — 独立 bbox 自适应「带预测 PID」。
//
// 每个观测帧 step(anchor, crosshair, bbox, image_size, dt, id):
//   1. vel  = EMA(anchor 位移 / dt)              目标速度(检测 px/s)
//   2. led  = anchor + vel · lead                预测领先点
//   3. err  = led − crosshair
//   4. u    = kp_eff·err + kd_eff·d(err)/dt_norm  位置式 P + D
//   5. |u| ≤ |err|·overshoot                      不过冲硬钳位(大框 = 1.0,绝不冲过)
//   6. 渐入 · 输出钳位 · 死区 · 亚像素残差 → 整数 dx/dy
//   7. 锁切 / teleport / 掉帧重捕 → 重播种,避免速度尖刺与微分瞬冲
//
// bbox 适配(内部自动,t_small ∈ [0,1],1 = 远小框 0 = 近大框):
//   kp_eff   = kp   · lerp(kGainBig,  kGainSmall,  t_small)
//   kd_eff   = kd   · lerp(kDampBig,  kDampSmall,  t_small)
//   lead     = pred · kLeadBaseSec · lerp(kLeadBig, kLeadSmall, t_small)
//   死区     = diag · lerp(kDzFracBig,   kDzFracSmall,   t_small)
//   钳位系数 =        lerp(kOvershootBig, kOvershootSmall, t_small)
// =============================================================================
class PredictiveMover
{
public:
    void reset();
    void configure(const PredictiveParams& p);

    // anchor / crosshair / bbox 均为检测像素;image_size = detection_resolution。
    Move step(double anchor_x, double anchor_y,
              double cross_x,  double cross_y,
              double bbox_w,   double bbox_h,
              double image_size, double dt, int track_id);

private:
    // ── bbox 自适应系数(near/big ←→ far/small)──
    static constexpr double kSizeFracLo    = 0.12;   // diag/image ≤ 此 → 全小框 (t_small=1)
    static constexpr double kSizeFracHi    = 0.50;   // diag/image ≥ 此 → 全大框 (t_small=0)
    static constexpr double kGainBig       = 0.55;   // 大框增益系数(压低 → 不过冲)
    static constexpr double kGainSmall     = 1.35;   // 小框增益系数(抬高 → 紧跟)
    static constexpr double kDampBig       = 1.60;   // 大框阻尼系数(更稳)
    static constexpr double kDampSmall     = 0.80;   // 小框阻尼系数(更跟手)
    static constexpr double kLeadBaseSec   = 0.05;   // 预测前瞻基准(秒)
    static constexpr double kLeadBig       = 0.40;   // 大框前瞻系数(少领先)
    static constexpr double kLeadSmall     = 1.00;   // 小框前瞻系数(满领先)
    static constexpr double kDzFracBig     = 0.025;  // 大框死区 = diag × 此
    static constexpr double kDzFracSmall   = 0.010;  // 小框死区
    static constexpr double kOvershootBig  = 1.00;   // 大框单帧 |u| ≤ |err| × 1.0(绝不冲过)
    static constexpr double kOvershootSmall= 1.50;   // 小框允许冲 1.5×(更跟手)

    // ── 通用常量 ──
    static constexpr double kDzMinPx    = 1.5;        // 死区下限(px)
    static constexpr double kOutMaxPx   = 120.0;      // 单帧输出上限(px)
    static constexpr double kVelCutHz   = 6.0;        // 估速 EMA 截止(Hz)
    static constexpr double kVelClampPx = 4000.0;     // 估速钳位(px/s,防尖刺)
    static constexpr double kDtNorm     = 1.0 / 240.0;// 微分归一化基准(240FPS)
    static constexpr double kSnapDiagMul= 2.5;        // anchor 跳变 > diag×此 → 重播种
    static constexpr double kSnapImgFrac= 0.50;       // 或跳变 > image×此 → 重播种
    static constexpr double kRampSec    = 0.08;       // 锁后渐入时长
    static constexpr double kInitScale  = 0.40;       // 渐入起点 scale

    static double lerp(double a, double b, double t) { return a + (b - a) * t; }
    static double clampd(double v, double lo, double hi)
    { return v < lo ? lo : (v > hi ? hi : v); }
    static double ema_alpha(double fc, double dt)
    { const double r = 2.0 * 3.14159265358979323846 * fc * dt; return r / (r + 1.0); }

    PredictiveParams params_{};

    int    last_track_id_ = -1;
    bool   has_prev_      = false;
    double prev_ax_ = 0.0, prev_ay_ = 0.0;     // 上帧 anchor(估速用)
    double vel_x_   = 0.0, vel_y_   = 0.0;      // 平滑速度
    double prev_err_x_ = 0.0, prev_err_y_ = 0.0;// 上帧 led-err(微分用)
    double rx_ = 0.0, ry_ = 0.0;               // 亚像素残差
    double lock_age_sec_ = 0.0;                 // 锁后时长(渐入用)
};

} // namespace mover

#endif // MOUSE_MOVERS_H
