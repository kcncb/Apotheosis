# AimMagic 1.0.30 完整破解事实（项目内定稿）

> 本文**只写从二进制/反编译/解包 QML 直接读到的东西**，每条带可复核的地址或行号。
> 与 `docs/aimmagic-comparison.md`、`docs/aimmagic-port-spec.md` 的关系：
> 那两份含"我的推断/设计意图"，**本文不含推断**；凡属推断都在 §9 单独标注。
>
> 语料根：`C:\Users\Administrator\Downloads\AimMagic_RE_extracted\AimMagic_RE\v1030\`
> 常量来源：`unpacked\AimMagic_unpacked.exe`，`image_info.txt` 记 `imageBase=0x140000000`，
> **所有节 VA==RawPtr**，故 `文件偏移 = VA − 0x140000000`（已实测校验，见 §1）。
>
> 本文的用途：**修正移植里的偏差**。§8 列出与本项目现实现不符的条目。

---

## 0. PID 链路的调用拓扑（2026-09-16 补齐，先读这一节）

★ 这一节是**本轮新补**的：之前只知道内核 `FUN_140056f10` 内部怎么算，
不知道它的输入从哪来、输出往哪去。现在把外壳和调用点都定位了，
**整条链是闭合的**（每条都有 `callmap.csv` 的边 + 反汇编行号）。

```
FUN_140067000  (主循环, 每帧一次)
   │
   ├─ 帧同步路径 (L1104-1121) ──> FUN_140057660(lVar17+0x800, puVar40+0x2d8,
   │                                            (double)*(f32*)(puVar40+0x534),
   │                                            (double)*(f32*)(puVar40+0x538))
   │
   └─ 事件同步路径 (L1235-1242) ─> FUN_140057d20(lVar17+0x800, puVar40+0x170,
   │                                            dVar51 /* dt */, dVar50)
   │
   └─ 两者都 ──> FUN_1400579f0(param_1, param_2, param_3, param_4, dt)
                     │                                    ← **仍缺函数体，待反编译**
                     └──> FUN_140056f10(param_1, axis, setpoint, dt)    [内核]
```

### 0.1 两个外壳函数（**已有函数体**，`funcs\FUN_140057660.txt` / `FUN_140057d20.txt`）

两者形状几乎相同，**都在算 dt 然后调同一个 `FUN_1400579f0`**：

```c
// FUN_140057660 — 自己取时钟算 dt
FUN_140025f80(&local_res8);                    // 取单调时钟
lVar1 = *(longlong *)(param_1 + 0xb0);         // 上一次的时间戳
*(longlong *)(param_1 + 0xb0) = local_res8;    // 存回
dVar3 = (double)(local_res8 - lVar1) / DAT_1401f8028;   // ★ /1e9  ⇒ 纳秒 → 秒
dVar2 = DAT_1401f8000;                         // 0.001
if (dVar2 <= dVar3) { dVar2 = dVar3; }         // dt = max(dt_raw, 0.001)
FUN_1400579f0(param_1, param_2, param_3, param_4, dVar2);

// FUN_140057d20 — dt 由调用方给(param_5)
FUN_140025f80(local_res8);
dVar2 = DAT_1401f8000;                         // 0.001
if (DAT_1401f8000 <= param_5) { dVar2 = param_5; }   // dt = max(param_5, 0.001)
*(undefined8 *)(param_1 + 0xb0) = *puVar1;     // 仍然更新时间戳
FUN_1400579f0(param_1, param_2, param_3, param_4, dVar2);
```

★ **两条 dt 下限都是 `0.001` 秒**(`DAT_1401f8000` = 0.001 f64, 见 §1)。
★ `param_1 + 0xb0` 是**上一拍的时间戳槽**(纳秒)。`FUN_140025f80` 是取时钟的封装。
★ **两个外壳的返回值都是 `param_2`**(不是内核的返回值)—— 也就是说
`FUN_1400579f0` 的**结果是通过 `param_2` 指针写回**的, 不是靠返回值传。

### 0.2 调用点证据(`FUN_140067000`)

**帧同步**(L1109-1111):
```c
FUN_140057660(lVar17 + 0x800, puVar40 + 0x2d8,
              (double)*(float *)(puVar40 + 0x534),     // setpoint X
              (double)*(float *)(puVar40 + 0x538));    // setpoint Y
auVar1 = *pauVar22;                    // 返回值当 16 字节向量读回 ⇒ 两个 double
*(undefined1 (*) [16])(puVar40 + 0xb8) = auVar1;   // X/Y 结果写在 +0xb8 / +0xc0
```

**事件同步**(L1235-1242):
```c
lVar17 = *plVar33;
*(undefined8 *)(puVar40 + 0x20) = *(undefined8 *)(puVar40 + 0x4d0);   // ★ 写入 +0x20
FUN_140057d20(lVar17 + 0x800, puVar40 + 0x170, dVar51 /*dt*/, dVar50);
dVar53 = *(double *)(puVar40 + 0x178);   // 结果 Y
dVar50 = *(double *)(puVar40 + 0x170);   // 结果 X
```

★★ **`puVar40 + 0x20` 就是 §6 里那个"在途换算链"写入的同一个槽**
(§6 L1209 `*(double *)(puVar40 + 0x20) = dVar56 / dVar51;`) —— 也就是说
**在途换算的结果在调 PID 之前被显式搬进 `+0x20`**, 供 `FUN_1400579f0` 消费。
这解释了 §6.2 那个"拷贝块"到底服务于谁: **它是 PID 的输入之一**。

### 0.3 ★★★ 更正：那个"双轴外层"其实**早就反编译过**了（我上一轮说它缺失是错的）

> ⚠️ 本节替代了上一轮那句"`FUN_1400579f0` 仍缺函数体、在拿到它之前不许动 PID 移植"。
> **那句话是错的** —— 它的完整 C 伪代码一直在 `ghidra_out\pid_core.txt` **L752-921**，
> 我上一轮只看了 `funcs\` 目录的文件名清单、没看 `ghidra_out\` 的正文就下了结论。
> **教训：说"某个函数缺失"之前，必须把两个语料目录都搜一遍正文，不能只看文件名。**

上一轮我声称 `FUN_1400579f0` "仍缺函数体" —— **这是错的**。它的完整 C 伪代码
**一直在 `ghidra_out\pid_core.txt` L752-921**，只是我搜的时候只看了函数名清单没看正文。
完整签名与结构（**每条都在文件里有行号可复核**）：

```c
double * FUN_1400579f0(double *param_1, double *param_2, undefined8 param_3, double param_4)
//                        ↑状态块        ↑输出(X,Y)        ↑setpoint对    ↑dt(秒)
{
  uVar11 = (uint)((ulonglong)param_3 >> 0x20);       // setpoint 的第二个 double

  // ── 档位判断：内联的 64-bit 字符串立即数比较 ──────────────────
  if (param_1[0x14] == <0x000000000000000C>) {        // == 12
    // 取 pid_profile 字符串（SSO: [0x15] > 0xf ⇒ *[0x12] 才是堆指针）
    lVar6 = (longlong)*pdVar4 + -0x706164412d444950;  // "PID-Adap"
    if (lVar6 == 0) lVar6 = *(uint*)(pdVar4+1) - 0x65766974;   // "tive"
    bVar7 = (lVar6 == 0);                             // ⇒ bVar7 ⟺ profile == "PID-Adaptive"
  } else bVar7 = false;

  *(int*)(param_1 + 0x1f) += 1;                       // ★ 帧计数器 +1

  if (param_1[0x14] == <0x0000000000000007>) {        // == 7
    bVar8 = (*pdVar3 == <0x656572462d444950>);        // "PID-Free"
    ...
  }
  ...
}
```

★ **档位枚举(`param_1[0x14]`)** —— 由内联立即数 + 字符串双重证据确定：

| 值 | profile | 证据(反汇编立即数) |
|---|---|---|
| **0x8** | `PID-Free` | `1400570db CMP RCX, 0x8`；`140057a93 CMP qword ptr [RDX+0x10], 0x8` |
| **0xA** | `PID-Kalman` | `14005725c CMP R9, 0xa` |
| **0xC** | `PID-Adaptive` | `140057009 CMP qword ptr [RDX+0x10], 0xc`；`140057100 CMP RCX, 0xc`；`140057a47` |
| **0xD** | `PID-FrameSync` **和** `PID-EventSync` | `1400571d9 CMP R9, 0xd`；`14005721c CMP R9, 0xd` |

★★★ **`0xD` 同时表示 FrameSync 和 EventSync** —— 两处以不同字符串区分：
`"PID-FrameSync"`(L194 附近) 与 `"PID-EventSync"`(L213 附近)。
两者的**算术行为完全相同**(都跳到 `LAB_140057288`，即 `dVar18 = 1 − 40π·dt`)。
⇒ **对 PID 内核而言 FrameSync ≡ EventSync**，档位只影响"谁来喂它"(§0.2 的两个外壳)。

★★★ **方法论警告(踩过的坑)**: Ghidra 把 `param_1[0x14] == 0x8` 渲染成了
`param_1[0x14] == 3.95252516672997e-323`(一个 denormal double) —— 而且**渲染错了**
(它印出来的位模式是 `0x...07`，与反汇编的 `0x8` 不符)。
**凡是对"枚举槽位"的比较，一律以反汇编的 `CMP <imm>` 为准，不要信 C 伪代码里的浮点常量。**
这条差点让我把整个档位映射抄错一位。

★ `param_1[0x14]` 是枚举值(以 8 字节槽存放)，`param_1[0x12]/[0x15]` 是那个 profile
**字符串**（std::string，SSO：容量 `> 0xf` 时 `*[0x12]` 才是堆指针）。

★★ **顺带更正 §9.9**: 那里说 `PID-Positional`/`PID-Incremental`/`PID-VelocityD`
是"无引用遗留字面量" —— **档位是数字枚举，不是字符串**，所以"字面量无引用"与
"档位存在"并不矛盾；但上面这张表里的**四个值**是**有据可查的真档位**。
其余数值(0–7, 9, 0xB)在本份语料里**没有找到比较点**，不得臆造。

（函数体余下部分见 `ghidra_out\pid_core.txt` L752-921：帧内插值比例
`dVar12 = 帧计数/param_1[0xd]`、对 `*param_1` 与 `param_1[3]` 做**按帧比例的
两段插值**、然后调 `FUN_140056f10(param_1,0x78)` 与 `(param_1,0x79)` 两轴、
再过 `FUN_140056e40` 做限幅、最后**取三个候选值中的中位数**
(`local_148[0..2]` / `local_130[0..2]`)写进 `param_2[0]`/`param_2[1]`。）

### 0.5 ★★★ PID 内核 `FUN_140056f10` 的完整状态块布局（移植的契约）

签名: `double FUN_140056f10(undefined8 *param_1, char param_2, double param_3, double param_4)`
- `param_1` = **状态块基址**(下面是全部偏移)
- `param_2` = **轴选择器字符**: `'x'`(0x78) 或 `'y'`(0x79)。★ 判据是 `param_2 != 'x'`
  (而不是 `== 'y'`)，所以**任何非 'x' 的值都走 Y 轴**。
- `param_3` = **setpoint**(目标位置)
- `param_4` = **dt**(秒)

★★ **轴偏移表**(`bVar16 = (param_2 != 'x')`，即 `bVar16` 为真 ⇒ **Y 轴**):

| 量 | 取址表达式 | X 偏移 | Y 偏移 |
|---|---|---|---|
| Kp / Ki 等参数对(读成 8 字节) | `param_1` 或 `param_1 + 3` | `+0x00` | `+0x18` |
| D 项状态(历史) | `lVar6` | `+0x08` | `+0x20` |
| D 增益 | `lVar6` | `+0x10` | `+0x28` |
| 某个增益(见下) | `lVar6` | `+0x30` | `+0x38` |
| 输出累加器(`pdVar14`) | `lVar13` | `+0xC8`(200) | `+0xD0` |
| setpoint 历史(`dVar3`) | `lVar6` | `+0xB8` | `+0xC0` |
| **F 项状态** | `lVar6` | `+0xD8` | `+0xE0` |
| **F 项累加器** | `lVar13` | `+0xE8` | `+0xF0` |
| 积分限幅(`x_i_clamp`) | `lVar9` | `+0x48` | `+0x50` |
| Kp 下限(`x_kp_floor`) | `lVar9` | `+0x58` | `+0x60` |

★ 注意 `+0xC8`(200) **不是 8 的整数倍对齐区**，Ghidra 给的字面量是 `200`。

★★ **函数开头的"参数对"读法**(Ghidra 把它当 `undefined8` 拆高低 dword):
```c
uVar22 = (undefined4)*puVar15;                       // 低 dword
uVar23 = (undefined4)((ulonglong)*puVar15 >> 0x20);  // 高 dword
...
return (double)CONCAT44(uVar23,uVar22) * param_3 + ... ;
```
⇒ **Kp 就在 `+0x00`(X) / `+0x18`(Y) 的 8 字节槽里，作为 double**。
开头的拆/合只是 Ghidra 的寄存器分配噪声, 语义就是 `Kp * setpoint`。

★★★ **出口**: `return Kp*setpoint + 输出累加器 + D项 + F项;`
```c
return Kp*param_3 + *pdVar14 + dVar21 + dVar2;
```
—— 即 **P + I + D + F**，其中"积分"是 `*pdVar14`(那个 `+0xC8/+0xD0` 累加器)。

### 0.6 ★★ 内核的三处"闸门"布尔（`+0xFC/+0xFD/+0xFE`）

```c
if ((*(char*)(param_1 + 0xfc) == '\0') || (*(char*)(param_1 + 0xfe) == '\0'))
     cVar5 = 0; else cVar5 = 1;      // ⇒ cVar5 ⟺ (+0xFC != 0 && +0xFE != 0)
