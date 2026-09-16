# 原神AI 与 AimMagic 推理优化归因分析

> 采集时间：2026-09-13
> 证据来源：
> - 原神AI：运行日志（`GenshinImpact/logs/`，GBK）、`CF.ini` 全量配置、从加密 `.ysm` 内存还原出的明文 ONNX（`cfv26m640.onnx`，81,733,377 B，onnx 校验通过）
> - AimMagic：`docs/aimmagic-comparison.md`、`docs/AimMagic_1030_trigger_recoil.md`（来自 `AimMagic_RE` 语料，该目录现已删除）
> - 本方基线：`build/cuda/Release/logs/chain_20260912_122256_session_end.log`（281 帧 `L,8` 链记录）

---

## 0. 结论先行：原神的"快"不是调出来的，是**模型和工程边界**决定的

原神AI 的推理优势可以拆成三层，重要性递减：

| 层级 | 做了什么 | 贡献 |
|---|---|---|
| **① 模型层（最大头）** | 用 `end2end=True` 的 **YOLO26m**，NMS/DFL/解码全部烘进计算图，输出直接是 `[1,300,6]` 成品框 | **省掉整条 CPU 后处理链**（DFL 解码 + 阈值筛 + NMS + 坐标还原），这一条就值 2~5 ms/帧 |
| **② 数据通路层** | 上传 uint8（PCIe 流量 ÷4）→ GPU 侧 resize+归一化+CHW → 引擎；两端 pinned + 高优先级 stream + host spin | 把 H2D/D2H 和预处理的 CPU 占用压到接近 0 |
| **③ 调度/时限层** | `HIGH_PRIORITY_CLASS` + MMCSS + `timeBeginPeriod(1)` + 限帧对齐 120 FPS + 引擎缓存 | 保证"跑得稳"，消除抖动和长尾 |

**关键认知：原神的强不在"TRT 参数调得妙"，而在于它把整个 NMS-free 端到端模型当成一等公民来设计数据通路。** 它敢用 `[1,300,6]` 直出，所以后处理只剩一次 7.2 KB 的 pinned D2H memcpy；而这正是大多数自研链路（含本方）最重的一段 CPU 工作。

---

## 1. 模型层：`end2end=True` 是决定性的

### 1.1 还原出的模型事实（硬证据）

```
文件: cfv26m640.onnx   81,733,377 B (77.95 MB)
ir_version = 7        producer = pytorch 2.9.1
Ultralytics 8.4.7     task=detect     stride=32
imgsz = [640, 640]    batch = 1       channels = 3
input : images  float32 [1,3,640,640]
output: output0 float32 [1,300,6]
图节点 418 个, initializer 242 个, 参数量 20.40 M, 卷积算力 ≈ 67.9 GFLOPs
metadata: end2end = True
names = {0:'quansheng',1:'tou',2:'duiyou',3:'ditu',4:'renwupentu',5:'shiti'}
```

算子分布：`Conv 112 / Mul 100 / Sigmoid 98`（SiLU = Sigmoid×Mul）、`Concat 26 / Add 23`、`Reshape 12 / Split 12`，末端是：

```
399 Transpose → 400 Split → 401 ReduceMax → 402 TopK(300)
→ 405 Tile → 406 GatherElements → 408 TopK(300)
→ 409 Div → 411 Mod → 413 Gather → 414 Cast → 415 Tile → 416 GatherElements
→ 417 Concat → output0 [1,300,6]
```

**图里没有 `NonMaxSuppression` 算子，也没有 DFL 的 `Conv(reg_max=16)` 解码分支。** 作者用 `ReduceMax → TopK → GatherElements` 手写了一段"选最高分 + 按索引取框 + 拼 class_id"的无 NMS 选择逻辑，直接烘进图里。这就是 YOLO26 的 end2end 头（一对一头 + TopK 选择，无 NMS）。

### 1.2 这意味着什么

传统 YOLO 推理链：

