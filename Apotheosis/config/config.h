#ifndef CONFIG_H
#define CONFIG_H

#include <memory>
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

    // ── 【2026-09-17 整条删除】瞄准控制链的配置槽位 ─────────────────────────
    // 整个瞄准控制链(aim_pid / boss_aim / aim_tracker / aim_scale / aim_path /
    // anchor_filter / auto_stop / trigger_scope / autotune_*)连同
    // runtime/mouse_thread_loop.cpp 一起被删除, 程序现在只做【采集 → 推理】。
    // 因此下列槽位再没有任何消费者, 字段本身也删掉了:
    //   pidf_mapping_version (迁移机制本身 —— 槽位没了, 没有东西可迁移)
    //   pidf_kp_*/pidf_ki_*/pidf_kd_*/pidf_psat_*/pidf_limit_*
    //   pidf_predict_*
    //   pidf_inflight_x/y 与 pidf_inflight_window_ms
    //   esync_min_hits/assoc_iou/vel_sample_ms/pred_*
    //   aim_scale_enabled/max/min/base_h
    //   aim_path_*/trigger_*
    // 老配置里这些键仍然存在, 加载时【读都不读】(不报错), 下次保存时自动消失。
    //
    // ── 【2026-09-14 整条删除】aim_px_per_count_x/y (每计数像素 k̂) ──────────
    // 它曾经是前馈(延迟预测/提前量)的必需输入, 也是那个「测量」按钮要测的东西。
    // 前馈删除后它只剩配置往返, 控制链 0 引用, 所以字段本身也删掉了。
    //
    // ★ 为什么不保留成"读到就忽略"的兼容槽位(与别的停用字段不同):
    //   保留这个键会【诱导】后来的人重新把 k̂ 接回控制器。而本项目是双机架构
    //   (游戏机 --HDMI--> 采集卡 --USB--> 采集机), k̂ 是【游戏机的属性】, 采集机
    //   只能从画面反推, 两条可行的反推路径都已实测证伪(见
    //   )。控制器必须在不依赖 k̂ 的前提下工作
    //   —— 配置里根本不该存在这个量, 免得又有人去"标定"它。
    //   老配置里的这两个键读进来会被忽略, 写回时不再出现。

    // 瞄点平滑原本由 mouse/anchor_filter.h 的 α-β 滤波器负责(紧挨在 PID 之前);
    // 两者都随瞄准控制链一起删除了, 这里只留个记号, 免得后来的人以为它还在。

    // ── 【2026-09-17 删除】尺度增益调度 / 瞄准轨迹曲线 / 扳机 ────────────────
    //   aim_scale_*   (mouse/aim_scale.h 的 s(bbox.height) 增益调度)
    //   aim_path_*    (mouse/aim_path.h 的直线/贝塞尔/手绘/WindMouse 整形)
    //   trigger_*     (mouse_thread_loop.cpp 的扳机 FSM + trigger_scope.h 的自动开镜
    //                  + auto_stop.h 的自动急停)
    // 这三组槽位的消费者全部随控制链删除, 字段本身也删掉了。老配置里的这些键
    // 读都不读(不报错), 下次保存时自动消失。

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

    // ─────────────────────────────────────────────────────────────────────
    // ★★ 通用控制器层 (2026-09-17 第三轮重建)
    //
    // 公式见 control/pid_controller.h。
    // 数值单位: 增益是【计数/(像素·秒)】, 时是【秒】。
    // ★★ 六个增益【全部分方向】—— 跟枪(x)与压枪(y)的需求不同。
    //
    // ★ 默认值刻意【等价于历史单套行为】(Kp=35, 其余 0):
    //   这样"从单套改成六套"这个动作本身不改变任何行为, 是安全的起点。
    //   具体该填多少【没有任何实测依据】, 由用户在实机上调
    //   (方案 §7 第 10 条: 这是重建控制链里最大的一笔实测工作量)。
    // ─────────────────────────────────────────────────────────────────────
    double ctl_kp_x = 35.0;
    double ctl_kp_y = 35.0;
    double ctl_ki_x = 0.0;
    double ctl_ki_y = 0.0;
    double ctl_kd_x = 0.0;
    double ctl_kd_y = 0.0;

    // 积分回吐时间常数(秒)。误差反向时积分按 exp(-dt/τ) 快速回吐。
    // ★★ 历史值 0.2s 在 AD 急停(2-3Hz, 变向间隔 330-500ms)下【太慢】,
    //    收紧到 30ms 起步。⚠️ 这个值没有实测依据, 由用户实调(方案 §7 第 7 条)。
    double ctl_tau_unwind_sec = 0.030;
    // D 项低通时间常数(秒)。零惯性下目标急停会让 de 出现尖峰,
    // 而 Kd 会把准星朝目标原来运动的方向猛推 —— 这就是 AD 急停超调的成因。
    // ⚠️ 无实测依据(方案 §7 第 8 条)。
    double ctl_tau_deriv_sec = 0.020;
    // 积分上限。0 = 用输出限幅作为上限。⚠️ 无实测依据(方案 §7 第 9 条)。
    double ctl_i_max = 0.0;
    // 输出限幅(计数/拍)。0 = 内置 200。
    int    ctl_max_output_counts = 200;
    // P 项连续饱和(像素)。误差超过此值时 P 项不再增大。
    // ★ 它取代了死区 —— 连续且经过原点, 不存在"停了"这个状态。
    // ★★ 死区已【整项删除】, 不要加回来(方案 §5.1)。
    double ctl_p_full_scale_px = 0.0;

    // 瞄点: 框内相对位置。1.0 = 框顶, 0.5 = 中心, 0.0 = 框底。
    // y_offset_max > y_offset 时每帧在两者之间随机取, 避免总打同一点。
    double ctl_y_offset = 0.5;
    double ctl_y_offset_max = 0.5;

    // 选靶滞回倍数。已锁定目标时, 新候选必须比它近这个倍数才切换。
    // ★ 不加滞回 ⇒ 两个目标交替最近 ⇒ 每帧换目标 ⇒ 滤波每帧复位 ⇒ 滤波白做。
    // ⚠️ 取值无实测依据(方案 §7 第 5 条, 建议从 1.3 起调)。
    double ctl_hysteresis_ratio = 1.3;

    // ★★ 总开关。默认【关】—— 新增的控制链在真机验证过之前不该自己动鼠标。
    //   打开后控制器会真的下发位移(通过 MouseThread::sendRawMove)。
    bool ctl_enabled = false;

    // 选靶距离上限(检测像素)。0 = 不限制。
    // ⚠️ 默认 0: 距离门控目前由 FOV 椭圆承担, 不在这里重复设一道。
    double ctl_max_distance_px = 0.0;

    // ── 检测稳定器(②)──────────────────────────────────────────────────
    // ★ 这 5 项此前【只存在于 control/ 的默认值里, 没有配置槽位】——
    //   等于"写死在代码里, 谁都调不了"。现在补上, 让它们可调。
    //   ⚠️ 全部没有实测依据(方案 §7 第 6 条), 当前值是占位。
    // 认目标: 中心距离小于"上一帧框对角线 × 此系数"算同一个目标。
    double ctl_match_center_ratio = 0.5;
    // 面积容差倍数: 面积比超出 [1/tol, tol] 判为换目标。
    double ctl_area_ratio_tol = 2.0;
    // 突变系数: 位移超过"上一帧对角线 × 此系数"判为瞬移。
    double ctl_k_snap_mult = 1.15;
    // 宽高比合理区间(剔除细长误检)。
    double ctl_min_aspect = 0.2;
    double ctl_max_aspect = 5.0;

    // 瞄点随机种子。0 = 用内部固定常数(同一帧可复现)。
    // ★ 非 0 时每局随机, 让 y 偏移的抖动不可预测。
    int ctl_random_seed = 0;

    // ─────────────────────────────────────────────────────────────────────
    // ★★ 自动扳机 (2026-09-17 恢复)
    //
    // 这一组在 2026-09-17 那轮"只留采集+推理"里随 mouse_thread_loop.cpp 一起
    // 被删除(连同触发它的 FSM)。现按用户要求重建 —— 后端在
    // `mouse/trigger_fsm.h`, 接线在 `runtime/aim_loop.cpp`。
    //
    // ★ 语义与旧版一致: 命中区 = 准星落在检测框水平中心
    //   附近、且纵向位于框的 trigger_y_percent 高度处。准星进区就开火。
    // ─────────────────────────────────────────────────────────────────────
    bool trigger_enabled = false;
    // 进入命中区后延迟 N ms 才按下(0=立即)。想"停稳再开枪"就调大它。
    int  trigger_fire_delay = 0;
    // 单次按住时长上限(ms)。0 = 用连点模式的 hold 常数。
    int  trigger_fire_duration = 0;
    // 连点模式的冷却间隔 / 长按模式离开命中区后的再触发间隔(ms)。
    int  trigger_fire_interval = 200;
    // 命中区占 bbox 的百分比(纵向): 100=整框, >100=框上方也预开火。
    int  trigger_y_percent = 100;
    // 三个延迟各自的随机 ±N ms 抖动(破除机械感)。
    int  trigger_delay_jitter_ms    = 0;
    int  trigger_duration_jitter_ms = 0;
    int  trigger_interval_jitter_ms = 0;
    // 目标身份变化时的转火冷却(ms)。
    int  trigger_switch_cooldown_ms = 0;
    // 自动开镜: 0 关 / 1 点按右键一下(不收镜) / 2 长按右键。
    int  trigger_auto_scope = 0;
    // 开镜后等多久才允许开火(ms) —— 保证第一颗子弹是开着镜打出去的。
    int  trigger_scope_delay_ms = 0;
    // 自动急停: 开火那一拍若玩家按着 WASD, 就补一个反方向键的短按。
    // ★ 只有 MAKCUNEW 有键盘通道; 其它输入方式整项跳过。
    int  trigger_auto_stop = 0;    // 0 = 关, 1 = 开
    int  trigger_stop_ms   = 60;   // 反方向键的短按时长(ms), 20~300

    // ─────────────────────────────────────────────────────────────────────
    // ★★ 瞄准轨迹曲线 (2026-09-17 恢复)
    //
    // 同一轮里被删(mouse/aim_path.h)。现恢复为 `mouse/aim_path.h`, 接线在
    // `runtime/aim_loop.cpp`。★ 四种模式【只旋转不缩放】控制器原始输出 ——
    // 曲线只提供局部切线方向, 幅值仍由 PID 决定。
    // ─────────────────────────────────────────────────────────────────────
    // 0 直线 / 1 贝塞尔 / 2 自定义手绘 / 3 WindMouse(风力曲线)
    int   aim_path_mode = 0;
    // 曲线对 PIDF 主方向的影响量。0=完全透传, 1=完整曲线切线。
    int   aim_path_influence = 25;   // 0..100
    float aim_path_bezier_cx1 = 0.30f;
    float aim_path_bezier_cy1 = 0.00f;
    float aim_path_bezier_cx2 = 0.70f;
    float aim_path_bezier_cy2 = 0.00f;
    // ── WindMouse 曲线 (aim_path_mode = 3) ─────────────────────────────────
    // 单位都是像素, 与 AimMagic 的 wind_mouse_G0/W0/M0/D0 同名同量纲。
    float aim_path_wind_gravity   = 5.0f;   // 重力: 越大越坚决, 路径越直
    float aim_path_wind_wind      = 2.0f;   // 风力: 越大越飘
    float aim_path_wind_step      = 10.0f;  // 单步最大长度: 越小路径越碎
    float aim_path_wind_distance  = 8.0f;   // 风力衰减距离: 越近风越小
    // 门控(px): 两轴误差都不超过它时整段曲线旁路(AM 的 curve_threshold)。
    int   aim_path_wind_threshold = 10;
    // 自定义手绘曲线采样点(由 UI 画, 运行时只读)。空 = 用直线。
    std::shared_ptr<const std::vector<float>> aim_path_custom_samples;

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

