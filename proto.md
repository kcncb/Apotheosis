# MAKCU 鼠标/键盘私有协议 v1.7

> 适用固件：`PID_MIRROR_V3R5`、`KBD_MIRROR_V3R2`（左板）  
> 对接链路：CH343 USB-UART；115200 启动，PID 会话使用 4000000 bps  
> 兼容性：帧格式和既有命令编号与 v1.3 兼容；v1.6 恢复 latest-value PID，v1.7 增加 HID 发送状态与自恢复诊断

## 1. 二进制帧

```text
偏移  大小  字段       说明
0     2     MAGIC      A5 5C
2     1     LEN        Payload 长度，0~244
3     1     SEQ        发送方递增序号
4     1     CMD        命令码
5     N     PAYLOAD    参数
5+N   2     CRC16      CRC-16/MODBUS，低字节在前
```

CRC 范围是 `LEN + SEQ + CMD + PAYLOAD`，不包含 MAGIC 和 CRC 本身。CRC 错帧静默丢弃并计数。没有帧尾，接收方按 LEN 定长解析。

PID 移动成功时不回 ACK，避免应答阻塞热路径；参数错误仍返回 NAK。查询命令直接返回对应响应。旧 `km.*` ASCII 命令保留为调试兼容入口。

## 2. 命令

### 2.1 移动

| CMD | 名称 | Payload | V3R5 语义 |
|---|---|---|---|
| `0x01` | MOVE | `int16 dx, int16 dy` | 写入 latest-value 单槽；默认兼容参数为 8ms |
| `0x02` | MOVE_RAW | `int16 dx, int16 dy` | MOVE 的兼容别名 |
| `0x03` | MOVE_BATCH | `(int16 dx, int16 dy) × N` | 只取批中最后一个点写入 latest-value 单槽 |
| `0x04` | MOVETO | `int16 x, int16 y` | 已退役，NAK/UNSUPPORTED |
| `0x05` | PID_MOVE_LATEST | `int16 dx, int16 dy, uint16 ttl_ms` | 推荐入口；新修正覆盖尚未取出的旧修正 |

`ttl_ms` 范围仍为 1~1000，以保持线缆和上位机接口兼容。V3R5 只校验该字段，不再用它排队、延时或重放。X/Y 是相对位移，不是绝对坐标。

### 2.2 按键、滚轮和键盘注入

| CMD | 名称 | Payload | 说明 |
|---|---|---|---|
| `0x10` | BUTTON_MASK | `uint8 mask` | 绝对态；bit0=L、bit1=R、bit2=M、bit3=S1、bit4=S2 |
| `0x11` | BUTTON_MASK_EX | — | 已退役，NAK/UNSUPPORTED |
| `0x12` | CLICK | `uint8 bits, uint16 down_ms` | 固件内确定性按下/释放，成功回 ACK |
| `0x20` | WHEEL | `int8 delta` | 注入滚轮，使用可靠队列 |
| `0x21` | KEY_MASK | `uint8 modifiers, uint8 keys[6]` | 键盘版：绝对注入状态，成功回 ACK |
| `0x22` | KEY_TAP | `uint8 modifiers, uint8 key, uint16 down_ms` | 键盘版：定时按下/释放，成功回 ACK |
| `0x23` | KEY_RELEASE_ALL | 空 | 键盘版：释放全部注入键，真实键盘不受影响 |

键盘命令仅由 `KBD_MIRROR_V3R2` 接受；鼠标命令仅由鼠标版接受，不会在另一版本中静默执行。`modifiers` 的 bit0~bit7 对应 HID Keyboard Usage `E0~E7`，`keys[]` 使用 HID Keyboard/Keypad Usage ID，`0` 表示空槽。`0x01~0x03` 是 HID 错误码，不能作为注入键。键盘版按镜像报告自动适配普通 6KRO 数组和 NKRO 位图，并把注入状态与真实键盘状态合并。媒体键、系统控制、鼠标接口和厂商 HID 报告仍原始透传。

`KEY_MASK` 是绝对状态：发送新的六键数组即可完成按下和释放。`KEY_TAP` 是临时覆盖，计时结束后恢复到调用前的 `KEY_MASK` 基础状态。旧头文件名称 `KB_BLOCK` 保留为 `KEY_RELEASE_ALL` 的数值别名，但不会屏蔽真实键盘。

`0x30~0x36` 与 `0x43` 是保留的旧编号，统一返回 `NAK_UNSUPPORTED(3)`。

### 2.3 系统和查询

