# C++ Configuration Reference (`config.ini`)

This document describes `config.ini` for the C++ project in this repository.

- Source of truth in code: `Apotheosis/config/config.cpp`
- Config schema: `Apotheosis/config/config.h`

> ⚠️ 部分历史小节（`capture_method` / UDP / WinRT / `minSpeedMultiplier` 等）描述的是
> 已被删除的旧实现。键名/默认值/钳位区间一律以 `Apotheosis/config/config.h` 与
> `config.cpp` 为准。下面 6、7 两节是 2026-09-12 新增功能的权威说明。

## 1. How Config Loading Works

1. On first run, if `config.ini` does not exist, the app creates it with defaults.
2. On startup, values are read from `config.ini`.
3. If a key is missing, a fallback value is used.
4. Some keys are clamped/validated during load (examples below).
5. `F4` reloads config at runtime (`button_reload_config` by default).
6. 建立过配置方案之后，运行中的生效配置是 `configs/<方案名>.ini`（见第 6 节），
   `Config::config_path` 指向它，`saveConfig("config.ini")` 会被重定向到该文件。

## 2. Value Formats

- `bool`: `true` or `false`
- `int`/`float`: regular numeric values
- `string`: raw text after `=`
- button lists: comma-separated key names, for example:
  - `button_targeting = RightMouseButton`
  - `button_exit = F2`
  - `screenshot_button = LeftAlt,F10`

Key names come from `Apotheosis/keyboard/keycodes.cpp` (examples: `LeftMouseButton`, `RightMouseButton`, `F1..F12`, `A..Z`, `Home`, `LeftAlt`, `RightControl`, `None`).

## 3. Quick Examples

### Example A: UDP receiver in LAN

```ini
capture_method = udp_capture
udp_ip = 0.0.0.0
udp_port = 1234
detection_resolution = 640
capture_fps = 60
backend = DML
```

### Example B: CUDA + TensorRT with GPU direct capture

```ini
capture_method = duplication_api
backend = TRT
capture_use_cuda = true
detection_resolution = 320
capture_fps = 120
circle_mask = false
depth_mask_enabled = false
```

### Example C: WinRT window capture by title

```ini
capture_method = winrt
capture_target = window
capture_window_title = Counter-Strike 2
capture_cursor = false
capture_borders = false
```

### Example D: Virtual camera capture

```ini
capture_method = virtual_camera
virtual_camera_name = None
virtual_camera_width = 1920
virtual_camera_heigth = 1080
```

### Example E: Lower latency and load for weak GPU

```ini
detection_resolution = 160
capture_fps = 60
confidence_threshold = 0.25
max_detections = 20
game_overlay_enabled = false
show_window = false
depth_inference_enabled = false
```

## 4. Full Key Reference

Defaults below are first-run defaults from `config.cpp`.

### 4.1 Capture

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `capture_method` | string | `duplication_api` | `duplication_api`, `winrt`, `virtual_camera`, `udp_capture` |
| `capture_target` | string | `monitor` | Used by WinRT: `monitor` or `window` |
| `capture_window_title` | string | empty | Used when `capture_method=winrt` and `capture_target=window` |
| `udp_ip` | string | `0.0.0.0` | For `udp_capture`; `0.0.0.0` accepts any sender |
| `udp_port` | int | `1234` | Clamped to `1..65535` |
| `detection_resolution` | int | `320` | Allowed: `160`, `320`, `640`; others become `320` |
| `capture_fps` | int | `60` | UI range `0..240`; `0` means uncapped capture limiter |
| `monitor_idx` | int | `0` | Monitor index for monitor-based capture |
| `circle_mask` | bool | `true` | Circular crop mask |
| `capture_borders` | bool | `true` | WinRT border option |
| `capture_cursor` | bool | `true` | WinRT cursor option |
| `virtual_camera_name` | string | `None` | `None` means auto-select |
| `virtual_camera_width` | int | `1920` | UI range `128..3840` |
| `virtual_camera_heigth` | int | `1080` | UI range `128..2160` |

### 4.2 Target

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `disable_headshot` | bool | `false` | If `true`, body targeting only |
| `body_y_offset` | float | `0.15` | Body target Y offset |
| `head_y_offset` | float | `0.05` | Head target Y offset |
| `auto_aim` | bool | `false` | Automatic target lock behavior |

### 4.3 Mouse (FOV, Speed, Prediction, Correction)

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `fovX` | int | `106` | UI range `10..120` |
| `fovY` | int | `74` | UI range `10..120` |
| `minSpeedMultiplier` | float | `0.1` | UI range `0.1..5.0` |
| `maxSpeedMultiplier` | float | `0.1` | UI range `0.1..5.0` |
| `predictionInterval` | float | `0.01` | UI range `0.00..0.5`; `0.00` disables prediction |
| `prediction_futurePositions` | int | `20` | UI range `1..40` |
| `draw_futurePositions` | bool | `true` | Draw predicted path |
| `kalman_enabled` | bool | `true` | Enable Kalman filter for aim prediction |
| `kalman_process_noise_position` | float | `40.0` | Clamped `0.0001..5000.0` |
| `kalman_process_noise_velocity` | float | `1800.0` | Clamped `0.0001..50000.0` |
| `kalman_measurement_noise` | float | `35.0` | Clamped `0.0001..5000.0` |
| `kalman_velocity_damping` | float | `0.08` | Clamped `0.0..3.0` |
| `kalman_max_velocity` | float | `20000.0` | Clamped `100.0..60000.0` |
| `kalman_warmup_frames` | int | `2` | Clamped `0..20` |
| `kalman_compensate_detection_delay` | bool | `true` | Add live inference delay to prediction horizon |
| `kalman_additional_prediction_ms` | float | `0.0` | Clamped `-80.0..120.0` |
| `kalman_reset_timeout_sec` | float | `0.5` | Auto reset timeout, clamped `0.05..3.0` |
| `snapRadius` | float | `1.5` | UI range `0.1..5.0` |
| `nearRadius` | float | `25.0` | UI range `1.0..40.0` |
| `speedCurveExponent` | float | `3.0` | UI range `0.1..10.0` |
| `snapBoostFactor` | float | `1.15` | UI range `0.01..4.0` |
| `easynorecoil` | bool | `false` | Recoil compensation master switch |
| `easynorecoilstrength` | float | `0.0` | UI range `0.1..500.0` when enabled |
| `input_method` | string | `MAKCU` | `MAKCU`, `MAKCUNEW`, `KMBOXNET`（三档共用 `mouse_driver.h` 的驱动抽象，形状仿 AimMagic 的 `FUN_140040ff0`） |