// ★ 2026-09-17: 检测上限固定为 20, 不再是用户可调项。
//   理由: 模型是 end2end 形态([1,N,6]), 每帧成品框本来就很少; 下游(预览/选靶)
//   只需要极少数目标。把它做成 1~100 的滑块只会让人误以为"调大能检出更多",
//   而实际上多出来的框在下游全被丢弃。
//   ★ 它同时是 UI 与配置读写的唯一来源 —— 不要再从 config.ini 读。
inline constexpr int kFixedMaxDetections = 20;

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
    // MAKCU | MAKCUNEW | KMBOXNET
    //
    // ★ 三档走同一套驱动抽象 (mouse/mouse_driver.h, 形状移植自 AimMagic 的
    //   驱动工厂 + vtable)。选哪一档由**名字字符串**决定, 和 AM 一样 ——
    //   但 AM 那九个后端是本项目没有的硬件(KMBox B+ / Ferrum / ProBox…),
    //   所以只接了本项目真正支持的三家。
    //
    // ★ 老配置的取值 "MAKCU"/"MAKCUNEW" **语义不变**, 所以这一项不涉及迁移;
    //   新增的 "KMBOXNET" 只是多一个可选值, 老配置不会自己变成它。
    std::string input_method = "MAKCU";
    // ── 【2026-09-13 删除】capture_age_offset_ms (界面: 采集回调前帧龄(估计)) ──
    // 它是个纯手填的猜测值, 用来在延迟遥测的时间戳上再减一刀。两个理由删掉它:
    //   1. 它不参与任何控制 —— 控制器是纯反馈, 不吃任何延迟估计;
    //   2. 它是"猜"不是"测"。真实的端到端延迟由 latency_probe 逐帧实测并落进
    //      延迟日志(那个保留), 再留一个手填的猜测值只会让人误以为它有意义。
    // 注: 曾有 mouse_pixels_per_count_x/y 与 mouse_effect_delay/uncertainty_ms 四项,
    // 供重构版 predictive_controller 扣除自身在途指令的像素位移。现役控制链全程在
    // 鼠标计数域(不需要像素<->计数换算), 那四项已删除。
    int makcu_baudrate = 115200;
    std::string makcu_port = "COM0";
    int makcu_new_baudrate = 6000000; // 固件上限; 协商失败自动退回 115200
    std::string makcu_new_port = "COM0";
    // ── KMBox Net (以太网 UDP, 2026-09-15 恢复) ──
    // 盒子屏幕上会显示这三个值, 照抄填进来即可。注意 **IP 不是盒子自己配的**,
    // 是出厂/上次 setconfig 写进去的, 所以换了网络环境要先核对屏幕。
    std::string kmbox_net_ip = "192.168.2.88";
    std::string kmbox_net_port = "6234";   // 字符串: 协议里当端口号用 atoi 解析
    std::string kmbox_net_uuid = "12345";  // 盒子的 MAC 标识(屏幕上有), 不是标准 UUID 格式

    // AI
    // ★ 2026-09-17: backend 恒为 "TRT"。DirectML 后端整条移除
    //   (dml_detector + DirectML.dll + dml_device_id), 所以这里不再是"用户可选"。
    //   老配置里的 backend = DML / dml_device_id 读都不读(不报错), 下次保存时消失。
    std::string backend = "TRT";
    std::string ai_model = "sunxds_0.5.6.engine";
    float confidence_threshold = 0.15f;
    float nms_threshold = 0.50f;
    // 固定为 kFixedMaxDetections (=20), 不可用户设置, 见上面的说明。
    int max_detections = kFixedMaxDetections;
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
    // 主机侧同步方式: 自旋等待 GPU 事件完成, 而不是 cudaEventSynchronize 的
    // 内核态阻塞等待。
    //
    // 动机 (取自原神AI 的 host_wait_spin=true): cudaEventSynchronize 走内核
    // 事件对象 + 线程唤醒, 唤醒延迟 10~40us, 且在 CPU 负载高时会被调度器
    // 推迟 —— 这部分是【尾部抖动】而不是均值, 而体感由尾部决定。
    // 自旋版用 cudaEventQuery 轮询, 完成即返回, 代价是拿一个核在转。
    //
    // 默认开启: 推理线程本来就是专用线程, 且 8.8ms 的推理里有 ~2ms 是等待,
    // 这段时间自旋不会挤占任何有用的工作。
    bool use_spin_wait_sync = true;
    // 单次同步的自旋上限 (ms), 防止 GPU 掉卡/上下文丢失时死转。
    // 超过后回退到一次阻塞式 cudaEventSynchronize (它有自己的超时语义)。
    int spin_wait_timeout_ms = 50;

    // 进程/线程调度特权 (取自原神AI 的 process_qos/thread_qos/mmcss/priority)。
    //
    // 原神日志:  process_priority=0x8000 (= HIGH_PRIORITY_CLASS)
    //            mmcss=true, thread_qos=true, timer=true
    //
    // 为什么需要: Windows 上普通优先级线程被 CPU 抢占一次就是 1~5ms, 足以把
    // 8.8ms 的推理拖成 12ms。这些设置不提高吞吐, 只消除【调度抖动】。
    //
    // 默认开启, 但都带失败回退 —— 权限不足(MMCSS 需要线程句柄权限)时只打日志
    // 不中断启动。
    bool use_process_boost = true;         // HIGH_PRIORITY_CLASS
    bool use_mmcss = true;                 // 推理线程注册 MMCSS
    // MMCSS 任务名。原神用的是 Games 类; "Games" 在 Win10+ 映射到
    // "\Games\Games"。留空则用该默认值。
    std::string mmcss_task_name = "Games";

    // ─ 控制侧: 检测间歇期【不做】预测推进 (PID-EventSync 语义) ────────────────
    //
    // AimMagic 的控制线程在"没有新推理"时也会推进一步(用跟踪器的预测分支)。
    // 但那是它【另一个档】(kalman/FrameSync) 的行为; 它的 PID-EventSync 档
    // 是"每次推理只消费一次", 两次推理之间不发任何位移。
    //
    // 本项目现在只有 EventSync 这一条链路, 所以: 没有新检测就 `continue`,
    // 控制节拍 = 检测节拍。原先的三个键(use_prediction_tick / prediction_tick_hz /
    // prediction_tick_max_run)与对应的外推分支已于 2026-09-16 一起删除 ——
    // 两条外推同开会叠加成两层提前量, 与 PID-EventSync 的定义直接冲突。


    // ── 【2026-09-17 删除】双缓冲流水线 (use_double_buffer) ──────────────────
    // 原来是"用第 N+1 帧的 GPU 推理去重叠第 N 帧 CPU 后处理"的开关。
    // 整个机制连同配置键一起删除, 理由:
    //   · 代价是【整整一帧延迟】(120fps = +8.33ms, 60fps = +16.7ms), 与
    //     "降低推理延迟"的目标正好相反;
    //   · 它换来的吞吐只在 GPU 链 + CPU 后处理逼近帧预算时才有意义, 而实测
    //     推理相对帧预算有整数量级余量 —— 属于白付一帧。
    // 现在推理恒为单缓冲: 结果一就绪就发布。CUDA Graph 与它无关, 仍然保留。
    // 老配置里的 use_double_buffer 键读都不读, 下次保存时自动消失。
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
    // ── 【2026-09-13 删除】crosshair_smooth (准星枢轴的 One-Euro 自适应平滑) ────
    // 原来这里是一道自适应时间滤波, 默认 0.5, 而且实现里还规定"填 0 也不许关"。
    //
    // 删掉的理由: 现在瞄点已经有一道 α-β 框平滑(mouse/anchor_filter.h)在 PID 之前。
    // 准星这里再叠一层就是【两道串联滤波】—— 各自引入滞后、参数互相耦合, 而能滤掉
    // 的东西是一样的。保留 α-β 是因为它就在控制器门口, 且对"匀速目标不落后"有解析保证。
    // 准星找色检测本身【保留】, 输出不再被时间平滑。旧 ini 里的这个键会被忽略。

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

    // 只改「后续 saveConfig() 写哪儿」, 不重读任何值。
    // 用途: 全局配置方案的落盘目标就是当前方案文件(见 config.cpp 的
    // config_path 重定向)。当那份方案在外部被删掉时, 必须把目标挪回来,
    // 否则下一次自动保存会把用户刚删掉的文件凭空写回来。
    void retargetConfigPath(const std::string& filename);

    // 读回当前落盘目标。★ 与 retargetConfigPath() 配对, 存在的理由是
    // 【loadConfig() 有副作用】: 它会把 config_path 改成传进去的那个路径
    // (见 config.cpp)。于是任何"读一份副本再应用"的调用方(live_tune / 调参
    // agent)都会顺手把落盘目标改掉, 让之后所有 saveConfig() 写进副本而不是
    // 用户激活的方案 —— 实测就是这么让"配置文件里是 15, 实际在跑 6.2"的。
    //   有了这对存取器, 那些调用方就能"解析前存下、解析后还原", 副作用被
    //   限制在那一次解析里, 不再泄漏到全局。
    std::string configPath() const { return config_path; }
    void setConfigPath(const std::string& path) { config_path = path; }

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
