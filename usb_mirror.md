# USB HID 镜像传输 v3

> 适用固件：`PID_MIRROR_V3R5`  
> 左板：USB Device，连接电脑；右板：USB Host，连接真实 HID 设备

V3 镜像面向鼠标、键盘和键鼠复合接收器。右板采集完整 HID 身份和运行时报告，左板用相同 VID/PID、接口结构和 HID 报告重新枚举。

## 1. 启动流程

1. 左板发送 `READY`，右板返回 `USB_WAIT` 或 `USB_HELLO`。
2. 左板发送 `sendRawMirror`。
3. 右板构造包含设备、配置、每接口 Report Descriptor、字符串和主鼠标 layout 的 JSON。
4. JSON 使用二进制 BEGIN/CHUNK/END 分块发送；左板校验帧 CRC16、完整 CRC32、长度、代次和连续 offset。
5. 左板回复 `USB_MIRROR_ACK`；若身份 CRC 改变，写入 NVS 并重启一次。
6. 左板完成电脑端枚举后发送 `USB_INIT` 和 `USB_MIRROR_RAW`，右板开始实时报告透传。

传输失败会 NACK 并最多完整重试三次。描述符安装是原子的，失败不会覆盖 NVS 中最后一个有效身份。

## 2. 描述符分块

外层帧：`A5 5C | LEN | SEQ | CMD | PAYLOAD | CRC16/MODBUS`。

```text
0x40 BEGIN: u16 protocol=3, u32 generation, u16 total, u32 crc32
0x41 CHUNK: u16 offset, bytes[1..64]
0x42 END:   u32 generation, u16 total, u32 crc32
```

完整序列最大 28672 字节。CHUNK 必须从 offset 0 严格连续到 total；任何缺块、乱序、重复、CRC 或长度错误都会拒绝整次镜像。描述符传输仅发生在枚举阶段，不阻塞实时 `0x30` HID 报告通道。

## 3. 实时报告

复合模式下，`0x30 RAW_HID` payload 为：

```text
uint8 interface_instance;
uint8 wire_report[1..64]; // 包含 Report ID（若接口使用）
```

每个 HID 接口独立转发。主鼠标报告可按解析出的位域合并注入按钮、X/Y、滚轮和平移；其他键盘、媒体键、系统控制和厂商报告逐字节转发。V3R5 为每个接口分配独立 32 项队列并轮询发送，一个未被电脑轮询的厂商端点不会再阻塞主鼠标。

V3R5 的 PID 使用单槽 latest-value：新修正覆盖尚未取出的旧修正，被取出的修正只提交一次，不累加也不补发余量。这条策略只用于上位机 PID 注入；实体 HID 在各自接口内保序并在 USB 暂忙时重试。

实体报告发送采用非阻塞 ready 检查。超过 50ms 的陈旧报告会被丢弃，避免恢复后突然重放几分钟前的相对移动；主 HID 连续 250ms 无发送进展会触发 USB 软重连，5 秒内最多一次。挂起时输入会请求远程唤醒，USB 生命周期事件会唤醒发送任务。

## 4. 双向 HID 控制

- `SET_REPORT`：Output 优先使用实体中断 OUT；没有 OUT 时使用控制端点。Feature 使用控制端点。
- `GET_REPORT`：缓存命中的 Input 可直接返回；Feature 或未命中的请求代理到实体设备，再通过二进制 `0x50 HID_GET_RESPONSE` 返回。
- `SET_PROTOCOL`、`SET_IDLE`：按 HID 接口反向同步。
- Report ID 在控制传输和中断 OUT 两条路径中按 USB HID 规则添加或移除。

## 5. 身份镜像范围

镜像内容包括：

- Device Descriptor 的 VID/PID、bcdUSB、bcdDevice、类字段和字符串索引；
- 完整 Configuration Descriptor、接口顺序、HID/附加描述符和端点参数；
- 每个 HID 接口的 Report Descriptor；
- 语言、厂商、产品、序列号、配置及接口引用字符串；
- 1~6 个 HID 接口的原始 Input，以及 HID Output/Feature/Protocol/Idle 控制。

ESP32-S3 Device 控制器的 EP0 固定为 64 字节，因此实体设备的 `bMaxPacketSize0` 若为 8/16/32，会在镜像设备描述符中规范化为 64。当前 F2 实体值为 8、镜像值为 64；这是唯一已知的设备描述符字节差异，不影响 Windows 驱动。

USB 1.1 F2 没有 BOS。V3 不生成不存在的 BOS，也不代理 Microsoft OS Descriptor 或任意厂商自定义控制请求。

## 6. 兼容边界

- Full-Speed USB，`bcdUSB <= 2.00`，单配置且配置值为 1；
- EP0 为 8/16/32/64 字节；
- 1~6 个 HID 接口，Alternate Setting 必须为 0；
- 每接口最多两个中断端点，整个设备至少一个 IN；端点号 1~6，最大包 64 字节；
- 配置描述符最多 512 字节，每接口 Report Descriptor 最多 1024 字节；
- 最多 16 个引用字符串；镜像序列化总长最多 28672 字节。

这不是任意 USB 类转发器。包含音频、存储、CDC、摄像头等非 HID 接口的复合设备会被拒绝。Low-Speed、High-Speed、多配置、Alternate Setting 和大于 64 字节的 HID 报告不支持。

## 7. 当前 F2 验证

`249A:5C2F / XCTECH / Wireless-Receiver / 2025`：

- MI_00：鼠标；
- MI_01：键盘与 Consumer Control Collection；
- MI_02：驱动控制 HID；
- 三个接口、三个 IN 和一个 OUT 均正常枚举，F2 MouseHub 驱动可识别；
- 原始透传、Feature/Output 控制和 1ms 轮询已通过实机测试；V3R5 的接口隔离与自恢复需刷写后重新压测。
