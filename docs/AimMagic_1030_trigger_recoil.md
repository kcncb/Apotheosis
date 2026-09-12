# AimMagic 1.0.30 — 扳机（触发）与压枪/鼠标输出路径 深度分析

> 目标语料：`C:\Users\Administrator\Desktop\AimMagic_RE\v1030\`（只读参考），对照基线 `..\v108\`
> 方法：`funcs\FUN_*.txt` 反编译单文件 + `ghidra_out\anchors.txt`（grep）+ `callmap.csv`（反查调用图）
> + `qml_z\`（前端原文）+ 对 `unpacked\AimMagic_unpacked.exe` 用 capstone 直接反汇编（Ghidra 未反编译的函数）
> 约定：**【已证】**= 反编译原文可直接引用；**【推断】**= 由多组独立证据交叉得出；**【未确认】**= 见文末清单

---

# (A) 自动扳机子系统

## A.0 线程与调用顺序：这是"瞬间"的第一来源【已证】

1.0.8 报告把扳机描述为"每帧第 ⑩ 步"，1.0.30 的实际结构如下（全部有原文）：

```
FUN_1400a9790        ← 自瞄运行时守护线程（anchor: "runtime init failed", "runtime stopped: code="）
  行 132  while (stop_flag(runtime+0x1B9) == 0) {      // 主循环
  行 150      iVar9 = FUN_14007e5c0(aimWorker);        // ★ 每 tick 调用一次，不是 FUN_14007e5c0 内部循环
              }
```

`FUN_14007e5c0`（1022 行，runtime = `param_1`）是**单次 tick 的完整流水线**，内部顺序为：

| 行号 | 调用 | 作用 |
|---|---|---|
| 135-183 | 帧获取 + 找色 | `FUN_140081760` / `FUN_140080430` |
| **291** | `FUN_1400824e0(param_1, local_518, puVar14, puVar15)` | **NMS + 跟踪 + 预测 + 选靶 + 扳机命中判定**，写 `runtime+0xF3` |
| **390** | `FUN_140088130(param_1, bVar24, cVar10, local_518)` | **扳机时序状态机**（消费同一拍的 `+0xF3`） |
| 397 | `FUN_1400874e0(param_1, runtime+0xF2)` | 按 aim 状态执行动作 |
| 402 | `FUN_140088010(param_1, cVar10, uVar18)` | 自动背闪判定的时间片逻辑 |
| **404** | `FUN_140085410(param_1, puVar21+0x58, cVar10, param_1+0x96)` | **压枪（后坐力）生产者/录制器** |
| 893-907 | `iVar12 = FUN_140025250(gcfg, "max_fps", 0); if (0 < iVar12) { …忙等… }` | 帧率上限 |

`FUN_1400824e0` 内部的扳机段（行 728-753 原文）：

```c
cVar9 = puVar38[0x184];                                // 目标有效
*(char*)((longlong)param_1 + 0x271) = cVar9;
*(undefined1*)((longlong)param_1 + 0xf2) = (cVar9 && puVar38[0x910]);      // aim 决定
*(float*)(puVar38 + 0x20) = fVar54;                                        // 准星 X（判据）
cVar9 = FUN_1400886f0(puVar38 + 0x90, *(undefined8*)(puVar38 + 0x108), param_2, fVar52);
*(undefined1*)((longlong)param_1 + 0xf3) = (cVar9 != 0 && *(char*)(param_2 + 0x5a) != 0);  // ★ trigger 决定
```

- `puVar38+0x90` = 检测框向量（元素步长 **0x28**，与报告 4.0② 一致）
- `param_2` = AimKeyCfg 配置块（`FUN_14007e5c0` 行 175 的 `local_518`）
- `param_2+0x5A` = 扳机总开关类字节（**【未确认】**具体键名）

### 为什么这让手感更好/更快
**命中判定（行 740）与按键执行（行 390）在同一个 tick 内串行，中间没有任何跨帧派发、没有消息队列、没有下一个采样周期。** 1.0.8 也是同线程，但 1.0.8 的逐帧推理走 vtable（报告 2.3.1），调用点不可见；1.0.30 把它变成显式直调，于是**可以确认**"判定→执行"的延迟是 0 拍（仅同拍内几条指令）。任何"检测线程 → 队列 → 执行线程"的两段式实现都会白送 1 个推理周期（在 200 fps 下 = 5 ms），AimMagic 没有这个开销。

## A.1 时序状态机 `FUN_140088130`（RVA 0x140088130，213 行）【已证】

签名：`void FUN_140088130(longlong runtime, char aimKeyActive, char triggerArmed, longlong aimKeyCfg)`

### A.1.1 状态变量（`runtime` 上的字节）

| 偏移 | 1.0.8 | 含义 | 证据 |
|---|---|---|---|
| `+0x971` | `+0x865` | 主按下闩（1 = 左键已按下） | 行 145/174/193 |
| `+0x972` | `+0x866` | 状态子位 2 | 行 148/162/180 |
| `+0x973` | `+0x867` | 状态子位 3（释放待完成） | 行 130/163/187/195 |
| `+0x974` | **不存在** | **1.0.30 新增**：本 tick 已按下过（"下次走延迟门"闩） | 行 75/81/137/151 |
| `+0x978` | `+0x86C` | 连发计数器 | 行 164/168 |
| `+0x980` | `+0x870` | 丢失保持计时基准（ns） | 行 38-46 |
| `+0xBA0` | `+0xAA8` | 当前按下的按钮号 | 行 124-133 |
| **`+0xBA8`** | **`+0xAB0`** | **延迟门 = "下次允许动作"的绝对 ns 时间戳** | 行 63-66, 207 |
| `+0xBB0` | `+0xAB8` | 按下状态机：0=空闲 1=按下中 2=待释放 | 行 74/93/107/113/117 |
| `+0xBB4` | `+0xABC` | "正在按住"标志 | 行 18-22, 106 |
| `+0x1D8` | `+0x190` | `trigger_down`（对外状态） | 行 129/146/175/194 |
| `+0xF3` | `+0xB3` | **trigger 决定**（上游写入） | 行 167 |
| `+0xF1` | `+0xB1` | paused | 行 29/32 |
| `+0xF5` | `+0xB5` | 另一运行态禁用位 | 行 32 |

1.0.8 ↔ 1.0.30 逐行 diff：**除 `+0x974` 与偏移平移 0x10C 外逐字相同**（`funcs\FUN_140088130.txt` vs `v108\funcs\FUN_14006f580.txt`，均为 213 行）。

### A.1.2 参数表：AimKeyCfg 的 trigger 子对象

偏移推导法（与报告 9.1 同法，且被两处独立印证）：`FUN_14001ba90` 行 963-967 依次读入
`start_delay / press_delay / end_delay / random_delay / target_lost_hold_ms` 到 `local_138/134/130/12c/128`，
而 `FUN_14001ba90` 行 1030 调 `FUN_1400216f0(local_2d8, uVar11, &local_124)`，其返回被拷进 `local_124…local_104`。
`local_124` 与 `local_128` 相邻 ⇒ 结构偏移 = `0x2A8 - X` 再加上 trigger 类起点 `0x60`：

```
0x2A8 - 0x128 + 0x60 = 0x1E0   ← target_lost_hold_ms
0x2A8 - 0x12C + 0x60 = 0x1DC   ← random_delay
0x2A8 - 0x130 + 0x60 = 0x1D8   ← end_delay
0x2A8 - 0x134 + 0x60 = 0x1D4   ← press_delay
0x2A8 - 0x138 + 0x60 = 0x1D0   ← start_delay
0x2A8 - 0x13C + 0x60 = 0x1CC   ← is_continuous_burst
0x2A8 - 0x140 + 0x60 = 0x1C8   ← status
```

**独立印证**：`FUN_140088130` 只读取 `param_4+0x169/16A/16B/16C/16D/170/174/178/17C/180`，
即 **0x10C + 0x60 = 0x16C** 起点（1.0.8 是 `+0x109……0x120`，差正好 0x10C）：
`status(0x169) / is_right_click(0x16A) …`。

| 结构偏移 | 键（QML） | 类型 | 默认 | QML 范围 | 含义 |
|---|---|---|---|---|---|
| `+0x169` | `trigger.status` | bool | false | — | 扳机总开关（= 触发模式 index ≥ 1） |
| `+0x16A` | `trigger.is_right_click` | bool | false | — | 开镜方式 = 点按右键 |
| `+0x16B` | `trigger.is_long_press_right` | bool | false | — | 开镜方式 = 长按右键 |
| `+0x16C` | `trigger.is_continuous_trigger` | bool | false | — | 连点/持续/智能持续 |
| `+0x16D` | `trigger.is_smart_continuous_trigger` | bool | false | — | 智能持续 |
| `+0x16E`? | `trigger.is_continuous_burst` | bool | false | — | 连点（burst） |
| `+0x170` | `trigger.start_delay` | int | **0** | 0–5000 | 触发起始延迟（仅"长按"分支用） |
| `+0x174` | `trigger.press_delay` | int | **0** | 0–5000 | 触发按下延迟（"点按/连点"分支用） |
| `+0x178` | `trigger.end_delay` | int | **0** | 0–5000 | 触发结束延迟（松手后） |
| `+0x17C` | `trigger.random_delay` | int | **0** | 0–5000 | 随机抖动**上限**（见 A.2） |
| `+0x180` | `trigger.target_lost_hold_ms` | int | **20** | 0–1000 | 智能持续：丢失后保持射击时长 |

> `+0x16E` 一栏标 `?` 是因为我把它定位在 `is_continuous_burst`（由行 962 的读序推得），但
> `FUN_140088130` 不读 `+0x16E`（它用 `+0x16B/16C/16D`），故未直接印证 → 见【未确认】。

**QML 原文**（`qml_z\触发模式选项__双语_.qml`）：
```js
triggerModes: [ "None"/无, "Click"/智能连点, "Burst"/连点, "Continuous"/持续, "Smart Continuous"/智能持续 ]
fireModes:    [ "None"/无, "Click Right"/点按右键, "Hold Right"/长按右键 ]
```
状态机行 67-72 求出的 `cVar8` 与此一一对应（`0=无, 1=智能连点, 2=连点, 3=持续, 4=智能持续`）。

### A.1.3 控制流分解（原文逐段）

**① 未武装**（行 17-28）：
```c
if (triggerArmed == 0) {
    if (runtime[0xBB4]) { if (!paused && mouse) FUN_1400774e0(runtime, 1, 0); runtime[0xBB4] = 0; }
    *(u32*)(runtime+0xBB0) = 0;  *(u64*)(runtime+0xBA8) = 0;   // ★ 清延迟门
    FUN_14007d080(runtime);                                     // 全状态复位
    return;
}
```

**② 松手/丢失保持分支**（行 29-59）：`aimKeyActive == 0 || paused`。分两种开火方式：
- `长按右键 && !连点 && !连发`（行 30-51）：`bVar2 = true` ⇒ **行 53 直接 return，即按钮保持按住不释放**。
  - 例外（行 34-47，"智能持续"）：`is_continuous_burst && != paused && target_lost_hold_ms > 0 && runtime[0x971]`
    ⇒ 用 `runtime+0x980` 做窗口判定；`now - baseline < hold_ms*1e6` 时 `return`（**继续射击**），否则落到 `LAB_1400885d0` → `FUN_14007d080`（释放）。
- 其他（点按右键/连点）：`bVar2 = false` ⇒ `LAB_1400885d0` → 立即 `FUN_14007d080`（松手）。

**③ 延迟门**（行 62-66）：
```c
FUN_140025f80(local_18);                       // clock_gettime → ns
lVar5 = *(longlong*)(runtime + 0xBA8);
if (lVar5 != 0 && local_18[0] != lVar5 && local_18[0] < lVar5) return;   // ★ 未到点 → 本拍什么都不做
```

**④ 开火方式解码**（行 67-72）：
```c
if (is_long_press_right /*0x16B*/ == 0) {
    cVar8 = (is_right_click /*0x16A*/ != 0);
    if (is_right_click != 0) goto LAB_14008820c;     // cVar8 = 1
} else { cVar8 = 2; goto LAB_14008820c; }
```

**⑤ 按下闸门 + 首枪**（行 74-108）——**这是"瞬间"的核心**：
```c
if (runtime[0xBB0] == 0) {                        // 空闲态
  if (runtime[0x974] == 0) {                      // 本 tick 未按下过
    if (is_long_press_right == 0) { cVar3 = runtime[0x971]; goto L239; }
  } else { cVar3 = runtime[0x971]; if (is_long_press_right == 0) {
L239:        if (cVar3 || runtime[0x972]) goto LAB_1400882f2;
             cVar3 = runtime[0x973]; }
    if (cVar3 == 0) {                             // ★ 三个状态位全空 = 目标刚进入
      if (cVar8 == 1) {                           // 点按右键
        FUN_1400774e0(runtime, 1, 1);             // ★★ 立即按下左键，无任何等待
        *(u32*)(runtime+0xBB0) = 1;
        iVar4 = FUN_140079980(start_delay@+0x170, random_delay@+0x17C);
        iVar6 = 0x1E;  if (0x1E < iVar4) iVar6 = iVar4;      // floor 30
        goto LAB_140088507;                       // → 写 +0xBA8
      }
      FUN_1400774e0(runtime, 1, 1);               // 长按：立即按下
      runtime[0xBB4] = 1;  *(u32*)(runtime+0xBB0) = 2;
    }
  }
}
LAB_1400882f2:
if (runtime[0xBB0] == 1) { FUN_1400774e0(runtime, 1, 0); *(u32*)(runtime+0xBB0) = 2; }
```

⚠ 注意 `cVar8 == 1`（点按右键）分支里 **按下是无条件的**，`start_delay` 只在之后被用来安排
**下一次**允许动作的时间（`+0xBA8`）。

**⑥ 连发/持续按下**（行 161-177）：
```c
if (runtime[0x971] == 0) {                        // 未按下
    if (runtime[0x972]) goto L4a2;  if (runtime[0x973]) goto L499;
    if (*(int*)(runtime+0x978) < 1) return;       // 连发计数器未开
    if (runtime[0xF3] == 0) { *(u32*)(runtime+0x978) = 0; return; }   // 目标离开 → 关计数器
    FUN_1400774e0(runtime, 0, 1);                 // 按右键（buttonIndex = 0）
    *(u16*)(runtime+0x971) = 0x101;               // +0x971=1, +0x972=1
    runtime[0x1D8] = 1;
    uVar1 = press_delay@+0x174;
}
...
iVar4 = FUN_140079980(uVar1, random_delay@+0x17C);
if (is_long_press_right && is_continuous_burst) iVar6 = 10;      // 行 199-201
if (iVar6 < iVar4) iVar6 = iVar4;
LAB_140088507:
*(longlong*)(runtime + 0xBA8) = (longlong)iVar6 * 1000000 + local_18[0];   // ★ 门 = now + ms*1e6
```

**⑦ 松手**（行 181-196）：`FUN_1400774e0(runtime, 0, 0)`；`+0x971=0; +0x1D8=0; +0x973=1`；
延迟用 `end_delay@+0x178`。

### A.1.4 "首枪能不能在第一帧就打出去？"

**能，而且这是设计意图。** 三重保证（逐条有原文）：

1. **延迟门是"下次允许"而非"本次必等"**：`+0xBA8` 只在动作**之后**被写（行 207 是唯一写入点之一，
   另一处在行 146-150 附近的"延迟后按下"路径）。门初值 0（行 25 清 0、行 7 由 `FUN_14007d080` 清 0），
   行 64 的条件 `lVar5 != 0` 让初值 0 直接放行。
2. **`start_delay / press_delay / end_delay / random_delay` 默认全 0**（QML `defaultValue: 0`），
   而 `FUN_140079980(0,0)` **返回 0**（见 A.2）⇒ 门被设为 `now + 0` ⇒ **下一拍立刻也放行**。
3. **进入"新一段目标"时状态位全空**（`+0x971=+0x972=+0x973=0`），且 `FUN_14007d080`
   （行 4-21）会把 `+0xBA8/+0x973/+0x978/+0x980/+0x972` 全清、必要时释放按钮 ⇒
   **每次目标重新进入触发区，延迟门都是 0**。

⇒ 结论：**"目标进入触发框" → 同一 tick 内 `FUN_1400824e0` 写 `+0xF3=1` → 同一 tick 内
`FUN_140088130` 走"按住/点按"分支 → `FUN_1400774e0(runtime,1,1)` 立即按下左键。第一枪的
算法延迟 = 0 拍，只受"推理结果本身有多新"约束。**

### A.1.5 唯一的硬下限：30 ms

```c
iVar6 = 0x1E;                       // 行 96, 1.0.8 行 96 同值
if (0x1E < iVar4) iVar6 = iVar4;
```
- 位置：*点按右键* 模式的按下路径（行 94-101）。
- 含义：`+0xBA8 = now + max(30, delay_fn(start_delay, random_delay)) * 1e6`
  ⇒ **该模式下两次按下之间至少 30 ms ⇒ 上限约 33.3 次/秒**。
- 另有 `iVar6 = 10`（行 200）用于 `长按右键 + 连发` 组合。
- **注意**：30 是**间隔地板**，不是"首枪延迟"。

## A.2 随机延迟：`FUN_140079980` 是 mt19937 均匀分布【已证，需自行反汇编】

Ghidra **没有**反编译这个函数（`funcs\` 无此文件，`anchors.txt` 只在调用点出现）。
我用 capstone 直接反汇编 `unpacked\AimMagic_unpacked.exe`（image base 0x140000000，RVA==文件偏移）：

```
140079980  mov [rsp+8],rbx / push rdi / sub rsp,0x20
14007998f  xor edi,edi ; test ecx,ecx ; cmovg edi,ecx     ; edi = max(0, arg1)
140079996  xor esi,esi ; test edx,edx ; cmovg esi,edx     ; esi = max(0, arg2)
14007999d  test edi,edi ; jle 0x140079a58                 ; arg1 <= 0 → return edi (=0)
1400799a5  test esi,esi ; jle 0x140079a58                 ; arg2 <= 0 → return edi  ★
1400799ad  mov ecx,[rip+0x6c0a69]                         ; TLS 索引
1400799b3  mov rax,gs:[0x58] ; mov rdx,[rax+rcx*8]        ; TLS 槽
1400799c0  mov ecx,0x1598 ; mov eax,[rcx+rdx]             ; 惰性初始化标志
1400799d0  test al,1 ; jne 0x140079a25                    ; 已初始化则跳过
1400799d4  or eax,1 ; mov [rcx+rdx],eax
1400799da  call 0x1401d9cd1                               ; 取种子（时间/随机）
...        mov [rbx],0x270 ; imul edx,eax,0x6c078965 ...  ; ★ 经典 mt19937 种子序列 1812433253, N=624
140079a25  mov ecx,edi ; sub ecx,esi ; xor eax,eax ; cmovg eax,ecx   ; n = max(0, arg1-arg2)
140079a30  mov [rsp+0x40],eax                             ; 参数块 {a, b}
140079a34  lea eax,[rsi+rdi] ; mov [rsp+0x44],eax         ;        {min, min+max}
140079a3b  mov rdx,rbx ; lea rcx,[rsp+0x40] ; call 0x14004e700   ; ★ std::uniform_int_distribution<int>::operator()
140079a58  mov eax,edi ; ret                              ; 早退返回 arg1
```

**语义（确定）**：
```
FUN_140079980(min_ms, max_ms):
    a = max(0, min_ms);  b = max(0, max_ms)
    if (a <= 0 || b <= 0) return a;        // ★ 任一为 0 → 返回 min_ms（不再是随机的！）
    return uniform_int( [min(a,b), max(a,b)] )   // 闭区间（由 uniform_int 参数对推得）
