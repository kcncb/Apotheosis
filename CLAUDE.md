# Project orientation

Read `AGENTS.md` first. UI text stays Chinese, source files stay UTF-8, and Chinese narrow strings use legal `u8` literals. Use UTF-8/wide filesystem paths for model names.

## ★★ 2026-09-17: 瞄准控制链已整条删除 —— 本程序现在只做【采集 → 推理】

这一节先读, 因为它决定了仓库里**什么已经不存在**。

用户的要求是"只保留采集和推理"。被删除的东西(整条瞄准控制链):

| 删除的东西 | 原来的职责 |
|---|---|
| `runtime/mouse_thread_loop.cpp` | 瞄准主循环: 扳机 FSM、下发、chainlog。**这是唯一的下游** |
| `mouse/boss_aim.{h,cpp}` | 锁谁 + 瞄框内哪个点 |
| `mouse/aim_tracker.h` | 身份/速度/每轨预测状态机 |
| `mouse/aim_pid.{h,cpp}` | 生产控制器(PID + 在途补偿 + 亚量化保护) |
| `mouse/aim_pid_am.h` | AimMagic PID 内核逐字移植(本来就是**未接线**的可选件) |
| `mouse/aim_scale.h` / `aim_path.h` / `anchor_filter.h` | 尺度调度 / 轨迹整形 / α-β 速度滤波 |
| `mouse/auto_stop.h` / `trigger_scope.h` | 自动急停 / 自动开镜 |
| `mouse/autotune_*.{h,cpp}` + `qt_ui/pages/AutoTunePage.*` | LLM 调参 agent(唯一调参对象就是那条回路) |
| `qt_ui/pages/HotkeyPage.*` | 瞄准参数界面 |
| `runtime/thread_loops.h` / `runtime/chain_log.h` | 鼠标线程声明 / 全链路日志(唯一写入者已删) |
| `HotkeyProfile` 里 **55 个字段** | 全部 `pidf_*` / `esync_*` / `trigger_*` / `aim_scale_*` / `aim_path_*` |
| 15 个逻辑测试 | `aim_*` / `inflight_*` / `dt_smooth` / `autotune_*` / `trigger_scope` / `auto_stop` |

**★ 为什么这些回归是删掉而不是留着**: 它们钉的是 PID 死区、在途补偿、尺度调度、
提前量、调参安全三层 —— 断言对象全都不存在了。留着只会编不过。**这不是"把测试删了
就完事"**: 每一个被删的回归, 它保护的**行为**也一起没了。

★★ **同样删掉的还有一批"没有生产者的读数"** —— 这是本轮最容易漏的一类:
`g_pid_last_err_px` / `g_pid_mode_track` / `g_dynamic_fov_radius_x_px` /
`g_dynamic_fov_radius_y_px` 的唯一写入者是 `mouse_thread_loop.cpp`。删掉主循环之后,
预览窗的动态 FOV 椭圆和 DebugPage 的读数**还在读它们**, 于是界面会显示一个
**永远为 0、看起来像"功能没生效"**的数字。**死读比没有更糟**, 所以读取点一并删除。
`ConfigManager::HotkeyData` 里 ~20 个只在自己和 QSettings 之间往返的瞄准成员同理。

**现在活着的只有**: 采集(Media Foundation) → 推理(**只剩 TensorRT**) →
`detectionBuffer` → 预览窗显示检测框。**没有任何东西会动鼠标、按键、或读写游戏。**

## ★★ 2026-09-17 (后一轮): 推理层收敛为【只 end2end + 只 FP16 + 单缓冲 + 候选固定 20】

同一批改动里把推理层剪成了一条路。**每一条都是"删掉一条并列路径", 不是调参**:

