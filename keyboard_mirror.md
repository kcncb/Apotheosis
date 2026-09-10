# 键盘复合镜像与注入固件

`KBD_MIRROR_V3R2` 是与鼠标版完全分开的键盘构建：右板连接实体键盘或键鼠接收器，左板连接电脑。它保留设备、配置、字符串、全部 HID 接口、Report Descriptor、Report ID、媒体键、系统控制和供应商定义报告；主动键盘命令只修改选中的 Keyboard/Keypad 输入报告，其他接口继续逐字节透传。

## 烧录

- `Left_MAKCM_MCU_MCU_KBD_MIRROR_V3R2.bin`：左板，地址 `0x0`。
- `Right_MAKCM_MCU_MCU_KBD_MIRROR_V3R2.bin`：右板，地址 `0x0`。
- 左右板必须成对烧录，不能与鼠标固件混用。

换键盘后，右板会重新读取完整描述符；身份变化时左板保存到独立的 `kbdmirror` NVS 槽并自动重启一次。键盘身份槽和鼠标身份槽互不覆盖。

## 注入命令

键盘版在 CH343 二进制协议中开放三个命令：

| CMD | Payload | 行为 |
|---|---|---|
| `0x21 KEY_MASK` | `modifiers + keys[6]` | 设置绝对注入状态；可用于按下和释放 |
| `0x22 KEY_TAP` | `modifiers + key + down_ms` | 固件内按下并定时释放，然后恢复基础状态 |
| `0x23 KEY_RELEASE_ALL` | 空 | 释放全部注入键，不释放或屏蔽实体键盘 |

修饰键 bit0~bit7 对应 HID Usage `E0~E7`；普通键使用 Keyboard/Keypad Usage ID，例如 `A=0x04`、`Enter=0x28`。命令成功返回 ACK，报告不可识别、参数非法或六个注入槽已满时返回 NAK。

固件根据镜像 Report Descriptor 自动寻找键盘接口和 Report ID，兼容标准六键数组及常见 NKRO 位图。实体键和注入键采用并集合并；释放注入状态会回到实体键盘的当前状态，不会把用户仍按住的键抬起。主机切换到 HID Boot Protocol 时自动使用标准 8 字节键盘报告。

`PANIC(0x44)` 只清除注入状态。`GET_HID_CAPS(0x4B)` 返回选中键盘报告的轮询间隔、Report ID 和字节数；X/Y 能力字段为 0。现有 `KBD_MIRROR_V3R2` 的 `GET_TIMING(0x4C)` 为 44 字节；V3R5 协议头文件仍兼容解码。

## 兼容边界

- 完整镜像支持最多 6 个 HID 接口、每个中断报告最多 64 字节、每个 Report Descriptor 最多 1024 字节。
- 注入只针对 HID Usage Page `0x07` 的键盘输入。媒体键、系统控制和厂商自定义 Usage 仍透传，但不由 `KEY_MASK/KEY_TAP` 生成。
- 普通 6KRO 报告在实体键已经占满六个数组槽时没有空间再加入一个新键；固件优先保留实体键，不覆盖真实按键。NKRO 位图不受六键物理数组容量限制，但单条命令仍最多维护六个注入 Usage。
- 右板当前直接枚举一个 USB 设备，不通过外置 Hub 枚举多个下游设备；键鼠二合一接收器自身暴露的复合接口可以完整镜像。

帧格式、ACK/NAK、4 Mbps 会话顺序和 C++ 帮助函数见 [proto.md](proto.md)，完整复合设备边界见 [composite_hid_mirror.md](composite_hid_mirror.md)。