| CMD | 名称 | Payload/响应 | 说明 |
|---|---|---|---|
| `0x40` | GET_VERSION | 空 → VERSION_RESP | 分别返回 `PID_MIRROR_V3R5` / `KBD_MIRROR_V3R2` |
| `0x41` | GET_STATS | 空 → STATS_RESP | CRC、收发帧数和运行时间 |
| `0x42` | SET_BAUD | `uint32 baud` | 在旧速率回 ACK，排空发送后延时 200ms 切速 |
| `0x44` | PANIC | 空 | 清除本固件支持的全部注入状态；真实设备继续透传 |
| `0x45` | REBOOT | 空 | ACK 后重启 |
| `0x46/0x47` | DESC_APPLY/COMMIT | — | 保留，NAK/UNSUPPORTED |
| `0x48` | SUB_ASYNC | `uint8 enable` | 订阅按键状态事件 |
| `0x49` | GET_STATE | 空 → STATE_RESP | 19 字节兼容状态 |
| `0x4A` | GET_PIPELINE | 空 → PIPELINE_RESP | PID latest槽、单次提交失败数和输出报告数 |
| `0x4B` | GET_HID_CAPS | 空 → HID_CAPS_RESP | 主鼠标或主键盘报告能力；键盘版 X/Y 字段为 0 |
| `0x4C` | GET_TIMING | 空 → TIMING_RESP | PID、UART 和原始透传统计 |
| `0x4D` | START_PID_SESSION | 空 | 清零本次会话统计；在 115200 回 ACK 后自动切至 4000000 |

## 3. 响应

| CMD | 名称 | Payload |
|---|---|---|
| `0x80` | ACK | `uint8 cmd_echo, uint8 status` |
| `0x81` | NAK | `uint8 cmd_echo, uint8 error` |
| `0x82` | VERSION_RESP | `char firmware[16], uint8 reserved[2]` |
| `0x83` | STATS_RESP | `uint32 crc_errors, rx_frames, tx_frames, uptime_s` |
| `0x84` | ASYNC_BUTTON | `uint8 real_mask, uint8 injected_mask`，主动帧 SEQ=0 |
| `0x85` | STATE_RESP | 19 字节兼容布局 |
| `0x86` | PIPELINE_RESP | 12 字节 |
| `0x87` | HID_CAPS_RESP | 24 字节 |
| `0x88` | TIMING_RESP | V3R5 鼠标为 60 字节；兼容旧 28/36/44 字节解码 |

NAK 错误码：`1=BAD_LENGTH`、`2=BAD_VALUE`、`3=UNSUPPORTED`、`4=UNKNOWN_CMD`、`5=BUSY`。

### 3.1 PIPELINE_RESP

```text
uint16 queued;       // 0=latest槽为空，1=存在待发送修正
uint16 capacity;     // V3R5 固定为 1
uint32 rejected;     // 兼容字段，当前返回单次USB提交失败数
uint32 emitted;      // 已被 USB 端点接受的 PID HID 报告数
```

### 3.2 HID_CAPS_RESP

```text
uint8 version, poll_interval_ms, report_id, report_bytes;
uint8 x_bits, y_bits, flags, reserved; // bit0=mounted, bit1=boot, bit2=suspended, bit3=primary_ready
int32 x_min, x_max, y_min, y_max;
```

### 3.3 TIMING_RESP

```text
offset  size  field
0       4     uint32 last_hid_send_us
4       4     uint32 pid_received
8       4     uint32 pid_emitted
12      4     uint32 pid_overwritten
16      4     uint32 pid_expired
20      4     uint32 raw_dropped
24      4     uint32 serial_crc_errors
28      2     uint16 raw_queue_depth
30      2     uint16 raw_queue_high_water
32      4     uint32 raw_oldest_age_us
36      4     uint32 raw_received
40      4     uint32 raw_emitted
44      1     uint8 usb_flags       // bit0=suspended, bit1=primary_ready, bit2=send_task_valid
45      1     uint8 send_task_state // 0=idle, 1=raw, 2=injected, 3=not_ready, 4=recovering
46      1     uint8 send_instance   // 0~5；0xFF表示无具体接口
47      1     uint8 reserved
48      4     uint32 hid_recoveries
52      4     uint32 primary_fail_streak
56      4     uint32 primary_no_progress_us
```

- `pid_received` 是收到的 PID 命令数。
- `pid_emitted` 是被 USB 端点接受的 PID 报告数。
- `pid_overwritten` 表示尚未被发送任务取出的旧修正被更新修正覆盖。这是 latest-value 为保证控制新鲜度而有意采用的行为。
- `pid_expired` 是保留的兼容字段名；V3R5 表示修正被取出后单次 USB 提交失败，不再表示 TTL 到期。
- `raw_dropped` 是真实复合 HID 原始报告队列溢出数。
- `raw_received/raw_emitted` 分别是收到的实体报告数和已被左板 USB 端点接受的实体报告数，便于键盘透传完整性压测。
- `raw_queue_depth/raw_queue_high_water` 是所有接口独立队列的合计值；每接口容量为 32。
- `raw_oldest_age_us` 是所有接口队首中最老报告的当前年龄，不是延迟分位数。
- `hid_recoveries` 是发送看门狗触发 USB 软重连的次数。
- `primary_fail_streak` 和 `primary_no_progress_us` 用于区分端点暂忙与永久 busy；`send_task_state=3` 表示最近一次提交时端点未 ready。

头文件兼容解析旧 28/36/44 字节和当前 60 字节 TIMING_RESP。

## 4. V3R5 PID 调度语义

