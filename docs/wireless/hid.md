# HID 通讯协议（USB 配置通道）

BinaryKeyboard 无线版通过 USB HID 配置通道与 Studio（WebHID）通信。本文档以当前固件实现为准，给出逐字节协议定义。

## 概览

| 项目 | 说明 |
| :--- | :--- |
| 配置通道帧长 | `64B` |
| WebHID 模式 | **无 Report ID**（`reportId=0`） |
| 通道方向 | 主机发送命令帧，设备返回响应帧 |
| 字节序 | 以字段定义为准（协议中同时存在大端/小端字段） |

除异步日志 `KBD_CMD_LOG (0x70)` 外，响应数据区 `DATA[0]` 均为状态码（`kbd_resp_t`）。

---

## 配置帧格式（64B）

### 主机 → 设备（命令帧）

对应固件 `kbd_cmd_frame_t`（`firmware/CH592F/keyboard/include/kbd_types.h`）。

| 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0x00` | 1 | `CMD` | 命令码（`kbd_cmd_t`） |
| `0x01` | 1 | `SUB` | 子命令/序号（层号、槽位号、宏分包序号等） |
| `0x02` | 1 | `LEN` | `DATA` 有效长度（`0~61`） |
| `0x03~0x3F` | 61 | `DATA` | 请求参数 |

### 设备 → 主机（响应帧）

当前固件响应帧格式与命令帧一致（`CMD` 为原命令回显，不携带状态码）。状态码放在 `DATA[0]`。

| 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0x00` | 1 | `CMD` | 原命令码回显 |
| `0x01` | 1 | `SUB` | 原 `SUB` 回显（如层号、槽位号） |
| `0x02` | 1 | `LEN` | `DATA` 有效长度 |
| `0x03~0x3F` | 61 | `DATA` | 响应数据（通常 `DATA[0]` 为状态码） |

### Studio / WebHID 对接（当前实现）

`tools/studio/src/services/HidService.ts` 使用统一封包方法：

- `sendCommand(cmd, sub, data)` 构造 `[CMD][SUB][LEN][DATA...]`
- `sendReport(0, frame)` 发送 64B
- `inputreport` 回调读取 `event.data.buffer`（64B）
- `CMD=0x70` 作为异步日志，不进入命令响应等待队列

---

## 状态码（`kbd_resp_t`）

| 值 | 名称 | 含义 |
| :--- | :--- | :--- |
| `0x00` | `KBD_RESP_OK` | 成功 |
| `0x01` | `KBD_RESP_ERR_INVALID` | 无效命令 |
| `0x02` | `KBD_RESP_ERR_PARAM` | 参数错误 |
| `0x03` | `KBD_RESP_ERR_BUSY` | 设备忙 |
| `0x04` | `KBD_RESP_ERR_FLASH` | Flash 操作失败 |
| `0x10` | `KBD_RESP_ERR_TOO_LARGE` | 数据过大 |
| `0x11` | `KBD_RESP_ERR_NO_SPACE` | 存储空间不足 |
| `0x12` | `KBD_RESP_ERR_NOT_FOUND` | 未找到目标 |

---

## 命令码总表（当前固件）

| 分类 | 命令 | 码 | `SUB` 含义 | Studio 当前是否封装 |
| :--- | :--- | :--- | :--- | :--- |
| 系统 | `SYS_INFO` | `0x01` | `0` | 是 |
| 系统 | `SYS_STATUS` | `0x02` | `0` | 是 |
| 配置 | `CFG_SAVE` | `0x10` | `0` | 是 |
| 配置 | `CFG_LOAD` | `0x11` | `0` | 是 |
| 配置 | `CFG_RESET` | `0x12` | `0` | 是 |
| 按键 | `KEYMAP_GET` | `0x20` | 层号 | 是 |
| 按键 | `KEYMAP_SET` | `0x21` | 层号 | 是 |
| 按键 | `LAYER_GET` | `0x22` | `0` | 间接（状态获取可替代） |
| 按键 | `LAYER_SET` | `0x23` | `0` | 可扩展使用 |
| RGB | `RGB_GET` | `0x30` | `0` | 是 |
| RGB | `RGB_SET` | `0x31` | `0` | 是 |
| 宏 | `MACRO_INFO` | `0x40` | `0` | 是 |
| 宏 | `MACRO_GET` | `0x41` | `0` | 是 |
| 宏 | `MACRO_SET` | `0x42` | `0=擦除`、`1=写入` | 是 |
| 宏 | `MACRO_DEL` | `0x43` | 有效宏索引 | 是 |
| FN | `FNKEY_GET` | `0x50` | `0` | 是 |
| FN | `FNKEY_SET` | `0x51` | `0` | 是 |
| 电源 | `BATTERY` | `0x60` | `0` | 是 |
| 日志 | `LOG` | `0x70` | 日志类别 | 异步接收 |
| 日志 | `LOG_GET` | `0x71` | `0` | 是 |
| 日志 | `LOG_SET` | `0x72` | `0` | 是 |

