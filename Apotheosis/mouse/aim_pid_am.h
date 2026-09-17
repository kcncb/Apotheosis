// ─────────────────────────────────────────────────────────────────────────────
// AimMagic 1.0.30 PID 内核 —— 逐字移植
// ─────────────────────────────────────────────────────────────────────────────
//
// 本文件是 `FUN_140056f10`(内核) + `FUN_1400579f0`(双轴外壳) 的**逐行移植**。
// 真值来源: `docs/aimmagic-ground-truth.md` §0.5–§0.11, 原始语料
// `ghidra_out\pid_core.txt` L300-542(内核) / L752-921(外壳)。
//
// ★★★ 移植纪律(与 aim_tracker.h 同源) ★★★
//   1. 常数一律**从镜像字节读出**, 不做"看着差不多"的改写。
//      每个常数旁边的地址就是复核依据。
//   2. AM 没有的夹取/安全阀**不在这里加**。要加就加在**调用方**, 并显式标注
//      "本项目偏差", 默认关闭 —— 这样"默认行为与 AM 逐位一致"才是可验证的。
//   3. AM 的**判据形状**必须照抄, 包括那些看起来奇怪的地方(严格/非严格、
//      先减后乘、取中位数当夹取…)。"等价改写"就是偏差的来源。
//
// ★★ 与原版不同的地方(全部是"语言/平台", 不是"算法") ★★
//   · 原版是 C++ `double` + 手写 SSO `std::string` 判断档位 ⇒ 这里用 `enum class`。
//     ★ 枚举**数值**与 AM 一致(见 kProfile* 常量), 因为它们是内联立即数。
//   · 原版用 XOR 符号位掩码实现取负/取绝对值 ⇒ 这里直接用 `-x` / `std::fabs`。
//     两者对有限值逐位等价, 对 NaN/Inf 有差异 —— 但 AM 的输入契约不含 NaN。
//   · 原版状态块是一个裸 `double[]`, 用**偏移**寻址(X 一套、Y 一套) ⇒ 这里用
//     两个具名结构体成员。**偏移表见 §0.5**, 移植时逐个对照过。
//
// ★★ 明确**没有**移植的东西(见 §0.12 / §9) ★★
//   · 档位字符串比较本身 —— 我们用枚举。AM 同时存枚举**和**字符串(双份),
//     且个别地方只比字符串。本移植以**枚举**为准, 因为枚举才是算术分支的判据。
//   · `FUN_1400579f0` 里那两个 `std::string` 相关的分支(SSO 读取)—— 纯语言噪声。
//
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cmath>
#include <cstdint>

