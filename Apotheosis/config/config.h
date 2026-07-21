#ifndef CONFIG_H
#define CONFIG_H

#include <memory>
#include <array>
#include <string>
#include <vector>

// Per-class routing bucket. The detection pipeline applies these in order:
// Delete  -> dropped right after NMS, never reaches the tracker or the UI.
// Filter  -> kept in DetectionBuffer (visible to debug UI / depth mask / etc.)
//            but excluded from the aim candidate pool.
// Aim     -> eligible aim candidate; per-hotkey priority decides which one wins.
enum class ClassBucket
{
    Delete = 0,
    Filter = 1,
    Aim = 2,
};

struct ClassFilterState
{
    int class_id = 0;
    std::string class_name; // best-effort display name, falls back to "class_<id>"
    ClassBucket bucket = ClassBucket::Delete;
};

// One aim class entry in a hotkey's priority-ordered aim list. Order in
// HotkeyProfile::aim_classes IS the priority (index 0 wins over index 1).
// y_offset..y_offset_max: 每次新锁定时随机抽取的 Y 锁点范围。
// 1=框顶, 0.5=中心, 0=框底 (数值大 = 更靠上);两端相等就是固定锁点。
// min_conf: 该类别的最低置信度 (0=不过滤)。低于阈值的检测不会参与该槽位的匹配。
struct HotkeyAimClass
{
    int   class_id = 0;
    float y_offset = 0.5f;
    float y_offset_max = 0.5f;
    float min_conf = 0.0f;
};

// Reserved synthetic class id for the flashlight halo "detection". Kept far
// above any real model class count so it can never collide, and it only ever
// flows into the engine's eligible-class set / priority map (hash lookups) and
// the preview's "#id" label — never an array index — so the large value is
// safe. The fixed name lets the user route it from the aim-class UI exactly
// like a model class: Config::ensure_flashlight_class() seeds a class_filters
// row {kFlashlightClassId, "shoudiantong", Aim} that survives model reloads.
inline constexpr int         kFlashlightClassId   = 9000;
inline constexpr const char* kFlashlightClassName = "shoudiantong";

// A single aim hotkey. Multiple HotkeyProfiles can exist; whichever one has
// any of its keys pressed wins. If several are pressed simultaneously, the
// one earlier in Config::hotkeys wins. Every hotkey carries its own full set
// of mouse parameters — there is no longer a "global mouse" to fall back to.
struct HotkeyProfile
{
    std::string name = "Aim";
    std::string group = u8"默认";
    std::vector<std::string> keys;

    int fovX = 106;
    int fovY = 74;

    // ─────────────────────────────────────────────────────────────────────
    // ART (Adaptive Reactive Tracker) — modified 1€ Filter
    //   speed_x/y: 比例驱动灵敏度 (对准静止目标调到一次到位)
    //   dead_zone_px: 死区半径 (px), 小于此误差不输出移动
    // ─────────────────────────────────────────────────────────────────────
    float speed_x = 0.6f;
    float speed_y = 0.6f;
    float dead_zone_px = 2.0f;

    // 疾风 (Predictive) 4 项可调: 每轴灵敏度(Kp)、阻尼(Kd)、预测权重(双轴共用)。
    // 渐入 / 输出上限 / 死区都硬编默认值(见 movers.cpp)。
    float predictive_kp_x       = 0.6f;
    float predictive_kp_y       = 0.6f;
    float predictive_kd         = 0.10f;
    float predictive_pred_weight= 0.5f;

    // 控制器:0=天枢,1=摇光。摇光五个宏参数中预测使用毫秒，其余为 0..100。
    int   mover_kind = 0;
    float yaoguang_pull_speed_x = 60.0f;
    float yaoguang_pull_speed_y = 60.0f;
    float yaoguang_tracking = 65.0f;
    float yaoguang_prediction_ms = 25.0f;
    float yaoguang_stability = 55.0f;

    // ─────────────────────────────────────────────────────────────────────
    // 天枢 (Classic) — 经典全参 PID + 动态 KP + EMA/Kalman 预测。
    //   aim_mode 0 = 简单(对称 KP + 共用 KI/KD);
    //   aim_mode 1 = 高级(独立 XY 全 PID + 距离/时间 KP 调度)。
    //   prediction_mode 0 = 无; 1 = EMA 速度外推; 2 = Kalman + lookahead。
    // ─────────────────────────────────────────────────────────────────────
    int   classic_aim_mode = 0;

