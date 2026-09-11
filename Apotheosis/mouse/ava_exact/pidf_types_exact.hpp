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

} // namespace cvm::recovered
