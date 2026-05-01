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

    // Per-class Kalman override. Heads have a much higher acceleration
    // density than torsos (a flicked head movement covers far more bbox
    // diameters per frame than a torso shift covering the same pixel count),
    // so a one-size-fits-all process/measurement noise pair is a poor fit
    // when both classes share a hotkey. When `kalman_override_enabled` is
    // true the five fields below replace the matching HotkeyProfile values
    // for any track of this class. The remaining Kalman knobs (warmup,
    // delay-compensation, reset timeout) stay at the hotkey level.
    bool  kalman_override_enabled = false;
    float kalman_process_noise_position = 40.0f;
    float kalman_process_noise_velocity = 1800.0f;
    float kalman_measurement_noise = 35.0f;
    float kalman_velocity_damping = 0.08f;
    float kalman_max_velocity = 20000.0f;
};

// A single aim hotkey. Multiple HotkeyProfiles can exist; whichever one has
// any of its keys pressed wins. If several are pressed simultaneously, the
// one earlier in Config::hotkeys wins. Every hotkey carries its own full set
// of mouse parameters — there is no longer a "global mouse" to fall back to.
struct HotkeyProfile
{
    std::string name = "Aim";
    std::vector<std::string> keys;                    // any-of: pressing any triggers the profile
    std::vector<HotkeyAimClass> aim_classes;          // priority-ordered

    int fovX = 106;
    int fovY = 74;

    float pid_p = 0.6f;
    float pid_p_x = 0.6f;
    float pid_p_y = 0.6f;
    float pid_i = 0.0f;
    float pid_d = 0.05f;

    // Flick / Track dual-mode PID. When `flick_track_enabled` is true the
    // controller switches between two gain sets based on the magnitude of
    // the pivot-to-crosshair error in detection-pixel space:
    //   - Far (err >= flick_track_threshold_px + hysteresis) → Flick gains
    //     (pid_p/i/d above) — aggressive, designed for big swings.
    //   - Near (err <= flick_track_threshold_px - hysteresis) → Track gains
    //     (the *_track values below) — gentle, designed to hold on target.
    // The hysteresis band prevents the controller from oscillating between
    // modes on the boundary. When disabled, only the Flick gains are used,
    // matching legacy behaviour exactly.
    bool  flick_track_enabled = false;
    float pid_track_p = 0.30f;
    float pid_track_p_x = 0.30f;
    float pid_track_p_y = 0.30f;
    float pid_track_i = 0.0f;
    float pid_track_d = 0.10f;
    float flick_track_threshold_px = 30.0f;
    float flick_track_hysteresis_px = 8.0f;

    // Smart trigger — automatic fire. When enabled, the controller actually
    // dispatches a left-button press (whichever input driver is bound) the
    // moment all three gates open simultaneously:
    //   (a) pivot inside the locked bbox shrunk by `hit_radius_frac`
    //       (hit-probability threshold below)
    //   (b) recent mouse-step RMS magnitude below `variance_max_px`
    //       (gun is settled, not still flicking)
    //   (c) cooldown elapsed since the last release.
    // The press is held for `fire_duration_ms`, then released. After
    // release a refractory period of the same length is enforced before
    // another fire can trigger — caps duty cycle at 50% so the system
    // always behaves like a controlled tap, not a stuck button. If the
    // hotkey is released or the toggle goes false mid-fire, the button is
    // force-released so it can never get stuck down.
    bool  smart_trigger_enabled = false;
    float smart_trigger_hit_radius_frac = 0.55f; // fraction of bbox half-size
    float smart_trigger_variance_max_px = 6.0f;  // RMS px over recent steps
    int   smart_trigger_window_frames = 8;       // window for variance calc
    float smart_trigger_min_prob = 0.70f;        // hit-probability threshold
    int   smart_trigger_fire_duration_ms = 40;   // press hold time per tap

    float predictionInterval = 0.01f;
    int prediction_futurePositions = 20;
    bool draw_futurePositions = true;

    bool kalman_enabled = true;
    float kalman_process_noise_position = 40.0f;
    float kalman_process_noise_velocity = 1800.0f;
    float kalman_measurement_noise = 35.0f;
    float kalman_velocity_damping = 0.08f;
    float kalman_max_velocity = 20000.0f;
    int kalman_warmup_frames = 2;
    bool kalman_compensate_detection_delay = true;
    float kalman_additional_prediction_ms = 0.0f;
    float kalman_reset_timeout_sec = 0.5f;

    // 吸附锁死力度 [0.0, 1.0]。0 = 关闭；越大，目标越靠近准星时单帧
    // PID 输出上限越低，形成类似"磁吸"的锁死效果。算法：使用
    //   strength * R^2 / (R^2 + err^2)
    // 在正常步长和近距步长之间插值。R 取 detection_resolution 的 12%。
    float aim_lock_strength = 0.0f;

    // Per-hotkey crosshair color detection toggle. Global config still owns
    // the ROI size / color palette / area filters; each hotkey just opts in.
    bool crosshair_detect_enabled = false;

    // Lock-switch hysteresis. A challenger track must beat the current lock
    // by `lock_switch_score_margin` (fraction of half-diagonal) for at least
    // `lock_switch_min_frames` consecutive frames before the lock transfers.
    // Higher-priority class still preempts immediately. Defaults are gentle
    // — raise either to make the lock stickier when crowds overlap.
    float lock_switch_score_margin = 0.15f;
    int   lock_switch_min_frames = 3;

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

    // Threat-weighted target priority. This is the production heuristic:
    // class score (user-selected head/body/other tiers), motion direction
    // relative to crosshair (closing -> threatening), and hit-frame count.
    // The score multiplies into lock candidate ranking. Set weight=0 to
    // disable entirely (legacy ranking). Class ids may be -1 when unselected.
    bool  threat_priority_enabled = false;
    float threat_weight = 0.50f;             // 0=ignore, 1=full multiply
    int   threat_head_class_id = -1;         // user-selected head class, -1 = unset
    int   threat_body_class_id = -1;         // user-selected body class, -1 = unset

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
    int opencv_capture_index = 0;
    std::string opencv_capture_api = "DSHOW"; // DSHOW | MSMF | FFMPEG | ANY
    std::string opencv_capture_url;           // optional connection URL (e.g. rtsp://...)
    int opencv_capture_width = 0;             // 0 = let device decide
    int opencv_capture_height = 0;
    int opencv_capture_fps = 0;
    std::string opencv_capture_format = "AUTO"; // AUTO | NV12 | MJPG | YUY2 | YUYV | RGB3 | BGR3
    int opencv_capture_crop_width = 0;        // 0 = use full captured width
    int opencv_capture_crop_height = 0;       // 0 = use full captured height
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
    bool export_enable_fp8 = false;
    bool export_enable_fp16 = true;
    bool fixed_input_size = false;

    // CUDA / System
    bool use_cuda_graph = false;
    // Double-buffer pipeline: overlap CPU post-processing of frame N with
    // GPU preprocess+inference of frame N+1. Trades ~1 frame of latency for
    // throughput. Disables CUDA Graph path when enabled (simpler code path).
    bool use_double_buffer = false;
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
    int crosshair_min_area = 2;
    int crosshair_max_area = 200;
    std::vector<CrosshairColorProfileConfig> crosshair_colors; // defaults to red double-band

    // Aim hotkeys. Must contain at least one entry so the UI always has
    // something to show; defaultConfig() populates a single "Aim" hotkey
    // bound to RightMouseButton.
    std::vector<HotkeyProfile> hotkeys;

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