```

| 偏移 | 名字(本项目的叫法) | 作用 |
|---|---|---|
| `+0xFC` | "可以出拍/已武装" | 在 `FUN_1400579f0` 里被写入(`*(undefined1*)((longlong)param_1+0xfc) = uVar5;`) |
| `+0xFD` | "允许预测/前馈" | `if (+0xFD == 0) *pdVar14 = 0.0;` ⇒ **为 0 时清零积分累加器** |
| `+0xFE` | "允许输出积分项" | 出口: `if (+0xFE != 0) { 把 *pdVar14 计入返回 }` |

★★ 注意 `+0xFD` 的语义**不是**"清零积分"这么简单 —— 它**同时**决定要不要走
`cVar5` 那条路(即是否更新积分)。
★ `+0xFE` 只影响出口要不要**加上**积分项，**不影响积分本身的累加**。

### 0.7 ★★ D 项的完整公式（含两支混合系数）

```c
dVar21 = DAT_1401f6358;            // = 1.0   ★★ 见下面的警告
if (*(int*)(param_1 + 0x1f) == 1) { dVar21 = 0.0; goto LAB_1400572bf; }   // ★ 第 1 帧 D=0
//   ★★ 注意: 这个 `== 1` 早退发生在【积分累加之后】,
//      所以第 1 帧**积分照常累加**、只是 D(和 F)被跳过。
...
dVar18 = DAT_1401f8010;            // 0.3   (Free/Adaptive 支的默认)
if (profile == "PID-FrameSync" || "PID-EventSync" || "PID-Kalman") {
    dVar18 = 1.0 - (*_DAT_1401f07a0)(param_4 * _DAT_1401f8030);   // = 1 - exp(-40π·dt)
}
dVar21 = (dVar21 - dVar18) * *(double*)(param_1 + lVar6)          // ← ★★★ 注意
       + ((param_3 - dVar3) / param_4) * dVar1 * dVar18;          // (Δsetpoint/dt) * Kd * 系数
LAB_1400572bf:
*(double*)(param_1 + lVar6) = dVar21;      // 存回 D 历史槽
```

★★★ **这一处是本轮最容易抄错的地方，必须逐字读**：
式子里那个 `dVar21` **不是 D 历史槽**。它在进入这条式子之前刚被赋成
`DAT_1401f6358 = 1.0`（上一批语句 `LAB_140057174`）。所以：

```
D_new = (1.0 - blend) * D_hist + (Δsetpoint / dt) * Kd * blend
```

真正的 D 历史槽是 `*(double*)(param_1 + lVar6)`（X `+0x08` / Y `+0x20`），
它在式子**右侧另取**、在式子**算完后写回**。
⇒ 误读成 `(D_hist - blend) * D_hist` 会得到一个**依赖历史值的系数**，
而且它**往往还能收敛、测试也可能全绿** —— 属于"看起来正常"的错误。

★★ `FUN_1401daf1a` 已确认为 **`exp`**（IAT thunk + 数值判据双重证据，见 §0.11）。
`_DAT_1401f8030` = **−40π**，所以是 **`1 − exp(−40π·dt)`**。
**绝对不要写成线性式** —— 见 §0.11 的判据表。

### 0.8 ★★ F 项（前馈）公式

```c
if (bVar16 /* = Adaptive */) {
    dVar1 = ((param_3 - dVar3) / param_4) * dVar17        // dVar17 = DAT_1401f8010 = 0.3
          + *(double*)(lVar13 + param_1) * DAT_1401f8018; // F 历史 * 0.7
    *(double*)(lVar13 + param_1) = dVar1;                  // 存回 F 历史槽(+0xD8/+0xE0)
    dVar21 = *(double*)((longlong)param_1 + lVar6);        // ★ 把 D 项重新读回 dVar21
    dVar2 = dVar2 * dVar1;                                 // ★ F 项 = Kf * F历史
} else {
    dVar2 = 0.0;                                           // ★ 非 Adaptive ⇒ F 项恒 0
}
```

★★ **F 项只在 `PID-Adaptive` 档存在**（`bVar16` 就是 Adaptive 判据）。
★ `dVar2` 在进这段之前从 `+0x30`(X)/`+0x38`(Y) 读出 ⇒ **它就是 Kf**。
★ **两个槽写的是同一个值**：`+0xE8/+0xF0`（下文称 F 累加器）与
`+0xD8/+0xE0`（F 历史）在本函数里**总是相等** —— 因为原版就是这么写的。
移植时可以只留一个，但**要知道原版有两个**（将来若在别处发现第二个槽被读，
就说明这里漏了消费者）。

### 0.9 ★★ 积分项的两道夹取

```c
dVar21 = dVar21 * param_3 * param_4 + *pdVar14;   // 累加: 误差 * dt
*pdVar14 = dVar21;
// 若 profile 不是 PID-Free 且不是 PID-Adaptive:
local_res18[0] = *(double*)(param_1 + lVar9);      // x_i_clamp / y_i_clamp
local_res8 = -local_res18[0];                       // ★ 取负 = 下界
// 然后取三值中位数(夹取): clamp(dVar21, -clamp, +clamp)
```

★ 夹取用的是**"取三值中位数"**手法(Ghidra 渲染成指针选择):
把 `{dVar21, −i_clamp, +i_clamp}` 排序取中 ⇒ 等价 `clamp(dVar21, −i_clamp, +i_clamp)`。
★ 负号来自 `DAT_1401f4680 = 0x8000000000000000`(仅符号位) XOR 原值 —— 即**取负**。
★★ **Free / Adaptive 档跳过这道积分夹取**。
★ **并且 Kp 也有下限**(`x_kp_floor` / `y_kp_floor`，`+0x58/+0x60`)，在此段之前使用。

### 0.10 ★ Adaptive 档的两处特殊行为

**(a) 增益按帧比例插值**(在 `FUN_1400579f0` 里，不在内核):
```c
if (Adaptive && 0 < *(int*)(param_1 + 0xd)) {
    dVar12 = (double)*(int*)(param_1 + 0x1f) / (double)*(int*)(param_1 + 0xd);  // 帧计数/总帧数
    *param_1    = (*param_1    - param_1[0xb]) * dVar12 + param_1[0xb];   // Kp 插值
    param_1[3]  = (param_1[3]  - param_1[0xc]) * dVar12 + param_1[0xc];   // 另一个增益插值
}
```
⇒ **Adaptive 档在 `param_1[0xd]` 帧内把 Kp 从初值线性插值到终值**。

**(b) setpoint 门控**(在内核里):
```c
if (profile == "PID-Adaptive") {
    if (|setpoint| > |dVar3|/*setpoint 历史*/) {
        Kp = *(double*)(param_1 + 0x58 /*或 0x60*/);   // ★ 换用 kp_floor 那个槽当 Kp
    }
}
```
★ 注意是**严格大于**(`dVar17 != (double)CONCAT44(uVar20,uVar19)` 那一项)，
且在**取绝对值之后**比较。

### 0.11 已确认的常量（本轮 byte 复核）

| VA | 类型 | 值 | 用途 |
|---|---|---|---|
| `0x1401f6358` | f64 | **1.0** | D 项第 1 帧的初值 |
| `0x1401f8010` | f64 | **0.3** | (a) Free/Adaptive 支的 D 混合系数；(b) F 项新值权重 |
| `0x1401f8018` | f64 | **0.7** | F 项历史权重(与 0.3 配对 ⇒ 1.0) |
| `0x1401f8030` | f64 | **−125.663706143592** = **−40π** | `exp(−40π·dt)` 的系数(注意负号) |

★★★ **`FUN_1401daf1a` 已确认为 `exp`** —— 三条独立证据：
1. 它是一个 **IAT thunk**(`FF 25` = `jmp qword ptr [rip+...]`)，槽位 `0x1401f07a0`
   存放运行时解析出的地址 `0x7FFAFDDEA5E0`(在 DLL 里，不在镜像内) ⇒ **是导入函数**。
2. 常数是 `−40π`(**负号**) —— 传给 `exp` 得 `exp(−1.0431) ≈ 0.3524`。
3. ★ **数值判据(决定性)**: 作为"低通混合系数"必须落在 `[0,1]`。

   ★★ **先纠正我自己上一轮算错的一处**: 线性式不是 `1 − 40π·dt`(把常数当正数了)。
   常数是 **`c = −40π`**, 所以线性式 = **`1 + c·dt = 1 − 40π·dt`**。

   | dt | `1−exp(c·dt)` ✅ | `1−exp(−c·dt)` ❌ | 线性 `1 + c·dt` ❌ |
   |---|---|---|---|
   | 0.1 ms | 0.012488 | −0.0126 | **0.987434** |
   | 1.0 ms | 0.118089 | −0.1339 | **0.874336** |
   | 8.3 ms | **0.647607** | −1.84 | **−0.043009** |
   | 16.6 ms | 0.8758 | −7.05 | −1.0860 |
   | 46 ms | 0.9969 | −322.93 | −4.7805 |

   ★★★ **注意线性式在 `dt > 1/(40π) ≈ 7.96 ms` 处变号 ⇒ 给出负系数**
   (8.3ms 帧率下是 **−0.043**)。负的混合系数会让"新值"与"历史"**反相叠加** ——
   那不是精度问题, 是**定性错误**。

   ★★★ **更关键的一条(比"变负"更能定性)**: 两者**在 `dt→0` 时的极限不同** ——
   `1 − e^{c·dt} → 0`, 而 `1 + c·dt → 1`。所以**任何**"用线性近似代替 exp"的写法
   在这里**必然**是错的(`dt=0.1ms` 时真值 0.0125 vs 线性 0.9874, 差 79 倍)。
   这和常见的"`1−e^{-x} ≈ x`"套路**形状就不一样**, 因为原版的 `1 −` 减的是
   `exp`, 不是"从 1 减去一个趋于 1 的量"。

★★ **移植时写成 `1.0 - std::exp(-40.0 * M_PI * dt)`**，
`dt=8.3ms ⇒ 0.6476`（即新值占 64.76%，历史占 35.24%）。
**时间常数 `1/(40π) ≈ 7.96 ms`** —— 比 §5 那个预测低通(0.05 ⇒ 约 20 帧)快得多，
两者是不同的滤波，别混。

★★★ **本轮教训(值得单列)**: 我在这一处**连续犯了两个错** ——
①先是把"`dVar21` 在式子前被赋成 1.0"读漏, 差点把 D 项写成 `(d_hist − blend)*d_hist`;
②然后在推导线性反例时**把常数的负号丢了**, 算出"1.043 > 1"这个**错误的反例**
(真反例是"−0.043 < 0", 而且真实判据是"极限都不同")。
两处都是**同一个毛病**: 依赖"我记得那个常数是什么", 而不是回去读字节。
⇒ **凡是式子里出现常数, 一律回镜像读一遍再写; 常数带符号时, 把符号当成式子的一部分抄。**
| `0x1401f4680` | u64 | **0x8000000000000000** | 仅符号位 ⇒ 用 XOR 实现**取负** |
| `0x1401f6360` | u32 | **0xFFFFFFFF** | 取绝对值掩码的**低** dword(passthrough) |
| `0x1401f6364` | u32 | **0x7FFFFFFF** | 取绝对值掩码的**高** dword(清零符号位) |

★★ **`−40π` 的负号 + 传给 `exp` ⇒ `exp(−40π·dt)`**，
`dt = 8.3ms` 时 = `exp(−1.0431)` ≈ **`0.3524`** ⇒ `dVar18 = 1 − 0.3524 = 0.6476`。
**若误写成线性式 `1 − 40π·dt` 会得到 `−0.043`(负数)** —— 这是必须避免的抄错。

---

### 0.12 ★★ 更正：`FUN_140089660` / `140089800` / `140089290` / `1400552e0` **不是选靶逻辑**

上一轮我把这四个列为"轨迹排序/并列打破规则"，**这个判断是错的**。
2026-09-16 用 Ghidra 反编译后确认，它们**全是 C++ 标准库容器/工具函数**：

| 函数 | 真身 | 证据 |
|---|---|---|
| `FUN_140089290` | `std::vector<Track>::insert` | 步长 `0x50`(= §4 的轨迹结构体大小)；`0x6666666666666667` 魔数倒数；容量上限 `0x333333333333333`；`(param_1[1]-*param_1)/0x50` |
| `FUN_140089660` | `std::vector<bool>` 构造/填充 | `param_2+0x1f >> 5` = 位向量字数；`1 << (param_2 & 0x1f)` 尾位掩码；按 `param_3` 填充 |
| `FUN_140089800` | 算术辅助 | 见 `build/re_out/tracker_internals.txt` |
| `FUN_1400552e0` | 同形状的容器增长 | 开头 **32 字节与 `FUN_140089290` 逐字节相同** |

★ **判据(可复核)**：两者开头字节完全一致 ——
```
40 55 57 41 54 41 55 41 57 48 83 EC 20 4C 8B 39 49 BD 66 66 66 66 66 66 66 66 4C 8B 49 08
```
同一个编译器模板的两个实例化(元素类型不同)。**凡是开头是
`push rbp/rsi/r12/r13/r15` + `SUB RSP,0x20` + `0x6666666666666667` 的，
都是 `vector<T>` 容器操作，别去里面找算法。**

★★★ **由此得到两条结论 —— 第二条已被 §0.15 部分推翻，务必连读**：

1. **`FUN_140089ac0`(主跟踪器) 内部确实没有打分** —— 它的关联判据
   (类别相等 + IoU 严格大于) 就是跟踪器**内部**的全部逻辑。
2. ~~`center_scoring_weight` / `distance_scoring_weight` / `size_scoring_weight`
   这三个键的算术消费点**仍未找到**~~ → **已找到: 见 §0.15。**
   真正的选靶打分器是 **`FUN_140088ab0`**（外壳 `FUN_140088a30`），
   它**不在跟踪器里**，在 `FUN_1400824e0` 里被调用。
   `size_scoring_weight` / `distance_scoring_weight` **确实有两个消费者**
   （`1400835b1` / `1400835df`）；而 **`center_scoring_weight` 在镜像里 0 处引用
   ⇒ 是死键**。

★★ **§0.12 原来的写法错在哪（这是要记的教训）**: 我用的判据是
"这四个函数都是 `vector` 容器函数 ⇒ **不存在**打分子系统" ——
那是**从"我没找到"推出"不存在"**。正确的做法是**顺着配置键的字符串引用往下找**
（每个键的字符串在镜像里几乎总是唯一或极少引用），而不是"看哪个函数名像排序"。
★ 唯一仍然成立的原结论是"这四个函数本身不是选靶逻辑"，那部分是对的。

### 0.14 ★★★ 闸门 `cVar5` 的真实语义（"三个开关"是错的）

原版 `pid_core.txt` L404-424 的完整结构：

```c
cVar5 = (+0xFC != 0) && (+0xFE != 0);          // ← 初值: 两个都要真
if (bVar16 /* = Adaptive */) {
    if (+0xFD == 0) *pdVar14 = 0.0;            // 清零积分累加器
    if (cVar5 != 0) {
        cVar5 = (+0xFD != 0);                  // ★★ cVar5 被 +0xFD 覆盖
        goto LAB_1400570af;                     // → 累加
    }
    // cVar5 == 0 且是 Adaptive ⇒ 落到 else, cVar5 保持 0
} else {
LAB_1400570af:
    if (cVar5 != 0) {
        dVar21 = dVar21 * param_3 * param_4 + *pdVar14;   // 累加积分
        *pdVar14 = dVar21;
        ...积分夹取...
    }
}
```

★★★ **三条极容易抄错的地方**：

1. **`+0xFD` 只在 Adaptive 档参与门控。** 非 Adaptive 档它**完全无效** ——
   既不累加也不清零。我第一版把它写成"全局积分开关"，被回归抓出来了。
2. **`+0xFD` 为假时清零积分这件事，也只在 Adaptive 档发生。**
3. **`+0xFC` 与 `+0xFE` 是积分的必要条件（全档通用）**；
   而出口那一处**又独立地**再测一次 `+0xFE`（`if (+0xFE != 0) i_term = integral`）。
   ⇒ `+0xFE` 有**两处**作用：既进 `cVar5` 的初值，又独裁出口。

★★ **出口那一处不可能只靠 `cVar5` 间接生效** —— 两者的效果不同：
`cVar5 = 0` 会让积分**停止累加**，而出口那处是"**累加了也不加进返回值**"。
`+0xFE` 关掉时两条同时发生（因为初值就是假）；但**若将来有人只改出口那处**，
行为会与"关掉 `cVar5`"不同。移植时两处都要保留。

### 0.15 ★★★ 找到真选靶了：`FUN_140088ab0`（**推翻 §0.12 的"没有独立打分子系统"**）

上一节我断定"没有独立的打分子系统" —— **这个结论只对了一半**，必须更正：

* **`FUN_140089ac0`（跟踪器）内部**确实没有打分（它的关联是"类别 + IoU 严格大于"）；
* 但**另有一个真正的选靶打分器 `FUN_140088ab0`**，它**不在跟踪器里**，
  在 `FUN_1400824e0`（瞄准主循环的一个大函数）里被调用。

★★★ **找到它的方法（可复现，也是这次的教训）**:
`size_scoring_weight` / `distance_scoring_weight` 这两个**字符串**在镜像里各只有
**1 处引用**（`0x1400835b1` / `0x1400835df`，都在 `FUN_1400824e0`）。
顺藤摸下去：

```
FUN_1400824e0
  LEA RDX,[0x1401f8e30] "size_scoring_weight"      ; 1400835b1
  CALL 0x14000e310                                  ; 造 std::string 键
  CALL 0x1400635e0                                  ; 读配置 -> XMM0
  MOVAPS XMM6,XMM0                                  ; ★ size weight
  LEA RDX,[0x1401f8e48] "distance_scoring_weight"   ; 1400835df
  CALL 0x14000e310
  CALL 0x1400635e0                                  ; 读配置 -> XMM0
  MOVAPS XMM2,XMM14                                 ; ★ distance weight
  ...
  CALL 0x140088a30(...)                             ; 只是转调外壳
      └─ FUN_140088ab0                              ; ★★ 真身
