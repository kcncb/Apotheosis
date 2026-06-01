# Apotheosis 调参文档

本文针对当前主分支的 `config.ini` 与覆盖层 UI 调参面板。所有键名/默认值/钳位区间均以 [`Apotheosis/config/config.h`](../Apotheosis/config/config.h) 与 [`Apotheosis/config/config.cpp`](../Apotheosis/config/config.cpp) 为准;瞄准控制器实现见 [`Apotheosis/mouse/mouse.cpp`](../Apotheosis/mouse/mouse.cpp),Kalman 见 [`Apotheosis/include/aim_kalman.h`](../Apotheosis/include/aim_kalman.h)。

> 调参生效路径:`config.ini` 启动时加载 → 覆盖层 UI 修改后调 `OverlayConfig_MarkDirty()` → 退出或主动保存时落盘。覆盖层右上角"已修改"指示亮起代表内存里的 config 与磁盘不一致。

---

## 0. 调参的总体思路

按这个顺序来,出问题排查范围会小很多:

1. **画面采集** —— 先把帧能稳定送进检测器(检测分辨率、来源、FPS)
2. **AI 推理** —— 后端、模型、阈值。先把检测能稳定看到目标
3. **类别分配** —— 哪些类参与瞄准、哪些只画框、哪些 NMS 后就丢
4. **瞄准热键** —— 触发键、类别优先级、FOV、PID、Kalman、自动开火。**这是你真正花时间的地方**
5. **准星找色** —— 想动态跟随准星颜色再开
6. **深度遮罩** —— 想压制墙后/烟雾里误锁再开
7. **硬件 / 宏** —— 输入后端、Lua 宏脚本

每一节后面都有"调参剧本"提示典型场景下该往哪边推。

---

## 1. 画面采集 (Capture)

| 键 | 类型 | 默认 | 说明 |
|---|---|---:|---|
| `capture_method` | string | `udp_capture` | 四选一:`udp_capture` / `tcp_capture` / `opencv_capture` / `mf_capture`。其它字符串会被改回 udp_capture(`capture_card` / `capture_card_cv` / `capture_card_ds` 兼容映射到 `opencv_capture`,`capture_card_mf` 映射到 `mf_capture`) |
| `detection_resolution` | int | `320` | 检测器输入边长,**正方形**。所有像素相关参数(FOV、Flick/Track 阈值、Kalman 速度上限)都以这个分辨率为参考系 |
| `capture_fps` | int | `60` | 采集端目标 FPS,只是个上限提示;实际 FPS 由源决定 |
| `circle_mask` | bool | `true` | 检测前给输入帧加圆形遮罩,中心圆内有效。开了能屏蔽屏幕角落的杂物,代价是远距视野缩小 |

### UDP / TCP 网传模式
另一台机器/采集卡通过 MJPEG 推流过来:

| 键 | 默认 | 说明 |
|---|---:|---|
| `udp_ip` | `0.0.0.0` | `0.0.0.0` = 接收任意来源 |
| `udp_port` | `1234` | `1..65535` |
| `tcp_ip` | `0.0.0.0` | TCP 模式下监听地址 |
| `tcp_port` | `1235` | `1..65535` |

UDP 没有重传;丢包/破帧严重就换 TCP。GPU JPEG 解码 (`gpu_jpeg_decoder` 走 nvJPEG) 在 TRT 后端 + `capture_use_cuda=true` 时自动启用。

### OpenCV 采集卡模式 (`opencv_capture`)

走 `cv::VideoCapture`,通用性高、能直接接 RTSP/文件。

| 键 | 默认 | 说明 |
|---|---:|---|
| `opencv_capture_index` | `0` | 设备索引,从 0 开始 |
| `opencv_capture_api` | `DSHOW` | `DSHOW` / `MSMF` / `FFMPEG` / `ANY` |
| `opencv_capture_url` | `""` | 非空时优先于 index;支持 `rtsp://...`、本地视频文件路径 |
| `opencv_capture_width` / `opencv_capture_height` | `0` | 0 = 让设备自己决定 |
| `opencv_capture_fps` | `0` | 0 = 让设备自己决定 |
| `capture_format` | `MJPG` | `NV12` / `MJPG` / `YUY2` / `RGB32` |
| `capture_crop` | `0` | 中心裁切正方形边长;>0 驱动 `detection_resolution`,0 = 整帧缩放到 `detection_resolution` |

### Media Foundation 模式 (`mf_capture`)

自写的 MF 后端,延迟和 CPU 占用通常优于 `opencv_capture`,只支持 MJPG 的硬件。共用上面所有 `opencv_capture_*` 几何参数(`opencv_capture_api` / `_url` 不适用)。

| 键 | 默认 | 说明 |
|---|---:|---|
| `capture_mf_gpu` | `true` | true = GPU 解码 (nvJPEG + NPP),false = CPU 解码 |

### 调参剧本
- **延迟最低 / 弱机**:`detection_resolution=320`,`circle_mask=false`,`capture_fps=60`
- **远距识别需要**:`detection_resolution=640`(代价是检测耗时翻 4 倍)
- **网传丢包**:用 TCP 而不是 UDP
- **本地采集卡**:优先 `mf_capture` + `capture_mf_gpu=true`;不支持/有奇怪格式再回退 `opencv_capture`