```

调用点全部是 `FUN_140079980(delay键, random_delay)`，所以：
- `random_delay = 0`（**默认**）⇒ 返回 `delay键` 本身 ⇒ **完全确定性延迟**。
- `random_delay = N > 0` 且 `delay键 = D > 0` ⇒ 在 `[min(D,N), max(D,N)]` 上均匀取值 ——
  注意这**不是**常见的"`D + U(0,N)`"，而是 **`U(min(D,N), max(D,N))`**（闭区间、按大小排序）。
- `delay键 = 0` 且 `N > 0` ⇒ 仍返回 0 ⇒ **下限 0，无延迟**。

**未使用 `rand()`/`srand()`**（全语料 `funcs\` grep `rand()`/`srand`/`RAND_MAX` 无命中），
用的是 MSVC 标准库 `<random>` 的 `std::mt19937` + `std::uniform_int_distribution<int>`，
且引擎放在 **TLS** 里惰性构造（`gs:[0x58]` 取 TLS 数组，`0x1598` 槽），说明作者刻意避开全局 `rand()` 的竞争。

## A.3 扳机命中判定：`FUN_1400886f0`（遍历）+ `FUN_140088790`（逐框）【已证】

### A.3.1 `FUN_1400886f0(rva 0x1400886f0, 32 行)` 遍历 + 类别过滤
```c
undefined1 FUN_1400886f0(longlong *boxVec, undefined8 classCfg, longlong aimKeyCfg,
                         undefined4 aimX, undefined4 aimY)
{
  lVar4 = *boxVec; lVar1 = boxVec[1];                 // begin / end
  while (true) {
    if (lVar4 == lVar1) return 0;                     // 遍历完 → 未命中
    if ((*(longlong*)(aimKeyCfg+0x150) == *(longlong*)(aimKeyCfg+0x158) ||   // 类别白名单为空
         thunk_FUN_1401d7660(*(longlong*)(aimKeyCfg+0x150),
                             *(longlong*)(aimKeyCfg+0x158),
                             *(undefined4*)(lVar4+0x14)) != *(longlong*)(aimKeyCfg+0x158))
        && (cVar3 = FUN_140088790(lVar4, classCfg, aimKeyCfg, aimX, aimY), cVar3 != 0))
      break;
    lVar4 = lVar4 + 0x28;                             // ★ 框步长 0x28
  }
  return 1;
}
```
- `lVar4+0x14` = 框的 **classId**（报告 4.0② 已证 index 5）
- `aimKeyCfg+0x150 / +0x158` = 该 AimKey 的**类别白名单**（`std::find` 风格）
- **语义是"任一命中即返回 1"**（OR，不是"最近的"）——**1.0.8 逐字相同**（报告 4.0 已证）

### A.3.2 `FUN_140088790(rva 0x140088790)` 触发区构造（`ghidra_out\trigger_test.txt` 有全量反汇编）
```c
longlong FUN_140088790(float *box /*0x28步长*/, longlong classCfg, longlong aimKeyCfg,
                       float aimX, float aimY)
{
  // ① 按 classId=box[5] 在 classCfg+0x1A8 的 std::map<int,PerClass*>[+0x34..] 里查该类的覆盖设置
  pfVar3 = *(float**)(classCfg + 0x1a8);   pfVar6 = *(float**)(pfVar3 + 2);
  ... 红黑树下行查找，落在 pfVar11（=根/该节点）
  // ② 小目标判定（★ 关键：默认阈值几乎总是"是小目标"）
  fVar13 = box[2];  fVar1 = box[3];                       // w, h
  bVar4 = (float)*(int*)(aimKeyCfg + 0x78) < fVar13;      // small_target_threshold_x
  bVar5 = (float)*(int*)(aimKeyCfg + 0x7c) < fVar1;       // small_target_threshold_y
  // ③ 四系数
  if (bVar4) { pfVar9  = (pfVar11==root) ? (classCfg+0x198) : (pfVar11+0xd); }  // X 范围
  else       { pfVar9  = (aimKeyCfg + 0x88); }                                  // = x_trigger_scope@+0x88
  if (bVar5) { pfVar12 = (pfVar11==root) ? (classCfg+0x19c) : (pfVar11+0xe); }  // Y 范围
  else       { pfVar12 = (aimKeyCfg + 0x8c); }
  if (bVar4) { pfVar10 = (pfVar11==root) ? (classCfg+0x1a0) : (pfVar11+0xf); }  // X 偏移
  else       { pfVar10 = (aimKeyCfg + 0x90); }
  if (bVar5) { pfVar8  = (pfVar11==root) ? (classCfg+0x1a4) : (pfVar11+0x10); } // Y 偏移
  else       { pfVar8  = (aimKeyCfg + 0x94); }
  // ④ 区间
  fVar14 = (box[0] - fVar13*0.5f) + fVar13 * *pfVar10;      // left  = cx - w/2 + w*offsetX
  if (!(fVar14 < aimX && aimX < fVar13 * *pfVar9 + fVar14)) return 0;   // 开区间！(严格 <)
  fVar13 = (box[1] - fVar1*0.5f) + fVar1 * *pfVar8;         // top   = cy - h/2 + h*offsetY
  if (fVar13 < aimY && aimY < fVar1 * *pfVar12 + fVar13) return 1;
  return 0;
}
```
- `DAT_1401f462c = 0.5f`（我实测确认，见 A.5）
- **区间是开区间**（`<` 而非 `<=`）
- 触发区 = **以检测框为基准、按 `[范围]` 缩放 + `[偏移]` 平移**的子矩形；准星（`aimX,aimY`）
  必须落在其中。**不涉及时间**，时间完全交给 A.1。

### A.3.3 小目标类别覆盖逻辑（含"类置信度"）
`FUN_1400216f0`（483 行，anchor `[x_trigger_scope, y_trigger_scope, x_trigger_offset, y_trigger_offset, class_confidence, invalid map<K, T> key]`）
是**按类别的覆盖装配器**，被 `FUN_14001ba90` 行 1030（"default" 节）与行 1066（每个类别 id 迭代）调用：

```c
undefined8 *FUN_1400216f0(undefined8 *out /*param_1*/, longlong *cfgNode /*param_2*/, undefined8 *src /*param_3*/)
{
  // 先把 src 的 5 个字段整体拷成 val 语义（box 形状参数）
  ...
  if ((char)param_2[4] != 4) return param_1;                  // 不是"对象"就不是类别覆盖
  local_e8 = *(undefined4*)((longlong)param_1 + 4);
  FUN_14001fa10(&local_d8, uVar15, &DAT_1401f5038, *(undefined4*)param_1);
  *(undefined4*)param_1 = (undefined4)local_d8;               // 处理 aim_min/max 之类
  *(undefined4*)((longlong)param_1+4) = local_d8._4_4_;
  uVar15 = FUN_1400177b0(param_2, "weight", *(undefined4*)(param_1+2));   // weight
  *(undefined4*)(param_1+2) = uVar15;
  // ★ 查 "class_confidence"：str 常量 s_class_confidence_1401f5048（长度 0x10=16）
  ... 红黑树按 key 查找 ...
}
```
- `FUN_140088790` 返回的小目标系数来自**类别的 `trigger` 子对象**（`classCfg+0x198..0x1A4`），
  非小目标走 **AimKey 级 `x/y_trigger_scope`、`x/y_trigger_offset`**（`aimKeyCfg+0x88..0x94`）
- **两个坐标轴的小目标判定是独立的**（`bVar4` 用宽、`bVar5` 用高），所以可能出现
  "X 用小目标参数、Y 用按键级参数"的混合区间
- QML 默认：`small_target_threshold_x = 20`、`y = 25`（`qml_z\----_模型与跟踪…qml` 行 183-190），
  类别覆盖的 trigger 默认 `x_scope=1.0, y_scope=1.0, x_offset=0.0, y_offset=0.0`（`qml_z\瞄准参数-按键配置…qml` 行 339-366）

### 为什么这让手感更好/更快
- **默认 `scope = 1.0 / offset = 0.0` + `threshold_x = 20 / threshold_y = 25` ⇒ "小目标"分支几乎总是生效**，
  于是**每个类别可以各自定义"哪里算命中"**（例如爆头框只在头部高度内触发）——用更小的触发区换来
  更少的误开火，同时不需要牺牲自瞄的选靶逻辑。
- 判定只用 **4 次比较**（两次区间），无 sqrt、无遍历，`FUN_1400886f0` 是线性扫描且命中即 break。

## A.4 运行速率：不是独立高频线程，而是"推理事件驱动"【已证】

### A.4.1 三层线程结构
| 线程 | 入口 | 周期 |
|---|---|---|
| **自瞄运行时守护** | `FUN_1400a9790`（行 132 `while` 循环） | 每轮调一次 `FUN_14007e5c0` |
| 帧率上限（在 `FUN_14007e5c0` 行 891-908 内） | 读 `max_fps` | 见下 |
| **扳机时序** | `FUN_140088130`，由 `FUN_14007e5c0` 行 390 调用 | **= 自瞄 tick 频率** |
| **PID/Scale 控制器（= 瞄准工作线程本体）** | **`FUN_140067000`**，线程入口 `FUN_140062910` → `FUN_140067000`，由 `FUN_140077f40` 行 814 `(*DAT_1401f08e0)(0,0,FUN_140062910,param)` 创建，handle 落在 `runtime+0xD98` | **1000 Hz（非事件驱动）/ 每次推理一次（事件驱动）**；`runtime+0xDF8` 条件变量、`runtime+0xDA8` 状态锁 |
| **压枪播放（= 压枪工作线程本体）** | **`FUN_14006b630`**（RVA 0x14006b630–0x14006ba1b，1003 字节），线程入口 `FUN_1400629a0` → `FUN_14006b630`，由 `FUN_140077f40` 行 852 创建，handle 落在 `runtime+0x988`、id `+0x990` | 按弹道 `delay_ms` + `runtime+0x9E8` 条件变量唤醒 |

**关键：扳机时序状态机不在 1000 Hz 的 PID 线程里。** 它和推理在同一个 tick 上，一次一评。
（`callmap.csv` 只有一条入边：`"FUN_14007e5c0","FUN_140088130"`，第 2225 行。）

### A.4.2 帧率上限：`max_fps`（默认 0 = 不限）
```c
// FUN_14007e5c0 行 883-908（原文）
local_358 = (undefined4)DAT_1401f9578;            // ★ 我实测 DAT_1401f9578 = "max_fps"
iVar12 = FUN_140025250(uVar13, &local_358, 0);     // 读 max_fps（默认参数 0）
if (0 < iVar12) {
    lVar19 = (longlong)(int)(1000000 / (longlong)iVar12) * 1000 + param_1[0xa7];   // 目标 ns 时刻
    FUN_14003f260(puVar21 + 0x48);                                                 // 若已过期 → 直接跳过
    do { plVar17 = FUN_140025f80(puVar21 + 0x48); if (*plVar17 == lVar19) break; } while (*plVar17 < lVar19);
}
```
- `1000000/us * 1000 = 1e9/us` = 目标帧间隔（ns）；**单位是微秒**
- **忙等回旋**（`do{}while` 里反复 `clock_gettime`），不是 `Sleep` ⇒ 精度亚毫秒，但吃满一个核
- **`max_fps` 默认 0 ⇒ `if (0 < iVar12)` 不成立 ⇒ 完全不做限速**，tick = 推理自然节奏
  （QML `qml_z\基础设置页_RuntimePage…qml` 行 484-489：`max_fps` / 帧率上限 / default 0 / 0–1000）

⇒ **1.0.30 的默认运行模式就是"推理事件驱动"**：没有 1000 Hz 空转，也没有人为限速。
报告 4.0 提到的 `FUN_140067000` 行 271-317 那条 `now+1000000`（1 ms）等待只在
**非事件驱动**（普通 PID）时才走，即 `+0xE4A == 0`。

### A.4.3 观察到的频率
| 场景 | 频率 | 证据 |
|---|---|---|
| 扳机评估 | **= 自瞄 tick = 推理帧率**（不是 1000 Hz） | `FUN_14007e5c0` 行 291 → 390 同 tick |
| 自瞄 tick（默认） | 推理帧率，无限速 | 行 893 `if (0 < iVar12)`，`max_fps` 默认 0 |
| 自瞄 tick（设了 max_fps） | `max_fps` Hz（忙等，0–1000） | 行 895-907 |
| 扳机连发上限 | `1000 / max(30, press_delay)` ≈ **33 Hz**（点按右键）；`1000/10`（长按+连发） | 行 96-98、行 200 |
| **PID 非事件驱动** | **1000 Hz**（`now + 1000000` ns 定时等待） | `FUN_140067000` 行 274-313 |
| **Scale / PID-EventSync** | **每次推理 1 次**（`+0xE50` 水位比对，不等就干活） | `FUN_140067000` 行 319-331 |
| 压枪步进 | 每步 `delay_ms`（用户录制/编辑） | `FUN_140085410` 行 696-720 |

## A.5 本报告新测得的常量（capstone 直读二进制）

| 符号 | 值 | 用途 |
|---|---|---|
| `DAT_1401f9578` | `"max_fps"` | 主循环帧率上限键名 |
| `DAT_1401f4620` | **f32 = 0.25** | `flick_damping` 默认 |
| `DAT_1401f462c` | **f32 = 0.5** | 框中心→左上角系数 |
| `DAT_1401f8000` | f64 = **0.001** | PID `dt` 下限（秒） |
| `DAT_1401f8008` | f64 = **0.25** | 预测状态机 dt 上限 |
| `DAT_1401f8028` | f64 = **1e9** | ns → s |
| `DAT_1401f8020` | f64 = **1000** | ms → s |
| `DAT_1401f5ce0` | f64 = **1e6** | ns → ms |
| `DAT_1401f9a60` | f64 = **30** | `axis_lock_hold_ms` 默认 |
| `DAT_1401f9a68` | f64 = **40** | `axis_lock_reversal_threshold` 默认 |
| `DAT_1401f9a58` | f64 = **20** | `axis_lock_deadzone`… 见 B.4 |
| `DAT_1401f9b90 / 9b98 / 8040` | f64 = **30 / 40 / 2** | axis_lock X/Y 轴初值 |
| `DAT_1401f80f8` | f64 = **10** | `axis_lock_flick_hold_ms` 加载默认 |

**`axis_lock` 存的是平方值**（用于免 sqrt 的距离比较）：
`+0x688 = 30² = 900`、`+0x690 = 40² = 1600`、`+0x698 = 2² = 4`
（`FUN_140067000` 行 655-712，默认常量 `DAT_1401f9a60=30 / 9a68=40 / 8040=2`；
flick 组行 727-794 同构，但 `axis_lock_flick_hold_ms` 被 `DAT_1401f80f8 = 10` 覆写）。

---

# (B) 压枪（后坐力控制）与鼠标输出路径

## B.0 结论先行：压枪是 **(2) 录制/编辑型弹道表**，不是 (1) 也不是 (3)【已证】

| 假设 | 判定 | 依据 |
|---|---|---|
| (1) 硬编码弹道表/曲线 | **否** | 全语料 `funcs\` grep `recoil` 只命中 `FUN_140085410`（+ 线程管理），**没有任何内嵌弹道常量数组**；`enable_recoil_control`、`smart_recoil_enabled` 只存在于 anchor 串表（`anchors_v1030.txt` 第 7 行），**无任何读取点** ⇒ **死键**（旧版遗留） |
| (2) Macro / 录制型序列 | **是（数据驱动，但不是通用 Macro 引擎）** | 弹道存在 `recoil_groups/<组名>` 的**文本列表**里，每步 12 字节 `{int rel_x; int rel_y; int delay_ms}`；有独立播放线程；可录制、可导入导出 |
| (3) 仅 `axis_lock_*` 轴向锁 | **否（是另一件事）** | `axis_lock` 是**输出层的最小方向持有**（抑制高频左右反转），与"抵消枪口上跳"无关 |

> 澄清与报告第 3 节的边界：通用 **Macro 引擎**（`FUN_14002b010` 执行器 / `FUN_14002cf60` 管理器，
> 步骤 `wait/mouse_move/mouse_click/...`）是**独立的事件/热键自动化**，与压枪**不是同一条代码路径**。
> 压枪有自己的**固定格式**（`rel_x, rel_y, delay`）和**自己的线程**。

## B.1 弹道数据模型与配置键【已证 + QML 原文】

**QML**（`qml_z\默认弹道下拉模型_顶部_无默认___弹道组__与组管理下拉分离_.qml`）：
```
行 253  placeholderText: "rel_x, rel_y, delay | 每行一个点"
行 176  "每个组保存一条相对移动序列，可在下方按"一行一个点"编辑。"
行 204  "不按自瞄键直接左键时走此弹道；按了自瞄键则严格按该键绑定走（互不影响）。"
```

| 键（scope=1, subObject=`recoil`） | 默认 | QML 范围 | 含义 |
|---|---|---|---|
| `recoil_enabled` | false | — | 启用后坐力控制 |
| `gate_right_down` | false | — | 门控：右键按下 |
| `gate_aim_key_down` | false | — | 门控：自瞄按键按下 |
| `gate_target_locked` | false | — | 门控：有目标被锁定 |
| `gate_target_on_screen` | false | — | 门控：画面有目标 |
| `record_enabled` | false | — | 启用录制 |
| `record_source` | `"aim"` | aim / optical_flow / aim_color | 录制方式：自瞄移动 / **光流** / 准星找色 |
| `record_fixed_interval` | false | — | 固定采集间隔 |
| `record_sample_ms` | 20 | 1–1000 | 固定采集间隔（ms） |
| `move_mode` | `"separate"` | separate / merge | 后坐力移动与自瞄移动重叠时：单独移动 / 合并移动 |
| `default_recoil_group` | `""` | 组名 | 不按自瞄键时用的弹道 |
| `recoil_group`（AimKey 级, scope=2） | `""` | 组名 | 按了自瞄键时严格按该键绑定 |

**门控全部 AND**（`FUN_14005f510`，21 行，原文可整段引用）：
```c
bool FUN_14005f510(char *gate /*param_1*/, char *state /*param_2*/) {
  if ((((((state[5] != 0) && (state[6] == 0)) && (state[7] == 0)) &&
       ((state[8] != 0 && (*state != 0)))) && ((*gate == 0 || (state[1] != 0)))) &&
      (((gate[1] == 0 || (state[2] != 0)) && ((gate[2] == 0 || (state[3] != 0)))))) {
    if (gate[3] != 0) return state[4] != 0;
    return true;
  }
  return false;
}
```
- `gate[0..3]` = `recoil_enabled / gate_right_down / gate_aim_key_down / gate_target_locked`
- `state[0..8]` = 运行时状态（`FUN_140085410` 行 422-430 装配）：
  `[0]=puVar28[0x31]`、`[1]=*(runtime+0x271)`(目标有效)、`[2]=puVar28[0x35]`、
  `[3]=*(runtime+0x271)`、`[4]= (*(int*)(runtime+0x18C) > 0)`、`[5]=puVar28[0x32]`、
  `[6]=cVar9`、`[7]=*(runtime+0xF1)`(paused)、`[8]= (*(runtime+0x128) != 0)`
- `gate[3]`（`gate_target_on_screen`）走 `state[4]`，即"检测框数量 > 0"

## B.2 生产者：`FUN_140085410`（RVA 0x140085410，869 行）【已证】
anchor：`[aim_color, record_enabled, record_sample_ms, record_fixed_interval, record_source, gate_aim_key_down, gate_target_locked, gate_target_on_screen]`
调用点：`FUN_14007e5c0` 行 404 `FUN_140085410(param_1, puVar21 + 0x58, cVar10, param_1 + 0x96);` —— **每 tick 一次**

- **行 101-102**：`recoil_enabled = FUN_14007b0c0(cfg,"recoil_enabled",0)`；`record_enabled` 同法
- **行 107-109**：`FUN_14007bb00(...,"default_recoil_group",...)` / `FUN_140079ae0(runtime,...)` /
  `FUN_14005f570(local_a0+2, param_3, uVar17, uVar16)` —— **按"是否按了自瞄键"二选一**（`move_mode` 之外的选组）
- **行 151-163**：解析 `move_mode`，`puVar28[0x36] = (name == "merge")`
- **行 410-432**：读四个 gate 键 → 行 432 调 `FUN_14005f510`
- **行 439-447**：`cVar10 == 0`（门控关闭）时，若正在播放（`+0xA58`）则 `+0xA32=1; FUN_1401d9bcd(runtime+0x9E8)` 去停
- **行 596-720 弹道解析**（原文）：
```c
puVar32 = *(undefined8 **)(puVar28 + 0x98);           // 输出向量 end
while (puVar27 != puVar18) {                          // 遍历 config 数组元素（步长 0x28）
  if ((*(char *)(puVar27 + 4) == 5) && (2 < (ulonglong)((puVar27[1] - (longlong)pdVar6) / 0x28))) {
    *(int *)(puVar28 + 0x58) = (int)*pdVar6;          // ★ step.rel_x  (elem[0])
    *(int *)(puVar28 + 0x5c) = (int)pdVar6[5];        // ★ step.rel_y  (elem[5])
    dVar2 = pdVar6[10];
    *(int *)(puVar28 + 0x60) = (int)dVar2;            // ★ step.delay_ms (elem[10])
    ... push_back 到 puVar28+0x90 的 vector（每步 0xC 字节）...
    puVar27 = puVar27 + 5;
  } else puVar27 = puVar27 + 5;
}
```
- **行 729-807 发布**：
```c
*(undefined1 *)(puVar18 + 3) = puVar28[0xa8];        // 附带 move_mode(merge?)
LOCK(); *(undefined1 *)(param_1 + 0xa31) = 1; UNLOCK();       // ★ 就绪位
LOCK(); *(undefined1 *)(param_1 + 0xa32) = 0; UNLOCK();
LOCK(); *(undefined1 *)(param_1 + 0xa59) = 1; UNLOCK();       // ★ 播放中
FUN_1401d9bcd(param_1 + 0x9e8);                        // ★ 唤醒压枪线程
```
- **行 812-813**：`**(char**)(puVar28+0x50) = cVar10; *(param_1+0x272) = *(param_1+0xa59);`

## B.2b 录制器核心：`FUN_14005f590` 定间隔累加采样器【已证】

`FUN_140085410` 行 308 / 314 调用 `FUN_14005f590(state /*= runtime+0xAE0*/, dx, dy, elapsed_ms)`，
**这个函数就是"弹道步"生成的唯一出口**（81 行，原文）：

```c
void FUN_14005f590(int *st, int dx, int dy, int elapsed_ms)
{
  if (*(char *)((longlong)st + 5) == 0) return;      // st[0..] : +5 = 录制使能
  st[2] += dx;                                        // +0x08 累加 X
  st[3] += dy;                                        // +0x0C 累加 Y
  local_18 = st[2];  local_14 = st[3];
  iVar4 = *st;                                        // +0x00 = 采样间隔 ms
  st[4] += max(0, elapsed_ms);                        // +0x10 累加已过时间
  local_10 = st[4];
  if (iVar4 <= local_10) {                            // ★ 累计时间 ≥ 间隔 ⇒ 落一个样本
    if ((char)st[1] != 0) {                           // +0x04 = "固定间隔"模式
      local_10 = iVar4;
      if (local_18 != 0) goto emit;
      do { local_10 = iVar4;
           if (local_14 != 0) {
emit:        piVar1 = *(int **)(st + 10);             // +0x28 = 输出 vector end
             if (piVar1 == *(int **)(st + 0xc)) FUN_14005d330(st + 8, piVar1, &local_18);
             else { *piVar1 = local_18; piVar1[1] = local_14; piVar1[2] = local_10;
                    *(longlong *)(st + 10) += 0xc; } // ★ 每样本 0xC 字节 {rel_x, rel_y, delay_ms}
           }
           iVar2 = st[4];  iVar4 = *st;
           st[2] = 0; st[3] = 0;                     // 清零累加器
           st[4] = iVar2 - iVar4;                    // ★ 余数结转（不丢时间）
           local_18 = 0; local_14 = 0;
      } while (iVar4 <= iVar2 - iVar4);
      return;
    }
    if ((local_18 != 0) || (local_14 != 0)) {        // 非固定间隔：只在有位移时落样本
      ... 同上 push_back 0xC 字节 ...
      st[2] = st[3] = st[4] = 0;
    }
  }
}
```

要点：
- **累加器 + 余数结转**（`st[4] = st[4] - interval`），所以长时间录制不会累积漂移
- **固定间隔模式**（`record_fixed_interval = true`）会在一段大位移里**补发多个零位移样本**
  （`do…while` 循环里 `local_18/local_14` 被清零后再判），即"时间轴等距、位移归零补点"
- **非固定间隔模式**只在实际有位移时落点 ⇒ 样本数少、时间戳不等距
- `record_sample_ms` 默认 20，**下限被夹到 > 1**（行 192-195：`iVar11 = FUN_14007b420(cfg,"record_sample_ms",0x14); iVar12 = 1; if (1 < iVar11) iVar12 = iVar11;`），
  然后 `*(int*)(runtime + 0xAE0) = iVar12`（行 250-255）⇒ **进入 `FUN_14005f590` 的 `*st`**
- 三个录制源的取数点（行 284-331）：
  - `record_source == "aim"`（`puVar28[0x33] == 0 && !bVar34`）：
    行 314 `FUN_14005f590(piVar1, *(u32*)(puVar28+0x70), *(u32*)(puVar28+0x68), elapsed_ms)`
    —— 用**自瞄/压枪工作线程上报的位移**（`0x68/0x70` 即该线程在 `+0xAD8/+0xADC` 交换的 dx/dy）
  - `record_source == "optical_flow"`（`bVar34`）：行 285-310，`FUN_140032560(&ts, runtime+0x568)` 取**上一帧**
    与 `runtime+0x540` 的当前帧做光流（`FUN_14005df80(runtime+0xB18, runtime+0x540, framePtr, &out)`），
    再用 `(*DAT_1401f07e0)(int)`（**取绝对值**的导入函数）转成幅度
  - `record_source == "aim_color"`（`puVar28[0x33] != 0`）：行 318-331，
    用**准星找色结果与画面几何**反算：`dx = cx/2 - *(int*)(runtime+0x174)`、`dy = cy/2 - *(int*)(runtime+0x178)`
    （`*(int**)(puVar28+0x58)` 是 ROI/画布尺寸指针），即"从准星相对画面的偏移推导后坐力方向"
- `FUN_14005de40/14005de70`（行 339/368）是**录制缓冲的复位**，`FUN_1400659e0(runtime+0xA60, runtime+0xB00)` 刷新

## B.3 播放线程【已证其存在、生命周期与唤醒协议；回放循环体见【未确认】】
- 线程对象/信号量：`runtime+0x9E8`（condvar）、`+0xA88`/`+0x998`（mutex）、`+0xA30`（stop）、
  `+0xA58`（active）、`+0xA59`（playing）、`+0xA31`（轨迹就绪）、`+0xA32`（请求停）、
  `+0xA38/0xA40/0xA48`（轨迹 vector begin/end/cap）、`+0x273`、`+0xB19`、`+0xB1A`
- 线程体：`FUN_1401e0180`（anchor `"recoil worker: "`）、异常处理 `FUN_1401e0300`（`"recoil worker: unknown exception"`）
  - `FUN_1401e0180` 行 23/26：`*(char*)(*plVar1 + 0xa59) = 0;` + 取线程启动参数，行 31-57 构造
    `"recoil worker: "` 线程名后 `FUN_140032ca0(uVar2, name)` 启动
- **停止流程**：`FUN_14007d140` 行 54-75（anchor `stop_recoil_worker:before/after`）：
  `LOCK(); *(char*)(runtime+0xa30)=1; UNLOCK(); FUN_1401d9bcd(runtime+0x9e8);` 然后 join
  （与 `stop_aim_worker`（`+0xE40` / `+0xDF8`）并列，是**两条独立线程**）

## B.4 与压枪容易混淆的第三族：`axis_lock`（轴锁定）【已证参数，语义为输出整形】

**它是什么**（QML 原文，`qml_z\左列__Hardware___AxisLock_.qml` 行 169-170）：
> 方向翻转率（sign flip rate）：每秒鼠标改变方向的次数。人类一般不超过 50 次/秒，反作弊通过行为分析
> 识别异常瞄准。轴锁定用最小方向持有时间抑制目标切换/过冲时的高频反转。若手感迟滞，可减小持有时间或调高反转阈值。

| 键（scope=0 全局） | 默认 | 范围 | 含义 | 结构接收处 |
|---|---|---|---|---|
| `axis_lock_enabled` | **true** | — | 启用轴锁定 | `FUN_140067000` 行 639-645 → `puVar40[0x680]`（bool） |
| `axis_lock_hold_ms` | 30.0 | 0–200 | 最小方向持有时间 | 行 655-658 → `+0x688`（**= 900.0 = 30²**） |
| `axis_lock_reversal_threshold` | 40.0 | 0–500 | 反转放行阈值（px） | 行 678-681 → `+0x690`（**= 1600.0 = 40²**） |
| `axis_lock_deadzone` | 2.0 | 0–50 | 输出死区（px） | 行 701-712 → `+0x698`（**= 4.0 = 2²**） |
| `axis_lock_flick_hold_ms` | 10.0 | 0–200 | 甩枪持有时间 | 行 723-737 → `+0xE0`（加载默认 `DAT_1401f80f8 = 10`） |
| `axis_lock_flick_reversal_threshold` | 100.0 | 0–500 | 甩枪反转放行阈值 | → `+0xE8`（默认组 `DAT_1401f9b98 = 40`） |
| `axis_lock_flick_deadzone` | 1.0 | 0–50 | 甩枪输出死区 | → `+0xF0`（默认组 `DAT_1401f8040 = 2`） |

- **两组二选一**：`FUN_140067000` 行 30904-30912（anchors.txt 行号）
  `puVar20 = (cVar41 == 0) ? (puVar40 + 0x680) : (puVar40 + 0xD8);`，
  其中 `cVar41 = puVar40[0x41]` = 报告 2.6 的 `runtime[0x41]`（"走时间型滤波器"总开关 / 档案相关）
- **存储为平方值**是**推断**（由 30²=900、40²=1600、2²=4 三对默认值同时吻合得出），
  与 `FUN_140067000` 行 1070-1074 用 `x*x + y*y < radius*radius` 做比较的风格一致 ⇒ 免 sqrt。
  **未找到直接读取这三个平方值的消费点** → 见【未确认】

## B.4b 驱动工厂：`FUN_140040ff0` —— 10 种 `move_method`【已证】

`move_method` 是**顶层**键（scope=0），默认 `"send_input"`。QML 枚举原文
（`qml_z\左列__Hardware___AxisLock_.qml` 行 29-48）：
```js
EComboField { labelEn:"move_method"; labelZh:"移动方式"; configKey:"move_method"; scope:0
  defaultValue: "send_input"
  model: [ send_input, dhz, km_net, makcu, ferrum, probox, kmbox_bplus, catbox, cpbox, kcom5 ] }