```
engine 输出 [1,84,8400] → CPU 遍历 8400 个候选 → DFL 解码 4 个坐标
→ 置信度筛选 → NMS(8400×8400 IoU) → 输出几十个框
```

原神AI 的链：

```
engine 输出 [1,300,6] → pinned memcpy 7200 B → 直接就是成品
```

**后处理从"O(N²) 且分支密集"变成"零计算"。** 在 640 输入下，8400 候选的 CPU 解码+NMS 通常要 1.5~4 ms（单线程），而原神把它变成 0。这就是它"秒锁"体感差距里推理侧的全部来源。

### 1.3 附带收益：`[1,300,6]` 让缓冲区极小且定长

- 输出 pinned 只有 **0.01 MB**（日志原文），D2H 拷 7.2 KB，传输时间可忽略
- 定长输出 → 不需要"候选数上限"动态分配，不产生 per-frame 内存抖动（本方 `postProcess.cpp` 里那条 `kMaxCandidates` 上限逻辑在原神里根本不存在）
- `300` 是固定上限 → TRT 可以做满静态 shape 优化，无 dynamic shape 开销

---

## 2. 数据通路层：从采集到引擎的每一步都在省

### 2.1 日志里能直接读到的通路事实

| 日志行 | 含义 | 作用 |
|---|---|---|
| `TRT加载框架: YOLO26 (归一化: 0.003921569)` | 自动识别 YOLO26 架构，并**把 1/255 作为常量烘进预处理** | 预处理不需要读 config，无分支 |
| `分配 GPU uint8输入缓冲区: 1.17 MB` | 640×640×3 = 1,228,800 B 的 **uint8** 显存缓冲 | **上传原始字节，PCIe 流量是 float 的 1/4** |
| `分配 GPU resize临时缓冲区: 1.17 MB` | GPU 侧 resize 的 scratch | resize 在显存里做，不回主机 |
| `GPU预处理已启用` | resize + 归一化 + HWC→CHW 全在 GPU | CPU 零参与 |
| `输入缓冲区使用Pinned Memory (4.69 MB)` | 640×640×3×4 = 4,915,200 B = **float32 NCHW 成品张量** | 引擎输入直接指向 pinned，无需中转 |
| `输出缓冲区使用Pinned Memory (0.01 MB)` | 300×6×4 = 7,200 B | 输出零额外拷贝 |
| `stream_priority=high` | 高优先级 CUDA stream | 推理 kernel 抢占其它 GPU 工作（预览/编码） |
| `host_wait_spin=true` | 主机侧**自旋等待**而非阻塞同步 | 省掉 `cudaStreamSynchronize` 的内核态唤醒（≈10~40 µs/帧） |
| `Load 12 TensorRT DLLs from exe directory` | TRT/CUDA 运行库自带 | 不依赖系统 CUDA，版本可控，避免 DLL 地狱 |
| `检测到已有FP16精度Engine文件，直接加载` | FP16 引擎缓存 | 二次启动跳过 ONNX 解析 + 构建（省数十秒） |
| `检测到XM Engine格式，元数据: nc=3, status=Trusted` | 自研引擎容器带**旁挂元数据** | 加载时无需重新推断输入输出布局 |

### 2.2 关键设计：uint8 上传 + GPU 转换

数据流：

```
采集帧 (1920×1080 BGR, 主机)
  │  只拷需要的 ROI，uint8
  ▼
GPU uint8 缓冲 (1.17 MB)            ← PCIe 流量 1.17 MB
  │  GPU kernel: letterbox/resize + ×(1/255) + HWC→CHW + BGR→RGB
  ▼
引擎输入 (640×640 float32 CHW, 4.69 MB)  ← 显存内转换，不过 PCIe
  │  67.9 GFLOPs FP16
  ▼
output0 [1,300,6] (7.2 KB)
  │  pinned D2H
  ▼
成品检测框
```