    // 简单模式
    float classic_simple_start_speed = 0.3f;
    float classic_simple_end_speed   = 0.8f;
    int   classic_simple_transition_ms = 0;
    float classic_simple_ki = 0.0f;
    float classic_simple_kd = 0.0f;

    // 高级模式 X
    float classic_adv_kpmin_x = 0.3f;
    float classic_adv_kpmax_x = 0.8f;
    float classic_adv_ki_x = 0.0f;
    float classic_adv_kd_x = 0.0f;
    float classic_adv_imax_x = 0.0f;
    float classic_adv_pfactor_x = 1.0f;
    int   classic_adv_time_x = 0;
    bool  classic_adv_time_dynamic_x = false;

    // 高级模式 Y
    float classic_adv_kpmin_y = 0.3f;
    float classic_adv_kpmax_y = 0.8f;
    float classic_adv_ki_y = 0.0f;
    float classic_adv_kd_y = 0.0f;
    float classic_adv_imax_y = 0.0f;
    float classic_adv_pfactor_y = 1.0f;
    int   classic_adv_time_y = 0;
    bool  classic_adv_time_dynamic_y = false;

    // 预测
    int   classic_prediction_mode = 0;
    float classic_velocity_lead_frames = 1.0f;
    bool  classic_independent_y = false;

    // Kalman
    float classic_kalman_q_pos = 1.0f;
    float classic_kalman_q_vel = 1.0f;
    float classic_kalman_r_obs = 1.0f;
    float classic_kalman_lookahead = 2.0f;

    // ─────────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────────
    // 瞄准轨迹曲线 (aim path).
    //   0 = Linear:   直接朝目标走 (ART 原行为)
    //   1 = Bezier:   起点(0,0)→终点(1,0) 的三次贝塞尔, 控制点 cx1/cy1, cx2/cy2
    //   2 = Custom:   用户在 UI 上自由画的偏离曲线, 32768 个 Y 采样均匀分布在 X∈[0,1]
    // X = 沿 start→goal 主轴的进度, Y = 垂直方向的偏离量(以 path 长度为单位)。
    // Linear 模式下后续 bezier / custom 参数被忽略, 不必清零。
    // ─────────────────────────────────────────────────────────────────────
    int   aim_path_mode = 0;
    float aim_path_bezier_cx1 = 0.30f;
    float aim_path_bezier_cy1 = 0.00f;
    float aim_path_bezier_cx2 = 0.70f;
    float aim_path_bezier_cy2 = 0.00f;
    // 32768 个采样, Y∈[-1, 1], 首尾端点固定 = 0。
    static constexpr int kAimPathSampleCount = 32768;
    // 不可变共享曲线资产:HotkeyProfile 快照只复制指针，UI 修改时才重建。
    std::shared_ptr<const std::vector<float>> aim_path_custom_samples =
        std::make_shared<const std::vector<float>>(); // 空 = 直线
    bool aim_path_neural_enabled = false;
    // 1→8→1 MLP:w1[8], b1[8], w2[8], b2。
    std::array<float, 25> aim_path_neural_weights{};

    // ─────────────────────────────────────────────────────────────────────
    // 死区 (shared, 三套 mover 通用):准星在目标框 N% 内时停止移动。
    // 在 engine tick() 层检查,mover dispatch 之前生效。
    // ─────────────────────────────────────────────────────────────────────
    bool  deadzone_enabled = false;
    float deadzone_percent = 0.0f;

    // 检测暂时丢失时继续保留同一 track 的帧数。期间不发送旧坐标移动，
    // 只等待同一目标重新出现；0 = 当帧丢失即释放，默认 5 与旧行为一致。
    int   lost_target_cache_frames = 5;