### 4.4 Wind Mouse

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `wind_mouse_enabled` | bool | `false` | Enable WindMouse behavior |
| `wind_G` | float | `18.0` | UI range `4.0..40.0` |
| `wind_W` | float | `15.0` | UI range `1.0..40.0` |
| `wind_M` | float | `10.0` | UI range `1.0..40.0` |
| `wind_D` | float | `8.0` | UI range `1.0..40.0` |

### 4.5 MAKCU

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `makcu_baudrate` | int | `115200` | UI presets: `9600`, `19200`, `38400`, `57600`, `115200` |
| `makcu_port` | string | `COM0` | UI COM list `COM1..COM30` |

### 4.6 MAKCUNEW

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `makcu_new_baudrate` | int | `6000000` | 固件上限 6 Mbps。固件上电固定 115200；!= 115200 时连接期发送 `0x42 SET_BAUD`（失败再用 `DE AD` 转义帧）并重连，协商失败自动退回 115200 |
| `makcu_new_port` | string | `COM0` | CH343 serial port |

### 4.7 KMBOXNET（2026-09-15 恢复）

以太网 UDP 协议。三个值照抄盒子屏幕上显示的内容：

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `kmbox_net_ip` | string | `192.168.2.88` | 盒子屏幕显示的 IP。注意**不是**盒子自动 DHCP 的，是固件内写死的 |
| `kmbox_net_port` | string | `6234` | 盒子屏幕显示的端口号 |
| `kmbox_net_uuid` | string | `12345` | 盒子屏幕显示的 UUID / MAC 标识（不是标准 UUID 格式） |

### 4.9 Mouse Shooting

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `auto_shoot` | bool | `false` | Auto fire logic |
| `bScope_multiplier` | float | `1.0` | UI range `0.5..2.0` |

### 4.10 AI

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `backend` | string | `TRT` on CUDA build, `DML` on DML build | CUDA build supports both `TRT` and `DML`; DML build uses `DML` |
| `dml_device_id` | int | `0` | DML adapter index |
| `ai_model` | string | `sunxds_0.5.6.engine` (CUDA) or `sunxds_0.5.6.onnx` (DML) | Model file name in `models` |
| `confidence_threshold` | float | `0.10` | UI range `0.01..1.00` |
| `nms_threshold` | float | `0.50` | UI range `0.00..1.00` |
| `max_detections` | int | `100` | UI range `1..100` |
| `export_enable_fp8` | bool | `false` | CUDA-only export option |
| `export_enable_fp16` | bool | `true` | CUDA-only export option |

