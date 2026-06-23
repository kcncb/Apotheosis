#ifndef CONFIG_H
#define CONFIG_H

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

// Per-hotkey class selection: a class the user has enabled for THIS hotkey,
// with an optional per-class Y offset (0.0 = top of the detection box,
// 1.0 = bottom). X is always box center.
struct HotkeyAimClass
{
    int class_id = 0;
    float y_offset = 0.5f;

    // 已精简:不再有每类别卡尔曼覆盖。卡尔曼参数统一在热键级(平滑度/预测提前量)。
};

// A single aim hotkey. Multiple HotkeyProfiles can exist; whichever one has
// any of its keys pressed wins. If several are pressed simultaneously, the
// one earlier in Config::hotkeys wins. Every hotkey carries its own full set
// of mouse parameters — there is no longer a "global mouse" to fall back to.
struct HotkeyProfile
{
    std::string name = "Aim";
    // Hotkey 所属"组"。同名的热键在 UI 里会聚成一段方便管理(预设/职业/武器)。
    // 运行时仍按 Config::hotkeys 的扁平顺序匹配,组名只影响展示和拷贝粘贴范围,
    // 留空时归入"默认"组。
    std::string group = u8"默认";
    std::vector<std::string> keys;                    // any-of: pressing any triggers the profile
    std::vector<HotkeyAimClass> aim_classes;          // priority-ordered

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

    // ─────────────────────────────────────────────────────────────────────
    // 移动控制器选择 (mover_kind):
    //   0 = 微澜 (Smooth)     — 默认,boss::AimEngine 走 ART/path 原路径,
    //                           speed_x/y/dead_zone_px 直接喂 ART::drive。
    //   1 = 疾风 (Predictive) — 位置式 PID + 导数估计预测器,参数完全暴露。
    // 不为 0 时,engine 旁路 AimPathDriver,把 (filtered aim − crosshair) 喂给
    // PID 直接出 dx/dy(此时 aim_path_mode 被忽略)。
    // ─────────────────────────────────────────────────────────────────────
    int mover_kind = 0;

    // 疾风 (Predictive) 4 项可调: 每轴灵敏度(Kp)、阻尼(Kd)、预测权重(双轴共用)。
    // 渐入 / 输出上限 / 死区都硬编默认值(见 movers.cpp)。
    float predictive_kp_x       = 0.6f;
    float predictive_kp_y       = 0.6f;
    float predictive_kd         = 0.10f;
    float predictive_pred_weight= 0.5f;

    // ─────────────────────────────────────────────────────────────────────
    // 瞄准轨迹曲线 (aim path).
    //   0 = Linear:   直接朝目标走 (ART 原行为)
    //   1 = Bezier:   起点(0,0)→终点(1,0) 的三次贝塞尔, 控制点 cx1/cy1, cx2/cy2
    //   2 = Custom:   用户在 UI 上自由画的偏离曲线, 32 个 Y 采样均匀分布在 X∈[0,1]
    // X = 沿 start→goal 主轴的进度, Y = 垂直方向的偏离量(以 path 长度为单位)。
    // Linear 模式下后续 bezier / custom 参数被忽略, 不必清零。
    // ─────────────────────────────────────────────────────────────────────
    int   aim_path_mode = 0;
    float aim_path_bezier_cx1 = 0.30f;
    float aim_path_bezier_cy1 = 0.00f;
    float aim_path_bezier_cx2 = 0.70f;
    float aim_path_bezier_cy2 = 0.00f;
    // 32 个采样, Y∈[-1, 1], 端点(index 0 / 31) 固定 = 0。
    static constexpr int kAimPathSampleCount = 32;
    std::vector<float> aim_path_custom_samples; // 留空 = 全 0 (= 直线)

    // Smart trigger — automatic fire, rewritten as a pure geometric
    // triggerbot decoupled from the aim controller. The crosshair (visible
    // reticle when crosshair-colour is on, otherwise detection centre) is
    // tested against the locked target's bounding box scaled per-axis by
    // `hit_scale_x` / `hit_scale_y` about the target's aim anchor. The test
    // is rectangular and uses the OBSERVED target position (never the Kalman
    // prediction) so lead/prediction never desyncs the fire decision.
    //
    // State machine: the crosshair must stay inside that region for
    // `reaction_ms` (human-like dwell) before the first press; the button is
    // then held for `hold_ms`, released, and a `cooldown_ms` refractory is
    // enforced before the next tap. Holding the aim hotkey is implicit (the
    // trigger only runs while a hotkey is active). Hotkey release, toggle
    // off, target loss, or session end all force-release the button so it can
    // never get stuck down.
    // 智能扳机参数:
    //   smart_trigger_hit_scale:命中容差(占 bbox 半轴,X / Y 共用)
    //   smart_trigger_aggression [0,1]:仅控制反应延迟 reaction_ms 档位(由
    //     mouse_thread_loop 反算: 0 → 80ms, 1 → 0ms)。
    //   smart_trigger_hold_ms / smart_trigger_cooldown_ms:用户直接控制的
    //     按住时长与两次按住之间的延迟(ms),分别透传给 MouseThread。
    bool  smart_trigger_enabled = false;
    float smart_trigger_hit_scale = 0.60f;
    float smart_trigger_aggression = 0.50f;
    int   smart_trigger_hold_ms = 45;
    int   smart_trigger_cooldown_ms = 55;