```

**`FUN_140088a30(undefined8 param_1)` 是个空壳**，只做 `FUN_140088ab0(...); return param_1;`。
真身是 **`float * FUN_140088ab0(float *param_1, undefined8 *param_2, longlong param_3,
longlong param_4, float param_5, float param_6, float param_7, float param_8)`**。

★★ **打分公式（`select_real2.txt` L714，逐字）**：

```c
fVar26 = fVar25 * param_8 + (DAT_1401f4634 - fVar23 / local_140) * param_7;
```

配合 L709-712 的 `fVar25`：
```c
fVar25 = fVar26 * pfVar17[3];                  // 框的某个尺寸 × 一个系数
if (fVar25 <= DAT_1401f4634) fVar25 = DAT_1401f4634;   // ★ 下限夹到 1.0
```

⇒ **`score = max(尺寸项, 1.0) × param_8 + (1.0 − 距离项/上限) × param_7`**

★★★ **`param_5`–`param_8` 的绑定：只确证了前两个，后两个【仍未定】 —— 而且我中途推错过一次**

**确证的部分**（这两条是硬的）：
- `param_5` ← `size_scoring_weight`（`1400835dd MOVSS XMM6,XMM0`，紧跟在读 size 键之后）
- `param_6` ← `distance_scoring_weight`（`1400835f6 MOVAPS XMM2,XMM14`，紧跟在读 distance 键之后）

**栈帧算术**（可复核，用来定 `param_7`/`param_8`）：
```
MOV [RSP+0x18],R8          ; 入口
PUSH RBP/RBX/RSI/RDI/R14   ; 5 × 8 = 0x28
LEA RBP,[RSP-0x80]
SUB RSP,0x180
⇒ RBP = RSP_entry − 0xA8 = (RSP_caller − 8) − 0xA8 = RSP_caller − 0xB0
⇒ [RBP+0xd0] = 调用方 [RSP+0x20]   ；  [RBP+0xd8] = 调用方 [RSP+0x28]
```
被调用方**只读这两个栈浮点**，并立刻算 `sqrt(a² + b²)`。

★★★ **我在这里犯了一个错，必须记下来**: 我一度断定"这两个是平台速度分量
⇒ `param_7/param_8` = `vx/vy`" —— **这是错的**。反证来自 `local_140` 的**用法**
（`select_real2.txt` L474-486、L714）：

```c
local_140 = param_5*param_5 + param_6*param_6;    // 注: 这里的 param_5/6 是 Ghidra 的标号
if (local_140 < 0.0) local_140 = FUN_1401daf4a();
local_140 = SQRT(local_140);
if (local_140 <= 1.0) local_140 = 1.0;            // ★ 下限夹到 1.0
...
fVar26 = fVar25 * <A> + (1.0 - fVar23 / local_140) * <B>;   // L714
```

`(1.0 − 距离 / local_140)` 是**归一化距离衰减**的标准形状，只在
`0 ≤ 距离 ≤ local_140` 时落在 `[0,1]`；
⇒ **`local_140` 是一个"距离归一化分母"（搜索半径/最大距离），不是速率。**
⇒ 被平方开方的那一对是 **(dx, dy)**，**不是 (vx, vy)**。

★★ **Ghidra 对这个函数的原型是错的**: 它写成
`(float *param_1, undefined8 *param_2, longlong param_3, longlong param_4,
float param_5, float param_6, float param_7, float param_8)` ——
但按 x64 约定那样前 4 个整数参数吃掉 RCX/RDX/R8/R9 之后，`param_5..param_8`
应当全在 `XMM0..XMM3`、**不会有栈参数**；而实际上序言读的是**栈槽**。
⇒ **`param_5`–`param_8` 这套标号在这个函数里不可信**，
`L714` 里的 `param_7`/`param_8` **很可能只是被误标的局部量**。

★★ **结论（诚实版）**: 已经确证的是
①真选靶函数 = `FUN_140088ab0`（外壳 `FUN_140088a30`）；
②两个打分权重键在 `1400835b1` / `1400835df` 被读，并作为**第 5/第 6 个浮点参数**传入；
③被调用方内部用 `(1.0 − 距离/半径)` 做距离衰减、`max(尺寸项, 1.0)` 做尺寸项。
**但打分式里两个乘数各自绑哪个参数，仍未定。**
⇒ **在把 `L714` 的两个乘数各自溯源清楚之前，不许照抄那一行。**
**"找到了函数"≠"看懂了公式"。**

★ **`center_scoring_weight` 在镜像里 0 处引用** —— 它在 `anchors_v1030.txt` L6 的键列表里
出现，但**二进制里根本没有这个字符串** ⇒ **是死键**（QML 侧也没有）。
这条与 §0.12 结尾"不许为找不到消费点的键写实现"一致。

★ **本节的教训（比发现本身更重要）**: 上一节我用的判据是"这四个函数都是容器函数
⇒ 没有打分子系统" —— 那是**从"我没找到"推出"不存在"**，属于 §8.3 同一类错误的变体。
正确的做法是**顺着配置键的字符串引用往下找**（每个键的字符串在镜像里几乎总是
唯一或极少引用），而不是"看哪个函数名像排序"。
★★ 而**本节我又犯了同一类错的反面**: 从"序言读了两个栈浮点、调用方在那儿压了 vx/vy"
推出"它们就是 vx/vy" —— 这是**用位置巧合当证据**，被 `local_140` 的**用法**直接推翻。
⇒ **判据必须来自量的"用法"，不能来自它的"来源位置"。**

---

## 1. 常量表（逐个从解包镜像字节读出，非文档转述）

读法：`f32 @ VA` = 4 字节小端 IEEE754；`f64 @ VA` = 8 字节。全部经独立复核。

| VA | 类型 | 值 | 用于 |
|---|---|---|---|
| `0x1401F461C` | f32 | **0.1** | 预测系数爬升步长 |
| `0x1401F9A08` | f32 | **0.2** | 预测系数回落步长 |
| `0x1401F4634` | f32 | **1.0** | 系数上限 |
| `0x1401F9A20` | f32 | **1.5** | 门限尺寸系数 |
| `0x1401F9A0C` | f32 | **0.8** | 门限尺寸系数（**不是 0.75**）+ 速度 Y 衰减 |
| `0x1401F8110` | f32 | **30.0** | 门限绝对下限 |
| `0x1401F9AA0` | f32 | **10.0** | 自身速度门（**不是 3.5057**） |
| `0x1401F9A00` | f32 | **0.02** | 方向翻转阻尼 |
| `0x1401F9A10` | f32 | **0.95** | 速度 X 衰减（滑行时） |
| `0x1401F4620` | f32 | **0.25** | 速度混合 K1 |
| `0x1401F4630` | f32 | **0.75** | 速度混合 K2（K1+K2=1.0） |
| `0x1401F462C` | f32 | **0.5** | IoU 半宽/半高换算 |
| `0x1401F5B88` | f32 | **0.3** | `tracking_iou_threshold` 默认 |
| `0x1401F8020` | **f64** | **1000.0** | ms 换算分母 |
| `0x1401F8028` | **f64** | **1e9** | ns 换算分母 |
| `0x1401F6358` | **f64** | **1.0** | dt 夹取上限 |
| `0x1401F8000` | **f64** | **0.001** | dt 夹取下限 |

★ **量纲陷阱**：`0x1401F8000` 当 f32 读是 `-5.19e11`，当 f64 读才是 `0.001`。
同理 `0x1401F8020/8028/6358` 当 f32 读都是 `0`。**这四个必须按 double 读。**

---

## 2. 用户可调参数（真值 = 配置解析器 + QML 双侧）

配置解析器：`ghidra_out\anchors.txt` 的 `FUN_14001ba90`（AimKey 装载）与同族函数。
QML：`qml_z\当前_PID_配置档位__驱动联动字段_enable_disable.qml`（PID/输出/Kalman/Scale）
与 `qml_z\----_预测补偿__全_AimKey_作用域_scope_2__----_.qml`（预测）、
`qml_z\----_模型与跟踪__全_Group_作用域_scope_1__----_.qml`（跟踪）。

### 2.1 跟踪（Group 作用域，scope=1）

| 键 | 默认 | 范围 | 解析器行 | QML 行 |
|---|---|---|---|---|
| `enable_tracking` | 0 | bool | anchors L21692 | 模型与跟踪 L86 |
| `min_hits` | **3** | [0,1000] | anchors L21694 | L98 |
| `max_age` | **5** | [0,100000] | anchors L21696 | L103 |
| `tracking_iou_threshold` | **0.3** | [0,1] | anchors L21698 | L108 |
| `tracking_velocity_sample_ms` | **20** | clamp[1,1000] | anchors L21702-21711 | L113 |

★★ **QML 界面显示 `min_hits` 默认 2、`max_age` 默认 30、`tracking_iou_threshold` 默认 0.5，
与解析器实到值 3 / 5 / 0.3 不一致。** QML 的 `defaultValue` 只是"控件初值"，
**二进制解析器的默认才是真正生效的那份**（解析器先读配置，缺键时用它的默认）。

### 2.2 预测补偿（AimKey 作用域，scope=2）

解析器 `anchors.txt` L14065-14070：

| 键 | 默认 | 出处 |
|---|---|---|
| `prediction_enabled` | 0 (false) | L14065 |
| `prediction_factor` | 0 | L14066（**旧键，作为 x/y 的回退默认**） |
| `prediction_factor_x` | ← `prediction_factor` | L14067 |
| `prediction_factor_y` | ← `prediction_factor` | L14068 |
| `prediction_min_width` | **0x14 = 20** | L14069 |
| `prediction_max_width` | **0x50 = 80** | L14070 |

★ **`prediction_factor` 这个旧键仍然被读**，且是 x/y 的缺省回退 —— 不是三选一，是"没给 x/y 就用它"。

### 2.3 PID 增益（档位作用域 `pid_profiles/<profile>`，scope=2）

QML `当前_PID_...qml` L168-278。全部 `subObject: "pid_profiles/" + currentProfile`。

| 键 | 默认 | 范围 | 显示条件 |
|---|---|---|---|
| `x_kp` / `y_kp` | 0.1 | [-100,100] | 恒显 |
| `x_ki` / `y_ki` | 0.0 | [-100,100] | 恒显 |
| `x_kd` / `y_kd` | 0.0 | [-100,100] | 恒显 |
| `x_kf` / `y_kf` | 0.0 | [-100,100] | 仅 PID-Adaptive |
| `x_i_clamp` / `y_i_clamp` | 6.0 | [0,9999] | FrameSync/EventSync/Kalman |
| `x_kp_floor` / `y_kp_floor` | 0.05 | [-100,100] | 仅 Free/Adaptive |
| `i_gate_radius` | 0.0 | [0,9999] | 仅 PID-Adaptive |
| `i_box_lost_hold_ms` | 20 | [0,5000] | FrameSync/EventSync/Kalman |
| `max_output_x` / `max_output_y` | 9999.0 | [0,9999] | 恒显 |
| `smooth_frames` | 0 | [0,60] | 仅 PID-Free |
| `smooth_threshold` | 0.0 | [0,1000] | 仅 PID-Free |
| `min_position_offset` | 0.0 | [0,1000] | 恒显 |
| `horizontal_deadzone` | 0.0 | [-10,10] | 恒显 |

★ `usesBoxIntegral(profile)`（QML L14-16）：
`x_i_clamp`/`i_box_lost_hold_ms` 只在 **FrameSync / EventSync / Kalman** 三档显示。
**EventSync 是"位置式带积分限幅"的档，与 Free/Adaptive 不同族。**

### 2.4 Kalman 参数（`kalman_*`）

QML L379-439，`visible: currentProfile === "PID-Kalman"`。键与默认：
`kalman_prediction_ms` 12.0 / `kalman_prediction_strength` 1.0 / `kalman_max_lead_px` 40.0 /
`kalman_warmup_frames` 3 / `kalman_mouse_effect_delay_ms` 8 / `kalman_process_noise` 20.0 /
`kalman_measurement_noise` 8.0 / `kalman_counts_per_pixel_x` 1.0 / `kalman_counts_per_pixel_y` 1.0。

★★ **这些键只在 `PID-Kalman` 档的界面上出现，但二进制里 `kalman_counts_per_pixel_x/y`
的真正消费者却是 FrameSync / EventSync 档**（见 §6.1）。界面 `visible` 只是显示控制，
**二进制不强制**它 —— `FUN_14001ba90` 读 `x_kp_floor`/`y_kp_floor`/`i_gate_radius`/
`smooth_frames`/`smooth_threshold` 时**没有任何档位判断**（loader L525-526, 636-637, 781-788），
**被隐藏的控件留在配置里的旧值照样被加载并消费**。
唯一真实的档位门控在 `FUN_140056f10` 内部（§7.2）。

### 2.4.1 `kalman_*` 到底有没有被算（逐键判定）

| 键 | 判定 | 证据 |
|---|---|---|
| `kalman_counts_per_pixel_x/y` | **真算**（当除数） | §6 L1208-1209 |
| `kalman_prediction_ms` | 有读取与 clamp [0,200] | loader L654-664 |
| `kalman_max_lead_px` | 有读取与 clamp [0,1000] | loader L708-718 |
| `kalman_process_noise` / `measurement_noise` | 传给噪声辅助函数 | loader L682-705 |
| `kalman_prediction_strength` | **只见搬运，未见算式消费点** | loader L667-679 |
| `kalman_warmup_frames` | **只见搬运，未见算式消费点** | loader L721-731 |
| `kalman_mouse_effect_delay_ms` | **只见搬运**（写进环窗 `0x530`，但消费点未定位） | loader L760-771 |

★ 后三项**不要断言"死键"** —— 它们可能被 `.themida` 未脱壳区域消费，Ghidra 没覆盖到。

### 2.5 本项目**凭空新增**、AM 里根本不存在的参数

对照 `anchors.txt` L4 的**全键清单**（从二进制字符串表抽出的权威列表），以下三个概念在 AM 中无任何对应键：

| 本项目的键 | AM 真值 |
|---|---|
| `esync_assoc_radius_px` | **不存在** —— 跟踪器只按"类别相等 + IoU 最大"，无距离门限（§4.2） |
| `esync_assoc_iou` | 对应物是 `tracking_iou_threshold`（**scope=1，Group 级**，默认 0.3），不是 AimKey 级新键 |
| `esync_self_motion_gain` | **不存在** —— 自运动项用的是 `runtime+0xC04/0xC08`（平台位移），无用户增益 |
| `esync_inflight_beta` | **不存在** —— AM 的发送环没有倍率旋钮 |
| `esync_counts_per_pixel_x/y` | 有对应键，但叫 `kalman_counts_per_pixel_x/y`，且**仅 Kalman 档**、默认 1.0 |
| `esync_vel_window_ms` | 对应物是 `tracking_velocity_sample_ms`（**scope=1**，默认 20） |

---

## 3. 瞄准主循环结构（`FUN_140067000`，2079 行）

### 3.1 调用关系（`callmap.csv`）

```
FUN_14007e5c0 → FUN_1400824e0 (1787 行, 主管线, 100 个下游)
FUN_140062910 → FUN_140067000 (2079 行, 瞄准+PID 编排, 50 个下游)
FUN_140067000 → FUN_14006e470 (预测状态机, 189 行)
FUN_1400824e0 → FUN_140089ac0 (跟踪器, 550 行)
FUN_1400824e0 → FUN_140056340 (NMS)
```

### 3.2 等待机制 —— EventSync 的**全部**含义

`FUN_1400824e0` L859-869 决定 `+0xE4A`（"事件驱动"标志）：

```c
859: LAB_140083a52:
861:   cVar9 = FUN_140015150(param_2 + 0x38,&DAT_1401f58b8);   // controllerType == "PID"?
862:   if (cVar9 != '\0') {
864:     cVar9 = FUN_140015150(param_2 + 0x30,"PID-EventSync");
865:     if (cVar9 != '\0') goto LAB_140083a84;
866:   }
867:   uVar11 = 0;
...
869:   *(undefined1 *)((longlong)param_1 + 0xe4a) = uVar11;
```

其中 `LAB_140083a84: uVar11 = 1;`（L854-855）。**`+0xE4A = 1` ⟺ 控制器是 `Scale`（L851-853）或档位是 `PID-EventSync`。**

消费者 `FUN_140067000` L271-331：

```c
271: if ((*(char *)(lVar17 + 0xe4a) == '\0') || ((char)uVar15 != '\0')) {
273:   FUN_140025f80(puVar40 + 0x1c8);              // 取 now
275:   lVar37 = *(longlong *)(puVar40 + 0x1c8) + 1000000;   // now + 1ms
...
313:   iVar13 = FUN_1401d9c74(lVar17 + 0xdf8,lVar45,uVar14);  // wait_for(1ms)
...
319: else {
320:   while( true ) {
321:     lVar37 = *(longlong *)(puVar40 + 0xd0);
322:     if (*(char *)(lVar17 + 0xe49) != '\0') { lVar37 = *(longlong *)(puVar40 + 0x100); }
324:     if (((((*(char *)(lVar17 + 0xe40) != '\0') || (*(char *)(lVar17 + 0xed6) != '\0')) ||
325:          (*(char *)(lVar17 + 0xe4a) == '\0')) ||
326:         ((*(longlong *)(lVar17 + 0xe50) != lVar37 || (*(int *)(lVar17 + 0xa78) != 0)))) ||
327:        (*(int *)(lVar17 + 0xa7c) != 0)) break;
329:     FUN_1401d9cd7(lVar17 + 0xdf8,lVar45);      // condvar wait
330:   }
331: }
```

**结论（可证）**：默认（非 EventSync）路径是 **1 ms 定时等待**（L275 `+1000000` ns），循环以 1000 Hz 跑；
EventSync 路径换成 **condvar 无限等待**（L329），只在 `+0xE50`（已消费的推理水位，L326）**推进时**才醒。
即"每个推理结果只消费一次、发一次位移"，**不在 1000 Hz 循环里复用同一帧结果**。
这与 QML 里 EventSync 的说明文字（L153-154）逐字吻合。

★ `+0xE50` 唯一写入点是 `FUN_1400824e0` L871-872（管道每接一帧就推进），
唯一读取点就是 L326 —— **全语料仅此一处**。

---

## 4. 跟踪器（`FUN_140089ac0`，550 行）

### 4.1 用到的配置偏移（**穷举**）

`+0x42`(L123) / `+0x44`(L448) / `+0x48`(L449,511,518) / `+0x4C`(L196) / `+0x50`(L370,371)。
**再无其它。** 特别地：`0x88/0x8C/0x90/0x94/0x98` 在该文件中出现 **0 次**。

### 4.2 关联：**类别相等 + IoU 最大**，没有距离门限

```c
196:   fVar39 = *(float *)((longlong)local_res20 + 0x4c);      // 初值 = tracking_iou_threshold
199:   if (((*(uint *)(local_178[0] + (uVar34 >> 5) * 4) >> ((byte)uVar34 & 0x1f) & 1) == 0) &&
200:      (*(int *)(lVar33 + 0x18 + uVar34 * 0x50) ==
201:       *(int *)(lVar19 + 0x14 + (longlong)param_5 * 0x28))) {
...
257:       fVar38 = (fVar48 * fVar43 + fVar44 * fVar46) - fVar47 * fVar37;   // IoU 分母
259:       fVar38 = (fVar47 * fVar37) / fVar38;                              // IoU
264:       if (fVar39 < fVar38) { uVar20 = uVar34; fVar39 = fVar38; }        // 严格 >
```

- 类别：track `+0x18` == detection `+0x14`，**整数严格相等**（L200-201）。
- 门限：`fVar39` 既是"当前最优"也是"准入下限"，比较用**严格 `<`**（L264）——
  IoU **恰好等于**门限不算命中。
- **没有"trackId 优先"路径，没有距离半径。** 贪心：detection 外层、track 内层，命中即置位（L411-412）。

### 4.3 生命周期

```c
448: if ((*(int *)((longlong)param_4 + 0x44) <= *(int *)(lVar19 + 0x2c + (longlong)puVar32)) &&
449:    (*(int *)(puVar32 + uVar31 * 10 + 6) <= *(int *)((longlong)param_4 + 0x48))) {
...
511: if (*(int *)((longlong)param_4 + 0x48) < *(int *)(puVar32 + 6)) break;   // 删除：max_age < misses
```

- 命中判据：`min_hits <= hits && misses <= max_age`，两个都 **`<=`**。
- 删除：`max_age < misses`（严格）。即"漏 `max_age` 帧后仍存活，第 `max_age+1` 帧删"。
- 命中：`+0x2C++`、`+0x30 = 0`（L407-409）。漏帧：`+0x30++`（L447）。
- 新建：`hits = 1`、`misses = 0`（L317-327），**没有独立的 confirmed 状态位**，每帧重算。

### 4.4 速度估计：**窗内沿用旧值，窗外重算并混合**（不是"累加后除窗"）

```c
367:   local_100 = (double)((longlong)dVar27 - *(longlong *)(lVar25 + 0x48 + lVar33)) / DAT_1401f8028;
370:   if (1 < *(int *)((longlong)local_res20 + 0x50)) { iVar17 = *(int *)((longlong)local_res20 + 0x50); }
373:   if (local_100 < (double)iVar17 / DAT_1401f8020) {
374:     fStack_194 = *(float *)(lVar25 + 0x20 + lVar33);     // 沿用 velX
375:     fVar39    = *(float *)(lVar25 + 0x24 + lVar33);      // 沿用 velY
376:   }
377:   else {
378:     local_180 = dVar14;      // 1.0
379:     local_108 = dVar15;      // 0.001
380-387:  ... clamp(local_100, 0.001, 1.0)  -> *pdVar21
389:     fStack_194 = (float)((double)(fVar43 - *(float *)(lVar25 + 0x40 + lVar33)) / *pdVar21) *
390:                  DAT_1401f4620 + *(float *)(lVar25 + 0x20 + lVar33) * fVar37;   // K1=0.25, K2=0.75
393:     *(float *)(lVar25 + 0x40 + lVar33) = fVar43;      // 记本次观测位置
395:     *(double *)(lVar25 + 0x48 + lVar33) = dVar27;     // 记本次观测时间戳
```

- 窗长 = `max(1, tracking_velocity_sample_ms)` **毫秒**（L370-373）。
- `elapsed_ms < window_ms` ⇒ **沿用旧速度，且不刷新时间戳**（所以窗口会自然过期）。
- 否则：`dt = clamp(elapsed_ms, 0.001, 1.0)`，`v = (Δp / dt) * 0.25 + v_old * 0.75`。
- ★ **K1+K2 = 1.0**，是一阶低通，不是"窗内平均"。

### 4.5 滑行期的位置外推 + 速度衰减（本项目完全没有）

```c
458:   param_5 = (double)((longlong)dVar27 - puVar32[uVar31 * 10 + 7]) / dVar16;   // +0x38, 距上次 emit
469:   local_1b0 = *pfVar1 + (float)((double)pfVar5[3] * *pdVar21);   // 位置 += v * clamp(dt)
471:   fStack_194 = pfVar5[3] * fVar13;                              // fVar13 = 0.8
472:   local_1a0 = *pfVar5 * fVar37;                                 // fVar37 = 0.95
485:   puVar32[uVar31 * 10 + 7] = dVar27;
```

每个"已确认但本帧未命中"的轨迹：位置按 `v × clamp(dt_ms,0.001,1.0)` 外推，
且发出速度按 **X ×0.95 / Y ×0.8** 衰减。

---

## 5. 预测状态机（`FUN_14006e470`，189 行）

### 5.1 尺寸权重（L107-116）

```c
107:  fVar19 = pfVar14[2];          // 检测框的【高】
108:  fVar23 = fVar4;               // fVar4 = 1.0
109:  if ((float)iVar15 < fVar19) {
110:    if (fVar19 < (float)iVar16) {
111:      fVar23 = ((float)iVar16 - fVar19) / (float)(iVar16 - iVar15);
113:    else { fVar23 = 0.0; }
```

`iVar15 = param_3+0x124`（`prediction_min_width`），`iVar16 = max(min+1, param_3+0x128)`
（`prediction_max_width`，L95-101）。

★ **权重吃的是框高 `pfVar14[2]`，而门限吃的是框宽 `pfVar14[3]`**（L124/L130 用 `pfVar14[3]`）。
★ 区间**外**（`h <= min` 或 `h >= max`）权重 = 0（`fVar23` 初值 1.0 只在区间内被覆盖；
`h <= min` 时 L109 为假 ⇒ 保持 1.0）—— **注意：`h <= min_w` 时 `fVar23` 保持 1.0，不是 0。**

### 5.2 涨落判据（L119-165）—— 一条复合 if

```c
119:  if (((param_6 != '\0') && (0.0 < fVar23)) &&
120:     ((0.0 < *(float *)(param_3 + 0x11c) || (0.0 < *(float *)(param_3 + 0x120))))) {
123:    fVar21 = DAT_1401f8110;                                  // 30
124:    if (DAT_1401f8110 <= pfVar14[3] * DAT_1401f9a0c) {      // 30 <= w*0.8
125:      fVar21 = pfVar14[3] * DAT_1401f9a0c;
127:    fVar24 = *(float *)(lVar12 + 0x18);                      // 系数 X
128:    fVar17 = fVar22 * _DAT_1401f9a20;                        // 系数Y * 1.5
129:    fVar18 = DAT_1401f8110;
130:    if (DAT_1401f8110 <= fVar19 * DAT_1401f9a0c) { fVar18 = fVar19 * DAT_1401f9a0c; }
133:    if ((fVar18 * (fVar24 * _DAT_1401f9a20 + fVar4) <=
134:         (float)((uint)(*pfVar14 - local_res20) & uVar5)) ||
135:       ((float)((uint)*(float *)(param_1 + 0xc04) & uVar5) <= fVar9)) {
136:      fVar24 = fVar24 - fVar8;              // 回落 0.2
142:    else { fVar24 = fVar24 + fVar3; }       // 爬升 0.1
```

- `param_6` = `prediction_enabled`（L119 第一项）。
- `param_3+0x11c/0x120` = `prediction_factor_x/y`（L120，任一 > 0 才进）。
- `*pfVar14 - local_res20` = **检测框中心的逐帧位移**（`pfVar14[0]` 是 x；`local_res20 = param_4` 是上一帧 x，L38）。
- `*(float*)(param_1 + 0xc04)` = **平台（自身）位移 X**，与 `DAT_1401f9aa0`(=10.0) 比较（L135）。
  Y 轴对应 `+0xc08`（L149）。
- 门限 = `max(30, 0.8×尺寸) × (1 + 1.5×当前系数)`（L133-134）。
- **`||` 后半是"落"的条件**：自身速度 `<= 10` ⇒ 回落。即"目标够快 **且** 自己在动"才涨。
- `uVar5 = _DAT_1401f4670 = 0x7FFFFFFF` 是**符号位掩码**（即 `fabs`），所以
  `(uint)x & uVar5` 就是 `|x|`。**已验证**（本轮 byte 核对）。
- ★★ **涨的完整条件（展开 De Morgan 后）**：
  ```
  涨 ⟺ |框逐帧位移| > max(30, 0.8×尺寸)×(1+1.5×系数)   且   |自身逐帧位移| > 10
  ```
  **两个都是必要条件，缺一不可。** 特别地：**目标静止（框位移 = 0）时系数恒为 0**，
  无论平台动得多快 —— 因为框位移本身就在涨的条件里。
  ⚠️ 本轮实现时我一度以为"AM 预测的是自己的滞后 ⇒ 目标不动也该有提前量"，那是**错**的。
  代码已按上式实现，`aim_tracker_test` §[7] 两条断言分别钉住这两个必要条件。

#### 5.2.1 ★★ 两轴的"尺寸"是【交叉】的（逐行核对 L121-149）

| 轴 | 门限尺寸变量 | 实际含义 | 框位移取 | 自身位移取 |
|---|---|---|---|---|
| **X** | `fVar18` ← `fVar19`（L107/L130） | **框高** `pfVar14[2]` | `*pfVar14`（框 x） | `+0xC04` |
| **Y** | `fVar21` ← `pfVar14[3]`（L124） | **框宽** | `fVar20 = pfVar14[1]`（框 y） | `+0xC08` |

⇒ **X 轴吃框高，Y 轴吃框宽。** 这是原文的不对称，照抄。
⚠️ 本轮实现时我一度写成"X 吃宽、Y 吃高"，是**反的**；已修正。

数值后果：对 400×20 的框，X 门限 = `max(30, 0.8×20) = 30`（被下限托住），
Y 门限 = `max(30, 0.8×400) = 320`。所以**同一个目标在 X 上容易触发预测、在 Y 上极难**。

### 5.3 输出落点（L162-178）

```c
162:  fVar20 = *(float *)(param_3 + 0x11c) * *(float *)(param_1 + 0xc04) * fVar23 * *(float *)(lVar12 + 0x18);
164:  fVar22 = *(float *)(param_3 + 0x120) * *(float *)(param_1 + 0xc08) * fVar23 * fVar22;
...
167:  fVar19 = fVar6;                                            // fVar6 = 0.05
168:  if (fVar20 * *(float *)(lVar12 + 0x20) < 0.0) { fVar19 = fVar7; }   // fVar7 = 0.02
174:  fVar19 = (fVar4 - fVar19) * *(float *)(lVar12 + 0x20) + fVar19 * fVar20;
177:  *pfVar14 = fVar19 + *pfVar14;                              // ★ 就地改写框中心 X
178:  pfVar14[1] = *(float *)(lVar12 + 0x24) + pfVar14[1];
```

★★ **提前量 = `用户系数 × 平台位移 × 尺寸权重 × 系数`，就地加到框中心上。**
不是"加到瞄点"，也不是"乘以目标速度"——两项都是**平台位移（自己的移动）**，
`runtime+0xc04/0xc08`。目标速度不参与输出，只参与"涨不涨"的判据。
状态住在**每条轨迹**上（`lVar12 = *(plVar11)` 由 `FUN_140062bf0` 按 `pfVar14+6`(trackId) 哈希取得，L104-105）。

#### 5.3.1 ★★ 低通混合系数：两个都是小量（不是"正常直通"）

```c
 88:  fVar6 = DAT_1401f5b7c;     // = **0.05**   正常混合
 87:  fVar7 = DAT_1401f9a00;     // = **0.02**   方向翻转时的混合
167:  blend = fVar6;
168:  if (new * prev < 0) blend = fVar7;
174:  prev = (1 - blend) * prev + blend * new;
```

⇒ **正常时也是重低通（0.05），翻转时更重（0.02）。**
时间常数约 20 帧 —— 这解释了 AM 的提前量为什么"慢慢起来"而不是阶跃。

⚠️ **本轮实现时我一度写成"正常 = 1.0 直通、翻转 = 0.02"**，方向完全反了。
真值 `DAT_1401f5b7c = 0.05` 已 byte 核对。`kPredNormalBlend = 0.05` 已加进
`aim_tracker.h`，并由 `aim_tracker_test` §[10] 钉死。

★ **没有硬像素上限，没有速度噪声门，没有 dt 除法** —— 全函数**唯一的除法**是 L111 的尺寸权重的分母。

### 5.4 滑行外推（`FUN_140089ac0`，漏检期间的位移外推）

轨迹在 `misses > 0` 时**继续按速度外推位置**，并带两轴不对称衰减：

```
pos += v * clamp(dt_s, 0.001, 1.0)
vel_x *= 0.95      // X 衰减慢
vel_y *= 0.80      // Y 衰减快
```

（常数 `DAT_1401f9a10 = 0.95` / `DAT_1401f9a0c = 0.8`。）

★★ **这个外推结果是"喂给下游的框心"**，不是内部状态而已 —— 也就是说目标被遮挡的那几帧，
AM 的准星**继续跟着预测轨迹走**，而不是停在最后一次观测上。

⚠️ 本项目的消费点：`boss_aim.cpp` 在 `lt->misses > 0` 时把
`(out_cx - 观测框心)` 加到瞄点上（`out.coast_consumed` / `coast_offset_x/y` 是遥测）。
**只算不消费 = 死代码** —— 这一类"算出来了但没人用"的移植缺口必须专门验，
`aim_tracker_test` §[9.5] 钉住外推量本身可区分（> 1px）且静止目标恒为 0。

---

## 6. 在途补偿（"发送环"，`FUN_140067000` L1170-1209）

```c
1187:  dVar56 = dVar56 + (double)(int)plVar33[1];              // 环内累加 X 计数
1188:  dVar57 = dVar57 + (double)*(int *)((longlong)plVar33 + 0xc);   // 环内累加 Y 计数
1198:  dVar50 = *(double *)(puVar40 + 0x528);
1199:  if (*(double *)(puVar40 + 0x528) <= DAT_1401f8000) { dVar50 = DAT_1401f8000; }   // 夹到 0.001
1202:  dVar51 = *(double *)(puVar40 + 0x520);
1203:  if (*(double *)(puVar40 + 0x520) <= DAT_1401f8000) { dVar51 = DAT_1401f8000; }
1208:  *(double *)(puVar40 + 0x28) = dVar57 / dVar50;
1209:  *(double *)(puVar40 + 0x20) = dVar56 / dVar51;
```

- `0x520/0x528` = `kalman_counts_per_pixel_x/y`，**这一处确实是除法**（L1208/1209），
  且 L1199/1203 用 `DAT_1401f8000`(0.001, **double**) 做下限夹取。
- 环形缓冲窗口由 `0x530` 决定（L1183/1185 用它与时间戳算边界），
  `0x530` 在 L234 初始化 = **8**，在 L368 被 `0xEB8`(`kalman_mouse_effect_delay_ms`) 覆盖。
- `0x520/0x528` 的**唯一写入**是 L366/367 —— 一次 **48 行连续 struct copy**（L350-L397）
  的一部分，源是 `lVar17 = *param_1`（配置结构体）。**所以它是"每帧把配置拷进来"，不是"用配置算"**
  —— 但这不矛盾：拷贝之后 L1208/1209 确实拿这个槽位当除数。

**判定（已逐行确认）**：这条链**真的被消费**（L1208/1209 是真实除法，不是搬运），
所以"AM 完全没用 k̂"是**错的**。默认 1.0 使之为恒等，但机制真实存在。

### 6.1 这条链的**档位门控**（★ 极易读错，已逐行核实并**推翻两份互相对立的错误结论**）

守卫在 L1130：

```c
1130: if (puVar40[0x44] == '\0') {      // 重置臂: L1131-1170, 清空环形缓冲
1170: }
1171: else {                            // 消费臂: L1171-1234, 含 L1187-1189 累加 + L1208-1209 除法
```

**`puVar40` 声明为 `undefined1 *`（L51），所以所有下标都是字节偏移。**
这一点是本节的**全部难点** —— Ghidra 在同一函数里混用了两种写法：
`puVar40[0x44]`（按 `undefined1*` 缩放 ⇒ 字节 `+0x44`）
与 `*(undefined8 *)(puVar40 + 0x520)`（字形上是 `+0x520`，也是字节 `+0x520`）。两者一致，无歧义。

#### 关键：`cVar41` 被**复用**，同一个名字在不同位置是**不同字段**

| 位置 | `cVar41` 实际含义 | 来源 |
|---|---|---|
| L576 / L593 / L596 | **档位标志** | 见下表 |
| **L803** | **Scale 标志**（被覆盖！） | `cVar41 = puVar40[0x41];` |
| L1684 / L1752 | EventSync 标志 | `cVar41 = puVar40[0x40];` |

★ 我最初把 L1103 的 `if (cVar41 == '\0')` 当成档位判断 —— **错了**。
L1103 在 L803 之后，读的是 **Scale 标志**（`puVar40[0x41]` 只在 L587 的 Scale 分支被置 1）。
**只有 L1130 的 `puVar40[0x44]` 才是档位标志。**

#### `puVar40[0x44]` = 档位标志的真值表

写入点唯一：L596 `puVar40[0x44] = cVar41;`。`cVar41` 在该点前的赋值链：

| 行 | 分支 | 值 |
|---|---|---|
| L568-569 | profile 长度 != 10 → `LAB_140067dc4` | `'\0'` |
| L593 | `Scale` 控制器分支 → `LAB_140067dc4` | `'\0'` |
| L572-576 | profile 长度 == 10 **且不匹配 `"PID-Kalman"`** | `'\x01'` |

档位字符串长度：`PID-FrameSync`=13、`PID-EventSync`=13、`PID-Kalman`=**10**、`PID-Adaptive`=12、`PID-Free`=8。

- 长度 13 的两个已在 L541 / L555 单独判过，**不经过 L567 的 `!= 10` 早退**，直达 L572；
  L572 拿它们与 `"PID-Kalman"`（长 10）比较必然不匹配 ⇒ **L576 置 `'\x01'`**。
- `PID-Kalman` 匹配 L572 ⇒ `goto LAB_140067dc4` ⇒ `'\0'`。
- `Adaptive`(12) / `Free`(8) 长度 != 10 ⇒ L568 ⇒ `'\0'`。

★★ **结论：`puVar40[0x44] != 0`（走除法臂）⟺ 档位是 `PID-FrameSync` 或 `PID-EventSync`。**
Kalman / Adaptive / Free / Scale 一律走重置臂，**不做换算**。

> ★★★ **这是反直觉的，且已经害我出错两次**：键名叫 `kalman_counts_per_pixel_*`，
> QML 界面也只在「卡尔曼预测」段显示它（`当前_PID_...qml` L428-439，`visible: currentProfile === "PID-Kalman"`），
> **但真正消费它的却是 FrameSync / EventSync 这两个"非 Kalman"档，Kalman 档反而不走这条路。**
> 若按名字把它归到 Kalman 档，会**恰好接反**。
> （界面 `visible` 只是显示控制，二进制**不强制**它 —— 见 §9.7。）

### 6.2 `FUN_140067000` L366-368 所在拷贝块的边界（实测）

```
L350: puVar40[0x4c0] = *(undefined1 *)(lVar17 + 0xe48);
...
L366: *(undefined8 *)(puVar40 + 0x520) = *(undefined8 *)(lVar17 + 0xea8);
L367: *(undefined8 *)(puVar40 + 0x528) = *(undefined8 *)(lVar17 + 0xeb0);
L368: *(undefined4 *)(puVar40 + 0x530) = *(undefined4 *)(lVar17 + 0xeb8);
...
L397: (连续拷贝结束)
L398: plVar33 = (longlong *)(lVar17 + 0xf70);
```

共 **48 行**同构 `mov`，源偏移 `0xe48 → 0xe48+k` 单调，目的偏移同步 —— 整块结构体复制。

---

## 7. PID 内核

★ **更正**：`FUN_140056f10`（逐轴 PID）**在语料里有**，但不在 `funcs\` ——
它在 `ghidra_out\pid_core.txt`（46877 字节）。
**并且 `FUN_1400579f0`（双轴外层）也一直有**，同在 `pid_core.txt` L752-921 ——
见 §0.3 与 §0.5–§0.11。

### 7.1 签名与分派

`FUN_140056f10(param_1, axis, setpoint, dt)`，`axis=='x'` 选槽 0/8/0x10，否则 0x18/0x20/0x28
（`pid_core.txt` L335-356）。每帧调两次：`FUN_140056f10(param_1,0x78)` / `(param_1,0x79)`（L880-881）。
★★ 判据是 `param_2 != 'x'`，所以**任何非 `'x'` 的值都走 Y 轴**。

### 7.2 档位分派**在这里**，不在别处

字符串比较是**内联的 64 位立即数比较**（不是 `LEA` + `strcmp`），所以"按引用搜字符串"的方法**找不到它**：

| 行 | 立即数 | 还原 | 档位数值 |
|---|---|---|---|
| L429 | `0x656572462d444950` | `"PID-Free"` | `0x8` |
| L434-439 | `0x706164412d444950` + `0x65766974` | `"PID-Adaptive"` + `"tive"` | `0xC` |
| L476-481 | `0x6d6172462d444950` + `0x6e795365` | `"PID-Fram"` + `"eSyn"` | `0xD` |
| L492-497 | `0x6e6576452d444950` + `0x6e795374` | `"PID-Even"` + `"tSyn"` | `0xD` |
| L503-508 | `0x6d6c614b2d444950` + `0x6e61` | `"PID-Kalm"` + `"an"` | `0xA` |

**真正有参数差异的那条**（L462-512）：

```c
466:  if (*(int *)(param_1 + 0x1f) == 1) { dVar21 = 0.0; goto LAB_1400572bf; }
482/497/508:  LAB_140057288:
              dVar18 = (double)FUN_1401daf1a(param_4 * _DAT_1401f8030);
              dVar18 = dVar21 - dVar18;                                 // = 1 - exp(-40π·dt)
502:          dVar18 = DAT_1401f8010;                                   // = 0.3
511:  dVar21 = (dVar21 - dVar18) * *(double*)(param_1 + lVar6)
              + ((param_3 - dVar3) / param_4) * dVar1 * dVar18;         // D 项混合
```

★★★ **两处更正（本轮才发现，之前这节写错了）**：

1. `FUN_1401daf1a` 是 **`exp`**（IAT thunk + 数值判据，见 §0.11），
   所以 L482 那两行的结果是 **`1 − exp(−40π·dt)`**，
   **不是** `1 − 40π·dt`（我之前把负号丢了、当成线性式，算错了判据）。
   时间常数是 **`1/(40π) ≈ 7.96 ms`**，**不是 20 Hz**（之前这句也是错的）。
2. L511 里那个 `dVar21` **不是 D 历史**，它在上面刚被赋成 `DAT_1401f6358 = 1.0`
   ⇒ 展开是 **`(1.0 − blend)`**。见 §0.7 的警告。

★ **FrameSync / EventSync / Kalman 共用同一支（`1 − exp(−40π·dt)`）；
Adaptive / Free 走 `dVar18 = 0.3`。** 这就是"档位真的换了参数消费方式"的唯一实锤。
（`0x1401F8030` = **−125.66370614359172** = −40π，见 §1 双精度池。
★ **注意这个常数是负的** —— 把它当正数会导致整个反例推导走偏。）

`x_i_clamp`/`y_i_clamp` 的夹取在 L441-458，`x_kp_floor`/`y_kp_floor` 的种子在 L380-399，
同样按 `param_1[0x14] == 0xc`（Adaptive）分档。

### 7.3 出口

`pid_core.txt` L529-530：`return Kp*setpoint + integral + dVar21 + dVar2;` —— **P + I + D + F**。
★ `integral` 那一项在出口处**又被 `+0xFE` 独立门控一次**（见 §0.14）。

### 7.4 移植状态（2026-09-16）

已逐行移植到 **`Apotheosis/mouse/aim_pid_am.h`**（`boss::amPidAxisStep`），
回归在 **`tests/aim_pid_am_test.cpp`**（47 项全绿，`aim_pid_am_test` 目标）。
★ 该文件是**独立**的 AM 语义实现，**尚未接线进生产控制回路** ——
生产仍走 `mouse/aim_pid.h`（本项目自己的、已实测整定的控制器）。
接线与否见 §8。

---

## 8. 与本项目现有实现的偏差（**这是本文的行动价值**）

| # | 本项目现状 | AM 真值 | 证据 |
|---|---|---|---|
| 1 | 门限判据换成 **px/s**（÷dt） | 吃**每帧位移**（px），**无 dt** | §5.2 L133-134；全函数唯一除法在 L111 |
| 2 | 门限常数 `0.75` | **0.8** | §1 `0x1401F9A0C` |
| 3 | 自身速度门 `3.5057` | **10.0** | §1 `0x1401F9AA0` |
| 4 | 提前量 = 系数 × **目标**速度 × 权重 | 系数 × **平台位移**(`+0xC04/C08`) × 权重 | §5.3 L162-164 |
| 5 | 提前量加到**瞄点** | **就地改写框中心**（选靶吃改后值） | §5.3 L177-178 |
| 6 | 加了**硬像素上限 12px** | **无上限** | §5.3 无 clamp |
| 7 | 加了**速度噪声门 60px/s** | **无噪声门** | §5.2 判据只有那一条 if |
| 8 | 关联 = IoU 门限 + **80px 距离半径** | **只有类别相等 + IoU 最大**，无半径 | §4.2 L199-201, L264 |
| 9 | 速度 = **窗内累加、满窗结算** | 窗内**沿用旧值**，窗外 `0.25×新 + 0.75×旧` | §4.4 L367-392 |
| 10 | 无滑行外推 | 未命中已确认轨迹**外推位置**并衰减速度(×0.95/×0.8) | §4.5 L458-485 |
| 11 | `min_hits=3` / `max_age=5` 硬编码 + 门限 0.20 | 3 / 5 / **0.3**，且是**Group 级** `tracking_iou_threshold` | §2.1, §4.3 |
| 12 | `esync_assoc_radius_px` 等 5 个键 | **AM 无对应键** | §2.5 |
| 13 | 预测系数默认/范围 | 系数**无范围限制**（本项目夹 ±0.2 是自加） | §2.2 解析器无 clamp |
| 14 | 尺寸权重用**框宽** | 权重用**框高**，门限用**框宽** | §5.1 L107 vs L124 |
| 15 | 本项目 AGENTS.md 称"AM 的 k̂ 无法测量因此没用" | **AM 真的拿它当除数**，只是默认 1.0 ⇒ 恒等 | §6 L1208-1209 |
| 16 | 本项目把 k̂ 归到 EventSync 档且默认关（窗 0） | AM 里它由**档位**门控（FrameSync/EventSync 开），**无窗口旋钮** | §6.1 |
| 17 | 本项目已删除其余 PID 档，单档 | AM 有 5 档，且**D 项混合系数按档不同**（`1−40π·dt` vs `0.3`） | §7.2 |
| 18 | 本项目无滑行衰减 | AM 未命中已确认轨迹外推 + 速度 ×0.95/×0.8 | §4.5 |

★ 第 15/16 条最要紧：**"在双机架构下测不出来"和"AM 没有这个机制"是两件事。**
只有前者站得住；后者已被 §6 推翻。

### 8.1 ★★ 本轮移植期间【新发现】的 4 处偏差（不在上表，因为是我自己写错的）

这四处是**实现阶段逐行核对时抓出来的**，都已修正并由回归钉死。记录下来是因为
它们都属于"看着差不多、其实反了"的类型 —— 最容易在重写时再犯：

| # | 我一度写成 | AM 真值 | 证据 | 钉住它的断言 |
|---|---|---|---|---|
| N1 | X 轴门限用**框宽** | X 轴吃**框高**、Y 轴吃**框宽**（交叉） | §5.2.1 L121-149 | — |
| N2 | 正常低通混合 = **1.0（直通）** | **0.05**（翻转时 0.02）—— 两个都是重低通 | §5.3.1 `DAT_1401f5b7c` | `aim_tracker_test` §[10] |
| N3 | 位移 = `vel(px/s) × dt_ms` | `vel × dt_s`（我把它算成了 px/1000） | §5.2 L133 | `aim_tracker_test` §[6] |
| N4 | "目标静止、平台在甩 ⇒ 也该有提前量" | **不会** —— 框位移是涨的必要条件之一 | §5.2 展开式 | `aim_tracker_test` §[7] |

★ N2/N3 合起来的后果很隐蔽：**两者都让"系数永远是 0"**，而"系数 0 ⇒ 提前量 0 ⇒
与关预测逐位一致"看起来完全正常。所以一致性断言必须配**反向验证**
（打开后必须不同），否则它会慢慢变成空转 —— 这条教训 CLAUDE.md 记过，本轮又验了一次。

★ N1 有一个量纲上的意外后果：X 门限吃**框高**、Y 门限吃**框宽**，于是对
**高瘦框**（如 400×20）X 门限被 30px 下限托住、Y 门限却是 320px ——
**同一个目标在 X 上容易触发预测、在 Y 上几乎不可能**。这是 AM 原文的形状，照抄。

### 8.2 ★ "算出来了" ≠ "用上了"：滑行外推的消费点

移植期间发现的一个**结构性缺口**（不在上表，因为它是"漏接线"而不是"写错常数"）：
跟踪器把滑行外推结果写进 `out_cx/out_cy`，但瞄点计算仍然只用 `out.bbox` 派生的
`out.anchor` —— 于是外推**算了个寂寞**。

已在 `boss_aim.cpp` 补上消费：`lt->misses > 0` 时把 `(out_cx − 观测框心)` 加到瞄点上
（遥测 `out.coast_consumed` / `coast_offset_x/y`）。
回归：`aim_tracker_test` §[9.5] 钉住外推量本身可区分（> 1px）且静止目标恒为 0。

★ **教训：逐字移植的对象是"算法"还是"算法+它的消费者"，必须显式确认。**
只搬算法主体、忘了它下游那一句 `*pfVar14 = fVar19 + *pfVar14`，搬过来的就是死代码，
而且**编译通过、测试全绿、行为与移植前完全一样** —— 静默失效。

### 8.3 ★★★ PID 内核移植期间【新发现】的 5 处偏差（N5–N9）

这一批是搬 `FUN_140056f10`/`FUN_1400579f0` 时抓出来的。**其中 N5–N7 是我在
写文档时就写错的** —— 值得单列，因为它们说明"我读了一遍、以为自己懂了"有多不可靠：

| # | 我一度写成 | AM 真值 | 证据 | 钉住它的断言 |
|---|---|---|---|---|
| N5 | 档位 `PID-Free` = **0x7** | **0x8** | `pid_core.txt` L589 `CMP qword ptr [RDX+0x10], 0x8` | `aim_pid_am_test` §[1] |
| N6 | D 混合系数 = **`1 − 40π·dt`**（线性） | **`1 − exp(−40π·dt)`**；线性式在 8.3ms 下是 **−0.043** | §0.11 的 IAT thunk + 极限判据 | `aim_pid_am_test` §[4] |
| N7 | D 项 = `(D_hist − blend) × D_hist` | `(1.0 − blend) × D_hist` | §0.7：`dVar21` 在式子前被赋成 1.0 | `aim_pid_am_test` §[5] |
| N8 | 第 1 帧早退 ⇒ **整段跳过**（含积分） | 早退只跳 **D/F**；**积分照常累加**（积分代码在早退之前） | §0.7、`pid_core.txt` L465-476 | `aim_pid_am_test` §[3] |
| N9 | `+0xFD` 是**全局**积分开关 | 只在 **Adaptive 档**有效；非 Adaptive 档完全无效 | §0.14 | `aim_pid_am_test` §[9] |

★★★ **N5 的成因值得记下来**: Ghidra 把 `CMP <imm>, 0x8` 渲染成了 C 伪代码里一个
**denormal double `3.95252516672997e-323`**，而那个字面量印出来的位模式还是
`0x...07`（**印错了**）。我照抄了伪代码 ⇒ 得到 0x7。
⇒ **凡是对"枚举槽位"的比较，一律回反汇编读 `CMP <imm>`，不要信 C 伪代码里的浮点常量。**

★★ **N6 我一共错了两次**（同一个毛病）：先丢掉常数的负号、算出"线性式 = 1.043 > 1"
这个**错误的反例**；再在测试里把这个错误结论写成断言。真值是"线性式 = −0.043 < 0"，
而且**最有力的判据不是符号而是极限**：`1 − e^{c·dt} → 0` 而 `1 + c·dt → 1`
（`dt = 0.1ms` 时 0.0125 vs 0.9874，差 79 倍）⇒ 线性近似在此处**根本不是近似**。
⇒ **常数带符号时，把符号当作式子的一部分抄；写完反例要回头看它是否自洽。**

★★ **N8/N9 的共性**: 都是"把**有条件的**代码当成**无条件**的读"。
原版的早退与闸门都嵌在 `if (Adaptive)` / `if (frame_index==1)` 的**内部结构**里，
跳过任何一层嵌套就会读成一条更简单的规则 —— 而那条更简单的规则**往往也能跑**。

★ **N5–N9 全部由 `tests/aim_pid_am_test.cpp`（47 项）钉住**，
且每条都配了**反向验证**（"关掉时必须不同"），避免"一致性断言慢慢空转"。

### 8.4 ★★ PID 内核的接线状态（**尚未接线，这是有意的**）

`Apotheosis/mouse/aim_pid_am.h` 是 **AM 语义的独立实现**，回归全绿，但
**还没有接进生产瞄准回路** —— 生产仍走 `mouse/aim_pid.h`。

理由（必须写清楚，否则下一个人会以为忘了接线）：
1. **AM 的 PID 内核吃的是"setpoint"**（目标位置），而本项目的控制器吃的是
   **误差**，两者的状态块布局、量纲、输出含义都不同。`FUN_1400579f0` 的外层
   还要先做**按帧比例的增益插值**与**三值取中位数的输出夹取**，那两层依赖
   AM 自己的"帧计数器 / 总帧数 / 幅度上限"三个量 —— 本项目目前**没有**对应的
   配置项与生命周期。
2. **生产控制器已经用实机数据整定过**（`kAimDeadTimeS = 46ms`、`inflight_beta = 1.6`
   的测量点、`Kp × s_max ≲ 52` 的工程上限）。直接替换会**一次性废掉**这些定标，
   而 AM 的默认档位参数本项目**没有量过**。
3. 用户的要求是**"都搬过来"** ⇒ 内核**已经搬完且可验证**；
   接不接线是**另一个决定**，需要单独做（并且应当可回归地对比两种控制器）。

★ **所以现在的状态是**: AM 的 PID 内核 **100% 搬完、47 项回归钉死**，
但它是**可选件**而不是现任件。任何"把它接上去"的改动都必须
① 先说清上面三个依赖怎么满足 ② 保留 `aim_pid.h` 的对照路径 ③ 用同一份
假游戏数据跑两者对比，而不是直接替换。

---

## 9. 明确未确定 / 属推断的部分

1. **`prediction_factor_x/y` 的取值域**：解析器（`anchors.txt` L14067-14068）直接调
   `FUN_1400177b0(key, default)`，**未见 clamp 调用**。本项目夹到 ±0.2 是自加的安全阀。
2. ~~**`FUN_1400579f0`（双轴 PID 外层）** 仍缺函数体~~ → **已解决**：完整伪代码在
   `pid_core.txt` L752-921。见 §0.3。（**上一轮"缺失"的结论是错的，已更正**）
3. ~~**`FUN_140089660`/`FUN_140089800`/`FUN_140089290`/`FUN_1400552e0`** 缺函数体
   ⇒ **轨迹排序/并列打破规则无法确定**~~ → **已解决，且结论完全相反**：
   这四个是 **C++ 标准库容器/工具函数**(`vector<Track>::insert`、`vector<bool>` 构造、
   算术辅助、同形状的容器增长)，**里面没有任何选靶/排序逻辑**。见 §0.12。
   ★★ **但"这四个不是选靶"不等于"没有选靶"** —— 真正的选靶打分器是
   `FUN_140088ab0`，见 **§0.15**。
   ★★ 教训：**"callmap 里有边但 funcs/ 里没文件"不等于"这函数有算法"** ——
   它也可能只是模板实例化。看到 `0x6666666666666667` + `/0x50` 就该停手。
4. **轨迹结构体布局有不明处**：IoU 块读 `+0x04/+0x08/+0x0C/+0x10`，
   而写入块（L398-402）写同一组；但新建路径（L317-321）把 trackId 放在 `+0x00` 低 dword。
   该结构存在类型双关，**本文不断言单一干净布局**。
   ★ 2026-09-16 补充：`FUN_140089290` 确认结构体大小**恰为 `0x50`**（容器步长），
   与 §4 一致 —— 大小可信，**内部字段仍有双关**。
5. `ANALYSIS_v1030.md` 说 `DAT_1401f4620` 是 `flick_damping` —— 那是常量池共享
   （它同时是 `flick_damping` 的默认值 `0.25`），**不代表语义**。
6. `ghidra_out\trigger_test.txt` L125 把 `DAT_1401f462c` 报成 `4.88e-4`，
   是因为按 f64 读了跨越 `0x3F000000(0.5)`+`0x3F400000(0.75)` 的 8 字节。**f32 真值是 0.5。**
7. **QML 的 `visible` 不构成门控**：`FUN_14001ba90` 对 `x_kp_floor`/`i_gate_radius`/
   `smooth_*` 等的读取**无档位判断**（L525-526, 636-637, 781-788）。
   QML 侧另有**独立的第二层 clamp**（`carved_0036B358.qml` L30-33 `clampValue()`
   在 `commitValue()` 里、`Config.setValue` **之前**执行），
   所以 `from:`/`to:` 是**每次编辑都强制**的，不只是滑块范围。
8. `kalman_prediction_strength` / `kalman_warmup_frames` / `kalman_mouse_effect_delay_ms`
   **未见算式消费点**，但可能落在 `.themida` 未脱壳区 —— **不断言为死键**。
9. `PID-Positional` / `PID-Incremental` / `PID-VelocityD` 是无引用遗留字面量。
   `frame_sync`/`event_sync`/`kalman` 是锚点抽取的**分词产物**，不是真键。
   ★★ **2026-09-16 更正**：这句只说明"这些**字符串**没被引用"，
   **不代表档位不存在** —— 档位是**数字枚举**（`0x8/0xA/0xC/0xD`，见 §0.3）。
   已确证的四个数值见 §0.3；其余数值（0–7, 9, 0xB）**在本份语料里没有比较点**，
   不得臆造。
10. ★★★ **`FUN_140088ab0` 的打分公式仍未破解**（2026-09-16 新增）：
    已确证"谁是打分器"与"两个权重键在哪被读、作为第 5/第 6 个浮点参数传入"，
    但 **`L714` 里两个乘数各自绑哪个参数未定** —— 见 §0.15 的详细说明。
    **在溯源清楚之前不许照抄那一行。**
11. `center_scoring_weight` **是死键**：`anchors_v1030.txt` L6 列了它，
    但镜像里**没有这个字符串**（0 处引用），QML 侧也没有 ⇒ 无消费者。
    （`size_scoring_weight` / `distance_scoring_weight` 则**确有**消费者。）
12. **`dump\` 与 `dump\manifest.csv` 在本份语料里不存在**（`INDEX.md` §1 列了它们，
    但 v1030 下只有 `funcs/`、`ghidra_out/`、`qml/`、`qml_z/`、`qml_compressed/`、`unpacked/`）。
    本节全部结论**不依赖 dump**，只依赖 `funcs/`、`ghidra_out/`、`qml_z/` 与解包镜像。

---

## 10. 复现方式

### 10.1 环境（**2026-09-16 已装好，不再是"需重装"**）

| 组件 | 位置 | 备注 |
|---|---|---|
| JDK 21 | `C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot` | `winget install Microsoft.OpenJDK.21` |
| Ghidra 12.1.3 | `C:\Users\Administrator\_re_tools\ghidra_12.1.3_PUBLIC` | zip = `ghidra_12.1.3_PUBLIC_20260817.zip`（**注意日期是 20260817**，不是 20260213） |
| Ghidra 工程 | `C:\Users\Administrator\_re_tools\ghproj`，工程名 `AimMagic` | 程序 `/AimMagic_unpacked.exe`，已做完整分析（约 296s） |

### 10.2 补反编译（headless）

★★ **必须设 `JAVA_HOME`**，否则 Ghidra 起不来：

```powershell
$env:JAVA_HOME = "C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot"
Set-Location "C:\Users\Administrator\_re_tools\ghidra_12.1.3_PUBLIC\support"
$sp = "<repo>\scripts\ghidra_scripts"
& .\analyzeHeadless.bat "C:\Users\Administrator\_re_tools\ghproj" AimMagic `
    -process AimMagic_unpacked.exe -noanalysis `
    -scriptPath $sp -postScript ForceDecompile.java <outFile> "<VA1,VA2,...>"
```

可用的脚本（都在 `scripts\ghidra_scripts\`）：

| 脚本 | 用途 |
|---|---|
| `DecompileAt.java` | 按地址反编译（已识别的函数） |
| **`ForceDecompile.java`** | **★ 先 `createFunction` 再反编译** —— 用于 Ghidra **没识别成函数**的地址 |
| `DisasmWindow.java` | 从一个地址起反汇编 N 条，自动解析 RIP 相对寻址并渲染字符串 |
| `RefsTo.java` | 谁的代码引用了某个地址（**找配置键消费者的主力工具**） |
| `ResolveThunk.java` | 追 IAT thunk 链（判断某地址是不是导入函数） |
| `DumpBytes.java` | 原始字节 + 指针识别 |
| `FuncSig.java` | 打印 Ghidra 恢复出的函数原型/调用约定 |
| `DecompileAnchors.java` | 批量反编译锚点 |

★★ **两个踩过的坑**:
1. `cmd /c "... 2>&1"` 会报 `2>&1 was unexpected at this time.` —— 直接在 PowerShell 里
   用 `& .\analyzeHeadless.bat ... 2>&1 | ...`。
2. Ghidra 脚本的 **stdout 在 PowerShell 里是乱码，但写出来的文件是正确的 UTF-8** ——
   看输出文件，别看控制台。
3. ★★ **Ghidra 可能不把某个地址识别成函数**（栈上有 `funcs\` 文件但这里没有）。
   这时必须用 `ForceDecompile.java`（它先 `disassemble` 再 `createFunction`）——
   §0.12 的 `FUN_140089290`/`FUN_1400552e0` 就是这么补出来的。

### 10.3 常量复核（**不依赖 Ghidra**，只用解包镜像）

★★ **所有 PE 节的 VA 都等于 RawPtr**，所以 **文件偏移 = VA − 0x140000000**：

```powershell
$exe  = "...\v1030\unpacked\AimMagic_unpacked.exe"
$base = 0x140000000
$b    = [System.IO.File]::ReadAllBytes($exe)
# f32 @ 0x1401F9A0C
[BitConverter]::ToSingle($b, [int](0x1401F9A0C - $base))   # => 0.8
# f64 @ 0x1401F8000
[BitConverter]::ToDouble($b, [int](0x1401F8000 - $base))   # => 0.001
```

★ **注意量纲**：`0x1401F8000/8010/8018/8020/8028/6358/8030` 必须按 **double** 读，
按 float 读会得到 `0`、denormal 或负值。

★★★ **两个必须避开的坑（本轮都踩过）**:
1. **PowerShell 里写 `0x1401F8030 - 0x140000000` 会被当 Int32 溢出** ——
   必须先 `[int64]`：`[int]([int64]0x1401F8030 - 0x140000000)`。
2. **不要信 Ghidra C 伪代码里的浮点字面量**：它把 `CMP <imm>, 0x8` 渲染成了
   `3.95252516672997e-323`（一个 denormal double），**而且印出来的位模式还是错的**。
   ⇒ **枚举槽位的比较，一律回反汇编读 `CMP <imm>`。**（这正是 §8.3 的 N5。）

### 10.4 找"某个配置键在哪被消费"的标准流程

**这是本轮最有用的一条方法**（§0.15 就是靠它找到真选靶函数的）：

1. 把键名转成 ASCII，**在解包镜像里全字节搜索**，记录命中的 VA：
   ```powershell
   $pat = [Text.Encoding]::ASCII.GetBytes("distance_scoring_weight")
   # 线性扫 $b，命中处 VA = 0x140000000 + offset
   ```
   ★ 一个真键通常只有 **1–3 处**引用；**0 处 = 死键**（如 `center_scoring_weight`）。
2. 用 `RefsTo.java` 找出**哪段代码**引用了那个 VA。
3. 用 `DisasmWindow.java` 从命中点往前铺开 ~40 条指令，
   看字符串被交给谁（通常是"造 `std::string` 键 → 查表 → 读值"三步）。
4. 顺着那个调用继续往下（必要时 `ForceDecompile.java`）。

★ **反面教材**: 不要靠"哪个函数名像排序/打分"去找 —— 我这么找了一轮，
找到的四个全是 `std::vector` 容器函数（§0.12）。**字符串引用比函数名可靠得多。**
