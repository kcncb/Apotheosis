#ifndef MOUSE_BOSS_AIM_H
#define MOUSE_BOSS_AIM_H

// 目标关联 + 瞄点 + 控制器 (PID-EventSync 单档, 移植 AimMagic 1.0.30 全链路)
//
// 这一层负责四件事:
//   1. 【锁谁】   —→ target_selector (ava_exact)
//   2. 【瞄哪一点】—→ target_to_aimpoint (ava_exact)
//   3. 【身份/速度/提前量】—→ AimTracker, 见 mouse/aim_tracker.h
//   4. 【怎么动】 —→ engine::AimPid, 见 mouse/aim_pid.h
//
// 旧 AVA PIDF 整条管线(pidf_mode1/mode2、postprocess、update、axis_policy、
// aim_movement_pipeline、controller_orchestration、pid_input、qx_curve、
// process_humanization 以及热键旁路)已整条删除, 由 mouse/aim_pid.h 的新控制器
// 取代, 接入点在 boss_aim.cpp 的 tick()。
//
// ★ 2026-09-16: 「经典 PID（现役）」档与它的全局 AimPredict 预测已删除, 只保留
//   PID-EventSync 这一套。原 aim_mode 分档开关也随之删除 —— 现在只有一条链路。

#include <memory>
#include <vector>

#include <opencv2/opencv.hpp>

#include "aim_pid.h"
#include "aim_scale.h"
#include "aim_tracker.h"
#include "anchor_filter.h"
#include "ava_exact/target_selector_top_exact.hpp"
#include "ava_exact/target_to_aimpoint_exact.hpp"

namespace boss
{

// ── 架构 (PID-EventSync, 移植 AimMagic 1.0.30) ──────────────────────────────
//
//   检测框 --(选靶)--> 【AimTracker: 身份/速度/每轨预测】--> 【α-β 锚点平滑】-->
//        【在途位移补偿(像素域 ÷ k̂)】--> PID --> 输出整形 --> 发送
//
// █ 各段职责 ██
//
//   跟踪器    : 给这一拍的选中框一个【跨帧稳定的身份】(IoU + 最近邻粘滞),
//               并用采样窗估速度、每轨一份预测状态机。换目标才换 id。
//   框平滑器  : 滤掉检测框的抖动(它每帧跳 1~2px, 在 8.3ms 里就是 230px/s 假速度)
//               → 喂给 PID 的信号干净了, Kp 才敢开大
//   在途补偿  : AM 的发送环 —— 已发出未生效的计数求和 ÷ k̂ 换回像素, 从误差里扣。
//   PID       : 高增益纯反馈。Kp 决定"跟得紧不紧", Ki 磨掉稳态滞后
//   P 项饱和  : 大误差段自动降增益 —— 甩枪/换靶不过冲, 且不像死区那样留盲区
//   输出限幅  : 兜住极端输出
//
// ██ 参数 ██
//
// 用户调: 追踪增益(Kp) / 积分增益(Ki) / 震荡抑制(Kd) / 饱和阈值 / 输出限幅,
// 外加跟踪器(min_hits/max_age/关联门限/速度窗)、预测(factor/宽度区间/上限/噪声门)
// 与 k̂(每计数像素, 手填)。平滑时间常数【故意不暴露】—— 它和 Kp 是耦合的。

// 框平滑时间常数(毫秒)。这是"平滑强度", 不是用户旋钮。
//
// 怎么定的: 检测框的抖动是【单帧孤立跳变】(实测框心逐帧变化中位 0.06px, 偶发 1.9px),
// 而目标的真实运动是【连续】的。取 30ms 意味着"约 3~4 拍之内的抖动会被平均掉, 而
// 超过 30ms 的持续运动会被当作真实运动跟上去"。α-β 带回速度状态, 所以匀速目标下
// 它【几乎不产生滞后】(这是它比一阶低通强的地方)。
//
// 太小(如 0) = 不平滑, 框抖直接进 PID; 太大(如 100ms) = 目标一动就落后, 得靠加 Kp 补。
inline constexpr double kAnchorFilterTauMs = 30.0;

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
    // 用户坐标系=框底，1=框顶）
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

    // 新PID 参数: 每拍从当前热键配置快照填充（runtime/mouse_thread_loop.cpp)。
    // 单位与取值见 mouse/aim_pid.h —— Kp[计数/像素]、Ki[计数/(像素·秒]）
    // Kd[计数·秒/像素]、死区[像素]、限幅[计数/拍]。
    AimPidParams pid_x{};
    AimPidParams pid_y{};

    // ── 距离尺度参数 (2026-09-13 新增, 见 mouse/aim_scale.h) ────────────────
    // 每拍从当前热键配置快照填入。全部为 0/负 => 尺度关闭, 行为与没有它逐位相同。
    // 唯一的距离代理是【检测框高】: 本项目双机架构下拿不到真实距离,
    // AimbotTarget::depth_at_pivot 是恒 -1 的占位常量, 不可用。
    AimScaleParams aim_scale{};

    // ── 跟踪器与预测参数 (移植 AimMagic 1.0.30 全链路) ─────────────────────
    AimTrackerParams esync{};

