# CH592F USB / BLE / 2.4G 三模架构

三模固件从已实机验证的 `feat/ch592-2g4-pairing` 基线演进，冻结点为
`87aec7a`（标签 `ch592-2g4-stable-20260903`）。接收器代码、RF 帧、管理分片、
最终响应 ACK 和重连清理算法保持兼容，现有接收器无需重新刷写。

## 为什么采用两个应用镜像

WCH 的 `libCH59xBLE.a` 与 `libCH59xRF_FAST.a` 同时导出 TMOS、RFBound、RF role、
基带和 AES 等大量同名符号，而且对应 object 并不是同一份实现。两套库直接链接
不仅会产生符号冲突，也无法保证已经验证的 RFBound 时序。当前 USB/BLE 与 2.4G
代码合计约 245KB，也超过原来的 216KB 单应用区。

因此采用与 WCH 官方三模示例一致的多镜像方案：USB/BLE 应用继续只链接 BLE
库，2.4G 应用继续只链接 RF FAST 库。模式切换保存 `last_mode` 后复位，由统一
Dispatcher 选择应用；对用户仍表现为一套可循环切换的三模固件。

## Flash 布局

| 地址 | 大小 | 内容 |
| --- | ---: | --- |
| `0x00000` | 4KB | JumpIAP |
| `0x01000` | 64KB | 2.4G 应用 |
| `0x11000` | 184KB | USB/BLE 应用 |
| `0x3F000` | 184KB | OTA 单镜像暂存区 |
| `0x6D000` | 12KB | IAP / Dispatcher |

应用在偏移 4 写入类型标识。IAP 根据标识把暂存镜像复制到对应分区，构建过程对
两个应用分别执行分区上限检查。DataFlash 地址不变，因此键位、RGB、宏、当前层、
BLE SNV 和 2.4G 绑定可跨镜像继续使用。

## 模式和默认操作

无有效 runtime 页时默认 USB。模式循环固定为 `USB -> BLE -> 2.4G -> USB`；每次
切换先释放 HID 状态，再写入带序号和 CRC 的 runtime 页并复位。

- FN1 短按或长按：切换到下一模式。
- FN2 短按：下一层。
- FN2 长按：BLE 下清除 BLE 绑定；2.4G 下进入配码；USB 下不清除无线数据。

FN 配置不再由 RF 镜像改写。两个镜像读取同一份配置，并按当前模式解释默认的
无线维护动作。

## OTA

HID 命令 `0x80` 到 `0x84`、分片格式和 CRC32 不变。Studio 先升级非当前应用，
等待 USB 重新枚举，再升级原模式所属应用。IAP 只在复制成功并验证目标头部后清除
更新标志；复制失败或中途掉电会保留暂存镜像并在下次启动重试。第二阶段完成后，
Dispatcher 依据升级前未被修改的 `last_mode` 回到原工作模式。

## 透传和重连为什么保持可靠

管理透传曾经在最后请求分片后连续发送独立 ACK 和真正响应，RFBound DMA 竞争会
使 Host 只收到 ACK，表现为 HID 能输入但改键、改灯超时。稳定实现把完整响应作为
末片的最终 ACK，只有非末片发送独立 ACK，并用事务号、停等重传和响应缓存避免
重复执行写入。三模开发不改变这套协议。

重连后的永久 `ERR_BUSY` 来自旧会话遗留的 callback、分片游标、TX active 和 DMA
描述符。稳定实现只持久化 peer/绑定代次；管理事务全部属于易失会话。RFBound 链路
边界由回调设置 reset-pending，主循环原子清空 RX/TX、ACK、callback、超时、响应
缓存和 TX DMA 环，再允许新事务进入。该算法完整保留在 2.4G 子镜像中。

RFBound 约 5 秒才报告某些突然断电场景仍是已知底层限制，三模合并不把它描述为
已经解决。

## 构建

```powershell
C:/BinaryKeyboardDev/tools/Python312/python.exe tools/scripts/ch592_trimode_full.py
```

产物位于 `firmware/CH592F/build/trimode-5key` 和
`firmware/CH592F/build/trimode-knob`，首次 ISP 应使用 `*-TRIMODE-full.bin` 或
`*-TRIMODE-full.hex`。
