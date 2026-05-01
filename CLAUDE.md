# CLAUDE.md

This file is a fresh project-orientation note for AI assistants. It is based on reading the current source tree and build configuration only; do not treat older assistant summaries as authoritative.

## Project Identity

- Windows C++ desktop application that builds a single executable named `ai`.
- Runtime purpose: capture frames, run object detection, choose targets, and drive mouse movement through a selectable input backend.
- UI purpose: ImGui/DX11 overlay launcher for configuring capture, model/backend, hotkeys, aiming behavior, depth mask, hardware, debug views, and session start/stop.
- The codebase is Windows-first and depends heavily on Win32, Direct3D/DXGI, OpenCV, ONNX Runtime DirectML, CUDA/TensorRT, serial/HID hardware integrations, and ImGui.

## Build System

- Primary generator: root `CMakeLists.txt`; Visual Studio project files also exist under `Apotheosis/`.
- Main target: `add_executable(ai ...)` in `CMakeLists.txt`.
- Both DirectML and TensorRT paths are compiled into the same binary.
- CMake expects local dependencies under:
  - `packages/Microsoft.ML.OnnxRuntime.DirectML.*`
  - `packages/Microsoft.AI.DirectML.*`
  - `Apotheosis/modules/opencv/build/install`
  - `Apotheosis/modules/TensorRT-10.15.1.29`
- CUDA is enabled unconditionally in CMake and uses CUDA Toolkit 12.9 style paths/options.
- Runtime DLL copying is controlled by `AIMBOT_COPY_RUNTIME_DLLS` and copies OpenCV, ONNX Runtime, DirectML, TensorRT, cuDNN, and `ghub_mouse.dll` when present.
- `tools/build_opencv_cuda.ps1` is the local helper for building the vendored OpenCV CUDA install.

## Runtime Architecture

- Entry point: `Apotheosis/Apotheosis.cpp::main()`.
- Global shared state lives mostly in `Apotheosis.cpp`, `capture.cpp`, and `detection_buffer.cpp`; changes should respect existing synchronization.
- Startup flow:
  1. Configure console/title and required folders such as `models`, `screenshots`, `models/depth`.
  2. Load `config.ini` via `Config::loadConfig()`.
  3. Probe CUDA/TensorRT availability and fall back to DirectML when necessary.
  4. Create selected mouse/input device wrappers.
  5. Construct `MouseThread` and `runtime::InferenceSession`.
  6. Start keyboard listener and overlay threads.
  7. Overlay controls when inference sessions start/stop.
- Session flow is encapsulated by `runtime::InferenceSession`:
  - owns the current detector instance;
  - starts capture, detector inference, and mouse loop threads;
  - exposes current backend/model and last error for UI;
  - can be stopped and restarted without restarting the process.

## Data Pipeline

- Capture layer (`Apotheosis/capture/`):
  - `captureThread()` selects the configured capture backend.
  - Supported capture modes are `udp_capture`, `tcp_capture`, and `opencv_capture`.
  - `IScreenCapture` exposes CPU frames and optionally GPU frames for zero-copy decode paths.
  - Shared outputs include `latestFrame`, `frameQueue`, capture FPS counters, and a detection suppression mask.
  - UDP/TCP capture can use GPU JPEG decode through `gpu_jpeg_decoder` when available.
- Detection layer (`Apotheosis/detector/`):
  - `IDetector` is the backend abstraction.
  - `DirectMLDetector` uses ONNX Runtime DirectML.
  - `TrtDetector` uses TensorRT/CUDA and has GPU preprocessing support through `cuda_preprocess.cu`.
  - Detector results are published to global `detectionBuffer` as boxes/classes plus a monotonically increasing version.
  - Post-processing lives in `postProcess.*`; model metadata/shape inspection lives in `model_inspector.*`.
- Mouse/target layer (`Apotheosis/mouse/`, `Apotheosis/runtime/mouse_thread_loop.cpp`):
  - `mouseThreadFunction()` consumes `detectionBuffer`, active hotkey profile, depth suppression mask, and tracker state.
  - `AimbotTarget` and `MultiTargetTracker` implement target selection, locking, smoothing/prediction, priority handling, and debug track state.
  - `MouseThread` applies runtime parameters and sends movement/click commands to the selected hardware/input backend.
- Depth layer (`Apotheosis/depth/`):
  - `DepthAnythingTRT` provides TensorRT depth inference.
  - `depth_mask` and `depth_utils` generate/maintain suppression masks used to filter detections/targets.

## UI and Configuration

