# Qt UI Rewrite Design Spec

## Overview
Replace the current ImGui + DirectX 11 overlay frontend with a standalone Qt Widgets application. The new UI adopts a clean, minimalist visual style (Apple/Figma-inspired) with improved navigation and a redesigned hotkey configuration experience.

## Architecture
- **Standalone Qt process** communicating with the inference engine via shared `config.ini`
- Qt app writes config → inference engine hot-reloads via file watcher
- Real-time stats (FPS, latency) fed back via shared memory or local UDP
- Backend code remains unchanged

## Tech Stack
- Qt 6 Widgets + QSS styling
- CMake build system
- C++17
- Cross-platform (develop on macOS, deploy on Windows)

## Visual Design
- Background: `#F5F5F7`, Cards: `#FFFFFF`, Accent: `#4A7FE5`
- Text: `#1D1D1F` primary, `#86868B` secondary
- Cards: 8px radius, `#E5E5E7` border, subtle shadow
- Font: system default (SF Pro / Segoe UI), 14px body
- Generous whitespace, capsule toggles, rounded sliders

## Window & Navigation
- Default 960×640, min 720×480, resizable
- Title bar with app name + minimize/close + system tray
- **Two-level top tabs**:
  - Level 1: 会话 | 配置 | 控制 | 监控
  - Level 2: sub-pages within each group
- Bottom status bar: inference status, FPS, backend type

## Tab Structure

### 会话 (Session)
- **账号授权** — login, register, model authorization
- **推理启动** — backend selection (TRT/DML), start/stop inference
- **模型工具** — model encryption and authorization

### 配置 (Config)
- **画面采集** — capture method, resolution, FPS, circle mask
- **目标** — class filter table (Delete/Filter/Aim per class)
- **硬件** — input method selection + device-specific params
- **AI 模型** — model file, confidence/NMS thresholds, max detections
- **深度模型** — depth model path, FPS, mask settings

### 控制 (Control)
- **瞄准热键** — profile-based hotkey config (see redesign below)
- **准星找色** — HSV color palette editor, ROI size
- **界面外观** — UI scale, opacity (for future overlay mode)

### 监控 (Monitor)
- **性能统计** — FPS, inference latency charts
- **日志** — application log viewer
- **调试** — screenshot hotkeys, replay, verbose toggle

## Hotkey Configuration Redesign
Left-right split layout:
- **Left panel**: profile list with name + key preview, add/delete/copy/rename
- **Right panel**: 4 collapsible cards:
  1. **按键绑定** — key dropdown(s), multi-key support
  2. **视野 FOV** — FOV X/Y sliders, dynamic FOV toggle + params
  3. **瞄准参数** — speed X/Y, lock strength/radius, smart trigger, threat priority
  4. **轨迹曲线** — preset buttons + Bezier canvas (QPainter)
- Kalman/advanced params in collapsed "Advanced" section
- 3 preset templates (close/mid/long range) for new profiles
- Per-card "Reset to default" button

## Project Structure
```
qt_ui/
├── CMakeLists.txt
├── main.cpp
├── MainWindow.h / .cpp
├── style/
│   └── theme.qss
├── pages/
│   ├── AuthPage.h / .cpp
│   ├── SessionPage.h / .cpp
│   ├── ModelToolsPage.h / .cpp
│   ├── CapturePage.h / .cpp
│   ├── TargetPage.h / .cpp
│   ├── HardwarePage.h / .cpp
│   ├── AiModelPage.h / .cpp
│   ├── DepthPage.h / .cpp
│   ├── HotkeyPage.h / .cpp
│   ├── CrosshairPage.h / .cpp
│   ├── AppearancePage.h / .cpp
│   ├── StatsPage.h / .cpp
│   ├── LogPage.h / .cpp
│   └── DebugPage.h / .cpp
├── widgets/
│   ├── CardWidget.h / .cpp
│   ├── StatusBar.h / .cpp
│   └── BezierEditor.h / .cpp
└── config/
    └── ConfigManager.h / .cpp
```

## Config Integration
- `ConfigManager` wraps `QSettings` to read/write the same `config.ini` format
- Maps all fields from `Config` class (capture, hardware, AI, depth, hotkeys, crosshair, debug)
- File watcher triggers reload signal on external changes
- Dirty tracking to avoid unnecessary writes