| 删除的东西 | 为什么 |
|---|---|
| **DirectML 整条后端** (`detector/dml_detector.*`, `postProcessYoloDML`, 界面后端下拉框、DML 显卡 ID、`config.dml_device_id`) | 只留 TRT。老 `config.ini` 里的 `backend` / `dml_device_id` 键**读都不读**, 下次保存消失。`backend` **仍是 `Config` 成员**(会话启动读), 只是恒为 `"TRT"` 且不再落盘 |
| **非 end2end 模型支持** (`[1,C,N]` / `[1,N,C]` raw 解码、GPU `launch_decode_and_filter` kernel、`kMaxCandidates`、候选/转置缓冲、CPU `NMS`) | 现在**只接受 `[1,N,6]`**。非 end2end 模型**明确报错并拒绝启动**(提示重新 `end2end=True` 导出), 不再静默走别的路 |
| **双缓冲流水线** (`use_double_buffer`, `numSlots`, `prev_slot`, 界面开关) | 它把"发布第 N 帧"门控在"第 N+1 帧到达"上 ⇒ **白加整整一帧延迟**(120fps = +8.33ms), 与降延迟目标相反 |
| **FP16→FP32 整块 CPU 转换** (`fp16OutputScratch`) | 旧实现每帧先把整块输出逐元素 `__half2float` 成 float 阵列; end2end 只需读 N 行 × 6 个数 ⇒ 改成**按 dtype 就地读取** |
| `mouse/ava_exact/` (6 对文件) | 已无消费者 |
| **候选数上限** | 固定 `kFixedMaxDetections = 20`, **不允许用户设置**(界面只读, 配置键不再落盘) |

★★ **FP16 现在是双向强制**: 输入**和输出**都必须是 `kHALF`, 否则拒绝启动。
输出这条检查是**必需**的, 不只是策略 —— `postProcess` 直接按 dtype 读 `__half`,
若输出实为 FP32 而放过, 会把 float 位模式当 `__half` 解释成**垃圾数值且不报错**。

★★ **end2end 路径【不跑 NMS】**(与旧 `cols==6` 路径逐字一致): 图内已完成选择,
再叠 IoU 抑制会把"两个真实目标靠得很近"误删。**这是特性, 不是遗漏。**

★ 配置面: `backend` / `dml_device_id` / `use_double_buffer` / `max_detections`
四个键现在**读也不读、写也不写**; 老配置里的它们被安全忽略。

★ 仍然活着、且**必须继续被看住**的配置面: `HotkeyProfile` 只剩 9 个字段
(`name` / `group` / `keys` / `fovX` / `fovY` / `aim_classes` /
`crosshair_detect_enabled` / `dynamic_fov_enabled` / `dynamic_fov_strength`)。
`pidf_mapping_version` 这套迁移机制**整条删除** —— 没有槽位需要迁移。
★★ `config_migration_test` 里那条"**老配置里那几十个已删除的键必须被安全忽略**"
(不报错、不污染活着的键、且不再被写回)是**升级路径上最要紧的一条回归**, 保留并加强了。

★★ **验证边界(诚实)**: 这些结论是在 macOS 上得到的 —— 主程序 `ai` 目标在非 Windows
上**根本编不了**(`CMakeLists.txt` 直接 FATAL_ERROR)。所以"编过"这件事**没有被验证过**。
本机能跑的是 `APOTHEOSIS_LOGIC_TESTS_ONLY=ON` 的逻辑回归, 现在是 **8 个**(原来是 22 个)。
其中 `mouse_driver_test` 在 `if(WIN32)` 里, **本机不编译 ⇒ 驱动层现在零覆盖**。

**★ 反过来也要记住: 被删掉的那些"实测结论"仍然是本仓库最有价值的资产。**
46ms 死区、`inflight_beta=1.6` 的实测最优带、`Kp × s_max ≲ 52`、k̂ 在双机下不可测、
以及"分母必须是量出来的"那一串教训 —— 它们记在 `docs/` 里, 若将来要重建控制链,
**先读 `docs/aiming-controller.md` 与 `docs/aimmagic-ground-truth.md`, 不要重新推一遍。**

## ★★ 2026-09-17 (第三轮): 通用控制器层已重建 (L1+L2 纯计算骨架)

★★ **这是"重建", 不是"恢复"** —— 上一轮删掉的那条链**没有回来**。
新链按 `docs/generic-controller-layer.md` 的设计重写, 放在 **`Apotheosis/control/`**,
**全部零依赖**(不引 OpenCV / Windows / Qt)⇒ **任何平台都能编、能单测**。