    // ─────────────────────────────────────────────────────────────────────
    // 扳机 — 5 态状态机 (idle/delay/pressed/cooldown/switch_cd)。
    //   trigger_fire_delay:    进入命中区后延迟 N ms 才按下(0=立即)
    //   trigger_fire_duration: 每次按住持续 N ms
    //   trigger_fire_interval: 松手后冷却 N ms 才能再次触发
    //   trigger_y_percent:     命中区占 bbox 的百分比 (100=整框, >100=预开火)
    //   trigger_*_jitter_ms:   对应延迟的随机 ±N ms 抖动(破除机械感)
    //   trigger_switch_cooldown_ms: 目标 track_id 变化时的转火冷却
    // ─────────────────────────────────────────────────────────────────────
    bool  trigger_enabled = false;
    int   trigger_fire_delay = 0;
    int   trigger_fire_duration = 100;
    int   trigger_fire_interval = 200;
    int   trigger_y_percent = 100;
    int   trigger_delay_jitter_ms    = 0;
    int   trigger_duration_jitter_ms = 0;
    int   trigger_interval_jitter_ms = 0;
    int   trigger_switch_cooldown_ms = 0;

    // ─────────────────────────────────────────────────────────────────────
    // Y 轴力度百分比(应用于所有 mover 输出的最终 dy)。
    //   100 = 原样;<100 削弱垂直分量,使弹道更"飘"(近人手感);
    //   >100 加强(不常用)。X 分量不动。
    // ─────────────────────────────────────────────────────────────────────
    int   y_strength_percent = 100;

    // ─────────────────────────────────────────────────────────────────────
    // 目标选择 — 按优先级排序的类别列表。列表顺序 = 优先级 (index 0 最高)。
    //   aim_classes[i].class_id: 该条目匹配的 class_id
    //   aim_classes[i].y_offset..y_offset_max: 每次新锁定随机抽取的 Y 范围
    //   aim_classes[i].min_conf: 该类别的最低置信度 (0=不过滤)
    // 优先级优先, 距离次之; 未在列表内的 class 被忽略。类别只能加一次
    // (来源: Target 页 "瞄准" 桶), 通过 UI 拖动整行调整上下顺序。距离上限
    // 已由 FOV 椭圆(HotkeyProfile::fovX/fovY)承担, 不再单独设。
    // ─────────────────────────────────────────────────────────────────────
    std::vector<HotkeyAimClass> aim_classes;

    // Per-hotkey crosshair color detection toggle. Global config still owns
    // the ROI size / color palette / area filters; each hotkey just opts in.
    bool crosshair_detect_enabled = false;

    // Per-hotkey laser color detection toggle. INDEPENDENT of crosshair
    // detection: both may be on at once. When both produce a pivot the
    // crosshair (centroid) result WINS — the laser tip is only used as a
    // fallback when crosshair-colour found nothing this frame. Global config
    // owns the laser ROI / palette / params (separate from the crosshair set).
    bool laser_detect_enabled = false;

    // Per-hotkey flashlight halo detection toggle. INDEPENDENT of crosshair /
    // laser: all three may be on at once. Detects an enemy flashlight glare
    // anywhere on screen (hue-agnostic, brightness + circularity) so the user
    // can flick onto / shoot at the over-exposed halo. Lowest priority of the
    // three colour sources — only published as the pivot when both
    // crosshair-colour and laser came up empty. Global config owns the
    // threshold / radius / circularity gates.
    bool flashlight_detect_enabled = false;

    // Per-hotkey 玻璃过滤开关。开启后,锁敌之前对每个 detection box 的边
    // 缘环做玻璃色覆盖率检测,被判玻璃的框直接从候选里剔除(不进
    // tracker、不出现在 aim 候选)。全局色带 / 环厚度 / 阈值在 Config 上,
    // 这里只是热键级 opt-in。
    bool glass_filter_enabled = false;