---

## 字节级协议定义（当前实现）

说明：下文 `DATA[n]` 均指帧内 `0x03` 起始的数据区偏移。

### 1. `SYS_INFO (0x01)`

**请求**

| 字段 | 值 |
| :--- | :--- |
| `SUB` | `0x00` |
| `LEN` | `0` |
| `DATA` | 无 |

**响应**（`LEN=18`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | 状态码 |
| `1` | 1 | `vid_hi` | VID 高字节 |
| `2` | 1 | `vid_lo` | VID 低字节 |
| `3` | 1 | `pid_hi` | PID 高字节 |
| `4` | 1 | `pid_lo` | PID 低字节 |
| `5` | 1 | `version_major` | 固件主版本 |
| `6` | 1 | `version_minor` | 固件次版本 |
| `7` | 1 | `version_patch` | 固件补丁版本 |
| `8` | 1 | `max_layers` | 最大层数 |
| `9` | 1 | `max_keys` | 单层最大键位数 |
| `10` | 1 | `macro_count` | 当前 MeowFS 中的有效宏数 |
| `11` | 1 | `keyboard_type` | 0=基础款, 1=五键款, 2=旋钮款 |
| `12` | 1 | `actual_key_count` | 当前键盘类型实际键位数 |
| `13` | 1 | `fn_key_count` | FN 键数量 |
| `14~17` | 4 | `reserved` | 当前为 `0` |

`VID/PID` 在该响应中采用高字节在前（大端表示）。

### 2. `SYS_STATUS (0x02)`

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=9`；旧固件为 `LEN=6`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | 状态码 |
| `1` | 1 | `work_mode` | 当前模式（USB/BLE/...） |
| `2` | 1 | `conn_state` | 连接状态：`0=断开, 1=广播中, 2=已连接, 3=挂起` |
| `3` | 1 | `current_layer` | 当前层 |
| `4` | 1 | `battery_level` | 电量（0-100） |
| `5` | 1 | `is_charging` | 滤波后的充电状态（0/1） |
| `6` | 1 | `adc_raw_lo` | 最近一次 ADC 原始平均值低字节 |
| `7` | 1 | `adc_raw_hi` | 最近一次 ADC 原始平均值高字节 |
| `8` | 1 | `charge_pin_raw` | TP4054 CHRG 瞬时原始电平：`0=低`，`1=高` |

### 3. 配置命令 `CFG_SAVE / CFG_LOAD / CFG_RESET`（`0x10/0x11/0x12`）

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=1`）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `status` |

### 4. `KEYMAP_GET (0x20)`

**请求**

| 字段 | 值 |
| :--- | :--- |
| `SUB` | `layer_index (0~4)` |
| `LEN` | `0` |

**响应（成功）**（`LEN=36`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | `0x00` |
| `1` | 1 | `num_layers` | 当前层数 |
| `2` | 1 | `current_layer` | 当前激活层 |
| `3` | 1 | `default_layer` | 默认层 |
| `4~35` | 32 | `layer_data` | `kbd_layer_t`（8 键 × 4B） |

**响应（失败）**（`LEN=1`）

| `DATA[0]` | 含义 |
| :--- | :--- |
| `KBD_RESP_ERR_PARAM` | 层号超出 `num_layers` |

#### `layer_data`（32B）=`kbd_layer_t`

每层固定 8 个按键，每键 4 字节 `kbd_action_t`：

| 偏移（层内） | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `type` |
| `1` | 1 | `modifier` |
| `2` | 1 | `param1` |
| `3` | 1 | `param2` |

第 `i` 个键偏移 = `i * 4`。

### 5. `KEYMAP_SET (0x21)`

