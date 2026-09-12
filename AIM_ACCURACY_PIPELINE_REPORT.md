# AimMagic 1.0.30 「瞄准精度/手感」全链路深度分析报告

> 语料：`C:\Users\Administrator\Desktop\AimMagic_RE\v1030\`（只读）
> 目标版本：AimMagic 1.0.30（Qt6/QML + MSVC，SizeOfImage 0x1406000，Ghidra 反编译）
> 对照基线：`v1030/ANALYSIS_v1030.md`（下称"主报告"）、`v108/ANALYSIS.md`
> 本报告只聚焦**瞄准精度链路**：目标关联/选靶 → 瞄点 → 控制器 → 预测补偿 → 参数默认值 → 端到端延迟结构。
> 写作约定：**【实证】**= 反编译代码/常量字节直接可得；**【推断】**= 由代码结构推出的语义，未经运行时验证。

---

## 0. 结论速览（先看这段）

我的实现是"**教科书时间维 PID**：`u = dt·Kp·[e + Ki·∫e + Kd·e']`，配一个在线估计 counts-per-pixel 的**前馈**"。
AimMagic 1.0.30 与它在瞄准精度上的差距**不在 PID 公式本身**（它的公式和我的几乎一样，甚至更简单），而在于**PID 外面那一整圈工程**：

| # | 机制 | 我的实现 | AimMagic | 为什么影响精度 |
|---|---|---|---|---|
| M1 | **选靶** | 每帧选分数最高的框（无记忆） | **trackId 持久化 + 最近邻 + 粘滞回退**（`FUN_140088ab0`/`FUN_140089ac0`） | 消除重叠框之间的抖动；目标短暂丢失不甩镜 |
| M2 | **预测** | 前馈项在**我自己的**回路里，标定不准就极限环 | 预测**在选靶之前就地改写框中心**，形成"提前量管道"（`FUN_14006e470`） | 预测误差被选靶的最近邻判据吸收，不进反馈环 |
| M3 | **误差定义** | 框中心 − 准星 | **瞄点（框内可调位置）− 准星**，瞄点由 `aim_pos`/FOV/跟随轴决定 | 瞄点可标定到"人眼认为的命中点"，而不是几何中心 |
| M4 | **内环频率** | 帧率（推理帧） | **1 kHz 定时内环持续复用最近一次检测**（`FUN_140067000` 行 269-331） | 采样间误差被连续积分，到达误差的距离显著更短 |
| M5 | **增益调度/防超调** | 固定 Kp | `kp_floor` 在**大误差时才切到小增益**（实际是"收敛增益"） | 最后几像素不乱跳；大误差段仍然快 |
| M6 | **微分/滤波** | 教科书 Kd·e' | `1−exp(−dt·40π)` 帧率无关一阶低通 + 首拍归零 + **去量化死区** | 240fps/144fps 手感一致；微分不放大检测抖动 |
| M7 | **积分门控** | 常开 | 积分被 warmup/瞄准键/目标有效三重门控（`+0xFC/+0xFD/+0xFE`） | 松键/失靶瞬间不残留 windup 甩镜 |
| M8 | **取整** | 先取整再算 → 死区 | **取整零头攒到下一拍**（`runtime+0xCD0/+0xCD8`） | 小于 1 个鼠标计数/拍的误差最终也会被消掉 |

一句话：**它把"准"和"稳"分别放在了控制环之外（选靶/瞄点）和反馈环之内（粘滞/门控/余数累加），并且用 1 kHz 内环在两次推理之间持续闭环。** 我的实现把所有这些都塞进了 PID，于是 PID 一调快就抖、一调稳就慢。

---

## 1. 目标关联 / 选靶

### 1.1 关联：IoU 匹配 + 类别必须一致（`FUN_140089ac0`，RVA 0x89AC0，550 行）

入口：`主报告 §4` 确认主干线 `FUN_1400824e0`（行 219）直接调用：
```c
uVar18 = FUN_140089ac0(param_1 + 0x128, &local_818, local_858, plVar39);
//                     ↑track表对象        ↑输出框         ↑NMS输入   ↑cfg
```

关联主循环（`FUN_140089ac0.txt` 行 189-271）双重循环：外层遍历**本帧检测框**（stride 0x28），内层遍历**上一帧的轨迹**（stride 0x50）。

- **类别硬门**（行 199-201）：
  ```c
  if (((*(uint *)(local_178[0] + (uVar34>>5)*4) >> ((byte)uVar34 & 0x1f) & 1) == 0) &&   // 该轨迹未被占用
     (*(int *)(lVar33 + 0x18 + uVar34*0x50) == *(int *)(lVar19 + 0x14 + param_5*0x28))) // classId 必须相等
  ```
  轨迹的类别在 `track+0x18`，检测框类别在 `box+0x14`（= index 5）。**不同类别的框永远不会互相匹配** → 类别切换不会造成 ID 串味。
- **被占用轨迹不重复匹配**：一个 `visited` 位图（`local_178`）保证一对一。
- **匹配分数**：行 202-267 是标准 **IoU**（`inter/(union)` 形式；行 257-263 就是 `inter/union`，且 `union<=0` 时置 0），行 264：
  ```c
  if (fVar39 < fVar38) { uVar20 = uVar34; fVar39 = fVar38; }   // 取 IoU 最大的轨迹
  ```
  `fVar39` 初值 = `*(float*)(local_res20 + 0x4c)`，即 `cfg + 0x4C` = **`tracking_iou_threshold`**（装载点见 §5）。也就是 **IoU 必须超过阈值才算匹配**，等价于"门控 + 最佳匹配"。

### 1.2 轨迹的生命周期：`min_hits` / `max_age`（`+0x44` / `+0x48`）【实证】

这是**时序持久性**（temporal persistence）的直接证据，`FUN_140089ac0.txt` 行 441-507 的"未匹配轨迹"清理段：

```c
*(int *)(puVar32 + uVar31*10 + 6) = *(int *)(puVar32 + uVar31*10 + 6) + 1;   // track.missed++ （+0x30）
if ((*(int *)(param_4 + 0x44) <= *(int *)(lVar19 + 0x2c + puVar32)) &&       // track.hits >= min_hits
    (*(int *)(puVar32 + uVar31*10 + 6) <= *(int *)(param_4 + 0x48)))         // track.missed <= max_age
{
    ...  // 用轨迹自己的速度把框中心往前推，并入输出
}
```
- `track+0x2C` = **命中计数**（行 407-408 每匹配一次 `++`）
- `cfg+0x44` = `min_hits`，装载默认 **3**（`FUN_14001dc10` 行 324）
- `cfg+0x48` = `max_age`，装载默认 **5**（`FUN_14001dc10` 行 326）
- 行 509-542 是 `max_age` 裁剪（STL `remove_if` 风格），把超龄轨迹从容器里删除。

**【实证结论】** 一个框必须**连续被关联 ≥3 帧**才会被输出给瞄准；丢失后**最多沿用 5 帧**。这就直接消掉了"单帧误检当作目标"和"框闪一下准星就跳走"。

### 1.3 选靶：不是"每帧最高分"，而是"最近邻 + 类别 + 可选分数项"

选靶核心在 **`FUN_140088ab0`（RVA 0x88AB0）**，由 `FUN_140088a30`（RVA 0x88A30，一行转发）调用；调用点 `FUN_1400824e0` 行 675：
```c
FUN_140088a30(puVar38 + 0x140, puVar38 + 0x90, param_1 + 0x59, puVar38 + 0x640);
//               ↑选择结果结构   ↑本帧框向量   ↑轨迹表        ↑预测表
```
> **方法说明**：`FUN_140088ab0` / `FUN_1400885f0` / `FUN_140088790` / `FUN_140089660` / `FUN_140089800` 这 5 个函数**没有独立反编译文件**（`funcs/` 里不存在），我在 `v1030/dump/000140001000_001EE000_p20_t1000000.bin` 中按 RVA 取原始字节手工反汇编得到下面结论。

**A. 候选过滤（类别 + 活跃轨迹）**（0x88BC8-0x88C44，与调用者传的 `param_1+0x59` 轨迹表配合）：
```asm
48 8B 97 58 01 00 00   mov  rdx,[rdi+0x158]
48 8B 8F 50 01 00 00   mov  rcx,[rdi+0x150]      ; 轨迹表 begin/end
48 3B CA               cmp  rcx,rdx
74 21                  je   +                     ; 空表 → 跳过
45 8B 46 14            mov  r8d,[r14+0x14]        ; 当前框 classId
E8 94 01 15 00         call 140089660             ; ← 按 trackId(classId) 在轨迹表中查找
48 3B 87 58 01 00 00   cmp  rax,[rdi+0x158]
0F 84 F7 05 00 00      je   跳过该框               ; 没找到活跃轨迹 → 该框不可选
80 BF 1B 01 00 00 00   cmp  byte [rdi+0x11B],0    ; AimKeyCfg.prediction_enabled
```
即：**只有存在对应活跃轨迹的检测框才参与选靶**。这是"用跟踪 ID 而不是每帧最高分"的第二重证据（第一重是 §1.2）。

**B. 尺寸权重（size scoring）**（0x88C20 附近，直接可见两次 SSE 比较/乘法 + `maxss`）：
```asm
F3 45 0F 10 46 08      movss xmm8,[r14+8]         ; 框宽 w
F3 10 4F 40            movss xmm1,[rdi+0x40]      ; size_scoring_weight
F3 41 0F 59 82 80000000 movss xmm0,[r10+0x80]     ; 阈值 A（配置块）
F3 41 0F 59 82 84000000 movss xmm0,[r10+0x84]     ; 阈值 B
F3 0F 59 C1 / F3 0F 5F C8 / F3 41 0F 59 C1        ; w * weight 等乘法
```
`size_scoring_weight` / `distance_scoring_weight` 由 `FUN_1400824e0` 行 659-671 从配置读取：
```c
uVar45 = FUN_1400635e0(lVar44, "size_scoring_weight", 0);      // 默认 0
uVar46 = FUN_1400635e0(lVar44, "distance_scoring_weight", fVar8); // fVar8 = DAT_1401f4634 = 1.0
```
**【推断】** 打分 = `size_scoring_weight·w − distance_scoring_weight·d²`，默认 `size=0, distance=1.0` → **默认完全按"到准星的距离平方"最小来选**。

**C. 距离 = 预测后框中心到准星的欧氏距离**（0x88D40 一段，可辨 `maxss`/`mulss`/`subss`/`sqrtss` 序列，选择变量在栈 `[rsp+0x38]`/`[rsp+0x40]`/`[rsp+0x6C]`）。

**D. 粘滞/回退（stickiness）**——**这是"两个重叠框之间不抖"的最直接原因**（`FUN_1400824e0` 行 680-720）：
```c
puVar38[0x60] = 0;
pfVar42 = *(float **)(puVar38 + 0xd0);           // 本帧框向量（=FUN_140063920 拷贝的 NMS 输出）
if (puVar38[0x184] != '\0') {                    // 目标有效
  pfVar34 = *(float **)(puVar38 + 0xd8);
  if (*(int *)(puVar38 + 0x158) < 0) {           // 无有效"当前 track id"
LAB_1400836ca:
    pfVar43 = pfVar24;                           // NULL
    if (pfVar42 != pfVar34) {
      pfVar24 = pfVar42;
      fVar51 = DAT_1401f9aa4;                    // = 3.402823e38  (FLT_MAX)
      do {
        if ((pfVar24[5] == *(float *)(puVar38 + 0x154)) &&              // classId == 选中类别
           (fVar53 = (pfVar24[1]-*(float*)(puVar38+0x144))*(pfVar24[1]-*(float*)(puVar38+0x144))
                   + (*pfVar24 -*(float*)(puVar38+0x140))*(*pfVar24 -*(float*)(puVar38+0x140)),
            fVar53 < fVar51)) {                  // 平方距离 < 当前最优
          pfVar43 = pfVar24; fVar51 = fVar53;     // ← 最近邻
        }
        pfVar24 = pfVar24 + 10;                  // stride 0x28
      } while (pfVar24 != pfVar34);
      if (pfVar43 != 0) goto LAB_140083736;      // 找到 → 用它
    }
    pfVar43 = (float *)(puVar38 + 0x140);        // ← 找不到任何框：沿用上一拍的瞄点！
  }
  else {
    FUN_140063320(puVar38 + 0x68, pfVar42, pfVar34, puVar38 + 0x140);
    // ↑ 线性查找"trackId == 上一帧选中的 trackId"的框：线性粘滞
    pfVar43 = *(float **)(puVar38 + 0x68);
    if (pfVar43 == pfVar34 || pfVar43 == 0) goto LAB_1400836ca;   // 找不到才退回最近邻
  }
LAB_140083736:
  fVar51 = pfVar43[2] * fVar50;                  // 框宽 × aim_scope
  if ((*pfVar43 - fVar51 < fVar52) && (fVar52 < *pfVar43 + fVar51)) {
    fVar50 = pfVar43[3] * fVar50;                // 框高 × aim_scope
    fVar51 = pfVar43[1];
    if ((fVar51 - fVar50 < fVar54) && (puVar38[0x60] = 1, fVar54 < fVar51 + fVar50))
      goto LAB_14008378f;                        // 准星在 FOV 内 → in-scope
  }
  puVar38[0x60] = 0;
```
三个独立机制叠在一起：
1. **首选 trackId 一致**（线性粘滞）—— 上一帧选中谁，只要它还在，就继续选它；
2. 只有 **trackId 对应不上时** 才做**最近邻**（判据里**没有** score/confidence，也没有"最高分"）；
3. 连最近邻都找不到时，**直接沿用上一拍的瞄点坐标**（`pfVar43 = puVar38+0x140`），即"**锁定位置记忆**"，而不是把准星丢开。

**【实证结论】** 目标切换只发生在：(a) 原 track 的 `max_age` 内没再被关联到，(b) 或者出现了 trackId 不同且距离更近的框 **且** 旧 trackId 已经不在。这是一个**带迟滞的离散选择器**，天然没有"两框各占一半"的抖动。

### 1.4 FOV / 瞄准框形状（`aim_bot_scope` / `aim_bot_inner_scope` / `aim_scope_mode`）

- `FUN_140076720`（RVA 0x76720）是**"准星是否落在某个框的瞄准范围内"**的判定器：
```c
if (param_2 的字符串 == "bubble")                       // 6 字节比较 'bubble'
    cVar3 = FUN_1400885f0(pfVar6, param_2, param_3, param_4);   // ★ 圆形/胶囊判定分支
else {                                                   // 否则矩形分支
    fVar7 = pfVar6[2] * 0.5f;                            // 框宽 * 0.5
    if ((*pfVar6 - fVar7 < param_3) && (param_3 < *pfVar6 + fVar7)) {
        fVar7 = pfVar6[3] * 0.5f;                        // 框高 * 0.5
        if ((pfVar6[1] - fVar7 < param_4) && (param_4 < pfVar6[1] + fVar7)) return 1;
    }
}
```
- **`aim_scope_mode` 默认 `"circle"`**（`FUN_14001ba90` 行 602-604）：
  ```c
  FUN_1400069f0(&local_340, "circle", 6);                     // 默认值
  uVar11 = FUN_140017cb0(local_2d8, ppuVar13, "aim_scope_mode", &local_340);
  FUN_140007ac0(local_298, uVar11);                           // 存入 0xC8(?) 字符串槽
  ```
  ```
  而 `FUN_1400885f0`（RVA 0x885F0，手工反汇编）确认了圆判定：
  ```
  cmp qword [rdx+0x28], 0x0F / lea rax,[rdx+0x10] / mov rcx,[rax+0x10]
  cmp rcx, 6 / jne 矩形分支
  cmp dword [rax],'bubb' ; cmp word [rax+4],'le'            ; "bubble"
  movss xmm1,[rdx+0x30]                                     ; params
  movss xmm0,[r8+0x08] ; mulss xmm1,[r8+0x08]               ; × frame 字段
  ... 类似 xmm2/xmm3 用 [rdx+0x34] 与 [r8+0x0C]
  ```
  ```
  → **默认是"圆（或按框尺寸缩放的椭圆）"判定**，不是矩形；参数同在一个函数里分派（外部字符串 == "bubble" 时走圆）。
- `bubble_width` / `bubble_height` 默认 **1.0 / 1.0**（行 607-608，`DAT_1401f4634`=1.0）。

### 1.5 「为什么它比我那种实现选靶更稳」小结

| 现象 | 机制（文件/行） |
|---|---|
| 重叠两框之间不左右横跳 | trackId 线性粘滞（`FUN_1400824e0` 行 707 + `FUN_140063320`）|
| 目标被短暂遮挡/漏检不甩镜 | `max_age=5` 内轨迹继续输出（`FUN_140089ac0` 行 448-450）|
| 单帧误检不抢镜 | `min_hits=3` （行 448）|
| 远处噪点不抢镜 | 最近邻 + FOV 门控（行 691-716）|
| 完全没框时不产生零误差假象 | 沿用上一拍瞄点（行 703）|

我的"每帧取最高 confidence 框"在这五条上全部为 0。

---

## 2. 瞄点（框内到底瞄哪里）

### 2.1 QML 侧的瞄点参数（`瞄准参数-按键配置-目标类别子页`）

`qml_z\瞄准参数-按键配置-目标类别子页_EvolveUI_风格__.qml`：

| QML 字段 | 行 | 默认 | 范围 | 含义 |
|---|---|---|---|---|
| `aim_pos[0]`（UI 名 `aim_min` / "瞄点上限"） | 215-255 | **0.10** | 0..1 | 框内 X 比例 |
| `aim_pos[1]`（UI 名 `aim_max` / "瞄点下限"） | 256-295 | **0.10** | 0..1 | 框内 Y 比例 |
| `class_confidence` | 318-326 | 0.0 | 0..1 | 每类置信度 |
| `weight` | 329-336 | 0.0 | 0..100 | 每类权重 |
| `trigger.x_scope` | 339-345 | 1.0 | 0..2 | 扳机范围 X |
| `trigger.y_scope` | 346-352 | 1.0 | 0..2 | 扳机范围 Y |
| `trigger.x_offset` | 353-359 | 0.0 | −1..1 | 扳机偏移 X |
| `trigger.y_offset` | 360-366 | 0.0 | −1..1 | 扳机偏移 Y |

> 注释原文（第 215 行）：`// aim_pos = [aim_min, aim_max]（array[2]，默认 [0.1, 0.1]），双写`
> **【实证】** 瞄点是一个 **2 元素比例对**，独立于框中心。
> **【推断】** `0.5/0.5` = 几何中心，`0.5/0.1` ≈ 头部，`0.5/0.0` ≈ 框顶。这是"瞄头 / 瞄胸口 / 瞄中心"的可标定接口。

### 2.2 C++ 侧瞄点：`FUN_140089660`（RVA 0x89660，手工反汇编）

调用点（`FUN_1400824e0` 行 675 → 内部）：`FUN_140089660(局部缓冲, 框数, &cfg, 输出)`，其机器码（`dump` 0x88940 起）逐条可读：

```asm
movss  xmm3,[rcx+0x0C]      ; box.h
movss  xmm2,[rcx+0x08]      ; box.w
mulss  xmm2,[rip+0x16BCA9]  ; × 0.5          ← DAT_1401f462c = 0.5
movaps xmm4,xmm2            ; w*0.5
addss  xmm3,xmm2            ; h + w*0.5      (寄存器复用，见下注)
...
movss  xmm7,[rdx+0x0C]      ; cfg[i].y  比例
movss  xmm6,[rdx+0x08]      ; cfg[i].x  比例
subss  xmm5,xmm4            ; center+0.5w - 0.5w
subss  xmm2,xmm6 ...        ; 比例加权
movss  [rax+0x08],xmm3
movss  [rax+0x18],xmm1
```
**【推断，置信度中】** 结论是干净的：**输出点 = 框中心 ± 框尺寸 × 比例**，比例来自 `AimKeyCfg + 0x124 / +0x128`（正是 §5 里 `aim_pos[0/1]` 的落点，`FUN_14001ba90` 行 617-618 把 `local_260/local_25c` 同时写进 `local_124/uStack_120`）。
保守表述：**瞄点 = f(box.cx, box.cy, box.w, box.h, aim_pos[0], aim_pos[1])**，含 0.5·尺寸 的缩放项；具体符号（`center − w·aim_pos` vs `top + h·aim_pos`）未能 100% 定死，但不改变"瞄点是框内可调比例点"这一结论。

### 2.3 误差 = 瞄点 − 准星（**这是 PID 的真正输入**）【实证】

`FUN_1400824e0` 行 907-911：
```c
if (cVar9 != '\0') {                                     // 目标有效
    *(undefined4 *)((longlong)param_1 + 0xebc) = *(undefined4 *)(puVar38 + 0x170);
    *(undefined4 *)(param_1 + 0x1d8)          = *(undefined4 *)(puVar38 + 0x174);
    *(float *)((longlong)param_1 + 0xec4) = *(float *)(puVar38 + 0x168) - fVar52;  // ★
    *(float *)(param_1 + 0x1d9)           = *(float *)(puVar38 + 0x16c) - fVar54;  // ★
    ...
}
```
`puVar38+0x168/0x16c` = 选靶产出的**瞄点**（见 §1.3 的 `pfVar43[0]/[1]` 及其下游）；
`fVar52/fVar54` = **准星坐标**（由 §2.4 的 `FUN_14007d4c0` 产出）。
→ **`runtime+0xEC4/0x1D9` = 瞄点 − 准星**，随后被 `FUN_140067000` 读作 PID 的 `param_3/param_4`（见 §3.6）。

> 这就是"比我准"的第一层：我的误差是"框中心 − 准星"，它是"**框内我指定的一点** − 准星"。瞄头/瞄胸的差别是系统性的、可标定的偏置，我的实现无法表达。

### 2.4 动态 FOV：立方 ease-out 的"开镜/收镜"过渡（`FUN_14007d4c0`，RVA 0x7D4C0，490 行）【实证】

参数读取（`FUN_14001ba90` 行 609-611）：
```c
uVar9 = FUN_1400172b0(ppuVar13, "aim_bot_fov_transition_ms", 0);
local_270 = FUN_1400172b0(ppuVar13, "aim_bot_fov_expand_transition_ms", uVar9);
local_26c = FUN_1400172b0(ppuVar13, "aim_bot_fov_shrink_transition_ms", uVar9);
```

`FUN_14007d4c0` 的状态机（行 371-468）：
```c
if (param_5 != '\0') {                        // 目标 FOV 切换（瞄准键按下/松开）
    /* 字符串 = AimKey 名；若变了 → 当前 FOV 直接跳到目标值，重置计时 */
}
if (*(longlong*)(param_1+0xbd8) == lVar9   /* 同一把瞄准键 */
    && *(longlong*)(param_1+0xbd8) != 0) {
    fVar20 = *(float *)(param_1 + 0xbf0);                 // fov_cur
    if (0.1f < fabsf(fVar21 - fVar20)) {                  // 0.1 = DAT_1401f5b74：死区，防抖
        *(float *)(param_1 + 0xbec) = *(float *)(param_1 + 0xbe8);   // fov_base ← fov_prev
        *(float *)(param_1 + 0xbf0) = fVar21;                        // fov_cur  ← fov_target
        iVar3 = (fov_base < fov_cur) ? cfg.expand_ms : cfg.shrink_ms;  // ★ 张开/收拢 用不同时长
        *(int *)(param_1 + 0xbf4) = iVar3;
        *(longlong *)(param_1 + 0xbf8) = lVar9;                      // 记录起始时间戳
        fVar20 = fVar21;
    }
    if (0 < *(int *)(param_1 + 0xbf4)) {
        dVar2 = ((double)(lVar9 - *(longlong*)(param_1+0xbf8)) / 1e6)   // 已过毫秒
                / (double)iVar3;                                        // ÷ 过渡时长
        t = clamp(dVar2, 0.0, 1.0);                                     // 三重比较夹取
        fVar19 = 1.0f - t;
        *(float *)(param_1 + 0xbe8) =                                    // ★ fov_out
              (1.0f - fVar19*fVar19*fVar19) * (fVar20 - *(float*)(param_1 + 0xbec))
            + *(float *)(param_1 + 0xbec);
    }
}
```
即 **`a(t) = 1 − (1−t)³`，`FOV(t) = base + a(t)·(target − base)`** —— 立方 ease-out（起步快、收尾极软），且**张开/收拢各有独立时长**，并带 0.1 死区与"同一时机不重启"逻辑。

FOV 的消费点（`FUN_1400824e0` 行 466-518）：
```c
fVar51 = (float)目标框宽; fVar53 = (float)目标框高;
fVar50 = 0.5f; fVar52 = 1.0f; fVar54 = 1.0f;
if (0 < *piVar19)  fVar52 = fVar51 / (float)*piVar19;   // *= 目标框宽/屏宽
if (0 < piVar19[1]) fVar54 = fVar53 / (float)piVar19[1];
if (...aim_follow_x && aim_follow_y 命中...) {
    fVar52 = (float)*(int *)(param_1 + 0x174) * fVar52;  // × aim_bot_fov
} else {
    fVar52 = fVar51 * 0.5f;                              // 回退：半个目标框
}
fVar54 = (cVar10 != 0) ? (float)(int)param_1[0x2f] * fVar54 : fVar53 * 0.5f;
uVar11 = FUN_140076720(puVar38 + 0x90, param_2, fVar52, fVar54);   // ← §1.4 的 FOV 判定
```
`DAT_1401f462c = 0.5`、`aim_follow_x/y` 由 `FUN_14007b0c0(obj,"aim_follow_x",1)` 读取（行 484-486，**默认 1**）。

### 2.5 动态 FOV / 增益调度：**"瞄准框随误差收缩"这一假设——不成立**【重要澄清】

我原本的假设是"瞄准框随误差减小而收缩 = 增益调度"。**代码里没有这个机制**：
- FOV 只由**瞄准键按下/松开**驱动（`param_5`），过渡时长是常数（expand/shrink ms），**与误差大小无关**；
- PID 增益也没有随误差连续调度（只有 §3.3 的 kp_floor 二值切换）。

**真正存在的"动态"是另外三层**，且都在别的维度：
1. **FOV 时间过渡**（上面）——防的是"开镜瞬间框突然张开导致选靶跳变"；
2. **预测权重随目标尺寸连续调度**（§4.2 的 `sizeWeight`，0→0.75 连续变化）——这才是"随目标大小自适应"的增益调度；
3. **kp_floor 的二值切换**（§3.3）——大误差 vs 小误差两档。

---

## 3. 控制器控制律（`FUN_140056f10` / `FUN_1400579f0`）

源码位置：**这两个函数没有 `funcs/*.txt`**，完整反汇编 + C 伪码在
`v1030\ghidra_out\pid_core.txt`：`FUN_140056f10` @ 第 24-290 行（反汇编）/ 304-531 行（C），`FUN_1400579f0` @ 第 545-745 / 757-917 行。

### 3.1 结构体与轴偏移（单轴状态机）

`FUN_140056f10(param_1 /*PID state*/, char axis /*'x'=0x78, 'y'=0x79*/, double e, double dt)`
（伪码行 304；轴选择 `bVar16 = param_2 != 'x'` 行 335）

**【实证】** 由反汇编里成对出现的 `CMOVNZ` 常量直接读出（`pid_core.txt` 行 40-77）：

| 字段 | x | y | 说明 |
|---|---|---|---|
| Kp（double） | +0x00 | +0x18 | 比例 |
| Ki（double） | +0x08 | +0x20 | 积分 |
| Kd（double） | +0x10 | +0x28 | 微分 |
| Kf（double） | +0x30 | +0x38 | 前馈（仅 Adaptive/Free 分支使用） |
| 积分限幅 `i_clamp` | +0x48 | +0x50 | 默认 6.0 |
| kp_floor | +0x58 | +0x60 | 默认 0.05 |
| inhibit | +0x68 | +0x70 | int，`==1` 时禁用微分滤波 |
| 微分权重 pw（double） | +0x70 | +0x78 | 0.02 `:` 0.2 `:` 10.0（见 §3.4） |
| `max_output` | +0x80 | +0x88 | 默认 9999.0 |
| 二次滤波状态 | +0x88 | +0x90 | 仅 inhibit 分支写 |
| lastErr | +0xB8 | +0xC0 | |
| 积分累加器 | +0xC8 | +0xD0 | |
| 微分滤波状态 dFiltered | +0xD8 | +0xE0 | |
| 微分滤波残差 α | +0xE8 | +0xF0 | 默认 0.7 |
| 帧计数 fcount | +0xF8 | — | int |
| `integrateEnabled(aimActive)` | +0xFC | — | bool |
| `integrateReset` | +0xFD | — | bool（Adaptive 用） |
| `integrateArmed(aimKeyDown?)` | +0xFE | — | bool |
| profile 字符串 | +0x90(ptr)/+0xA0(len)/+0xA8(cap) | — | size 字段实测在 +0xD0 |

> 轴对的顺序是 **x 在低偏移、y 在高偏移**，Ghidra 的 `lVar6 = 0x20 (y) / 0x8 (x)` 就是这个（伪码行 340-343）。

### 3.2 积分项：门控、累加、限幅【实证】

伪码行 404-410 / 421-424 / 525-528：
```c
cVar5 = (*(char*)(P+0xFC) != 0) && (*(char*)(P+0xFE) != 0);      // integrateEnabled = aimActive && armed
if (isAdaptive && *(char*)(P+0xFD) == 0) *pdVar14 = 0.0;         // Adaptive：未 armed 则清积分
if (cVar5) {
    dVar21 = Kp_ki /*Ki*/ * param_3 /*e*/ * param_4 /*dt*/ + *pdVar14;
    *pdVar14 = dVar21;                                            // 行 423-424
    if (profile != "PID-Free" && profile != "PID-Adaptive") {      // 行 429-439
        limit = *(double*)(P + (x?0x48:0x50));                     // i_clamp
        if      (+limit <= dVar21) integral = 0.0;                 // 行 450-452（CMOVBE RSI）
        else if (dVar21 <= -limit) integral = -limit;              // 行 454-456
        else                       integral = dVar21;
        *pdVar14 = integral;                                       // 行 457
    }
}
```
- **积分只在 `+0xFC && +0xFE` 时才累加**（不是常开）；
- `+0xFC` 由**双轴包装层**每拍写入：`FUN_1400579f0` 行 856-857 `*(char*)(P+0xFC) = uVar5`；
  `uVar5=0` 的条件（伪码行 845-850）：`profile ∈ {Free, Adaptive} 且 fcount < cfg+0x68` → **warmup 帧数内不积分**；
- **限幅确实是无条件生效的**（主报告 §2.4 说"只有 Free/Adaptive 不夹积分"与代码相反，以本报告为准）。
- **`i_box_lost_hold_ms`（默认 20）** 不在这里 —— 它在**上层**控制 `+0xFE`（"框丢失后还能积多少毫秒"），见主报告 §2.5 与 §5 表。

### 3.3 kp_floor ——**不是"小误差保底增益"，而是"大误差切小增益"**【实证，且与直觉相反】

反汇编（`pid_core.txt` 行 99-108）：
```asm
140057042  ANDPS  XMM2, [0x1401f6360]     ; XMM2 = |e|   （0x1401f6360 = 0x7FFFFFFFFFFFFFFF 掩码）
140057055  ANDPS  XMM0, [0x1401f6360]     ; XMM0 = |kp_floor|
140057064  COMISD XMM0, XMM2
140057068  JBE    +0x05                   ; if (!(floor <= |e|)) 跳过
14005706A  MOVSD  XMM7, [RAX+RBX]         ; ★ Kp ← kp_floor
```
条件：只有 **`profile == "PID-Adaptive"`（长度 0xC 比较，行 380-399）** 且 **`|kp_floor| <= |e|`** 时才把 Kp 换成 kp_floor（`COMISD/JBE` 的组合我已按标志位逐条核对）。

语义（结合 QML 第 204 行注释原文）：
> `// Free 用作启动Kp，Adaptive 用作收敛Kp；同步控制器只使用唯一Kp。`

即 **kp_floor = 收敛增益**：误差大时用**较小的** kp_floor 抑制超调（防止"冲过去再回来"的极限环），进入小误差区后回到正常 Kp 去啃最后几像素。
**我的假设（"保证最小增益以便咬住最后几像素"）方向相反**——代码里它是**大误差段的增益**。真正负责"咬住最后像素"的是下面的取整余数机制（§3.7）。

### 3.4 微分项：`1 − exp(−dt·40π)` 到底作用在哪

反汇编 0x140057174-0x1400571CB + 0x140057288-0x1400572BF（`pid_core.txt` 行 173-255）：
```asm
140057174  CMP dword [RBX+0xF8], 1
140057199  JNZ  +                    ; fcount == 1 → 跳到首拍特例
14005719B  XORPS XMM6, XMM6          ; ★ 首拍：dTerm = 0（微分完全禁用一拍）
14005719E  JMP  0x1400572BF
...
1400571B8  DIVSD XMM9, XMM14         ; XMM9 = (e - lastErr) / dt          ← 逐帧差分
1400571BD  MULSD XMM9, XMM5          ; × Kd
1400571CB  MOVSD XMM6, [0x1401f6358] ; XMM6 = 1.0
1400571D9..0x140057288 :  profile ∈ {FrameSync, EventSync, Kalman}
140057288  MOVAPS XMM0, XMM14  ; dt
14005728C  MULSD  XMM0, [0x1401f8030] ; × (−125.66370614359172) = −40π
140057294  CALL   0x1401daf1a        ; ← IAT thunk → ucrtbase `exp`（主报告 §2.3 已坐实）
140057299  MOVAPS XMM1, XMM6          ; 1.0
14005729C  SUBSD  XMM1, XMM0          ; ★ dw = 1 − exp(−40π·dt)
1400572A0  JMP    0x1400572A6
1400572A2  MOVAPS XMM1, XMM11         ; 非同步档：dw = DAT_1401f8010 = 0.3（固定每帧权重）
1400572A6  SUBSD  XMM6, XMM1          ; (1 − dw)
1400572AA  MULSD  XMM9, XMM1          ; ((e−lastErr)/dt · Kd) · dw
1400572AF  MULSD  XMM6, [RBX+RBP]     ; (1−dw) · prev_dTerm        ← RBP = 0xD8(x)/0xE0(y)
1400572B4  ADDSD  XMM6, XMM9
1400572B9  (恢复 XMM9)
1400572BF  MOVSD  [RBX+RBP], XMM6     ; ★ dTerm := 结果（存回状态）
```
**结论（逐条回答提问）：**
1. **滤波器作用在"微分项"上，不是误差也不是输出**。被滤波的量是**原始逐帧差分导数** `(e−e_prev)/dt·Kd`。
2. 形式：`dTerm_k = (1−dw)·dTerm_{k−1} + dw·(Kd·Δe/dt)`，其中 `dw = 1 − e^{−40π·dt}`。
3. `40π = 125.6637` → 一阶低通**截止频率 20 Hz**，`τ = 1/(40π) ≈ 7.96 ms`。dt 变大 → dw 变大 → 保持等效模拟带宽不变 ⇒ **帧率无关**（"FrameSync"字面含义）。
4. **首拍（fcount==1）dTerm 直接置 0** —— 瞄准开始瞬间不产生微分冲击（防"开镜第一下一抖"）。
5. 非同步档（Free/Adaptive）退化为固定 `dw = 0.3`，每帧权重，帧率变了手感会变——这正是 1.0.30 要重做的地方。
6. **量化死区（重要，主报告未提）**（反汇编 0x1400572D1-0x1400572FE）：
```asm
1400572D1  MOVSD XMM0, [R14+RBX]        ; 二次滤波状态
1400572D7  MULSD XMM0, [0x1401f8018]    ; × 0.7
1400572DF  DIVSD XMM8, XMM14            ; XMM8 = (e − lastErr)/dt = dyNorm
1400572E4  MULSD XMM8, XMM11            ; × 1.0
1400572E9  ADDSD XMM8, XMM0
1400572EE  MOVSD [R14+RBX], XMM8
1400572F4  MOVSD XMM6, [RBX+RBP]
1400572F9  MULSD XMM12, XMM8
140057300  XORPS XMM12, XMM12           ; ★ inhibit 路径下 dTerm → 0
```
`Kf(+0x30/+0x38)` 乘的是这个 0.7 一阶低通后的导数 `XMM8`，且 **只在"inhibit"路径**才有非零贡献；而 **同步档（FrameSync/EventSync/Kalman）走 `140057300 → XMM12 = 0`，前馈项恒为 0**。
→ 所以 **QML 里 `x_kf` 只对 `PID-Adaptive` 可见**（`当前_PID_配置档位*.qml` 行 194：`visible: root.currentProfile === "PID-Adaptive"`）与代码完全自洽。

### 3.5 返回式与输出限幅

**单轴返回**（伪码行 529-530 / 反汇编 0x14005733F-0x14005737F）：
```c
return  Kp_eff * e                    // XMM7 × XMM13(=e)   行 278
      + integral_active ? integral : 0 // XMM10（+0xFE≠0 时取积分，否则 0）行 269-277/280
      + dTerm                          // XMM6（3.4 的滤波结果）
      + dVar2 /* = Kf·filtered(0.7·dy)（非同步档）或 0（同步档） */ ;
```
注意：**同步档的 Kf 项 = 0**，即默认档位是 `u = Kp·e + I + D_filtered`，**没有前馈**。这与我的实现（教科书 PID + 在线 counts-per-pixel 前馈）形成鲜明对比：
AimMagic 的"前馈"在默认配置下**根本不参与瞄准闭环**，它的"提前量"全部由 §4 的**预测改框**承担。

**双轴包装层 `FUN_1400579f0`（伪码行 757-917）另做三件重要的事：**

**(a) 输出限幅（`max_output_x/y`）**（反汇编 0x140057C1A-0x140057CC3）：
```asm
MOVSD XMM3,[RBX+0x80] ; |max_output_x|
XMM2 = −|max_output_x| ; XMM6 = pid_y
; 三个候选放进栈：[-limit, +limit, raw]，用两次 COMISD/CMOVBE 做 clamp
; 结果：
*param_2     = clamp(pid_x, −max_output_x, +max_output_x);
param_2[1]   = clamp(pid_y, −max_output_y, +max_output_y);
```
→ **单拍位移硬限幅**，默认 9999.0（等于不限），但调小它就是"单帧步长钳制"（防甩飞）。

**(b) 记录状态**（行 894-896）：
```c
*(uint *)(param_1 + 0x17) = (uint)param_3;             // 本拍误差原始位（x）
*(uint *)((longlong)param_1 + 0xbc) = uVar11;          // （y 的高位）
param_1[0x18] = param_4;                                // dt
```

**(c) dt 与"启动/收敛"调度**（行 873-879）：
```c
if (bVar1 && 0 < *(int *)(param_1 + 0xd)) {            // bVar1 = Free/Adaptive 且 fcount < cfg+0x68
    dVar12 = (double)*(int *)(param_1 + 0x1f) / (double)*(int *)(param_1 + 0xd);
    *param_1         = (*param_1         - param_1[0x0b]) * dVar12 + param_1[0x0b];  // Kp:  启动→收敛
    param_1[3]       = (param_1[3]       - param_1[0x0c]) * dVar12 + param_1[0x0c];  // Ki:  启动→收敛
}
```
`param_1[0x0b]/[0x0c]` = 结构偏移 **+0x58/+0x60 = kp_floor/ki_floor 槽**。
→ **【实证，修正 QML 注释的方向】** 这里才是 kp_floor 的第二种用法：**Free/Adaptive 档在启动阶段把增益从 kp_floor 线性爬到正常 Kp**（`t = fcount/warmup`）。
→ 所以 kp_floor 的真实语义是 **"启动增益"（低）**：启动时用小增益防冲击，warmup 后爬升到主增益；而 §3.3 的 Adaptive 分支则是在**大误差**时又切回这个小增益。两句 QML 注释（"Free 用作启动 Kp / Adaptive 用作收敛 Kp"）与两处代码一一对应。

### 3.6 dt 怎么来的、尖峰怎么办、1 kHz vs 事件驱动

**dt 来源（双入口）**：
```c
// FUN_140057660 (RVA 0x57660，共 22 行)
FUN_140025f80(&local_res8);                                  // QPC → ns
lVar1 = *(longlong *)(param_1 + 0xb0);
*(longlong *)(param_1 + 0xb0) = local_res8;
dVar3 = (double)(local_res8 - lVar1) / DAT_1401f8028;        // ÷1e9
dVar2 = DAT_1401f8000;                                       // 0.001
if (dVar2 <= dVar3) dVar2 = dVar3;                           // dt = max(dt, 0.001)
FUN_1400579f0(param_1, param_2, param_3, param_4, dVar2);
```
```c
// FUN_140057d20 (RVA 0x57d20) —— 显式传入 dt，同样 max(dt, 0.001)
```
**【实证 + 重要单位说明】** 时间戳是**纳秒**、除以 `1e9` 得**秒**；下限钳到 `0.001`。但实际调用点传进来的值在**毫秒量级**：
- `FUN_140067000` 行 1109：`FUN_140057660(lVar17+0x800, puVar40+0x2d8, (double)*(float*)(puVar40+0x534), (double)*(float*)(puVar40+0x538))` —— 走 `FUN_140057660`（**自算** dt）；
- 行 1239：`FUN_140057d20(lVar17+0x800, puVar40+0x170, dVar51, dVar50)` —— 显式 dt。

因为 1 ms 定时内环每拍都调用一次，`local_res8 - lVar1 ≈ 1e6 ns = 1 ms` ⇒ `dVar3 ≈ 1e-3 ~ 3e-3`，**恰在 0.001 下限附近**，所以：
- **dt 尖峰（线程被抢占、GC、GPU 卡顿）会被下限钳住吗？不会——下限不防大尖峰。** 代码里**没有 dt 上限钳制**（只有下限）。
- 但上游有一道防线：`FUN_1400824e0` 行 616-621 在算"平台速度"时用 `FUN_140056b00(dt, &0.001, &10.0)` 夹到 **[0.001, 10.0]**（`DAT_1401f80f8 = 10.0`）。**PID 自身的 dt 没有上限**——【推断】这是 1.0.30 的一个残留弱点：一次 50 ms 的线程饥饿会让 `u` 变成平时的 50 倍（被 `max_output` 兜住）。

**1 kHz 定时内环 vs 事件驱动（`+0xE4A` / `+0xE49`）**【实证，`FUN_140067000` 行 269-331】：
```c
if ((*(char *)(lVar17 + 0xe4a) == '\0') || ((char)uVar15 != '\0')) {
    /* 普通路径（默认 PID-FrameSync / PID-Kalman） */
    FUN_140025f80(puVar40 + 0x1c8);                       // now (ns)
    lVar37 = now + 1000000;                               // ★ 截止 = now + 1 ms
    while (*(char *)(lVar17 + 0xe40) == '\0') {           // 0xE40 = 停止请求
        lVar16 = now_ns();
        if (lVar37 <= lVar16) break;                      // 到点 → 走一次迭代
        FUN_1401d9c74(lVar17 + 0xdf8, lVar45, uVar14);    // 定时 condvar 等待（剩余时间）
    }
} else {
    while (true) {                                        // 事件驱动（Scale / PID-EventSync）
        lVar37 = *(longlong *)(puVar40 + 0xd0);           // 推理序号 A
        if (*(char *)(lVar17 + 0xe49) != '\0') lVar37 = *(longlong *)(puVar40 + 0x100);  // Scale 用序号 B
        if (stop || 0xED6 || !0xE4A) break;
        if (*(longlong *)(lVar17 + 0xe50) != lVar37) break;   // ★ +0xE50 = 已消费水位
        if (*(int *)(lVar17 + 0xa78) != 0 || *(int *)(lVar17 + 0xa7c) != 0) break;
        FUN_1401d9cd7(lVar17 + 0xdf8, lVar45);            // 无限期等待，被推理线程唤醒
    }
}
```
`+0xE4A` 的写入在 `FUN_1400824e0` 行 847-869：
```c
if (controllerType.size() == 5 && strcmp(pfVar24,"Scale")==0) { isScale=1; eventDriven=1; }
else if (controllerType=="PID" && pid_profile=="PID-EventSync")  eventDriven=1;
```
`+0xE49` = isScale。

**"1 kHz 内环 + 陈旧检测" 结构（本报告最重要的延迟结论）**：
- 循环体一次 = **一遍完整的 PID 迭代**（读快照 → 选靶结果 → 控制器 → 鼠标输出累加），循环尾等 1 ms；
- 快照来源：`FUN_140067000` 行 350-417 把推理线程写好的**整个状态块**（0xE48..0xFE0）拷进本线程的帧结构，其中
  `puVar40[0x4c8] = *(lVar17 + 0xe50)`（已消费水位）、`puVar40[0x4c0..] = 各项标志`；
- **因此：推理 5 ms 来一帧时，PID 用同一份（5 ms 前的）目标点连算 5 拍**；
- `PID-EventSync` 用 `+0xE50` 水位把"每份结果只消费一次"变成硬约束，QML 原文（`当前_PID_配置档位*.qml` 行 153-154）：
  > "EventSync 每次只消费最新的一帧推理结果并发送一次PID位移，不会在1000Hz输出循环中持续复用该结果。"
- **FrameSync（默认档）则是"持续复用"**——这正是它"跟手"的来源：等效于把 1 kHz 的步进累加在两次推理之间，而不是等下一帧。
- **`timeBeginPeriod` 在导入表里**（`unpacked/AimMagic_unpacked.exe` 文件偏移 0x76C094 处字符串 `timeBeginPeriod` 紧邻 `...ole32.dll`）：**【强推断】** 程序调用了 `timeBeginPeriod(1)`，把 Windows 定时器分辨率降到 1 ms —— 否则 `SleepConditionVariableSRW` 默认 15.6 ms 精度会让这个"1 kHz 循环"实际只有 ~64 Hz，整个设计就不成立。

### 3.7 取整余数累加（"没有取整死区"）【实证】

`FUN_140067000` 行 1523-1596，两处对称代码：
```c
if (abs(0xE??) ...) {                       // 非 Scale 控制器路径
    dVar50 = *(double *)(puVar40 + 0xb8);   // PID 输出（双精度，单位：鼠标计数）
    if (0.1f <= |dVar50|) {                 // 0.1 = min_position_offset 阈值（默认 0）
        lVar25 = *plVar33;
        dVar50 = dVar50 + *(double *)(lVar25 + 0xcd0);        // ★ 加上上一拍攒下的零头
        dVar53 = round-half-away(dVar50);                      // 见下
        *(int *)(puVar40 + 0x54) = (int)dVar53;                // 本拍真正发送的整数位移
        *(double *)(lVar25 + 0xcd0) = dVar50 - (double)(int)dVar53;   // ★ 零头留到下一拍
    } else {
        *(int *)(puVar40 + 0x54) = (int)round(dVar50);
        *(undefined8 *)(*plVar33 + 0xcd0) = 0;
    }
}
```
取整是 **"向零/远离零的对称四舍五入"**，实现为 `if (x<0) x-0.5 else x+0.5` 后截断（行 1530-1536）：
```c
if (dVar50 < 0.0) dVar53 = dVar50 - 0.5; else dVar53 = dVar50 + 0.5;
*(int *)(...) = (int)dVar53;
```
**【实证结论】** 浮点输出**只在出口取整一次**，取整误差 `residual ∈ (−0.5, 0.5)` 被写进 `runtime+0xCD0(x)/+0xCD8(y)` 攒到下一拍。
→ **稳态误差可以一路压到 0**，不会出现"每拍 0.4 个计数被丢掉 ⇒ 恒定 0.4 px 偏差且 Kp 再大也消不掉"的量化死区。这是我实现里最容易吃亏的一点，AimMagic 与项目里 `mouse/aim_pid.h` 描述的做法一致（"只在出口取整一次成整数鼠标计数，取整零头攒到下一拍"）。

### 3.8 输出通路与"分段鼠标"

行 1865-1924：整拍位移被送进一个**分段列表**（`FUN_140058230` / `FUN_140071a60`，受 `segmented_max_move`（默认 `DAT_1401f9a58=20`）与 `enable_mouse_segmented` 控制），然后逐段通过**虚调用** `(*pcVar3)(driver, dx, dy, name)` 发出（行 1903-1915，`driver = runtime+0x5c8`，同一套 KMBox/Makcu 抽象）。
行 1776-1796：每拍把 `(dx,dy)` 累加到 `runtime+0xAD8/+0xADC`（对外的总位移遥测）以及 **`runtime+0xC10/+0xC18`** —— 后者正是 §4.4 平台自运动速度的原始累加量。

### 3.9 「为什么它比我那种实现瞄得更准」——控制律层面

| 差异点 | 我的实现 | AimMagic（偏移） | 后果 |
|---|---|---|---|
| 微分 | `Kd·(e−e_prev)/dt` 裸差分 | 20 Hz 一阶低通 + 首拍归零（`FUN_140056f10` 0x57174-0x572BF） | 检测框抖动被 7.96 ms 时间常数抹掉，不产生"抖振" |
| 帧率 | 换帧率要重调 Kd | `dw = 1−e^{−40π·dt}` 恒定 20 Hz | 144/240 fps 手感一致 |
| 增益 | 固定 Kp | kp_floor 双用（启动爬升 + 大误差切小，§3.3/§3.5c） | 起步不冲、大误差不超调 |
| 积分 | 常开 + 输出前夹 | `+0xFC&&+0xFE` 门控 + Adaptive 复位 + warmup 帧不积分 | 松键/失靶不 windup |
| 限幅 | 输出夹 | 积分 ±i_clamp(6) + 单拍 ±max_output + 分段 max_step | 三级，且各管一段时间尺度 |
| 取整 | 先取整 | **余数累加**（`+0xCD0/+0xCD8`） | 消掉量化死区 → 稳态精度真的是 0 |
| 内环 | 帧率 | 1 kHz（`+0xE4A==0` + `timeBeginPeriod`） | 采样间持续收敛 |

---

## 4. 预测 / 补偿

### 4.1 预测状态机 `FUN_14006e470`（RVA 0x6E470，189 行）【实证】

签名：`FUN_14006e470(runtime /*param_1*/, boxes /*param_2: begin/end 向量*/, cfg /*param_3*/, crosshair_x /*param_4*/, crosshair_y /*param_5*/, pred_enable /*param_6*/)`
调用点：`FUN_1400824e0` 行 646：
```c
FUN_14006e470(param_1, puVar38 + 0x90, puVar38 + 0x640, fVar52);
```

**入口门控**（行 39）：`if (*(char *)(param_3 + 0x11b) != '\0')` → `cfg+0x11B = prediction_enabled`（QML/装载默认 **0**，`FUN_14001ba90` 行 840）。

**2 秒超时清理**（行 41-84）：每 1e9 ns 扫描一次预测表 `runtime+0xC30`（`FUN_140067000` 里用 `runtime+0xC28` 是另一张表），把 `now − entry[5] >= 0x7744D640 (=2s)` 的条目删掉。

**核心循环（行 102-180，作用于 0x28 字节的检测框，就地改写）**：
```c
iVar15 = max(0, *(int*)(cfg+0x124));                  // prediction_min_width  默认 0x14 = 20
iVar16 = max(iVar15+1, *(int*)(cfg+0x128));           // prediction_max_width  默认 0x50 = 80
for (pfVar14 = *param_2; pfVar14 != param_2[1]; pfVar14 += 10 /*0x28*/) {
    if (pfVar14[6] < 0) continue;                     // 无 trackId → 跳过
    track = FUN_140062bf0(runtime+0xc28, local_e8, pfVar14+6);   // 以 trackId 为键取/建预测状态
    track[0x28/8] = now;                              // 时间戳
    /* ---- ① 尺寸权重 sizeWeight（连续 0..0.75，随框宽线性）---- */
    fVar23 = 1.0f;                                    // DAT_1401f4634
    if ((float)iVar15 < pfVar14[2] /*w*/) {
        if (pfVar14[2] < (float)iVar16) fVar23 = (iVar16 - w) / (float)(iVar16 - iVar15);
        else                            fVar23 = 0.0f;   // ← 大框反而 0
    }
    /* ---- ② 平台位移门控 + sizeWeight 的加/减步进 ---- */
    if (param_6 && 0 < fVar23 && (pred_x>0 || pred_y>0)) {
        fVar24 = track[0x18/4];  fVar22 = track[0x1C/4];            // 两轴自适应权重（0..0.75）
        /* X 轴： */
        fVar21 = max(30.0f, w * 0.8f);                              // DAT_1401f8110=30, 1401f9a0c=0.8
        if (fVar21 * (fVar24*1.5f + 1.0f) <= fabsf(box.cx − crosshair_x)
            || fabsf(*(float*)(runtime+0xc04)) <= 10.0f) {          // DAT_1401f9aa0 = 10.0
            fVar24 -= 0.2f; if (fVar24 < 0) fVar24 = 0;             // DAT_1401f9a08 = 0.2
        } else {
            fVar24 += 0.02f; if (fVar24 > 0.75f) fVar24 = 0.75f;    // 0.02 / 上界 0.75（见 §8.2 第 6 条）
        }
        track[0x18/4] = fVar24;
        /* Y 轴同构，用 cy 与 runtime+0xc08 */
        pred_x = *(float*)(cfg+0x11c) * *(float*)(runtime+0xc04) * fVar23 * track[0x18/4];
        pred_y = *(float*)(cfg+0x120) * *(float*)(runtime+0xc08) * fVar23 * track[0x1C/4];
    } else { pred_x = pred_y = 0; }
    /* ---- ③ 反相阻尼 + 低通 ---- */
    fVar19 = 0.05f;                                   // DAT_1401f5b7c = 0.05（K_damp）
    if (pred_x * track[0x20/4] < 0.0f) fVar19 = 10.0f;   // DAT_1401f9aa0 = 10.0（方向翻转时用大系数）
    fVar23 = 0.05f;
    if (pred_y * track[0x24/4] < 0.0f) fVar23 = 10.0f;
    track[0x20/4] = (1.0f − fVar19) * track[0x20/4] + fVar19 * pred_x;
    track[0x24/4] = (1.0f − fVar23) * track[0x24/4] + fVar23 * pred_y;
    /* ---- ④ ★ 就地改写框中心 ---- */
    pfVar14[0] = track[0x20/4] + pfVar14[0];   // cx ← cx + predX_smoothed
    pfVar14[1] = track[0x24/4] + pfVar14[1];   // cy ← cy + predY_smoothed
}
```
> 注：常量 `0.05` 的取值来自 `DAT_1401f5b7c`（同时被 `x_kp_floor` 默认值复用，值为 0.05）；翻转时的 `10.0` 来自 `DAT_1401f9aa0`（float 10.0）。**方向翻转时把低通系数从 0.05 提到 10.0**：`(1−10)·prev + 10·new = new − 9·prev`，这是"**过冲量反向补偿**"——目标突然反向时，预测立刻从"旧方向的残留"里扣掉 9 倍，避免预测滞后半个身位。

### 4.2 `sizeWeight` 的真实语义（`prediction_min_width/max_width`）【实证 + 修正】

```c
if (w <= 20)      → weight = 1.0     （原判 fVar23 初值 1.0，小框不进入分支）
if (20 < w < 80)  → weight = (80 − w)/60      ← 连续线性衰减，w=20→1.0，w=80→0
if (w >= 80)      → weight = 0.0     （直接置 0）
```
**"sizeWeight 是尺寸自适应阈值"这个描述不准确**：它是 **"预测强度随目标框尺寸反比衰减"**：
- 小目标（<20 px）：满强度预测（小目标速度快、需要提前量）；
- 中等目标：线性衰减；
- 大目标（≥80 px）：**完全不预测**（贴脸敌人不需要提前量，提前量反而导致过冲）。

这是一个**按目标尺寸的增益调度**——正是我猜的"增益调度"思想，但调度对象是**预测强度**，不是 PID 增益。

### 4.3 预测强度的三个因子（`prediction_factor_x/y`）

```c
pred = prediction_factor × platform_displacement_velocity × sizeWeight × axisWeight
```
- `prediction_factor_x/y` = `AimKeyCfg + 0x11C/+0x120`，装载（`FUN_14001ba90` 行 840-843）：
  ```c
  local_18d = FUN_1400182c0(ppuVar13, "prediction_enabled", 0);      // 默认关
  uVar8     = FUN_1400177b0(ppuVar13, "prediction_factor", 0);       // 兼容旧键
  local_18c = FUN_1400177b0(ppuVar13, "prediction_factor_x", uVar8);// 默认 0
  local_188 = FUN_1400177b0(ppuVar13, "prediction_factor_y", uVar8);// 默认 0
  ```
  即 **三键同源，`prediction_factor` 是 x/y 的兜底默认**，默认全 0 → **出厂状态预测是关的**（开关 `prediction_enabled` 默认 0）。这几项是"玩家自己调"的量。
- `prediction_min_width = 20` / `prediction_max_width = 80`（行 844-845，**实证默认值**）。

**"预测是就地改写框中心"这一关键细节 —— 完全证实**（§4.1 ④，`FUN_14006e470.txt` 行 177-178）。意义：
- 预测**不进入反馈回路**（不是把预测当 reference 去减，而是把目标框本身挪走）；
- 下游**选靶的最近邻判据（§1.3）直接用挪过的位置** → 预测偏差被"最近邻选择"吸收：预测偏了顶多改成选旁边那个框，不会在 PID 里变成振荡分量；
- 这也解释了为什么 `prediction_factor` 可以设得比较大而不炸。

### 4.4 "平台位移" `runtime+0xC04/+0xC08`：自运动补偿 vs 目标速度前馈

`FUN_1400824e0` 行 598-644（**这是 QML 里"同时补偿软件自身发出的鼠标移动"的实现**）：
```c
FUN_140025f80(puVar38 + 0x70);                       // now (ns)
dVar48 = 1.0 / (double)param_1[0x1f];                // 1/fps
*(double *)(puVar38 + 0xf8) = dVar48;
FUN_140032560(puVar38 + 0x130, param_1 + 0x1b5);     // 取该互斥量保护的时间戳
lVar44 = *(longlong *)(puVar38 + 0x70);
if (param_1[0x184] != 0)                             // 有效目标数 != 0
    *(double *)(puVar38 + 0xf8) = (double)(lVar44 - param_1[0x184]) / 1e9;   // 实测 dt（秒）
*(undefined8 *)(puVar38 + 0x68) = DAT_1401f8008;     // 0.25（下限?）
*(double *)(puVar38 + 0xe8) = 0.001;
pdVar20 = (double *)FUN_140056b00(puVar38 + 0xf8, puVar38 + 0xe8, puVar38 + 0x68);  // clamp(dt, 0.001, 0.25)
dVar1 = *pdVar20;  *(double *)(puVar38 + 0xf8) = dVar1;
if (cVar9) {                                          // 有目标
    dVar47 = (double)param_1[0x182] / dVar1;          // Δy_self / dt   （0x182/0x183 = C10/C18 累加量）
    dVar49 = (double)param_1[0x183] / dVar1;
}
param_1[0x182] = 0; param_1[0x183] = 0;               // 清零累加
...
if (cVar9 == '\0') {                                  // 无目标 → 纯衰减
    fVar51 = *(float *)(param_1 + 0x181) * 0.1f;      // DAT_1401f461c = 0.1
    fVar53 = *(float *)((longlong)param_1 + 0xc04) * 0.1f;
} else {                                              // 有目标 → 混合
    fVar51 = (float)dVar49 * 0.05f + *(float *)(param_1 + 0x181) * 0.95f;   // DAT_1401f5b7c=0.05, 1401f9a10=0.95
    fVar53 = (float)dVar47 * 0.05f + *(float *)((longlong)param_1 + 0xc04) * 0.95f;
}
*(float *)((longlong)param_1 + 0xc04) = fVar53;       // ★ 写回平台位移速度（X）
*(float *)(param_1 + 0x181) = fVar51;                 // ★（Y，等价偏移）
```
**关键点 —— 两种补偿的区别：**
| | 目标速度估计 | 平台自运动估计 |
|---|---|---|
| 位置 | `FUN_140089ac0` 行 366-410（轨迹表 `track+0x20/0x24`） | `FUN_1400824e0` 行 598-644（`runtime+0xC04/0xC08`） |
| 输入 | **检测框中心在时间窗内的位移** | **我们自己发出的鼠标位移累加**（`runtime+0xC10/+0xC18`，由 §3.8 行 1782-1785 累加） |
| 窗口 | `tracking_velocity_sample_ms`（默认 20 ms，行 370-373） | 一次推理帧的 dt（clamp 到 [0.001, 0.25] s） |
| 滤波 | `新速度×0.25 + 旧速度×0.75`（`DAT_1401f4620=0.25`, `DAT_1401f4630=0.75`，行 389-392） | `新速度×0.05 + 旧速度×0.95`（无目标时纯 ×0.1 衰减） |
| 用途 | **轨迹外推**（未匹配轨迹往前推，`FUN_140089ac0` 行 469-471） | **喂给预测状态机做前馈**（`+0xC04/+0xC08`） |

**"平台位移"的物理含义**：因为软件自己发的鼠标会让画面整体平移，检测框中心的"位移"里混了**镜头自运动**。用 `Δself/dt` 把它减掉，剩下的才是**目标的真实运动**。这正好对应 QML 原文（`当前_PID_配置档位*.qml` 行 366-367）：
> "预测目标运动，同时补偿软件自身发出的鼠标移动。counts_per_pixel 需与当前游戏灵敏度和倍镜匹配。"

**与我的实现的关键差异**：我的前馈是"在线估计 counts-per-pixel"再乘速度。AimMagic 的预测**完全不需要 counts-per-pixel**（`kalman_counts_per_pixel_*` 默认 1.0 且在这条路径上没被使用，主报告 §2.5.1 已确认 Kalman 参数只是被搬运，不构成协方差滤波）——因为它在**像素域**做预测，然后交给 PID（PID 出口才是像素→计数）。
→ 这消掉了我实现里"标定不准 → 自运动补偿增益错 → 锚点处极限环"的整类风险（CLAUDE.md 里 `predictive_controller` 被删除的原因，正是同一类问题）。

### 4.5 「为什么它比我那种实现瞄得更准」——预测层面

| 差异 | 我的实现 | AimMagic |
|---|---|---|
| 预测施加位置 | 进 PID 回路的 setpoint/前馈 | **改框中心（选靶之前）** |
| 预测误差的后果 | 变成振荡分量 | 被最近邻吸收，最坏是换目标 |
| 标定量 | counts-per-pixel（需在线估计，不准就极限环） | 无（纯像素域比例 `prediction_factor`） |
| 尺寸自适应 | 无 | `sizeWeight = (80−w)/60`，大框不预测 |
| 反向处理 | 靠 Kd | 反相时低通系数 0.05→10.0 的过冲补偿 |
| 平台自运动 | 无（或写死增益） | `+0xC04/+0xC08` 由自身位移在线估计并 0.95 强平滑 |

---

## 5. 参数默认值总表（QML + 装载器双侧印证）

### 5.1 PID / 输出（QML `当前_PID_配置档位__驱动联动字段_enable_disable.qml`，scope=2，subObject = `pid_profiles/<档位>`）

| 键 | 默认 | UI 范围 | QML 行 | 结构偏移 |
|---|---|---|---|---|
| `x_kp` / `y_kp` | **0.1** | −100..100 | 168-174 / 235-241 | +0x00 / +0x18 |
| `x_ki` / `y_ki` | **0.0** | −100..100 | 175-181 / 242-248 | +0x08 / +0x20 |
| `x_kd` / `y_kd` | **0.0** | −100..100 | 182-188 / 249-255 | +0x10 / +0x28 |
| `x_kf` / `y_kf` | **0.0** | −100..100（仅 Adaptive 可见） | 189-195 / 256-262 | +0x30 / +0x38 |
| `x_i_clamp` / `y_i_clamp` | **6.0** | 0..9999 | 197-203 / 264-270 | +0x48 / +0x50 |
| `x_kp_floor` / `y_kp_floor` | **0.05** | −100..100（仅 Free/Adaptive 可见） | 205-211 / 272-278 | +0x58 / +0x60 |
| `i_gate_radius` | **0.0** | 0..9999（仅 Adaptive 可见） | 213-219 | （i_gate） |
| `i_box_lost_hold_ms` | **20** | 0..5000 | 220-226 | +0x0?（float） |
| `max_output_x` / `max_output_y` | **9999.0** | 0..9999 | 300-313 | +0x80 / +0x88 |
| `smooth_frames` | **0** | 0..60（仅 Free） | 315-321 | +0x68（warmup 槽） |
| `smooth_threshold` | **0.0** | 0..1000（仅 Free） | 330-336 | +0x70 / +0x78 |
| `min_position_offset` | **0.0** | 0..1000 | 337-342 | 出口阈值（§3.7） |
| `horizontal_deadzone` | **0.0** | −10..10 | 343-348 | — |
| `pid_profile` | **`"PID-FrameSync"`** | 5 档 | 18/23/116-143 | +0xC0（32B string） |
| `aim_controller` | **`"PID"`** | PID / Scale | 56-67 | +0xE0（32B string） |

### 5.2 Kalman 档（同页，行 379-439）

| 键 | 默认 | 范围 |
|---|---|---|
| `kalman_prediction_ms` | 12.0 | 0..200 |
| `kalman_prediction_strength` | 1.0 | 0..3 |
| `kalman_max_lead_px` | 40.0 | 0..1000 |
| `kalman_warmup_frames` | 3 | 0..120 |
| `kalman_mouse_effect_delay_ms` | 8 | 0..200 |
| `kalman_process_noise` | 20.0 | 0.001..100000 |
| `kalman_measurement_noise` | 8.0 | 0.001..100000 |
| `kalman_counts_per_pixel_x/y` | 1.0 | 0.001..10000 |

（主报告 §2.5.1 已证明：**这些参数只被 `FUN_1400824e0` 行 880-889 整块搬进 runtime 0xE70..0xEBC，PID 路径不使用它们做协方差增益**；`FUN_140056f10` 里 Kalman 与 FrameSync/EventSync 共用同一条 exp 滤波分支。）

### 5.3 Scale 档（同页，行 479-540）

| 键 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `flick_scale_x` / `flick_scale_y` | 1.0 | 0.1..50 | 实测配置里是 **3.225806 = 100/31**（原始 counts-per-pixel） |
| `flick_damping` | 0.25 | 0.01..2.0 | 实测 0.06 |
| `scale_delivery_ms` | 0 | 0..100 | |
| `scale_max_move_x` / `_y` | 9999.0 | 1..100000 | |

### 5.4 瞄准框 / 跟随 / 过渡 / 找色（装载器 `FUN_14001ba90`）

| 键 | 默认 | 行 | 说明 |
|---|---|---|---|
| `confidence_threshold` | 0.5（`DAT_1401f462c`） | 594 | |
| `nms_iou_threshold` | 0.5 | 595 | NMS 抑制阈值 |
| `aim_bot_scope` | **0** | 596 | 瞄准范围 |
| `aim_bot_inner_scope` | = `aim_bot_scope` | 597 | 内圈 |
| `aim_scope_mode` | **`"circle"`** | 602-604 | FOV 形状（圆/其它） |
| `bubble_width` / `bubble_height` | **1.0 / 1.0** | 607-608 | `DAT_1401f4634` |
| `aim_bot_fov_transition_ms` | **0** | 609 | 统一过渡时长（0 = 不过渡） |
| `aim_bot_fov_expand_transition_ms` | = 上一项 | 610 | 张开时长 |
| `aim_bot_fov_shrink_transition_ms` | = 上一项 | 611 | 收拢时长 |
| `min_position_offset` | 0 | 612 | |
| `horizontal_deadzone` | 0 | 613 | |
| `aim_bot_position` | **0.1** | 615 | 瞄点默认（= `aim_pos[0]`） |
| `aim_bot_position2` | = 上一项 | 616 | |
| `i_gate_radius` | **0** | 636-637 | 积分门控半径 |
| `i_box_lost_hold_ms` | **20** | 640-651 | 夹到 [0, 5000] |
| `x_i_clamp` / `y_i_clamp` | `DAT_1401f5b98` | 772-775 | |
| `max_output_x/y` | `DAT_1401f5bb0` | 777-780 | |
| `x_kp_floor/y_kp_floor` | `DAT_1401f5b7c` = **0.05** | 525-526, 781-784 | |
| `prediction_enabled` | **0** | 840 | |
| `prediction_factor` / `_x` / `_y` | **0 / 0 / 0** | 841-843 | |
| `prediction_min_width` | **20** | 844 | |
| `prediction_max_width` | **80** | 845 | |
| `allow_aim_find` | 1 | 838 | |
| `aim_follow_x` / `aim_follow_y` | **1 / 1**（`FUN_14007b0c0(...,1)`） | `FUN_1400824e0` 484-486 | 跟随轴 |
| `target_lost_hold_ms` | **20** | 967 | |
| `crosshair_size` | **8**（≥1） | `FUN_1400824e0` 1634 | 找色准星尺寸 |
| `hsv_x_scope` / `hsv_y_scope` | **20 / 20**（≥1） | 1650-1662 | |
| `hsv_y_offset` | 0 | 1664 | |

### 5.5 跟踪 / 过滤（`FUN_14001dc10`，Group 层）

| 键 | 默认 | 行 | 结构偏移 |
|---|---|---|---|
| `enable_tracking` | **0** | 322 | +0x42 |
| `min_hits` | **3** | 324 | +0x44 |
| `max_age` | **5** | 326 | +0x48 |
| `tracking_iou_threshold` | `DAT_1401f5b88` | 328 | +0x4C |
| `tracking_velocity_sample_ms` | **20** | 332-341 | +0x50 |
| `enable_third_person_self_filter` | 0 | 342 | +0x54 |
| `third_person_self_bottom_ratio` | `DAT_1401f5b90` | 348 | +0x58 |
| `third_person_self_side_ratio` | `DAT_1401f5b8c` | 361 | +0x5C |
| `third_person_self_min_area_ratio` | `DAT_1401f5b80` | 373 | +0x60 |
| `third_person_self_edge_margin_ratio` | `DAT_1401f5b78` | 385 | +0x64 |
| `third_person_self_hard_area_ratio` | `DAT_1401f5b84` | 397 | +0x68 |
| `third_person_self_confirm_frames` | **3** | 409 | +0x6C |
| `third_person_self_release_frames` | **5** | 421 | +0x70 |
| `small_target_threshold_x` | **20** | 435 | +0x78 |
| `small_target_threshold_y` | **25** | 437 | +0x7C |
| `small_target_aim_pos` | `DAT_1401f462c` = **0.5** | 439 | +0x80 |
| `small_target_horizontal_deadzone` | 0 | 441 | +0x84 |
| `fire_delay_time` | 0 | 451 | |
| `death_judge_frames` | 2 | 457 | +0x9C |
| `long_press_x/y_active` | 0 / 0 | 475-478 | +0xC4/+0xC5 |
| `long_press_x/y_multiplier` | 1.0 | 479-482 | +0xC8/+0xCC |

### 5.6 我读取的常量（从 `dump\0001401EF000_004E7000_p2_t1000000.bin` 按 RVA 直接读字节）

| 地址 | 值 | 用途 |
|---|---|---|
| 0x1401F8030 | **−125.663706143592** (double) | `−40π`，微分滤波器带宽 |
| 0x1401F8010 | 0.3 | 非同步档固定滤波权重 |
| 0x1401F8018 | 0.7 | inhibit 路径二次滤波系数 |
| 0x1401F8028 | 1e9 | ns→s |
| 0x1401F8020 | 1000 | ns→ms / ms→s |
| 0x1401F8000 | 0.001 | dt / 速度下限 |
| 0x1401F8008 | 0.25 | 平台速度 dt 上限（**注意：不是 PID 的 dt 上限**） |
| 0x1401F6358 | 1.0 | — |
| 0x1401F6360 | 0x7FFFFFFFFFFFFFFF | 取绝对值掩码 |
| 0x1401F462C | 0.5 | 半尺寸 / 默认置信度 |
| 0x1401F4634 | 1.0 | — |
| 0x1401F461C | 0.1 | FOV 变化死区 / `aim_bot_position` 默认 |
| 0x1401F4620 | 0.25 | 轨迹速度混合系数（新） |
| 0x1401F4630 | 0.75 | 轨迹速度混合系数（旧） |
| 0x1401F4638 | 0.5 | 鼠标计数取整偏置 |
| 0x1401F4654 | 20.0 | `kalman_process_noise` 默认 |
| 0x1401F4670 | 0x7FFFFFFF | 取绝对值掩码（int） |
| 0x1401F5B7C | 0.05 | `kp_floor` 默认 / 预测反相阻尼 |
| 0x1401F5B98 | 6.0 | `i_clamp` 默认（与 QML 6.0 吻合） |
| 0x1401F5BA0 | 12.0 | `kalman_prediction_ms` |
| 0x1401F5BA4 | 40.0 | `kalman_max_lead_px` |
| 0x1401F5BA8 | 200.0 | `kalman_prediction_ms` 上界 |
| 0x1401F5B9C | 8.0 | `kalman_measurement_noise` |
| 0x1401F5CE0 | 1e6 | ns→ms |
| 0x1401F80F8 | 10.0 | `segmented_max_step` 默认；**不是** dt 上限 |
| 0x1401F8110 | 30.0 | `sizeWeight` 门控的尺寸下限 |
| 0x1401F9A0C | 0.8 | `max(30, w·0.8)` |
| 0x1401F9A20 | 1.5 | 权重缩放 |
| 0x1401F9AA0 | 10.0 | 平台位移门控阈值；反相阻尼放大系数 |
| 0x1401F9AA4 | **3.402823e38 (FLT_MAX)** | 最近邻搜索初值 |
| 0x1401F9A00 | 0.02 | sizeWeight 增步 |
| 0x1401F9A08 | 0.2 | sizeWeight 减步 |
| 0x1401F9A10 | 0.95 | 平台速度旧值权重 |
| 0x1401F9A58 | 20.0 | `segmented_max_move` / 目标有效阈值 |
| 0x1401F9A60 | 30.0 | 默认参数（`axis_lock_hold_ms` 等） |
| 0x1401F9A68 | 40.0 | 默认参数 |
| 0x1401F9A78 | 100.0 | 默认参数 |

> ⚠️ 主报告 §9.2 把 `i_box_lost_hold_ms` 放在 `+0x74`、把 `kalman_prediction_ms` 放在 `+0x78`。我按**实际指令位移**核对后的结论是：PID 状态结构里 **`+0x70/+0x78` 是微分权重对（0.02 / 0.2）**，AimKeyCfg 里 `+0x74` 才是 `i_box_lost_hold_ms`（值 20，与 QML 默认 20 吻合，且 `FUN_1400824e0` 行 880 用 `param_2[0x1d]` = 字节 0x74 搬运）。两处不矛盾（不同结构体），但引用时请注意**区分 PID state 与 AimKeyCfg**。

---

## 6. 端到端延迟结构

### 6.1 流水线阶段与调用点（全部在 `FUN_1400824e0`，1787 行）

| # | 阶段 | 实现 | 调用点（行） | 备注 |
|---|---|---|---|---|
| 0 | 采集 | MF Source Reader 回调（**不在本函数内**） | — | 帧自带 T0 时间戳 |
| 1 | 预处理 | `FUN_14005f7c0` | — | 主报告 §4 映射 |
| 2 | 推理 | TensorRT `FUN_1401ca7a0` / DML | — | 异步；`param_1[0x20]` 记录耗时 |
| 3 | **NMS** | `FUN_140056340`（358 行） | 176（预览路径）/ **208（主路径）** | `IoU >= cfg+4 && IoU != thr` |
| 4 | 分类统计 | `FUN_14002a650`/`FUN_1400660c0` 循环 | 182-200 | 每类计数/最大宽高（给宏事件用） |
| 5 | **跟踪（IoU 关联 + 速度窗）** | `FUN_140089ac0`（550 行） | **219** | `min_hits/max_age/tracking_iou_threshold/tracking_velocity_sample_ms` |
| 6 | 跟踪后处理 | `FUN_140065910` / `FUN_140025f80` 计时 | 220-226 | `param_1[0x26]` 记录耗时 |
| 7 | 预处理后计时 | `param_1[0x27]` | 725-727 | |
| 8 | **FOV 过渡** | `FUN_14007d4c0` | 529 | 立方 ease-out |
| 9 | 找色 | `FUN_140080430`（978 行） | — | 预览/准星颜色 |
| 10 | **平台自运动速度** | 内联块 | 598-644 | 写 `runtime+0xC04/0xC08` |
| 11 | **预测（改写框中心）** | `FUN_14006e470` | **646** | 就地改 cx/cy |
| 12 | 每类事件/宏派发 | `FUN_14007a780` / `FUN_14007ab40` | 649 / 651 | 派发到工作线程，**不阻塞主管线** |
| 13 | **选靶 + 瞄点** | `FUN_140088a30→ab0` | **675** | 结果写 `puVar38+0x140..` |
| 14 | 扳机判定 | `FUN_1400886f0` / `FUN_140088790` | 740 | |
| 15 | **误差 = 瞄点 − 准星** | 内联 | **907-911** | 写 `runtime+0xEC4/0x1D9` |
| 16 | 状态块整体拷贝 + 提交 | 行 350-417 | | 供 1 kHz 线程消费 |
| 17 | **PID 内环（1 kHz）** | `FUN_140067000` | — | 见 §6.2 |
| 18 | 输出整形 / 分段 | `mouse/aim_path.h` 对应物：`FUN_140058230`/`FUN_140071a60` | 1800/1857 | |
| 19 | 驱动虚调用 | `(*(driver+0x10))(dx,dy,name)` | 1915 | KMBox/Makcu |
| 20 | 遥测累加 | `runtime+0xAD8/+0xADC` | 1770-1773 | 发给 UI |

### 6.2 关键：控制器是"对**陈旧检测**跑的高频内环"

```
推理线程（~N fps / 可能数百 fps）
  ├─ NMS → 跟踪 → 预测（改框中心）→ 选靶 → 瞄点 → 误差
  └─ 把整个状态块写进 runtime+0xE48..0xFE0（含 +0xE50 推理序号水位）
             │  (condvar signal)
             ▼
瞄准线程（FUN_140067000）
  loop {
      if (默认档 FrameSync) 等 1 ms（deadline = now + 1_000_000 ns）
      else if (Scale / PID-EventSync) 等 +0xE50 != 推理序号（每份结果只消费一次）
      读快照（上一份推理结果）
      for (每个 AimKey) { 控制器 → 分段 → 驱动 }
  }
```
- **默认 `PID-FrameSync` 走"1 ms 定时"分支**：`FUN_140067000` 行 271-317。
- **`+0xE4A`（事件驱动）只在 `Scale` 或 `PID-EventSync` 时为 1**（行 318-331）。
- `+0xE50` 是"已消费推理序号"：`+0xE50 == 当前序号` ⇒ 无新结果 ⇒ 睡死等唤醒（行 326）。
- 因此：**推理 3 ms 一帧时，PID 用同一份目标连算 3 拍**（FrameSync），这就是"跟手"的量化来源；`EventSync` 则是"每帧只动一次总位移"，总位移与内环频率解耦（QML 原文，`当前_PID_配置档位*.qml` 行 153-155）。
- **`timeBeginPeriod` 出现在导入表**（`unpacked/AimMagic_unpacked.exe`，文件偏移 0x76C094）⇒ **【强推断】** 进程把系统定时器分辨率提到 1 ms；否则 `SleepConditionVariableSRW` 的默认 ~15.6 ms 粒度会让 1 ms 等待退化为 ~64 Hz。

### 6.3 哪些阶段被跳过 / 重叠

| 机制 | 位置 | 效果 |
|---|---|---|
| 预测"就地改框" | `FUN_14006e470` 行 177-178 | 预测**不是**独立阶段，融进选靶；无二次应用（主报告 §4.2 已证） |
| 偏差 < 0.1 时**不做 FOV 过渡** | `FUN_14007d4c0` 行 414 | 避免每帧重启过渡 |
| 失靶后**沿用上一拍瞄点** | `FUN_1400824e0` 行 703 | 跳过选靶的"无目标"分支，不产生零误差假象 |
| `+0xE50` 水位 | `FUN_140067000` 行 326 | EventSync/Scale 时**跳过**无新结果的迭代 |
| 宏/事件派发到工作线程 | 行 649-651 → `FUN_14007a780` → `CreateThread`（`FUN_1401d9e20`+`DAT_1401f08e0`） | 主瞄准管线**不被宏阻塞** |
| 预览路径复用同一帧 | 行 171-204 | 预览/找色走独立分支，不占用主选靶 |

### 6.4 遥测字符串（可直接用于验证）

`FUN_1400824e0` 行 1016-1127 拼出的日志（格式串 + 顺序）：
```
"claude_code" " fps=" <fps> " infer=" <ms> "ms post " <ms> "ms nms " <ms>
"frame " <n> " fps " <fps> " infer " <ms> "ms post " <ms> "ms nms " <ms> ...
```
其中 `post` 段（`param_1[0x24]`）= **跟踪+预测+选靶** 的总耗时。**这给出了精度/延迟的可观测口径**：如果 `post` 只有几十微秒，说明"稳"不是靠滤波，而是靠 §1 的离散决策。

### 6.5 「为什么它比我那种实现更跟手」——延迟层面

| 我的实现 | AimMagic |
|---|---|
| 一次推理 → 一次 PID → 一次输出（延迟 = 推理周期 + 控制） | 一次推理 → **N 拍 1 kHz PID** 持续累加 → 输出连续 |
| 目标在两次推理之间移动的部分**完全丢失** | 在 **~每 1 ms** 上累加，直到新结果到来 |
| 靠前馈补偿 | 靠 **内环 + 预测改框** 双管齐下，且预测误差不进反馈 |
| 帧率变化 → 增益要重调 | `1−e^{−40π·dt}` + 1 ms 定时内环，帧率解耦 |

---

## 7. 「为什么它比我那种实现瞄得更准/更稳」——汇总排序

### 7.1 逐环节对比

| 环节 | 我的实现 | AimMagic 1.0.30 | 我该改什么 |
|---|---|---|---|
| **选靶** | argmax(score) | trackId 粘滞 → 最近邻 → 沿用上一拍（§1.3） | 引入 trackId 持久化 + 迟滞 |
| **关联** | 无 | IoU + 类别硬门 + `min_hits=3`/`max_age=5`（§1.1/1.2） | 加跟踪器（SimpleIOU 级别即可） |
| **瞄点** | 框中心 | 框内可调比例 + 每类覆盖（§2.1/2.2） | 加 `aim_pos[2]` |
| **误差** | 中心 − 准星 | **瞄点 − 准星**（§2.3） | 改成瞄点 |
| **PID 微分** | 裸差分 | 20 Hz 低通 + 首拍归零（§3.4） | 加同款时间常数滤波 |
| **PID 增益** | 固定 | 启动爬升 + 大误差切换（§3.3/3.5c） | 加 kp_floor 双用 |
| **积分** | 常开 | 三重门控 + warmup（§3.2） | 加 `aimActive && armed` 门控 |
| **取整** | 先取整 | 余数累加（§3.7） | **立刻改**（成本最低、收益最直接） |
| **预测** | 在线 counts/px 前馈进回路 | 改框中心 + sizeWeight + 反相阻尼（§4.1/4.2） | 前馈移出回路；预测改框 |
| **自运动** | 写死增益 | 在线估计 + 0.95 强平滑（§4.4） | 去掉写死常数 |
| **内环** | 帧率 | 1 kHz + `timeBeginPeriod`（§3.6/6.2） | 把 PID 放进独立 1 kHz 线程，消费最近快照 |

### 7.2 最可能解释"瞄准质量差距"的机制排序（我的判断）

1. **取整余数累加（§3.7）** —— 成本几乎为 0，但它决定了"稳态误差能否真的到 0"。我的"先取整再算"必然留下 <1 计数的永久偏差，而且 Kp 越大越抖、越小越不动。**这一条最容易被忽略、也最直接决定"最后几像素准不准"**。
2. **1 kHz 内环 + 最近检测复用（§3.6/6.2）** —— 直接决定"跟手"和"两次推理之间的收敛量"。没有它，预测再好也只是"猜"，不是"闭环补"。
3. **预测的施加位置：改框中心而不是进回路（§4.1）** —— 把预测误差隔离在反馈环之外，是"能放心把预测调大"的前提；我在回路里做前馈，标定一错就是极限环（这正是项目里 `predictive_controller` 被删的原因）。
4. **trackId 粘滞 + min_hits/max_age（§1.2/1.3）** —— 直接决定"换靶不抖、丢靶不甩"。这是"稳"的最大来源。
5. **瞄点可标定（§2.1/2.3）** —— 决定"系统性偏差"能否归零；瞄头和瞄中心是两套完全不同的命中分布。
6. **微分 20 Hz 帧率无关低通 + 首拍归零（§3.4）** —— 决定"抖不抖"以及跨帧率一致性。
7. **sizeWeight 按目标尺寸调度预测强度（§4.2）** —— 决定"贴脸不过冲、远距离有提前量"。
8. **kp_floor 双用（§3.3/3.5c）** —— 决定"起步不冲、大误差不超调"。
9. **积分三重门控（§3.2）** —— 决定"松键/失靶不 windup"。
10. **FOV 立方 ease-out + 0.1 死区（§2.4）** —— 决定"开镜瞬间不跳靶"。

---

## 8. 未确认 / 不确定

### 8.1 工具链限制导致无法直接验证的

| 项 | 状态 | 影响 |
|---|---|---|
| `FUN_140088ab0`（选靶+瞄点核心，约 1800 字节）| **无反编译**，我按原始字节手工反汇编（`dump\000140001000_001EE000_p20_t1000000.bin` @RVA 0x88AB0） | 打分公式的**确切组合方式**（尺寸项与距离项的加法/乘法关系）未能 100% 定死；"最近邻为主 + 可选尺寸项"这一结论可靠（默认 `size_scoring_weight=0`） |
| `FUN_1400885f0`（圆/胶囊 FOV 判定）| 手工反汇编，已见 `'bubb'+'le'` 字符串比较与两轴半径比较 | 圆的**确切几何**（圆 vs 按框缩放的椭圆、是否用 `bubble_width/height`）未定死；已确认"默认 `aim_scope_mode="circle"` 时走非矩形分支" |
| `FUN_140089660`（瞄点构造）| 手工反汇编，已见 `mulss ×0.5`、`subss/addss`、`cfg+0x08/+0x0C`（= `aim_pos[0/1]` 落点 +0x124/+0x128） | **符号与顺序**未完全定死；"瞄点 = 框中心 + 尺寸 × 比例"的方向性可靠，"是否用 0.5−ratio 还是 ratio"存疑 |
| `FUN_140088790`（逐框扳机判定）| **无反编译文件**，但主报告 §4.0① 给出了逐字原文 | 本报告未独立复核；报告中引用其 `key+0x78/0x7C`（小目标阈值）与四系数覆盖时标注为"来自主报告" |
| `FUN_140089800`（visited 位图分配）| **无反编译文件** | 仅知其被 `FUN_140089ac0` 用于 IoU 一对一占用 |

### 8.2 语义存疑（代码可见但含义不确定）

1. **`FUN_1400824e0` 行 266-267 的"inhibit"位**：`if (*(int *)(P + 0x68) == 1)` 时走 `(1−0.7)·prev + 0.7·dyNorm` 并把 dTerm 归零。**0x68 是谁写的、什么条件下为 1，我未找到写入点**（全文扫描 0x68 偏移的写入没有唯一定位）。这可能是"平滑/防抖模式"或死代码。
2. **`FUN_140056f10` 里那段 0.7 低通 + `Kf` 的计算（0x572D1-0x57304）在同步档被 `xorps xmm12,xmm12` 归零**：数学上**白算**（浪费但无副作用）。我判断这是 1.0.8→1.0.30 重构时保留的遗留路径，但**没有编译期证据**。
3. **`i_gate_radius`（默认 0）的实际消费者未定位**：QML 只在 `PID-Adaptive` 档显示（行 213-219），装载点 `FUN_14001ba90` 行 636-637 存入 `local_238`；但 `FUN_140056f10`/`FUN_1400579f0` 里没有对应的"仅当误差 < gate 才积分"分支。**可能是 Adaptive 分支的额外状态，或未被消费。**
4. **`+0xFC/+0xFD/+0xFE` 的确切语义命名**：我由 `FUN_1400579f0` 的写入条件（`profile ∈ {Free,Adaptive} && fcount < cfg+0x68` ⇒ `+0xFC=0`）与 `+0xFE` 对返回式积分项的开关作用，推断为 `aimActive / integrateReset / integrateArmed`。**+0xFE 的写入点未定位**（应在 `FUN_140067000` 的瞄准键处理段或 `FUN_1400574f0` 里）。
5. **`i_box_lost_hold_ms` 的消费点**：装载在 AimKeyCfg `+0x74`（行 640-651，夹到 [0,5000]），被 `FUN_1400824e0` 行 880 搬进 runtime `+0xE70`，再由 `FUN_140067000` 搬到帧结构 `+0x4e8`。**真正用它去门控 `+0xFE` 的代码我没找到**（`FUN_140067000` 行 1092 把 `puVar40[0x4e8]` 传给 `FUN_1400574f0`，行 1094 的结果写 `runtime+0x8fe`，行 1095-1099 再据此复位控制器）。**【推断】** 这条链就是"框丢失宽限"，但未闭合。
6. **`sizeWeight` 的两个中间量**（`fVar23` 尺寸权重 与 `track+0x18/0x1C` 逐轴权重）的**乘法顺序与上界**：主轴权重的增/减步长（0.02 / 0.2）与"目标靠近准星则增长、远离则衰减"的方向可靠；但**增长上界**在两种反编译口径下不一致（Ghidra 伪码给 `DAT_1401f4634 = 1.0`，段内常量表给 `0x3f800000`，而 `0.75` 出现在同一条指令流里）。按"小框满强度"的语义，**上界应为 0.75**，但未定死。中间量（`fVar23` 与 `track+0x18`）的**先后加权顺序**我也看得不够确定。
7. **平台位移的 `0.05/0.95` 与 `0.1`**：无目标时纯 ×0.1 衰减、有目标时 0.05 新 / 0.95 旧。这给出 **τ ≈ 20 帧** 的极强平滑。数值确凿，但"为什么这么慢"（可能是为了彻底滤掉自运动的反向抖动）属推断。
8. **`timeBeginPeriod` 是否真的被调用**：只证明它**在导入表里**（字符串 + IAT 槽存在），未定位 `call` 与实参。**【强推断】** 参数为 1（否则该设计不成立）。

### 8.3 未覆盖（不在本报告范围）

- 找色 / `Best Blob`（主报告 §5 已覆盖），与瞄准精度仅在"准星参考点"上相关。
- 扳机链路（`FUN_1400886f0`/`FUN_140088790`/`FUN_140088130`）的完整时序。
- 宏引擎与 `Scale` 校准算法（`FUN_140075c50` PID 自标定、`FUN_1400726d0`/`FUN_140074ec0` Scale 校准）。
- `FUN_14007c6b0` / `FUN_14006ea10` 等 Group/AimKey 级配置装载的完整字段表。

---

## 附录 A：本报告用到的文件与定位

| 路径 | 用途 |
|---|---|
| `v1030\funcs\FUN_140056340.txt` | NMS（358 行） |
| `v1030\funcs\FUN_140089ac0.txt` | 跟踪器（550 行，行 189-271 关联 / 366-410 速度窗 / 441-542 max_age） |
| `v1030\funcs\FUN_14006e470.txt` | 预测状态机（189 行，行 39 门控 / 41-84 清理 / 102-180 主循环） |
| `v1030\funcs\FUN_140067000.txt` | 瞄准+档案编排（2079 行，行 269-331 等待 / 350-417 快照搬运 / 1068-1281 控制器调用 / 1500-1618 取整余数 / 1755-1796 输出累加） |
| `v1030\funcs\FUN_1400824e0.txt` | 主管线（1787 行，行 219 跟踪 / 466-518 FOV / 598-644 平台速度 / 646 预测 / 675 选靶 / 680-720 粘滞 / 907-911 误差 / 880-889 Kalman 参数搬运） |
| `v1030\funcs\FUN_140057660.txt` | dt 计算入口（22 行） |
| `v1030\funcs\FUN_140057d20.txt` | dt 显式入口（21 行） |
| `v1030\funcs\FUN_140056b00.txt` | clamp（21 行） |
| `v1030\funcs\FUN_140076720.txt` | FOV/瞄准框命中判定（53 行） |
| `v1030\funcs\FUN_14001ba90.txt` | AimKey 参数装载（1191 行，行 594-853 参数段 / 1153 提交） |
| `v1030\funcs\FUN_14001dc10.txt` | Group 参数装载（655 行，行 318-450 跟踪/过滤/小目标） |
| `v1030\funcs\FUN_140063320.txt` | trackId 线性查找（19 行） |
| `v1030\funcs\FUN_140063920.txt` | 框向量深拷贝（31 行） |
| `v1030\funcs\FUN_14007d4c0.txt` | FOV 过渡状态机（490 行，行 371-468 核心） |
| `v1030\ghidra_out\pid_core.txt` | `FUN_140056f10` 反汇编（行 24-290）+ C 伪码（行 304-531）；`FUN_1400579f0` 反汇编（行 545-745）+ C 伪码（行 757-917） |
| `v1030\qml_z\当前_PID_配置档位__驱动联动字段_enable_disable.qml` | PID/输出/Kalman/Scale 参数表（554 行） |
| `v1030\qml_z\瞄准参数-按键配置-目标类别子页_EvolveUI_风格__.qml` | `aim_pos` 与每类覆盖（385 行） |
| `v1030\dump\000140001000_001EE000_p20_t1000000.bin` | 代码段（RVA 0x1000 起，offset = RVA − 0x1000），用于手工反汇编缺失函数 |
| `v1030\dump\0001401EF000_004E7000_p2_t1000000.bin` | `.rdata` 常量区（offset = RVA − 0x1EF000） |
| `v1030\unpacked\AimMagic_unpacked.exe` | 导入表字符串（`timeBeginPeriod` @ 文件偏移 0x76C094） |

## 附录 B：手工反汇编缺失函数的方法（可复现）

1. `funcs/` 共 1416 个文件，**缺** `FUN_140088790`、`FUN_1400885f0`、`FUN_140088ab0`、`FUN_140089660`、`FUN_140089800`（Ghidra 批处理未覆盖）。
2. 代码段 dump 文件 `000140001000_001EE000_p20_t1000000.bin` 满足 **文件偏移 = RVA − 0x1000**（区域基址 0x140001000）。
3. 按调用点已知的 `call rel32` 目标算出 RVA，直接读取字节并人工解码。
4. 交叉验证：`call` 目标 RVA 与「前一条指令是 `E8 xx xx xx xx`，`target = next_ip + rel32`」必须一致（本报告所有手工定位都已按此法核对）。
5. 常量值从 `0001401EF000_004E7000_p2_t1000000.bin` 按 `偏移 = RVA − 0x1EF000` 读 8 字节，同时按 float/double 两种解释取用（附录 §5.6 表）。