### 4.11 CUDA (CUDA build only)

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `use_cuda_graph` | bool | `false` | TensorRT execution optimization |
| `use_pinned_memory` | bool | `false` | Host pinned memory mode |
| `use_spin_wait_sync` | bool | `true` | Spin on `cudaEventQuery` instead of blocking `cudaEventSynchronize` when waiting for GPU events. Saves the 10–40 µs kernel wake-up per frame and removes the scheduling tail (ported from 原神AI's `host_wait_spin=true`). Costs one busy core during the wait. |
| `spin_wait_timeout_ms` | int | `50` | Spin budget per sync before falling back to the blocking wait. Guards against a hung GPU burning a core forever. |
| `use_process_boost` | bool | `true` | Raise the process to `HIGH_PRIORITY_CLASS` (same as 原神AI's `process_priority=0x8000`). Removes CPU preemption jitter; does not raise throughput. |
| `use_mmcss` | bool | `true` | Register the inference thread with MMCSS so the OS reserves CPU bandwidth for it. |
| `mmcss_task_name` | string | `Games` | MMCSS task class. Windows maps `Games` to `\Games\Games`. |
| ~~`use_prediction_tick`~~ | — | — | **Removed 2026-09-16.** It let the control loop keep ticking through the tracker's prediction branch during detection gaps (AimMagic's other tier). The surviving EventSync chain is one displacement tick per inference frame with **no inter-frame extrapolation**, so the branch was dead code and the key is gone. A residual key in an old INI is ignored. |
| ~~`prediction_tick_hz`~~ | — | — | Removed with `use_prediction_tick`. |
| ~~`prediction_tick_max_run`~~ | — | — | Removed with `use_prediction_tick`. |
| `gpuMemoryReserveMB` | int | `2048` | GPU reserve target |
| `enableGpuExclusiveMode` | bool | `true` | Exclusive behavior toggle |
| `capture_use_cuda` | bool | `true` | Direct GPU capture path for TRT + duplication API |

### 4.12 System

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `cpuCoreReserveCount` | int | `4` | Reserved CPU cores |
| `systemMemoryReserveMB` | int | `2048` | Reserved RAM amount |

### 4.13 Buttons

All button keys are comma-separated lists of key names.

| Key | Type | Default |
|---|---|---|
| `button_targeting` | list | `RightMouseButton` |
| `button_shoot` | list | `LeftMouseButton` |
| `button_zoom` | list | `RightMouseButton` |
| `button_exit` | list | `F2` |
| `button_pause` | list | `F3` |
| `button_reload_config` | list | `F4` |
| `button_open_overlay` | list | `Home` |
| `enable_arrows_settings` | bool | `false` |

### 4.14 Overlay

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `overlay_opacity` | int | `225` | UI range `220..255` |
| `overlay_ui_scale` | float | `1.0` | UI range `0.85..1.35` |
| `overlay_exclude_from_capture` | bool | `true` | Hide overlay from capture/recording |

### 4.15 Depth

Depth features require CUDA build.

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `depth_inference_enabled` | bool | `true` | Enable depth pipeline |
| `depth_model_path` | string | `depth_anything_v2.engine` | Depth model in `models/depth` |
| `depth_fps` | int | `100` | Clamped to `>= 0`; UI `0..120` |
| `depth_colormap` | int | `18` | Clamped to `0..21` |
| `depth_mask_enabled` | bool | `false` | Enable depth-based mask |
| `depth_mask_fps` | int | `5` | Clamped to `>= 0`; UI `1..30` |
| `depth_mask_near_percent` | int | `20` | Clamped to `1..100` |
| `depth_mask_alpha` | int | `90` | Clamped to `0..255` |
| `depth_mask_invert` | bool | `false` | Invert mask side |

### 4.16 Game Overlay

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `game_overlay_enabled` | bool | `false` | Master toggle |
| `game_overlay_max_fps` | int | `0` | UI `0..256`; `0` uncapped |
| `game_overlay_draw_boxes` | bool | `true` | Draw detection boxes |
| `game_overlay_draw_future` | bool | `true` | Draw future points |
| `game_overlay_draw_wind_tail` | bool | `true` | Draw WindMouse tail |
| `game_overlay_draw_frame` | bool | `true` | Draw capture frame |
| `game_overlay_show_target_correction` | bool | `true` | Draw correction debug |
| `game_overlay_box_a` | int | `255` | `0..255` |
| `game_overlay_box_r` | int | `0` | `0..255` |
| `game_overlay_box_g` | int | `255` | `0..255` |
| `game_overlay_box_b` | int | `0` | `0..255` |
| `game_overlay_frame_a` | int | `180` | `0..255` |
| `game_overlay_frame_r` | int | `255` | `0..255` |
| `game_overlay_frame_g` | int | `255` | `0..255` |
| `game_overlay_frame_b` | int | `255` | `0..255` |
| `game_overlay_box_thickness` | float | `2.0` | UI `0.5..10.0` |
| `game_overlay_frame_thickness` | float | `1.5` | UI `0.5..10.0` |
| `game_overlay_future_point_radius` | float | `5.0` | UI `1.0..20.0` |
| `game_overlay_future_alpha_falloff` | float | `1.0` | UI `0.1..5.0` |
| `game_overlay_icon_enabled` | bool | `false` | Enable icon overlay |
| `game_overlay_icon_path` | string | `icon.png` | Path to icon image |
| `game_overlay_icon_width` | int | `64` | UI `4..512` |
| `game_overlay_icon_height` | int | `64` | UI `4..512` |
| `game_overlay_icon_offset_x` | float | `0.0` | UI `-500..500` |
| `game_overlay_icon_offset_y` | float | `0.0` | UI `-500..500` |
| `game_overlay_icon_anchor` | string | `center` | `center`, `top`, `bottom`, `head` |
| `game_overlay_icon_class` | int | `-1` | `-1` for all classes |

### 4.17 Aim Simulation Overlay

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `aim_sim_enabled` | bool | `false` | Master toggle |
| `aim_sim_x` | int | `24` | UI `-3000..3000` |
| `aim_sim_y` | int | `24` | UI `-3000..3000` |
| `aim_sim_width` | int | `560` | Clamped `220..1920` (UI `220..1600`) |
| `aim_sim_height` | int | `360` | Clamped `180..1080` (UI `180..1000`) |
| `aim_sim_fps_min` | int | `90` | Clamped `15..360` |
| `aim_sim_fps_max` | int | `120` | Clamped `15..360` |
| `aim_sim_fps_jitter` | float | `0.15` | Clamped `0.0..0.8` |
| `aim_sim_capture_delay_ms` | float | `6.0` | Clamped `0.0..80.0` |
| `aim_sim_inference_delay_ms` | float | `12.0` | Clamped `0.0..120.0` |
| `aim_sim_use_live_inference` | bool | `true` | Use runtime inference delay |
| `aim_sim_input_delay_ms` | float | `2.0` | Clamped `0.0..60.0` |
| `aim_sim_extra_delay_ms` | float | `2.0` | Clamped `0.0..60.0` |
| `aim_sim_target_max_speed` | float | `560.0` | Clamped `20.0..2500.0` |
| `aim_sim_target_accel` | float | `1850.0` | Clamped `20.0..10000.0` |
| `aim_sim_target_stop_chance` | float | `0.25` | Clamped `0.0..0.95` |
| `aim_sim_show_observed` | bool | `true` | Show delayed target marker |
| `aim_sim_show_history` | bool | `true` | Show trajectory history |
| `aim_sim_show_kalman_debug` | bool | `true` | Draw Kalman estimate/innovation/velocity debug |

### 4.18 Classes

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `class_player` | int | `0` | Must match your model class index |
| `class_head` | int | `1` | Must match your model class index |

### 4.19 Debug

| Key | Type | Default | Allowed / Notes |
|---|---|---:|---|
| `show_window` | bool | `true` | Capture preview window in overlay |
| `show_fps` | bool | `false` | Legacy key; currently not used in runtime logic |
| `screenshot_button` | list | `None` | Screenshot hotkey list |
| `screenshot_delay` | int | `500` | Minimum interval (ms) between screenshots |
| `verbose` | bool | `false` | Verbose console logging |

### 4.20 Active Game Profile

| Key | Type | Default | Notes |
|---|---|---:|---|
| `active_game` | string | `UNIFIED` | Name of active profile from `[Games]` |

Profiles are stored in a `[Games]` section.

Format:

```ini
[Games]
UNIFIED = 1.0,0.022,0.022,false,0.0
MyGame = 1.2,0.022,0.022,true,103.0
```

Field order:

1. `sens`
2. `yaw`
3. `pitch` (optional; defaults to yaw if missing)
4. `fovScaled` (`true`/`false`, optional)
5. `baseFOV` (optional)

## 5. Special Notes

### 5.1 CUDA direct capture conditions

`capture_use_cuda` is effective only when all are true:

- CUDA build
- `backend = TRT`
- `capture_method = duplication_api`
- `circle_mask = false`
- depth mask is disabled

### 5.2 Runtime-only / auto-managed fields

- `fixed_input_size` exists in runtime state and is auto-detected by detector code.
- It is not a regular persisted `config.ini` key in current implementation.

### 5.3 Migration notes

If you update from older config versions:

- New/missing keys are auto-filled by fallback values.
- Invalid ranges are clamped (for keys with validation in `loadConfig`).

## 6. 全局配置方案（Profile）

一套「配置方案」= 一份完整的配置快照，放在程序目录下的 `configs/`：

```
configs/<方案名>.ini        完整配置（与一份 config.ini 等价）
configs/<方案名>.curves/    该方案自己的轨迹曲线二进制资产（有自定义曲线才存在）
configs/active.txt          当前生效方案名（单行，纯文本）
```

**运行中的生效配置永远是当前方案文件本身。** `Config::loadConfig()` 会把
`config_path` 指到那份文件，而 `saveConfig()` 在 `filename == "config.ini"` 时
会重定向到 `config_path`（`Apotheosis/config/config.cpp`），因此既有所有
`config.saveConfig("config.ini")` 调用点都自动写回当前方案，不需要改一行。

程序目录下的 `config.ini` 因此只承担两个角色：首次运行生成默认值的引导文件、
以及还没有任何方案时的落盘位置。建立方案后它不再被写入，也不会被删除。

| 操作 | UI 位置 | 语义 |
|---|---|---|
| 一键切换 | 顶栏「配置方案」下拉 | 先把当前内存改动 flush 回旧方案，再加载新方案并重新发布运行时快照 |
| 一键保存 | 顶栏「保存设置」/ 下拉右侧 ⋯ →「保存当前方案」 | 当前配置写回当前方案（含曲线资产） |
| 另存为 / 新建 | ⋯ →「另存为...」 | 当前配置写进新方案并立刻切过去 |
| 重命名 | ⋯ →「重命名...」 | 改名 `.ini` 与同名 `.curves/` 目录 |
| 一键删除 | ⋯ →「删除当前方案...」 | 删除 `.ini` 与 `.curves/`；最后一个方案不允许删 |
| 打开目录 | ⋯ →「打开方案目录」 | 用资源管理器打开 `configs/` |

注意：

- 方案名不能包含 `\ / : * ? " < > |`，不能以 `.` 或空格结尾，最长 48 字符。
- 活动方案名记在 `configs/active.txt`，不写进 `config.ini`（**没有新增配置键**，
  所以不会和旧配置文件的 schema 冲突）。
- 切换方案会重发 `ConfigManager::configLoaded`，所有页面按新值重读控件；
  采集设备/模型等变化在推理运行中需要重启会话才完全生效，UI 会先提示。

实现：`qt_ui/config/config_profiles.h` / `config_profiles.cpp`（核心），
`qt_ui/widgets/TopNavBar.cpp`（控件），`qt_ui/MainWindow.cpp`（确认对话框）。

## 7. 轨迹曲线（AimPath）与 WindMouse

每个 `[hotkey.N]` 段都带一套轨迹整形参数（`Apotheosis/mouse/aim_path.h`）：

| Key | 类型 | 默认 | 说明 |
|---|---:|---:|---|
| `aim_path_mode` | int | `0` | `0` 直线(透传) / `1` 贝塞尔 / `2` 自定义手绘 / `3` WindMouse |
| `aim_path_influence` | int | `25` | 0..100，曲线对 PIDF 主方向的影响比例 |
| `aim_path_bezier_cx1/cy1/cx2/cy2` | float | `0.30/0.00/0.70/0.00` | 贝塞尔控制点（X∈[0,1], Y∈[-1,1]） |
| `aim_path_custom_file` | string | — | 自定义曲线的二进制资产路径（`<方案名>.curves/hotkey_N.curve`） |
| `aim_path_wind_gravity` | float | `5.0` | WindMouse 重力 **G0**（像素），AM 的 `wind_mouse_G0` |
| `aim_path_wind_wind` | float | `2.0` | WindMouse 风力 **W0**（像素），AM 的 `wind_mouse_W0` |
| `aim_path_wind_step` | float | `10.0` | WindMouse 步长 **M0**（像素），AM 的 `wind_mouse_M0` |
| `aim_path_wind_distance` | float | `8.0` | WindMouse 距离 **D0**（像素），AM 的 `wind_mouse_D0` |
| `aim_path_wind_threshold` | int | `10` | 门控（像素），AM 的 `curve_threshold`；0..100 |

### 7.1 两条必须守住的性质

1. **只旋转不缩放。** 曲线给出的只是局部切线方向，位移幅值仍等于 PID 输出。
   一旦轨迹层开始放大/缩小幅值，控制器的增益语义与 46ms 死区的稳定边界就全废。
   回归测试 `tests/aim_path_test.cpp [3]/[7]` 逐拍断言这一点。
2. **门控。** `aim_path_wind_threshold` 之内（`|err_x| <= T && |err_y| <= T`，与
   AimMagic `FUN_140071a60` 行 70-79 的逐轴判据一致）整段曲线旁路、直接走直线：
   微修正保精度，只有大甩枪才走拟人路径。另外 WindMouse 在 `dist < D0` 时风力
   本来就被衰减掉，路径自然退化成直线 —— 这也是 AM 需要这层门控的原因。

### 7.2 WindMouse 在本项目里的接法
在**锁定瞬间的局部坐标系**里跑一次标准 WindMouse（起点 `(0,0)`、终点 `(L,0)`，
`L` = 该段真实像素误差），把折线按「进度 → 侧向偏移 / L」重采样成 256 段归一化
剖面，再交给与贝塞尔/自定义完全相同的整形路径（进度由投影驱动、切线低通、
起止淡出、settle_radius 回退）。这样做的两个理由：

- `G0/W0/M0/D0` 的量纲是像素，必须在真实尺度上生成才有意义；
- 路径只提供局部切线，幅值仍归 PID —— 不会和控制器打架。

路径随机但**可复现**：使用固定种子的 xorshift32，并把目标 `track_id` 混进种子，
所以同一串输入逐位可复现（回归测试可断言具体位移），换目标则会摇出不同的路径。

UI：`qt_ui/pages/HotkeyPage.cpp` 的「轨迹曲线」卡片（`buildTrajectoryCard`），
WindMouse 四个参数 + 门控只在选中该模式时显示。

## 8. 自动开镜（仿 AimMagic 的「开火方式」）

每个 `[hotkey.N]` 段两个键：

| Key | 类型 | 默认 | 说明 |
|---|---:|---:|---|
| `trigger_auto_scope` | int | `0` | `0` 关闭 / `1` 点按右键(只点一下) / `2` 长按右键(按住开镜) |
| `trigger_scope_delay_ms` | int | `0` | 点/按住右键之后，等多久才允许开火；0 = 同一拍（AM 默认） |

对应 AimMagic 的 `fireModes: [None, Click Right, Hold Right]`
（`trigger.is_right_click@+0x16A` / `trigger.is_long_press_right@+0x16B`）。

### 8.1 状态机

实现在 `Apotheosis/mouse/trigger_scope.h`（`boss::ScopeController`），
每拍把「准星是否在命中区」映射成「右键该点/该按/该松」：

- **点按右键（mode 1，只点一下）**：**每次接敌开始时点一下**（按下 → 下一拍抬起），
  然后就不再碰右键了 —— 同一次接敌里不会重复点，**也不自动收镜**，开镜状态由用户
  自己处理。一次点击**跨两拍**完成：同一拍里连发 down/up 在多数游戏里会被合并或丢掉，
  而本循环的节拍就是检测帧率（≈8ms），跨一拍正好是机械鼠标能给出的最小可靠点击。
- **长按右键（mode 2）**：命中区里一直按住，离开时松开 —— 给「按住开镜」的游戏。

「接敌」= 按住瞄准热键期间的一次连续交火。接敌途中的复位（丢目标、滑行、
检测流断）**不重新武装**点按，所以重新锁到目标时不会又点一下、把「切换开镜」的
游戏来回切；只有热键松开/换热键/会话结束才算新的接敌。

### 8.2 与 AM 的一处有意偏差

AM 在**同一拍内先按左键、再按右键**（`FUN_140088130` 的按下分支 + ⑥ 连发分支），
所以它的第一颗子弹其实是没开镜打出去的。本项目**先开镜、再允许左键**：
`ScopeController::ready()` 是 `begin_fire()` 的闸门，`trigger_scope_delay_ms > 0` 时
还会等够开镜过渡时间才按左键（等待从点下右键那一刻算）。

### 8.3 三条安全规则

1. **热键自己绑了右键就不用**。本热键的 `keys` 里含 `RightMouseButton` 时
   （默认的 `Aim` 热键就是），用户本来就在按右键开镜，此时自动开镜一律不生效 ——
   否则会把镜切回去。判据在 `mouse_thread_loop.cpp`：`auto_scope_allowed`。
2. **`ready()` 在不适用时必须返回 true**。返回 false 会把整个扳机卡死（一枪都打不出去），
   回归测试 `tests/trigger_scope_test.cpp [1]` 专门断言这一条。
3. **右键绝不允许卡在按下**。点按模式下「按下已发、抬指还没发」时若接敌结束，
   抬指必须被补发（这正是实机上出现过的「右键卡住」事故，回归测试 `[5]` 第一条）。
   接敌途中的复位走 `flushUp()`（补抬指、不动接敌状态），会话/接敌结束走
   `forceRelease()`；进程退出前 `Apotheosis.cpp` 还会再补一次 `releaseRightButton()`。
   运行中把模式从「长按」改成别的，也会先把按住的右键还回去（`[6]`）。

### 8.4 已知边界（没在实机上验证过的部分）

- **注入的右键可能被盒子的按键监控回读**。自动开镜是往盒子里写 `press(2)`/`release(2)`；
  多数 MAKCU 固件把「物理按键监控」和「注入按键」分开，但这一点没有实机验证。
  如果被回读，`keyboard_listener` 会把 `RightMouseButton` 判成按下 —— 于是**另一个**
  绑了右键的热键会在自动开镜期间被误激活。规避办法：用了自动开镜的热键，就别在同一个
  分组里再放一个绑右键的热键。（热键**自己**绑右键的情况已经由 §8.3 规则 1 拦掉了。）
- 开镜是否真的生效、游戏的开镜过渡有多长，都属于「游戏侧」的物理量：本项目只保证
  右键报文按上面的时序发出去了，不保证游戏已经完成开镜。`trigger_scope_delay_ms`
  就是给这个不确定性留的旋钮。

诊断：全链路日志的 `SecTrigger` 段多了两个字段 —— `scope=`（当前是否处于开镜态）与
`scope_ok=`（是否已允许开火）。

## 9. 前馈 / 预测 / 标定整条删除 (2026-09-13)

> 本节原来叫「亚量化保持（高 Kp
锁定后的细小抖动的修复）」，描述
一项依赖 `px_per_count`（k̂）的保护。
> 随着前馈整条删除，该保护与它依赖
的 k̂ 一并删除。下面是现状与为什么。

### 9.1 现状：纯反馈 PID

控制器现在只有两段：

```
检测框 --(瞄点)--> 【α-β 框平滑】--> PID -->
输出限幅 --> 发送
```

**没有观测器、没有前馈、没有预测、
没有标定。**

删除的东西：

| 删除对象 | 位置 |
| --- | --- |
| α-β 观测器 | `mouse/anchor_observer.h`（整个文件） |
| 在线估 k̂ / 速度估计 | `mouse/aim_motion.h` / `.cpp`（整个文件） |
| 前馈输入结构 | `AimPidFeedback`（`target_velocity_px_s` / `pending_self_motion_px` / `dead_time_s`） |
| 输出量子参数 | `AimPidParams::px_per_count` |
| 亚量化保护①② | `aim_pid.cpp` 里的 `sub_quantum` 分支 |
| 标定 API | `AimEngine::beginCalibrationMeasure` 等整组 |
| 标定遥测 | `runtime::calib`（`aim_telemetry.h` 里 20 个 atomic） |
| 界面入口 | 「每计数像素」输入框 + 「测量」按钮 + 状态显示 |

新增：`mouse/anchor_filter.h`（α-β 框平滑，**只输出位置**，速度不外传）。

### 9.2 为什么删：前馈靠一个测不准的参数

前馈 = `-在途自身位移 + 目标速度 × 死区`，
两项都需要「每计数多少像素」（k̂）。

而本项目的游戏运行在**另一台电脑**上
，程序只看得见采集卡送来的画面。
k̂ 是游戏机的属性，只能从画面反推，
而三条可行的反推路径都已被实测证伪
（详见 `aimmagic-comparison.md` §6.7）。

**一个测不准的参数进了回路，估错就
直接变成固定瞄偏** —— 历史上 `+6.2px @300px/s`
就是这么来的。

### 9.3 现在怎么抗抖：框平滑放在 PID 之前

检测框每帧都在跳（实测框心逐帧变化
中位 0.06px，偶发跳变 1.9px）。在 8.3ms 拍间隔下
，1.9px 就是 **230px/s 的假速度** ——
PID 的 P 项放大的是这个噪声，不是真实误差。

把平滑放在 PID **之前**，喂进去的信号干净了
，Kp 才敢开大。

**为什么用 α-β 而不是一阶低通**：α-β 带
速度状态，是「预测 + 修正」。匀速目标下
预测本来就准、新息接近 0，所以**几乎不落后**
；而随机抖动互相抵消，照样被滤掉。
一阶低通只知道位置，追匀速目标会持续落后
一个固定量，只能靠更大的 Kp 去补，而 Kp
越大残留噪声放大得越厉害。

时间常数是编译期常数 `kAnchorFilterTauMs`
（30ms），**故意不做成用户旋钮** ——
平滑强度与 Kp 是耦合的，两个一起调极容易调乱。

### 9.4 用户调的参数

| 界面名 | 配置键 | 作用 | 调过头会怎样 |
| --- | --- | --- | --- |
| 追踪增益 X/Y | `pidf_kp_x/y` | **唯一决定「拉枪快不快、跟得紧不紧」的旋钮** | 输出在目标附近高频抖动 |
| 积分增益 X/Y | `pidf_ki_x/y` | 磨掉追匀速目标的固定滞后 | **“一抽一抽”**（极限环，由链路延迟造成） |
| 震荡抑制 X/Y | `pidf_kd_x/y` | 压 Kp 带来的抖动 | 拖慢收敛、把框抖动放大 |
| 移动死区 X/Y | `pidf_deadzone_x/y` | 固定死区（0 = 关，默认关） | 主动丢精度 |
| 移动限幅 X/Y | `pidf_limit_x/y` | 每拍计数上限（0 = 内置 200） | 大误差段强制降速 |
| **在途增益 X/Y** | `pidf_inflight_x/y` | **把「已发出还没生效」的自身位移从误差里减掉**，解开 Kp 的死结 | 加过头会“发木”：接近目标时提前收手、落不到位 |
| **在途时间窗** | `pidf_inflight_window_ms` | 多久之内发出的指令算“还没生效” | 太短压不住晃；太长把早已生效的也算进去，发木 |

### 9.5 跟踪与提前量（PID-EventSync，2026-09-15 新增；2026-09-16 起为**唯一**链路）

完整说明见 **`docs/eventsync-mode.md`**。要点：

| 界面名 | 配置键 | 默认 | 作用与调过头的后果 |
| --- | --- | --- | --- |
| ~~档位~~ | ~~`aim_mode`~~ | — | **已删除（2026-09-16）**：原来 0 = 经典纯反馈档、1 = EventSync。经典档连同 `AimPredict`、`use_prediction_tick` 一起删掉了，EventSync 现在是唯一链路，因此不再需要开关。老配置里残留的 `aim_mode` 被**忽略**（不报错、也不影响任何键） |
| 确认命中帧数 | `esync_min_hits` | 3 | 连续命中多少帧算“确认轨迹”。★ **不会拖慢锁定**，只影响“锁定目标死了能否自动转移” |
| 滑行帧数上限 | `esync_max_age` | 5 | 漏帧多少帧后删除轨迹（= 滑行窗口）。调大更扛遮挡，但目标真走掉后会多追几拍残影 |
| 关联距离门限 | `esync_assoc_radius_px` | 80 | 框心距离超过它判为新目标。太小 ⇒ 高速横穿时身份乱跳、积分反复清零 |
| 关联重叠门限 | `esync_assoc_iou` | 0.20 | 0 = 只靠最近邻。这是身份稳定的主要来源 |
| 速度采样窗 | `esync_vel_window_ms` | 100 | 速度 = 窗内位移 ÷ 窗时间。8.3ms 逐帧差分噪声是几百 px/s，太短 ⇒ 预测抖；太长 ⇒ 急停/换向时提前量收不回来 |
| 每计数像素 X/Y | `esync_counts_per_pixel_x/y` | 1.0 | ⑤ **你自己填的常量**（AimMagic 的 `kalman_counts_per_pixel`，它也是让用户填的）。1.0 = 不换算。填错只影响在途补偿强度，不会让准星飞掉 |
| 在途换算窗 | `esync_inflight_window_ms` | 0 | ⑤ **0 = 整条链关闭**。打开等于把在途补偿从「计数域」换成 AimMagic 的「像素域」做法。★ 上限硬夹 **46ms**（= 链路死区），超过会把已生效指令再扣一遍 ⇒ 正反馈发散 |
| 在途换算强度 | `esync_inflight_beta` | 1.0 | ⑤ AimMagic 在这一档没有额外增益（= 1.0）。夹 [0, 4] |
| 自运动补偿 | `esync_self_motion_gain` | 0.0 | ⑥ 提前量正比于**你自己甩枪的速度**。★ **默认关**；这类项在本项目被删过一次（标定增益进反馈回路 ⇒ 每拍反向极限环）。夹 ±1.0 |

★★ **在途补偿严格「二选一」，判据是「在途换算窗」是否为 0**（`boss_aim.cpp` 的
`am_inflight = (in.esync.inflight_window_s > 0.0)`）：
窗口 = 0（默认）⇒ AM 那条像素域链**根本没在跑**（`inflightPixels()` 恒返回 0），
在途补偿由计数域的 `pidf_inflight_beta`（生产 1.6）独家承担；
窗口 > 0 ⇒ 引擎**自动把 `pidf_inflight_beta` 置 0**，整条换成 AM 的像素域做法。
两者是同一个 Smith 预测器的两种做法（一个在像素域、一个在计数域），
同时开 = 同一批在途指令被扣两次 = 过补偿 = **正反馈发散**。
★ **反过来不要搞错**：窗口 = 0 时绝不能顺手把计数域那项也关掉 —— 实测 `beta = 0`
不是"关掉一个可选优化"，而是**拆掉主刹车**（60fps 直接发散，见 §9.4.1 的扫描表）。

★ 预测强度、尺寸区间、提前量上限、速度噪声门由 `pidf_predict_*` 提供，消费点是
`AimTracker::predictionLead()`（**唯一的**提前量来源）。

★★ 关于 ⑤ 的 k̂：**它和 `pid_calib`（甩枪档的自动标定）不是一回事。**
后者是"程序发一批已知计数、读光标、反算"，依赖单机架构，在本项目双机架构下
**不成立**（`docs/aimmagic-comparison.md` §6.7）。前者是用户在设置里手填的常量，
双机架构下**同样可以填**（你在自己游戏里知道灵敏度）。详见 `docs/eventsync-mode.md` §4。

另外 `p_full_scale_px`（P 项连续饱和，默认 100px）
是代替死区的防冲机制：`|e| <= 阈值` 原样透传
（满增益），超过则按 `阈值/|e|` 衰减。
它连续且过原点，所以不存在死区那个极限环。

### 9.4.1 为什么需要「在途增益」（2026-09-13 新增）

用户实测的原始问题：**“Kp 拉高会在目标身上来回晃动，拉低的话拉枪又很慢。”**

独立仿真确认这不是调参问题，而是**链路延迟决定的增益天花板**：

| Kp | 延迟 5ms | 15ms | 30ms | 50ms |
| --- | --- | --- | --- | --- |
| 60 | 稳 | 稳 | 稳 | 稳 |
| **100** | **稳** | **稳** | **稳** | **抖 214px** |
| 150 | 稳 | 稳 | 抖 | 抖 397px |

同一个 Kp=100，延迟 30ms 时稳、50ms 时抖 —— **卡住的是延迟本身**。
`Kd` 救不了（Kd 从 0 加到 0.10，每一档抖动都变大）；死区也救不了
（抖动由**已经在途**的输出驱动，与新输出无关）。

**机理**：延迟意味着这一拍发出的指令要过几十毫秒才在画面上生效。
这段在途位移迟早会消掉一部分误差，但控制器看不见它，于是继续按全部误差出力
→ 叠加成过冲 → 反向再出 → 来回晃。

**做法**：`e_eff = e - (inflight_gain / kp) * N在途`，其中 `N在途` 是控制器
自己记账的“已发出未生效的计数”。

**为什么除 kp**：在途项的物理值是 `k̂·N` 像素，而 `k̂`（每计数像素）在本项目
双机架构下**测不准**。写成 `(g/kp)·N` 后，进输出时 `u = kp·e_eff·dt` 里的 kp
把它约掉，于是 `g` 的单位是**计数/秒**，与 `k̂` 和 `Kp` **都解耦** ——
**测不准的 k̂ 被除掉了，不需要任何人去量它**。换灵敏度、换 Kp 后这个值基本不用改。

**它不是预测**：不估计目标速度、不需要观测器、不吃标定常数，只统计自己的输出历史。
所以它绕开了“预测”那条已证明走不通的路，却拿到了 Smith 补偿的主要好处。

**效果**（仿真，Kp=150、延迟 50ms）：

| | gain=0 | gain=50 |
| --- | --- | --- |
| 静止目标抖动 | 397px（晃） | **0.25px** |
| 追踪 100px/s | 抖动 397px | **0.42px** |
| 追踪 300px/s | 抖动 398px | **0.50px** |
| 追踪 500px/s | 抖动 393px | **0.67px**，滞后 39→33.7px |

**目标随便动，准星抖动小于 1 像素。** 可用增益带很宽（Kp=150 时 25~150 都稳），
超过约 200 会重新变差 —— 这是**自限性**：提前认定太多就会发木。

**怎么调**：先把 Kp 按拉枪速度定下来，再从 0 往上加在途增益，加到目标附近不再
来回晃为止。加过头（发木）就退回来。建议从 50 开始试。

**默认 0**（关闭）—— 旧配置读进来也是 0，行为与改动前完全一致。

### 9.5 配置兼容

`aim_px_per_count_x/y`、`pidf_kf_x/y`、`pidf_lr_x/y` 字段**保留**
（读到就忽略），所以旧配置文件不会报错、不会丢键。它们**不参与任何计算**。

### 9.6 同一轮删掉的另外三项（2026-09-13）

| 删除对象 | 位置 | 为什么 |
| --- | --- | --- |
| `capture_age_offset_ms`（界面：采集回调前帧龄（估计）） | `config` + 硬件页 | 手填的**猜测**值，只用于在延迟遥测的时间戳上再减一刀；控制器纯反馈、不吃任何延迟估计。真实延迟由延迟探针实测（那个保留）。 |
| `crosshair_smooth` —— 准星枢轴的 One-Euro 滤波 | `crosshair_runtime.cpp` 的 `OneEuro2D` | 与 `anchor_filter` 构成**两道串联滤波**，各自引入滞后、参数互相耦合。 |
| `crosshair_smooth` —— 参考点 `StaticCrosshairRef` 的惯性低通 | `mouse_thread_loop.cpp` | 同上。**该结构的另外两个职责保留**：限速（抗单帧野值）与丢帧保持/限速滑回中心（抗阶跃）——那些不是平滑。 |

**准星找色检测本身保留**，只是输出改为原始质心，不再做时间平滑。

这三个键和 `capture_age_offset_ms` 都**不再写入配置文件**，旧 ini 里残留的会被忽略
（实测：带这两个废弃键的旧配置能正常加载，不报错、不崩）。

诊断日志 `SecPid` 段已移除 `ff_ready` / `fits` / `ffx` / `ffy` /
`fsx` / `fsy` / `pndx` / `pndy` / `hold` / `kx` / `ky` / `vx` / `vy` 字段（它们
报的都是已删除的量），保留的是真正还在
生效的：`dt_ms` / `ex` / `ey` / `eux` / `euy` / `ipx` / `ipy` / `dpx` / `dpy` /
`ix` / `iy` / `carryx` / `carryy` / `cmdx` / `cmdy` / `lim`。

## 10. 自动急停（开火时抵消移动）

| Key | 类型 | 默认 | 说明 |
|---|---:|---:|---|
| `trigger_auto_stop` | int | `0` | `0` 关 / `1` 开 |
| `trigger_stop_ms` | int | `60` | 反方向键的短按时长（ms），钳位 20~300 |

### 10.1 原理

绝大多数 FPS 引擎里**相反方向键同时按下 = 相互抵消 = 立刻停住**。所以不需要松开
玩家手上的 W —— 只要在开火那一拍往盒子里补一个 `S` 的短按，游戏侧就认为 W+S
同时存在，速度归零，这一枪才是站定打出去的。是「补键」，不是「抢键」
（盒子也没有屏蔽物理键的能力，见 §10.4）。

方向映射：`W→S`、`S→W`、`A→D`、`D→A`，**一次只补一个键，前/后轴优先**
（按住 W 在走是最常见的情形）。斜向（W+A）只会抵消掉前后轴那一半，横向仍在 ——
这一点不假装能停干净，因为 `KEY_TAP` 一次只带一个键码。

### 10.2 哪些后端能用自动急停（2026-09-15 扩充）

自动急停要求后端有**键盘注入通道**（`kCapKeyboard`）。通过 `mouse/mouse_driver.h` 查询能力位，
不再硬编码类型：

- **MAKCUNEW**：原生支持（`0x22 KEY_TAP`，固件自清，不会卡键）。
- **KMBOXNET**：支持（走 `kmNet_keydown` + `Sleep` + `kmNet_keyup`，HID usage 自动转 Windows Virtual-Key）。
- **老 MAKCU**：**不支持**（官方库只有鼠标报文，`kCapKeyboard` 为 0）。
  配了却用不了时日志会写一行 `auto_stop=unsupported,input=MAKCU`，
  `MouseThread::tapKey()` 返回 `false`，功能安全退化。

**为什么用 KEY_TAP 而不是 KEY_MASK**：`KEY_TAP` 由固件定时弹起，是**自清**的 ——
上位机崩了、会话停了，键也会在 ms 级被放开。`KEY_MASK` 是绝对态，一旦漏发清除帧
（或者中途异常）就会把玩家的移动键**永久卡住**，那是不可接受的失败模式。

### 10.3 三条硬规则（`tests/auto_stop_test.cpp` 断言）

1. **注入的键不能当成玩家按的键**。`GetAsyncKeyState` 会把我们注入的 `S` 也读成
   "按下"，若不剔除就会变成"判定玩家按 S → 去补 W"的自激，来回打。所以控制器
   内部会把自己正在注入的那个键从物理键集合里剔除（回归 `[4]`）。
2. **短按窗口内不重发**。连点模式下每一发都重发等于把反方向键变成"一直按着"，
   玩家根本走不动。窗口 = `trigger_stop_ms`，过期后才允许再补（回归 `[3]`）。
3. **不按方向键就不发**。只在真的开火那一拍读一次键（不是每拍读，省掉每秒上百次
   `GetAsyncKeyState`），没按 WASD 就什么都不做（回归 `[1]`）。

### 10.4 用法与已知边界

- **想让「停稳了再开枪」**：把 `trigger_fire_delay`（开火延迟）也设成相近的毫秒数。
  时序是「进命中区 → 补反方向键 → 等开火延迟 → 开火」，这样第一颗子弹是在
  减速完成之后出去的。把 `trigger_stop_ms` 设 40~80ms、开火延迟设 60~80ms 是个
  不错的起点。
- **斜向只停一半**（见 §10.1）。
- **盒子无法屏蔽物理键**：本项目没有 AM 那种设备侧 `mask_x/mask_y`，所以只能
  "补"反方向键让游戏自己抵消，不能真的把玩家的 W 拿走。若某个引擎是"后按的键赢"
  而不是"相反键抵消"，那这个短按会变成往后走一小步 —— 把 `trigger_stop_ms` 调小
  （20~40ms）可以减轻，或者干脆关掉。
- **我们注入的键会被系统读到**：`GetAsyncKeyState` 看到的是"物理 + 注入"的合成态，
  这也是规则 1 必须存在的原因。反过来讲，如果玩家**自己**同时按着 W 和 S，
  逻辑只会按前后轴优先级补 `S`，这是预期行为。
- **`hold_ms` 与键码是照着 `proto.md` 的协议描述实现的**，没有实机回读验证过：
  协议只写了 `KEY_TAP = uint8 mod, uint8 key, uint16 ms`，`key` 的解释依据是
  `KEY_MASK` 用的是标准 HID 键盘报表。第一次上机若发现补的键不对
  （比如没反应或按错了键），先看日志那行 `auto_stop=tap,key=S,ms=60,sent=1`
  确认报文发出去了，再把键码表按固件实际语义修正即可 —— 表在
  `Apotheosis/mouse/auto_stop.h` 顶部四个常量里。