**如果换成"CPU 转 float 再上传"，PCIe 流量会从 1.17 MB 涨到 4.69 MB（×4）**，在 PCIe 3.0 x16 上约 6.6 ms vs 1.6 ms。原神选了前者。本方 `cuda_preprocess.cu` 的设计（`resize_bgr_u8_to_chw_rgb_f16_kernel`，一次 launch 完成 resize+归一化+通道变换）思路一致，方向正确。

### 2.3 `host_wait_spin=true` 的价值

`cudaStreamSynchronize` 走内核事件等待，唤醒延迟约 10~40 µs，且在负载高时会被调度器推后。自旋等待把这部分压到接近 0，代价是一个核在转。在"推理 8.8 ms/帧"的量级下，这点节省看起来小，但它**消除的是尾部抖动**（p99 vs p50 的差），而体感恰恰由尾部决定。

---

## 3. 调度与时限层：让上面的优化不被打断

`CF.ini` + 日志共同暴露的配置：

```ini
inference_backend=1        # TensorRT
trt_use_fp16=1             # FP16 引擎
trt_mixed_precision=1      # 混合精度
process_frame=3            # 抽帧步长（不每帧都推）
max_range=640              # 有效检测范围
fps_limit=120              # 限帧，与采集 120 fps 对齐
enable_stats_optimization=1
enable_high_precision_timing=1
```

日志：

```
推理性能集: process_qos=true, process_timer=true, process_priority=0x8000,
            timer=true, thread_qos=true, thread_priority=true,
            mmcss=true, mmcss_priority=true
预估输出模式: 对齐用户限帧上限 120 FPS
```

- **`process_priority=0x8000`** = `HIGH_PRIORITY_CLASS`
- **`timer=true`** = `timeBeginPeriod(1)`，让 1 ms 级 Sleep/等待精度达标
- **`thread_qos=true` + `mmcss=true`** = 把推理线程注册进 MMCSS，拿"Pro Audio/Gaming"级别的调度优先级
- **`process_qos=true`** = 进程级 QoS（PowerThrottling 关闭之类）

这四条合起来的效果：推理线程几乎不会被系统调度让出。**在 Windows 上，CPU 抢占抖动动辄 1~5 ms，能把 8.8 ms 的推理拉成 12 ms。原神用这四条把这块按住了。**

对照 AimMagic：它的"快"**不来自这些**。文档 §A.4 已证：AimMagic 主循环 `max_fps` **默认 0（完全不限速）**，运行模式是"推理事件驱动"，三层线程（主 tick / 瞄准工作线程 / 压枪播放线程）里没有任何 MMCSS/QoS 注册痕迹。**AimMagic 靠的是结构而非优先级** —— 见 §5。

### 3.1 `process_frame=3` 与限帧：承认"不需要每帧都推"

`process_frame=3` 是抽帧：不是每个采集帧都送去推理。配合 `fps_limit=120`，推理实际节奏由这两者共同决定。**这是清醒的取舍**：把 GPU 算力花在"有意义的帧"上，而不是盲目追采集帧率。

---

## 4. 为什么原神体感像"瞬间"——把推理放回链路里算

本方实测（`chain_20260912_122256`，281 帧）：

| 指标 | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| `capture_wait_ms` | 1.82 | 1.81 | 1.97 | 2.20 | 2.23 |
| `inference_ms` | 8.84 | 8.80 | 8.96 | 9.25 | 9.36 |
| `total_ms`（软件链） | 10.78 | 10.76 | 10.97 | 11.22 | 11.24 |
| `end_to_end_ms` | 60.96 | 60.95 | 61.19 | 61.44 | 61.49 |

注意 **`total_ms` 的标准差极小**（10.76~11.24 = 抖动 < 0.5 ms）。本方软件链本身**已经非常干净**，`inference_ms` 只有 8.8 ms。

那 `end_to_end_ms = 61 ms` 的另外 50 ms 去哪了？按对比文档 §1.2 的分解：

```
端到端 = 采集卡前延迟 + 推理 + tick + HID/游戏/显示
当时按 capture_age_offset_ms = 50 估算 ⇒ 采集侧约 50 ms
```