---

## 2. AI 推理

| 键 | 类型 | 默认 | 说明 |
|---|---|---:|---|
| `backend` | string | `TRT` | `TRT` 或 `DML`。CUDA 不可用时主进程会自动落到 DML |
| `dml_device_id` | int | `0` | DML 时 DXGI 适配器索引 |
| `ai_model` | string | `sunxds_0.5.6.engine` | 文件位于 `models/<name>`,TRT 是 `.engine`,DML 是 `.onnx` |
| `confidence_threshold` | float | `0.15` | 0..1。提高可减少误检,代价漏检 |
| `nms_threshold` | float | `0.50` | 0..1。NMS IoU 阈值,提高 = 框重叠更宽容 |
| `max_detections` | int | `20` | 单帧最多保留几个框 |
| `export_enable_fp8` | bool | `false` | 导出 .engine 时是否启用 FP8(Hopper/Ada+) |
| `export_enable_fp16` | bool | `true` | 导出 .engine 时启用 FP16 |
| `fixed_input_size` | bool | `false` | true = 用固定输入 shape(略快),false = 动态 |

> 主推模型是 YOLOv11 end2end(输出 [1,N,6]),类别名/类别数走 ONNX `names` 元数据,UI 里"目标"页直接显示。

### 调参剧本
- **检测漂浮 / 偶尔抖**:`confidence_threshold` 0.15 → 0.25
- **远距小目标漏检**:阈值降到 0.10,同时提分辨率到 640
- **同一目标出多个框**:`nms_threshold` 0.5 → 0.4

---

## 3. 类别分配 (Target / Class Filters)

每个 `class_id` 有三个桶之一:

| 桶 | 行为 |
|---|---|
| `delete` | NMS 后立刻丢,**不进** detectionBuffer,深度遮罩/UI/瞄准都看不到 |
| `filter` | 进入 detectionBuffer,可在 UI 调试看到框,但**不进**瞄准候选池 |
| `aim` | 真正参与瞄准的候选 |

落盘 `[classes]` 段格式 `<class_id> = <bucket>,<class_name>`。例:
```ini
[classes]
0 = aim,player
1 = aim,head
2 = filter,vehicle
3 = delete,gun
```

新模型加载后,UI 会把没见过的 class 默认放进 `delete`,你需要去"目标"页打勾。

### 关键概念:全局桶 vs 每热键 aim_classes

- **全局桶**控制"这个类别在系统里的角色"(删/滤/瞄)
- **每个热键** (`HotkeyProfile.aim_classes`) 自己再选一个**子集**作为这个热键的瞄准目标,**有序**就是优先级。每项 `{class_id, y_offset, kalman_override...}`
  - 热键 1 (RMB) 的 aim_classes = [head, body] → 同时检出时优先头
  - 热键 2 (X1) 的 aim_classes = [body] only → X1 永远只锁身

---

## 4. 瞄准热键 (Hotkey Profile)

**最重要的一节**。每个 `HotkeyProfile` 是一组完整的瞄准参数,触发键按下时整组生效;多个 profile 时按 `config.hotkeys` 的顺序,谁的键被按住谁先匹配。

### 4.1 触发、分组与类别

| 字段 | 默认 | 说明 |
|---|---:|---|
| `name` | `"Aim"` | 仅 UI 显示用 |
| `group` | `"默认"` | UI 里同名 group 会聚成一段方便管理(预设/职业/武器)。运行时仍按扁平顺序匹配,不影响逻辑;留空归"默认" |
| `keys` | `["RightMouseButton"]` | any-of:任一键按住就触发。**当前只接受 5 个鼠标键**:`None` / `LeftMouseButton` / `RightMouseButton` / `X1MouseButton` / `X2MouseButton`。中键被刻意排除以避免和滚轮冲突;键盘键已废弃 |
| `aim_classes[]` | `[]` | 优先级有序数组 |
| 每项的 `y_offset` | `0.5` | 瞄准点相对 bbox 的纵向位置:0=顶,1=底,0.5=中心。头类常设 0.05~0.15;胸类 0.25;肚类 0.5 |

### 4.2 FOV (瞄准锥)

| 字段 | 默认 | 说明 |
|---|---:|---|
| `fovX` | `106` | 横向半径(检测像素),候选必须落在这个椭圆里才被瞄 |
| `fovY` | `74` | 纵向半径(检测像素) |

`fovX/fovY` 是**像素**不是角度。值越大瞄越远,但更容易锁错;太小就需要先把准星甩近才咬上。

### 4.3 PID 瞄准控制器

控制器是**经典离散 PID**,每轴独立,误差 (target − pivot) 直接喂控制器,输出 lround 后送驱动 —— 无单帧钳位、无死区、无 EMA 平滑,靠保守增益与 `Kd` 阻尼收敛。算法见 [`Apotheosis/mouse/pid_controller.h`](../Apotheosis/mouse/pid_controller.h)。

