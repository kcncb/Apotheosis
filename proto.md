# MAKCUNEW 直通透传协议 v1.0

> 适用固件：`fw_device`（MAKCUNEW 设备侧）
> 对接方：Apotheosis `MakcuNewConnection`
> 链路：CH343 USB-UART，上电固定 115200 bps，运行期可切至 6000000 bps
> 对应头文件：[`makcu_proto.h`](makcu_proto.h)

本文档以固件源码为准（`fw_device/src/proto_parser.cpp`、`fw_device/src/handleCommands.cpp`），
并镜像到固件仓库的 `docs/proto.md`，两份内容逐字一致。
早期固件文档中的错误说法已全部更正，勘误见 §6。

---

## 1. 帧格式

```text
偏移  大小  字段      说明
0     2     MAGIC     固定 0xA5 0x5C
2     1     LEN       PAYLOAD 长度，取值 0 ~ 244
3     1     SEQ       发送方递增序号
4     1     CMD       命令码（见 §2）
5     N     PAYLOAD   参数区
5+N   2     CRC16     CRC-16/MODBUS，低字节在前
```

- 总帧长 = `LEN + 7`，无帧尾，接收端按 LEN 定长截取。
- CRC 范围为 `LEN + SEQ + CMD + PAYLOAD`，不含 MAGIC 与 CRC 本身。
- **`LEN > 244` 时固件丢弃头部并重新同步**（`proto_parser.cpp` 长度检查）。
- CRC 错帧静默丢弃并累加 `crc_errors`。

### 1.1 固件不会发送任何二进制响应帧

```c
// fw_device/include/proto_parser.h:42
bool (*sendFrame)(const uint8_t* data, size_t len, void* ctx) = nullptr;
```

`sendFrame` 在整份固件中**从未被赋值**，而所有回包都经由：

```c
auto send_resp = [&](uint8_t resp_cmd, const uint8_t* p, uint8_t plen) {
    if (!sendFrame) return;      // <-- 恒真，直接返回
    ...
};
auto ack = [&](uint8_t status) { uint8_t p[2] = { cmd, status }; send_resp(0x80, p, 2); };
```

**结论：设备永远不会发出 `0x80 ACK`、`0x81 NAK`、`0x82 VERSION_RESP`、`0x83 STATS_RESP`。**
上位机不得以任何回包作为命令成功与否或连接建立的前提。

设备唯一会主动写出的字节是：

| 来源 | 条件 | 内容 |
|---|---|---|
| `updateButtonState()` | 订阅了 `0x48 SUB_ASYNC` | `0x84` 二进制帧，`{uint8 real_mask, uint8 inj_mask}` |
| `updateButtonState()` | 收到过 ASCII `km.buttons(1)` | 单字节掩码（值 < 32），无帧头无 CRC |
| ASCII 命令应答 | 收到 `km.*` 文本命令 | `Serial0.println()` 纯文本行 |

### 1.2 DE AD 硬件级转义帧

固件还有一条**独立于 A5 5C 的第二套帧格式**，同样由字节状态机解析：

```text
DE AD | LEN_LO LEN_HI | CMD | PAYLOAD[LEN-1]
```

- `LEN` 为 **16 位小端**，且**包含 CMD 字节本身**（即 `LEN = 1 + len(payload)`）。
- **无 CRC、无帧尾、无 ACK**。
- `LEN == 0` 或 `LEN > 256` 时丢弃并重新同步。

目前只实现了一个命令：

| CMD | 名称 | Payload | 行为 |
|---|---|---|---|
| `0xA5` | SET_BAUD（硬件级） | `uint32 baud`（需 `LEN >= 5`） | 直接调用 `onSetBaud()` → `cbSetBaud()`，与 `0x42` 同一条实现 |

固件 `proto_parser.h` 注释明确写明这条路径是留给上位机的：

```c
// 2. DE AD 硬件级协议 (如 Apotheosis 动态高速切波特率 0xDE 0xAD 0x05 0x00 0xA5 [baud])
```

示例（6 Mbps = `0x005B8D80`）：

```text
DE AD 05 00 A5 80 8D 5B 00
│  │  └─┬─┘ │  └────┬─────┘
│  │    │   │       baud = 6000000 (小端)
│  │    │   └─ CMD = 0xA5
│  │    └───── LEN = 5 (1 字节 CMD + 4 字节 payload)
│  └────────── MAGIC 高字节
└───────────── MAGIC 低字节
```

> 上位机 `MakcuNewConnection` 优先使用带 CRC 的 `A5 5C 0x42`；
> 若切速后在新波特率复验失败，则自动回退重发本条 `DE AD` 帧再试一次。

---

## 2. 命令码

### 2.1 移动类