```

**工厂函数 `FUN_140040ff0`（408 行）按 `move_method` 字符串构造驱动对象**
（全部是「先比长度、再 `memcmp`」的内联比较，`local_70` = 字符串长度）：

| QML 值 | 长度 | 工厂分支行号 | 构造出的对象 |
|---|---|---|---|
| `send_input` | 10 | **行 58-59**（`local_70 == 10 && "send_input"`，或 `local_70 == 0` 即空串 = 默认） | 见行 60-73 |
| （3 字符，`DAT_1401f73f0`） | 3 | 行 75 | `dhz` 系 |
| `ferrum` | 6 | 行 116 | `FUN_*` 对象 |
| `probox` | 6 | 行 159 | `FUN_*` 对象 |
| `kmbox_bplus` | 11 | 行 201 | `FUN_14003ef70(&local_b8)` |
| `makcu` | 5 | 行 214 | `FUN_1401d9e20(0x1a0)` + `FUN_14003f5c0(obj, name)` |
| （5 字符，`DAT_1401f7344`，疑 `km_net`） | 5 | 行 227 | `FUN_1401d9e20(0x1c8)` + `memset 0x1c8` + `FUN_14003f460(obj)` |
| （6 字符，`DAT_1401f77f0`，疑 `catbox`） | 6 | 行 244 | `FUN_*` 对象 |
| （5 字符，`DAT_1401f7868`，疑 `cpbox`） | 5 | 行 262 | `FUN_*` 对象 |
| `kcom5` | 5 | — | 由 `kcom5_forward_enabled` / `enable_mouse_forwarder_key` 配置（`FUN_1400439b0`） |

驱动的**共同接口**由 `FUN_1400774e0` 暴露（见 B.5）：`vtable[+0x10]` 移动、`+0x18`（带 `isUp!=0` ⇒ 按下）、
`+0x20`（带 `buttonIndex` ⇒ 抬起）。`DAT_1401f7344 / 1401f77f0 / 1401f7868 / 1401f73f0` 的具体字符串
我未逐字节解出 → 见【未确认】。

相关硬件配置键（scope=0）：`km_com`（串口号，默认空）、`km_serial`（波特率，默认 **460800**）、
`km_box_vid` / `km_box_pid`（默认 0）、`km_net_ip` / `km_net_port` / `km_net_mac`、
`dhz_ip` / `dhz_port` / `dhz_random`、`enable_mouse_forwarder_key`（默认 `"f6"`）。
`kcom5_forward_enabled` / `enable_mouse_forwarder_key` 由 `FUN_1400439b0` 读取（anchor 直接给出两键名）。

搬运器函数（供参考）：`FUN_140042ca0` 行 63-65 与 `FUN_140043e00` 行 66-67 都在装配 `km_net_ip`
（前后两条不同的装载路径）；`FUN_140020b80`（anchor `[move_method]`）行 33-41 装配 `send_input`
并调 `FUN_1400187a0(..., param_2, ...)` ⇒ **是"设置 move_method"的写入口**。

## B.5 鼠标按下/移动的实际出口【已证到分发器层】

**按键分发器 `FUN_1400774e0`（RVA 0x1400774e0，44 行，原文）**：
```c
void FUN_1400774e0(longlong runtime, undefined4 buttonIndex, char isUp) {
  if (*(char *)(runtime + 0x10) == 0) {                  // 未初始化
    LOCK(); *(int*)(runtime+0x670) += 1; UNLOCK();
    if (FUN_1401d9b8b(runtime + 0x5d8) != 0) FUN_1401d9b97(5);        // 取 mutex
    if (*(int*)(runtime + 0x624) == 0x7fffffff) { ... FUN_1401d9b97(6); }
    plVar1 = *(longlong **)(runtime + 0x5c8);            // ★ 鼠标驱动对象
    if (plVar1 != 0) {
      if (isUp == 0) (**(code **)(*plVar1 + 0x20))(plVar1, buttonIndex);   // vtable[0x20] = ？ 抬起
      else           (**(code **)(*plVar1 + 0x18))();                      // vtable[0x18] = ？ 按下
    }
    FUN_1401d9b91(runtime + 0x5d8);                      // 放 mutex
    LOCK(); *(int*)(runtime+0x670) -= 1; UNLOCK();
    FUN_1401d9bcd(runtime + 0x628);
  }
}
```
- **驱动是纯虚接口**：`runtime+0x5C8` 是驱动对象指针，方法走 vtable `+0x18`（带 `isUp!=0` 时，
  即"按下"路径）与 `+0x20`（带 `buttonIndex`，即"抬起"路径）
- 调用者（`callmap.csv` 行 2147/2236/2477）：`FUN_14007d080`、`FUN_14007e5c0`、**`FUN_140088130`**
- 同样的虚接口在**宏引擎** `FUN_14002b010` 里被复用于 `mouse_move/mouse_click/mouse_down/mouse_up`
  （报告 3.3 已证 `*(*driver+0x10)`）；`FUN_140085410` 行 99 用 `FUN_14007c9b0(runtime)` 取配置对象
- 驱动实现族（字符串证据）：`km_net_ip` / `km_net_port` / `km_net_mac`（KMBox 网络）、
  `dhz_ip` / `dhz_port` / `dhz_random`（DHZ 盒）、`enable_mouse_forwarder_key` / `kcom5_forward_enabled`
  （KCOM5 转发）、`move_method`（`FUN_140020b80` 读取）；`makcu` 串出现在 `FUN_140040ff0` 行 214
- **驱动释放**：`FUN_14007d140` 行 113-133（anchor `release_mouse:before/after`）对 `runtime+0x5D0`
  做引用计数 + vtable `*plVar6` / `*(plVar6+8)` 析构

### 为什么这让手感更好/更快
- **一次虚调用 + 一把 mutex 就是全部开销**：没有"发送队列 → 工作线程 → 驱动"的第二跳。
  按下动作在**扳机状态机所在的同一线程栈上**直接落到驱动。
- 驱动抽象只暴露 `+0x18 / +0x20` 两个按钮入口 + `+0x10` 移动入口，说明**运动是单一通道**：
  自瞄（Scale/PID）、压枪、宏、自动背闪全部复用同一个 `mouse_move`，由上层做矢量合成，
  而不是各自调用一个驱动 API —— 这避免了多源输出互相覆盖/互相延迟。

## B.6 `Scale` 控制器（"甩枪"型）控制律：纯比例 + 输出端插值【已证】

**分派与标志位**（`FUN_1400824e0` 行 847-869，原文见报告 6.1）：
```c
if (*(longlong*)(param_2 + 0x3c) == 5) {                       // controllerType.size() == 5
    iVar16 = FUN_1401daeb4(pfVar24, "Scale");
    *(bool*)((longlong)param_1 + 0xe49) = (iVar16 == 0);        // isScale
    if (iVar16 != 0) goto LAB_140083a52;
    uVar11 = 1;                                                 // Scale ⇒ 事件驱动
} else { *(undefined1*)((longlong)param_1 + 0xe49) = 0;
LAB_140083a52:
    if (FUN_140015150(param_2+0x38,"PID") && FUN_140015150(param_2+0x30,"PID-EventSync")) uVar11 = 1;
    else uVar11 = 0; }