```
err   = target − pivot                    (pivot = 准星位置,检测像素)
P 项  = pid_p * err
I 项  = pid_i * Σ(err * dt)
D 项  = pid_d * d(err)/dt
move  = lround(P + I + D)
```

**Flick/Track 双增益**:开了之后控制器按 |err| 在两套 PID 增益间切换:
- |err| ≥ `flick_track_threshold_px + flick_track_hysteresis_px` → **Flick**(远距,猛甩,用 `pid_*`)
- |err| ≤ `flick_track_threshold_px − flick_track_hysteresis_px` → **Track**(近距,稳跟,用 `pid_track_*`)
- 迟滞带中间保持当前模式,防边界抖动
- 切换瞬间 PID 积分被 `reset()`,避免累积误差跨模式炸开

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `pid_p` | `0.6` | `0..2` | Flick P 增益(横/纵共用)。**最关键**,先调这个;太大会过冲,太小跟不上 |
| `pid_i` | `0.0` | `0..2` | Flick I。一般留 0;只有目标静止时仍有稳态偏置才小心加 (0.02~0.1) |
| `pid_d` | `0.05` | `0..2` | Flick D。阻尼抖动,代价是响应略慢;0.05~0.15 通常够 |
| `flick_track_enabled` | `false` | | Flick/Track 双模式总开关。关掉只用 Flick 增益(传统单 PID 行为) |
| `pid_track_p` | `0.30` | `0..2` | Track P。近距通常用 Flick 的一半,防过冲 |
| `pid_track_i` | `0.0` | `0..2` | Track I |
| `pid_track_d` | `0.10` | `0..2` | Track D。近距噪声相对更大,可比 Flick D 略高 |
| `flick_track_threshold_px` | `30.0` | 检测像素 | 切换中心线。`detection_resolution=320` 时 30 ≈ 半个肩宽 |
| `flick_track_hysteresis_px` | `8.0` | 检测像素 | 迟滞带半宽 |
| `aim_lock_strength` | `0.0` | `0..1` | 吸附锁死力度。0=关。**仅在 Track 模式生效**:近距时按洛伦兹曲线 `1 + s * R²/(R²+err²)` 放大 PID 输出,半径自动取 `0.12 * detection_resolution`。Flick 模式下永不放大(否则远距甩枪会被尾巴拽乱) |

**典型组合(检测分辨率 320):**
- **传统单 PID,远距压枪**:`flick_track_enabled=false`,`pid_p=0.5~0.7`,`pid_d=0.05`
- **远甩 + 近吸**:`flick_track_enabled=true`,Flick `(P,D)=(0.7, 0.10)`,Track `(P,D)=(0.30, 0.12)`,`threshold=30`,`hysteresis=8`,`aim_lock_strength=0.5`
- **头瞄,近距贴脸**:同上,Track P 降到 0.20,`aim_lock_strength=0.7`

> ⚠️ 旧版的 `speed_x` / `speed_y` / `lock_radius_px` 字段、以及 `aim_trajectory_mode=Bezier` 贝塞尔轨迹模式已经整段移除。如果你看到这些键残留在 `config.ini` 里,直接删掉(留着也会被忽略)。

### 4.4 Kalman 预测 (α-β 滤波器)

把目标轨迹建模成"位置 + 速度",在缺帧或抖动时给出预测位置。**当前实现是 α-β 滤波器**(数学上等价于稳态恒速 Kalman 的极限形式),参数从旧版"过程噪声 / 测量噪声"换成两个 0..1 的无量纲增益 + 一个 0..3 的预测倍率。

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `kalman_enabled` | `true` | | 总开关 |
| `kalman_position_alpha` | `0.6` | `0.05..0.99` | 位置增益。每帧 `position += α * (measurement − predicted)`。**越大越信测量(准星紧贴检测),越小越平滑**。常用 0.4~0.8 |
| `kalman_velocity_beta` | `0.25` | `0..0.99` | 速度学习率。**越大速度变化跟得越快,越小越稳**。太大会让速度被检测抖动驱动出来回弹。常用 0.05~0.35 |
| `kalman_prediction_gain` | `1.0` | `0..3` | 预测倍率,直接乘在 `速度 × lookahead` 上。1.0 = 物理上正确;>1 主动提前打,<1 保守。**最直观能感受到"Kalman 有没有在工作"的旋钮** |
| `kalman_velocity_damping` | `0.08` | `0..3` | 预测期间速度指数衰减,模拟目标减速停下 |
| `kalman_max_velocity` | `20000.0` | `100..60000` | 速度上限(检测像素/秒),挡住噪声把速度放大成离谱值 |
| `kalman_warmup_frames` | `2` | `0..6000` | 前 N 帧不输出预测,只观察 |
| `kalman_compensate_detection_delay` | `true` | | true = 把推理延迟自动加进 lookahead |
| `kalman_additional_prediction_ms` | `0.0` | | 额外往前看 N ms;>0 提前打,<0 滞后 |
| `kalman_reset_timeout_sec` | `0.5` | `0.05..3.0` | 目标消失多久后丢预测 |