    // Dynamic FOV — applied live every frame. `fovX`/`fovY` above become
    // the BASE (max) aim-region diameters in detection pixels around the
    // crosshair. While a target is locked the effective FOV interpolates
    // between that base and a tight rectangle that just contains the
    // locked bbox plus `dynamic_fov_margin_frac` padding, with the blend
    // driven by how far the locked pivot currently sits from the crosshair:
    //   far  → use base (room to flick onto another target)
    //   near → use bbox-tight (no room for distractors to steal the lock).
    // Floors at `dynamic_fov_min_radius_frac` × base radius so the region
    // never collapses below something the user can recover from. The same
    // region also gates which detections enter the lock candidate pool.
    // 动态 FOV 收敛为一个 knob。0 = 不收缩(只用基础 FOV),1 = 紧贴目标 bbox。
    // 内部反算:margin_frac = 2.0 - strength; min_radius_frac = 0.50 - 0.40 * strength。
    bool  dynamic_fov_enabled  = false;
    float dynamic_fov_strength = 0.60f;

};

// One entry in the shared crosshair color palette. Red needs two entries
// because hue wraps at 0/180; green / purple / cyan need only one.
struct CrosshairColorProfileConfig
{
    std::string name = "Red-Low";
    bool enabled = true;
    int h_low = 0;
    int h_high = 10;
    int s_min = 120;
    int s_max = 255;
    int v_min = 120;
    int v_max = 255;
};

class Config
{
public:
    // Capture
    std::string capture_method;
    std::string udp_ip;
    int udp_port = 0;
    std::string tcp_ip;
    int tcp_port = 0;
    // eth_capture: ProSexy 原始以太网帧接收端。eth_adapter 为 npcap 设备名
    // (\Device\NPF_{GUID}); eth_ethertype 须与发送端一致(ProSexy 默认 0x88B5)。
    std::string eth_adapter;
    int eth_ethertype = 0x88B5;
    // 采集卡几何参数,由 OpenCV / MF / 圆刚 SDK 后端共用。
    int opencv_capture_index = 0;
    std::string opencv_capture_api = "DSHOW"; // DSHOW | MSMF | FFMPEG | ANY (仅 opencv)
    std::string opencv_capture_url;           // 可选连接 URL (rtsp:// / 文件路径); 空 = 用设备索引 (仅 opencv)
    int opencv_capture_width = 0;             // 原始采集宽度, 0 = 让设备决定
    int opencv_capture_height = 0;            // 原始采集高度, 0 = 让设备决定
    int opencv_capture_fps = 0;               // 采集 FPS, 0 = 设备默认
    int capture_crop = 0;                     // 中心裁切正方形边长; >0 时驱动 detection_resolution, 0 = 整帧缩放到 detection_resolution
    std::string capture_format = "MJPG";      // NV12 | MJPG | YUY2 | RGB32
    bool capture_mf_gpu = true;               // MF/圆刚: true=GPU 转换, false=CPU 转换
    int detection_resolution = 320;
    int capture_fps = 60;
    bool circle_mask = true;

    // Hardware
    std::string input_method = "WIN32"; // WIN32 | GHUB | ARDUINO | KMBOX_NET | KMBOX_A | MAKCU
    int arduino_baudrate = 115200;
    std::string arduino_port = "COM0";
    bool arduino_16_bit_mouse = false;
    bool arduino_enable_keys = false;

    std::string kmbox_net_ip = "10.42.42.42";
    std::string kmbox_net_port = "1984";
    std::string kmbox_net_uuid = "DEADC0DE";

    std::string kmbox_a_pidvid;

    int makcu_baudrate = 115200;
    std::string makcu_port = "COM0";

    // AI
    std::string backend = "TRT";
    int dml_device_id = 0;
    std::string ai_model = "sunxds_0.5.6.engine";
    float confidence_threshold = 0.15f;
    float nms_threshold = 0.50f;
    int max_detections = 20;
    // 小目标召回增强:面积自适应置信度阈值。开启后,框面积 < small_target_area_frac
    // × detection_resolution² 的小目标用 small_target_confidence 作为保留门槛,大目标
    // 仍用 confidence_threshold;GPU 粗筛阈值同步降到两者较小值,让弱小目标候选先进入
    // CPU 再按面积二次过滤。默认关闭以保持旧行为。
    bool  small_target_enabled = false;
    float small_target_area_frac = 0.012f;
    float small_target_confidence = 0.06f;
    bool export_enable_fp8 = false;
    bool export_enable_fp16 = true;
    bool fixed_input_size = false;