> **【2026-09-13 更新】`capture_age_offset_ms` 已删除**（手填估计值，且控制器纯反馈、不吃延迟
> 估计）。这个"50 ms"从来就是**推算**而非实测 —— 但下面的结论不受影响：`total_ms` 的标准差
> 极小（抖动 < 0.5 ms）说明**软件链本身很干净**，而 `end_to_end_ms - total_ms` 的差额必然来自
> 采集卡内部与显示侧，那部分软件测不到。

**所以真正的差距不在软件推理这一段。** 软件链已经压到 11 ms，剩下那部分属于采集卡硬件/缓冲与显示延迟 —— 这是原神和 AimMagic 同样要面对的物理下限。

**结论修正：原神在"推理优化"上确实做到了极致（模型端到端 + 零后处理 + GPU 全通路 + 调度特权），但"秒锁"体感的另一半来自它的链路结构和首枪策略，那部分属于控制侧，不属于推理侧。**

---

## 5. AimMagic 的推理优化：另一种哲学

AimMagic 在推理侧几乎没做原神那种工程堆料，它的选择是**"让推理结果不被消费两次、不被中转、不被等待"**：

| 机制 | 事实（文档已证） | 效果 |
|---|---|---|
| **单一 tick 串行** | `FUN_14007e5c0` 行 291（推理）→ 390（按键）在**同一 tick 同一线程** | 判定→执行 0 拍延迟，不跨帧、无队列 |
| **事件驱动消费** | `runtime+0xE50` 是"已消费的推理序号水位"，`+0x4c8 != +0x100` 才干活 | 每个推理结果**只被消费一次**，不重复复用 |
| **无限速主循环** | `max_fps` 默认 0 | tick = 推理自然节奏，无 1000 Hz 空转 |
| **控制器不等下一帧** | `FUN_140067000` 两条路径：定时 1 ms（≈1000 Hz）或无限期等条件变量 | 位移不受推理节拍限制 |
| **一步发完** | `scale_delivery_ms` 默认 0 ⇒ 跳过分期，整段位移一拍到达 | 无"每帧最多 N counts"的隐性限幅 |
| **驱动直落** | 一次虚调用 + 一把 mutex，在扳机状态机同一线程栈上落驱动 | 没有"发送队列 → 工作线程 → 驱动"第二跳（本方若用队列则白送 1 个推理周期）|
| **惰性 TLS 引擎** | `mt19937` 放 TLS（`gs:[0x58]` + `0x1598`） | 避开全局 `rand()` 竞争 |

**两者对比：**

| 维度 | 原神AI | AimMagic |
|---|---|---|
| 优化重心 | **推理吞吐/延迟本身**（模型 + GPU 通路 + 调度） | **推理结果的消费路径**（0 中转、0 等待、0 重复） |
| 模型 | YOLO26m end2end，手写无 NMS 头 | 未从文档确认（语料已删） |
| 后处理 | 烘进图，CPU 零工作 | 同 tick 内完成 |
| 线程模型 | 推理 + 预览解耦，性能集特权 | 单 tick 串行，无限速 |
| 优先级手段 | HIGH_PRIORITY_CLASS + MMCSS + timer | 无（靠结构而非优先级） |
| "快"的来源 | **算得少、搬得少、不被打断** | **不中转、不等待、不重复消费** |

**AimMagic 教给本方的是结构（同 tick 串行、事件驱动水位、一步发完），原神教给本方的是数据通路（end2end 模型、uint8 上传、GPU 全通路、pinned+spin）。两者不冲突，可以同时要。**

---

## 6. 本方（Apotheosis）现状与差距

### 6.1 已经做对的（不必再动）

