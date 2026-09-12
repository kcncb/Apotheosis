find_package(Threads REQUIRED)
set(APP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Apotheosis")
function(add_logic_test target source)
    # 额外的源文件作为 ARGN 追加(控制器实现等), 测试自己的 main 仍在 ${source} 里。
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

add_logic_test(latency_probe_test "${APP_SOURCE_ROOT}/runtime/tests/latency_probe_test.cpp")
add_logic_test(capture_card_caps_test "${APP_SOURCE_ROOT}/capture/tests/capture_card_caps_test.cpp")
add_logic_test(device_frame_age_test "${CMAKE_CURRENT_LIST_DIR}/device_frame_age_test.cpp")
add_logic_test(interruptible_slot_test "${CMAKE_CURRENT_LIST_DIR}/interruptible_slot_test.cpp")
add_logic_test(gpu_ready_event_test "${CMAKE_CURRENT_LIST_DIR}/gpu_ready_event_test.cpp")
# This test verifies event ownership with an instrumented CUDA API, not GPU work.
target_include_directories(gpu_ready_event_test BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stubs")
add_logic_test(raw_frame_layout_test "${CMAKE_CURRENT_LIST_DIR}/raw_frame_layout_test.cpp")
add_logic_test(latest_move_slot_test "${CMAKE_CURRENT_LIST_DIR}/latest_move_slot_test.cpp")

# 新 PID (mouse/aim_pid.h + mouse/aim_motion.h) 的闭环回归。它自己搭了一个"假游戏"
# 当被控对象(目标有世界速度、镜头按我们的计数转、测量与执行都有延迟), 覆盖:
#   · 静止目标的收敛与稳态误差(60/240/1000fps)
#   · 匀速目标的稳态滞后(证明真积分把延迟造成的滞后消掉), 以及主动提前量 v*lead
#   · 目标运动观测器在线估 k̂(每计数多少像素)与目标速度, 并且会拒绝无激励/预测框
#   · 延迟预测只影响瞬态、不产生稳态偏移
#   · 输出限幅、分数余量攒到下一拍、NaN/dt 异常、非法增益、换目标复位
add_logic_test(aim_pid_test "${CMAKE_CURRENT_LIST_DIR}/aim_pid_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_motion.cpp")
