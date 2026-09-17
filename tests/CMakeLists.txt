find_package(Threads REQUIRED)

# 鐣岄潰鍑犱綍娴嬭瘯闇€瑕?Qt銆傗槄 鐢?QUIET + 鍙€? 閫昏緫娴嬭瘯鐨勫崠鐐瑰氨鏄?浠讳綍骞冲彴閮借兘
# 鍗曠嫭缂?(瑙?CLAUDE.md), 涓嶈兘鍥犱负鎵句笉鍒?Qt 灏辫鏁翠釜 logic-tests 閰嶇疆澶辫触銆?
# 鎵句笉鍒板氨璺宠繃杩欎竴绫绘祴璇? 鍏朵綑鐓у父銆?
find_package(Qt6 QUIET COMPONENTS Widgets)

set(APP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Apotheosis")
function(add_logic_test target source)
    # 妫版繂顦婚惃鍕爱閺傚洣娆㈡担婊€璐?ARGN 鏉╄棄濮?閹貉冨煑閸ｃ劌鐤勯悳鎵搼), 濞村鐦懛顏勭箒閻?main 娴犲秴婀?${source} 闁插被鈧?
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

# 鏂?PID (mouse/aim_pid.h) 鐨勯棴鐜洖褰掋€傚畠鑷繁鎼簡涓€涓€滃亣娓告垙鈥?
# (琚帶瀵硅薄: 鐩爣鏈変笘鐣岄€熷害銆侀暅澶存寜鎴戜滑鐨勮鏁拌浆銆佹祴閲忎笌鎵ц閮芥湁寤惰繜), 瑕嗙洊:
#   路 闈欐鐩爣鐨勬敹鏁涗笌绋虫€佽宸?(60/240/1000fps)
#   路 鍖€閫熺洰鏍囩殑绋虫€佹粸鍚?(璇佹槑鐪熺Н鍒嗘妸寤惰繜閫犳垚鐨勬粸鍚庢秷鎺?
#   路 杈撳嚭闄愬箙銆佸垎鏁颁綑閲忔敀鍒颁笅涓€鎷嶃€丯aN/dt 寮傚父銆侀潪娉曞鐩娿€佹崲鐩爣澶嶄綅
#
# 銆?026-09-13銆戝師鏉ヨ繕閾炬帴 mouse/aim_motion.cpp, 骞跺寘鍚€滅洰鏍囪繍鍔ㄨ娴嬪櫒鍦ㄧ嚎浼?k虃鈥濅笌鈥滃墠棣堝彧褰卞搷鐬€佲€濅袱缁勭敤渚嬨€?
# 鍓嶉鏁存潯鍒犻櫎鍚? anchor_observer.h 涓?aim_motion.cpp 宸蹭笉瀛樺湪, 閭ｄ簺鐢ㄤ緥涓€骞剁Щ闄ゃ€?
# 鐜板湪鎺у埗鍣ㄦ槸绾弽棣?PID, 娴嬭瘯涔熷彧楠岃瘉瀹冦€?
add_logic_test(aim_pid_test "${CMAKE_CURRENT_LIST_DIR}/aim_pid_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# PID-EventSync 妗ｇ殑鐩爣璺熻釜鍣?(mouse/aim_tracker.h, 2026-09-15 绉绘鑷?
# AimMagic 1.0.30 鐨?FUN_140089ac0 / FUN_14006e470 璇箟)銆傚ご鏂囦欢绾у疄鐜般€?
# 瑕嗙洊: 韬唤绮樻粸(鎶栧姩/婕忓抚涓嶈鎹?id, 鐪熸崲鐩爣蹇呴』鎹?銆佺敓鍛藉懆鏈?min_hits 涓嶆尅
# 閿佸畾銆乵ax_age 娣樻卑)銆侀€熷害閲囨牱绐?鏀舵暃 + 璺ㄥ抚鎸佹湁 + 鍋滀綇褰掗浂)銆侀娴嬬郴鏁?FSM
# (0.1 鐖?/ 0.2 钀?/ 澶瑰湪 [0,1])銆佹彁鍓嶉噺鐨勭‖涓婇檺涓庨€熷害鍣０闂ㄣ€佸叧棰勬祴鏃堕€愪綅鎭掔瓑銆?
# 纭畾鎬т笌寮傚父杈撳叆(dt=0/璐熴€侀暱鏈熺┖瑙傛祴銆乺eset)銆?
add_logic_test(aim_tracker_test "${CMAKE_CURRENT_LIST_DIR}/aim_tracker_test.cpp")

# PID-EventSync 鍏ㄩ摼璺殑闂幆鍥炲綊 (璺熻釜鍣?鈫?姣忚建棰勬祴 鈫?鎺у埗鍣?鈫?璁℃暟)銆?
# 鍋囨父鎴忎笌 aim_acceptance_test 鍚屾簮(涓栫晫閫熷害 + 44/46ms 寤惰繜 + 妗嗗績鏁存暟閲忓寲)銆?
# 瑕嗙洊: 鈶犲叧棰勬祴鏃跺繀椤讳笌鐜板焦閾捐矾銆愰€愪綅涓€鑷淬€?鎹㈡。涓嶆敼琛屼负鐨勭‖璇佹嵁);
#       鈶″紑棰勬祴鏃舵彁鍓嶉噺鏈夌晫銆侀棴鐜笉鍙戞暎; 鈶涪甯ф椂鐘舵€佽法甯ф寔鏈?绉垎涓嶈娓呴浂);
#       鈶ｈ韩浠界矘婊?鎶栧姩+婕忓抚涓嬪彧寤轰竴娆¤建); 鈶ゅ湪閫旇ˉ鍋夸笌妗ｄ綅鏃犲叧銆?
add_logic_test(aim_eventsync_test "${CMAKE_CURRENT_LIST_DIR}/aim_eventsync_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# AimMagic PID 内核逐字移植 (mouse/aim_pid_am.h) —— FUN_140056f10 + FUN_1400579f0。
# 头文件级实现, 不需要额外源文件。覆盖: 档位枚举数值(= 反汇编立即数, 不是列表下标)、
# 第 1 帧早退、D 项混合系数【必须是 1-exp(-40pi*dt) 而不是线性式】、D 项形状
# (1-blend)*hist、F 项只在 Adaptive、Adaptive 的 kp_floor 替换、积分夹取在
# Free/Adaptive 档被跳过、三处闸门 +0xFC/+0xFD/+0xFE 的语义差异、setpoint 历史的
# 更新时机。★ 每条"关掉时一致"的断言都配了一条"打开时必须不同"的反向验证。
add_logic_test(aim_pid_am_test "${CMAKE_CURRENT_LIST_DIR}/aim_pid_am_test.cpp")

# 杞ㄨ抗鏁村舰灞?(mouse/aim_path.h) 鈥斺€?澶存枃浠剁骇瀹炵幇, 涓嶉渶瑕侀澶栨簮鏂囦欢銆? 瑕嗙洊鍥涚妯″紡鐨勫叕鍏变笉鍙樺紡(鍙棆杞笉缂╂斁) 涓?WindMouse 鏇茬嚎(浠?AimMagic
# enable_mouse_curve) 鐨勯棬鎺с€佺‘瀹氭€с€佹敹鏁涙€с€?
add_logic_test(aim_path_test "${CMAKE_CURRENT_LIST_DIR}/aim_path_test.cpp")

# 鑷姩寮€闀?(mouse/trigger_scope.h) 鈥斺€?浠?AimMagic 鐨勩€屽紑鐏柟寮忋€嶃€傚悓鏍峰彧鏈夊ご鏂囦欢,
# 瑕嗙洊闀挎寜/鐐规寜涓ょ妯″紡銆佸紑闀滃埌寮€鐏殑绛夊緟闂搁棬, 浠ュ強鈥滃彸閿粷涓嶅崱鍦ㄦ寜涓嬧€濄€?
add_logic_test(trigger_scope_test "${CMAKE_CURRENT_LIST_DIR}/trigger_scope_test.cpp")

# 浜氶噺鍖栨姈鍔ㄧ殑鍥炲綊 (2026-09-13 閲嶅啓鍚庛€愭仮澶嶈繘鏋勫缓銆?銆?
#
# 鑰佺増鏈獙璇佺殑鏄?px_per_count(姣忚鏁板儚绱?鐨勪袱椤逛繚鎶? 鑰?px_per_count 宸茶鍒犻櫎,
# 鎵€浠ュ畠涓€搴﹁绉诲嚭鏋勫缓銆傞噸鍋氬悗鍦ㄩ€旇ˉ鍋挎惉杩?AimPid 鍐呴儴骞舵敼鎴愩€愯鏁板煙銆?涓嶅惈 k虃),
# 浜氶噺鍖栨姈鍔ㄧ殑鎴愬洜鍜屼繚鎶ら兘杩樺湪(鑰屼笖淇濇姢鍒ゆ嵁涔熶粠"鍗婁釜閲忓瓙"鏀规垚浜嗘棤閲忕翰鐨?
# "琛ュ伩椤规妸鎸囦护椤跺弽鍙?), 鎵€浠ヨ繖浠藉洖褰掗噸鏂板彉寰楀彲鎵ц銆佷篃閲嶆柊鍙樺緱蹇呰:
#   路 澶嶇幇"灏炬姣忔媿 卤1 鎺?鐨勬姈鍔?璇佹槑妯″瀷鏈夋晥)
#   路 淇濇姢寮€鍚悗灏炬闈欓粯銆佹畫宸笉閫€鍖?
#   路 鐢╂灙娈甸€愭媿鎸囦护銆愬畬鍏ㄤ竴鑷淬€?淇濇姢鏄簹閲忓寲涓撶敤, 涓嶈鏀规墜鎰?
#   路 beta=0 鏃朵繚鎶ゆ槸褰诲簳鐨勬亽绛夊彉鎹?
add_logic_test(aim_pid_quantum_test "${CMAKE_CURRENT_LIST_DIR}/aim_pid_quantum_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")




# 閼奉亜濮╅幀銉ヤ粻 (mouse/auto_stop.h): 瀵偓閻忣偅妞傜悰銉ょ娑擃亜寮介弬鐟版倻闁款喗濮峰☉鍫⑿╅崝銊ｂ偓鍌氥仈閺傚洣娆㈢痪褍鐤勯悳鑸偓?
# 鐟曞棛娲婇弬鐟版倻閺勭姴鐨犻妴浣虹叚閹稿鐛ラ崣锝呭敶娑撳秹鍣搁崣鎴欌偓浣蜂簰閸?閹存垳婊戝▔銊ュ弳閻ㄥ嫰鏁稉宥堝厴鐞氼偄缍嬮幋鎰负鐎硅埖瀵滈惃鍕暛"閵?
add_logic_test(auto_stop_test "${CMAKE_CURRENT_LIST_DIR}/auto_stop_test.cpp")

# 鍦ㄩ€旇ˉ鍋?(mouse/aim_pid.h 鐨?inflight_beta, 2026-09-13)銆?
# 鐢ㄦ埛瀹炴祴: "Kp 鎷夐珮浼氬湪鐩爣韬笂鏉ュ洖鏅冨姩, 鎷変綆鎷夋灙鍙堝緢鎱? 鈥斺€?杩欐槸銆愰摼璺欢杩熴€?
# 鍐冲畾鐨勫鐩婂ぉ鑺辨澘, 涓嶆槸璋冨弬鑳借В鍐崇殑銆傛湰娴嬭瘯瑕嗙洊:
#   路 鍦ㄩ€旇璐︾殑姝ｇ‘鎬?(|in_flight| == 绐楀唴鎷嶆暟 x 姣忔媿杈撳嚭, 涓旇緭鍑哄仠姝㈠悗浼氭竻绌?
#   路 beta=0 鏃跺繀椤讳笌涓嶅仛琛ュ伩鐨勭函鍙嶉銆愰€愪綅涓€鑷淬€?鍚戝悗鍏煎鐨勭‖瑕佹眰)
#   路 beta>0 鑳芥妸涓嶇ǔ瀹氱殑澶?Kp 鎷夊洖绋冲畾, 涓旇繃澶ф椂浼氶噸鏂板彉宸?鑷檺鎬?
#   路 NaN/dt<=0/闈炴硶鍙傛暟/kp=0 涓嶄細浜х敓 NaN 鎴栫垎杈撳嚭, reset() 娓呭共鍑€
#   路 绉诲姩鐩爣涓嬬殑婊炲悗涓庢姈鍔?鐢ㄦ埛鐨勬牳蹇冭瘔姹? 鐩爣鎬庝箞鍔ㄩ兘杩藉緱姝?
add_logic_test(inflight_comp_test "${CMAKE_CURRENT_LIST_DIR}/inflight_comp_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# dt 浣庨€?(mouse_thread_loop 鐨?kDtSmoothTau = 25ms, 2026-09-13)銆?
# 鑳屾櫙: 鎺у埗鐜師鏉ユ妸銆愯８澧欓挓闂撮殧銆戠洿鎺ュ杺缁?PID, 瀹炴祴鏃ュ織閲?p10=7.45ms / p50=8.55ms
# / p90=11.64ms / max=23.22ms銆侾ID 鎸?dt 缂╂斁杈撳嚭, 鎵€浠ヨ繖鏄瓑鏁堝鐩婂湪鎶?鈥斺€?
# 浠跨湡瀵规瘮: Kp=60 鏃跺埌浣?0.83s -> 2.83s銆佽窡韪粸鍚?52 -> 92px, 涓?Kp 璋冨灏戞棤鍏炽€?
# 鏈祴璇曢拤浣忎袱鍗? 鎶栧姩蹇呴』琚帇鎺? 鑰岀湡瀹炵殑鎸佺画鎺夊抚蹇呴』杩樿兘璺熶笂銆?
add_logic_test(dt_smooth_test "${CMAKE_CURRENT_LIST_DIR}/dt_smooth_test.cpp")

# 璺濈灏哄害浼拌 (mouse/aim_scale.h, 2026-09-13, G6銆岃繎澶ц繙灏忋€?銆?
# 瀹冩妸 bbox.height 鏄犲皠鎴愪竴涓€愯繛缁€佹湁鐣屻€佸崟璋冦€戠殑涔樻暟 s 鈭?[s_min, s_max],
# 渚涙帶鍒跺櫒璋冨埗: 绛夋晥澧炵泭 / 鎻愬墠閲忎笂闄?/ 閫熷害鍙俊闂ㄩ檺 / 骞虫粦寮哄害銆?
# 鏈祴璇曞彧閽夎繖涓ā鍧楄嚜韬殑褰㈢姸(杩炵画/鏈夌晫/鍗曡皟/鍖洪棿澶栧す浣?涓嶆妸娣卞害绠椾袱閬?
# 妗嗛珮骞虫粦/鍧忚緭鍏?, 瀹冪殑鏀剁泭鐢遍棴鐜洖褰?aim_pid_test 鐨?G1~G6 鐢ㄤ緥璇佹槑銆?
add_logic_test(aim_scale_test "${CMAKE_CURRENT_LIST_DIR}/aim_scale_test.cpp")

# G1~G7 楠屾敹鍥炲綊 (2026-09-13 閲嶅啓)銆傚畠鎶婁换鍔＄殑楠屾敹鏍囧噯閫愭潯閲忓寲:
#   G1 闈欐鎷夋灙 t90 / G2 杩囧啿涓庡熬娈垫畫宸?/ G3 鏈哄姩鍚庨噸鏂拌创鍚?/ G4 鏃犳憜鎸?/
#   G5 绋虫€佽窡闅忚宸笌鏃犳娊鍔?/ G6 杩戜腑杩滃昂搴﹀鐓?/ G7 甯х巼鏃犲叧+澶嶄綅+閫愪綅鐩稿悓銆?
# 琚帶瀵硅薄娌跨敤 aim_pid_test 鐨?鍋囨父鎴?, 浣嗘墽琛屽欢杩熺敤瀹炴祴姝诲尯 46ms, 骞跺姞妫€娴嬫閲忓寲銆?
add_logic_test(aim_acceptance_test "${CMAKE_CURRENT_LIST_DIR}/aim_acceptance_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# 鈹€鈹€ 璋冨弬 agent (2026-09-14) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
# 娴嬬殑鏄€愬彲鍒ゅ畾鐨勬€ц川銆? 鏃ュ織瑙ｆ瀽銆佹寚鏍囧畾涔?杩囧啿/缈昏浆鐜?鍙戞暎)銆佸畨鍏ㄥす鍙?
# (鍗曟骞呭害/beta 涓嶅绉?NaN)銆佸洖婊氬垽瀹氥€丣SON 鎻愬彇銆?
# 鈽?鍒绘剰涓嶈仈缃戙€佷笉娴?LLM 杈撳嚭鍐呭 鈥斺€?閭ｄ笉鍙噸澶嶃€傛祴鐨勬槸
#   "鏃犺妯″瀷璇翠粈涔? 鎴戜滑鐨勫鐞嗛兘鏄鐨?銆?
# 闇€瑕?autotune_llm.cpp (缃戠粶鍑哄彛) 涓?autotune_agent.cpp (涓诲惊鐜?, 浜岃€呴兘涓?
# 渚濊禆 Qt / CUDA / 閲囬泦鍗°€?
#
# 鈽?2026-09-14 杩藉姞 autotune_runtime.cpp銆傚師鍥犳槸韪╀簡涓€涓潙:
#   `Runtime::begin_session()` 閲屾湁涓€琛?`last_seq_ = 0;`, 鑰?
#   `feed_from_chainlog()` 鐢?0 褰?灏氭湭鍒濆鍖?鐨勫摠鍏?鈥斺€?浜庢槸鐐广€屽紑濮嬨€嶄箣鍚?
#   鍐嶄篃鏀朵笉鍒版牱鏈?鐢ㄦ埛鎶ョ殑"杩欐鏁版嵁鍙湁 0 鏉?)銆?
#   **閭ｄ釜 bug 鍙湪銆愪袱灞備氦鐣屽銆戞墠鏄惧舰**: AutoTuner 鑷繁閭ｅ眰瀹屽叏姝ｇ‘,
#   鍙祴 agent 鏄祴涓嶅埌鐨? 蹇呴』鎶婄湡瀹?Runtime + 鐪熷疄 chainlog 鎺ヨ捣鏉ヨ窇銆?
#   runtime 灞備笉渚濊禆 Qt(config 鐨勮鍐欐槸閫氳繃 RuntimeHooks 娉ㄥ叆鐨勫嚱鏁版寚閽?,
#   鎵€浠ラ€昏緫娴嬭瘯閲屽彲浠ョ洿鎺ョ敤鍋?hooks 椹卞姩瀹冦€?
add_logic_test(autotune_test "${CMAKE_CURRENT_LIST_DIR}/autotune_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_llm.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_agent.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_runtime.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# 鈹€鈹€ 澶栭儴鏀归厤缃悗鐣岄潰蹇呴』閲嶈 (2026-09-15) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
# 鐢ㄦ埛鎶ャ€屼竴鐩存病鑳界湡姝ｇ敓鏁?!銆嶃€傚疄鏈烘棩蹇楅噷璋冨弬鏄庢槑銆愬悓鏃躲€戝啓 x/y, 鍗村嚭鐜?
#   x=(kp=60.0) y=(kp=15.0)
# 杩欑涓嶅绉?鈥斺€?鍥犱负 HotkeyPage 鍦?configLoaded 涓婂彧閲嶅缓浜嗕笅鎷? 鍏釜鐬勫噯鍙傛暟
# 鐨勬帶浠舵病閲嶈; 涔嬪悗鐢ㄦ埛鏀逛换浣曞埆鐨勬帶浠? "鏁寸墖鍐欏洖"灏辨妸闄堟棫鍊肩亴浜嗗洖鍘汇€?
# 鈽?杩欑被 bug 鏄潤榛樼殑(涓嶆姤閿欍€佷笉鐣欑棔), 鍙兘闈犲洖褰掗拤浣忋€?
add_logic_test(autotune_reload_test "${CMAKE_CURRENT_LIST_DIR}/autotune_reload_test.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_llm.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_agent.cpp"
    "${APP_SOURCE_ROOT}/mouse/autotune_runtime.cpp"
    "${APP_SOURCE_ROOT}/mouse/aim_pid.cpp")

# 鈹€鈹€ 閰嶇疆妲戒綅杩佺Щ (2026-09-14) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
# `pidf_mapping_version` 鏄湰椤圭洰銆愬敮涓€銆戠殑閰嶇疆杩佺Щ鏈哄埗, 鑰岃縼绉诲啓閿欑殑鍚庢灉鏄?
# 銆愰潤榛樼殑銆? 鐢ㄦ埛鐨勮€侀厤缃細琚寜鏂拌涔夎璇? 鎵嬫劅绐佺劧鍙樹簡鍗存煡涓嶅嚭鍘熷洜銆?
# 鎵€浠ユ瘡涓€妗ｈ縼绉婚兘瑕佹湁鍙墽琛岀殑鍥炲綊 鈥斺€?杩欓噷楠岃瘉 v6 -> v7 (灏哄害璋冨害鐨勬Ы浣嶈涔?
# 鍙樻洿: 涓や釜缁濆闃堝€?-> 涓€涓嚜鍔ㄥ鐨勫熀鍑?銆?
# 闇€瑕?config.cpp (瑙ｆ瀽/澶瑰彇/杩佺Щ) 涓?SimpleIni銆?
# 鈽?NOMINMAX 鏄繀椤荤殑: config.cpp 涓?SimpleIni.h 閮戒細鐢ㄥ埌 std::min/max, 鑰?
#   windows.h 鐨?min/max 瀹忎細鎶婂畠浠挄鎴愯娉曢敊璇€備富绋嬪簭鏄€氳繃 Apotheosis.h 閲?
#   鐨勫畾涔夋嬁鍒扮殑, 閫昏緫娴嬭瘯娌℃湁閭ｄ竴灞? 鎵€浠ヨ鏄惧紡鍔犮€?
if(EXISTS "${APP_SOURCE_ROOT}/modules/SimpleIni.h")
add_logic_test(config_migration_test "${CMAKE_CURRENT_LIST_DIR}/config_migration_test.cpp"
    "${APP_SOURCE_ROOT}/config/config.cpp")
target_compile_definitions(config_migration_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# 鈹€鈹€ 鐣岄潰鍑犱綍 (2026-09-14) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
# 鈽?涓轰粈涔堥渶瑕佽繖涓€绫绘祴璇? 鍒楀/瑁佸垏闂鏄€愮函瑙嗚銆戠殑, 閫昏緫娴嬭瘯瀹屽叏娴嬩笉鍒般€?
#   瀹炴祴韪╄繃涓€娆? 鍘嗗彶琛ㄦ牸鐢?setStretchLastSection(true), 浠ｇ爜璇昏捣鏉ユ槸"璁╂渶鍚?
#   涓€鍒楀崰婊″墿浣欏搴?, 瀹為檯鏁堟灉鏄€愬墠 6 鍒楄鍘嬪埌鍒氭斁寰椾笅琛ㄥご瀛椼€佹暟瀛楁尋鎴愪竴鍥€戙€?
#   鍙湁鐪熺殑鎶婃帶浠跺缓鍑烘潵銆佸竷灞€涓€閬嶃€佸啀璇诲儚绱犲搴︽墠鑳藉彂鐜般€?
#
# 鐢?Qt 鐨?offscreen 骞冲彴鎻掍欢: 涓嶅脊绐椼€佷笉鎶㈢劍鐐? 鍙互鍦ㄨ嚜鍔ㄥ寲閲岃窇銆?
# 鍥犳杩欎釜鐩爣銆愬繀椤婚摼鎺?Qt6::Widgets銆? 涓嶈兘璧?add_logic_test銆?
if(TARGET Qt6::Widgets)
    # 鈽?鍒绘剰銆愪笉銆戦摼 autotune_hooks.cpp / config.cpp: 瀹冧滑浼氫竴璺嫋杩?
    #   Apotheosis.h -> i_detector.h -> OpenCV, 鑰屾湰娴嬭瘯鍙叧蹇冨嚑浣曘€?
    #   鍞竴缂虹殑绗﹀彿 uninstall_production_hooks 鍦ㄦ祴璇曟簮閲岀粰浜嗙┖澹炽€?
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
    # 鈽?娴嬭瘯 exe 鏃佽竟娌℃湁 Qt 鐨?DLL, 蹇呴』鎶?Qt 鐨?bin 鍔犺繘 PATH, 鍚﹀垯杩涚▼
    #   鐩存帴浠?0xC0000135 (DLL not found) 閫€鍑恒€佷竴琛屼笟鍔¤緭鍑洪兘娌℃湁 鈥斺€?閭ｇ
    #   澶辫触鐪嬭捣鏉ュ儚"娴嬭瘯宕╀簡", 瀹為檯鍙槸鎵句笉鍒?Qt6Widgets.dll銆?
    # 鈽?ENVIRONMENT 鏄竴涓垎鍙峰垎闅旂殑銆愬垪琛ㄣ€? 鍚屼竴涓睘鎬у彧鑳借涓€娆? 璁句袱娆?
    #   鍚庤€呬細鏁翠綋瑕嗙洊鍓嶈€?韪╄繃)銆傛墍浠ヤ袱涓彉閲忓啓鍦ㄤ竴涓?set_tests_properties 閲屻€?
    get_filename_component(QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    set_tests_properties(autotune_layout_test PROPERTIES
        ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH};QT_QPA_PLATFORM=offscreen")
endif()

# 鈹€鈹€ 纭欢椹卞姩鎶借薄鍗曞厓娴嬭瘯 (2026-09-15 鏂板) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
#
# 楠岃瘉 AimMagic 椹卞姩宸ュ巶褰㈢姸鐨勬娊璞″眰濂戠害:
#   涓夊鍚庣鍚嶇О / 鑳藉姏浣嶄腑鏂囨覆鏌?/ 鐘舵€佷覆甯︾悊鐢?/ 涓嶈璇嗙殑鍚嶅瓧鎷掔粷 /
#   鍚勫寘瑁呭櫒鍦ㄧ┖鎸囬拡涓嬬殑瀹夊叏闄嶇骇銆?
#
# 鈽?鐙珛鍙墽琛屾枃浠? 閾剧湡瀹炵殑 mouse_driver.cpp + 渚濊禆搴?Makcu / serial / ws2_32),
#   涓嶉渶瑕?Qt 鎴栨帹鐞嗚繍琛屾椂銆?
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