**每类覆盖:** `aim_classes[].kalman_override_enabled=true` 时下面五个字段会替换掉热键级:
`kalman_position_alpha` / `kalman_velocity_beta` / `kalman_prediction_gain` / `kalman_velocity_damping` / `kalman_max_velocity`。warmup / delay 补偿 / reset_timeout 仍走热键级。**头/身分别调:头类轨迹变向更频繁,`velocity_beta` 通常比身大 0.1~0.2,`prediction_gain` 也可略大**。

### 4.5 简单匀速预测 (传统兜底)

| 字段 | 默认 | 说明 |
|---|---:|---|
| `predictionInterval` | `0.01` | 0~0.5 秒。线性外推 lookahead;Kalman 关掉时仍生效 |
| `prediction_futurePositions` | `20` | UI 画的预测点数量 |
| `draw_futurePositions` | `true` | 画不画 |

### 4.6 自动开火 (Smart Trigger)

控制器在三道闸门同时打开时**自动按下左键**,松开靠定时和占空比。

```
闸门 (a): 准星在 bbox 缩小后的命中圈内 (hit_radius_frac)
闸门 (b): 最近 N 帧鼠标抖动 RMS 低于阈值 (variance_max_px)
闸门 (c): 上次松开后冷却已过
```

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `smart_trigger_enabled` | `false` | | 总开关 |
| `smart_trigger_hit_scale_x` | `0.60` | `0.05..1.0` | 横向命中容差,占 bbox 半宽的比例。越小越严格 |
| `smart_trigger_hit_scale_y` | `0.60` | `0.05..1.0` | 纵向命中容差,占 bbox 半高的比例。瞄头可调小 |
| `smart_trigger_reaction_ms` | `40` | `0..1000` | 准星停留命中框内多久才触发第一枪(拟人反应延迟) |
| `smart_trigger_hold_ms` | `45` | `5..5000` | 每次按下左键的时长 |
| `smart_trigger_cooldown_ms` | `55` | `0..5000` | 松开后到下一枪的强制冷却,控制连发节奏 |

**机制:** 纯几何扳机 —— 把当前准星(开启准星颜色检测时用真实准星位置,否则用检测画面中心)与锁定目标的轴对齐矩形(瞄准锚点 ± `半宽×hit_scale_x` / `半高×hit_scale_y`)比较;判定用目标的**观测位置**而非卡尔曼预测点,因此预测提前量不会影响开火时机。准星连续停留满 `reaction_ms` 后按下、保持 `hold_ms` 松开、再冷却 `cooldown_ms`。开火由 `MouseThread` 走当前 `input_method` 后端发左键(包括 KMBOX/MAKCU/Arduino)。热键松开、开关切关、目标丢失或会话结束都会强制释放,不会卡键。

### 4.7 锁定切换迟滞

防止两个目标距离接近时来回换锁。

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `lock_switch_score_margin` | `0.15` | `0..200` | 挑战者得分要超过当前锁这么多(占半对角线的比例) |
| `lock_switch_min_frames` | `3` | `1..6000` | 至少连续这么多帧 |
| `lock_hold_min_frames` | `10` | `0..2400` | 锁定后无条件保持的最低帧数(0 = 关) |

**抑制换锁不丢牌**:把 `lock_switch_min_frames` 调到 5~8,`lock_hold_min_frames` 调到 15~30。

### 4.8 Y 偏移随距离衰减

近距离 bbox 大,固定的 y_offset (比如 0.1) 在像素上偏出去很远,容易抖。开了之后大 bbox 把 y_offset 向 0.5 (几何中心) 拉。

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `y_offset_size_decay_enabled` | `false` | | |
| `y_offset_size_decay_low_frac` | `0.10` | `0..1` | bbox 高度占检测帧高度的比例,小于此值不衰减 |
| `y_offset_size_decay_high_frac` | `0.40` | `0..1` | 大于此值完全衰减到 0.5 |

### 4.9 动态 FOV

锁定中,实时把 FOV 从 base(`fovX/Y`)插值到"刚好包住目标 bbox + margin"的紧框。准星离目标远 → 用 base(留余地切换);近 → 用紧框(防被噪点抢锁)。

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `dynamic_fov_enabled` | `false` | | |
| `dynamic_fov_margin_frac` | `1.10` | `1..3` | bbox 外扩多少 |
| `dynamic_fov_min_radius_frac` | `0.20` | `0.05..1.0` | 紧框最小占 base 的比例(防过紧) |

### 4.10 威胁优先级 (Threat-Weighted)

把"距离更近"或"头部置信度更高"加权进锁定打分。

```
threat = (1 − r) * depth_normalized + r * head_conf      r = threat_depth_head_ratio
score *= 1 + threat_weight * (threat − 0.5)              (近似;实际看代码)
```

| 字段 | 默认 | 范围 | 说明 |
|---|---:|---|---|
| `threat_priority_enabled` | `false` | | |
| `threat_weight` | `0.50` | `0..1` | 0 = 关,1 = 完全乘进去 |
| `threat_head_class_id` | `-1` | | -1 = 未指定时头项退化为中性 0.5 |
| `threat_depth_head_ratio` | `0.5` | `0..1` | 0 = 全靠深度,1 = 全靠头置信度 |