**请求**（`LEN=35`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `num_layers` | `1~KBD_MAX_LAYERS` |
| `1` | 1 | `reserved` | 保留（Studio 当前写 `0`） |
| `2` | 1 | `default_layer` | 默认层 |
| `3~34` | 32 | `layer_data` | 目标层 `kbd_layer_t` |

`SUB = layer_index (0~4)`。

**响应**（`LEN=1`）

| `DATA[0]` | 含义 |
| :--- | :--- |
| `status` | 成功或失败 |

::: warning 兼容行为
当前固件对 `LEN < 35` 不会直接报错，仍可能返回 `OK`（仅打印日志）。Studio 应始终发送完整 `35B`。
:::

### 6. `LAYER_GET (0x22)`

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=4`）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `status` |
| `1` | 1 | `current_layer` |
| `2` | 1 | `num_layers` |
| `3` | 1 | `default_layer` |

### 7. `LAYER_SET (0x23)`

**请求**（`LEN>=1`）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `target_layer` |

**响应**（`LEN=2`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | `OK` 或 `ERR_PARAM` |
| `1` | 1 | `current_layer` | 实际切换后的层 |

### 8. `RGB_GET (0x30)`

**请求**：`SUB=0x00, LEN=0`

**响应**（当前 `LEN=15`；旧固件可能返回 13/14）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `status` |
| `1` | 1 | `enabled` |
| `2` | 1 | `mode` |
| `3` | 1 | `brightness` |
| `4` | 1 | `speed` |
| `5` | 1 | `color_r` |
| `6` | 1 | `color_g` |
| `7` | 1 | `color_b` |
| `8` | 1 | `indicator_enabled` |
| `9` | 1 | `indicator_brightness` |
| `10` | 1 | `press_effect` |
| `11` | 1 | `auto_sleep_min` |
| `12` | 1 | `deep_sleep_min` |
| `13` | 1 | `seamless_wake`，LIGHT 首键正常执行 |

### 9. `RGB_SET (0x31)`

**请求**（当前 `LEN=14`；兼容旧固件的 12/13）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `enabled` | RGB 总开关 |
| `1` | 1 | `mode` | 灯效模式 |
| `2` | 1 | `brightness` | 按键灯亮度 |
| `3` | 1 | `speed` | 动画速度 |
| `4` | 1 | `color_r` | 静态颜色 R |
| `5` | 1 | `color_g` | 静态颜色 G |
| `6` | 1 | `color_b` | 静态颜色 B |
| `7` | 1 | `indicator_enabled` | 保留；固件始终启用状态指示灯 |
| `8` | 1 | `indicator_brightness` | 指示灯亮度；低于 13 时按 13 保存 |
| `9` | 1 | `press_effect` | `0=无`、`1=亮起渐灭`、`2=熄灭渐亮`；其他值按 0 保存 |
| `10` | 1 | `auto_sleep_min` | LIGHT 休眠分钟数；`0=禁用` |
| `11` | 1 | `deep_sleep_min` | 进入 LIGHT 后到 DEEP 的分钟数；`0=禁用` |
| `12` | 1 | `seamless_wake` | LIGHT 首键正常执行；旧固件可省略 |

**响应**（`LEN=1`）：`DATA[0] = status`

### 10. `FNKEY_GET (0x50)`

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=33`）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `status` |
| `1~32` | 32 | `kbd_fnkey_config_t` |

#### `kbd_fnkey_config_t`（32B）字节布局

由 4 个 `kbd_fnkey_entry_t` 组成，每项 8B。

单项（8B）布局：

| 偏移（项内） | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `click_action` |
| `1` | 1 | `click_param` |
| `2` | 1 | `long_action` |
| `3` | 1 | `long_param` |
| `4` | 1 | `long_press_ms_lo` |
| `5` | 1 | `long_press_ms_hi` |
| `6` | 1 | `reserved0` |
| `7` | 1 | `reserved1` |

第 `i` 个 FN 项偏移 = `i * 8`。

### 11. `FNKEY_SET (0x51)`