    // 本拍的单调时钟读数(秒)。跟踪器的速度采样窗按 **AM 的做法用时间戳比较**
    // (AM 的 tracking_velocity_sample_ms 是毫秒墙钟量), 所以必须给它一个
    // 单调时钟 —— 用"拍数 × dt"累加也可以, 但那样窗的边界会随 dt 抖动,
    // 而 AM 的语义是"两次观测的真实间隔超过 20ms 就重算"。
    // 调用方(mouse_thread_loop.cpp)填 std::chrono::steady_clock 的读数。
    double now_s = 0.0;
};

struct EngineOutput
{
    bool have_target = false;
    int current_track_id = -1;
    cv::Point2f anchor{};       // 瞄点(框内按类别比例算出的那个点)
    cv::Rect2f bbox{};          // 用于日志/扳机判定的观测框
    cv::Rect2f observed_bbox{}; // 与bbox 相同, 保留独立字段以免调用方语义混淆
    int class_id = -1;

    bool coasting = false;          // 本拍用的是tracker 的预测框而非新观测
    bool motion_suppressed = false; // 本拍刚换了目标身份 新控制器应重置信

    // ── 预测补偿遥测 (2026-09-13) ────────────────────────────────────────────
    // 只用于日志/界面显示, 不参与控制。predict_active 为 false 时三个量都无意义。
    float predict_lead_x = 0.0f;      // 本拍加在瞄点上的 X 偏移(像素)
    float predict_lead_y = 0.0f;      // 同上, Y
    float predict_size_weight = 0.0f; // 尺寸权重 (maxW-w)/(maxW-minW), 0..1
    bool predict_active = false;

    // ── 滑行外推遥测 (2026-09-16, 逐字移植 AM 的 coasting) ───────────────────
    // AM 的轨迹在漏检期间按速度外推位置(X ×0.95 / Y ×0.8 衰减), 并把外推结果
    // 直接加在框心上喂给下游。coast_consumed = 本拍真的用了外推值(即 misses > 0)。
    bool  coast_consumed = false;
    float coast_offset_x = 0.0f;      // 本拍滑行外推补上的 X 位移(像素)
    float coast_offset_y = 0.0f;      // 同上, Y

    // ── 新PID 的接入点 ─────────────────────────────────────────────────────
    // 误差 = 瞄点 - 准星(单位: 检测图像素, 浮点), 输出 = 整数鼠标计数。
    // 控制器全程在像素域用 double 算, 整数只在出口出现一次, 取整零头会攒到下一拍
    // 所以亚计数的小误差不会被丢掉。见 mouse/aim_pid.h 的说明。
    // 现役链路是计数域: 外面直接用sendRawMove 发出, 不做像素<->计数换算；
    //    · 换目标身份(motion_suppressed)/丢失目标时, 控制器reset(), 观测器
    //      resetTargetMotion()(保留已标定出的每计数多少像素)。
    int dx = 0;
    int dy = 0;

    // ── 控制器遥测(只给日志/调试用, 不参与控制) ─────────────────────────────
    // 误差单位像素, 速度单位像素/秒。前馈相关字段已随前馈一起删除。
    // 本拍是不是【真的换目标】: 身份变了 或 瞄点跳了 >=25px。仅供日志判断策略用。
    bool   target_switched = false;
    double target_anchor_jump_px = 0.0;  // 本拍瞄点相对上一拍的跳变量(像素)
    double pid_dt_ms = 0.0;           // 本拍实际用于输出缩放的 dt(已夹在典型拍间隔附近)
    double pid_error_px_x = 0.0;
    double pid_error_px_y = 0.0;
    // ── 在途补偿遥测 (2026-09-13) ────────────────────────────────────────────
    // ★ 排查"冲过头/贴不死"最先看这几个量: 补偿是不是吃掉了稳态误差。
    int    pid_inflight_x = 0;        // 本拍进入窗口的在途计数(原始, 未乘 beta)
    int    pid_inflight_y = 0;
    double pid_inflight_beta_x = 0.0; // 本拍生效的补偿强度
    double pid_inflight_beta_y = 0.0;
    double pid_used_error_px_x = 0.0; // 送给 P/I 的误差(现在恒等于 error — 无前馈)
    double pid_used_error_px_y = 0.0;
    double pid_cmd_x = 0.0;           // 取整前的浮点指令(计数)
    double pid_cmd_y = 0.0;
    // 排查"抽一下/冲过头"要看的中间量: 各分项的像素当量 + 积分状态 + 零头 + 限幅值
    double pid_i_px_x = 0.0;
    double pid_i_px_y = 0.0;
    double pid_d_px_x = 0.0;
    double pid_d_px_y = 0.0;
    double pid_integral_x = 0.0;   // 像素·秒
    double pid_integral_y = 0.0;
    double pid_carry_x = 0.0;      // 未发出的计数零头
    double pid_carry_y = 0.0;
    int    pid_limit_x = 0;        // 本拍生效的输出上限(计数/拍)
    int    pid_limit_y = 0;