> ⚠️ 旧的 `threat_body_class_id` 字段已删除,身类不再参与威胁打分(身就是默认基线)。

### 4.11 准星颜色检测开关 (per-hotkey)

| 字段 | 默认 | 说明 |
|---|---:|---|
| `crosshair_detect_enabled` | `false` | 开启则该热键瞄准时用真实准星位置(由"准星找色"模块输出)替代帧中心做 pivot |

调色板 / ROI 大小是全局的(见第 5 节)。

---

## 5. 准星找色

通过 BGR→HSV 在画面中心 ROI 找指定颜色,把检测到的质心当作"真实准星位置"喂给瞄准控制器。开启它能在准星非画面中心(瞄具偏移)或带后坐力位移时仍然准确。

| 键 | 类型 | 默认 | 范围 | 说明 |
|---|---|---:|---|---|
| `crosshair_rect_w` | int | `40` | `4..512` | ROI 宽 |
| `crosshair_rect_h` | int | `40` | `4..512` | ROI 高 |
| `crosshair_min_pixel_count` | int | `4` | `1..10000` | 命中像素数门限 |
| `crosshair_close_radius` | int | `1` | `0..7` | MORPH_CLOSE 半径,1~3 适合大多数渐变/白心准星 |

调色板每条对应一个 HSV 色带 `[crosshair_color.N]`:

| 字段 | 默认(Red-Low) | 范围 | 说明 |
|---|---:|---|---|
| `name` | `"Red-Low"` | | 仅 UI 标签 |
| `enabled` | `true` | | |
| `h_low` / `h_high` | `0` / `10` | `0..179` | 色相范围 |
| `s_min` / `s_max` | `120` / `255` | `0..255` | 饱和度范围 |
| `v_min` / `v_max` | `120` / `255` | `0..255` | 明度范围 |

红色一般需要两条带(Red-Low 0~10 + Red-High 160~179),因为 HSV 色相在 0/180 处回绕;绿/紫/青只需要一条。第一次使用空表会自动播种红双带。

---

## 6. 深度遮罩 (Depth Mask)

把"近处的近 N% 像素"或"远处的远 N% 像素"标成遮罩,落入遮罩超过阈值比例的检测被丢掉。挡住烟雾/掩体后的伪检测。

### 6.1 深度推理本身

| 键 | 类型 | 默认 | 范围 | 说明 |
|---|---|---:|---|---|
| `depth_inference_enabled` | bool | `true` | | 总开关。即使关了 depth_mask 也跑不起来 |
| `depth_model_path` | string | `depth_anything_v2.engine` | | `models/depth/<name>` |
| `depth_fps` | int | `100` | `0..` | 0 = 不限速 |
| `depth_opt_input_size` | int | `224` | `160..640` | TRT OptimizationProfile 的 OPT 档输入边长(方形)。**只在导出 .engine 时生效**,改完必须删旧 engine 重新导出 |

### 6.2 可视化

| 键 | 类型 | 默认 | 范围 | 说明 |
|---|---|---:|---|---|
| `depth_colormap` | int | `18` | `0..21` | OpenCV colormap 索引 |
| `depth_show_heatmap` | bool | `false` | | 在覆盖层显示热力图 |
| `depth_heatmap_gamma` | float | `1.0` | `0.1..5.0` | `pow(d_norm, gamma)` 后再着色。<1 把远景拉亮、近景轻微压暗;>1 反之。**仅影响显示,不影响遮罩** |
| `depth_show_bbox_distance` | bool | `false` | | 在独立检测预览窗口给每个 bbox 标注相对深度 (0..1) |

### 6.3 归一化裁剪(新)

| 键 | 类型 | 默认 | 范围 | 说明 |
|---|---|---:|---|---|
| `depth_norm_clip_low_pct` | float | `0.0` | `0..50` | 归一化前裁掉的低百分位 |
| `depth_norm_clip_high_pct` | float | `100.0` | `50..100` | 归一化前裁掉的高百分位 |

0/100 = 纯 MIN-MAX(传统行为)。**把上限调到 95 可以裁掉极近离群值(贴脸的枪/手),避免远景被压扁到 `depth_norm≈0` 和敌人一起被遮罩误伤。**

### 6.4 遮罩

| 键 | 类型 | 默认 | 范围 | 说明 |
|---|---|---:|---|---|
| `depth_mask_enabled` | bool | `false` | | 真正的遮罩开关 |
| `depth_mask_fps` | int | `5` | `0..` | 遮罩刷新 FPS。**5 通常够用**,深度变化没那么快 |
| `depth_mask_near_percent` | int | `20` | `1..100` | 取最近的 N% 像素作为遮罩 |
| `depth_mask_expand` | int | `0` | `0..128` | 形态学膨胀像素数,扩大遮罩边界 |
| `depth_mask_hold_frames` | int | `0` | `0..120` | 遮罩保持帧数,平滑闪烁 |
| `depth_mask_alpha` | int | `90` | `0..255` | UI 上遮罩半透明强度 |
| `depth_mask_invert` | bool | `false` | | true 改成"远处 N%" |
| `depth_mask_suppression_ratio` | float | `0.30` | `0..1` | 检测 bbox 中遮罩像素比例超过此值就丢。0 = 触一即丢(老行为),1 = 永不丢 |