    // CUDA / System
    bool use_cuda_graph = true;
    // Double-buffer pipeline: overlap CPU post-processing of frame N with
    // GPU preprocess+inference of frame N+1. Trades ~1 frame of latency for
    // throughput. Disables CUDA Graph path when enabled (simpler code path).
    // Default ON: hides the CPU NMS / D2H sync from the inference critical
    // path. The 1-frame extra latency is well below typical capture jitter
    // (~8ms at 120fps), and downstream Kalman prediction compensates.
    bool use_double_buffer = true;
    bool use_pinned_memory = true;
    int gpuMemoryReserveMB = 2048;
    bool enableGpuExclusiveMode = true;
    bool capture_use_cuda = true;
    int cpuCoreReserveCount = 4;
    int systemMemoryReserveMB = 2048;

    // Overlay
    int overlay_opacity = 240;
    float overlay_ui_scale = 1.0f;

    // Authorization
    bool auth_require_online = true;
    std::string auth_server_url = "http://110.42.232.243:8787";

    // Depth
    bool depth_inference_enabled = true;
    std::string depth_model_path = "depth_anything_v2.engine";
    // TRT OptimizationProfile 的 OPT 档输入边长(方形)。只在导出 .engine 时被用作
    // kernel autotune 最优尺寸,改完必须删旧 .engine 重新导出才生效。范围 [160, 640]。
    int depth_opt_input_size = 224;
    // 深度归一化时裁掉的低/高百分位。0/100 = 纯 MIN-MAX(传统行为);
    // 把上限调到 95 可以裁掉极近离群值(贴脸的枪/手),避免远景
    // 被压扁到 depth_norm≈0、和敌人一起被遮罩误伤。范围 [0, 50] / [50, 100]。
    float depth_norm_clip_low_pct  = 0.0f;
    float depth_norm_clip_high_pct = 100.0f;
    int depth_mask_fps = 5;

    // Debug
    bool show_window = true;
    bool show_fps = false;

    // Aim trajectory replay (debug). When `replay_record_enabled` is true
    // a ring buffer in mouse_thread_loop captures the last N seconds of
    // detections, locked target, mouse moves, and hotkey state. The debug
    // panel exposes a "snapshot + slow-play" button to freeze and replay
    // the buffer onto the detection overlay. Cheap when off (no allocation).
    bool replay_record_enabled = false;
    int  replay_seconds = 10;          // ring-buffer length in seconds
    float replay_playback_speed = 0.25f; // 0.25 = 1/4 speed
    std::vector<std::string> screenshot_button;
    int screenshot_delay = 500;
    bool verbose = false;

    // ─────────────────────────────────────────────────────────────────────
    // 自动采集 (auto data collection)
    //   enabled         总开关
    //   use_high/low    "高置信度采集" / "低置信度采集" 两个独立开关
    //   high/low_conf   分别的阈值,conf ≥ high 或 conf ≤ low 触发采集
    //   cooldown_ms     最小存盘间隔,防止一个画面连续写
    //   force_keys      强制采集按键(通常侧键),按住期间每帧都存
    //   output_dir      .jpg / .txt 落盘目录
    //   save_label      true = 同时写 YOLO 格式 .txt 标签
    // ─────────────────────────────────────────────────────────────────────
    bool   auto_capture_enabled    = false;
    bool   auto_capture_use_high   = true;
    float  auto_capture_high_conf  = 0.85f;
    bool   auto_capture_use_low    = false;
    float  auto_capture_low_conf   = 0.30f;
    // "任意检测" 触发:忽略 high/low 阈值,只要该帧有一个 YOLO 检测就采集。
    // 用于快速积累样本(适合刚训完模型或新场景数据启动阶段)。
    bool   auto_capture_any_detection = false;
    // "寻光" 触发:当 flashlight_runtime 本帧有 valid 命中时采集
    // (跟 YOLO detection 独立)。适合专门收集光晕样本。
    bool   auto_capture_use_flashlight = false;
    int    auto_capture_cooldown_ms = 200;
    std::vector<std::string> auto_capture_force_keys;
    std::string auto_capture_output_dir = "screenshots/auto";
    bool   auto_capture_save_label = true;