| CMD | 名称 | Payload | 固件行为 |
|---|---|---|---|
| `0x01` | MOVE | `int16 dx, int16 dy` | `len >= 4` 即直通输出；**热路径旁路**（见 §4.1） |
| `0x02` | MOVE_RAW | `int16 dx, int16 dy` | 与 `0x01` 完全等价的别名 |
| `0x03` | MOVE_BATCH | `(int16 dx, int16 dy) × N` | `len >= 4 && len % 4 == 0`；**累加求和后只调用一次 `onMove`**，不是队列 |
| `0x04` | MOVETO | `int16 x, int16 y` | 绝对虚拟坐标 |
| `0x05` | MOVE_CANCEL | 空 | **清空设备侧待发出的位移/滚轮积压**。上位机停火时发，保证"松手即停"而不是把攒下的位移继续吐完 |

### 2.2 按键 / 滚轮 / 键盘

| CMD | 名称 | Payload | 固件行为 |
|---|---|---|---|
| `0x10` | BUTTON_MASK | `uint8 mask` | 绝对态采纳为当前注入状态。bit0=L bit1=R bit2=M bit3=S1 bit4=S2 |
| `0x12` | CLICK | `uint8 btn_bits, uint16 down_ms` | 固件内定时完成按下/弹起，由 `clickTick` 任务（core 0，1 ms 周期）扫描 |
| `0x20` | WHEEL | `int8 delta` | 滚轮。USB 未就绪时**排入积压队列**由 `mouseMoveTask` 回放，不再丢弃 |
| `0x21` | KEY_MASK | `uint8 mod, uint8 keys[6]` | 需 `len >= 7` |
| `0x22` | KEY_TAP | `uint8 mod, uint8 key, uint16 ms` | 需 `len >= 4` |

### 2.3 系统类

| CMD | 名称 | Payload | 固件行为 |
|---|---|---|---|
| `0x40` | GET_VERSION | 空 | 构造 `VERSION_RESP`，但因 §1.1 **实际不发出** |
| `0x41` | GET_STATS | 空 | 构造 `STATS_RESP`，因 §1.1 **实际不发出** |
| `0x42` | SET_BAUD | `uint32 baud` | `Serial0.end()` → 延时 50 ms → `Serial0.begin(baud)`；需 `len >= 4`，范围 `115200 ~ 6000000` |
| `0x43` | GHOST_MODE | `uint8 on` | 预留，固件仅回 ACK（即无动作） |
| `0x44` | PANIC | 空 | 清除全部注入按键状态 |
| `0x45` | REBOOT | 空 | 重启 |
| `0x48` | SUB_ASYNC | `uint8 enable` | 置/清 `g_async_sub`，开启 `0x84` 按键上报。**使能为 true 时立刻补发一帧当前按键态**，避免上位机接入时漏掉"已经按着"的键。会话级，重启归零 |

**未实现**：`0x11 BUTTON_MASK_EX` 在固件中不存在，会落入 `default` 分支并累加 `unknown_cmds`。

---

## 3. 连接流程（ACK-free）

固件 `main.cpp` 中 `Serial0.begin(115200)` 是硬编码的，波特率**不持久化**，因此每次上电设备一定在 115200 bps。
加之 §1.1 无任何回包，`MakcuNewConnection` 采用如下流程：

```text
1. 以 115200 打开串口
2. 写 ASCII "km.version\r\n"
3. 等待设备回 "MAKCU-PASSTHROUGH-..."（唯一可靠的探活手段）
4. 若目标波特率 != 115200：
     a. 【方案 A】发送 A5 5C 0x42 SET_BAUD(uint32 target)，不等待任何回包
     b. 休眠 250 ms（固件内部 end + 50 ms + begin）
     c. 关闭并以目标波特率重新打开，重复步骤 2~3 复验
     d. 【方案 B】A 失败则回退到 115200，重发 DE AD 0xA5 <u32> 硬件级切速帧（见 §1.2）
     e. 休眠 250 ms 后再次以目标波特率复验
     f. 仍失败则退回 115200，保证链路可用
5. 发送 0x48 SUB_ASYNC(1) 订阅 0x84 按键上报，不等待回包
```

> **反例**：固件 `onSetBaud` 在 `ack(0)` 之前执行，即使 `sendFrame` 被接线，该 ACK 也会在新波特率上发出，
> 停留在旧波特率的上位机根本读不到。因此"等 ACK 再重连"在设计上就是错的。

ASCII 兼容入口（调试用，`Serial0.println` 会真正回包）：

| ASCII 命令 | 效果 |
|---|---|
| `km.version` | 回 `MAKCU-PASSTHROUGH-1.0.0` |
| `km.move(x,y)` | 等价 `0x01`，但**不走**热路径旁路，延迟更高 |
| `km.buttons(1/0)` | 开/关单字节掩码上报 |
| `km.left/right/middle/side1/side2/wheel` | 按键与滚轮 |
| `SERIAL_<baud>` | 等价 `0x42`，但固件内额外插入 1000 ms 延时 |
| `DEBUG_<n>` | 调试开关 |

---

## 4. 固件热路径

### 4.1 二进制 MOVE 的旁路

`handleCommands.cpp::protoOnMove` 在 USB 就绪且无待发位移时**内联直接调用 `handleMove()`**，
跳过 `s_pending_dx/dy` 累积与 `xTaskNotifyGive` 任务唤醒；否则才累积并唤醒 `mouseMoveTask`。