### 调参剧本
- **烟里乱锁**:`depth_mask_enabled=true`,`near_percent=15~25`,`expand=2~4`,`suppression_ratio=0.30`
- **被掩体后误锁**:同上,加 `hold_frames=2~3`
- **检测被遮罩误杀**:`suppression_ratio` 提到 0.5 或更高
- **贴脸枪/手把整张图压平**:`depth_norm_clip_high_pct=92~95`

---

## 7. 硬件输入

| 键 | 默认 | 说明 |
|---|---:|---|
| `input_method` | `WIN32` | 六选一:`WIN32` / `GHUB` / `ARDUINO` / `KMBOX_NET` / `KMBOX_A` / `MAKCU` |

各后端字段:

### Arduino
| 键 | 默认 |
|---|---:|
| `arduino_port` | `COM0` |
| `arduino_baudrate` | `115200` |
| `arduino_16_bit_mouse` | `false` |
| `arduino_enable_keys` | `false` |

### KMBOX_NET
| 键 | 默认 |
|---|---:|
| `kmbox_net_ip` | `10.42.42.42` |
| `kmbox_net_port` | `1984` |
| `kmbox_net_uuid` | `DEADC0DE` |

### KMBOX_A
| 键 | 默认 | 说明 |
|---|---:|---|
| `kmbox_a_pidvid` | `""` | 8 位十六进制,格式 `PPPPVVVV`(PID+VID 拼接) |

### MAKCU
| 键 | 默认 |
|---|---:|
| `makcu_port` | `COM0` |
| `makcu_baudrate` | `115200` |

### GHUB
没有自己的 ini 字段。需要 `ghub_mouse.dll` 在 exe 同目录,且 G HUB 服务在跑。

### 调参剧本
- **反作弊敏感**:KMBOX_NET / KMBOX_A / MAKCU 优先,Win32 兜底。GHUB 走 Logitech 设备签名,中等隐蔽
- **所有按键都被反作弊忽略**:多半是 Win32 走 SendInput 被检测到,换硬件后端

---

## 8. 宏脚本 (Lua / G HUB 兼容)

| 键 | 默认 | 说明 |
|---|---:|---|
| `macro_enabled` | `false` | 启动时载入脚本并接事件 |
| `macro_script_path` | `""` | `.lua` 文件绝对路径(支持 UTF-8 含中文) |
| `macro_primary_button_events` | `false` | 等同脚本里 `EnablePrimaryMouseButtonEvents(true)`。默认关闭和 G HUB 一致 |

实现的 G HUB API:`OnEvent` / `MoveMouseRelative` / `MoveMouseTo` / `MoveMouseWheel` / `PressMouseButton` (1..5,X1/X2 走 KMBOX 原生 side1/side2) / `ReleaseMouseButton` / `PressAndReleaseMouseButton` / `PressKey` (变长) / `ReleaseKey` / `PressAndReleaseKey` / `Sleep` / `GetRunningTime` / `IsMouseButtonPressed` / `IsModifierPressed` / `IsKeyLockOn` / `IsKeyDown` / `GetMousePosition` / `OutputLogMessage` (printf) / `OutputDebugMessage` / `ClearLog` / `EnablePrimaryMouseButtonEvents` / `GetMKeyState` / `SetMKeyState` / `PlayMacro` / `AbortMacro` / `SetBacklightColor` (no-op) / `RestoreBacklightColor` / `GetDate` / `print`。

输入分发自动走当前 `input_method`,脚本无需关心后端。

---

## 9. 系统 / CUDA

资源预留(防止 OS 把 GPU/RAM 抢去做别的):

| 键 | 类型 | 默认 | 说明 |
|---|---|---:|---|
| `cpuCoreReserveCount` | int | `4` | 给关键线程钉的核数 |
| `systemMemoryReserveMB` | int | `2048` | 在 Working Set 里预占的 RAM(MB) |
| `gpuMemoryReserveMB` | int | `2048` | TRT 时预申请的显存(MB) |
| `enableGpuExclusiveMode` | bool | `true` | 设进程优先级 / GPU 独占类提示 |

推理路径调优:

| 键 | 类型 | 默认 | 说明 |
|---|---|---:|---|
| `use_double_buffer` | bool | `true` | 双缓冲流水线:CPU 后处理第 N 帧 + GPU 推理 N+1 帧。**默认开**,把 CPU NMS / D2H 同步从推理关键路径里挖出去 |
| `use_cuda_graph` | bool | `true` | TRT 启用 CUDA Graph 减少 kernel launch 开销。**`use_double_buffer=true` 时自动绕开**(走更简单的路径),所以同时为 true 不冲突 |
| `use_pinned_memory` | bool | `true` | 锁页主机内存。通常更快但会吃 RAM |
| `capture_use_cuda` | bool | `true` | 采集→检测的 GPU 直连(BGRA→BGR / nvJPEG 在 GPU 上,跳过 CPU) |