    // (legacy prediction fields removed — adaptive aim handles prediction internally)

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

    // 锁定灵活度 lock_aggression。**一个 knob 同时控制"切换迟滞"与"击杀检测"两套
    // 逻辑**(原本是两个对立 knob,会互相打架产生切换乒乓,已合并)。
    //   0 = 死锁:hold/min_frames/coast_grace 全拉满,kill_detect 关闭。锁定后基本
    //       不可能切换,适合单人对枪。
    //   1 = 灵活:hold=0、margin=0、min_frames=1、coast_grace=1,kill_detect 满激进。
    //       任何更好目标立即夺锁,适合团战换人头。
    // 高优先级类别仍可立即抢锁,不受此值影响。内部反算见 mouse_thread_loop.cpp。
    float lock_aggression = 0.30f;

    // y_offset distance/size decay. When enabled, large bboxes (close
    // targets) blend the per-class y_offset toward 0.5 (geometric centre)
    // because the same fractional offset maps to many more pixels at close
    // range, amplifying jitter. Disabled by default to preserve legacy
    // behaviour for users who tuned y_offset around close-range play.
    // 大目标(框高占检测高 ≥ 40%)瞄点向中心衰减、小目标(< 10%)保留原 y_offset。
    // 上下两个边界内部硬编(10% / 40%),只暴露开关。
    bool  y_offset_size_decay_enabled = false;

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

    // Threat-weighted target priority. The threat score is a [0,1] blend of
    // (a) normalized depth at the track pivot (closer = higher threat) and
    // (b) head-class detection confidence (higher conf = higher threat).
    // `threat_depth_head_ratio` linearly mixes the two: 0 = full depth, 1 =
    // full head-conf. The result multiplies into the lock-candidate score
    // via `threat_weight` (0 = disable, 1 = full multiplicative effect).
    // `threat_head_class_id` may be -1 when no head class is selected — the
    // head-conf term then falls back to neutral 0.5.
    // 威胁权重 + 头部类别;depth/head 混合比内部硬编 0.5(各占一半)。
    bool  threat_priority_enabled = false;
    float threat_weight           = 0.50f;
    int   threat_head_class_id    = -1;

    // 近距离瞄头(body→head pivot 吸附)。当一个 body(非 head)目标框够大(近距离,
    // 框高 / 检测高 ≥ close_range_trigger_height_frac),且其内部上半区存在一个
    // close_range_head_class_id 的 head 检测时,把该目标的瞄准点吸附到 head 框,让贴脸
    // 大目标优先瞄头 / 上半身。只移动 pivot、不改变锁定的 track 身份,因此不会触发锁切换
    // 或卡尔曼重置。head 类别无需加入瞄准类别列表。与 y_offset_size_decay(把大框拉向
    // 中心)方向相反,建议二选一。默认关闭以保持旧行为。
    // 近距离瞄头:框高占检测高 ≥ 30%(硬编)触发 body→head pivot 吸附。
    bool  close_range_head_aim_enabled = false;
    int   close_range_head_class_id    = -1;