*(undefined1 *)((longlong)param_1 + 0xe4a) = uVar11;            // 事件驱动
...
param_1[0x1ca] = param_1[0x1ca] + 1;                            // ★ 推理序号（= 0xE50）
```
（`param_1[0x1ca]` 是 `longlong*` 索引 ⇒ 字节 0x1CA × 8 = **0xE50**，与 `FUN_140067000` 行 326 的
水位比对完全对上。行 871-872 另存 `+0xE58`、`+0xE60`。）

**Scale 的控制律**（`FUN_140067000` 行 1307-1345，原文）：
```c
if (*(longlong *)(puVar40 + 0x4c8) != *(longlong *)(puVar40 + 0x100)) {   // 新推理结果
  *(longlong *)(puVar40 + 0x100) = *(longlong *)(puVar40 + 0x4c8);
  dVar53 = (double)*(float *)(puVar40 + 0x400)      // ★ flick_scale_x   (AimKeyCfg +0x100)
         * (double)*(float *)(puVar40 + 0x534)      // ★ errX            (框中心-准星)
         * (double)*(float *)(puVar40 + 0x408)      // ★ flick_damping   (AimKeyCfg +0x108)
         * *(double *)(puVar40 + 0x650);            //   常量/换算
  *(double *)(puVar40 + 0x1d8) = dVar53;
  dVar54 = (double)*(float *)(puVar40 + 0x404)      // flick_scale_y (+0x104)
         * (double)*(float *)(puVar40 + 0x538)      // errY
         * (double)*(float *)(puVar40 + 0x408)      // flick_damping
         * *(double *)(puVar40 + 0x658);
  *(double *)(puVar40 + 0x210) = dVar54;
  dVar50 = (double)*(float *)(puVar40 + 0x410);     // scale_max_move_x
  if (dVar50 <= dVar51) dVar50 = dVar51;            // dVar51 = DAT_1401f6358 = 1.0  → 下限 1
  *(double *)(puVar40 + 0x1e0) = dVar50;
  dVar55 = (double)*(float *)(puVar40 + 0x414);     // scale_max_move_y
  if (dVar55 <= dVar51) dVar55 = dVar51;
  *(double *)(puVar40 + 0x180) = dVar55;
  // ---- 带符号幅值夹取（把 dVar53/dVar54 夹到 ±min(|v|, limit)）----
  dVar49 = (double)(bitcast<int64>(dVar50) ^ bitcast<int64>(DAT_1401f4680));   // DAT_1401f4680 = -0.0
  *(double *)(puVar40 + 0x1d0) = dVar49;
  pdVar30 = &(puVar40 + 0x1d0); if (dVar49 <= dVar53) pdVar30 = &(puVar40 + 0x1d8);
  pdVar21 = &(puVar40 + 0x1e0); if (dVar53 <= dVar50) pdVar21 = pdVar30;
  dVar50 = *pdVar21;
  dVar50 = (dVar50 < 0.0) ? (dVar50 - dVar8) : (dVar50 + dVar8);   // dVar8 = 0.5 → 四舍五入
  iVar43 = (int)dVar50;                                            // ★ X 鼠标计数
  *(int *)(puVar40 + 0xa8) = iVar43;
  ... Y 同法 → iVar36 = (int)dVar50; *(int *)(puVar40 + 0x68) = iVar36;
  *(undefined4 *)(puVar40 + 0x74) = 0;   *(undefined4 *)(puVar40 + 0x70) = 0;
```

**控制律（确定）**：
```
鼠标计数 = round( flick_scale[轴] × err[轴] × flick_damping × k )      , 夹到 ±scale_max_move[轴]
err[轴] = 检测框中心 − 准星
```
- **纯比例（P-only）**：无积分项、无微分项、**误差也没有低通**（`0x534/0x538` 是**原始**误差）
- `flick_scale_x/y` 是**原始 counts-per-pixel**（QML 说明原文："校准值现在保存原始 counts-per-pixel"）
- `flick_damping` 是**全局增益系数**（QML 中文标签是**"甩枪力度"**，默认 0.25，范围 0.01–2.0），
  ⇒ 实际增益 = `flick_scale × flick_damping`（0.06 配 3.225806 之类的组合就是"标定值 × 力度"）
- `k`（`0x650`/`0x658`）是每轴常量换算项（**【未确认】**其确切来源，疑似 1.0 或 dt 归一）
- **输出端有插值/摊薄**（行 1396-1431 原文）：
```c
lVar46 = max(0, *(int *)(puVar40 + 0x40c));            // ★ scale_delivery_ms（int）
if (0 < lVar46) {
  dVar50 = ((double)(lVar17 - lVar47) / 1e9) / ((double)lVar46 / 1000.0);    // = 已过ms / delivery_ms
  clamp(dVar50, 0.0, 1.0, 1.0);                        // 超时/负值都落到 1.0
  if (dVar50 < 1.0) { iVar43 = round(iVar43 * dVar50); }   // ★ 按已过时间比例发一部分
}
if (dVar50 < 1.0) { iVar36 = round(iVar36 * dVar50); }
iVar13 = iVar43 - *(int *)(puVar40 + 0x74);            // 本拍增量 = 新目标 - 上一拍已发
iVar32 = iVar36 - *(int *)(puVar40 + 0x70);
```
⇒ `scale_delivery_ms > 0` 时：一次推理结果算出的**总位移被按百分比分期发出**，
`scale_delivery_ms` 是"分发窗口"；`0`（**默认**）时 `lVar46 = 0` ⇒ 跳过整段 ⇒ **一步发完**。

**参数表**：

| 键 | 默认 | 范围 | 结构偏移（AimKeyCfg） | 帧结构偏移 | 含义 |
|---|---|---|---|---|---|
| `flick_scale_x` | 1.0 | 0.1–50.0 | 0x100 | 0x400 | X 原始 counts-per-pixel |
| `flick_scale_y` | 1.0 | 0.1–50.0 | 0x104 | 0x404 | Y 原始 counts-per-pixel |
| `flick_damping` | **0.25** | 0.01–2.0 | 0x108 | 0x408 | 甩枪力度（全局增益） |
| `scale_delivery_ms` | **0** | 0–100 | 0x9C | 0x40C | 位移分发时长（0=一步发完） |
| `scale_max_move_x` | 9999.0 | 1.0–100000.0 | 0xA0 | 0x410 | X 单帧最大移动（实际下限夹到 1.0） |
| `scale_max_move_y` | 9999.0 | 1.0–100000.0 | 0xA4 | 0x414 | Y 单帧最大移动 |
| `goal`/`aim_controller` | `"PID"` | PID / Scale | 0xE0（std::string） | — | 控制器类型 |

> 偏移不变量核对：帧结构 `puVar40+0x4C0 ⇄ runtime+0xE48`，故 `0x400 ⇄ 0xE88`。
> 报告 9.2 把 `flick_*` 放在 `0x100/0x104/0x108`；此处从**消费端**独立得到同一组偏移，
> 且三个坐标差恰为 4/4 ⇒ 都是 4 字节 float，与报告一致。`scale_delivery_ms(0x9C)` 为 **int**
> （消费者 `*(int*)(puVar40+0x40c)`，加载器用 `(float)FUN_1400172b0(...)` 后夹 `0..0x65`）。

**事件驱动"每个推理结果只消费一次"**（`FUN_140067000` 行 271-331，原文见报告 6.2）：
```c
if (*(char *)(lVar17 + 0xe4a) == '\0' || stopped) {
    /* 普通路径：定时等待 1 ms ⇒ ~1000 Hz */
    lVar37 = now_ns() + 1000000;
    while (!*(char *)(lVar17 + 0xe40)) { if (now_ns() >= lVar37) break;
        FUN_1401d9c74(lVar17 + 0xdf8, mutex, timeout_ns); }
} else {
    /* 事件驱动：无限期等条件变量，被推理线程唤醒 */
    while (true) {
        lVar37 = *(longlong *)(puVar40 + 0xd0);              // 当前推理序号
        if (*(char *)(lVar17 + 0xe49) != 0) lVar37 = *(longlong *)(puVar40 + 0x100);   // isScale 用另一路
        if (stop || 0xed6 || !0xe4a) break;
        if (*(longlong *)(lVar17 + 0xe50) != lVar37) break;   // ★ 水位变了 ⇒ 干活（走一次控制律）
        if (*(int *)(lVar17 + 0xa78) != 0) break;
        if (*(int *)(lVar17 + 0xa7c) != 0) break;
        FUN_1401d9cd7(lVar17 + 0xdf8, mutex);                 // ★ 无限期 wait
    }
}
```
**`+0xE50` = "已消费到的推理序号水位"**；行 1123 在事件分支里做 `if (*(puVar40+0x4c8) == *(puVar40+0xd0))`
就沿用上一拍输出，否则才重新算控制律；行 1255/2025 把 `+0xE50 = +0x4C8` 提交。

### QML 原文（用户可见语义）
`qml_z\当前_PID_配置档位…qml` 行 457-458：
> "Scale is driven directly by inference events: each latest inference result is consumed once,
> so total movement no longer depends on the 1000 Hz PID loop. Recalibrate after this update;
> calibration now stores raw counts per pixel."
> 中文：Scale 由推理事件直接驱动：每个最新推理结果只消费一次，因此总位移不再依赖 1000Hz PID 循环。
> 更新后请重新校准；校准值现在保存原始 counts-per-pixel。

### 为什么这让手感更好/更快（Scale vs naive PID）
1. **总位移与执行频率解耦**：naive PID 在 1000 Hz 上跑，一拍发 `Kp*e`；如果 `e` 在两拍之间没变，
   它会**重复发**同一个位移（过冲），或者靠积分/阻尼去压。Scale 是**每次新观测发一次**，
   位移严格 = `scale × e`，重复消费被水位比对挡掉（行 326）。
2. **延迟不受 1000 Hz 相位影响**：naive 实现要等到下一个 1 ms tick 才发；
   Scale 在推理序号变化后**立刻被唤醒**（`FUN_1401d9bcd(runtime+0xDF8)`）。
3. **一步到位，不被"每拍限幅"切碎**：`scale_max_move_x/y` 默认 9999（等于不限），
   所以一次大甩枪的**全部位移在一拍内到达**；naive 实现常见的"每帧最多 N counts"限幅
   会把一次 200 px 的甩枪拉成十几拍，手感就是"黏、慢、追不上"。
4. **纯比例 ⇒ 零相位滞后**：没有微分低通、没有积分爬升，所以"停下就停"、无超调尾巴；
   代价是没有稳态误差消除（靠 `flick_scale` 的**在线标定**而不是靠积分来补）。
5. **`flick_damping` 是"力度"而不是"滤波强度"**：调它直接改增益，不引入任何时间常数，
   所以调参不会改变响应速度 —— 这解释了为什么作者把它标成"甩枪力度"。

## B.7 人类化 / 平滑参数（输出层三选一互斥）【已证】

`qml_z\左列__Hardware___AxisLock_.qml`，三组**互斥**（每组的 toggle 会关掉另两组）：

| 组 | 键 | 默认 | 范围 | 含义 |
|---|---|---|---|---|
| **Curve** | `enable_mouse_curve` | false | — | 启用曲线 |
| | `enable_custom_mouse_curve` | false | — | 启用自定义曲线（读 `custom_curve_profile` JSON） |
| | `custom_curve_profile` | `custom_curve_profile.json` | — | 轨迹小游戏（独立软件）导出的 JSON 路径 |
| | `wind_mouse_G0` | 5.0 | 0–200 | "重力"（WindMouse 算法参数） |
| | `wind_mouse_W0` | 2.0 | 0–200 | "风力" |
| | `wind_mouse_M0` | 10.0 | 0–200 | 步长 |
| | `wind_mouse_D0` | 8.0 | 0–200 | 距离 |
| | `curve_threshold` | 10 | 0–100 | 曲线门控（px）：误差小于它才走曲线 |
| **Jitter** | `enable_mouse_jitter` | false | — | 随机扰动 |
| | `mouse_jitter_min` | 0.9 | 0.5–1.5 | 扰动下限（倍率） |
| | `mouse_jitter_max` | 1.1 | 0.5–1.5 | 扰动上限（倍率） |
| **Segmented** | `enable_mouse_segmented` | false | — | 拟人分段 |
| | `segmented_max_move` | 20.0 | 0–200 | 触发阈值（px） |
| | `segmented_max_step` | 10.0 | 0–50 | 拆段粒度（px） |
| **其他** | `counts_per_360_x/y` | — | — | 每 360° 的计数（校准辅助） |
| | `move_method` | — | — | 驱动/移动方式（`FUN_140020b80` 读取） |

**QML 语义原文**：
> "把一次较大的瞄准移动按加减速拆成多段（先快后慢），总位移不变，更像真人甩枪。
> 与随机扰动、曲线互斥，三者只能开一个。"（`segmented`）

**效果分析**：
- **Segmented**：**总位移不变**，只把"一拍一大步"拆成"多拍小步、先快后慢"⇒ 平滑但与 1 冲突
  （它引入额外时间）。它是**反作弊取向**的选择，不是性能取向。
- **Jitter**：把每一步位移乘上 `U(0.9, 1.1)` 的倍率 ⇒ 位移有随机误差、**总位移有漂移**，
  稳态时会表现为小抖动；同样损失精度。
- **Curve/WindMouse**：用 WindMouse 物理模型（重力/风力/步长/距离）生成路径，
  `curve_threshold` 限定"只在小误差时启用"⇒ 大甩枪仍走直线（保精度），小修正才拟人化。
- **默认三者全关** ⇒ **默认输出是"纯比例一步到位"**，只有插值（`scale_delivery_ms`，默认也关）。

---

# (B2) 精解补遗：压枪播放线程、鼠标驱动接口全表、输出整形与屏蔽

> 本节是第二轮（并行深挖 + 我自己独立复核）的补充，**其中所有关键论断我都用 capstone 直接反汇编二进制复核过**。
> 复核方法记录在每条结论后。

## B2.1 两个工作线程的线程体在哪（重要修正）

`v1030\funcs\` 里**没有**压枪线程体的反编译文件（Ghidra 没切出来），必须从**线程创建点**回溯。
`FUN_140077f40`（运行时初始化）行 803-873 一处创建两个线程：

```c
// 行 814 —— 瞄准工作线程
lVar26 = (*DAT_1401f08e0)(0,0,FUN_140062910,puVar16);      // std::thread / CreateThread
// 行 852 —— 压枪工作线程
lVar26 = (*DAT_1401f08e0)(0,0,FUN_1400629a0,puVar23);
param_1[0x131] = lVar26;                                    // handle → runtime+0x988
*(u32*)((longlong)param_1 + 0x994) = uVar6;                 // id     → runtime+0x990
```
两个 trampoline（**我已直接读文件复核**）：
```c
// FUN_140062910  (funcs\FUN_140062910.txt 行 4-13)
undefined8 FUN_140062910(longlong param_1) { FUN_140067000(); FUN_1401d9bd3(); if (param_1) free8(param_1); return 0; }
// FUN_1400629a0  (funcs\FUN_1400629a0.txt 行 4-13)
undefined8 FUN_1400629a0(longlong param_1) { FUN_14006b630(); FUN_1401d9bd3(); if (param_1) free8(param_1); return 0; }
```
⇒ **修正**：
- **`FUN_140067000`（2079 行）就是"瞄准工作线程体"本尊**，不是被别的线程每拍调用的库函数。它内部那个
  `runtime+0xDF8` 条件变量等待（行 271-331）就是这条线程自己的节拍器。
- **压枪线程体 = `FUN_14006b630`**（RVA 0x14006b630–0x14006ba1b ≈ 1003 字节；`funcs\` 里**无**此文件，
  `Test-Path` 返回 False，已复核）。它自己**不含** anchor 字符串，所以按字符串永远搜不到它 ——
  线程名来自它的 **MSVC EH funclets** `FUN_1401e0180`（`"recoil worker: "`）与 `FUN_1401e0300`
  （`"recoil worker: unknown exception"`），两者都先 `*(u8*)(runtime+0xa59)=0` 再打日志
  （`FUN_1401e0180` 行 22-24）。**这两个是 catch handler，不是循环体** —— 这解释了第一轮我以为
  `FUN_1401e0180` 是线程体却找不到循环的困惑。

## B2.2 压枪回放循环（`FUN_14006b630`）—— 已解

```
6b685 rdi = runtime+0x998 ; 6b6a3 _Mtx_lock                  // 整个 idle/取货阶段一把 unique_lock
6b6d0 movzx eax,[rbx+0xa31] / 6b6dc movzx eax,[rbx+0xa30]    // ready / stop
6b6e8 mov rdx,rdi ; lea rcx,[rbx+0x9e8] ; 6b6f2 _Cnd_wait    // ★ 空闲：等 runtime+0x9E8
6b6fd if (runtime+0xa30) { unlock ; return }                 // 停止 → 线程退出
6b745 xchg byte [rcx+0xa31],0                                // 原子清 ready
6b74f rbx = runtime+0xa38 ; 6b782 call 0x140061fd0           // ★ 把 vector<step>（0xa38/0xa40/0xa48）复制到本地
6b787 movzx ebx, byte [rbx+0x18]                             // = runtime+0xa50 = move_mode "merge" 标志
6b796 unlock runtime+0x998                                   // ★ 先放锁再发送 ⇒ 与 +0x5D8 无锁序倒置
6b7a0 for (r13 = begin ; r13 != end ; )                      // 6b9a1: add r13,0xc  ← 12 字节步长
6b7af/6b7c3 if (runtime+0xa32 /*抢占*/ || runtime+0xa30 /*停止*/) break
6b7d3 cmp byte [rcx+0x10],0 ; jne skip                       // runtime+0x10 = force_aim → 抑制输出
6b7e5 mov edx,[r13]      (rel_x)
6b7e1 mov r8d,[r13+4]    (rel_y)
6b7dd movzx r9d,bl       (merge 标志)
6b7e9 call 0x140077200                                       // ★ 发射（见 B2.4）
6b823 mov ecx,[r13+8]    (delay_ms) ; 6b82b cmovg → max(0,delay)
6b846 imul rcx,rsi,0xf4240                                   // ms → ns
6b836 call 0x140025f80 (steady_clock::now) → deadline = now + delay*1e6 ns
6b890-6b95a 若 deadline > now: 6b941 FUN_1400372d0（ns→ms 向上取整，上限 24h）
            6b953 FUN_1401d9c74(runtime+0x9e8, mutex, ms)      // ★ 定时条件变量等待 = 可中断 delay
6b9a1 add r13,0xc
6b9af xchg [rcx+0xa32],0 ; 6b9d1 if(!runtime+0xa31) xchg [rcx+0xa59],0
6b9dc free 本地 vector ; jmp 0x6b664                          // 回到 idle 等待
```
**要点**：
- 步结构 `{int rel_x; int rel_y; int delay_ms}` **二次确认**（12 字节步长、三个 int 偏移 0/4/8）
- 回放用**绝对截止时刻调度**（`deadline = now + delay`），且用**定时条件变量等待**而不是 `Sleep`
  ⇒ 生产者 notify 或 `+0xA32` 抢占 / `+0xA30` 停止都能**立刻打断**正在等待的 delay
- **没有任何 `Sleep`/`usleep`/`nanosleep`/`WaitForSingleObject`** 出现在驱动层与发送循环里
  （全语料 grep + 子代理独立确认）
- `runtime+0x10` = `force_aim`（由 `FUN_140077f40` 行 917-919 的 `", force_aim="` 串选出 byte[0x10] 推得）

## B2.3 `move_mode` 的真实语义：**单流合并 vs 双流独立**（重要）

- 默认 `"separate"`，`puVar28[0x36] = (strcmp(cfg,"merge") == 0)`（`FUN_140085410` 行 148-163）
- 发布时该标志被搬到 **`runtime+0xA50`**（行 728 `puVar28[0xa8] = puVar28[0x36]` →
  行 793 `*(u8*)(puVar18+3) = puVar28[0xa8]`，`puVar18 = param_1+0xA38`），
  然后 `+0xA31=1`、`+0xA32=0`、`+0xA59=1`、`FUN_1401d9bcd(+0x9E8)`、放锁（行 795-806）
- 线程在 `0x6b787` 读它并作为第 4 参传给发射器

| 模式 | 行为 |
|---|---|
| `separate` | 每个弹道步**立即**作为一次独立 HID 移动发出，label = `"recoil"` |
| `merge` | 位移**原子累加**到 `runtime+0xA78`(dx) / `+0xA7C`(dy)，并 `notify runtime+0xDF8`（**瞄准线程的条件变量**）；瞄准线程在自己的 tick 里**原子取走并清零**（`FUN_140067000` 行 1756-1763），加进本拍输出（行 1759/1764-1765），再把总量累加到 `runtime+0xAD8/0xADC`（mutex `0xA88`，行 1768-1775），最后合成路径并**每段发一次 label = `"aim"` 的合并移动**（行 1865-1924） |
| `merge` 但自瞄未激活（`runtime+0xA80 == 0`） | **静默退化为** 独立的 `"recoil"` 发送 |

⇒ **不是"两条并行流"**：merge 是"压枪位移注入到自瞄输出下游、由自瞄线程统一合成发送"。
而且 merge 会**强制多跑一次瞄准 tick**（`FUN_140067000` 行 326-327 的事件驱动等待谓词里显式包含
`*(int*)(runtime+0xa78)!=0 || *(int*)(runtime+0xa7c)!=0`）。merge 模式每拍在行 520-525 重新武装
（要求自瞄键按下快照 `puVar40[0x4c0]!=0`、`runtime+0x10==0`、鼠标对象非空），行 2064-2066 清除。

**关键设计结论**：**压枪位移注入在控制器（PID/Scale）之后，绝不进入自瞄误差** —— 两者不会互相积分为振荡。

## B2.4 鼠标发射器 `FUN_140077200`（RVA 0x140077200–0x1400774da）

签名 `void(runtime*, int dx, int dy, char merge)`（`merge` = r9b）。

**我自己用 capstone 复核了这段（关键 6 条指令）**：
```
140077229  test r9b,r9b ; 14007722c jne 0x1400772f9        ; merge != 0 → 合并分支
140077232  lea rdi,[rcx+0x5d8] ; 140077240 call 0x1401d9b8b  ; 锁 runtime+0x5D8（鼠标设备锁）
14007725f  mov rcx,[rbx+0x5c8]                             ; 鼠标驱动对象
14007726f  mov rax,[rcx] ; 140077272 mov r10,[rax+0x10]     ; ★ vtable[+0x10] = move
14007727d  mov qword [rbp-0x18],6                           ; std::string 长度 = 6
14007728d/296d  "recoil" 的 6 个字节（0x1401f8844）
1400772a5  lea r9,[rbp-0x28]                                ; 第 4 参 = label
1400772ac  mov edx,r14d (dx) ; 1400772a9 mov r8d,r15d (dy)
1400772af  call r10                                         ; device->move(dx, dy, "recoil")
```
merge 分支（`0x1400772f9`）：若 `runtime+0xA80 != 0` ⇒ `lock add [rcx+0xA78],dx` / `[rcx+0xA7C],dy`
+ `notify runtime+0xDF8`；否则清零 `0xA78/0xA7C` 后走同一条独立发送。

## B2.5 驱动工厂与完整虚接口表

**工厂 `FUN_140040ff0(out, configScope)` 先调 `FUN_140020b80`（`move_method` 读取器）再按字符串分派**：

| `move_method` | 对象大小 | 主 vtable RVA | 次接口 |
|---|---|---|---|
| `""` / `send_input` | 0x28 | **0x1f6d78** | 0x1f6dc8 @obj+8（键盘） |
| `dhz` | 0x1c8 | **0x1f7358** | — |
| `ferrum` | 0x1a8 | **0x1f70d8** | 0x1f7130 @obj+0x1a0（键盘） |
| `probox` | 0x1a0 | **0x1f71c8** | — |
| `kmbox_bplus` | (经 `FUN_14003ef70`) | **0x1f7230** | — |
| `makcu` | 0x1a0 | **0x1f6f38** | — |
| `kcom5` | 0x1c8 | **0x1f72c0** | — |
| `catbox` | 0x6b0 | **0x1f7768** | 0x1f77b8 @obj+8 |
| `cpbox` | 8 | **0x1f7800** | — |
| `km_net` | — | **0x1f7448** | — |
| 未知值 | 0x28 | **0x1f6e08**（名字 + `"-protocol-pending"`） | 全部槽 = `FUN_1400406c0`（`xor al,al; ret`） |

驱动对象被包在引用计数控制块里（vtable `&DAT_1401f98b8`，引用计数 `+8/+0xc`），存在
**`runtime+0x5C8`**（`FUN_140077f40` 行 584-610）；控制块 `+0x5D0`，**mutex `+0x5D8`**。

### 完整鼠标输出接口（主 vtable，槽位 = 字节偏移）

| 槽 | 语义 | send_input | km 串口系（makcu/ferrum/probox） | dhz |
|---|---|---|---|---|
| +0x00 | 析构 | FUN_1400403e0 | FUN_1400403a0 / 140040180 | FUN_140040140 |
| +0x08 | **init/connect(name)** → bool | FUN_1400406b0（`mov al,1;ret`） | FUN_1400450e0（4 Mbaud）/ FUN_1400436a0（115200）/ FUN_140044dd0（921600） | FUN_1400433a0 |
| **+0x10** | **move(dx, dy, label)** → bool | FUN_140047910 | FUN_140047550 | FUN_140046e40 |
| **+0x18** | **button_down(index)** | FUN_1400408b0 | FUN_140040960（→`FUN_14004a3d0`, r8=1） | FUN_140040730 |
| **+0x20** | **button_up(index)** | FUN_140040b40 | FUN_140040bf0（r8=0） | FUN_1400409c0 |
| +0x28 | **axis_lock(mask_x, mask_y)** | FUN_1400406c0（空操作） | FUN_14004b720（`km.lock_mx/lock_my`） | FUN_14004aca0（`mask_x(`/`mask_y(`） |
| +0x30 | **button_mask(l,r,m,s1,s2,wheel)** | FUN_1400406b0（空操作） | FUN_14004cd10 / FUN_14004ca20 | FUN_1400406b0（空操作） |
| +0x38 | **read_hard_button_state(out)** | FUN_140048950 | FUN_1400489d0 | FUN_140048640 |
| +0x40 | **get_name**（std::string out） | FUN_140047ae0 → `"send_input"` | FUN_140047b20 | FUN_1400479e0 |
| +0x48 | bool **"协议需要 ACK"** | — | 0（probox = 1） | — |

**按钮索引全驱动统一：0=左, 1=右, 2=中, 3=侧1, 4=侧2**
（send_input `X:1400408b0`：`0x02 LEFT DOWN / 0x08 RIGHT DOWN / 0x20 MIDDLE DOWN / 0x80 XDOWN` +
`mouseData = 1|2`；km `X:14004a3d0`：`km.left/right/middle/side1/side2`）。

### **`+0x18` / `+0x20` 的方向：我已经独立反汇编定论**
```
1400774f2  movzx r14d,r8b               ; r8b = 第 3 个参数(param_3)
...
14007753a  test r14b,r14b
14007753d  je   0x140077544
14007753f  call qword ptr [rax + 0x18]  ; ★ param_3 != 0 → +0x18
140077544  call qword ptr [rax + 0x20]  ; ★ param_3 == 0 → +0x20
```
而 `FUN_140088130` 里 `FUN_1400774e0(runtime, 1, 1)` 是**按下**、`(runtime, 1, 0)` 是**抬起** ⇒
**`+0x18` = DOWN，`+0x20` = UP**，两槽都签名 `(this, int buttonIndex)`。
（本轮修正：先前把 `+0x18` 读成"抬起"是错的。）

### 传输层
- **`send_input`** = Win32 **`SendInput`**（导入名 `"SendInput"` 出现一次）。全镜像**没有**
  `mouse_event` / `NtUserSendInput` / `vgamepad` / `ViGEm` / `VirtualHID` / `MouClass` / `HidD_` / `\Device\`（计数全 0）。
- **km 串口系** = COM。`km_serial`（0x1f6f88）、`COM3`（0x1f6f94，默认）、`\\.\`（0x1f6f28）；
  `FUN_140025250(port,"km_serial",baud)`，**115200 / 921600 / 4000000**。
  ASCII 协议：`km.buttons(1)` 握手、`km.move(dx,dy)`、`km.left(1|0)`、`km.lock_mx(1|0)`、`km.mask(`、`km.getpos()`、`km.init()`
- **kcom5** = 串口 + 转发线程（见 B2.8）
- **dhz / km_net / catbox** = 网络。`dhz_ip` 默认 **192.168.8.88**，ASCII `move(`、`mask_x(`、`monitor(0)`；
  `km_net_ip` 默认 **192.168.2.189**、`km_net_port` **8888**、`km_net_mac`、`km_net_uuid`，
  日志 `[kmnet] connecting to %s:%s mac=%s...`、`kmNet_init took %lld ms`、`[kmnet] move #%d: (%d, %d)`
  ⇒ **官方 kmboxNet API 被静态链接进来**；`FUN_140052950` 构造 `htons` 序的 0x10/0x1c 字节头

## B2.6 绝对 vs 相对：**相对计数**（已证）
`FUN_140047910`（send_input 的 move）：`edx` → `INPUT+0x08`（`mi.dx`）、`r8d` → `+0x0C`（`mi.dy`）、
`mouseData = 0`、`dwFlags = 1` = **只有 `MOUSEEVENTF_MOVE`；绝对标志 0x8000 从不设置**；40 字节 INPUT，`SendInput(1,..)`。
km move 是插入 8 字节前缀 `km.move(`（RVA 0x1f6fc0）再把两个整数 `itoa` 进去；dhz 插入 5 字节 `move(`。
**独立佐证**：`FUN_14006bf50` 先读光标、发一次移动、再读光标，返回 `param_5 / |Δ像素|` = **counts-per-pixel 标定**
（只有相对移动才成立）。

## B2.7 速率、批量与"爆发式"发送
- 每拍（`FUN_140067000`）先构造 `{int dx, int dy}` 的 8 字节元素列表，然后**背靠背全部刷出**：
```c
// FUN_140067000 行 1868-1918
for (p = vec.begin; p != vec.end; p += 8) {
  if (*p == 0 && p[1] == 0) continue;
  lock(runtime+0x5d8);
  while (!stop && *(int*)(runtime+0x670) != 0) FUN_1401d9cd7(runtime+0x628, cv);   // ★ 互锁
  driver = *(void**)(runtime+0x5c8);
  if (driver) (*(code**)(*driver + 0x10))(driver, p[0], p[1], &label);              // label = "aim"
  unlock;
}
```
- `runtime+0x670` 正是 `FUN_1400774e0` 自增的那个计数器 ⇒ **移动会等"没有按键操作在途"**（通知 `+0x628`）
- **因为整批在一个 tick 内无间隔写入，驱动看到的是"爆发"，不是"摊开"** —— `scale_delivery_ms`
  是**唯一**把位移摊到多个 tick 的机制，而它默认 0
- 传输层自我限速：`FUN_140049790` 在 `driver->+0x48 != 0`（probox）时等回复 token `"success"`
  （`FUN_14004aa10(...,5,10,1,5,...)`）⇒ **probox 每个命令都是往返受限**；其余走 `FUN_1400495e0` 即发即忘

## B2.8 输出整形全在**生产者侧**，驱动不做任何处理
驱动**不做**缩放/限幅/插值：`FUN_140047910` 原样拷贝两个整数；`FUN_140047550`/`FUN_140046e40` 只 `itoa`。
所有整形在虚调用之前：
- **`FUN_140071a60`** = 输出整形总入口：`enable_mouse_curve` → `FUN_140071f30`（wind mouse，
  用 `wind_mouse_G0/W0/M0/D0`）；`enable_custom_mouse_curve` → `FUN_14005a070`（`custom_curve_profile.json`）；
  否则 `FUN_140063850`。**当 `|dx| <= param_5 && |dy| <= param_5` 时整段曲线被旁路**
  （`FUN_140071a60` 行 70-79），该阈值就是 **`curve_threshold`**
- **`FUN_140058230(vec, dx, dy, maxStep)`** 把一次移动拆成 ≤ `segmented_max_step` 的段
  （`FUN_140067000` 行 1851 读键 `"segmented_max_step"`），结果由 `FUN_140065860` 并入发送列表（行 1861）
- 每次调用的 label（3–5 字符 std::string）：`"aim"`（行 1908-1910，字节 @RVA 0x1f87dc）、
  `"calib"`（`FUN_14006bf50` 行 75）、`"recoil"`（压枪）、`"other"`（`FUN_14007de40`）

## B2.9 鼠标按键屏蔽 / 轴屏蔽的消费者 = `FUN_1400874e0`
QML 10 个键（scope=1, subObject `"mask"`）：`mask_left/right/middle/side1/side2/wheel` + `mask_x/y`、`aim_mask_x/y`，
**全部默认 false**。C++ 消费点 `FUN_1400874e0` 行 529-567（反汇编 `0x140087d20`–`0x140087f30`）：
读 8 个标志（`FUN_140066720(scope,"mask_*",0)`），合并
`mask_x |= (aim_active && aim_mask_x)`（Y 同）→ 存 `runtime+0xBB5..+0xBBC`，然后
```asm
140087f2b call qword ptr [rax+0x30]   ; button_mask(l,r,m,s1,s2,wheel)
140087f5b call qword ptr [rax+0x28]   ; axis_lock(mask_x, mask_y)
```
由"标志有变化"**或** 1 秒重下发定时器保护（`runtime+0xBC0 = now + 0x11e1a300 ns`），失败则重排同一截止时刻。
实际效果：km 系发 `km.mask(...)` / `km.lock_m*`、dhz 发 `mask_x(v)` / `mask_y(v)`、
kcom5 把 6 字节掩码写到 `obj+0x102..0x107`、**send_input 是空操作**。
⇒ **这是设备侧屏蔽（盒子过滤物理鼠标自身按键），不是 OS 层钩子** —— 所以对游戏无侵入、也无 Hook 痕迹。

## B2.10 mouse_forwarder（KCOM5）
`kcom5_forward_enabled` + `enable_mouse_forwarder_key` 在 kcom5 的 init `FUN_1400439b0` 里被消费：
标志→`obj+0x10a`、运行中→`+0x10b`、键对象→`+0x110`（`FUN_140007ac0`）；
启用时**另起一条线程**（`(*DAT_1401f08e0)(0,0,FUN_14003ec60,this)`，handle `obj+0x138`）。
每拍 kcom5 的 **`+0x38` = `FUN_1400486c0`** 用 `FUN_1400263e0` = `GetAsyncKeyState(vk) >> 15` 轮询真实按键，
热键状态变化时翻转 `+0x10b`，然后在 `obj+0x170` 锁下返回盒子缓存的按键状态。
（send_input 的 `+0x38` = `FUN_140048950`，轮询 VK 1,2,4,5,6 → 左/右/中/侧1/侧2。）

## B2.11 另外两条发送路径（本轮新发现，与压枪不同）
- **`FUN_14007de40`**：批量 `move()` 发送器，label = `"other"`（RVA 0x1f8ce4），
  内部**再种一次 MT19937**（0x270 状态，`0x14007de96`），读方向选择器 `"left"`/`"right"`/`"auto"`，
  并在 `struct+0x8c/0x90/0x94/0x98/0x9c/0xb0/0xb4` 上做轴向夹取（`FUN_140066b00`）；唯一调用者 `FUN_140066a90`。
  **【未确认】** 它属于哪个功能（子代理推测是某种"抖动/其他"发送器，"recoil" 只是从选择器串猜的）。
- 宏引擎 `FUN_14002b010` 的 `mouse_move/mouse_click/...` 走同一 `vtable[+0x10]` / 同一按钮槽。

## B2.12 第二轮新增的"未确认"
1. **km_net / catbox 的 socket 类型（UDP vs TCP）未证**：头部用 `htons/htonl` 且字符串与 kmboxNet 一致
   （kmboxNet 自身是 UDP/8888），但从镜像无法证明 socket 类型；`0x1f7448` 与 `0x1f7768` 哪个对应
   `km_net` / `catbox` 是按分支顺序推的（`A:66352/66382`），非直接证明
2. `km.mask(...)` 的 6 键报文编码未逐字节验证（只反汇编到 probox 变体的 `km.lock_mw(` 尾）
3. `+0x48` = "协议需要 ACK" 是**从 `FUN_140049790` 行 9-18 的形态推断**（`==0` 直发，否则等 ACK），非命名证据
4. `move()` 第 4 参（label）已被证明是 3–6 字符 std::string，但 send_input 的 `FUN_140047910` **根本不读 r9**
   ⇒ 多数驱动忽略该参数（它更像遥测/日志标签）
5. km_net / catbox 的 move 实现**是否在驱动内限幅**未验证；send_input / dhz / km 串口系已证**不限幅**
6. `mask_wheel` 只在 probox 变体里定位到 `km.lock_mw(`；makcu/ferrum 的滚轮屏蔽未证
7. 镜像**没有导入表**（`DataDirectory[1] = 0`，IAT 槽在运行时填充）⇒ OpenCV/CRT 符号名不可恢复，
   `optical_flow` 用到金字塔+LK 是**按调用形态与参数校验推断**的

---

# (D) 未确认 / 边界清单

1. **【未确认】`AimKeyCfg + 0x5A` 的语义**：`FUN_1400824e0` 行 741 用它做扳机总开关
   （`trigger 决定 = 命中 && (param_2+0x5A != 0)`），但没在 `FUN_14001ba90` 找到明确的写入点；
   由 `local_250`（结构偏移 0x58，行 625 从 `DAT_1401f571c` 读出）推得它是 `0x58` 那个 8 字节值的高 4 字节。
2. **【未确认】AimKeyCfg trigger 子对象内 `is_continuous_burst` 的精确偏移**：我按
   `FUN_14001ba90` 行 958-967 的**读取顺序**推为 `+0x16E`，但 `FUN_140088130` 不读该偏移，
   所以没有第二处印证（其余 9 个键有两个独立来源）。
3. **【未确认】`flick_scale`/`flick_damping` 之外的换算常量 `0x650 / 0x658`**：
   `FUN_140067000` 行 1309-1313 各乘一次，但我没定位到它的来源与量纲（疑似 counts-per-pixel
   的每轴归一或 1.0）。**控制律的形状（纯比例 + 乘 damping + 夹幅 + 四舍五入）已确定，
   但"乘 0.650 之后是否还等于物理计数"未确认。**
4. **【未确认】`axis_lock` 三个平方值的消费点**：`+0x688 (900) / +0x690 (1600) / +0x698 (4)` 的
   **写入**已闭合（行 655-712），`+0x680/+0xD8` 组的二选一选择点也已找到（anchors.txt
   行 30904-30912），但在 `FUN_140067000`（frame 结构）与 `FUN_1400824e0`（state 结构）里
   我都没定位到"用它们决定方向持有"的比较/状态机。**"存平方值"是由 `30²/40²/2²` 三对默认值
   吻合得出的推断，未找到读点直接印证。**
5. **【未确认】`axis_lock_flick_*` 的加载默认值与 QML 默认值不一致**：
   加载器用 `DAT_1401f80f8 = 10`（flick_hold，与 QML 一致）但 `DAT_1401f9b98 = 40`
   （flick_reversal，QML 默认 **100**）、`DAT_1401f8040 = 2`（flick_deadzone，QML 默认 **1**）。
   即 **QML 首次写入前的运行时默认值与界面显示值不同**，可能造成"没动过滑块时参数与显示不符"。
6. ~~**【未确认】压枪线程的回放循环体与 `mouse_move` 出口**~~ → **已闭环（见 (B2)）**。
   回放循环体 = **`FUN_14006b630`**（`funcs\` 里没有，需从线程创建点 `FUN_140077f40` 行 852
   `(*DAT_1401f08e0)(0,0,FUN_1400629a0,…)` 回溯；`FUN_1401e0180/1401e0300` 只是它的 MSVC EH catch handler）；
   发射出口 = **`FUN_140077200(runtime, dx, dy, merge)`** → `vtable[+0x10]`，label `"recoil"`；
   `move_mode == "merge"` 的合并落点 = **`runtime+0xA78/0xA7C` 原子累加 + 唤醒瞄准线程**
   （`notify runtime+0xDF8`），由瞄准线程在 `FUN_140067000` 行 1756-1763 取走并合成发送。
7. ~~**【未确认】`optical_flow` 录制源的具体光流实现**~~ → **大体闭环，仅符号名不可恢复**。
   `FUN_14005df80(runtime+0xB18, &frame@0x540, prevFrame, out_dx, out_dy)`：
   要求两帧同尺寸、≥64×64、≥3 通道，建金字塔（迭代/尺度循环），
   只保留 `status!=0 && err <= DAT_1401f8110 && |dy| <= DAT_1401f8100`（≥6 个样本），
   最后返回**位移中位数**（`FUN_14005dc50`），调用方取整后喂给 `FUN_14005f590`。
   **仍未确认**：镜像无导入表（`DataDirectory[1] = 0`），OpenCV 符号名不可恢复，
   「金字塔 + LK」是按调用形态推断。
8. **【未确认】QML 里 `max_fps` 与二进制 `DAT_1401f9578` 的绑定**是**字符串内容相等**（我实测
   `DAT_1401f9578` = `"max_fps"`）加调用形态（`FUN_140025250(cfg,"max_fps",0)`）推出，
   但该常量在 `anchors.txt` 里显示 `[无引用]`（应为 Themida 保护区写入），故不是"直接 xref 级"证据。
9. **【未确认】`FUN_140088130` 中 `+0x974` 的精确语义边界**：它只在"空闲态 + `is_long_press_right`"
   这条路径上被判定，我把它读作"本 tick 已按下过"；但代码同时用 `+0x971/972/973` 三个位，
   完整的状态命名（除 `+0x971` = 主按下闩外）仍是推断。
10. **【未确认】`: +0x16B`（`is_long_press_right`）在"点按右键"分支（行 89-101）里的作用**：
    该分支里 `start_delay` 被用来设 `+0xBA8`，而 `is_long_press_right` 此时为 0；
    但行 89 判断的是 `cVar8 == 1`（= 点按右键），故这一分支**不会**在长按模式下进入，
    `start_delay` 实际只在行 139/153 的路径生效 —— 这个"读法"我保留一处不确定性。
11. **【未确认】`FUN_1400886f0` 的"OR 语义"** 已由 1.0.8/1.0.30 逐字相同 + 原文确认；
    但"多目标时哪个先被判定"取决于框向量顺序（NMS 分数排序后），未进一步追。

---

# (E) 一句话工程可迁移结论（面向自己的实现）

如果你的实现"压枪差、瞄准差"，从 AimMagic 反编译能直接抄的**结构性**设计是：

1. **把"检测 → 决定 → 执行"放在同一 tick 的同一线程**，不要用两个线程 + 队列。
2. **首枪不要走延迟门**；延迟门只在"动作发生之后"写入，并让初值为 0。
3. **控制律用纯比例 + 输出端限幅**，增益是 `counts_per_pixel × 力度`；不要用"每帧最多 N counts"
   的限幅去代替限幅语义（那会切碎大位移）。
4. **让"每个新观测只消费一次"**（水位/序号比对），而不是在一个固定高频循环里重复应用同一个误差。
5. **总位移与执行频率解耦**：限速只用于保护 CPU，不用于"平滑"；平滑要显式开（`scale_delivery_ms`）。
6. **压枪用预录/可编辑的 `{rel_x, rel_y, delay_ms}` 序列 + 独立线程**，门控用 AND 组合，
   而不是把后坐力补偿塞进自瞄误差里。
7. **按钮/移动走同一个虚接口**（`+0x10` move / `+0x18` down / `+0x20` up），多源输出在上层做矢量合成；
   驱动对象只做传输，**不做缩放、限幅、插值**，并保持**相对计数**语义。
8. **一个 tick 的整批位移要背靠背一次刷完**，不要在步与步之间 sleep；需要"平滑"时再用显式的
   `scale_delivery_ms` 式分发（按已过时间比例分期），并把默认值留在 0。
9. **让"移动发送"等"按键操作在途数为 0"**（一个计数器 + 条件变量），避免按键与移动交叉乱序。
10. **压枪位移注入在控制器下游**（与自瞄输出相加后再发一次），而不是加进自瞄误差 —— 否则两条
    反馈回路会互相积分成振荡。
11. **压枪线程的每步 delay 用"定时条件变量"而非 Sleep**，这样抢占/停止/新弹道都能立即打断。
12. **屏蔽类功能放设备侧**（让盒子过滤物理鼠标按键），不要做 OS 层 Hook。
