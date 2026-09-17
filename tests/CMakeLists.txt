find_package(Threads REQUIRED)

# 界面几何测试需要 Qt。用 QUIET + 可选: 逻辑测试的卖点是任何平台都能单独编
# (见 CLAUDE.md), 不能因为找不到 Qt 就让整个 logic-tests 配置失败。
# 找不到就跳过这一类测试, 其余照常。
find_package(Qt6 QUIET COMPONENTS Widgets)

set(APP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Apotheosis")
function(add_logic_test target source)
    # 模板化的源文件列表(通过 ARGN 传入额外源文件), 源文件里带 main 函数的是 ${source}
    add_executable(${target} "${source}" ${ARGN})
    target_include_directories(${target} PRIVATE "${APP_SOURCE_ROOT}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /Zc:char8_t-)
    else()
        target_compile_options(${target} PRIVATE -fno-char8_t)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 20)
endfunction()

# ══════════════════════════════════════════════════════════════════════════════
#  ★ 2026-09-17: 瞄准控制链已整条删除, 本文件随之大幅收缩。
#
#  被删掉的测试(连同它们的被测源文件一起 —— 那些源文件已不存在):
#    aim_pid_test / aim_tracker_test / aim_eventsync_test / aim_pid_am_test /
#    aim_path_test / trigger_scope_test / aim_pid_quantum_test / auto_stop_test /
#    inflight_comp_test / dt_smooth_test / aim_scale_test / aim_acceptance_test /
#    autotune_test / autotune_reload_test / autotune_layout_test
#
#  这些回归钉的是 PID 死区、在途补偿、尺度调度、提前量与调参 agent —— 全是
#  已被删除的模块。它们的断言对象不存在了, 留着只会编不过。
#
#  现在剩下的都是【采集 / 推理 / 运行期数据面】这一侧的回归。
# ══════════════════════════════════════════════════════════════════════════════

add_logic_test(latency_probe_test "${APP_SOURCE_ROOT}/runtime/tests/latency_probe_test.cpp")
add_logic_test(capture_card_caps_test "${APP_SOURCE_ROOT}/capture/tests/capture_card_caps_test.cpp")
add_logic_test(device_frame_age_test "${CMAKE_CURRENT_LIST_DIR}/device_frame_age_test.cpp")
add_logic_test(interruptible_slot_test "${CMAKE_CURRENT_LIST_DIR}/interruptible_slot_test.cpp")
add_logic_test(gpu_ready_event_test "${CMAKE_CURRENT_LIST_DIR}/gpu_ready_event_test.cpp")
# This test verifies event ownership with an instrumented CUDA API, not GPU work.
target_include_directories(gpu_ready_event_test BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stubs")
add_logic_test(raw_frame_layout_test "${CMAKE_CURRENT_LIST_DIR}/raw_frame_layout_test.cpp")

# ── 控制器层回归 (2026-09-17 重建) ────────────────────────────────────────────
#
# ★★ 这一组钉的是【通用控制器层】—— ①选靶 ②稳定器 ③α-β滤波 ④瞄点 ⑤⑥PID+量化。
#    它是控制链删除后第一批重建的回归。
#
# ★ 关键设计: 控制层【零依赖】(不引 OpenCV / Windows / Qt), 所以任何平台都能编。
#   这正是它被单独拆出来的理由 —— 主程序 ai 目标在非 Windows 上根本编不了。
#
# ★★ 每条断言都必须能反向验证(改一行代码就变红)。见 tests/control_layer_test.cpp。
# ★ 接线回归: "配置 → 控制器" 的映射。漏映射 = 静默失效(界面能改、跑起来没变),
#   不报错不留痕, 所以它必须可测 —— 这就是把纯映射拆成 aim_loop_config.cpp 的原因。
add_logic_test(control_wiring_test "${CMAKE_CURRENT_LIST_DIR}/control_wiring_test.cpp"
    "${APP_SOURCE_ROOT}/runtime/aim_loop_config.cpp"
    "${APP_SOURCE_ROOT}/control/types.cpp"
    "${APP_SOURCE_ROOT}/control/selector.cpp"
    "${APP_SOURCE_ROOT}/control/stabilizer.cpp"
    "${APP_SOURCE_ROOT}/control/alpha_beta_filter.cpp"
    "${APP_SOURCE_ROOT}/control/anchor.cpp"
    "${APP_SOURCE_ROOT}/control/pid_controller.cpp"
    "${APP_SOURCE_ROOT}/control/aim_controller.cpp")

add_logic_test(control_layer_test "${CMAKE_CURRENT_LIST_DIR}/control_layer_test.cpp"
    "${APP_SOURCE_ROOT}/control/types.cpp"
    "${APP_SOURCE_ROOT}/control/selector.cpp"
    "${APP_SOURCE_ROOT}/control/stabilizer.cpp"
    "${APP_SOURCE_ROOT}/control/alpha_beta_filter.cpp"
    "${APP_SOURCE_ROOT}/control/anchor.cpp"
    "${APP_SOURCE_ROOT}/control/pid_controller.cpp"
    "${APP_SOURCE_ROOT}/control/aim_controller.cpp")

# ── 配置解析 (2026-09-14) ─────────────────────────────────────────────────────
# 需要 config.cpp (解析/夹取) 与 SimpleIni。
# ★ NOMINMAX 是必须的: config.cpp 与 SimpleIni.h 都会用到 std::min/max, 而
#   windows.h 的 min/max 宏会把它们撕成语法错误。主程序是通过 Apotheosis.h 里
#   的定义拿到的, 逻辑测试没有那一层, 所以要显式加。
if(EXISTS "${APP_SOURCE_ROOT}/modules/SimpleIni.h")
add_logic_test(config_migration_test "${CMAKE_CURRENT_LIST_DIR}/config_migration_test.cpp"
    "${APP_SOURCE_ROOT}/config/config.cpp")
target_compile_definitions(config_migration_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── 硬件驱动抽象单元测试 (2026-09-15) ─────────────────────────────────────────
#
# 验证 AimMagic 驱动工厂形状的抽象层契约:
#   三家后端名称 / 能力位中文串查 / 状态串理由 / 不认识的名称拒绝 /
#   各包装器在空指针下的安全降级。
#
# ★ 独立可执行文件: 链真实的 mouse_driver.cpp + 依赖库(Makcu / serial / ws2_32),
#   不需要 Qt 或推理运行时。
#
# ★★ 这是【唯一一个在 macOS 上不编译】的逻辑测试(WIN32 守卫)。
#    驱动层没有跨平台替身, 所以本机跑 ctest 时它不在列表里 —— 那是覆盖缺口,
#    不是"它通过了"。
if(WIN32)
add_executable(mouse_driver_test "${CMAKE_CURRENT_LIST_DIR}/mouse_driver_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/mouse_driver.cpp"
    "${APP_SOURCE_ROOT}/mouse/Makcu.cpp"
    "${APP_SOURCE_ROOT}/mouse/MakcuNew.cpp"
    "${APP_SOURCE_ROOT}/mouse/kmboxNetConnection.cpp"
    "${APP_SOURCE_ROOT}/mouse/kmbox_net/kmboxNet.cpp"
    "${APP_SOURCE_ROOT}/modules/makcu/src/makcu.cpp"
    "${APP_SOURCE_ROOT}/modules/makcu/src/serialport.cpp"
    "${APP_SOURCE_ROOT}/modules/serial/src/serial.cc"
    "${APP_SOURCE_ROOT}/modules/serial/src/impl/win.cc"
    "${APP_SOURCE_ROOT}/modules/serial/src/impl/list_ports/list_ports_win.cc")
target_include_directories(mouse_driver_test PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${APP_SOURCE_ROOT}"
    "${APP_SOURCE_ROOT}/config"
    "${APP_SOURCE_ROOT}/include"
    "${APP_SOURCE_ROOT}/mouse"
    "${APP_SOURCE_ROOT}/modules/makcu/include"
    "${APP_SOURCE_ROOT}/modules/serial/include")
target_compile_features(mouse_driver_test PRIVATE cxx_std_20)
target_compile_definitions(mouse_driver_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mouse_driver_test PRIVATE ws2_32 setupapi)
if(MSVC)
    target_compile_options(mouse_driver_test PRIVATE /utf-8 /Zc:char8_t-)
endif()
add_test(NAME mouse_driver_test COMMAND mouse_driver_test)
set_tests_properties(mouse_driver_test PROPERTIES TIMEOUT 10)
endif()