    // 多人锁切（kill-detect）。开火期间锁定目标突然消失（失观帧数 ≥
    // kill_suspicion_missed_frames 且最后一次观测到的 bbox 面积 ≤ kill_suspicion_bbox_shrink）
    // 视为击杀，绕过 lock_hold_min_frames 立即把锁切到下一个可见目标。
    // kill_followup_grace_frames：击杀后短时间内允许同 rank 立即夺锁，让连杀更顺滑。
    // kill_trigger_fresh_ms：trigger 击发"新鲜度"窗口（毫秒），超时不再触发 kill 判定。
    // 默认 enable，三角洲组队对枪场景受益明显。
    // (kill_detect 已并入 lock_aggression;此处保留空注释,旧 ini 字段会被读取并
    //  迁移为 lock_aggression。)

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
    // 采集卡几何参数,由 opencv_capture 与 mf_capture 两套后端共用。
    int opencv_capture_index = 0;
    std::string opencv_capture_api = "DSHOW"; // DSHOW | MSMF | FFMPEG | ANY (仅 opencv)
    std::string opencv_capture_url;           // 可选连接 URL (rtsp:// / 文件路径); 空 = 用设备索引 (仅 opencv)
    int opencv_capture_width = 0;             // 原始采集宽度, 0 = 让设备决定
    int opencv_capture_height = 0;            // 原始采集高度, 0 = 让设备决定
    int opencv_capture_fps = 0;               // 采集 FPS, 0 = 设备默认
    int capture_crop = 0;                     // 中心裁切正方形边长; >0 时驱动 detection_resolution, 0 = 整帧缩放到 detection_resolution
    std::string capture_format = "MJPG";      // NV12 | MJPG | YUY2 | RGB32
    bool capture_mf_gpu = true;               // 仅 mf_capture: true=GPU 解码(nvJPEG/NPP), false=CPU 解码
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
    int depth_fps = 100;
    int depth_colormap = 18;
    // TRT OptimizationProfile 的 OPT 档输入边长(方形)。只在导出 .engine 时被用作
    // kernel autotune 最优尺寸,改完必须删旧 .engine 重新导出才生效。范围 [160, 640]。
    int depth_opt_input_size = 224;
    bool depth_show_heatmap = false;
    // 热力图 gamma 曲线。pow(d_normalized, gamma) 后再着色。
    // < 1 把暗端(远景)拉亮、亮端(近景)轻微压暗;> 1 反之。1 = 不变。
    // 只影响显示,不影响深度遮罩。范围 [0.1, 5.0]。
    float depth_heatmap_gamma = 1.0f;
    // 在独立检测预览窗口里给每个 bbox 标注相对深度(每帧 0..1)。
    bool depth_show_bbox_distance = false;
    // 深度归一化时裁掉的低/高百分位。0/100 = 纯 MIN-MAX(传统行为);
    // 把上限调到 95 可以裁掉极近离群值(贴脸的枪/手),避免远景
    // 被压扁到 depth_norm≈0、和敌人一起被遮罩误伤。范围 [0, 50] / [50, 100]。
    float depth_norm_clip_low_pct  = 0.0f;
    float depth_norm_clip_high_pct = 100.0f;
    bool depth_mask_enabled = false;
    int depth_mask_fps = 5;
    int depth_mask_near_percent = 20;
    int depth_mask_expand = 0;
    int depth_mask_hold_frames = 0;
    int depth_mask_alpha = 90;
    bool depth_mask_invert = false;
    // Fraction of bbox pixels (0..1) that must fall under the depth-derived
    // suppression mask before the detection is dropped. 0 means "suppress on
    // any single pixel" (legacy behaviour); 1 means "never suppress". 0.30
    // is a good default — drops detections that are >30% on a wall but
    // ignores edge grazing.
    float depth_mask_suppression_ratio = 0.30f;

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
    int    auto_capture_cooldown_ms = 200;
    std::vector<std::string> auto_capture_force_keys;
    std::string auto_capture_output_dir = "screenshots/auto";
    bool   auto_capture_save_label = true;

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
    int   flashlight_brightness_threshold = 220; // grey-level on max(B,G,R) [0,255]
    int   flashlight_min_radius = 5;           // min equiv-area radius (det px)
    int   flashlight_max_radius = 100;         // max equiv-area radius (det px)
    float flashlight_min_circularity = 0.60f;  // 4πA/P² floor (1.0 = perfect circle)
    int   flashlight_open_radius = 1;          // MORPH_OPEN px before CC (0=off)
    int   flashlight_min_local_contrast = 30;  // sky-rejection: inner_mean − ring_mean floor (0=off)
    // Class id the synthesized halo "detection" is filed under so the user
    // can route it via the existing per-hotkey aim_classes (priority, y-offset,
    // smart-trigger gating, etc.). -1 = unassigned (mouse loop still appends
    // it but the user must tag a class for any aim hotkey to pick it up).
    int   flashlight_target_class_id = -1;

    // ---- Glass filter (穿不透玻璃后的人形抑制) ----------------------------
    // 三角洲里有打不穿的玻璃,模型只看轮廓也会识别出后面的人,锁过去白浪
    // 费子弹。本模块在 mouse 循环里、tracker 之前,对每个 detection box
    // 的"边缘环"采样玻璃膜特征色,命中率超过阈值的 box 直接抹掉。所有
    // 工作在 CPU 完成、对每框 < 0.2 ms,延迟开销可忽略。
    //
    // 全局色带 / 环厚 / 命中阈值,启用与否在 HotkeyProfile::glass_filter_enabled。
    bool  glass_filter_show_preview = false;     // 预览叠图(框边缘画环)
    float glass_edge_ring_frac      = 0.15f;     // 环厚 ÷ 框短边 [0.05, 0.45]
    float glass_coverage_threshold  = 0.45f;     // 环内命中率 ≥ 此值 → 判玻璃
    int   glass_min_box_short_side  = 20;        // 框短边 < 此值不参与过滤
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
