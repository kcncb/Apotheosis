#ifndef MOUSE_BOSS_AIM_H
#define MOUSE_BOSS_AIM_H

// 目标关联 + 瞄点 + 控制�?
//
// 这一层负责三件事:
//   1. 【锁谁�?   —�?target_selector (ava_exact)
//   2. 【瞄哪一点】—�?target_to_aimpoint (ava_exact)
//   3. 【怎么动�? —�?engine::AimPid, �?mouse/aim_pid.h
//
// �?AVA PIDF 整条管线(pidf_mode1/mode2、postprocess、update、axis_policy�?
// aim_movement_pipeline、controller_orchestration、pid_input、qx_curve�?
// process_humanization 以及热键旁路)已整条删�? �?mouse/aim_pid.h 的新控制�?
// 取代, 接入点在 boss_aim.cpp �?tick()�?

#include <memory>
#include <vector>

#include <opencv2/opencv.hpp>

#include "aim_motion.h"
#include "aim_pid.h"
#include "anchor_observer.h"
#include "ava_exact/target_selector_top_exact.hpp"
#include "ava_exact/target_to_aimpoint_exact.hpp"

namespace boss
{

// ── 运动观测�?+ 死区补偿的常�?不是用户旋钮) ───────────────────────────────
// 位置: 不平�?0ms) —�?控制器吃原始瞄点, "甩到瞄点"永远是一拍的事�?
// 速度: 只喂前馈, 走输入延迟扣�?+ 200ms 低�? 因为不进位置回路, 估错了也不会让准星抽�?
//
// ★★ 链路死区 kAimDeadTimeS —�?这是整个回路最关键的物理量, 实测出来�?
//   我们发出去的计数要过这么久才在【检测画面】里生效(HID + 游戏�?+ 显示 + 采集缓冲 +
//   推理)�?026-09-12 那场日志(chain_live.log)给了两个独立证据:
//     �?极限环周�? Kp=100 时误差以 5.00Hz/200ms 摆动 = 24 拍。离�?积分+纯延�?环路�?
//        振荡周期�?2(2d+1) �?=> d = 5.5 �?�?46ms; 临界每拍增益 g_crit = 2sin(π/(2(2d+1)))
//        = 0.256, 而实�?g = Kp*dt*k = 100*0.00833*0.593 = 0.494 = 1.9 倍临�?—�?正好解释
//        "Kp=30 稳、Kp=100 �?(Kp=30 �?g=0.148 < 0.256)�?
//     �?起始那几�? 连发 259 counts 期间画面里的瞄点纹丝不动, �?50ms 后才开始走�?
//   所以延迟预�?=盲区里目标走掉的距离)的合理值也是这个量�? �?在途自身位�?的窗�?
//   就是它�?
inline constexpr double kAimDeadTimeS = 0.046;

// 在途自身位移的补偿强度: 只补 80%�?
//   �?方向是不对称�?—�?补偿【不足】只是回到原来那个延�?安全), 补偿【过头】会�?
//   "已经生效的指�?再扣一�? 变成正反�?-> 发散。所以留 20% 余量�?
inline constexpr double kAimDeadTimeCompScale = 0.8;

inline constexpr double kAnchorObserverSmoothMs = 0.0;
inline constexpr double kAnchorObserverVelTauMs = 200.0;
inline constexpr double kAnchorObserverGatePx = 40.0;

struct Track
{
    int id = -1;
    cv::Rect2f bbox{};
    cv::Point2f anchor{};
    int missed = 0;
    bool alive = true;
    int class_id = -1;
    float confidence = 0.0f;
    bool observed_this_frame = false;
};

struct TargetSlot
{
    int class_id = -1;
    // 用户坐标�?=框底�?=框顶�?
    float y_offset_min = 0.5f;
    float y_offset_max = 0.5f;
    float min_conf = 0.0f;
};

struct EngineInput
{
    const std::vector<cv::Rect2f>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    const std::vector<float>* confidences = nullptr;
    std::vector<TargetSlot> target_slots;
    // 目标缓存已按需求删除(2026-09-12): 丢了就立刻释放。恒为 0, 不再有配置项。
    int lost_target_cache_frames = 0;  // 运行时固定 0

    double crosshair_x = 0.0;
    double crosshair_y = 0.0;
    double fov_radius_x = 0.0;
    double fov_radius_y = 0.0;
    double image_size = 0.0;

    // �?PID 参数: 每拍从当前热键配置快照填�?�?runtime/mouse_thread_loop.cpp)�?
    // 单位与取值见 mouse/aim_pid.h —�?Kp[计数/像素]、Ki[计数/(像素·�?]�?
    // Kd[计数·�?像素]、死区[像素]、限幅[计数/拍]�?
    AimPidParams pid_x{};
    AimPidParams pid_y{};

    // 每计数像�?px/count), 由热键配置手填�?0 = 直接用它(前馈立刻生效);
    // 0 = 仍然自动在线估算。见 config.h �?aim_px_per_count_x/y�?
    double px_per_count_x = 0.0;
    double px_per_count_y = 0.0;

    // 【删�?2026-09-12】原「瞄点滤�?anchor_filter_ms)」旋钮已移除�?
    //   理由: 位置一平滑, "甩到瞄点"就多�?3~4 �? 手感上就�?不跟�?; 而它想解决的
    //   "框在�?问题, 真正该做的是【只把速度滤掉�?控制器吃原始瞄点, 速度只喂前馈,
    //   速度不进位置回路)—�?�?mouse/anchor_observer.h �?aim_pid.cpp 的速度噪声门�?
};

struct EngineOutput
{
    bool have_target = false;
    int current_track_id = -1;
    cv::Point2f anchor{};       // 瞄点(框内按类别比例算出的那个�?
    cv::Rect2f bbox{};          // 用于日志/扳机判定的观测框
    cv::Rect2f observed_bbox{}; // �?bbox 相同, 保留独立字段以免调用方语义混�?
    int class_id = -1;

