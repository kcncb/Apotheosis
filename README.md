# Apotheosis

Windows x64 实时视觉检测与鼠标控制程序，使用中文 Qt6 Widgets 界面。

当前主链路：采集卡（Media Foundation）→ 解码/转色/中心裁切 → TensorRT 或 DirectML 检测 → 目标关联 → 时间一致的状态估计与阶段控制 → 浮点 AimPath / 计数换算 → MAKCU 或 MAKCUNEW。

## 使用

1. 从完整运行目录启动 `Apotheosis.exe`，模型放在程序旁的 `models/`。
2. 在「配置 → 画面采集」选择采集卡及它实际支持的格式、分辨率和帧率。当前支持 NV12、MJPG、YUY2、RGB32；不支持的组合会报错，不会静默切换设备或格式。
3. 选择模型、推理后端、输入硬件和瞄准热键。采集中心裁切尺寸跟随模型输入尺寸。
4. 在「概览」启动或停止推理。启停在后台执行，过程中暂时禁用配置修改；关闭窗口会先停止会话。
5. 查看性能统计、日志和独立检测预览。默认配置位于程序旁的 `config.ini`，延迟日志位于 `logs/`。

当前不再使用旧版 ImGui 启动界面，也没有 README 旧版提到的 F2/F3/F4/Home 全局快捷键；瞄准使用已配置的热键。

## 构建与验证

- [构建说明](docs/build.md)：VS 2026、x64、Release、`ai` 目标；产物 `build/cuda/Release/Apotheosis.exe`。
- 一个 portable 主程序同时包含 TensorRT 和 DirectML。选择 DirectML 不代表可以省略这个构建所需的 CUDA 依赖。
- OpenCV 使用普通预编译包；GPU 图像处理由项目自有 CUDA/NPP/nvJPEG 代码负责，不要求重新编译 OpenCV CUDA 模块。
- 独立逻辑回归可在 macOS/Linux/Windows 上运行，不需要 CUDA、Qt 或采集卡：

```sh
cmake -S . -B build/logic-tests -DAPOTHEOSIS_LOGIC_TESTS_ONLY=ON
cmake --build build/logic-tests --config Release
ctest --test-dir build/logic-tests -C Release --output-on-failure
```

逻辑测试覆盖采集能力校验、逐帧延迟与丢帧统计、设备帧龄有效性、异步等待取消、GPU 完成事件所有权、原始帧边界和移动指令时间戳。事件测试使用测试专用 CUDA 替身，不等价于 GPU 实机验证。

## 延迟口径

T0 是 Media Foundation 样本回调入口，之后的样本拷贝、排队、解码、转色及检测输入等待计入「接收→取帧」。TRT/DML 都按帧保留时间戳；设备侧帧龄单独显示，缺失、无效、过期均显示不可用。

当前 E2E 是软件观测范围，不包含设备打戳前的 HDMI 流水线，也不代表鼠标硬件执行或游戏画面响应完成。设备帧龄与软件链路使用不同统计窗口，不应直接相加当作逐帧真值。

## 代码入口

- `Apotheosis/Apotheosis.cpp`：进程入口、输入设备生命周期、Qt 主循环。
- `Apotheosis/runtime/inference_session.cpp`：会话启停及工作线程回收。
- `Apotheosis/capture/`、`detector/`、`mouse/`：采集、检测、控制链路。
- `qt_ui/`：中文界面与配置桥接；`qt_ui/preview/` 是独立外观预览，不启动真实推理。
- `Apotheosis/config/config.h`、`config.cpp`：配置字段、默认值和读写的权威来源。

控制链设计见 [控制架构](docs/control-architecture.md)。

其他参考：[配置](docs/config.md)、[调参](docs/tuning.md)、[界面设计](docs/art-design.md)、[许可证](LICENSE)。旧版操作细节若与当前界面冲突，以源码和当前界面为准。