    // 事件编排规则(每行一条,单行序列化,见 event_orchestrator::serialize_rule)。
    // 引擎启动时反序列化并 event_orch::set_rules;UI 修改后重新序列化写回。
    std::vector<std::string> event_rules_serialized;

    // Class filter table (one entry per class_id the user has seen). New
    // classes discovered after a model change start in Delete and the user
    // opts them in from the Target panel.
    std::vector<ClassFilterState> class_filters;

    // Crosshair color detector (shared palette). The per-hotkey toggle lives
    // on HotkeyProfile::crosshair_detect_enabled; this struct only carries
    // the sampling rectangle, the color list, and the contour-area filters.
    int crosshair_rect_w = 40;
    int crosshair_rect_h = 40;
    // Minimum red-pixel count inside the ROI for a detection to count
    // (replaces the old contour-area filter — detector now uses a
    // shape-agnostic mask-density centroid).
    int crosshair_min_pixel_count = 4;
    // MORPH_CLOSE radius (px). 0 = off. 1–3 covers most gradient/white-
    // centred crosshair styles. >5 risks merging red noise.
    int crosshair_close_radius = 1;
    // Anti-jitter: adaptive (One-Euro) temporal smoothing of the published
    // pivot. 0 = off (raw). Higher = steadier when settled while staying
    // responsive on fast moves (so it won't lag recoil/tracking). [0,1].
    float crosshair_smooth = 0.5f;

    std::vector<CrosshairColorProfileConfig> crosshair_colors; // defaults to red double-band

    // ---- Laser-sight color find (independent module) ----------------------
    // Fully separate from the crosshair centroid detector above: its own
    // enable (HotkeyProfile::laser_detect_enabled), its own colour palette,
    // ROI and params. Both can run at once; crosshair has priority and the
    // laser tip is only the fallback. The beam is found as a line and its AIM
    // END (tip near centre) is reported, optionally extrapolated a few px along
    // the beam to recover the faint gradient end. See crosshair/laser_detector.h
    // for the geometry-based, background-robust selection (elongation gate +
    // muzzle-below-tip orientation).
    std::vector<CrosshairColorProfileConfig> laser_colors; // separate palette
    // Laser sampling rectangle, freely positionable: width/height + explicit
    // CENTRE point (x,y) in detection-image pixels. Lets the user drag the
    // laser ROI anywhere (the beam body sits below the crosshair, often
    // off-centre). Clamped to the frame at use time.
    int   laser_rect_w = 160;          // detect rect width  (det px)
    int   laser_rect_h = 240;          // detect rect height (det px)
    int   laser_center_x = 160;        // detect ROI centre X (det px)
    int   laser_center_y = 200;        // detect ROI centre Y (det px)
    int   laser_min_pixel_count = 10;  // min matched pixels for a beam component
    int   laser_close_radius = 1;      // MORPH_CLOSE radius to bridge the beam (0=off)
    float laser_min_elongation = 3.0f; // line-likeness gate (rejects red blobs)
    float laser_smooth = 0.5f;         // anti-jitter adaptive smoothing of the tip [0,1], 0=off
    // Target region (the 2nd box, drawn brown) near the static screen centre
    // where the true beam endpoint lies. The fitted line (from the reliable
    // front segment) is projected into this box to estimate the tip — this
    // REPLACES a fixed pixel extension, so a fuzzy/flickering visible end no
    // longer makes the reported tip jump. The tip is clamped inside this box,
    // which bounds over/under-extension.
    int   laser_target_center_x = 160; // target box centre X (det px)
    int   laser_target_center_y = 160; // target box centre Y (det px)
    int   laser_target_rect_w = 60;    // target box width  (det px)
    int   laser_target_rect_h = 60;    // target box height (det px)