    bool coasting = false;          // 本拍用的�?tracker 的预测框而非新观�?
    bool motion_suppressed = false; // 本拍刚换了目标身�? 新控制器应重置信

    // ── �?PID 的接入点 ─────────────────────────────────────────────────────
    // 误差 = 瞄点 - 准星(单位: 检测图像素, 浮点), 输出 = 整数鼠标计数�?
    // 控制器全程在像素域用 double �? 整数只在出口出现一�? 取整零头会攒到下一�?
    // 所以亚计数的小误差不会被丢掉。见 mouse/aim_pid.h 的说明�?
    // 现役链路是计数域: 外面直接�?sendRawMove 发出, 不做像素<->计数换算�?
    //    · 换目标身�?motion_suppressed)/丢失目标�? 控制�?reset(), 观测�?
    //      resetTargetMotion()(保留已标定出�?每计数多少像�?)�?
    int dx = 0;
    int dy = 0;

    // ── 控制器遥�?只给日志/调试�? 不参与控�? ────────────────────────────
    // 误差单位像素, 前馈偏移单位像素, 速度单位像素/�? 每计数像素单位像�?计数�?
    bool   pid_ff_ready = false;      // 观测器是否已标定�?每计数多少像�?
    // 本拍是不是【真的换目标�? 身份变了 �?瞄点跳了 >=25px。仅供日志判断策略�?
    bool   target_switched = false;
    double target_anchor_jump_px = 0.0;  // 本拍瞄点相对上一拍的跳变�?像素)
    double pid_dt_ms = 0.0;           // 本拍实际用于输出缩放�?dt(已夹在典型拍间隔附近)
    double pid_error_px_x = 0.0;
    double pid_error_px_y = 0.0;
    double pid_used_error_px_x = 0.0; // 加了前馈、送给 P/I 的误�?
    double pid_used_error_px_y = 0.0;
    double pid_cmd_x = 0.0;           // 取整前的浮点指令(计数)
    double pid_cmd_y = 0.0;
    double pid_feedforward_px_x = 0.0;
    double pid_feedforward_px_y = 0.0;
    double pid_ff_scale_x = 0.0;      // 速度前馈的噪声门(0~1): 0 = 视为静止, 不提�?
    double pid_ff_scale_y = 0.0;
    double pid_pending_px_x = 0.0;    // 在途自身位移补�?Smith): 从误差里扣掉的像�?
    double pid_pending_px_y = 0.0;
    double pid_px_per_count_x = 0.0;
    double pid_px_per_count_y = 0.0;
    double pid_target_vel_x = 0.0;
    double pid_target_vel_y = 0.0;
    // 排查"抽一�?冲过�?要看的中间量: 各分项的像素当量 + 积分状�?+ 零头 + 限幅�?
    double pid_i_px_x = 0.0;
    double pid_i_px_y = 0.0;
    double pid_d_px_x = 0.0;
    double pid_d_px_y = 0.0;
    double pid_integral_x = 0.0;   // 像素·�?
    double pid_integral_y = 0.0;
    double pid_carry_x = 0.0;      // 未发出的计数零头
    double pid_carry_y = 0.0;
    int    pid_limit_x = 0;        // 本拍生效的输出上�?计数/�?
    int    pid_limit_y = 0;
    int    pid_fits_x = 0;         // 观测器已接受的有效拟合窗口数(0 = 还没标定出来)
    int    pid_fits_y = 0;
};

class AimEngine
{
public:
    AimEngine();
    ~AimEngine();

    void reset();
    EngineOutput tick(const EngineInput& in, double dt);
    int lockedTrackId() const { return current_id_; }
    const std::vector<Track>& tracks() const { return tracks_; }

private:
    bool selectorConfigChanged(const EngineInput& in) const;
    void rebuildSelector(const EngineInput& in);

    std::unique_ptr<cvm::recovered::AimTargetSelectorExact> selector_;
    cvm::recovered::AimPointState64Abi aimpoint_state_{};
    std::vector<TargetSlot> selector_slots_;
    int selector_lost_frames_ = -1;
    int selector_normalizer_ = -1;

    int current_id_ = -1;
    int last_generation_ = -1;
    std::vector<Track> tracks_;

    // X/Y 各一个控制器 + 一个目标运动观测器。误差是像素、输出是计数, 状�?积分/微分/
    // 零头)跨帧保留; 观测器在线估"每计数多少像�?与目标自身速度, 供前馈使用�?
    AimPid pid_x_;
    AimPid pid_y_;
    AnchorMotionEstimator motion_x_;
    AnchorMotionEstimator motion_y_;
    int last_counts_x_ = 0;  // 上一拍实际下发的计数(观测器要用它�?在途自身位�?)
    int last_counts_y_ = 0;
    // 上一拍的瞄点, 用来判断"身份变化"到底是真换目标还�?tracker 把同一个目标重锁一次�?
    cv::Point2f last_anchor_{};
    bool has_last_anchor_ = false;
    // 目标运动观测�?X/Y 各一�?: 平滑瞄点 + 估目标自身速度, �?anchor_observer.h�?
    AnchorObserver anchor_obs_x_;
    AnchorObserver anchor_obs_y_;
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