**请求**（`LEN=32`）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0~31` | 32 | `kbd_fnkey_config_t` |

**响应**（`LEN=1`）：`DATA[0] = status`

### 12. `BATTERY (0x60)`

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=8`；旧固件为 `LEN=5`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | 状态码 |
| `1` | 1 | `level` | 电量百分比 `0~100` |
| `2` | 1 | `charging` | 滤波后的状态：`0=未充电, 1=充电中` |
| `3` | 1 | `voltage_lo` | 电压毫伏低字节 |
| `4` | 1 | `voltage_hi` | 电压毫伏高字节 |
| `5` | 1 | `adc_raw_lo` | 最近一次 ADC 原始平均值低字节（未应用校准偏移） |
| `6` | 1 | `adc_raw_hi` | 最近一次 ADC 原始平均值高字节（未应用校准偏移） |
| `7` | 1 | `charge_pin_raw` | TP4054 CHRG 引脚瞬时原始电平：`0=低`，`1=高` |

::: info 字节序
`voltage_mV` 和 `adc_raw` 在 `BATTERY` 响应中使用 **小端序**（LE）。Studio 按 `LEN` 判断诊断字段是否存在，仍兼容只返回前 5 字节的旧固件。
:::

::: info 电压与电量算法
CH592 按 WCH 外部分压参考方式使用 `0dB` 单端 ADC：丢弃首次转换，连续采样去掉一个最大值和一个最小值，再应用 ADC 粗校准、`1.05V` 参考值和 `100kΩ/100kΩ` 分压比。周期结果使用低通滤波；超过 `250mV` 的突变必须由第二组采样确认。

电量由单节 LiPo 电压曲线估算。充电时先补偿 TP4054 造成的端电压抬升，普通曲线结果最高限制为 `99%`；只有高压区连续确认满足满电锁存条件后才显示 `100%`。由于硬件没有电流检测或库仑计，该百分比不是精确 SOC。`CHRG=1` 也只能表示 TP4054 当前没有拉低引脚，无法区分“未接入 VBUS”和“已经充电结束”。

为适配样板约 `4.17~4.18V` 的实测满充平台，高压区另有满电锁存：`4.16V` 以上连续 3 次有效采样后显示 `100%`，降到 `4.10V` 以下连续 2 次后解除。阈值之间的回差用于避免满电百分比随采样误差反复变化。

满电锁存后，CHRG 高电平稳定约 `1s` 即将稳定充电状态切换为“未充电”。随后 TP4054 因板载负载产生的短低电平补充充电脉冲只保留在 `charge_pin_raw` 中，不会重新改变稳定状态或 RGB；满电锁存解除后恢复普通的 `300ms` 进入、`10s` 退出判定。

充电末段 TP4054 可能因终止和板载负载进入短周期补充充电，CHRG 原始电平会在 `0/1` 间切换。固件每 `100ms` 采样 CHRG：连续低电平约 `300ms` 后进入“充电中”，连续高电平约 `10s` 后退出“充电中”。`charging` 使用该稳定状态，RGB 也以它为准；`charge_pin_raw` 保留瞬时电平用于诊断，因此两者在退出延迟期间可以暂时不同。
:::

### 13. `LOG_GET (0x71)`

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=2`）

| `DATA` 偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| `0` | 1 | `status` |
| `1` | 1 | `enabled` |

### 14. `LOG_SET (0x72)`

**请求**（`LEN=1`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `enabled` | `0=关, 非0=开` |

**响应**（`LEN=1`）：`DATA[0] = status`

### 15. 异步日志 `LOG (0x70)`

设备主动推送，不走命令应答队列。

**帧格式**

| 帧字段 | 含义 |
| :--- | :--- |
| `CMD` | `0x70` |
| `SUB` | 日志类别 `kbd_log_category_t` |
| `LEN` | 日志数据长度（`0~8`） |
| `DATA` | 日志负载（**无状态码前缀**） |

#### 日志类别与 `DATA` 字节定义

| `SUB` | 类别 | `DATA` 字节定义 |
| :--- | :--- | :--- |
| `0x01` | `KEY_EVENT` | `[0]=key_index, [1]=pressed, [2]=action_type, [3]=param` |
| `0x02` | `FN_EVENT` | `[0]=fn_id, [1]=is_long, [2]=action, [3]=param` |
| `0x03` | `LAYER_EVENT` | `[0]=old_layer, [1]=new_layer` |
| `0x04` | `MODE_EVENT` | `[0]=old_mode, [1]=new_mode` |
| `0x05` | `BLE_EVENT` | `[0]=state` |
| `0x06` | `RGB_EVENT` | `[0]=mode, [1]=brightness` |
| `0x07` | `SYSTEM_EVENT` | `[0]=event`（`BOOT/SLEEP/WAKEUP`） |

### 16. `MACRO_INFO (0x40)`

**请求**：`SUB=0x00, LEN=0`

**响应**（`LEN=8`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | 状态码 |
| `1~2` | 2 | `total_size` | MeowFS 总容量，大端 |
| `3~4` | 2 | `page_size` | 擦除页大小，大端 |
| `5` | 1 | `used_count` | 有效宏数 |
| `6~7` | 2 | `free_bytes` | 剩余字节数，大端 |

### 17. `MACRO_GET (0x41)`

**请求**（`SUB=0x00, LEN=3`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `offset_hi` | 读取偏移高字节 |
| `1` | 1 | `offset_lo` | 读取偏移低字节 |
| `2` | 1 | `req_len` | 请求长度；固件最多读取 59 字节 |

**响应（成功）**（`LEN = 2 + read_len`）

| `DATA` 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0` | 1 | `status` | `OK` |
| `1` | 1 | `read_len` | 实际读取长度 |
| `2..` | `read_len` | `chunk` | MeowFS 原始数据 |

