# Codex project notes

- UI 文案必须保留中文；不要为了规避乱码把界面改成英文。
- 修改含中文的 `.cpp/.h` 文件时，使用 UTF-8 保存，并确保中文字符串写成合法 `u8"..."` 字面量。
- 如果遇到旧文件里已有乱码中文，优先恢复成可读中文 UTF-8，而不是删除或英文化。
- 当前推荐编译方式：使用 VS 自带 CMake，生成器 `Visual Studio 18 2026`，平台 `x64`，构建 `Release` 的 `ai` 目标。
- 当前运行/调试产物目录：`build/cuda/Release`，可执行文件：`build/cuda/Release/ai.exe`。
- CUDA/cuDNN/TensorRT/DML 依赖已按 `CMakeLists.txt` 的单一 portable 构建路径配置；不要再恢复旧的多套构建逻辑。
- 中文模型文件名路径要使用 UTF-8 / wide path 处理，避免 `.string()` 在系统代码页无法表示中文时抛异常。
