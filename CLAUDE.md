# Project orientation

Read `AGENTS.md` first. UI text stays Chinese, source files stay UTF-8, and Chinese narrow strings use legal `u8` literals. Use UTF-8/wide filesystem paths for model names.

## Current build

- Windows x64 C++20 / CUDA C++17 application with a Qt6 Widgets UI.
- Root `CMakeLists.txt` is the main build; recommended generator: Visual Studio 18 2026.
- Target `ai`, executable `build/cuda/Release/Apotheosis.exe`.
- A single portable binary includes TensorRT and DirectML. Do not restore old multi-build paths.
- Qt prefix, TensorRT, cuDNN and CRT locations are CMake cache paths. See `docs/build.md` for actual defaults.
- Ordinary prebuilt OpenCV 4.13.0; GPU operations use `GpuImage`, custom kernels, NPP and nvJPEG. No OpenCV CUDA rebuild is required.
- `APOTHEOSIS_LOGIC_TESTS_ONLY=ON` builds only independent regressions on any platform; it is not another application/backend build.

## Runtime

- `Apotheosis/Apotheosis.cpp`: entry, input devices, Qt main loop.
- `runtime/InferenceSession`: serialized start/stop, detector ownership and worker cleanup. Qt dispatches operations asynchronously; no widget is touched from its operation worker.
- `capture/capture.cpp`: only the Media Foundation capture-card backend is currently created. No UDP/TCP/raw-Ethernet backend is built into this path.
- `capture/mf_capture.cpp`: asynchronous Source Reader callback with a cancellable sample wait; bounded processing/decode workers; latest output frames. Device capability selection is strict.
- Each frame carries its callback-entry steady-clock timestamp through CPU/GPU output and the detector input slot. Input dequeue establishes T1; publishing uses that frame's T0/T1, never the latest global timestamp.
- `detector/`: TensorRT / DirectML implementations of `IDetector`. TensorRT keeps per-slot timestamps for double buffering.
- `mouse/boss_aim.cpp`: target association and aim point only — 锁谁, 以及瞄框内哪个点。它不再包含任何控制器。
- **控制回路**: 新 PID 在 `mouse/aim_pid.h` + `mouse/aim_motion.h` + `mouse/anchor_observer.h`, 接入点是 `boss_aim.cpp::tick()`。误差/积分/微分/前馈全程在【检测图像素】域用 double 计算, 只在出口取整一次成【整数鼠标计数】, 取整零头攒到下一拍(所以没有"先把浮点误差取整再算"造成的死区)。三个增益都带时间量纲(`u = dt*Kp*[e + Ki*∫e + Kd*e']`), 换帧率不用重调。
- **瞄准点(位置)路径不过滤**(2026-09-12): 控制器吃【原始瞄点】—— `AnchorObserver` 的 τ=0, α=1, 甩到瞄点永远是一拍。"瞄点滤波(anchor_filter_ms)"旋钮已从 config/UI 移除。观测器现在只提供两件东西: ①**速度**(给前馈), ②野值门限(>40px 整拍丢弃且位置原地不动)。速度与位置路径解耦, 所以速度估错也不会让准星抽。
- **观测器的输入延迟 = 链路死区(46ms)**, 见下面那条; 它必须和 Smith 补偿用同一个数, 而且不能超过它。用错(尤其用成 11ms)会让速度估计混进 `k*Δu/dt` 的假速度 → 乘前馈时间变成上百像素假误差。回归测试 `[15]④c/④d` 是两组对照(11ms: v̂=0、末段 0.32 counts; 75ms: v̂=395px/s、指令 155 counts 发散)。
- **链路死区 = 46ms**(`boss_aim.h` 的 `kAimDeadTimeS`) —— 整个回路最关键的物理量, 从实机日志反推: ①Kp=100 时误差以 5.00Hz/200ms 摆动 = 24 拍, 离散"积分+纯延迟"环路的振荡周期是 `2(2d+1)` 拍 => d = 5.5 拍 ≈ 46ms, 临界每拍增益 `g_crit = 2sin(π/(2(2d+1))) = 0.256`, 而实机 `g = Kp*dt*k = 0.494` = 1.9 倍临界 —— 这就是"Kp=30 稳、Kp=100 抽"的全部原因(Kp=30 时 g=0.148); ②起始那几拍连发 259 counts 而画面里的瞄点 50ms 内纹丝不动。**不要再用"软件 E2E ≈11ms"当这个量**: 它漏掉了 HID+游戏帧+显示+采集缓冲。
- **在途自身位移补偿(Smith)**(`AnchorObserver::inFlightPx(死区)` × `kAimDeadTimeCompScale=0.8` → PID 的 `pending_self_motion_px`) —— 把"已经发出去、游戏里已生效、只是画面还没回来"的那批计数从看到的误差里扣掉, 环路就退化成 `y_n = y_{n-1} - g*y_{n-1}`(每拍收缩 1-g 倍, Kp=100 时一拍走一半), 于是 100 的增益既能瞬时贴上去又不振荡。★ 补偿方向不对称: 补**不足**只是回到原来的延迟(安全), 补**过头**会把已生效的指令再扣一遍 → 正反馈发散, 所以只补 80%, 且窗口不许超过真实死区。回归测试 `tests/aim_pid_test.cpp [17]` 里有强度扫描表。
- 旧 AVA PIDF 整条管线(pidf_mode1/mode2、postprocess、update、axis_policy、aim_movement_pipeline、controller_orchestration、pid_input、qx_curve、process_humanization、humanization_math、热键旁路、mouse_output、timed_button)已于 2026-09-11 删除; 更早的 `predictive_controller`(1a5a792)也一并删除 —— 它的自运动补偿把一个写死的"像素/计数"标定增益放在反馈回路内部, 标定不准就在锚点附近每拍反向极限环。新实现需要同一个量, 但它是【在线估出来的】(aim_motion.h 的窗口最小二乘, 带激励/相关性/残差检查), 估不出来就一直不 ready, 前馈自动关闭 → 退化成普通 PID, 没有任何写死的标定常数。
- `pidf_kp/ki/kd/kf/lr_*` 槽位沿用但语义已换(单位见 config.h): Kp 计数/(像素·秒)、Ki 1/秒、Kd 秒、kf 提前量 秒、lr 延迟预测 秒。`pidf_mapping_version < 4` 的旧配置在这五个值上一律回到新默认(旧 Kf=1 会变成 1 秒的提前量)。
- 前馈三个环节的分工(2026-09-12 改定): ①**在途自身位移的扣除** = Smith 补偿, 归 `AnchorObserver::inFlightPx(kAimDeadTimeS)` × 0.8, 由 `boss_aim.cpp` 塞进 PID 的 `pending_self_motion_px`(窗口必须 = 真实死区 46ms, 不是 11ms; 也不能超过它); ②**延迟预测**只剩"死区里目标走掉的距离", 推荐值就是 46ms 那个量级, 填大了就是额外的提前量; ③**提前量**是主动瞄未来的位置, 稳态偏移 = `v*提前量`。②③两项都乘一个**速度噪声门**(`aim_pid.cpp` 的 kFeedforwardVelFloorPxS=60 / FullPxS=110): |v̂|<60px/s(实测静止目标的 v̂ 噪声 p99=46、max=52)时完全不给前馈, >110 满额 —— 否则静止目标的噪声速度会被乘成几像素假误差, 在 Kp=100 下就是瞄点上嗡嗡抖。日志里的 `fsx/fsy` 是这个门, `pndx/pndy` 是在途补偿量。匀速目标的滞后靠 Ki 消。
- `mouse/aim_path.h` 仍然保留可用于输出整形, 接在控制器之后(`mouse_thread_loop.cpp`)。
- 闭环回归: `tests/aim_pid_test.cpp` —— 假游戏(目标有世界速度、镜头按我们的计数转、测量与执行都有延迟), 覆盖收敛/稳态滞后/观测器标定/提前量/限幅/取整余量/复位, 以及观测器(τ=0 零滞后、输入延迟估错会发散、野值门限、速度噪声统计)和速度前馈噪声门。`ctest --test-dir build/logic-tests -C Release`。
- `crosshair/`: reference-point colour detection. Preview/host copies run off the inference capture path where possible.
- Input methods: MAKCU and MAKCUNEW. Reconnection detaches borrowed pointers before destroying a device; `inputDeviceMutex` protects global device pointer readers, and MouseThread's input mutex protects sends.
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

Run existing logic tests via CMake/CTest (`docs/build.md`). Tests include per-frame timing, overwritten input counts, device timestamp validity/freshness, cancelled sample waits and event ownership with an instrumented CUDA API.

A successful logic test or appearance preview does not prove Windows application compilation, driver behavior, GPU inference or hardware operation. State what was actually tested. Avoid modifying third-party modules or generated build directories.