    // ── 距离尺度 (2026-09-13 新增, 见 mouse/aim_scale.h) ────────────────────
    // 唯一的距离代理是【检测框高】。s ∈ [s_min, s_max], 近处 -> 大, 远处 -> 小。
    // 它作用于控制器的【等效增益】(近处更快)。★ 不用来做"几何投影补偿" —— 屏幕
    // 速度本身已经是世界速度的投影, 再乘一个和距离成正比的因子就是重复计算。
    double aim_scale = 1.0;          // 本拍使用的尺度(平滑后)
    double aim_scale_height_px = 0.0;// 本拍用于算尺度的框高(平滑后, 像素)
    bool   aim_scale_active = false; // false = 尺度关闭, 行为与没有它逐位相同

    // ── 跟踪器/预测遥测 (2026-09-15) ─────────────────────────────────────────
    // 排查"为什么跟丢了 / 提前量没起来 / 手感变了"先看这几个量。
    bool   esync_active = false;       // 本拍跟踪器给出了锁定轨迹
    int    esync_track_hits = 0;       // 跟踪器锁定轨迹的连续命中数
    int    esync_track_age = 0;        // 锁定轨迹的连续漏帧数(0 = 本帧有观测)
    bool   esync_track_confirmed = false;
    double esync_vel_x = 0.0;          // 跟踪器给出的框心速度(像素/秒)
    double esync_vel_y = 0.0;
    bool   esync_vel_valid = false;
    double esync_pred_k_x = 0.0;       // 预测系数 FSM 当前值 [0, 1]
    double esync_pred_k_y = 0.0;
    int    esync_track_id = -1;        // 跟踪器身份(与 current_track_id 不同源)
    int    esync_track_count = 0;      // 本拍存活轨迹数
    // ── 【2026-09-16 删除】esync_inflight_x/y ────────────────────────────────
    // 它们记录像素域在途补偿(AM 发送环 ÷ k̂)扣掉的量。整条链已随 k̂ 一起删除
    // (AM 里那条链由 FrameSync/EventSync 档消费, 且 k̂ 在双机架构下无法测量),
    // 在途补偿现在只有计数域一种落点。见 docs/aimmagic-ground-truth.md §6。
};

class AimEngine
{
public:
    AimEngine();
    ~AimEngine();

    void reset();
    EngineOutput tick(const EngineInput& in, double dt);

    // ── 【2026-09-16 删除】noteAimSend(int, int) ─────────────────────────────
    // 它是 AM 发送环的登记入口, 供像素域在途补偿使用。整条链已删除。
    int lockedTrackId() const { return current_id_; }
    const std::vector<Track>& tracks() const { return tracks_; }

    // ── 【2026-09-13 删除】「每计数像素」测量 API ─────────────────────────────
    // beginCalibrationMeasure / cancelCalibrationMeasure / measuringCalibration /
    // calibrationMeasureReady / measuredPxPerCountX/Y / calibrationMeasureFits(Needed) /
    // calibrationFitStats / resetCalibrationFitStats / calibrationHistorySize /
    // calibrationReady / autoPxPerCountX/Y —— 整组删除。
    //
    // 它们全都在测 k̂(每计数像素)。k̂ 在本项目双机架构下测不准(§6.7), 而且前馈删除后
    // 控制器根本不再需要这个量 —— 没有消费者了。界面上对应的「测量」按钮与其状态
    // 提示也一并移除(见 qt_ui/pages/HotkeyPage.cpp)。

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

    // X/Y 各一个控制器 + 一个框平滑器。
    //   误差是像素、输出是计数, 状态(积分/微分/零头)跨帧保留;
    //   平滑器只做"滤掉检测抖动", 位置进 PID, 速度只用于它自己的内部预测、不外传。
    AimPid pid_x_;
    AimPid pid_y_;
    AnchorFilter anchor_filter_x_;
    AnchorFilter anchor_filter_y_;
    // 距离尺度估计器 (2026-09-13)。X/Y 共用【一个】(距离是目标属性, 不分轴),
    // 结果通过 setScale() 分别注入两个 PID。见 mouse/aim_scale.h。
    AimScale aim_scale_;
    // 上一拍用于预测的框宽(像素)与预测后的瞄点(仅遥测)。
    float last_predict_width_ = 0.0f;
    float last_predict_lead_x_ = 0.0f;
    float last_predict_lead_y_ = 0.0f;
    int last_counts_x_ = 0;  // 上一拍实际下发的计数(仅遥测/日志用)
    int last_counts_y_ = 0;

    // ── 跟踪器状态 ───────────────────────────────────────────────────────────
    // 跟踪器跨帧持有(不因单帧漏检清空); 身份来自它, 速度与每轨预测状态也来自它。
    AimTracker esync_tracker_;
    int esync_locked_track_ = -1;   // 上一拍的跟踪器身份(判"换目标")
    bool esync_configured_ = false; // 跟踪器参数是否已按当前配置装过
    AimTrackerParams esync_params_{};  // 上一拍装进去的参数(变了才重装)
};

} // namespace boss

#endif // MOUSE_BOSS_AIM_H