namespace boss
{
// ═══════════════════════════════════════════════════════════════════════════
// 档位枚举 —— ★ 数值来自反汇编的 `CMP <imm>`, 不是 QML 列表下标
// ═══════════════════════════════════════════════════════════════════════════
//
// 证据(见 ground-truth §0.3):
//   PID-Free     : `1400570db CMP RCX, 0x8` + 串比较 "PID-Free"
//   PID-Kalman   : `14005725c CMP R9, 0xa`  + 串比较 "PID-Kalman"
//   PID-Adaptive : `140057100 CMP RCX, 0xc` + 串比较 "PID-Adaptive"
//   PID-FrameSync/PID-EventSync : `1400571d9/14005721c CMP R9, 0xd`
//
// ★★★ FrameSync 与 EventSync **共用数值 0xD** —— 原版靠字符串区分, 但
//     在 PID 内核里**两者走完全相同的算术**(都到 `LAB_140057288` ⇒
//     `1 − exp(−40π·dt)`)。所以合并成一个枚举值是**忠实的**, 不是简化。
//
// ★ 本枚举只列**在语料里有比较点**的值。其余数值不得臆造(§0.3 结尾)。
enum class AimPidProfile : int
{
    kFree      = 0x8,   // PID-Free
    kKalman    = 0xA,   // PID-Kalman
    kAdaptive  = 0xC,   // PID-Adaptive
    kEventSync = 0xD,   // PID-FrameSync ≡ PID-EventSync (算术相同)
};

// ═══════════════════════════════════════════════════════════════════════════
// 已 byte 复核的常数(见 ground-truth §0.11)
// ═══════════════════════════════════════════════════════════════════════════
namespace pid_am
{
// `0x1401f6358` f64 = 1.0
inline constexpr double kDBlendInit      = 1.0;
// `0x1401f8010` f64 = 0.3   (D 项 Free/Adaptive 支的混合; 同时是 F 项新值权重)
inline constexpr double kBlendPointThree = 0.3;
// `0x1401f8018` f64 = 0.7   (F 项历史权重; 0.3 + 0.7 = 1.0)
inline constexpr double kBlendPointSeven = 0.7;
// `0x1401f8030` f64 = −40π  (★ 负号在外层, 送给 exp)
inline constexpr double kNegFortyPi      = -125.66370614359172;
// `0x1401f8000` f64 = 0.001 (dt 下限, 在两个外壳里各夹一次)
inline constexpr double kDtFloor         = 0.001;
// `0x1401f8028` f64 = 1e9   (纳秒 → 秒)
inline constexpr double kNanosPerSecond  = 1.0e9;

// ★ 档位判据用的是**取绝对值后**的比较(原版走 `0x7FFFFFFF` 掩码),
//   这里直接用 fabs。见 §0.10(b)。
} // namespace pid_am

// ═══════════════════════════════════════════════════════════════════════════
// 单轴状态 —— 对应 AM 状态块里 X(或 Y)那一套槽位
// ═══════════════════════════════════════════════════════════════════════════
//
// 偏移对照(§0.5 的轴偏移表):
//   本成员            X 偏移   Y 偏移
//   kp                +0x00    +0x18
//   d_hist            +0x08    +0x20
//   kd                +0x10    +0x28
//   kf                +0x30    +0x38
//   integral          +0xC8    +0xD0
//   setpoint_prev     +0xB8    +0xC0
//   f_hist            +0xD8    +0xE0
//   f_accum           +0xE8    +0xF0
//   i_clamp           +0x48    +0x50
//   kp_floor          +0x58    +0x60
struct AimPidAxis
{
    double kp = 0.0;             // +0x00/+0x18
    double kd = 0.0;             // +0x10/+0x28
    double kf = 0.0;             // +0x30/+0x38
    double i_clamp = 0.0;        // +0x48/+0x50  积分夹取幅值(对称)
    double kp_floor = 0.0;       // +0x58/+0x60  Adaptive 档的备用 Kp

    // ── 运行时状态(跨帧) ──────────────────────────────────────────────────
    double d_hist = 0.0;         // +0x08/+0x20   D 项低通历史
    double integral = 0.0;       // +0xC8/+0xD0   积分累加器(也是输出累加器)
    double setpoint_prev = 0.0;  // +0xB8/+0xC0   上一拍 setpoint
    double f_hist = 0.0;         // +0xD8/+0xE0   F 项低通历史
    double f_accum = 0.0;        // +0xE8/+0xF0   F 项累加器

