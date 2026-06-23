# 瞄准算法回滚说明（PID + 卡尔曼）

本目录与 `Apotheosis/include/legacy/` 保存了**重写前**的旧算法逐字副本，用于回滚。

> 重写内容：卡尔曼 → 强跟踪卡尔曼滤波(STF)；PID → 每轴独立 2-DOF PID（X/Y 各一套 P/I/D）。
> 重写日期：2026-05-29。

## 保存的旧文件

| 旧副本（不参与编译） | 对应的现役文件 |
|---|---|
| `Apotheosis/include/legacy/aim_kalman_legacy.h` | `Apotheosis/include/aim_kalman.h` |
| `Apotheosis/mouse/legacy/pid_controller_legacy.h` | `Apotheosis/mouse/pid_controller.h` |

这两个副本不在 CMake 源列表里、也没有任何 `#include` 引用它们，所以放在这里**不影响编译**。

---

## 一、只回滚卡尔曼（单文件，最简单）

新卡尔曼保持了类名与公共 API 完全不变（`AimKalman2D` / `AimKalmanSettings` / `AimKalmanTelemetry` 及全部方法签名），所以回滚只需用旧副本覆盖现役文件：

```powershell
Copy-Item "Apotheosis\include\legacy\aim_kalman_legacy.h" "Apotheosis\include\aim_kalman.h" -Force
```

然后增量编译即可。`config.ini` 里的 `kalman_smoothness` / `kalman_lead` 键沿用，无需改动。

---

## 二、回滚 PID（涉及 X/Y 拆分，需连带还原字段）

PID 重写把"单套 P/I/D"改成了"X 轴 + Y 轴各一套 P/I/D"，贯穿了 config / UI / mouse。要完整回滚需：

1. 覆盖控制器实现：
   ```powershell
   Copy-Item "Apotheosis\mouse\legacy\pid_controller_legacy.h" "Apotheosis\mouse\pid_controller.h" -Force
   ```
2. 还原以下文件里的 `pid_x_* / pid_y_*` 双轴字段回到单套 `pid_p/pid_i/pid_d`：
   - `Apotheosis/config/config.h`（`HotkeyProfile` 字段声明 + 默认值）
   - `Apotheosis/config/config.cpp`（load / clamp / save 三处）
   - `Apotheosis/mouse/mouse.h`（`MouseRuntimeParams`：`pid_x`/`pid_y` → 单 `pid`）
   - `Apotheosis/mouse/mouse.cpp`（`sanitize` / `pid_gains_equal` / `updateParams` / `driveAimToTarget`）
   - `Apotheosis/runtime/mouse_thread_loop.cpp`（`build_params`）
   - `Apotheosis/overlay/draw_hotkeys.cpp`（PID 小节 UI）

   旧副本 `pid_controller_legacy.h` 的 `PidGains` 仍带 `d_filter` / `i_separation` 字段，旧 `mouse.cpp::updateParams` 会按分辨率设置 `i_separation` —— 还原时一并恢复。

> 最稳妥的整体回滚：直接 `git checkout <重写前的提交> -- <上述文件>`，或用 git 比对本次提交的 diff 反向应用。

---

## 三、配置兼容性

新版 `config.cpp` 读取时做了向后兼容：旧 `config.ini` 里若只有单套 `pid_p/pid_i/pid_d`，会被同时种入 X/Y 两轴。反向（新→旧）则旧版只读 `pid_p/pid_i/pid_d`，会忽略 `pid_x_*/pid_y_*`，回落到结构体默认值——回滚后按需重设一次 PID 即可。
