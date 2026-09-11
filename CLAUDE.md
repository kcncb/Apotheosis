# Project orientation

Read `AGENTS.md` first. UI text stays Chinese, source files stay UTF-8, and Chinese narrow strings use legal `u8` literals. Use UTF-8/wide filesystem paths for model names.

## Current build

- Windows x64 C++20 / CUDA C++17 application with a Qt6 Widgets UI.
- Root `CMakeLists.txt` is the main build; recommended generator: Visual Studio 18 2026.
- Target `ai`, executable `build/cuda/Release/ai.exe`.
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
- `mouse/boss_aim.cpp`: association and raw anchor observations. AVA association predictions do not feed the control observer as new measurements.
- `mouse/control/predictive_controller.*`: timestamped state observer, independent acquire/settle/track control clock, calibrated output mapper and final-command journal. Ki stays zero. `aim_path.h` shapes floating pixel deltas before the single integer rounding boundary.
- Legacy PIDF remains a reference/test implementation; it is not selectable in the live application. See `docs/control-architecture.md`.
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
