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

    // ── 瞄准控制器 (mouse/aim_pid.h) ────────────────────────────────────────
    // 旧 AVA PIDF 管线已整条删除, 这几个槽位沿用下来但语义已换, 单位都带时间量纲
    // (u = dt*Kp*[e + Ki*∫e + Kd*e'], 所以换帧率不用重调):
    //   pidf_kp_*   Kp 计数/(像素*秒) —— 回路速度。唯一与游戏灵敏度挂钩的旋钮:
    //              手感拖沓就往上加(每次 +50%), 开始抖/画圈就退回来。
    //   pidf_ki_*   Ki 1/秒 —— 积分速率(Ti = 1/Ki), 磨掉匀速目标的滞后与残余偏置。
    //   pidf_kd_*   Kd 秒 —— 微分时间, 压过冲。检测框抖动在微分眼里是尖峰, 所以默认很小。
    //   pidf_psat_* P项饱和阈值 像素(0 = 关, 默认关: 见字段处说明)
    //   pidf_limit_*    每拍计数上限(0 = 内置 200)
    // 默认值由闭环回归(每计数 0.25 像素 + 50ms 死区)扫出来的稳定边界决定, 见
    // tests/aim_pid_test.cpp。
    int pidf_mapping_version = 6;
    float pidf_kp_x = 35.0f, pidf_kp_y = 35.0f;
    float pidf_ki_x = 1.0f, pidf_ki_y = 1.0f;
    float pidf_kd_x = 0.0f, pidf_kd_y = 0.0f;

    // ── P 项饱和阈值 (原「移动死区」, 2026-09-14 改名并重新界定) ─────────────
    // 历史: 这里曾经是【死区宽度】(像素), 死区本身已整段删除 —— 它会让误差在瞄点
    // 周围永久丢精度, 还会制造 10 次/秒的输出抖动。字段随后被【借去】当 P 项饱和
    // 阈值用, 但界面一直叫"移动死区", 造成名实不符。
    //
    // 现在正式改名。语义: |误差| <= 此值(px) 时 P 项满增益, 超过则按 此值/|误差|
    // 连续衰减(经过原点, 无硬切换)。0 = 关闭饱和(默认, 生产用它)。
    //
    // 实测(400px 阶跃, Kp 扫描): 饱和对过冲的影响【取决于 Kp】——
    //      Kp 15/25: 饱和让过冲【变差】(12.1→20.4 / 11.0→21.6)
    //      Kp 35+  : 饱和让过冲【变好】(43.6→28.2 / 145.6→50.1)
    // 转折点在 Kp≈30。生产 Kp=35 恰好压在转折点右侧, 所以默认关闭(少一个非线性
    // 环节, 行为更可预测)。★ 改这一段必须重做那张扫描表。
    int pidf_psat_x = 0, pidf_psat_y = 0;

    // 输出限幅: 每个控制周期最多下发的鼠标计数。0 = 用内置上限(200)。
    int pidf_limit_x = 0, pidf_limit_y = 0;

    // ── 预测补偿 (2026-09-13 重做) ──────────────────────────────────────────
    //
    // 这是 **AimMagic 1.0.30「预测补偿」那一页的等价实现**, 公式逐项对齐逆向报告
    // (依据见 mouse/aim_predict.h 顶部)。做法是: 把"目标在这一拍里会走到哪"算出来,
    // 直接加到瞄点上, 再送去 PID —— 所以 PID 追的是一个"将要到"的位置。
    //
    // 原式 (AM 两处代码路径互证):
    //     sizeWeight = (maxW - w) / (maxW - minW)      // minW < w < maxW, 否则 0
    //     predX      = factor_x * motionX * sizeWeight
    //     k          = (predX * predXPrev < 0) ? K_damp : 1.0   // 方向翻转阻尼
    //     box.cx    += (1-k)*predXPrev + k*predX       // ★ 直接改写框心
    //
    // ★ 与之前那套"在途补偿"的区别(那套已删除, 不是 AM 也不是原神的做法):
    //     预测: 改【目标在哪】—— 输出仍然是 误差→PID→计数;
    //     在途: 改【PID 怎么算】—— 把未落地的计数从误差里扣掉。
    //   两者不叠加, 现在只保留预测。
    //
    // ★★ 2026-09-14 重做: 系数范围从 0..100 收到 0..0.2, 并加【硬像素上限】★★
    //
    // 原范围是错的: 提前量 = 系数 × sizeWeight × 目标屏幕速度, 想要"死区 46ms 内目标
    // 走过的距离"(物理上合理的提前量), 需要的系数是 0.046/sizeWeight ≈ 0.05~0.2。
    // 而界面给的是 0..100 —— 差 500~2000 倍。实测把滑条拨到 1.0, v=300px/s 时提前量
    // 就是 150px(等效 0.5 秒), 拨到 100 是 15000px。这直接违反任务书 §4.2 的
    // "绝不允许稳态瞄偏随速度线性增长"。
    //
    // 现在三道约束(§4.2 五条的落地):
    //   ① 系数范围 0..0.2, 合理值就在区间内, 拨到头也只到物理量级;
    //   ② 【硬像素上限】(pidf_predict_max_px): lead 被夹在 该值 与 v×死区 的较小者,
    //      这是"从物理量导出的硬上限", 不是可调旋钮放开就无限大;
    //   ③ 【速度噪声门】(见 pidf_predict_vel_floor): 目标速度低于门限时提前量归零,
    //      因为静止目标的 v̂ 噪声实测 p99=46px/s —— 不设门会把噪声乘成几像素假误差。
    //
    // pidf_predict_x/y      预测系数 X/Y。0 = 该轴不预测(默认关闭)。
    //   提前量 = 系数 × 目标屏幕速度 × 尺寸权重, 再被上面 ② 夹取。
    // pidf_predict_min_w    最小预测宽度(像素, AM 默认 20)。
    // pidf_predict_max_w    最大预测宽度(像素, AM 默认 80)。
    //   这两个框定"哪些目标才补": 框宽 <= minW 或 >= maxW 一律不补, 中间按线性梯度。
    //   ★ 注意用的是【框宽 bbox.width】, 而尺度调度用的是【框高 bbox.height】—— 两者
    //     历史来源不同, 不算 bug, 但同时启用时"远近"判断不一致, 已在设计说明记明。
    // pidf_predict_damp     方向翻转阻尼(0..1, 1 = 不阻尼)。
    //   目标左右横跳时速度估计瞬间反向, 提前量会从 +x 直接跳到 -x —— 幅度 2×lead 的
    //   突变, 看起来是"准星猛抽一下"。阻尼让它滑过去。
    // pidf_predict_max_px   提前量的【硬上限】(像素, 0 = 用内置默认 12px)。
    // pidf_predict_vel_floor 速度噪声门(px/s): |v̂| 低于此值提前量归零。实测静止目标
    //   的 v̂ 噪声 p99=46 / max=52, 所以默认 60 能挡住噪声又不影响真实移动目标。
    float pidf_predict_x = 0.0f, pidf_predict_y = 0.0f;
    int pidf_predict_min_w = 20;
    int pidf_predict_max_w = 80;
    float pidf_predict_damp = 0.25f;
    int pidf_predict_max_px = 12;
    int pidf_predict_vel_floor = 60;

    // ── PID-EventSync 档 (2026-09-15 新增) ──────────────────────────────────
    //
    // ★★ 这是 AimMagic 1.0.30「PID-EventSync」整条链路的移植开关 ★★
    //
    // 它移植的是 AM 的【架构】, 不是它的数字(docs/aimmagic-port-spec.md §0.1:
    // "抄形状, 不抄数值" —— AM 的 x_kp=0.1 是 counts/像素/帧, 与本项目的
    // counts/(像素·秒) 不同纲, 照抄必错)。
    //
    // 移植进来的三件(全部在 aim_mode == 1 时生效):
    //   ① 【跟踪器】(mouse/aim_tracker.h): 目标身份跨帧粘滞(IoU + 最近邻),
    //      min_hits / max_age 生命周期, 速度按【采样窗】估计而不是逐帧重建。
    //      → 解掉本项目"身份每秒变 8 次、每次都可能复位积分"的老问题。
    //   ② 【每轨预测状态机】: 预测系数按 0.1/帧 爬升、-0.2/帧 回落,
    //      状态住在【每条轨迹】上。→ 换目标不串状态, 短暂漏检不清零。
    //   ③ 【事件驱动消费】: 每次推理只出一拍位移, 帧间不做外推补拍
    //      (由 mouse_thread_loop.cpp 强制关掉 use_prediction_tick 保证)。
    //      → 与 AM 的 EventSync 语义一致: 控制器只在"有新观测"时动。
    //
    // ★ 两项【有意不移植】(与本项目硬约束冲突, 理由必须留在代码里):
    //   ✗ AM 的在途补偿换算链(发送日志窗口 ÷ counts_per_pixel → 像素):
    //     需要 k̂(每计数像素)。双机架构下 k̂ 是游戏机的属性、测不出来, 两条反推
    //     路径均已实测证伪(docs/aimmagic-comparison.md §6.7)。同一物理量在本项目
    //     由 mouse/aim_pid.h 的【计数域】在途补偿承担(u -= beta*N/W, 无 k̂)。
    //   ✗ "预测偏移与自身瞄准速度成正比"那个分量(AM runtime+0xC04 自运动项):
    //     同样依赖输出侧标定。本项目只吃【目标框心速度】(量出来的)。
    //
    // ★ 默认 aim_mode = 0 = 现役纯反馈档: 上面所有东西【一行都不生效】,
    //   行为与本档存在之前逐位相同(有回归钉住)。想试 EventSync 再切到 1。
    //
    // 单位/取值:
    int aim_mode = 0;   // 0 = 现役纯反馈档(默认); 1 = EventSync 档(AM 全链路移植)

    // 跟踪器: 连续命中多少帧才算"确认轨迹"(AM FUN_140089ac0 行 448-450 = 3)。
    // ★ 未确认轨迹照样可以锁定目标(上游 selector 已经决定瞄谁), 这个数只用于
    //   遥测标记与"锁定目标死了之后能否自动转移"(见 aim_tracker.h)。
    int esync_min_hits = 3;
    // 跟踪器: 漏帧多少帧后删除轨迹(AM 同上 = 5)。它就是 EventSync 的滑行窗口:
    //   ★ 调大 = 遮挡/闪帧时更能扛(状态不清、身份不变), 但"目标真走了"之后会
    //     多追几拍残影; 调小 = 反应快但容易把一次漏检当成丢目标。
    int esync_max_age = 5;
    // 跟踪器: 关联的最近邻门限(像素, 框心距离)。超过就判"不是同一个目标"。
    // AM 靠上游 FOV 过滤承担这件事, 本项目放在关联层, 语义等价且可测。
    int esync_assoc_radius_px = 80;
    // 跟踪器: IoU 关联阈值(0 = 只靠最近邻)。0.20 是保守值 —— 目标高速横穿时
    // 相邻两帧框重叠很小, 阈值高了会频繁新建轨迹(表现为身份乱跳)。
    float esync_assoc_iou = 0.20f;
    // 跟踪器: 速度采样窗(毫秒)。窗内位移/窗时间 = 速度(AM §4.3 的采样窗速度)。
    // ★ 这个量直接决定预测的输入质量: 8.3ms 拍间隔下逐帧差分噪声就是几百 px/s,
    //   100ms 窗能把噪声压到可用, 同时滞后仍在可接受范围(AM 同量级)。
    //   0 = 逐帧重算(不推荐)。
    int esync_vel_window_ms = 100;

    // ── ⑤ 计数→像素换算率 k̂ (AM: kalman_counts_per_pixel_x/y) ───────────────
    //
    // ★★ 这一对键的历史必须读 ★★
    // 2026-09-14 曾把"k̂"整个赶出本项目, 理由写在 CLAUDE.md("双机架构下 k̂ 是
    // 游戏机的属性、无法测量, 两条反推路径均已实测证伪")。那个结论对**甩枪档的
    // pid_calib**(发已知计数→读光标→反算)是成立的 —— 它依赖单机架构。
    // 但 2026-09-15 复查 EventSync 档的语料后确认: AM 在这一档用的是
    // `kalman_counts_per_pixel_x/y`, 那是一个**用户在设置里手填的常量**
    // (默认 1.0, 范围 [0.001, 10000], ANALYSIS_v1030.md 行 143-144 / 794-795),
    // **不是测量值** —— AM 也没有任何在线标定。它在代码里只被当作一个带下限的
    // 除数用(FUN_140067000 行 1198-1209)。
    //
    // 所以本项目照 AM 一样让用户手填: 他在自己的游戏里知道灵敏度。
    // ★ 默认 1.0 = 【不做换算】(1 计数 = 1 像素), 也就是"没填就不生效"。
    // ★ 填错的风险由用户承担 —— 这与 AM 一致, 且比"程序猜一个"安全得多。
    float esync_counts_per_pixel_x = 1.0f;
    float esync_counts_per_pixel_y = 1.0f;
    // 在途补偿窗口(毫秒)。AM: mouse_effect_delay_ms 默认 8ms。
    // ★ 必须 <= 真实链路死区(46ms), 否则会把早已生效的指令再扣一次 → 正反馈发散。
    // ★ 0 = 整条换算链关闭(在途量恒为 0) —— 默认值, 见 esync_inflight_beta 说明。
    int esync_inflight_window_ms = 0;
    // 在途补偿增益(无量纲)。AM 在这一档没有额外增益(它求和后直接除 k̂ 与拍数),
    // 所以默认 1.0 = 严格照 AM。
    // ★★ 默认窗口是 0(关闭): 打开它等于**把在途补偿从计数域换成像素域**。
    //   而 aim_mode == 1 时引擎会**自动把计数域那项(beta)置 0**(见 boss_aim.cpp)——
    //   两处同开 = 同一批在途指令被扣两次 = 过补偿 = 正反馈发散。
    //   所以它是一个**二选一的开关**, 不是叠加项。
    float esync_inflight_beta = 1.0f;
    // ── ⑥ 自运动补偿 (AM: 提前量与自身瞄准速度成正比) ────────────────────────
    //
    // ★★ 默认 0 = 关闭, 而且**必须**默认关 ★★
    // 这一类项在本项目被删过一次(predictive_controller, 1a5a792): 它把输出侧的
    // 标定增益放进反馈回路内部, 标定不准就在锚点附近每拍反向极限环。
    // 本项目保留 AM 的形状但加三道夹取: 默认 0 / 幅度上限 ±1.0 / 输出仍走硬像素上限。
    // 用户明确要用才开, 开多大他自己负责。范围 [-1.0, 1.0]。
    float esync_self_motion_gain = 0.0f;

    // ── 在途自身位移补偿 (Smith) —— 2026-09-14 【重新启用】 ──────────────────
    //
    // ★★ 这一段的历史必须读, 否则会犯同一个错 ★★
    // 2026-09-13 它曾被整条停用, 停用理由写在旧注释里("不是 AM 的做法, 也没解决
    // 用户的问题")。但 2026-09-14 重做时查明: 当时【不是这个机制错, 是它的实现错】——
    // 旧实现在【像素域】扣, 隐含假设 kp = 1/k̂, 于是补偿被放大了约 5 倍, 同时把误差
    // 顶成反号导致积分永不累积, 表现为"滞后随系数线性增长"(正是任务书 §4.2 禁止的
    // 形态)。所以当时得出的"没用"是错的结论。
    //
    // 现在的实现改成【计数域】: `u -= beta * (N / W)`。N 是控制器自己记账的已下发
    // 计数, W 是窗口拍数。★ 全程不做 counts<->px 换算, 表达式里【没有 k̂】, 所以
    // 不存在"标定不准就自激"的老毛病。
    //
    // 实测效果 (Kp=35, 400px 阶跃, 4s, 60fps):
    //     beta = 0.0  -> 尾段 293~621px 的自持极限环 (60fps 发散)
    //     beta = 1.6  -> 0.295px, 且在 60/120/240/1000fps 四个帧率上完全一致
    // 即: beta=0 不是"关掉一个可选优化", 而是【拆掉了主刹车】。
    //
    // 单位: 无量纲(补偿强度倍数)。0 = 关闭(仅用于对照实验), 1.6 = 生产默认。
    // 上限 3.0(kMaxInflightBeta); 2.0 在 60fps 已发散, 不要填大。
    //
    // ★ 用【负值】表示"没填/用默认 1.6"。0 是合法值(关闭), 见 mouse_thread_loop.cpp
    //   的 pid_params()。默认给 -1 就是为了让"没配置过的机器"自动拿到 1.6,
    //   而不是静默地关掉补偿。
    float pidf_inflight_x = -1.0f, pidf_inflight_y = -1.0f;
    // 窗口长度(ms)。★ 必须 = 真实链路死区 46ms。窗口小于死区 = 补不足(安全, 只是
    // 回到原来的延迟); 大于死区 = 把已生效的位移当在途重复扣除 → 正反馈发散。
    // 运行时实际用它去填 inflight_window_s, 但代码里是直接引用 boss::kAimDeadTimeS,
    // 这个槽位只作为配置文件里的可见记录, 改它不影响行为(见 mouse_thread_loop.cpp)。
    int pidf_inflight_window_ms = 46;

    // ── 【2026-09-14 整条删除】aim_px_per_count_x/y (每计数像素 k̂) ──────────
    // 它曾经是前馈(延迟预测/提前量)的必需输入, 也是那个「测量」按钮要测的东西。
    // 前馈删除后它只剩配置往返, 控制链 0 引用, 所以字段本身也删掉了。
    //
    // ★ 为什么不保留成"读到就忽略"的兼容槽位(与别的停用字段不同):
    //   保留这个键会【诱导】后来的人重新把 k̂ 接回控制器。而本项目是双机架构
    //   (游戏机 --HDMI--> 采集卡 --USB--> 采集机), k̂ 是【游戏机的属性】, 采集机
    //   只能从画面反推, 两条可行的反推路径都已实测证伪(见
    //   docs/aimmagic-comparison.md §6.7)。控制器必须在不依赖 k̂ 的前提下工作
    //   —— 配置里根本不该存在这个量, 免得又有人去"标定"它。
    //   老配置里的这两个键读进来会被忽略, 写回时不再出现。

    // 瞄点平滑现在由 mouse/anchor_filter.h 的 α-β 滤波器负责, 位置紧挨在 PID 之前
    // (boss_aim.cpp)。时间常数是编译期常数 kAnchorFilterTauMs, 故意不做成用户旋钮
    // —— 平滑强度与 Kp 是耦合的, 两个一起调极容易调乱。

    // ── 尺度增益调度 s(bbox.height) —— 2026-09-14 新增 (mouse/aim_scale.h) ──
    //
    // 用【检测框高度】判断目标"比我调参时大了多少", 按公式调制等效增益:
    //
    //     s = clamp( (h / 基准框高)^γ , s_min , s_max )      γ = 1.0
    //
    //   h = 当前框高, 基准框高 = 用户整定参数那个距离上的框高中位数(自动学)。
    //   h = 基准 时 s 恒为 1.0 —— 与你整定时的行为逐位相同。
    //
    // ★★ 为什么不再有"近处框高/远处框高"两个阈值 (2026-09-14 用户纠正) ★★
    //   那版设计让用户填两个绝对像素值, 但用户【根本测不出来】当前框高是多少,
    //   而且在靶场里也无法判断"这个距离算近还是算远"。换游戏/换分辨率后同一段
    //   像素高度对应的距离感完全不同, 写死两个数换个场景就废。
    //   用户真正需要的参照系是【他自己调参时的那个框高】, 所以基准改成自动学,
    //   用户一个像素值都不用填。
    //
    // ★ 远处【也】真的降增益(与上一版相反, 已按用户意见纠正)。
    //   上一版把 s_min 钉在 1.0, 理由是"降增益会让远处更跟不上"; 那个论证隐含
    //   假设了像素速度固定, 但远处目标的像素速度本来就小(v_px ≈ f·v_world/d),
    //   所以按框高等比缩放正是正确的距离补偿。用户原话: "远处敌人看着移动会变慢
    //   啊, 那肯定也是按公式下降"。s_min 因此放开, 允许 < 1.0。
    //
    // ★ 上界受临界增益约束: Kp x s_max 不能越过 g_crit 反推的上限(60fps、k̂≈0.593
    //   时约 52)。默认 Kp=35 x 1.5 = 52.5 就贴在这个上限上, 想再提高倍数必须先降 Kp。
    //
    // ★ 尺度只用 bbox.height 这一个量, 【不引入任何深度/相机内参假设】;
    //   也不许再乘第二个"按框高做几何距离补偿"的因子(那会把深度算两遍)。
    int   aim_scale_enabled = 1;      // 0 = 关闭(等价于 s 恒为 1.0)
    float aim_scale_max = 1.50f;      // 近处上限 s_max, 有效区间 [1.0, 2.0]
    float aim_scale_min = 1.00f;      // 远处下限 s_min, 有效区间 [0.30, 1.00]
    // 基准框高(px)。由调参 agent 在整定期间自动学出(那一段的框高中位数)。
    // 0 = 还没学到 -> 整条链路恒为中性 1.0, 与关闭本项逐位相同。
    // ★ 这不是要给用户填的阈值, 是"用户整定时目标有多大"的记录。
    float aim_scale_base_h = 0.0f;

    // 用户 AimPath：在 AVA PIDF 输出后执行轨迹整形。
    //   0 = 直线(透传)  1 = 贝塞尔  2 = 自定义手绘曲线  3 = WindMouse 拟人曲线
    int   aim_path_mode = 0;
    int   aim_path_influence = 25; // 0..100，仅影响 PIDF 主方向
    float aim_path_bezier_cx1 = 0.30f;
    float aim_path_bezier_cy1 = 0.00f;
    float aim_path_bezier_cx2 = 0.70f;
    float aim_path_bezier_cy2 = 0.00f;
    // 32768 个采样, Y∈[-1, 1], 首尾端点固定 = 0。
    static constexpr int kAimPathSampleCount = 32768;

    // ── WindMouse 曲线 (aim_path_mode = 3) ─────────────────────────────────
    // 仿 AimMagic 的 enable_mouse_curve: 用 WindMouse 物理模型生成一条"像人
    // 甩出来的"路径, 只把它当【局部切线】去旋转 PID 输出, 幅值仍由控制器决定。
    // 单位都是像素 (与 AM 的 wind_mouse_G0/W0/M0/D0 同名同量纲):
    //   gravity   重力(朝目标的吸引) —— 越大越坚决、路径越直
    //   wind      风力(横向随机游走幅度) —— 越大越飘
    //   step      单步最大长度 —— 越小越碎、越慢
    //   distance  风力开始衰减的距离 —— 越接近目标风越小
    //   threshold 门控(px, AM 的 curve_threshold): 两轴误差都不超过它时整段
    //             曲线旁路走直线 —— 微修正保精度, 大甩枪才拟人化。默认 10。
    float aim_path_wind_gravity   = 5.0f;
    float aim_path_wind_wind      = 2.0f;
    float aim_path_wind_step      = 10.0f;
    float aim_path_wind_distance  = 8.0f;
    int   aim_path_wind_threshold = 10;
    // 不可变共享曲线资产:HotkeyProfile 快照只复制指针，UI 修改时才重建。
    std::shared_ptr<const std::vector<float>> aim_path_custom_samples =
        std::make_shared<const std::vector<float>>(); // 空 = 直线
    bool aim_path_neural_enabled = false;
    // 1→8→1 MLP:w1[8], b1[8], w2[8], b2。
    std::array<float, 25> aim_path_neural_weights{};

    // ─────────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────────

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

    // ── 自动开镜 (2026-09-12, 仿 AimMagic 的「开火方式」) ──────────────────
    // 开镜 = 右键。AM 的 fireModes 是 [None, Click Right, Hold Right]:
    //   0 = 关闭
    //   1 = 点按右键(切换开镜): 进命中区点一下右键, 离开命中区再点一下收镜。
    //       靠游戏自己的"切换开镜"。跨两拍完成(按下 → 下一拍抬起), 同一拍里连发
    //       down/up 多数游戏收不到。
    //   2 = 长按右键(按住开镜): 命中区里一直按住, 离开时松开。靠"按住开镜"。
    //
    // ★ 安全规则: 如果本热键的 keys 里就有 RightMouseButton, 那用户本来就在按
    //   右键(开镜时也在瞄), 此时自动开镜【一律不生效】—— 否则会把镜切回去。
    int trigger_auto_scope = 0;
    // 开镜到开火之间的等待(ms): 先开镜, 等这么久才允许按左键。
    // 0 = 同一拍(AM 默认)。游戏的开镜过渡要 100~200ms 时, 填上它第一颗子弹
    // 才是开着镜打出去的; 填 0 则第一枪按 AM 原样"同拍开镜+开火"。
    int trigger_scope_delay_ms = 0;

    // ── 自动急停 (2026-09-13) ───────────────────────────────────────────────
    // 开火那一拍, 如果玩家正按着 WASD, 就往盒子补一个【反方向键】的短按:
    // 绝大多数 FPS 引擎里 W+S 同时存在 = 相互抵消 = 立刻停住, 这一枪才是站定
    // 打出去的。不需要松开玩家手上的键(盒子也做不到), 所以是"补键"而不是"抢键"。
    //
    // ★ 只有 MAKCUNEW 能做 —— 只有它有键盘注入(0x22 KEY_TAP, 固件内定时弹起)。
    //   输入方式不是 MAKCUNEW 时本项自动失效(runtime 判 input_method)。
    // ★ 用的是 KEY_TAP 而不是 KEY_MASK: TAP 由固件定时弹起, 是自清的, 上位机
    //   崩了/停会话了键也会在 ms 级放开; MASK 是绝对态, 漏发清除帧就会把玩家
    //   的移动键永久卡住。
    // ★ 一次只补一个方向键, 前/后轴优先(W 在走最常见)。斜向(W+A)只会抵消掉
    //   前后轴那一半, 横向仍在。
    // 想"停稳了再开枪"就把 trigger_fire_delay 也设为相近的毫秒数:
    //   进命中区 -> 补反方向键(本项) -> 等 fire_delay -> 开火。
    int trigger_auto_stop = 0;     // 0 = 关, 1 = 开
    int trigger_stop_ms = 60;      // 反方向键的短按时长(ms), 20~300


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

    // ── 控制侧: 检测间歇期的预测推进 (取自 AimMagic) ──────────────────────────
    //
    // AimMagic 的控制线程【不等待下一次推理】: 它要么按 1ms 定时自己迭代,
    // 要么被推理事件唤醒, 而且"没有新推理"时它照样推进一步 —— 用的是跟踪器
    // 的预测分支 (use_measurement=false), 不清状态、不重置。
    //
    // 本方现状 (runtime/mouse_thread_loop.cpp): 没有新检测就 `continue`, 也就是
    // 【控制节拍 = 检测节拍】。这带来的代价是采样保持引入的相位滞后 ω·T/2:
    // 同样穿越频率下, 8.33ms 的滞后是 1ms 的 8.3 倍。
    //
    // 打开后: 检测间歇期用预测分支继续推进, 让输出比"等下一帧"更早开始收敛。
    //
    // 默认【关闭】, 理由:
    //   · 它会改变 PID 的 dt 序列 (相邻拍间隔从"检测间隔"变成"检测间隔 + 若干
    //     预测拍"), 于是 D 项、前馈学习率、分数余量累积节奏都会变 ——和
    //     docs/aimmagic-port-spec.md §0.2 说的是同一件事: 改完必须重扫增益。
    //   · 预测拍没有新观测, 用的是跟踪器外推的锚点; 外推错了会先把鼠标推出去,
    //     再被下一帧拉回来 (表现是"抖")。要在实机上按 §1.4 的指标确认再固化。
    bool use_prediction_tick = false;
    // 预测拍的节拍上限 (Hz)。AimMagic 的常规路径是 1ms 定时 (≈1000Hz), 但它
    // 明确说"不要在 1000Hz 输出循环里持续复用同一个结果" —— 即 1kHz 是【内环
    // 迭代】节拍, 输出仍然每拍一次、且每个推理结果只消费一次。
    // 这里取 240Hz 作为默认: 明显快于 120fps 的检测节奏, 又不会让串口写成为
    // 新的瓶颈 (对照 §4: 1kHz 打串口会把写延迟堆成新的死区)。
    int prediction_tick_hz = 240;
    // 预测拍的最大连续次数。超过就停下等新观测 —— 防止"检测流断了还在一直猜",
    // 把鼠标推着走。按 240Hz 计, 默认值 ≈ 允许 41ms 的检测空档。
    int prediction_tick_max_run = 10;
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
