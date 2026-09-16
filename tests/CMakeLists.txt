find_package(Threads REQUIRED)

# 界面几何测试需要 Qt。★ 用 QUIET + 可选: 逻辑测试的卖点就是"任何平台都能
# 单独编"(见 CLAUDE.md), 不能因为找不到 Qt 就让整个 logic-tests 配置失败。
# 找不到就跳过这一类测试, 其余照常。
find_package(Qt6 QUIET COMPONENTS Widgets)

set(APP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Apotheosis")
function(add_logic_test target source)
    # 棰濆鐨勬簮鏂囦欢浣滀负 ARGN 杩藉姞(鎺у埗鍣ㄥ疄鐜扮瓑), 娴嬭瘯鑷繁鐨?main 浠嶅湪 ${source} 閲屻€?
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

# 新 PID (mouse/aim_pid.h) 的闭环回归。它自己搭了一个“假游戏”
# (被控对象: 目标有世界速度、镜头按我们的计数转、测量与执行都有延迟), 覆盖:
#   · 静止目标的收敛与稳态误差 (60/240/1000fps)
#   · 匀速目标的稳态滞后 (证明真积分把延迟造成的滞后消掉)
#   · 输出限幅、分数余量攒到下一拍、NaN/dt 异常、非法增益、换目标复位
#
# 【2026-09-13】原来还链接 mouse/aim_motion.cpp, 并包含“目标运动观测器在线估 k̂”与“前馈只影响瞬态”两组用例。
# 前馈整条删除后, anchor_observer.h 与 aim_motion.cpp 已不存在, 那些用例一并移除。
# 现在控制器是纯反馈 PID, 测试也只验证它。
add_logic_test(aim_pid_test "${CMAKE_CURRENT_LIST_DIR}/aim_pid_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# PID-EventSync 档的目标跟踪器 (mouse/aim_tracker.h, 2026-09-15 移植自
# AimMagic 1.0.30 的 FUN_140089ac0 / FUN_14006e470 语义)。头文件级实现。
# 覆盖: 身份粘滞(抖动/漏帧不许换 id, 真换目标必须换)、生命周期(min_hits 不挡
# 锁定、max_age 淘汰)、速度采样窗(收敛 + 跨帧持有 + 停住归零)、预测系数 FSM
# (0.1 爬 / 0.2 落 / 夹在 [0,1])、提前量的硬上限与速度噪声门、关预测时逐位恒等、
# 确定性与异常输入(dt=0/负、长期空观测、reset)。
add_logic_test(aim_tracker_test "${CMAKE_CURRENT_LIST_DIR}/aim_tracker_test.cpp")

# PID-EventSync 全链路的闭环回归 (跟踪器 → 每轨预测 → 控制器 → 计数)。
# 假游戏与 aim_acceptance_test 同源(世界速度 + 44/46ms 延迟 + 框心整数量化)。
# 覆盖: ①关预测时必须与现役链路【逐位一致】(换档不改行为的硬证据);
#       ②开预测时提前量有界、闭环不发散; ③丢帧时状态跨帧持有(积分不被清零);
#       ④身份粘滞(抖动+漏帧下只建一次轨); ⑤在途补偿与档位无关。
add_logic_test(aim_eventsync_test "${CMAKE_CURRENT_LIST_DIR}/aim_eventsync_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# 轨迹整形层 (mouse/aim_path.h) —— 头文件级实现, 不需要额外源文件。# 覆盖四种模式的公共不变式(只旋转不缩放) 与 WindMouse 曲线(从 AimMagic
# enable_mouse_curve) 的门控、确定性、收敛性。
add_logic_test(aim_path_test "${CMAKE_CURRENT_LIST_DIR}/aim_path_test.cpp")

# 自动开镜 (mouse/trigger_scope.h) —— 从 AimMagic 的「开火方式」。同样只有头文件,
# 覆盖长按/点按两种模式、开镜到开火的等待闸门, 以及“右键绝不卡在按下”。
add_logic_test(trigger_scope_test "${CMAKE_CURRENT_LIST_DIR}/trigger_scope_test.cpp")

# 亚量化抖动的回归 (2026-09-13 重写后【恢复进构建】)。
#
# 老版本验证的是 px_per_count(每计数像素)的两项保护, 而 px_per_count 已被删除,
# 所以它一度被移出构建。重做后在途补偿搬进 AimPid 内部并改成【计数域】(不含 k̂),
# 亚量化抖动的成因和保护都还在(而且保护判据也从"半个量子"改成了无量纲的
# "补偿项把指令顶反号"), 所以这份回归重新变得可执行、也重新变得必要:
#   · 复现"尾段每拍 ±1 推"的抖动(证明模型有效)
#   · 保护开启后尾段静默、残差不退化
#   · 甩枪段逐拍指令【完全一致】(保护是亚量化专用, 不许改手感)
#   · beta=0 时保护是彻底的恒等变换
add_logic_test(aim_pid_quantum_test "${CMAKE_CURRENT_LIST_DIR}/aim_pid_quantum_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")




# 鑷姩鎬ュ仠 (mouse/auto_stop.h): 寮€鐏椂琛ヤ竴涓弽鏂瑰悜閿姷娑堢Щ鍔ㄣ€傚ご鏂囦欢绾у疄鐜般€?
# 瑕嗙洊鏂瑰悜鏄犲皠銆佺煭鎸夌獥鍙ｅ唴涓嶉噸鍙戙€佷互鍙?鎴戜滑娉ㄥ叆鐨勯敭涓嶈兘琚綋鎴愮帺瀹舵寜鐨勯敭"銆?
add_logic_test(auto_stop_test "${CMAKE_CURRENT_LIST_DIR}/auto_stop_test.cpp")

# 在途补偿 (mouse/aim_pid.h 的 inflight_beta, 2026-09-13)。
# 用户实测: "Kp 拉高会在目标身上来回晃动, 拉低拉枪又很慢" —— 这是【链路延迟】
# 决定的增益天花板, 不是调参能解决的。本测试覆盖:
#   · 在途记账的正确性 (|in_flight| == 窗内拍数 x 每拍输出, 且输出停止后会清空)
#   · beta=0 时必须与不做补偿的纯反馈【逐位一致】(向后兼容的硬要求)
#   · beta>0 能把不稳定的大 Kp 拉回稳定, 且过大时会重新变差(自限性)
#   · NaN/dt<=0/非法参数/kp=0 不会产生 NaN 或爆输出, reset() 清干净
#   · 移动目标下的滞后与抖动(用户的核心诉求: 目标怎么动都追得死)
add_logic_test(inflight_comp_test "${CMAKE_CURRENT_LIST_DIR}/inflight_comp_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# dt 低通 (mouse_thread_loop 的 kDtSmoothTau = 25ms, 2026-09-13)。
# 背景: 控制环原来把【裸墙钟间隔】直接喂给 PID, 实测日志里 p10=7.45ms / p50=8.55ms
# / p90=11.64ms / max=23.22ms。PID 按 dt 缩放输出, 所以这是等效增益在抖 ——
# 仿真对比: Kp=60 时到位 0.83s -> 2.83s、跟踪滞后 52 -> 92px, 与 Kp 调多少无关。
# 本测试钉住两半: 抖动必须被压掉, 而真实的持续掉帧必须还能跟上。
add_logic_test(dt_smooth_test "${CMAKE_CURRENT_LIST_DIR}/dt_smooth_test.cpp")

# 距离尺度估计 (mouse/aim_scale.h, 2026-09-13, G6「近大远小」)。
# 它把 bbox.height 映射成一个【连续、有界、单调】的乘数 s ∈ [s_min, s_max],
# 供控制器调制: 等效增益 / 提前量上限 / 速度可信门限 / 平滑强度。
# 本测试只钉这个模块自身的形状(连续/有界/单调/区间外夹住/不把深度算两遍/
# 框高平滑/坏输入), 它的收益由闭环回归 aim_pid_test 的 G1~G6 用例证明。
add_logic_test(aim_scale_test "${CMAKE_CURRENT_LIST_DIR}/aim_scale_test.cpp")

# G1~G7 验收回归 (2026-09-13 重写)。它把任务的验收标准逐条量化:
#   G1 静止拉枪 t90 / G2 过冲与尾段残差 / G3 机动后重新贴合 / G4 无摆振 /
#   G5 稳态跟随误差与无抽动 / G6 近中远尺度对照 / G7 帧率无关+复位+逐位相同。
# 被控对象沿用 aim_pid_test 的"假游戏", 但执行延迟用实测死区 46ms, 并加检测框量化。
add_logic_test(aim_acceptance_test "${CMAKE_CURRENT_LIST_DIR}/aim_acceptance_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# ── 调参 agent (2026-09-14) ─────────────────────────────────────────────────
# 测的是【可判定的性质】: 日志解析、指标定义(过冲/翻转率/发散)、安全夹取
# (单步幅度/beta 不对称/NaN)、回滚判定、JSON 提取。
# ★ 刻意不联网、不测 LLM 输出内容 —— 那不可重复。测的是
#   "无论模型说什么, 我们的处理都是对的"。
# 需要 autotune_llm.cpp (网络出口) 与 autotune_agent.cpp (主循环), 二者都不
# 依赖 Qt / CUDA / 采集卡。
#
# ★ 2026-09-14 追加 autotune_runtime.cpp。原因是踩了一个坑:
#   `Runtime::begin_session()` 里有一行 `last_seq_ = 0;`, 而
#   `feed_from_chainlog()` 用 0 当"尚未初始化"的哨兵 —— 于是点「开始」之后
#   再也收不到样本(用户报的"这段数据只有 0 条")。
#   **那个 bug 只在【两层交界处】才显形**: AutoTuner 自己那层完全正确,
#   只测 agent 是测不到的, 必须把真实 Runtime + 真实 chainlog 接起来跑。
#   runtime 层不依赖 Qt(config 的读写是通过 RuntimeHooks 注入的函数指针),
#   所以逻辑测试里可以直接用假 hooks 驱动它。
add_logic_test(autotune_test "${CMAKE_CURRENT_LIST_DIR}/autotune_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_llm.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_agent.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_runtime.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# ── 外部改配置后界面必须重读 (2026-09-15) ───────────────────────────────────
# 用户报「一直没能真正生效!!」。实机日志里调参明明【同时】写 x/y, 却出现
#   x=(kp=60.0) y=(kp=15.0)
# 这种不对称 —— 因为 HotkeyPage 在 configLoaded 上只重建了下拉, 六个瞄准参数
# 的控件没重读; 之后用户改任何别的控件, "整片写回"就把陈旧值灌了回去。
# ★ 这类 bug 是静默的(不报错、不留痕), 只能靠回归钉住。
add_logic_test(autotune_reload_test "${CMAKE_CURRENT_LIST_DIR}/autotune_reload_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_llm.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_agent.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_runtime.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# ── 配置槽位迁移 (2026-09-14) ───────────────────────────────────────────────
# `pidf_mapping_version` 是本项目【唯一】的配置迁移机制, 而迁移写错的后果是
# 【静默的】: 用户的老配置会被按新语义误读, 手感突然变了却查不出原因。
# 所以每一档迁移都要有可执行的回归 —— 这里验证 v6 -> v7 (尺度调度的槽位语义
# 变更: 两个绝对阈值 -> 一个自动学的基准)。
# 需要 config.cpp (解析/夹取/迁移) 与 SimpleIni。
# ★ NOMINMAX 是必须的: config.cpp 与 SimpleIni.h 都会用到 std::min/max, 而
#   windows.h 的 min/max 宏会把它们撕成语法错误。主程序是通过 Apotheosis.h 里
#   的定义拿到的, 逻辑测试没有那一层, 所以要显式加。
if(EXISTS "${APP_SOURCE_ROOT}/modules/SimpleIni.h")
add_logic_test(config_migration_test "${CMAKE_CURRENT_LIST_DIR}/config_migration_test.cpp"
    "${APP_SOURCE_ROOT}/config/config.cpp")
target_compile_definitions(config_migration_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── 界面几何 (2026-09-14) ───────────────────────────────────────────────────
# ★ 为什么需要这一类测试: 列宽/裁切问题是【纯视觉】的, 逻辑测试完全测不到。
#   实测踩过一次: 历史表格用 setStretchLastSection(true), 代码读起来是"让最后
#   一列占满剩余宽度", 实际效果是【前 6 列被压到刚放得下表头字、数字挤成一团】。
#   只有真的把控件建出来、布局一遍、再读像素宽度才能发现。
#
# 用 Qt 的 offscreen 平台插件: 不弹窗、不抢焦点, 可以在自动化里跑。
# 因此这个目标【必须链接 Qt6::Widgets】, 不能走 add_logic_test。
if(TARGET Qt6::Widgets)
    # ★ 刻意【不】链 autotune_hooks.cpp / config.cpp: 它们会一路拖进
    #   Apotheosis.h -> i_detector.h -> OpenCV, 而本测试只关心几何。
    #   唯一缺的符号 uninstall_production_hooks 在测试源里给了空壳。
    add_executable(autotune_layout_test "${CMAKE_CURRENT_LIST_DIR}/autotune_layout_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../qt_ui/pages/AutoTunePage.cpp"
        "${APP_SOURCE_ROOT}/mouse/autotune_llm.cpp"
        "${APP_SOURCE_ROOT}/mouse/autotune_agent.cpp"
        "${APP_SOURCE_ROOT}/mouse/autotune_runtime.cpp"
        "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")
    target_include_directories(autotune_layout_test PRIVATE
        "${APP_SOURCE_ROOT}" "${CMAKE_CURRENT_LIST_DIR}/../qt_ui"
        "${APP_SOURCE_ROOT}/mouse")
    target_compile_features(autotune_layout_test PRIVATE cxx_std_20)
    target_compile_definitions(autotune_layout_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(autotune_layout_test PRIVATE Qt6::Widgets Threads::Threads)
    if(WIN32)
        target_link_libraries(autotune_layout_test PRIVATE winhttp)
    endif()
    set_target_properties(autotune_layout_test PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(autotune_layout_test PRIVATE /utf-8 /Zc:char8_t-)
    else()
        target_compile_options(autotune_layout_test PRIVATE -fno-char8_t)
    endif()
    add_test(NAME autotune_layout_test COMMAND autotune_layout_test)
    set_tests_properties(autotune_layout_test PROPERTIES TIMEOUT 60)
    # ★ 测试 exe 旁边没有 Qt 的 DLL, 必须把 Qt 的 bin 加进 PATH, 否则进程
    #   直接以 0xC0000135 (DLL not found) 退出、一行业务输出都没有 —— 那种
    #   失败看起来像"测试崩了", 实际只是找不到 Qt6Widgets.dll。
    # ★ ENVIRONMENT 是一个分号分隔的【列表】, 同一个属性只能设一次, 设两次
    #   后者会整体覆盖前者(踩过)。所以两个变量写在一个 set_tests_properties 里。
    get_filename_component(QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    set_tests_properties(autotune_layout_test PROPERTIES
        ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH};QT_QPA_PLATFORM=offscreen")
endif()

# ── 硬件驱动抽象单元测试 (2026-09-15 新增) ──────────────────────────────────
#
# 验证 AimMagic 驱动工厂形状的抽象层契约:
#   三家后端名称 / 能力位中文渲染 / 状态串带理由 / 不认识的名字拒绝 /
#   各包装器在空指针下的安全降级。
#
# ★ 独立可执行文件, 链真实的 mouse_driver.cpp + 依赖库(Makcu / serial / ws2_32),
#   不需要 Qt 或推理运行时。
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
