# 构建与验证

主程序只支持 Windows x64，使用根目录 `CMakeLists.txt` 的单一 portable 构建路径。

## 环境

推荐 Visual Studio 2026（C++ 桌面开发组件和 Windows SDK）自带 CMake，生成器 `Visual Studio 18 2026`，平台 `x64`。CMake 也接受 VS 2022，但其运行库目录需要对应覆盖。

当前依赖路径/版本由根 CMake 定义：

| 依赖 | 默认位置 |
| --- | --- |
| Qt6 Widgets 6.8.3 | `C:/Qt/6.8.3/msvc2022_64`，可用 `CMAKE_PREFIX_PATH` 覆盖 |
| OpenCV 4.13.0 | `Apotheosis/modules/opencv/prebuilt/opencv/build`，需含 `opencv_world4130.lib/.dll` |
| TensorRT 10.15.1.29 | `Apotheosis/modules/TensorRT-10.15.1.29`，可用 `TENSORRT_ROOT` 覆盖 |
| CUDA Toolkit | 由 `find_package(CUDAToolkit)` 定位；当前 DLL 部署规则按 CUDA 12.x 命名及 cuDNN 的 `12.9` 子目录配置 |
| cuDNN 9.17 | `C:/Program Files/NVIDIA/CUDNN/v9.17`，可用 `CUDNN_ROOT` 覆盖 |
| ONNX Runtime DirectML 1.22.0 | 根目录 `packages/Microsoft.ML.OnnxRuntime.DirectML.1.22.0` |
| DirectML 1.15.4 | 根目录 `packages/Microsoft.AI.DirectML.1.15.4` |
| serial、MAKCU、Npcap SDK | `Apotheosis/modules/` 下对应目录 |
| MSVC 运行库 | `MSVC_REDIST_DIR` 指向当前 Visual Studio 的 x64 CRT 目录 |

NuGet 包声明位于 `Apotheosis/packages.config`，例如使用 NuGet CLI 恢复：

```powershell
nuget install Apotheosis/packages.config -OutputDirectory packages
```

不需要 OpenCV CUDA 模块。项目自有 `GpuImage`、CUDA kernel、NPP 和 nvJPEG 负责 GPU 图像处理。仓库中没有旧文档所述的 `tools/build_opencv_cuda.ps1`，不再使用那套步骤。

## 主程序

在仓库根目录运行（若 PATH 中 CMake 不是 VS 自带版本，使用它的完整路径）：

```powershell
cmake -S . -B build/cuda -G "Visual Studio 18 2026" -A x64
cmake --build build/cuda --config Release --target ai
```

产物：`build/cuda/Release/Apotheosis.exe`（目标名仍是 `ai`，只是产物文件名为 `Apotheosis`）。构建自动部署 Qt、模型推理运行库和界面资源；交付时保留完整输出目录，不只复制 exe。C++ 使用 C++20，CUDA 使用 C++17；MSVC 开启 UTF-8，并禁用原生 `char8_t` 以兼容现有 UTF-8 字符串接口。

当前 CUDA 架构为 `75-real;86-real;86-virtual`。这提供 Turing/Ampere 本机代码及 compute_86 PTX，但实际能否运行仍取决于显卡、驱动和 TensorRT 兼容性。

DirectML 与 TensorRT 编入同一个程序，在界面选择。DirectML 运行路径可不使用 CUDA GPU，但构建及打包仍要满足当前主程序的 CUDA 链接依赖。不要恢复另一套旧的 DML-only 构建逻辑。

## 回归测试

只编译纯逻辑测试（支持 macOS/Linux/Windows，无需 Windows SDK/CUDA/Qt）：

```sh
cmake -S . -B build/logic-tests -DAPOTHEOSIS_LOGIC_TESTS_ONLY=ON
cmake --build build/logic-tests --config Release
ctest --test-dir build/logic-tests -C Release --output-on-failure
```

Windows 主构建也可构建并运行这些测试：

```powershell
cmake --build build/cuda --config Release --target latency_probe_test capture_card_caps_test device_frame_age_test interruptible_slot_test gpu_ready_event_test raw_frame_layout_test latest_move_slot_test predictive_controller_test aim_engine_test continuous_tracking_test
ctest --test-dir build/cuda -C Release --output-on-failure
```

控制链回归 `ava_chain_test` 使用实际 OpenCV。便携入口 `aim_engine_test` 使用几何类型替身并调用相同生产控制代码。`aim_scenario_sim` 是旧 PIDF 的参考实验，不代表当前主程序的算法。三者不随 `ai` 目标构建。

逻辑测试不能验证 MF 驱动、实际 CUDA kernel、HID 硬件或完整 UI。Windows 实机还需验证：TRT/DML 启停、启动中关闭窗口、断开采集卡后停止/重连、运行中更换采集格式及输入设备、中文模型路径、延迟分段和设备帧龄失效显示。