```
① 筛选(Delete/Filter/Aim) + 选靶(最近 + 滞回) ─ selector.{h,cpp}
② 检测稳定器(认目标 + 剔除异常框, ★【不滤波】) ─ stabilizer.{h,cpp}
③ 滤波(α-β 默认 ┃ Kalman 可选, ★二选一禁止串联) ─ filter.h / alpha_beta_filter.{h,cpp}
④ 瞄点 = 滤波后中心点 + y偏移       ─ anchor.{h,cpp}
⑤ PID(FK 结构, 六个增益全分方向)      ─ pid_controller.{h,cpp}
⑥ 限幅 → 单次量化 + 余量【分方向】结转  ─ pid_controller.{h,cpp}
   整链编排 ─ aim_controller.{h,cpp}
```

★★ **四条最容易改坏的不变量**（改前先读 `docs/generic-controller-layer.md`）：

| 不变量 | 它的依据 | 改坏会怎样 |
|---|---|---|
| **② 稳定器不做任何位置平滑**(D6) | 用户明确要求; 位置平滑只在 ③ 一处 | 与"全链路只有一道滤波"冲突 |
| **③ 卡尔曼【替换】α-β, 不串联**(D7) | 二者是同一位置的两种实现(α-β 本身就是稳态 Kalman) | 两个滤波器各带滞后、参数互耦 |
| **⑥ carry 必须分方向** | 不加就跨轴串扰 | 水平的余量被垂直"借走" |
| **限幅在余量结转【之前】** | 截掉的位移不许攒欠账 | 表现为"松手后冲一下" |

★ **六个增益默认值全部等价于历史行为**(`Kp_x=Kp_y=35`, 其余 0)
—— 所以"从单套改成六套"这个动作本身不改变任何行为。

★★ **没有移动死区, 而且不要加回来**(2026-09-17 第三轮, 用户决定)。
死区被实测证伪: 5px 死区 ⇒ 误差 <5px 时完全不出力, 实测 **10.2 次/秒的
"动/不动"翻转抖动就是这个极限环**。它想解决的问题(末段不冲过头)已由
`pFullScalePx` **连续饱和**负责 —— 后者经过原点且连续, 不存在"停了"这个状态。
★ 保留一个"能调但调了就坏"的旋钮比没有更糟(与"配了没反应的死旋钮"是同类坑的反面)。
★ 回归里有一条专门钉它: "误差 0.01px 也必须出力" —— 加回死区就会变红(实测 5 条失败)。
★ 若真机发现准星在锚点附近高频微动, **先查检测噪声与滤波**, 见 `docs/generic-controller-layer.md` §5.1。

★★ **验证边界(诚实)**: `control_layer_test` 有 125 条断言, 本机全绿; 并且做了
**23 处反向变异, 全部变红**(逐条改坏被测行为, 确认断言真的会失败)。
但它验证的是**纯数值行为** —— **不涉及驱动下发、不涉及真机、不涉及 Windows 编译**。
★★ **2026-09-17 (第三轮续): 控制层已接进主程序**（原来这里是"还没有生产调用者"）。
接线点是 `Apotheosis/runtime/aim_loop.cpp`（运行期外壳）+ `aim_loop_config.cpp`（纯映射）：

| 件 | 职责 |
|---|---|
| `runtime/aim_loop_config.cpp` | **纯映射**（`FlatConfig` → `ControllerConfig`），不依赖 OpenCV ⇒ 逻辑测试也编它 |
| `runtime/aim_loop.cpp` | 取检测/取准星/算 dt/下发；**只管接线，不写控制逻辑** |
| `detector/trt_detector.cpp` | 检测发布【之后】调一次 `runtime::aim_loop::tick()` |
| `runtime/inference_session.cpp` | `stop_locked()` 在 `join_all_locked()` **之后**复位控制器并释放驱动通道 |

★ **节拍（用户决定）**: **一批检测 = 一拍控制**，在检测线程上跑，`dt` 是实际检测间隔。
不用采集帧率 —— 重复喂同一个框会让滤波器误判"目标停住了"。

★ **`aim_loop.cpp` 自己建一个 `MouseThread`**：旧控制链删除后程序里已无别的实例。
惰性构造（只在真要下发时才建），`reset()` 时 join 掉它的 `moveWorker_` 并清空未发队列。

