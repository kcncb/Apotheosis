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

    // 经典离散 PID,X / Y 轴各一套独立的 P / I / D(共 6 个参数,每轴恰好 3 个)。
    // Kp 决定追击力度、Ki 修正常驻偏置(通常 0;Y 轴对抗后坐力时可小量加)、Kd 提供阻尼。
    // 误差(检测像素)直接喂控制器,输出 lround 后送驱动。鲁棒处理(不完全微分/反算抗饱和/
    // 积分分离/微分限幅)全部内部自适应,每轴对外仍只有 P/I/D。算法见 mouse/pid_controller.h。
    float pid_x_p = 0.6f;
    float pid_x_i = 0.0f;
    float pid_x_d = 0.05f;
    float pid_y_p = 0.6f;
    float pid_y_i = 0.0f;
    float pid_y_d = 0.05f;

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
    bool  smart_trigger_enabled = false;
    float smart_trigger_hit_scale_x = 0.60f;  // frac of bbox half-width  (X tolerance)
    float smart_trigger_hit_scale_y = 0.60f;  // frac of bbox half-height (Y tolerance)
    int   smart_trigger_reaction_ms = 40;     // dwell-on-target before first press
    int   smart_trigger_hold_ms = 45;         // left-button hold time per tap
    int   smart_trigger_cooldown_ms = 55;     // refractory after release before next tap

    float predictionInterval = 0.01f;
    int prediction_futurePositions = 20;
    bool draw_futurePositions = true;

    // 卡尔曼:精简为两个旋钮(详见 aim_kalman.h)。
    //   smoothness [0,1] — 越大越平滑抗抖,越小越紧跟检测反应快
    //   lead       [0,2] — 预测提前量,0=不预测,1≈物理正确,>1 主动多领先
    // 其余(机动自适应、延迟补偿、预热、尺寸缩放…)全部内部自动。
    bool  kalman_enabled = true;
    float kalman_smoothness = 0.5f;
    float kalman_lead = 1.0f;

    // Per-hotkey crosshair color detection toggle. Global config still owns
    // the ROI size / color palette / area filters; each hotkey just opts in.
    bool crosshair_detect_enabled = false;

    // Per-hotkey laser color detection toggle. INDEPENDENT of crosshair
    // detection: both may be on at once. When both produce a pivot the
    // crosshair (centroid) result WINS — the laser tip is only used as a
    // fallback when crosshair-colour found nothing this frame. Global config
    // owns the laser ROI / palette / params (separate from the crosshair set).
    bool laser_detect_enabled = false;

    // Lock-switch hysteresis. A challenger track must beat the current lock
    // by `lock_switch_score_margin` (fraction of half-diagonal) for at least
    // `lock_switch_min_frames` consecutive frames before the lock transfers.
    // Higher-priority class still preempts immediately. Defaults are gentle
    // — raise either to make the lock stickier when crowds overlap.
    float lock_switch_score_margin = 0.15f;
    int   lock_switch_min_frames = 3;

    // Minimum lock hold duration in frames. After a lock is acquired the
    // tracker will refuse to switch for this many frames regardless of
    // priority changes or eligibility flips. Smooths out high-recoil /
    // muzzle-flash scenarios. 0 disables (legacy behaviour).
    int   lock_hold_min_frames = 10;

    // y_offset distance/size decay. When enabled, large bboxes (close
    // targets) blend the per-class y_offset toward 0.5 (geometric centre)
    // because the same fractional offset maps to many more pixels at close
    // range, amplifying jitter. Disabled by default to preserve legacy
    // behaviour for users who tuned y_offset around close-range play.
    bool  y_offset_size_decay_enabled = false;
    float y_offset_size_decay_low_frac = 0.10f;
    float y_offset_size_decay_high_frac = 0.40f;

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
    bool  dynamic_fov_enabled = false;
    float dynamic_fov_margin_frac = 1.10f;        // bbox padding factor
    float dynamic_fov_min_radius_frac = 0.20f;    // floor as frac of base

    // Threat-weighted target priority. The threat score is a [0,1] blend of
    // (a) normalized depth at the track pivot (closer = higher threat) and
    // (b) head-class detection confidence (higher conf = higher threat).
    // `threat_depth_head_ratio` linearly mixes the two: 0 = full depth, 1 =
    // full head-conf. The result multiplies into the lock-candidate score
    // via `threat_weight` (0 = disable, 1 = full multiplicative effect).
    // `threat_head_class_id` may be -1 when no head class is selected — the
    // head-conf term then falls back to neutral 0.5.
    bool  threat_priority_enabled = false;
    float threat_weight = 0.50f;             // 0=ignore, 1=full multiply
    int   threat_head_class_id = -1;         // user-selected head class, -1 = unset
    float threat_depth_head_ratio = 0.5f;    // 0=full depth, 1=full head-conf

    // 近距离瞄头(body→head pivot 吸附)。当一个 body(非 head)目标框够大(近距离,
    // 框高 / 检测高 ≥ close_range_trigger_height_frac),且其内部上半区存在一个
    // close_range_head_class_id 的 head 检测时,把该目标的瞄准点吸附到 head 框,让贴脸
    // 大目标优先瞄头 / 上半身。只移动 pivot、不改变锁定的 track 身份,因此不会触发锁切换
    // 或卡尔曼重置。head 类别无需加入瞄准类别列表。与 y_offset_size_decay(把大框拉向
    // 中心)方向相反,建议二选一。默认关闭以保持旧行为。
    bool  close_range_head_aim_enabled = false;
    int   close_range_head_class_id = -1;          // 头部类别;-1 = 未选(功能不生效)
    float close_range_trigger_height_frac = 0.30f; // 框高 / 检测高 ≥ 此值才触发

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

    // Aim hotkeys. Must contain at least one entry so the UI always has
    // something to show; defaultConfig() populates a single "Aim" hotkey
    // bound to RightMouseButton.
    std::vector<HotkeyProfile> hotkeys;

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