- ✅ 高优先级 CUDA stream（`cudaStreamCreateWithPriority`，`trt_detector.cpp:264`）
- ✅ CUDA Graph 捕获 + 双缓冲 slot，每 slot 独立 graph + 独立 pinned dst（`:1019-1023`）
- ✅ 融合预处理 kernel：`resize_bgr_u8_to_chw_rgb_f16_kernel` 一次 launch 完成 resize + ×(1/255) + BGR→RGB + HWC→CHW（`cuda_preprocess.cu:19-70`）
- ✅ GPU 侧 decode + filter kernel（`decode_and_filter_kernel`，`:122`）
- ✅ 全程 `cudaMemcpyAsync` + event 计时（preprocess/inference/copy 三段分离）
- ✅ 实测软件链 `total_ms` 抖动 < 0.5 ms，`inference_ms` 8.8 ms —— **已经达标**

### 6.2 真实差距（按收益排序）

| 优先级 | 差距 | 原神怎么做 | 本方现状 | 预期收益 |
|---|---|---|---|---|
| **P0** | **模型仍是 NMS-on-CPU 形态** | 用 `end2end=True` 导出，NMS/解码入图，输出 `[1,300,6]` | `postProcess.cpp` 仍跑 `postProcessYolo` + `nmsThreshold`，有 `kMaxCandidates` 上限与 CPU NMS | **1.5~4 ms/帧**（最大单项） |
| **P1** | 上传路径未确认是否 uint8 最优 | uint8 1.17 MB 上传，GPU 内转 float | `resize_bgr_u8_to_chw_rgb_f16_kernel` 已是 u8→f16，**方向已对**，需确认 ROI 只传必要区域 | 0~3 ms（取决于当前 ROI） |
| **P2** | 主机同步方式 | `host_wait_spin=true` 自旋 | 用 `cudaEventSynchronize`（阻塞等待） | p99 抖动的量级 |
| **P3** | 进程/线程特权 | `HIGH_PRIORITY_CLASS` + MMCSS + `timeBeginPeriod(1)` | 对比文档 §5 提到 `timeBeginPeriod(1)` 已在 `Apotheosis.cpp:225` 做；**MMCSS 与进程优先级未确认** | 抗 CPU 抢占抖动 1~5 ms |
| **P4** | 控制侧结构（非推理） | — | 控制节拍 = 检测节拍，无新检测就 skip（对比文档 §1.2） | 见 §1.4/§1.5 验收指标 |

### 6.3 一条可以直接抄的清单

1. **换 end2end 模型导出**（最高优先级）。Ultralytics 加 `end2end=True` 导出，确认输出是 `[1,N,6]`（`N=300`），坐标已是原图尺度。然后把 `postProcess` 从"NMS + 解码"降级为"遍历 N 行、按 conf 过滤"。
   - 注意 `trt_detector.cpp:715/882/944/961` 已有对 `[1,N,6]` 的识别分支，**说明本方已经预留了这条路径**，只差模型侧配合。
2. **确认上传的是 uint8 且只传 ROI**，不要先转 float 再上传。
3. **考虑把 `cudaEventSynchronize` 换成自旋等待**（或 busy-poll `cudaEventQuery` + `_mm_pause`），消 p99 尾部。
4. **给推理线程注册 MMCSS**（`AvSetMmThreadCharacteristicsW(L"Games")`）+ 进程 `HIGH_PRIORITY_CLASS`。
5. **照搬 AimMagic 的结构**（与推理正交，可并行）：同 tick 串行判定→执行、事件驱动水位消费、`scale_delivery_ms` 默认 0 一步发完。

---

## 7. 一句话总结

**原神的推理优化好，是因为它把"后处理"这件事从系统里删掉了（end2end 模型），并把剩下的数据搬运压到物理下限（uint8 上传 + GPU 全通路 + pinned + spin），最后用 Windows 调度特权保证这条通路不被打断。**
**AimMagic 的好，是因为它把"推理结果的消费"从系统里删掉了（同 tick 串行、事件驱动、一步发完）。**
**本方软件链已经干净（11 ms 抖动 < 0.5 ms），缺的是模型侧换 end2end，以及控制侧的消费结构 —— 不是 TRT 调参。**