### 调参剧本
- **稳定 / 高吞吐**:全默认(`use_double_buffer=true`)。多 ~1 帧延迟换吞吐,~8ms 在 120fps 下被 Kalman 吃掉
- **极致低延迟(对枪零延迟优先)**:`use_double_buffer=false`,`use_cuda_graph=true`
- **小显存 GPU**(比如 8GB,记忆中本机就是 CMP 40HX):`gpuMemoryReserveMB` 降到 1024;不要把 detection_resolution 拉到 640

### 鉴权(在线校验)

| 键 | 类型 | 默认 | 说明 |
|---|---|---:|---|
| `auth_require_online` | bool | `true` | 启动时强制连鉴权服务器 |
| `auth_server_url` | string | `http://110.42.232.243:8787` | 服务器地址 |

---

## 10. 界面 / 调试

### 覆盖层外观

| 键 | 类型 | 默认 | 范围 |
|---|---|---:|---|
| `overlay_opacity` | int | `240` | UI `220..255`(防过透看不清) |
| `overlay_ui_scale` | float | `1.0` | UI `0.85..1.35` |

### 调试

| 键 | 类型 | 默认 | 说明 |
|---|---|---:|---|
| `show_window` | bool | `true` | 检测预览窗口 |
| `show_fps` | bool | `false` | FPS HUD |
| `screenshot_button` | list | `None` | 逗号分隔的键名,见 `keycodes.cpp` |
| `screenshot_delay` | int | `500` | 截图最小间隔 ms |
| `verbose` | bool | `false` | 控制台 verbose 日志 |

### 轨迹回放

| 键 | 类型 | 默认 | 范围 | 说明 |
|---|---|---:|---|---|
| `replay_record_enabled` | bool | `false` | | 启用环形缓冲录制 |
| `replay_seconds` | int | `10` | `1..60` | 缓冲长度(秒) |
| `replay_playback_speed` | float | `0.25` | `0.05..2.0` | 慢放速度 |

调试面板里"快照 + 慢放"按钮触发回放到检测覆盖层上,关闭时零开销。

---

## 11. 常见调参剧本

### A. 弱机 / 笔记本核显 / 矿卡 (CMP 40HX 等 Turing 8GB)
```ini
backend = TRT
detection_resolution = 320
capture_fps = 60
circle_mask = false
depth_inference_enabled = false
use_double_buffer = false
use_cuda_graph = true
gpuMemoryReserveMB = 1024
show_window = false
```
HotkeyProfile: `flick_track_enabled=false`,`pid_p=0.5`,`pid_d=0.05`,`aim_lock_strength=0.0`,`kalman_enabled=true`,`position_alpha=0.7`,`velocity_beta=0.15`,`prediction_gain=0.8`(省 CPU,保守预测)

### B. 强机 / 4090 + 高刷
```ini
backend = TRT
detection_resolution = 640
capture_fps = 144
use_cuda_graph = true
use_double_buffer = true
use_pinned_memory = true
capture_use_cuda = true
```
HotkeyProfile: `flick_track_enabled=true`,Flick `(P,I,D)=(0.7, 0, 0.10)`,Track `(P,I,D)=(0.30, 0, 0.12)`,`flick_track_threshold_px=30`,`aim_lock_strength=0.6`,Kalman `α=0.55, β=0.30, gain=1.2`,头类 per-class 覆盖 `β=0.40, gain=1.3`

### C. 远距压枪(主追身)
- `aim_classes` 只放身类
- `flick_track_enabled=false`,`pid_p=0.5`,`pid_d=0.10`(保守 + 阻尼)
- `aim_lock_strength=0.0`(远距不需要吸附)
- `kalman_position_alpha=0.5`,`velocity_beta=0.25`,`prediction_gain=1.3`(适度提前补垂直后坐力)
- `crosshair_detect_enabled=true`(后坐力让准星偏离画面中心)

### D. 近距贴脸 / 头瞄
- `aim_classes` = [头, 身] 优先级
- 头类 `y_offset=0.10`,身类 `y_offset=0.30`
- `flick_track_enabled=true`,Flick `P=0.7`,Track `P=0.25`,`threshold=25`
- `aim_lock_strength=0.7`(吸附半径自动 = 0.12 × 检测分辨率)
- `dynamic_fov_enabled=true` 防被路人抢锁
- 头类 per-class Kalman 覆盖:`β=0.40`,`gain=1.3`
- `smart_trigger_enabled=true`,`hit_scale_x/y=0.40`,`reaction_ms=30`(更严格、更快点射)

### E. 多人混战防换锁
- `lock_switch_score_margin=0.30`
- `lock_switch_min_frames=8`
- `lock_hold_min_frames=20`
- `threat_priority_enabled=true`,`threat_weight=0.7`,`threat_depth_head_ratio=0.4`

### F. 烟雾/掩体后误锁严重
- `depth_mask_enabled=true`
- `depth_mask_near_percent=20`,`depth_mask_expand=4`
- `depth_mask_suppression_ratio=0.30`,`depth_mask_hold_frames=3`
- 极端情况下 `depth_norm_clip_high_pct=92` 抑制贴脸离群值
- 把烟/枪等次要类放进 `filter` 桶或 `delete` 桶

