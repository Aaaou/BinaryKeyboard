# 低功耗蓝牙 (BLE)

BinaryKeyboard 无线版基于 CH592F 的 BLE5.4 协议栈，实现 HID over GATT，支持标准 HID 设备配对与使用。

## 概览

| 项目 | 说明 |
| :--- | :--- |
| 芯片 | CH592F (RISC-V, BLE5.4) |
| 角色 | 外设 (Peripheral) |
| 连接数 | 单连接 |
| 设备名称 | `BinaryKeyboard5KEY` / `BinaryKeyboardKNOB` |

## GATT 服务列表

| 服务 UUID | 名称 | 说明 |
| :-------- | :--- | :--- |
| 0x1800 | GAP | 通用访问，设备名、外观等 |
| 0x1801 | GATT | 通用属性，服务变更等 |
| 0x180A | Device Information | 设备信息（型号、厂商、固件版本等） |
| 0x180F | Battery Service | 电池电量 |
| 0x1812 | HID | 键盘、鼠标、多媒体输入 |
| 0x1813 | Scan Parameters | 扫描参数（主机写、从机通知） |

---

## HID 服务 (0x1812)

### 特性列表

| 特性 | UUID | 方向 | 说明 |
| :--- | :--- | :--- | :--- |
| HID Information | 0x2A4A | 读 | bcdHID、Country、Flags |
| Report Map | 0x2A4B | 读 | HID 报告描述符 |
| HID Control Point | 0x2A4C | 写无响应 | Suspend/Exit Suspend |
| Report | 0x2A4D | 读/写/通知 | 复合报告（键盘/鼠标/多媒体等） |
| Protocol Mode | 0x2A4E | 读/写 | Boot/Report 模式 |
| Boot Keyboard Input | 0x2A22 | 通知 | Boot 键盘输入 |
| Boot Keyboard Output | 0x2A32 | 读/写 | Boot 键盘 LED |
| Boot Mouse Input | 0x2A33 | 通知 | Boot 鼠标输入 |

### HID 报告 ID

| Report ID | 用途 | 方向 | 大小 |
| :-------- | :--- | :--- | :--- |
| 0 | 键盘输入 | 设备→主机 | 8B |
| 0 | LED 输出 | 主机→设备 | 1B |
| 1 | 鼠标输入 | 设备→主机 | 4B |
| 2 | 多媒体输入 | 设备→主机 | 2B |
| 4 | 电池电量 (HID Report Ref) | 设备→主机 | 1B |

键盘、鼠标、多媒体报告与 USB HID 格式一致，详见 [HID 通讯协议](./hid.md)。

固件使用 **Report Protocol Mode**。主机通过 Protocol Mode 特性选择 Report 模式，再通过 Report 特性读写各 Report ID 对应数据。

---

## Device Information (0x180A)

| 特性 | UUID | 说明 |
| :--- | :--- | :--- |
| System ID | 0x2A23 | 系统标识 |
| Model Number | 0x2A24 | 型号字符串 |
| Serial Number | 0x2A25 | 序列号 |
| Firmware Revision | 0x2A26 | 固件版本 |
| Hardware Revision | 0x2A27 | 硬件版本 |
| Software Revision | 0x2A28 | 软件版本 |
| Manufacturer Name | 0x2A29 | 厂商名称 |
| PnP ID | 0x2A50 | PnP 标识 |

---

## Battery Service (0x180F)

| 特性 | UUID | 方向 | 说明 |
| :--- | :--- | :--- | :--- |
| Battery Level | 0x2A19 | 读/通知 | 电量百分比 (0–100) |

主机可订阅 Battery Level 通知以获取电量更新。

---

## Scan Parameters (0x1813)

| 特性 | UUID | 方向 | 说明 |
| :--- | :--- | :--- | :--- |
| Scan Interval Window | 0x2A4F | 写 | 主机写入扫描间隔/窗口 |
| Scan Parameter Refresh | 0x2A31 | 通知 | 从机通知刷新请求 |

用于主机控制从机广播参数，符合 BLE 规范。

---

## 配对与绑定

| 配置项 | 值 | 说明 |
| :----- | :--- | :--- |
| Pairing Mode | Wait For Req | 等待主机发起配对 |
| Bonding | 启用 | 保存绑定信息 |
| MITM | 禁用 | 无中间人保护 |
| IO Capabilities | NoInputNoOutput | 无显示/输入能力（默认 Just Works） |

绑定信息存储在 DataFlash 的 BLE SNV 保留扇区。当前编译参数下 SNV 从偏移 `0x7000` 开始，占用 256B，但底层按 4KB 扇区擦除，因此 `0x7000`～`0x7FFF` 都不能与应用数据复用。详见 [DataFlash 布局](./dataflash.md#ble-snv)。

固件启用绑定记录，但同一时刻只支持一个活动连接。在 BLE 模式长按 `FN2` 会清除全部绑定并重启；主机端也要删除旧设备后重新配对。

---

## 广播与连接参数

### 广播

| 参数 | 值 | 单位 |
| :--- | :--- | :--- |
| 广播间隔最小 | 48 | 0.625ms → 30ms |
| 广播间隔最大 | 80 | 0.625ms → 50ms |
| 广播超时 | 60 | 秒 |

### 连接

| 参数 | 值 | 单位 |
| :--- | :--- | :--- |
| 连接间隔 | 8 | 1.25ms → 10ms |
| 从机延迟 | 20 | 连接事件 |
| 监督超时 | 500 | 10ms → 5s |

---

## BLE 协议栈配置 (ble_config.h)

| 配置项 | 默认值 | 说明 |
| :----- | :----- | :--- |
| BLE_SNV | TRUE | 启用 SNV 存储绑定 |
| BLE_SNV_ADDR | 0x77000 - FLASH_ROM_MAX_SIZE | 当前编译参数下为 DataFlash 偏移 0x7000 |
| BLE_SNV_BLOCK | 256 | SNV 块大小 |
| BLE_SNV_NUM | 1 | SNV 块数量 |
| BLE_BUFF_MAX_LEN | 27 | 单包最大长度 (ATT_MTU=23) |
| BLE_TX_NUM_EVENT | 1 | 单连接事件最多发包数 |
| PERIPHERAL_MAX_CONNECTION | 1 | 从机最大连接数 |

---

## 低功耗与唤醒

| 配置项 | 值 | 说明 |
| :----- | :--- | :--- |
| LIGHT 默认超时 | 1 分钟 | 以 DataFlash / Studio 配置为准，0=禁用 |
| DEEP 默认延时 | LIGHT 后 1 分钟 | 总空闲默认约 2 分钟，0=禁用 |
| 按键唤醒 | 启用 | 按键可唤醒休眠 |
| USB 唤醒 | 启用 | USB 插入可唤醒 |

USB 已完成枚举或检测到充电时不会进入自动低功耗。具体用户行为见 [无线款使用手册](./manual.md)，实现见 `kbd_mode.c` 和 DataFlash 中的 system 配置。

---

## 相关文档

- [HID 通讯协议](./hid.md) - 报告格式与配置命令
- [DataFlash 布局](./dataflash.md) - SNV 区与 BLE 存储
- [TMOS 调度](./tmos.md) - 任务/事件/消息与定时处理
- [固件开发](./dev.md) - 编译与调试