    void reset()
    {
        d_hist = 0.0;
        integral = 0.0;
        setpoint_prev = 0.0;
        f_hist = 0.0;
        f_accum = 0.0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// 三处"闸门"布尔 —— AM 状态块的 +0xFC / +0xFD / +0xFE (见 §0.6)
// ═══════════════════════════════════════════════════════════════════════════
struct AimPidGates
{
    // `+0xFC` —— 由外壳 `FUN_1400579f0` 写入("已武装/可出拍")。
    bool armed = true;
    // `+0xFD` —— 为假时**清零积分累加器**, 且不更新积分。
    bool allow_integral = true;
    // `+0xFE` —— 只决定出口**加不加上**积分项, 不影响累加本身。
    bool output_integral = true;
};

// ═══════════════════════════════════════════════════════════════════════════
// 内核: 单轴一步 —— `FUN_140056f10` 的逐行移植
// ═══════════════════════════════════════════════════════════════════════════
//
// 原签名: `double FUN_140056f10(undefined8 *param_1, char param_2,
//                              double param_3 /*setpoint*/, double param_4 /*dt*/)`
//
// ★★★ 三条必须照抄的**形状**(不是笔误), 顺序都在原版里逐一核对过 ——
//     每一条都是我第一版写错、被回归抓出来的(见 docs/aimmagic-ground-truth.md §8.3):
//   ① 积分累加与它的夹取**在**第 1 帧早退**之前**(原版 `LAB_140057174` 那段
//      积分代码在 `if (frame_index == 1)` 上面) ⇒ **第 1 帧会累加积分**,
//      早退只跳过 **D 项与 F 项**。(我第一版把早退当成"整段跳过", 被回归抓出来了。)
//   ② `+0xFD` 的语义**只在 Adaptive 档存在**(见函数体内的 `c_var5` 推导)。
//   ③ setpoint 历史(`+0xB8/+0xC0`)在**第 1 帧不被写入**(早退跳过它), 从第 2 帧起才更新。
//   ★ 另有第四条(积分夹取在 Free/Adaptive 档被跳过), 见函数体内注释。
//
// 参数:
//   ax          该轴状态(会被就地更新)
//   g           闸门
//   setpoint    目标位置(AM 的 param_3)
//   dt          秒(AM 的 param_4); 调用方已保证 >= kDtFloor
//   profile     档位
//   frame_index 本轴第几拍调用(AM 的 `*(int*)(param_1 + 0x1f)`, 从 1 开始)
//
// 返回: 该轴本拍输出(AM 的返回值 = P + I + D + F)
inline double amPidAxisStep(AimPidAxis& ax, const AimPidGates& g,
                            double setpoint, double dt,
                            AimPidProfile profile, int frame_index)
{
    // ── 闸门 1: 积分累计开关 `cVar5`(§0.6) ────────────────────────────────
    // 原版:
    //   cVar5 = (+0xFC != 0) && (+0xFE != 0)          // 初始
    //   if (Adaptive) {
    //       if (+0xFD == 0) *pdVar14 = 0.0;           // 清零积分累加器
    //       if (cVar5) { cVar5 = (+0xFD != 0); goto 累加; }   // ★ cVar5 被 +0xFD 覆盖
    //       // cVar5 == 0 且是 Adaptive ⇒ 落到 else, 保持 cVar5 = 0
    //   } else { /* 非 Adaptive 直接到 累加, cVar5 不被覆盖 */ }
    //
    // ★★★ 三条极容易抄错的地方:
    //   ① `cVar5` 的初值是 `+0xFC && +0xFE`(**两个都要真**)。
    //   ② `+0xFD` 只在 **Adaptive 档**才参与门控 —— 非 Adaptive 档 `+0xFD` 完全没用!
    //      (原版那段 `if (bVar16)` 就是 Adaptive 判据。)
    //   ③ `+0xFD` 为假时清零积分这件事也**只在 Adaptive 档**发生。
    const bool adaptive = (profile == AimPidProfile::kAdaptive);
    bool c_var5 = g.armed && g.output_integral;      // ①
    if (adaptive)                                    // ②
    {
        if (!g.allow_integral)
            ax.integral = 0.0;                       // ③
        if (c_var5)
            c_var5 = g.allow_integral;               // ★ 覆盖
    }

    // ── Adaptive 档的 Kp 替换(§0.10(b)) ──────────────────────────────────
    // 原版:
    //   if (profile == "PID-Adaptive") {
    //       if (|setpoint| > |setpoint_prev|) { Kp = kp_floor; }
    //   }
    // ★ 是**严格大于**, 且两边都**先取绝对值**(原版走 0x7FFFFFFF 掩码)。
    double kp = ax.kp;
    if (adaptive)
    {
        if (std::fabs(setpoint) > std::fabs(ax.setpoint_prev))
            kp = ax.kp_floor;
    }

    // ── 积分累加 ★★ 这一段在【第 1 帧早退之前】(原版顺序如此) ─────────────
    // 原版: `dVar21 = dVar21 * param_3 * param_4 + *pdVar14; *pdVar14 = dVar21;`
    // 注意乘法顺序是 `Kp * e * dt`(照抄顺序是纪律)。
    if (c_var5)
    {
        double acc = kp * setpoint * dt + ax.integral;
        ax.integral = acc;

        // ── 积分夹取: 三值取中位数 = clamp(acc, -i_clamp, +i_clamp) ───────
        // 原版用 `{acc, -clamp, +clamp}` 取中;B/C 由 `0x8000000000000000`
        // 符号位异或得到 `-clamp`(§0.9)。
        // ★★ Free / Adaptive 两档**跳过**这道夹取(原版:
        //    `if ((profile != 8) || (串 != "PID-Free"))` 那一串判断)。
        const bool skip_clamp =
            (profile == AimPidProfile::kFree) ||
            (profile == AimPidProfile::kAdaptive);
        if (!skip_clamp)
        {
            const double lo = -ax.i_clamp;
            const double hi = ax.i_clamp;
            // 中位数(照抄原版的比较结构, 不写 std::clamp)
            double med = acc;
            if (lo <= med) { med = (med <= hi) ? med : hi; }
            else           { med = lo; }
            ax.integral = med;
        }
    }

    // ── 第 1 帧: 直接返回 P, 不碰 D/F(原版 `== 1` 早退) ───────────────────
    // ★ 早退**只**跳过 D 与 F —— 跳过之前积分已经累加过了(见上面的顺序说明)。
    if (frame_index == 1)
        return kp * setpoint;

    // ── D 项混合系数 dVar18(§0.7) ─────────────────────────────────────────
    double blend = pid_am::kBlendPointThree;   // DAT_1401f8010 = 0.3
    if (profile == AimPidProfile::kEventSync ||
        profile == AimPidProfile::kKalman)
    {
        // ★★★ `1 − exp(−40π·dt)` —— **不是**线性式!
        // 常数 `0x1401f8030` = −40π(带负号), 原版调 IAT 里的 exp。
        // 线性式在 dt ≈ 7.96ms 处就等于 1, 8.3ms 帧率下是 1.043 > 1 —— 荒谬。
        // 见 ground-truth §0.11 的数值判据表。
        blend = 1.0 - std::exp(pid_am::kNegFortyPi * dt);
    }

    // ── D 项(§0.7) ────────────────────────────────────────────────────────
    // 原版:
    //   dVar21 = DAT_1401f6358;                 // = 1.0  ★ 见下
    //   dVar21 = (dVar21 - dVar18) * d_hist + ((setpoint - setpoint_prev)/dt) * Kd * dVar18;
    //   d_hist = dVar21;
    //
    // ★★★ 这里**极容易抄错**: `dVar21` 在进这条式子之前被赋成了
    //     `DAT_1401f6358 = 1.0`(上一批语句 `LAB_140057174` 处),
    //     所以 `(dVar21 - dVar18)` 展开就是 **`(1.0 - blend)`**。
    //     它不是"历史值减系数" —— 那是把 `dVar21` 误当成历史槽了
    //     (真正的历史槽是 `*(double*)(param_1 + lVar6)`, 在式子右侧另取)。
    //     写错会得到一个**依赖历史值的系数**, 而且往往还能收敛, 所以极难发现。
    const double d_new =
        (pid_am::kDBlendInit - blend) * ax.d_hist
        + ((setpoint - ax.setpoint_prev) / dt) * ax.kd * blend;
    ax.d_hist = d_new;

    // ── F 项(§0.8) —— ★ 只有 Adaptive 档存在 ─────────────────────────────
    double f_term = 0.0;
    if (profile == AimPidProfile::kAdaptive)
    {
        // 原版:
        //   dVar1 = ((setpoint - setpoint_prev)/dt) * 0.3 + f_accum_hist * 0.7
        //   f_accum_hist = dVar1
        //   dVar2 = kf * dVar1
        const double f_new =
            ((setpoint - ax.setpoint_prev) / dt) * pid_am::kBlendPointThree
            + ax.f_hist * pid_am::kBlendPointSeven;
        ax.f_hist = f_new;
        ax.f_accum = f_new;          // 原版把同一个值也写进 +0xE8/+0xF0
        f_term = ax.kf * f_new;
    }

    // ── 更新 setpoint 历史 ★ 位置必须在 D/F 都算完之后 ────────────────────
    ax.setpoint_prev = setpoint;

    // ── 出口(§0.5): P + I + D + F ────────────────────────────────────────
    // 原版: `return Kp*setpoint + (output_integral ? integral : 0) + D + F;`
    const double i_term = g.output_integral ? ax.integral : 0.0;
    return kp * setpoint + i_term + d_new + f_term;
}

} // namespace boss