- Config model: `Apotheosis/config/config.h` and `config.cpp`.
- Config file: `config.ini` in the working directory by default.
- `Config::resetToDefaults()`, `loadConfig()`, and `saveConfig()` define authoritative keys/defaults.
- UI code is split by page/section under `Apotheosis/overlay/`:
  - `draw_ai.cpp`: backend/model/inference options.
  - `draw_capture.cpp`: capture source and resolution/FPS options.
  - `draw_depth.cpp`: depth inference and depth-mask settings.
  - `draw_hardware.cpp`: Win32/GHUB/Arduino/KMBOX/MAKCU input settings.
  - `draw_hotkeys.cpp`: active hotkey profiles and aim classes.
  - `draw_target.cpp`: target selection/aim tuning.
  - `draw_debug.cpp`: preview, replay, screenshots, debug diagnostics.
  - `draw_session.cpp`: launcher/session controls.
  - `draw_settings.h` and `ui_sections.h`: shared UI section helpers.
- UI changes should call `OverlayConfig_MarkDirty()` when they mutate config, and respect the existing dirty/save pattern.
- `configMutex` is a recursive mutex used when UI/runtime read or mutate shared config.

## Input and Hardware Backends

- `input_method` options are `WIN32`, `GHUB`, `ARDUINO`, `KMBOX_NET`, `KMBOX_A`, and `MAKCU`.
- Device construction and teardown are centralized in `createInputDevices()` and `assignInputDevices()` in `Apotheosis.cpp`.
- Implementations live under `Apotheosis/mouse/`:
  - Win32 behavior is handled inside `MouseThread`/mouse code.
  - GHUB uses `ghub_mouse.dll` through `ghub.*`.
  - Arduino uses serial code in `Arduino.*` and the embedded serial library.
  - KMBOX and MAKCU have separate network/HID/serial wrappers.
- Runtime input-method changes are signaled through `input_method_changed` and handled by the mouse loop.

## Threading and Shared State

- Important global stop flags:
  - `shouldExit`: whole process shutdown.
  - `session_stop_requested`: current inference session shutdown.
- Important change flags:
  - `detection_resolution_changed`
  - `capture_method_changed`
  - `capture_fps_changed`
  - `detector_model_changed`
  - `input_method_changed`
- `StartThreadGuarded()` wraps long-running threads and requests shutdown on uncaught exceptions.
- Avoid adding unsynchronized access to global config, detection buffers, frame buffers, or tracker debug state.

## Repository Map

- `Apotheosis/Apotheosis.cpp`: process entry, global state, startup, input-device lifecycle.
- `Apotheosis/config/`: config schema, defaults, ini load/save.
- `Apotheosis/capture/`: frame acquisition from UDP/TCP/OpenCV and optional GPU JPEG decode.
- `Apotheosis/detector/`: DirectML/TensorRT detector implementations, preprocessing, postprocess, shared detection buffer.
- `Apotheosis/depth/`: TensorRT depth inference and depth-mask generation.
- `Apotheosis/mouse/`: target model, mouse runtime, hardware integrations, PID/Kalman support.
- `Apotheosis/runtime/`: session orchestration, active hotkey state, telemetry, CUDA availability probe, mouse loop.
- `Apotheosis/overlay/`: ImGui overlay/launcher and per-section UI drawers.
- `Apotheosis/keyboard/`: keyboard listener and keycode mapping.
- `Apotheosis/mem/`: GPU resource and CPU affinity helpers.
- `Apotheosis/tensorrt/`: TensorRT engine/export helper wrapper.
- `Apotheosis/scr/`: file picker and miscellaneous utilities.
- `Apotheosis/imgui/`: vendored ImGui sources.
- `Apotheosis/modules/`: vendored third-party dependencies; avoid editing unless specifically working on dependency integration.
- `packages/`: NuGet-restored binary packages; do not edit manually.
- `build/`, `x64/`: generated build output; do not edit manually.

## Development Rules for Future Agents

- Prefer CMake-oriented changes unless the user explicitly asks for Visual Studio project-file updates too.
- Do not edit vendored dependencies (`Apotheosis/modules/`, `packages/`) for normal feature work.
- Keep changes surgical: this app uses many global runtime contracts and cross-thread flags.
- When adding config fields, update all three places together: `Config` declaration, default/reset logic, load/save logic, and any overlay UI that mutates it.
- When adding a detector/backend feature, update `IDetector` only if both DirectML and TensorRT contracts remain coherent.
- When changing capture/detection resolution behavior, audit capture thread, detector input shape handling, mouse coordinate scaling, tracker reset, and UI dirty flags.
- When changing input hardware, update creation/assignment, config load/save, UI hardware page, and `MouseThread` dispatch.
- Be careful with comments containing mojibake in existing files; do not reformat unrelated areas just to clean encoding.
- Build/test commands may require a full Windows Visual Studio + CUDA + TensorRT environment; if unavailable, state that clearly instead of guessing.