    // ---- Flashlight halo detector (whole-frame, brightness-based) ----------
    // 寻光 / 手电筒检测。Hue-agnostic: thresholds on max(B,G,R) so any colour
    // tinted flashlight glare matches; circularity gate rejects bright UI
    // bars and elongated sun glints. See crosshair/flashlight_detector.h for
    // the geometry of the search. Per-hotkey opt-in lives on
    // HotkeyProfile::flashlight_detect_enabled.
    bool  flashlight_show_preview = false;     // overlay debug toggle
    // Three behaviour-level macro knobs (each 0..100). ALL internal detection /
    // discrimination constants (brightness, radius band, circularity, contrast,
    // depth-gate, temporal tracker, colocation) are derived from these by
    // crosshair::flashlight_derive_tuning() — the seven old raw parameters no
    // longer exist as config. See crosshair/flashlight_tuning.h.
    int   flashlight_sensitivity     = 50; // 灵敏度: 锁得勤 ↔ 锁得稳 (外观轴)
    int   flashlight_reject_strength = 50; // 抗误锁: 信外观 ↔ 信判别 (深度/时间/联动)
    int   flashlight_spot_size       = 50; // 光斑大小: 可接受半径档 (按分辨率自动)

    // ---- Glass filter (穿不透玻璃后的人形抑制) ----------------------------
    // 三角洲里有打不穿的玻璃,模型只看轮廓也会识别出后面的人,锁过去白浪
    // 费子弹。本模块在 mouse 循环里、tracker 之前,对每个 detection box
    // 的"边缘环"采样玻璃膜特征色,命中率超过阈值的 box 直接抹掉。所有
    // 工作在 CPU 完成、对每框 < 0.2 ms,延迟开销可忽略。
    //
    // 全局色带 + 一个宏观旋钮「过滤强度」,启用与否在 HotkeyProfile::glass_filter_enabled。
    // 旧的环厚 / 命中阈值 / 最小框三个原始参数已收进单旋钮,环厚固定 0.15、最小框
    // 按分辨率自动,命中阈值由强度反推。映射唯一来源:crosshair/glass_tuning.h。
    bool  glass_filter_show_preview = false;     // 预览叠图(框边缘画环)
    int   glass_filter_strength     = 50;        // 过滤强度 0..100: 保守 ↔ 激进
    std::vector<CrosshairColorProfileConfig> glass_colors; // 默认浅蓝 + 浅绿薄膜双带

    // Aim hotkeys. Must contain at least one entry so the UI always has
    // something to show; defaultConfig() populates a single "Aim" hotkey
    // bound to RightMouseButton.
    std::vector<HotkeyProfile> hotkeys;
    std::string active_hotkey_group;

    // Macro layer (G HUB-compatible Lua). When `macro_enabled` is true the
    // runtime loads `macro_script_path` at startup and dispatches mouse-
    // button events to its OnEvent callback. macro_primary_button_events
    // mirrors the script-side EnablePrimaryMouseButtonEvents() default —
    // when true the script will receive LMB events without having to opt
    // in. Persisted so the user's last-used script auto-attaches across
    // process restarts.
    bool        macro_enabled = false;
    std::string macro_script_path;
    bool        macro_primary_button_events = false;

    bool loadConfig(const std::string& filename = "config.ini");
    bool saveConfig(const std::string& filename = "config.ini");

    // Rebuild/expand class_filters based on the latest model metadata:
    // - keep existing bucket for class_ids still present
    // - fill in new class_ids with bucket=Delete
    // - update class_name when the model supplies a better one
    void sync_class_filters_from_model(int class_count,
                                       const std::vector<std::string>& class_names);

    // Guarantee the synthetic flashlight aim class ({kFlashlightClassId,
    // "shoudiantong", Aim}) exists in class_filters. Idempotent. Called after
    // loadConfig() and after sync_class_filters_from_model() (which otherwise
    // rebuilds the table from 0..class_count-1 and would drop it), so the
    // flashlight stays routable in the aim-class UI across model reloads.
    void ensure_flashlight_class();

    // Return the first hotkey whose keys list includes any currently-pressed
    // physical key. Caller owns the pressed-test predicate (keyboard_listener
    // already knows how to resolve device-specific presses). Returns -1 when
    // nothing is active.
    // Implementation lives in keyboard_listener.cpp to avoid pulling Win32
    // into Config.
    // (declared here only so call sites can find it next to Config.)

    std::string joinStrings(const std::vector<std::string>& vec,
                            const std::string& delimiter = ",") const;

private:
    std::vector<std::string> splitString(const std::string& str, char delimiter = ',') const;
    std::string config_path;
    void writeDefaultsInPlace();
};

#endif // CONFIG_H