偏移使用高字节在前（大端表示）。读取到末尾时，固件返回的 `read_len` 会小于请求长度；协议没有 `is_last` 字段。

### 18. `MACRO_SET (0x42)`

固件将 Flash 操作延后到 TMOS 主循环执行，完成后再发送响应。

| `SUB` | 请求 | 用途 |
| :--- | :--- | :--- |
| `0` | `DATA[0]=page` | 擦除指定页；`page=0xFF` 擦除整个宏区 |
| `1` | `offset_hi, offset_lo, len, data...` | 从指定偏移写入原始 MeowFS 数据；每包最多 58 字节数据 |

`offset` 为大端。擦除或写入成功时响应 `LEN=1`，`DATA[0]=OK`；参数错误或 Flash 操作失败时返回相应错误码。

### 19. `MACRO_DEL (0x43)`

**请求**：`SUB=有效宏索引, LEN=0`

**响应**（`LEN=1`）

| `DATA[0]` | 含义 |
| :--- | :--- |
| `status`（`OK` 或 `ERR_PARAM`） | 删除结果 |

索引按 MeowFS 中的有效宏顺序编号。删除操作仅写入删除标记，不会立即回收空间；Studio 保存宏时会重建整个宏区。

---

## Studio 当前命令调用映射（`HidService`）

| Studio 方法 | 命令 | 请求字节 | 响应字节（`DATA`） |
| :--- | :--- | :--- | :--- |
| `getSysInfo()` | `SYS_INFO` | 无 | `status + 17B 信息` |
| `getSysStatus()` | `SYS_STATUS` | 无 | `status + 5B 状态` |
| `getKeymap(layer)` | `KEYMAP_GET` | `SUB=layer` | `status + 3B 层头 + 32B层数据` |
| `setKeymap(...)` | `KEYMAP_SET` | `35B` | `status` |
| `getRgbConfig()` | `RGB_GET` | 无 | `status + 12B` |
| `setRgbConfig()` | `RGB_SET` | `12B` | `status` |
| `getFnKeyConfig()` | `FNKEY_GET` | 无 | `status + 32B` |
| `setFnKeyConfig()` | `FNKEY_SET` | `32B` | `status` |
| `getMacroOverview()` | `MACRO_INFO` | 无 | `status + 7B MeowFS 信息` |
| `getMacroData()` | `MACRO_INFO`、`MACRO_GET` | 由 Studio 分段读取 | MeowFS 条目 |
| `setMacroData()` | `MACRO_SET` | 擦除全区后分段写入 | `status` |
| `deleteMacro()` | `MACRO_DEL` | `SUB=宏索引` | `status` |
| `saveConfig()` | `CFG_SAVE` | 无 | `status` |
| `loadConfig()` | `CFG_LOAD` | 无 | `status` |
| `resetConfig()` | `CFG_RESET` | 无 | `status` |
| `getBattery()` | `BATTERY` | 无 | `status + 4B 电池信息 + 3B 原始诊断值` |
| `getLogConfig()` | `LOG_GET` | 无 | `status + enabled` |
| `setLogConfig()` | `LOG_SET` | `enabled(1B)` | `status` |

---

## 相关文档

- [DataFlash 布局](./dataflash.md) - 配置槽轮转、runtime 热数据页、宏区布局
- [无线版开发指南](./dev.md) - 构建与固件架构
- [改键软件使用](./remap.md) - Studio / WebHID 使用说明
