cmake_minimum_required(VERSION 3.24)

project(Apotheosis LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Apotheosis only supports Windows builds.")
endif()

if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio (17 2022|18 2026)$")
    message(FATAL_ERROR "Use Visual Studio 2022 or 2026 generator. Current: ${CMAKE_GENERATOR}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Qt6 requires AUTOMOC for Q_OBJECT macro processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64" CACHE PATH "Qt6 prefix")

if(NOT CMAKE_CUDA_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
    string(APPEND CMAKE_CUDA_FLAGS " --allow-unsupported-compiler")
endif()
enable_language(CUDA)

set(SRC_DIR "${CMAKE_SOURCE_DIR}/Apotheosis")
set(PACKAGES_DIR "${CMAKE_SOURCE_DIR}/packages")
set(OPENCV_ROOT "${SRC_DIR}/modules/opencv/prebuilt/opencv/build")
set(TENSORRT_ROOT "${SRC_DIR}/modules/TensorRT-10.15.1.29" CACHE PATH "TensorRT 10.15 root")
set(CUDNN_ROOT "$ENV{ProgramW6432}/NVIDIA/CUDNN/v9.17" CACHE PATH "cuDNN 9 root")
set(MSVC_REDIST_DIR "$ENV{ProgramW6432}/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.50.35710/x64/Microsoft.VC145.CRT" CACHE PATH "MSVC x64 runtime redist")
set(WINDOWS_DEBUGGERS_X64_DIR "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Debuggers/x64" CACHE PATH "Windows Debugging Tools x64")
set(GHUB_MOUSE_DLL "${SRC_DIR}/ghub_mouse.dll")

function(pick_latest_dir out_var glob_pattern)
    file(GLOB candidates LIST_DIRECTORIES true "${glob_pattern}")
    if(candidates)
        list(SORT candidates COMPARE NATURAL ORDER DESCENDING)
        list(GET candidates 0 latest)
        set(${out_var} "${latest}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(require_file path label)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${label} not found: ${path}")
    endif()
endfunction()

function(copy_runtime target path)
    if(EXISTS "${path}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

pick_latest_dir(ONNXRUNTIME_DIR "${PACKAGES_DIR}/Microsoft.ML.OnnxRuntime.DirectML.*")
pick_latest_dir(DIRECTML_DIR "${PACKAGES_DIR}/Microsoft.AI.DirectML.*")
pick_latest_dir(OPENCV_VC_DIR "${OPENCV_ROOT}/x64/vc*")
if(OPENCV_VC_DIR STREQUAL "")
    set(OPENCV_VC_DIR "${OPENCV_ROOT}/x64/vc18")
endif()

set(OPENCV_INCLUDE_DIR "${OPENCV_ROOT}/include")
set(OPENCV_LIB "${OPENCV_VC_DIR}/lib/opencv_world4130.lib")
set(OPENCV_DLL "${OPENCV_VC_DIR}/bin/opencv_world4130.dll")

if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(CPPWINRT_INCLUDE_DIR "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
endif()
if(NOT EXISTS "${CPPWINRT_INCLUDE_DIR}/winrt/base.h")
    pick_latest_dir(CPPWINRT_INCLUDE_DIR "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
endif()

require_file("${OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" "OpenCV headers")
require_file("${OPENCV_LIB}" "OpenCV library")
require_file("${OPENCV_DLL}" "OpenCV runtime")
require_file("${ONNXRUNTIME_DIR}/build/native/include/onnxruntime_cxx_api.h" "ONNX Runtime headers")
require_file("${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.lib" "ONNX Runtime library")
require_file("${DIRECTML_DIR}/bin/x64-win/DirectML.dll" "DirectML runtime")
require_file("${TENSORRT_ROOT}/include/NvInfer.h" "TensorRT headers")
require_file("${TENSORRT_ROOT}/lib/nvinfer_10.lib" "TensorRT nvinfer library")
require_file("${TENSORRT_ROOT}/lib/nvonnxparser_10.lib" "TensorRT nvonnxparser library")
require_file("${CPPWINRT_INCLUDE_DIR}/winrt/base.h" "C++/WinRT headers")
require_file("${MSVC_REDIST_DIR}/vcruntime140.dll" "MSVC runtime redistributables")

find_package(CUDAToolkit REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(QT_UI_DIR "${CMAKE_SOURCE_DIR}/qt_ui")

set(AVA_EXACT_SOURCES
    "${SRC_DIR}/mouse/ava_exact/aim_key_selector.cpp"
    "${SRC_DIR}/mouse/ava_exact/aim_movement_pipeline_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/controller_orchestration_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/hotkey_eval_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/key_vk_table_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/humanization_math_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/process_humanization_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/target_region_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/target_selection_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/target_selector_config_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/target_tracker_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/target_selector_top_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/target_to_aimpoint_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/pid_input_pipeline.cpp"
    "${SRC_DIR}/mouse/ava_exact/qx_curve_config_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/qx_curve_target_reset_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/qx_curve_step_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/pidf_postprocess_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/pidf_update_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/pidf_mode1_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/pidf_mode2_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/pidf_axis_policy_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/mouse_output_exact.cpp"
    "${SRC_DIR}/mouse/ava_exact/timed_button_exact.cpp"
)

set(APP_SOURCES
    "${SRC_DIR}/Apotheosis.cpp"
    "${SRC_DIR}/capture/auto_capture.cpp"
    "${SRC_DIR}/capture/capture.cpp"
    "${SRC_DIR}/capture/capture_utils.cpp"
    "${SRC_DIR}/capture/gpu_color_ops.cu"
    "${SRC_DIR}/capture/gpu_jpeg_decoder.cpp"
    "${SRC_DIR}/capture/gpu_h264_decoder.cpp"
    "${SRC_DIR}/capture/eth_capture.cpp"
    "${SRC_DIR}/capture/mf_capture.cpp"
    "${SRC_DIR}/capture/opencv_capture.cpp"
    "${SRC_DIR}/capture/tcp_capture.cpp"
    "${SRC_DIR}/capture/udp_capture.cpp"
    "${SRC_DIR}/capture/avermedia/avermedia_sdk.cpp"
    "${SRC_DIR}/capture/avermedia/avermedia_capture.cpp"
    "${SRC_DIR}/config/config.cpp"
    "${SRC_DIR}/crosshair/crosshair_detector.cpp"
    "${SRC_DIR}/crosshair/color_picker.cpp"
    "${SRC_DIR}/crosshair/laser_detector.cpp"
    "${SRC_DIR}/crosshair/flashlight_detector.cpp"
    "${SRC_DIR}/crosshair/flashlight_tuning.cpp"
    "${SRC_DIR}/crosshair/flashlight_tracker.cpp"
    "${SRC_DIR}/crosshair/flashlight_runtime.cpp"
    "${SRC_DIR}/crosshair/glass_filter.cpp"
    "${SRC_DIR}/crosshair/glass_tuning.cpp"
    "${SRC_DIR}/crosshair/glass_runtime.cpp"
    "${SRC_DIR}/crosshair/crosshair_runtime.cpp"
    "${SRC_DIR}/depth/depth_anything_trt.cpp"
    "${SRC_DIR}/depth/depth_mask.cpp"
    "${SRC_DIR}/depth/depth_utils.cpp"
    "${SRC_DIR}/detector/cuda_preprocess.cu"
    "${SRC_DIR}/detector/detection_buffer.cpp"
    "${SRC_DIR}/detector/dml_detector.cpp"
    "${SRC_DIR}/detector/model_inspector.cpp"
    "${SRC_DIR}/detector/postProcess.cpp"
    "${SRC_DIR}/detector/trt_detector.cpp"
    "${SRC_DIR}/keyboard/keycodes.cpp"
    "${SRC_DIR}/keyboard/keyboard_listener.cpp"
    "${SRC_DIR}/macro/macro_keymap.cpp"
    "${SRC_DIR}/macro/macro_input.cpp"
    "${SRC_DIR}/macro/lua_runtime.cpp"
    "${SRC_DIR}/macro/ghub_api.cpp"
    "${SRC_DIR}/mem/cpu_affinity_manager.cpp"
    "${SRC_DIR}/mem/gpu_image.cpp"
    "${SRC_DIR}/mem/gpu_resource_manager.cpp"
    "${SRC_DIR}/model_crypto/model_crypto.cpp"
    "${SRC_DIR}/auth/auth_client.cpp"
    "${SRC_DIR}/auth/auth_state.cpp"
    "${SRC_DIR}/modules/makcu/src/makcu.cpp"
    "${SRC_DIR}/modules/makcu/src/serialport.cpp"
    "${SRC_DIR}/mouse/AimbotTarget.cpp"
    "${SRC_DIR}/mouse/Arduino.cpp"
    "${SRC_DIR}/mouse/boss_aim.cpp"
    ${AVA_EXACT_SOURCES}
    "${SRC_DIR}/mouse/ghub.cpp"
    "${SRC_DIR}/mouse/hid.c"
    "${SRC_DIR}/mouse/KmboxAConnection.cpp"
    "${SRC_DIR}/mouse/kmboxA.cpp"
    "${SRC_DIR}/mouse/kmboxNetConnection.cpp"
    "${SRC_DIR}/mouse/kmbox_net/kmboxNet.cpp"
    "${SRC_DIR}/mouse/Makcu.cpp"
    "${SRC_DIR}/mouse/mouse.cpp"
    "${SRC_DIR}/overlay/app_log.cpp"
    "${SRC_DIR}/overlay/preview_window.cpp"
    "${SRC_DIR}/runtime/active_hotkey.cpp"
    "${SRC_DIR}/runtime/aim_telemetry.cpp"
    "${SRC_DIR}/runtime/cuda_availability.cpp"
    "${SRC_DIR}/runtime/config_snapshot.cpp"
    "${SRC_DIR}/runtime/config_snapshot.h"
    "${SRC_DIR}/runtime/event_orchestrator.cpp"
    "${SRC_DIR}/runtime/inference_session.cpp"
    "${SRC_DIR}/runtime/mouse_thread_loop.cpp"
    "${SRC_DIR}/scr/file_picker.cpp"
    "${SRC_DIR}/scr/other_tools.cpp"
    "${SRC_DIR}/tensorrt/nvinf.cpp"
    # Qt UI sources
    "${QT_UI_DIR}/MainWindow.cpp"
    "${QT_UI_DIR}/MainWindow.h"
    "${QT_UI_DIR}/config/ConfigManager.cpp"
    "${QT_UI_DIR}/config/ConfigManager.h"
    "${QT_UI_DIR}/config/config_bridge.cpp"
    "${QT_UI_DIR}/config/config_bridge.h"
    "${QT_UI_DIR}/pages/AiModelPage.cpp"
    "${QT_UI_DIR}/pages/AiModelPage.h"
    "${QT_UI_DIR}/pages/AutoCapturePage.cpp"
    "${QT_UI_DIR}/pages/AutoCapturePage.h"
    "${QT_UI_DIR}/pages/CapturePage.cpp"
    "${QT_UI_DIR}/pages/CapturePage.h"
    "${QT_UI_DIR}/pages/CrosshairPage.cpp"
    "${QT_UI_DIR}/pages/CrosshairPage.h"
    "${QT_UI_DIR}/pages/DebugPage.cpp"
    "${QT_UI_DIR}/pages/DebugPage.h"
    "${QT_UI_DIR}/pages/EventPage.cpp"
    "${QT_UI_DIR}/pages/EventPage.h"
    "${QT_UI_DIR}/pages/DepthPage.cpp"
    "${QT_UI_DIR}/pages/DepthPage.h"
    "${QT_UI_DIR}/pages/FlashlightPage.cpp"
    "${QT_UI_DIR}/pages/FlashlightPage.h"
    "${QT_UI_DIR}/pages/GlassFilterPage.cpp"
    "${QT_UI_DIR}/pages/GlassFilterPage.h"
    "${QT_UI_DIR}/pages/HardwarePage.cpp"
    "${QT_UI_DIR}/pages/HardwarePage.h"
    "${QT_UI_DIR}/pages/HotkeyPage.cpp"
    "${QT_UI_DIR}/pages/HotkeyPage.h"
    "${QT_UI_DIR}/pages/LogPage.cpp"
    "${QT_UI_DIR}/pages/LogPage.h"
    "${QT_UI_DIR}/pages/MacroPage.cpp"
    "${QT_UI_DIR}/pages/MacroPage.h"
    "${QT_UI_DIR}/pages/ModelToolsPage.cpp"
    "${QT_UI_DIR}/pages/ModelToolsPage.h"
    "${QT_UI_DIR}/pages/OverviewPage.cpp"
    "${QT_UI_DIR}/pages/OverviewPage.h"
    "${QT_UI_DIR}/pages/SessionPage.cpp"
    "${QT_UI_DIR}/pages/SessionPage.h"
    "${QT_UI_DIR}/pages/StatsPage.cpp"
    "${QT_UI_DIR}/pages/StatsPage.h"
    "${QT_UI_DIR}/pages/TargetPage.cpp"
    "${QT_UI_DIR}/pages/TargetPage.h"
    "${QT_UI_DIR}/widgets/AdaptiveStack.h"
    "${QT_UI_DIR}/widgets/BezierEditor.cpp"
    "${QT_UI_DIR}/widgets/BezierEditor.h"
    "${QT_UI_DIR}/widgets/FreehandCurveEditor.cpp"
    "${QT_UI_DIR}/widgets/FreehandCurveEditor.h"
    "${QT_UI_DIR}/widgets/NeuralCurveTrainer.cpp"
    "${QT_UI_DIR}/widgets/NeuralCurveTrainer.h"
    "${QT_UI_DIR}/widgets/CardWidget.cpp"
    "${QT_UI_DIR}/widgets/CardWidget.h"
    "${QT_UI_DIR}/widgets/FormKit.cpp"
    "${QT_UI_DIR}/widgets/FormKit.h"
    "${QT_UI_DIR}/widgets/IconFont.cpp"
    "${QT_UI_DIR}/widgets/IconFont.h"
    "${QT_UI_DIR}/widgets/LoginDialog.cpp"
    "${QT_UI_DIR}/widgets/LoginDialog.h"
    "${QT_UI_DIR}/widgets/MetricCard.cpp"
    "${QT_UI_DIR}/widgets/MetricCard.h"
    "${QT_UI_DIR}/widgets/SideNav.cpp"
    "${QT_UI_DIR}/widgets/SideNav.h"
    "${QT_UI_DIR}/widgets/StatusBar.cpp"
    "${QT_UI_DIR}/widgets/StatusBar.h"
    "${QT_UI_DIR}/widgets/StatusPill.cpp"
    "${QT_UI_DIR}/widgets/StatusPill.h"
    "${QT_UI_DIR}/widgets/TelemetryChart.cpp"
    "${QT_UI_DIR}/widgets/TelemetryChart.h"
    "${QT_UI_DIR}/widgets/ToggleSwitch.cpp"
    "${QT_UI_DIR}/widgets/ToggleSwitch.h"
    "${QT_UI_DIR}/widgets/TopNavBar.cpp"
    "${QT_UI_DIR}/widgets/TopNavBar.h"
)

add_library(serial_embedded STATIC
    "${SRC_DIR}/modules/serial/src/serial.cc"
    "${SRC_DIR}/modules/serial/src/impl/win.cc"
    "${SRC_DIR}/modules/serial/src/impl/list_ports/list_ports_win.cc")

target_include_directories(serial_embedded PUBLIC "${SRC_DIR}/modules/serial/include")
target_link_libraries(serial_embedded PUBLIC setupapi)

# ---------------------------------------------------------------------------
# Lua 5.1.5 static library — used by the macro layer for G HUB-compatible
# scripting. Sources are vendored under Apotheosis/modules/lua-5.1.5/src/.
# Run modules/lua-5.1.5/fetch_lua.ps1 once to populate that directory; the
# script downloads the official tarball from www.lua.org. We pin 5.1 so
# scripts copy-pasted from G HUB / LGS run unmodified.
# ---------------------------------------------------------------------------
set(LUA_VENDOR_DIR "${SRC_DIR}/modules/lua-5.1.5")
set(LUA_SRC_DIR    "${LUA_VENDOR_DIR}/src")

if(NOT EXISTS "${LUA_SRC_DIR}/lua.h")
    message(FATAL_ERROR
        "Lua 5.1.5 sources missing. Run:\n"
        "  powershell -NoProfile -ExecutionPolicy Bypass -File \"${LUA_VENDOR_DIR}/fetch_lua.ps1\"\n"
        "and re-configure. The script downloads lua-5.1.5.tar.gz from www.lua.org and extracts the .c/.h files into ${LUA_SRC_DIR}.")
endif()

file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${LUA_SRC_DIR}/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac|print)\\.c$")

add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC "${LUA_SRC_DIR}")
set_target_properties(lua_static PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    set_target_properties(lua_static PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # Lua 5.1 uses tmpfile/getenv/strcpy/etc. — silence /sdl + secure-CRT
    # warnings for the vendor sources only. Our own code keeps /sdl.
    target_compile_definitions(lua_static PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE)
    target_compile_options(lua_static PRIVATE /wd4334 /wd4267 /wd4244 /wd4146 /wd4310 /wd4702 /wd4310 /wd4068)
endif()

add_executable(ai WIN32 ${APP_SOURCES})

# Pin CUDA architectures to REAL SMs (no trailing "-virtual" or PTX-only).
# Without this, CMake default emits PTX targeting compute_<future-arch> and
# relies on the NVIDIA driver to JIT it to SASS at load time. With CUDA 12.9
# the PTX version is 8.5+, which older drivers (e.g. R535) reject with
# "the provided PTX was compiled with an unsupported toolchain" — every
# kernel launch then fails silently and our preprocess kernel never writes
# anything to the engine input binding.
#
# Listing concrete SM real-arch numbers makes nvcc embed prebuilt SASS for
# every GPU TensorRT 10 still supports — no driver JIT required:
#   70  = Volta     (V100 / Titan V)
#   75  = Turing    (RTX 20 / GTX 16 / T4)
#   80  = Ampere    (A100)
#   86  = Ampere    (RTX 30)
#   89  = Ada       (RTX 40)
#   90  = Hopper    (H100 / H200)
#   100 = Blackwell datacenter (B100 / B200)
#   120 = Blackwell client     (RTX 50)
# (SM 72 / 87 — Xavier / Orin — are Jetson embedded only and skipped here.)
set_target_properties(ai PROPERTIES
    OUTPUT_NAME "Apotheosis"
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    # 运行机使用的 CMP 40HX/TU106 是 sm_75，开发机 RTX 3060 是 sm_86。
    # 同时保留 compute_86 PTX，供更新架构的驱动进行前向 JIT，避免再次出现
    # “解码/产帧 FPS 正常，但 CUDA 转色核无法启动而画面全黑”。
    CUDA_ARCHITECTURES "75-real;86-real;86-virtual")

foreach(target serial_embedded ai)
    if(MSVC)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endforeach()

target_compile_definitions(ai PRIVATE UNICODE _UNICODE _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN USE_CUDA)
if(MSVC)
    target_compile_options(ai PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8 /Zc:char8_t->
        $<$<COMPILE_LANGUAGE:CXX>:/W3>
        $<$<COMPILE_LANGUAGE:CXX>:/sdl>
        $<$<COMPILE_LANGUAGE:CXX>:/wd4005>
        $<$<COMPILE_LANGUAGE:CXX>:/wd4996>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
    target_link_libraries(ai PRIVATE delayimp)
    target_link_options(ai PRIVATE "/ENTRY:mainCRTStartup" "/DELAYLOAD:nvinfer_10.dll" "/DELAYLOAD:nvonnxparser_10.dll")
endif()

target_include_directories(ai PRIVATE
    "${SRC_DIR}"
    "${SRC_DIR}/auth"
    "${SRC_DIR}/capture"
    "${SRC_DIR}/config"
    "${SRC_DIR}/crosshair"
    "${SRC_DIR}/depth"
    "${SRC_DIR}/detector"
    "${SRC_DIR}/include"
    "${SRC_DIR}/keyboard"
    "${SRC_DIR}/macro"
    "${SRC_DIR}/mem"
    "${SRC_DIR}/mouse"
    "${SRC_DIR}/overlay"
    "${SRC_DIR}/runtime"
    "${SRC_DIR}/scr"
    "${SRC_DIR}/tensorrt"
    "${SRC_DIR}/modules/makcu/include"
    "${SRC_DIR}/modules/serial/include"
    "${SRC_DIR}/modules/stb"
    "${SRC_DIR}/modules/npcap-sdk/Include"
    "${SRC_DIR}/modules/nv-codec-headers"
    "${QT_UI_DIR}"
    "${OPENCV_INCLUDE_DIR}"
    "${ONNXRUNTIME_DIR}/build/native/include"
    "${CPPWINRT_INCLUDE_DIR}"
    "${TENSORRT_ROOT}/include")

if(EXISTS "${CUDNN_ROOT}/include/12.9/cudnn.h")
    target_include_directories(ai PRIVATE "${CUDNN_ROOT}/include/12.9")
elseif(EXISTS "${CUDNN_ROOT}/include/cudnn.h")
    target_include_directories(ai PRIVATE "${CUDNN_ROOT}/include")
endif()

target_link_directories(ai PRIVATE "${TENSORRT_ROOT}/lib")
target_link_libraries(ai PRIVATE
    serial_embedded
    lua_static
    Qt6::Widgets
    "${OPENCV_LIB}"
    "${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.lib"
    CUDA::cuda_driver
    CUDA::cudart
    CUDA::nvjpeg
    CUDA::nppc
    CUDA::nppicc
    CUDA::nppidei
    "${SRC_DIR}/modules/npcap-sdk/Lib/x64/wpcap.lib"
    "${SRC_DIR}/modules/npcap-sdk/Lib/x64/Packet.lib"
    nvinfer_10
    nvonnxparser_10
    bcrypt
    winhttp
    crypt32
    d3d11
    dxgi
    d2d1
    setupapi
    cfgmgr32
    strmiids
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    oleaut32
    winmm
    WindowsApp)

copy_runtime(ai "${SRC_DIR}/assets/apotheosis_logo.png")

copy_runtime(ai "${OPENCV_DLL}")
copy_runtime(ai "${GHUB_MOUSE_DLL}")
copy_runtime(ai "${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.dll")
copy_runtime(ai "${ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime_providers_shared.dll")
copy_runtime(ai "${DIRECTML_DIR}/bin/x64-win/DirectML.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/concrt140.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/msvcp140.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/msvcp140_1.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/vcruntime140.dll")
copy_runtime(ai "${MSVC_REDIST_DIR}/vcruntime140_1.dll")
copy_runtime(ai "${WINDOWS_DEBUGGERS_X64_DIR}/dbghelp.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cudart64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cublas64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cublasLt64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/cufft64_11.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nvjpeg64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppc64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppial64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppicc64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppidei64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppif64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppig64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppim64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppist64_12.dll")
copy_runtime(ai "${CUDAToolkit_BIN_DIR}/nppitc64_12.dll")
copy_runtime(ai "${TENSORRT_ROOT}/bin/nvinfer_10.dll")
copy_runtime(ai "${TENSORRT_ROOT}/bin/nvonnxparser_10.dll")

file(GLOB TENSORRT_RESOURCE_DLLS "${TENSORRT_ROOT}/bin/nvinfer_builder_resource_*.dll")
foreach(runtime_dll IN LISTS TENSORRT_RESOURCE_DLLS)
    copy_runtime(ai "${runtime_dll}")
endforeach()

add_custom_command(TARGET ai POST_BUILD
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Copy-Item -Path '${CUDNN_ROOT}/bin/12.9/*.dll' -Destination '$<TARGET_FILE_DIR:ai>' -Force"
    VERBATIM)

# Deploy Qt6 runtime DLLs next to the executable
add_custom_command(TARGET ai POST_BUILD
    COMMAND "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe"
            --no-translations --no-opengl-sw --no-system-d3d-compiler
            "$<TARGET_FILE:ai>"
    VERBATIM)

# Copy Qt UI assets (stylesheet, icon font) into build dir
add_custom_command(TARGET ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ai>/style"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_UI_DIR}/style/theme.qss"
            "$<TARGET_FILE_DIR:ai>/style/theme.qss"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ai>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_UI_DIR}/assets/tabler-icons.ttf"
            "$<TARGET_FILE_DIR:ai>/assets/tabler-icons.ttf"
    VERBATIM)

add_executable(ava_chain_test EXCLUDE_FROM_ALL
    "${SRC_DIR}/mouse/tests/ava_chain_regression_test.cpp"
    "${SRC_DIR}/mouse/boss_aim.cpp"
    ${AVA_EXACT_SOURCES})
target_include_directories(ava_chain_test PRIVATE
    "${SRC_DIR}/mouse"
    "${OPENCV_INCLUDE_DIR}")
target_link_libraries(ava_chain_test PRIVATE "${OPENCV_LIB}" user32)
target_compile_features(ava_chain_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ava_chain_test PRIVATE /utf-8 /Zc:char8_t-)
endif()

message(STATUS "Apotheosis portable build (Qt6 UI)")
message(STATUS "OpenCV: ${OPENCV_VC_DIR}")
message(STATUS "ONNX Runtime: ${ONNXRUNTIME_DIR}")
message(STATUS "DirectML: ${DIRECTML_DIR}")
message(STATUS "TensorRT: ${TENSORRT_ROOT}")
message(STATUS "cuDNN: ${CUDNN_ROOT}")
message(STATUS "CUDA: ${CUDAToolkit_BIN_DIR}")