★★ **`ctl_enabled` 默认 false** —— 新增的控制链在真机验证过之前不该自己动鼠标。
打开后走 `MouseThread::sendRawMove`，**单位就是整数计数，边界上零换算**。

★★ **接线时抓到并修掉一个真 bug**: `AimController::setConfig()` 原来**只写 `cfg_`，
从不把 PID 部分转交给 `pid_`**（它是独立对象、有自己的配置副本）。后果是**六个增益
全部被忽略、PID 一直用默认值跑** —— 界面/配置改了完全没反应，不报错不留痕，
正是本仓库反复踩的"死旋钮"坑。修法: `setConfig` 里补 `pid_.setConfig(cfg.pid)`。
★ 回归 `tests/control_wiring_test.cpp`（120 条断言）就是钉它的，**13 处反向变异全部变红**。

★★ **2026-09-17 (第三轮续2): 前后端对齐** —— 接线之后发现"参数能改但界面/配置对不上"，
逐条修完。**这一轮修掉的都是"静默失效"类问题**:

| 对不上的地方 | 后果 | 修法 |
|---|---|---|
| **`config.class_filters` 与 `hotkeys[].aim_classes` 是两套桶** | TargetPage 写的是前者，**控制器只读后者 ⇒ 界面上设的类别对控制器完全无效** | 控制器**两源都读**：全局桶打底 + 逐热键只做"提升为 Aim"（不降级） |
| **6 项参数写死在 control/ 的默认值里** | `maxDistancePx` / 稳定器 5 项 / `randomSeed` **谁都调不了** | 补 8 个配置槽位（load/clamp/save 三处对齐） |
| **`Filter` 桶永远为空** | 枚举值存在但没有任何生产者 | `class_filters` 现在真的产出 Filter（有测试钉住正反两面） |
| **`HotkeyPage` 上轮被删** | ★★ `config.hotkeys[]` **从此没有任何写入者**，界面再也改不动热键/类别/找色/FOV | 重建为 **`AimSettingsPage`**（右栏 6 张卡：触发键/FOV/瞄准类别/准星找色/动态FOV/**控制器参数**） |
| **`HotkeyData` 没有 `ctl_*`** | 就算做了页面也存不下来 | 加 22 个成员 + QSettings 双向 + bridge profile→UI 22 项 |

★★ **两个必须记住的坑（都是实测踩到的）**:

1. **`flattenProfile` 放在 `aim_loop.cpp` 里 = 零覆盖。** 它是 22 条逐字段赋值，
   而该文件要 include OpenCV/Windows，本机编不了。反向变异时
   *"不读 classFilters / 不回填 maxDistancePx"* 两条**全绿** —— 因为测试根本没编到它。
   搬到 `aim_loop_config.cpp`（只依赖 `config.h`，本机可编）后同样两处都会变红。
   ★ **教训：变异"全绿"时先确认那条变异是否有效** —— 我第一条写的是把
   `reserve()` 换掉，而 reserve 只影响容量，那是【无效变异】，不是覆盖缺口。

2. **`ClassBucket{Delete=0,Filter=1,Aim=2}` 与 `toControllerConfig` 的 `case` 标签
   是隐含契约。** 给枚举重排序会编译通过但让 Filter 静默变成 Aim。
   已加三行 `static_assert` 把它变成编译期错误（实测重排序后确实编不过）。

★ **UI 与配置的对应关系**（改参数前先看这张表）:
`AimSettingsPage` 是 `config.hotkeys[]` 的**唯一界面写入者**；
`TargetPage` 管**全局** `class_filters`；两者的关系是"全局打底、逐热键提升"。

## ★★ 2026-09-17 (第四轮): 自动扳机 + 风力曲线已【重建】并接进主程序

用户要求"把自动扳机和风力曲线加回来"。**注意用词: 这是重建, 不是恢复** ——
上一轮删掉的那些文件**没有回来**, 新件是照旧语义在新位置重写的:

| 新件 | 职责 | 与旧实现的关系 |
|---|---|---|
| `mouse/trigger_fsm.h` | 命中区几何 + 五相状态机 (Idle/Delay/Pressed/Cooldown/SwitchCooldown) | 语义逐条对齐旧 `mouse_thread_loop.cpp` 289-370 / 1222-1446 行 |
| `mouse/trigger_scope.h` | 自动开镜 (点按/长按右键) | 逐字搬回 (本来就零依赖) |
| `mouse/auto_stop.h` | 开火那一拍补反方向键 | 逐字搬回 |
| `mouse/aim_path.h` | 四种轨迹 (直线/贝塞尔/手绘/WindMouse) | 逐字搬回 (`boss::AimPathDriver`, 零依赖) |

★★ **为什么把它们拆成独立头文件**: 旧的扳机 FSM 住在 1536 行的
`runtime/mouse_thread_loop.cpp` 里, 那个文件要 include OpenCV/Windows ⇒
**本机编不了 ⇒ 零覆盖**。拆出来之后它们是零依赖的, 任何平台都能编、能单测 ——
这正是 `trigger_path_test` 能存在的前提。

★ **配置面**: `trigger_*` (13 个) 与 `aim_path_*` (11 个) **重新变成活键**,
load/clamp/save 三处都对齐了。`config_migration_test` 里原来那两条
"已删除的键不再写出: trigger_enabled / aim_path_mode" **已按事实改写** ——
它们现在断言的是"这些键**重新**写出且值真的往返"。
**仍然删除、不恢复**的是 `aim_path_custom_samples` / `aim_path_custom_file` /
`aim_path_neural_*` (手绘编辑器与神经权重都没重建), 它们继续被安全忽略。

★ **接线点** `runtime/aim_loop.cpp`: 控制器算完之后
① `g_path.step()` 整形 (只旋转不缩放) → ② `g_scope` 自动开镜 →
③ `g_trigger.tick()` 出 press/release → ④ `g_autoStop` 补键 → ⑤ `sendRawMove`。
★★ **顺序是契约**: 开镜必须在左键**之前** (`ready()` 当闸门, 保证第一颗子弹
是开着镜打出去的), `release_left` 必须先于 `press_left`。

### ★★ 这批断言的覆盖是【验过】的, 而且验的过程抓出了真问题

`trigger_path_test` 现在让 `ctest` 从 10 个变成 **11 个**。
★★ **它的价值来自"15 处反向变异, 13 处变红"** —— 不来自"全绿"。
逐条改坏被测行为确认真的失败, 过程中抓到 4 个假覆盖并修掉:

| 假覆盖 | 为什么原来抓不住 | 修法 |
|---|---|---|
| 门控测试 | "误差 <1px 早退"和"progress=0 时 entry_fade=0"**两条别的旁路**把输出也变成透传 ⇒ 把 gate 改成 -1.0(永不旁路)照样绿 | 先把驱动器"跑热", 再收误差进门控带 |
| target_id 测试 | 只写 `sample(42) != sample(43)`。progress_ 的累积会让**很靠后的拍**出现极小浮点差异 ⇒ `!=` 成立。抹掉 target_id 后照样绿 | 改成**定量**断言: 前 10 拍的横向分量差异必须 > 0.1 |
| 幅值不缩放 | 上界写成"不超过 3 倍"。实现里有显式的 `restore = base_mag/mixed_mag` ⇒ 真实比值恒为 1.000, 3 倍的上界抓不住去掉 restore 的变异 | 改成**逐位精确相等** |
| Delay 相位 | 原来的序列是在 **Idle** 相位离开命中区的 ⇒ Delay 分支的清理代码**根本没执行** | 补一段"在 Delay 相位中途离开"的序列 |

★ 剩下 2 处未捕获的变异已确认为**无效变异**(语义中性的代码), 不是覆盖缺口:
`if (target_delay <= 0) → if (false)` (`now-now>=0` 同拍仍成立) 与
`reset()` 里的 `progress_ = 0.0` (紧接的 `engaged_=false` 分支会再置一次)。
两条都写进了测试注释, 免得后人重走。

★★ **另有一条实测结论, 值得记住**: `AimPathDriver::reset()` 目前
**等价于"换一个 target_id"** —— 把 reset 整个改成 no-op, 输出逐位不变。
原因: `step()` 的 `id_changed` 路径本来就做完了 reset 做的全部事情,
而 `rng_` 两者都不回拨(有意: 否则每次会话第一枪都抖成同一条死曲线)。
★ 它仍被 `aim_loop.cpp` 在会话结束时调用, 保留是有意的(语义上表达
"这段接敌结束了"), 只是当前没有额外的可观察效果。**测试不假装它有效果**。

★★ **验证边界(诚实)**: 本轮**真的启动了 `Apotheosis.exe`** 并做了实测 ——
往 `config.ini` 写非默认值 (`trigger_enabled=true` / `trigger_fire_delay=45` /
`trigger_stop_ms=77` / `aim_path_mode=3` / `aim_path_wind_gravity=7.5`),
启动应用、正常退出, 再读回: **5 个值逐条保留**。这证明 schema→load→clamp→save
整条链在**真实二进制**里成立 —— 不只是单测里成立。
★ 但**没有验证**的是: 扳机真的点到了游戏、曲线真的让鼠标走成曲线、以及
新加的两张卡在界面上的**外观**(截图工具在本模型下看不了图)。
这两件事仍然必须在真机上由用户确认。

## Current build

- Windows x64 C++20 / CUDA C++17 application with a Qt6 Widgets UI.
- Root `CMakeLists.txt` is the main build; recommended generator: Visual Studio 18 2026.
- Target `ai`, executable `build/cuda/Release/Apotheosis.exe`.
- A single portable binary includes TensorRT. ★ DirectML was removed on 2026-09-17; do not restore it.
- Qt prefix, TensorRT, cuDNN and CRT locations are CMake cache paths. See `docs/build.md` for actual defaults.
- Ordinary prebuilt OpenCV 4.13.0; GPU operations use `GpuImage`, custom kernels, NPP and nvJPEG. No OpenCV CUDA rebuild is required.
- `APOTHEOSIS_LOGIC_TESTS_ONLY=ON` builds only independent regressions on any platform; it is not another application/backend build.

## Runtime

- `Apotheosis/Apotheosis.cpp`: entry, input devices, Qt main loop.
- `runtime/InferenceSession`: serialized start/stop, detector ownership and worker cleanup. Qt dispatches operations asynchronously; no widget is touched from its operation worker.
- `capture/capture.cpp`: only the Media Foundation capture-card backend is currently created. No UDP/TCP/raw-Ethernet backend is built into this path.
- `capture/mf_capture.cpp`: asynchronous Source Reader callback with a cancellable sample wait; bounded processing/decode workers; latest output frames. Device capability selection is strict.
- Each frame carries its callback-entry steady-clock timestamp through CPU/GPU output and the detector input slot. Input dequeue establishes T1; publishing uses that frame's T0/T1, never the latest global timestamp.
- `detector/`: **only** the TensorRT implementation of `IDetector` remains (DirectML deleted).
  ★ Single-buffered: the double-buffer pipeline was removed on 2026-09-17 because it added a
  full frame of latency (+8.33ms @120fps). Results publish as soon as they are ready.
  ★ Engine I/O must be FP16 in **and** out, output must be end2end `[1,N,6]` — otherwise the
  engine is rejected at load time rather than silently mis-decoded.
- ★★ **瞄准控制链已于 2026-09-17 整条删除**(`boss_aim` / `aim_tracker` / `aim_pid` /
  `aim_scale` / `aim_path` / `anchor_filter` / `auto_stop` / `trigger_scope` /
  `autotune_*` / `runtime/mouse_thread_loop.cpp` / `HotkeyPage` / `AutoTunePage`)。
  本节下面原来那一大段(控制回路、46ms 死区、在途补偿、亚量化保护、扳机 FSM、
  调参 agent、PID-EventSync、尺度调度、AimMagic PID 内核移植…)**描述的东西都已经不在
  代码里了**, 已整段移除以免误导。**那些实测结论没有丢**: 它们完整记在
  `docs/aiming-controller.md`、`docs/aimmagic-ground-truth.md`、`docs/aimmagic-comparison.md`、
  `docs/eventsync-mode.md`、`docs/autotune-agent.md` 里 —— **要重建控制链就先读它们,
  不要重新推一遍**(46ms 死区、`inflight_beta` 的最优带、`Kp×s_max≲52`、
  k̂ 在双机架构下不可测, 这些都是量出来的, 不是想出来的)。
- `mouse/mouse_driver.h` / `mouse.cpp`: 驱动抽象与下发队列**仍然存在且仍然编译**,
  但它现在**没有任何下发消费者**(原来唯一调用它的是 `mouse_thread_loop.cpp`)。
  输入设备仍会被探测/打开/关闭(`Apotheosis.cpp::createInputDevices()`),
  只是不会再往游戏里发任何东西。★ `assignInputDevices()` 现在是空实现,
  这是有意的(保留调用序列, 去掉已经没有消费者的推送)。
- **输入方式与驱动抽象**(2026-09-15 升级, 形状仿 AimMagic 的 `FUN_140040ff0` 驱动工厂): `mouse/mouse_driver.h` 的 `IDriver` 统一接口 + `mouse_driver::open()` 工厂。支持三家后端: **`MAKCU`**(官方库, 串口) / **`MAKCUNEW`**(直通透传固件, 串口, 6Mbps) / **`KMBOXNET`**(从 commit `9236942^` 恢复, 以太网 UDP, 屏幕显示 IP/端口/UUID)。★ 核心变化: 从"MouseThread 里 `if (makcu_new_) ... else if (makcu_)` 按具体类分叉"改成**按能力位查询**(`supports(kCapKeyboard)` / `kCapMove` / `kCapButtonLeft` / `kCapPhysicalRead`…); ★ 注意: 能力位本身仍然有效, 但**它的第一个消费者(自动急停)已随控制链删除** —— 所以现在"能力位被查询"这件事在生产路径上**没有下游**。★ 连不上时**带得出证据**: `describeStatus()` / `lastError()` 打印具体 IP/端口/COM 与错误原因, 不再是旧代码那种只有一行的 `Error connecting.`。★ 设备热插拔流程保留: `createInputDevices()` 解除借出指针后才关闭旧设备, `inputDeviceMutex` 保护指针读取, `MouseThread::refreshDriver()` 自动把当前借出指针包成统一接口。回归: `tests/mouse_driver_test.cpp`(测试 24, 覆盖名称/能力/状态串/非法名字拒绝/空指针安全降级)。
★★ 它在 `tests/CMakeLists.txt` 的 `if(WIN32)` 里 ⇒ **macOS 上不编译、不在 ctest 列表里**。
   所以"逻辑测试全绿"这句话**不覆盖驱动层**。
- UI: `qt_ui/MainWindow.cpp`, `qt_ui/pages/`, shared widgets. `overlay/preview_window.cpp` is the runtime image preview, not the old ImGui launcher.
- `qt_ui/preview/` is an independent visual preview, not a working inference application.

## Shared state and telemetry

- `configMutex` protects mutable config; `runtime_config::publish()` creates an immutable snapshot consumed by runtime readers.
- `ConfigBridge` synchronizes Qt settings, runtime config and debounced persistence. A new setting needs schema/default/load/save/bridge/UI alignment.
- `shouldExit`: application exit. `session_stop_requested`: current pipeline stop. Session workers are joined before their detector is destroyed.
- Detection boxes/classes/timestamps share `detectionBuffer.mutex`; frame buffers share `frameMutex`.
- UI/preview read thread-safe latency snapshots, not live detector pointers or mutable timing fields.
- GPU completion events are reference-counted with the frames. A producer pool must not re-record an event still held by a consumer, or destroy it on capture restart.
- Device frame age is separate from software T0..T4. Missing, invalid or stale values are unavailable, not zero. It does not establish the capture card's internal HDMI latency.
- Current E2E does not prove physical HID execution or game/display response.

## Validation

★ **2026-09-17 (第四轮) 之后的实情**: 本机能跑的只有 `APOTHEOSIS_LOGIC_TESTS_ONLY=ON`, 现在 **11 个**测试:

```
cmake -S . -B build/logic-tests -DAPOTHEOSIS_LOGIC_TESTS_ONLY=ON
cmake --build build/logic-tests -j8
ctest --test-dir build/logic-tests --output-on-failure
```

它们是 `latency_probe_test` / `capture_card_caps_test` / `device_frame_age_test` /
`interruptible_slot_test` / `gpu_ready_event_test` / `raw_frame_layout_test` /
**`control_wiring_test`** / **`control_layer_test`** / **`trigger_path_test`** /
`config_migration_test` / `mouse_driver_test`

前 6 个是**采集与推理数据面**的回归(逐帧时序、被覆盖的输入计数、设备时间戳有效性/新鲜度、
可取消的采样等待、GPU 事件所有权)。

★ `control_layer_test` 是第三轮新增的**控制器层回归**(125 条断言), 钉 ①~⑥ 全链:
选靶滞回、稳定器不平滑、突变→硬重置、α-β 公式、锚点方向、六增益分方向、
余量结转(含"不放大")、限幅顺序、**无死区(0.01px 也出力)**、D 项低通、
积分 clamp 与按分量回吐。
★★ **它的价值来自"23 处反向变异全部变红"这个事实, 不只来自"全绿"。**
   做法: 逐条改坏被测行为(见 `docs/generic-controller-layer.md`), 确认断言真的失败。
   **一个不会失败的断言不是证据** —— 本轮就靠这个抓出了 4 个假覆盖:
   从未被触发的 D 项低通、只看遥测快照的 reset 断言、
   以及被"中心太远"回退路径掩盖的尺寸判据。
   (另有 `deadzonePx` —— 它最初是**声明了却没人用**的死参数, 补实现后发现
   "能调但调了就坏"同样有害, 于是**整项删除**, 并留了一条防它回来的回归。)

★ 原来那 22 个里有 15 个钉的是瞄准控制链, 已随控制链删除。
**每一个被删的回归, 它保护的行为也一起没了。**

★★ **`config_migration_test` 在 macOS 上被静默跳过** —— 它需要 `Apotheosis/modules/SimpleIni.h`,
而那个第三方单头文件在本 checkout 里**不存在**(Windows 上才有)。`tests/CMakeLists.txt` 用
`if(EXISTS ...)` 包着它, 所以它**既不编译也不在 ctest 列表里** —— 本机**无法**验证配置层。
这是覆盖缺口, 不是"它通过了"。

★★ **`mouse_driver_test` 同样跳过**(在 `if(WIN32)` 里) ⇒ 驱动层本机零覆盖。

★★ `control_wiring_test`（**120 条断言**, 13 处反向变异全红）钉的是
**"配置 → 控制器"的映射**（含 `flattenProfile` 的 22 条逐字段搬运）、
**"配置真的驱动控制"**、**类别桶两源合并**与 **dt 门禁**。它的存在理由: 漏映射 = 静默失效（界面能改、跑起来没变），
不报错不留痕 —— 上面那个 `setConfig` 不转发 PID 配置的 bug 就是这么被抓出来的。

★★★ **"8/8 绿"证明的是这 8 件事, 不是别的。** 主程序 `ai` 目标在非 Windows 上
**编不了**(`CMakeLists.txt` 直接 FATAL_ERROR), 所以:
**Windows 编译、Qt 界面接线、TensorRT 推理、采集卡行为、驱动下发、以及
`Apotheosis.exe` 到底能不能起来"—— 全部没有被验证过。**
不要用"逻辑测试全绿"去说"改好了"或"能跑了"。

★★ **第三轮的控制器层尤其要注意这一点**: `control_layer_test` 全绿 + 23 处反向变异
全红, 证明的是**"这套数值公式按设计实现了"**。它**不证明**:
- 参数取值合理(六个增益的实测定值**一个都还没有**);
- 接上真机后表现如何（★ 接线**已在源码层完成**，但**没有在 Windows 上编过**，
  更**没有在真机上跑过** —— 本机只有逻辑回归；见下面"第三轮续"那节）;
- Windows 上能编过(本机只编了 `control_layer_test`, **主程序没碰过**)。

A successful logic test or appearance preview does not prove Windows application compilation,
driver behavior, GPU inference or hardware operation. State what was actually tested.
Avoid modifying third-party modules or generated build directories.
