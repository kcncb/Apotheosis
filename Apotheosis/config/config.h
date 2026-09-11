#ifndef CONFIG_H
#define CONFIG_H

#include <memory>
#include <array>
#include <string>
#include <vector>

// Per-class routing bucket. The detection pipeline applies these in order:
// Delete  -> dropped right after NMS, never reaches the tracker or the UI.
// Filter  -> kept in DetectionBuffer (visible to debug UI / etc.)
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

    // AVA PIDF Mode 1。AVA 界面只暴露 Kp/Kd/Kf/LR，Ki 固定为 0。
    int pidf_mapping_version = 3;
    // 1.0 -> 2.0: 配合下面新增的链路延迟补偿(Smith 预测器)一起调出来的值。
    // 补偿让回路对延迟不敏感, 因此可以用更硬的 Kp; 两者必须配套 —— 只留 Kp=2.0
    // 而没有补偿时, 4 帧延迟下综合分会从 22.9 恶化到 67.5(见 pidf_mode1_exact.cpp)。
    float pidf_kp_x = 2.0f, pidf_kp_y = 2.0f;
    float pidf_ki_x = 0.0f, pidf_ki_y = 0.0f;
    // 前馈三件套必须配套: kf=0 会让 ff_output = ff_state*dt*kf 恒为 0, 即前馈
    // 整条关死, 此时 lr 调多少都没反应(实测: 追横移目标落后 10.4px、摆头咬不住)。
    // 默认直接给可用值; 多场景模拟综合分 37.8 -> 20.6(见 tests/aim_scenario_sim.cpp)。
    float pidf_kd_x = 0.05f, pidf_kd_y = 0.05f;
    float pidf_kf_x = 1.0f, pidf_kf_y = 1.0f;
    // 0.05 -> 0.08(延迟 <=4 帧时的实测最优); 控制器会按实测延迟自动收紧上限,
    // 所以高延迟场景不会因此变差(见 pidf_mode1_exact.cpp 的 ff_learning_rate_cap)。
    float pidf_lr_x = 0.08f, pidf_lr_y = 0.08f;
    int pidf_deadzone_x = 0, pidf_deadzone_y = 0;
    int pidf_limit_x = 0, pidf_limit_y = 0;

    // 用户 AimPath：在 AVA PIDF 输出后执行轨迹整形。
    int   aim_path_mode = 0;
    int   aim_path_influence = 25; // 0..100，仅影响 PIDF 主方向
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
    // ─────────────────────────────────────────────────────────────────────

    // 检测暂时丢失时继续保留同一 track 的帧数。期间不发送旧坐标移动，
    // 只等待同一目标重新出现；0 = 当帧丢失即释放，默认 5 与旧行为一致。
    int   lost_target_cache_frames = 5;

    // ─────────────────────────────────────────────────────────────────────
    // 扳机 — 5 态状态机 (idle/delay/pressed/cooldown/switch_cd)。
    //   trigger_fire_delay:    进入命中区后延迟 N ms 才按下(0=立即)
    //   trigger_fire_duration: 单次按住时长上限。
    //                          0 = 【长按模式】: 按住不松手, 直到准星离开命中区
    //                              (目标真的丢了也会松手), 不做"按-松"循环。
    //                          >0 = 连点模式: 按住 N ms → 松手 → 冷却 interval
    //                              → 若仍在命中区再按, 即老的点射行为。
    //   trigger_fire_interval: 连点模式的冷却间隔 / 长按模式离开命中区后的
    //                          最短重按间隔(防止在判定边缘反复按松)
    //   trigger_y_percent:     命中区占 bbox 的百分比 (100=整框, >100=预开火)
    //   trigger_*_jitter_ms:   对应延迟的随机 ±N ms 抖动(破除机械感)
    //   trigger_switch_cooldown_ms: 目标 track_id 变化时的转火冷却
    // ─────────────────────────────────────────────────────────────────────
    bool  trigger_enabled = false;
    int   trigger_fire_delay = 0;
    int   trigger_fire_duration = 0;
    int   trigger_fire_interval = 200;
    int   trigger_y_percent = 100;
    int   trigger_delay_jitter_ms    = 0;
    int   trigger_duration_jitter_ms = 0;
    int   trigger_interval_jitter_ms = 0;
    int   trigger_switch_cooldown_ms = 0;


    // ─────────────────────────────────────────────────────────────────────
    // 目标选择 — 按优先级排序的类别列表。列表顺序 = 优先级 (index 0 最高)。
    //   aim_classes[i].class_id: 该条目匹配的 class_id
    //   aim_classes[i].y_offset..y_offset_max: AVA 瞄点稳定范围
    //   aim_classes[i].min_conf: 该类别的最低置信度 (0=不过滤)
    // 优先级优先, 距离次之; 未在列表内的 class 被忽略。类别只能加一次
    // (来源: Target 页 "瞄准" 桶), 通过 UI 拖动整行调整上下顺序。距离上限
    // 已由 FOV 椭圆(HotkeyProfile::fovX/fovY)承担, 不再单独设。
    // ─────────────────────────────────────────────────────────────────────
    std::vector<HotkeyAimClass> aim_classes;

    // Per-hotkey crosshair color detection toggle. Global config still owns
    // the ROI size / color palette / area filters; each hotkey just opts in.
    bool crosshair_detect_enabled = false;


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
    // ── 采集方式: 只有一种 ──
    // 全程序只有「采集卡」这一条采集路径。参数全部来自设备真实能力探测,
    // UI 用 格式 / 分辨率 / 帧率 三级联动下拉让用户从中选, 不再手填。
    //
    // 关键: 这里存的组合【必须】是设备真实支持的。采集侧不做任何替换 ——
    // 对不上就直接报错, 而不是悄悄换个"差不多的"模式跑起来。
    std::string capture_device;   // 设备 friendly name (index 随插拔变化, 名字不会)
    std::string capture_format;   // NV12 | MJPG | YUY2 | RGB32
    int  capture_width  = 0;
    int  capture_height = 0;
    int  capture_fps    = 0;
    bool capture_gpu_decode = true;

    // 没有 capture_crop: 中心裁切恒等于模型输入边长(detection_resolution),
    // 保证送进模型的永远正好是模型要的尺寸 —— 不多裁, 也不少裁再缩。
    int detection_resolution = 320;
    bool circle_mask = true;

    // Hardware
    std::string input_method = "MAKCU"; // MAKCU | MAKCUNEW
    int makcu_baudrate = 115200;
    std::string makcu_port = "COM0";
    int makcu_new_baudrate = 6000000; // 固件上限; 协商失败自动退回 115200
    std::string makcu_new_port = "COM0";

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
    bool fixed_input_size = false;

    // CUDA / System
    bool use_cuda_graph = true;
    // 双缓冲流水线: 用第 N+1 帧的 GPU 推理去重叠第 N 帧的 CPU 后处理。
    //
    // 代价是【整整一帧延迟】: 检测结果的发布被门控在"下一帧到达"上(后处理块
    // 在 hasNewFrame 分支内, post_slot 取 prev_slot), 所以帧间隔多长就多等多久
    // —— 120fps = +8.33ms, 60fps = +16.7ms。
    //
    // 默认关闭, 理由:
    //   · 它换来的吞吐只在 GPU 链 + CPU 后处理逼近帧预算时才有意义。实测
    //     infer≈0.5ms, 相对 120fps 的 8.33ms 预算有整个数量级的余量, 属于白付一帧。
    //   · 旧注释称"这一帧延迟远低于采集抖动(~8ms@120fps)"—— 8ms 就是 120fps 的
    //     整个帧间隔, 它并不"远低于"任何东西。
    //   · 旧注释称"下游 Kalman 预测会补偿"—— 但预测通路实际是关闭的(tracker 的
    //     predicted_center_valid 恒为 0, 且 PIDF 的 LR/KF 默认为 0), 没有任何东西
    //     在补偿这一帧。
    // 注意: 它与 CUDA Graph 现在可以共存(每槽一张图), 需要吞吐时在界面打开即可。
    bool use_double_buffer = false;
    int gpuMemoryReserveMB = 2048;
    bool enableGpuExclusiveMode = true;
    int cpuCoreReserveCount = 4;
    int systemMemoryReserveMB = 2048;


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
