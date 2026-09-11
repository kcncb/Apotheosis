#pragma once

// =============================================================================
// PIDF 共用类型（只此一套）
// =============================================================================
//
// 背景: 移植期这里同时存在三套并行的 PIDF 实现 ——
//   · mode1（现役, pidf_mode1_exact.*）        ← 唯一在跑的
//   · mode2（pidf_mode2_exact.*）              ← 每次重建流水线都会被构造, 但
//                                                pidf_mode 被写死为 mode1, 从不 step
//   · update/postprocess（pidf_update_exact.* / pidf_postprocess_exact.*）
//                                              ← 零调用者
// 三者只是各自有自己的状态结构, 却共用同一批输入/输出/增益类型, 于是类型被定义在
// 某个具体实现里、被别的实现反向依赖 —— 删谁都删不动。
//
// 本文件把那批共用类型抽出来单独放, 使"只保留 mode1"成为可能。偏移注释保留,
// 因为它们记录的是原生二进制的 ABI。
// =============================================================================

#include <array>
#include <cstdint>

namespace cvm::recovered {

// 送进 PIDF 的每帧输入（原生 sub_140047060 / sub_140048770 的公共入参）。
struct PidfInputExact {
    std::uint8_t valid{};          // +0
    std::uint8_t _pad01[7]{};
    double target_x{};             // +8
    double target_y{};             // +16
    double radius_x{};             // +24
    double radius_y{};             // +32
    std::int32_t frame_divisor{};  // +40
    std::uint8_t _pad2c[4]{};
    double current_x{};            // +48
    double current_y{};            // +56
};

// 96 字节的增益/死区/限幅配置（原生 sub_1400482D0 逐字拷贝的构造入参）。
// 原先叫 PidfMode2Config, 因为 mode1 复用它却被定义在 mode2 的头里 —— 名字与
// 实际归属不符, 现按实际用途改名。
struct PidfConfig {
    double kp_x{};                         // +0
    double kp_y{};                         // +8
    double ki_x{};                         // +16
    double ki_y{};                         // +24
    double kd_x{};                         // +32
    double kd_y{};                         // +40
    double kf_x{};                         // +48
    double kf_y{};                         // +56
    double lr_x{};                         // +64
    double lr_y{};                         // +72
    std::int32_t deadzone_x{};             // +80
    std::int32_t deadzone_y{};             // +84
    std::int32_t movement_limit_x{};        // +88
    std::int32_t movement_limit_y{};        // +92
};

// PIDF 每帧输出（48 字节）。
struct PidfNativeOutput {
    std::uint8_t initialized_this_frame{}; // +0
    std::uint8_t has_move{};               // +1
    std::uint8_t _pad02[2]{};
    std::int32_t dx{};                     // +4
    std::int32_t dy{};                     // +8
    std::uint8_t _pad0c[4]{};
    double original_error_x{};             // +16
    double original_error_y{};             // +24
    double corrected_error_x{};            // +32
    double corrected_error_y{};            // +40
};


// -----------------------------------------------------------------------------
// 链路延迟补偿(Smith 预测器)的持久状态。
//
// 为什么需要单独的模型: 原生 PidfMode1State 是 704 字节的复刻 ABI(有 static_assert
// 与 offsetof 断言), 不能加字段。而这个模型需要"无延迟的内部状态 + 历史", 所以由
// 流水线持有、按引用传进 update_pidf_mode1。
//
// 它解决什么: 链路从"画面被采集"到"控制环拿到它"有 2-3 帧延迟, 控制环却把它当成
// 当前误差用, 于是比例项在延迟下振荡。多场景模拟(aim_scenario_sim, kp=2, kd=0.05,
// kf=1, lr=0.05)实测的综合分:
//     链路延迟      2帧     3帧     4帧
//     不补偿      15.45   24.36   67.45
//     补偿        15.51   17.65   22.95
// 即补偿把"回路对延迟的敏感性"基本消掉 —— 这正是"准星焊得住"的前提。
//
// 注意: 补偿的价值依赖回路本身足够"硬"(Kp). Kp=1.0 时补偿反而略差
// (18.96->22.02), 所以本机制与「瞄准速度」是配套的, 默认 Kp 已随之调到 2.0。
//
// 全 0 延迟时补偿退化为恒等(与不补偿逐位一致), 便于 A/B 对照与安全回退。
// -----------------------------------------------------------------------------
struct PidfDelayModelExact {
    static constexpr int kHistory = 32;

    // 每轴: 内部模型(延迟自由)的历史 + 指令历史。
    std::array<double, kHistory> model_x{};
    std::array<double, kHistory> model_y{};
    std::array<double, kHistory> command_x{};
    std::array<double, kHistory> command_y{};
    // 控制周期的一阶平滑(用于把"每帧增益"归一化到参考帧率)。用平滑值而不是
    // 瞬时 dt, 否则周期抖动会直接变成增益抖动。
    double frame_dt_ema{};
    double model_x_state{};
    double model_y_state{};
    int step{};
    bool initialized{};

    // 测量侧延迟(采集->控制环拿到, 由 latency_probe 实测)与指令侧延迟
    // (HID + 游戏帧, 观测不到, 取 1 帧的保守常数)。单位: 秒。
    double measure_latency_sec{};
    // 指令侧延迟用"帧"表示: 从"位移下发"到"它反映到误差上"之间的帧数, 与检测帧率
    // 无关。它观测不到(取决于 HID 上报 + 游戏帧 + 采集), 只能取保守值。
    //
    // 取 1 帧: HID 上报 + 游戏帧的最小值。这个量观测不到, 只能估; 但实测它对
    // 结果影响很大(检测延迟 3 帧时, 真实 1 帧而假定 3 帧会让综合分从 16.2 恶化到
    // 66.4), 所以不要随手加大 —— 它必须与真实链路量级一致。
    double command_latency_frames{1.0};
};

void reset_pidf_delay_model(PidfDelayModelExact& model) noexcept;

} // namespace cvm::recovered