1. PID 命令到达后立即通知高优先级 HID 发送任务，不再等待固定 1ms 空闲轮询。
2. 尚未取出的相对 X/Y 修正只保存在一个 latest-value 槽中；新命令直接覆盖旧命令，不做加法，也不建立 FIFO。
3. 发送任务原子取走当前修正后只尝试提交一次。更新修正可立即进入空槽，不与已经取出的修正合并。
4. 每次发送按镜像 Report Descriptor 的 Logical Min/Max 限幅；超出单报告范围的余量不补发，避免旧方向残量污染后续闭环修正。
5. 成功提交到 TinyUSB 端点缓冲区后立即处理下一值，不同步等待完成回调。物理鼠标主报告存在时优先把当次注入合并进该报告。
6. `ttl_ms` 只为协议兼容保留和校验，不参与 V3R5 热路径调度。

这套语义优先控制新鲜度，而不是相对位移总量守恒。上位机发送频率高于 USB 可提交频率时，中间修正可能被更新值覆盖；这与原版 MAKCU 的 latest-value 行为一致，更适合闭环 PID。V3R5 将实体复合 HID 改为每接口独立 32 项队列，避免厂商接口队首阻塞主鼠标。

V3R5 实体报告提交不再同步等待端点 ready。发送任务按接口轮询，每 1ms 重试；超过 50ms 的旧相对报告会被淘汰。主 HID 连续 250ms 无进展且不处于 USB suspend 时，看门狗清理陈旧报告并执行一次 USB 软重连，重连冷却时间为 5 秒。USB mount、unmount、suspend、resume 事件都会更新状态并唤醒发送任务；suspend 期间收到输入会请求远程唤醒。

## 5. CH343 会话顺序

```text
1. 以 115200 打开端口
2. 发送 START_PID_SESSION
3. 在旧速率收到 ACK
4. 立即把本机端口切到 4000000
5. 等待至少 250ms
6. 查询 GET_VERSION / GET_HID_CAPS
7. 开始发送 PID_MOVE_LATEST
```

结束测试或退出程序前建议发送 PANIC，再用 SET_BAUD 把板子和本机端口恢复到 115200。

## 6. C++ 对接头文件

[`makcu_proto.h`](makcu_proto.h) 是 C++11 单头文件实现。推荐调用：

```cpp
makcu::Link link(serial);
link.start_pid_session(change_baud_callback, sleep_ms_callback, context);
link.request_hid_caps();

// PID热路径；成功不等待ACK，新值覆盖尚未取出的旧值
link.pid_move_latest(dx, dy, 8);
```

旧 `pid_move_accumulated()` 名称继续保留用于源码兼容，但 V3R5 会调用同一个 latest-value 实现，不再累加。

## 7. 双板镜像传输

左右板的 Serial1 使用相同的 `A5 5C + LEN + SEQ + CMD + PAYLOAD + CRC16` 外层帧：

| CMD | 方向 | Payload |
|---|---|---|
| `0x30 RAW_HID` | 右→左 | `uint8 instance + 原始中断 IN 报告` |
| `0x40 MIRROR_BEGIN` | 右→左 | `u16 version(3), u32 generation, u16 total, u32 crc32` |
| `0x41 MIRROR_CHUNK` | 右→左 | `u16 offset + 最多64字节数据` |
| `0x42 MIRROR_END` | 右→左 | `u32 generation, u16 total, u32 crc32` |
| `0x50 HID_GET_RESPONSE` | 右→左 | `uint8 token, uint8 status, 最多64字节报告` |

描述符在内存中序列化为 JSON，但通过 BEGIN/CHUNK/END 二进制分块传输。每帧有 CRC16，完整传输另有 CRC32、长度、代次和严格连续 offset 校验；失败最多重试三次。实时 `0x30` 原始报告通道与描述符控制通道分离。

电脑发起的 HID `GET_REPORT`、`SET_REPORT`、`SET_PROTOCOL` 和 `SET_IDLE` 由左板通过 `HID_GET/HID_OUT/HID_PROTO/HID_IDLE` 控制消息代理到右板；GET 响应使用 `0x50` 返回。详见 [复合 HID 镜像](composite_hid_mirror.md)。

## 8. 历史基准与 V3R5 验证

以下数据来自 V3R2 累加调度，仅用于链路能力参考，不代表 V3R5 latest-value 的覆盖/位移守恒结果：

- 1000Hz：10,000 条命令，`pid_overwritten=0`、CRC/过期/拒绝=0；正反向各 5,000 计数在 Windows Raw Input 精确守恒。
- 2000Hz：10,000 条命令合并为 4,992 个 USB 报告，Windows Raw Input 仍精确得到 `+5000/-5000`。
- CH343 写入到 Windows Raw Input：500 次零丢失，中位 1.038ms、P95 1.529ms、P99 1.573ms、最大 1.588ms。

V3R5 需要刷写后重新测量端到端延迟、接口队列深度、`hid_recoveries`、`pid_overwritten` 和 `pid_expired`。latest-value 模式不以 2000Hz 输入下的总位移守恒为目标。所有数据都只代表当前测试机，不是跨电脑、鼠标和 USB 控制器的固定保证。
