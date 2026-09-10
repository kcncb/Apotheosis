# 鼠标 V3R5 / 键盘 V3R2 复合 HID 镜像

两套独立固件都用一对 ESP32-S3 完成全速复合 HID 镜像：右板作为 USB Host 连接真实设备，左板作为 USB Device 向电脑复现其身份。鼠标版只修改可安全解析的主鼠标报告；键盘版只修改自动识别出的 Keyboard/Keypad 报告。其余接口保持原始透传，两套固件及 NVS 身份槽互不混用。

## 镜像能力

- 最多 6 个 HID 接口和 16 个引用字符串；
- 原始 Device/Configuration/Report/String Descriptor；
- 每接口独立 Input 报告，支持 Report ID；
- 中断 OUT 与控制端点 Output/Feature；
- `GET_REPORT`、`SET_REPORT`、`SET_PROTOCOL`、`SET_IDLE` 双向代理；
- 身份 CRC 持久化，更换设备时自动保存并重新枚举；
- 描述符使用带帧 CRC16 和整包 CRC32 的二进制分块传输；
- 实时原始报告与描述符控制通道分离。

完整线缆格式见 [USB HID 镜像传输 v3](usb_mirror.md)，CH343 对接见 [鼠标/键盘私有协议 v1.7](proto.md)。

## V3R2 键盘注入

键盘右板从所有复合接口的 Report Descriptor 中选择包含 Keyboard/Keypad Usage 的输入报告，并把接口、Report ID 和报告长度随身份一起交给左板。左板再次解析具体修饰键、按键数组或 NKRO 位图字段，真实按键和注入按键始终在原报告格式中合并；Boot Protocol 则切换为标准 8 字节格式。媒体键、系统控制、鼠标和厂商接口不会被键盘命令改写。

## V3R5 低延迟与自恢复

V3R5 保留原版 MAKCU 式“新 PID 覆盖尚未取出的旧 PID”。数据到达立即唤醒高优先级 HID 任务；发送任务原子取走最新值并只提交一次，不累加、不补发限幅余量。实体复合报告按接口独立排队和轮询，不同步等待端点 ready 或完成回调。

因此：

- 只保留当前仍有控制价值的最新修正，不建立轨迹 FIFO；
- 输入频率高于 USB 可提交频率时允许中间修正被覆盖，以新鲜度换取更低的闭环拖尾；
- 单报告限幅后的旧方向余量不会污染后续修正；
- 每个实体 HID 接口有独立 32 项队列，单个厂商端点无法再队首阻塞主鼠标；
- 50ms 陈旧报告淘汰、USB 生命周期唤醒和 250ms 无进展看门狗负责自动恢复。

V3R2 曾在当前 F2 上测得 CH343→Windows Raw Input 中位 1.038ms、P99 1.573ms、最大 1.588ms；这是旧累加调度的历史链路数据。V3R5 的端到端延迟、各接口积压和看门狗恢复次数需刷写后重新压测，且不再以高频输入下的总位移守恒为目标。

## 当前 F2 身份

- VID/PID：`249A:5C2F`；
- bcdDevice：`0x0184`；
- Manufacturer：`XCTECH`；
- Product：`Wireless-Receiver`；
- Serial：`2025`；
- MI_00 鼠标、MI_01 键盘/Consumer Control、MI_02 驱动控制 HID。

Windows 把一个 HID 接口中的多个 Top-Level Collection 拆成多个 HID 节点属于正常行为，不代表 Report Descriptor 缺失。

## 限制

支持 Full-Speed、单配置、纯 HID 复合设备。ESP32-S3 的 EP0 固定为 64 字节，所以物理设备 EP0=8/16/32 时会规范化为 64；其他已支持描述符字段保持原样。

不支持任意非 HID USB 类、High-Speed/Low-Speed、多配置、Alternate Setting、端点号大于 6、单报告大于 64 字节、BOS/MS OS Descriptor 或通用厂商控制请求。未来更换设备时，是否能镜像以这些边界为准。