---

## 12. 排错指南

| 现象 | 检查项 | 建议改动 |
|---|---|---|
| 完全不检测 / 框不出现 | 模型路径、`backend` | 看控制台 `[MAIN]` 输出,确认 .engine 加载成功;TRT 失败时会自动降到 DML |
| 检测有但不瞄 | 类别桶、`aim_classes`、`fovX/Y`、热键 | 目标页确认类别在 `aim`,热键的 `aim_classes` 包含该类,`fovX/Y` 够大 |
| 瞄但完全不动 | `input_method` 是否选对、设备是否连通 | 硬件页"移动测试"按钮验证 |
| 移动太慢 | `pid_p`(Flick)/ `pid_track_p`(Track) | 0.6 起步,每次 +0.1 试 |
| 移动到目标会冲过 | `pid_p` 太大、`pid_d` 太小 | 降 P 0.1 或者 `pid_d` 提到 0.1~0.15 |
| 准星抖 | `pid_d` 太小、`kalman_position_alpha` 太大、`kalman_velocity_beta` 太大 | 任一项收紧;先试 `position_alpha` 0.6→0.4 |
| 锁死目标但跟不上拉枪 | `kalman_velocity_beta` 太小、`kalman_prediction_gain` 太小 | `β` 调 0.30~0.45,`gain` 调 1.1~1.5,并开 `kalman_compensate_detection_delay` |
| 远甩很猛但近距乱抖 | 双增益没开 | `flick_track_enabled=true`,Track P 降到 Flick P 的 1/3~1/2,Track D 略大 |
| 近距吸附不够 | `aim_lock_strength` 太小、Track 模式没生效 | 先确认 `flick_track_enabled=true` 且 |err| 进 Track 阈值,然后把 `aim_lock_strength` 提到 0.5~0.8 |
| Flick / Track 边界抖 | 迟滞带太窄 | `flick_track_hysteresis_px` 8 → 12 |
| 来回换锁 | 见 4.7 | `lock_switch_min_frames` 提到 5~8,`lock_hold_min_frames` 提到 15~30 |
| 自动开火不发 | `smart_trigger_enabled`、`hit_scale_x/y`、`reaction_ms` | 命中比例过小或反应时间过长都会一直不发;先确认有锁定目标 |
| 自动开火乱发 | 命中比例放太大 / 冷却太短 | `hit_scale_x/y` 降到 0.4,`cooldown_ms` 提到 80+ |
| 远距小目标抓不到 | `confidence_threshold`、`detection_resolution`、`fovX/Y` | 阈值降到 0.10,分辨率到 640,`fovX/Y` 不要过大 |
| 准星找色没反应 | ROI 大小、HSV 色带、`crosshair_detect_enabled`(per-hotkey) | UI 调试视图能看到 ROI 和命中像素 |
| 深度遮罩误杀真目标 | 离群值压扁了归一化 | `depth_norm_clip_high_pct=92~95`,或 `depth_mask_suppression_ratio` 提到 0.5 |
| 宏脚本不响应 | `macro_enabled`、`macro_primary_button_events`、脚本里的 `OnEvent` | 看"宏脚本"页日志面板;左键事件需要显式开 |
| 宏脚本卡死 | 脚本里有死循环 | UI 点"卸载"或"重新加载",runtime 会通过 hook 强制中断 |

---

## 13. 数值速查

| 参数 | 弱机/省 | 平衡 | 高精度 |
|---|---|---|---|
| `detection_resolution` | 320 | 320 | 640 |
| `confidence_threshold` | 0.20 | 0.15 | 0.10 |
| `nms_threshold` | 0.5 | 0.5 | 0.4 |
| `pid_p`(Flick) | 0.5 | 0.6 | 0.7 |
| `pid_d`(Flick) | 0.05 | 0.08 | 0.10 |
| `flick_track_enabled` | false | true | true |
| `pid_track_p` | — | 0.30 | 0.25(头) |
| `aim_lock_strength` | 0 | 0.4 | 0.7 |
| `kalman_position_alpha` | 0.7 | 0.6 | 0.5 |
| `kalman_velocity_beta` | 0.15 | 0.25 | 0.35(头 0.40+) |
| `kalman_prediction_gain` | 0.8 | 1.0 | 1.2~1.4 |
| `lock_switch_min_frames` | 3 | 5 | 8 |

---

## 14. 持久化 / 文件位置

- 配置:`config.ini` 在 exe 同目录,UTF-8 编码
- 模型:`models/<name>.engine` 或 `.onnx`
- 深度模型:`models/depth/<name>.engine`
- TRT 引擎导出缓存:`models/engines/`
- 截图:`screenshots/`
- 宏脚本:你自己放的位置,UTF-8 路径都行

修改 `config.ini` 后重启 exe 才会生效。运行中通过 UI 修改的会在退出/手动保存时落盘。多个 hotkey profile 序列化到 `[hotkey.0]`、`[hotkey.1]`、... 段;调色板 `[crosshair_color.0]`、...;类别表 `[classes]`。