ASCII `km.move` 路径**没有**这个旁路，恒定走任务通知。因此**二进制 `0x01` 的延迟严格低于 ASCII `km.move`**。

### 4.2 单步内联

`handleMove` 在 `|dx| <= 127 && |dy| <= 127` 时单次调用 `Mouse.move(int8_t, int8_t)`，
不外拆、不加延时。

### 4.3 任务与核心绑定

| 任务 | 优先级 | 核心 |
|---|---|---|
| `mouseMoveTask` | 7 | 1 |
| `serial0Task` | 6 | 1 |
| `serial1Task` | 6 | 1 |
| `ClickTick` | 2 | 0 |

`Serial0`（PC 侧）与 `Serial1`（Host 侧）均有 ISR 唤醒对应任务，接收按 128 字节分块读取 FIFO。

`mouseMoveTask` 除回放位移积压外，也负责回放滚轮积压（见 §4.5）。

### 4.4 超频

`oc.cpp`：默认 `DEFAULT_OC = 260` MHz，`SAFE_MHZ = 240`。
开机自检后由 `ocConfirm` 任务在 30 s 内确认稳定；`.noinit` 段计数器记录崩溃，
连续 2 次异常则回落到 240 MHz。

### 4.5 可靠性与实时性行为（v1.0 起）

| 行为 | 实现 |
|---|---|
| 停火清积压 | `0x05 MOVE_CANCEL` 把设备侧 `s_pending_dx/dy` 与滚轮积压一次性归零。直通透传下上位机没有本地队列，但设备侧在 USB 未就绪时会攒位移，停火必须把那份也丢掉 |
| 滚轮不丢 | `0x20 WHEEL` 在 USB 未就绪时不再直接丢弃，改为排入积压由 `mouseMoveTask` 回放（与位移同等对待） |
| 订阅补发 | `0x48 SUB_ASYNC enable=1` 时立刻补发一帧 `0x84`，携带当前 `real/inj` 掩码。否则上位机接入时若用户已经按着键，会一直误判为未按下 |
| 断线重连 | 上位机 `MakcuNewConnection` 起一个守护线程，会话丢失后每 ~1 s 重新走一遍完整握手（含重订阅 `0x48`） |
| 会话校验 | 上位机把「串口已打开」与「会话已验证」分开：只有 `km.version` 探活成功才算在线，避免握手中的 500 ms 窗口被误报为已连接 |
| 触发轮询 | `keyboard_listener` 以 **1 ms** 周期读取物理按键。固件 `0x84` 是变化即推、微秒级的，轮询必须够快才不会成为整条感知链的瓶颈 |

---

## 5. 线格式示例

```text
MOVE dx=125 dy=-88
A5 5C 04 01 01 7D 00 A8 FF 45 3D
│  │  │  │  │  └──┬──┘ └──┬──┘ └──┬──┘
│  │  │  │  │     dx=125   dy=-88  CRC16
│  │  │  │  └─ CMD = 0x01
│  │  │  └──── SEQ
│  │  └─────── LEN = 4
│  └────────── MAGIC 高字节
└───────────── MAGIC 低字节
```

---

## 6. 历史勘误（早期固件文档的说法 → 源码实测）

| 固件文档描述 | 源码实际行为 |
|---|---|
| `LEN` 取值 0~248 | **0~244**，`> 244` 丢弃并重同步 |
| 存在 `0x11 BUTTON_MASK_EX` | **未实现**，落入 `default` |
| 系统类 `0x40~0x48` 回 ACK | **不回任何帧**（`sendFrame` 未接线） |
| `0x40` 返回 18 字节 `char ver[16], uint8 ghost, uint8 hum_on` | 构造了该帧但**发不出去** |
| `0x03 MOVE_BATCH` 为"批量增量队列" | 实为**累加求和后单次输出** |
| `0x82 VERSION_RESP` 返回 `PID_MIRROR_V3R5` 等版本串 | 本固件构造的是 `"PASSTHROUGH_V1"`，且发不出去 |

---

## 7. 实测链路开销

| 链路 | 每帧字节 | 波特率 | 单帧线路时间 |
|---|---|---|---|
| MAKCU ASCII `km.move(x,y)` | 18 | 115200 | 1562.5 µs |
| MAKCU ASCII `km.move(x,y)` | 18 | 921600 | 195.3 µs |
| MAKCUNEW 二进制 `0x01 MOVE` | 11 | 4000000 | 27.5 µs |
| MAKCUNEW 二进制 `0x01 MOVE` | 11 | **6000000** | **18.3 µs** |

二进制帧的线路开销约为 ASCII 路径在 115200 下的 **1/57**。
ASCII 路径在 1000 Hz 输入下已接近饱和（实测有效吞吐约 600 Hz，出现排队卡顿），
6 Mbps 二进制链路理论余量超过 54000 Hz（按 8N1 每字节 10 bit 计）。
