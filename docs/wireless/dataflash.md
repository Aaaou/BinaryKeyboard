# CH592F DataFlash 使用说明

本文记录 BinaryKeyboard CH592F 当前固件实际使用的 DataFlash 布局。地址均为传给 `EEPROM_READ / WRITE / ERASE` 的 DataFlash 偏移。

## 整体布局

CH592F 提供 32KB DataFlash，偏移范围为 `0x0000`～`0x7FFF`。

| 地址范围 | 大小 | 用途 |
| :--- | :--- | :--- |
| `0x0000`～`0x0BFF` | 3KB | 冷 / 温配置，3 个 1KB 轮转槽 |
| `0x0C00`～`0x0FFF` | 1KB | runtime 热数据，4 个 256B 轮转页 |
| `0x1000`～`0x2FFF` | 8KB | 动态 MeowFS 宏区 |
| `0x3000`～`0x6FFF` | 16KB | 当前未使用 |
| `0x7000`～`0x7FFF` | 4KB | BLE SNV 所在擦除扇区，整扇区保留给协议栈 |

当前存储策略：

- 配置按 1KB 槽轮转，槽内只擦写发生变化的 256B 页。
- 当前层和最后工作模式放入独立 runtime 页环，避免每次切层重写整份配置。
- 宏按紧凑的动态条目连续保存，共享 8KB，不再使用“8 槽 × 2KB”旧布局。
- BLE SNV 与应用配置、宏分离。

## 配置槽 `0x0000`～`0x0BFF`

共有 3 个槽：

| 槽 | 地址范围 |
| :--- | :--- |
| 0 | `0x0000`～`0x03FF` |
| 1 | `0x0400`～`0x07FF` |
| 2 | `0x0800`～`0x0BFF` |

保存配置时写入下一个槽。固件先写 payload 页，最后写含 header 的第 0 页；启动时扫描三个槽，校验版本和 CRC 后选择 `save_count` 最大的有效槽。

### 槽内布局

| 槽内偏移 | 大小 | 内容 |
| :--- | :--- | :--- |
| `0x000` | 32B | `kbd_config_header_t` |
| `0x020`～`0x0FF` | 224B | 保留 |
| `0x100` | 64B | `kbd_system_config_t` |
| `0x140`～`0x1FF` | 192B | 保留 |
| `0x200` | 164B | `kbd_keymap_t` |
| `0x2A4`～`0x2FF` | 92B | 保留 |
| `0x300` | 32B | `kbd_fnkey_config_t` |
| `0x320`～`0x33F` | 32B | 保留 |
| `0x340` | 32B | `kbd_rgb_config_t` |
| `0x360`～`0x3FF` | 160B | 保留 |

### 配置头 `kbd_config_header_t`

| 偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0x00` | 4B | `magic` | `0x4D454F57`，即 `MEOW` |
| `0x04` | 2B | `version` | 当前布局版本 `0x0103` |
| `0x06` | 2B | `flags` | 保留 |
| `0x08` | 4B | `crc32` | system、keymap、FN、RGB 四块 CRC32 的异或值 |
| `0x0C` | 4B | `save_count` | 保存序号 |
| `0x10` | 16B | `reserved` | 保留 |

### 系统配置 `kbd_system_config_t`

| 槽内偏移 | 字段 | 说明 |
| :--- | :--- | :--- |
| `0x100` | `default_mode` | 默认模式：0=USB，1=BLE |
| `0x101` | `auto_sleep_min` | LIGHT 休眠分钟数，0=禁用 |
| `0x102` | `debounce_ms` | 按键消抖时间，默认 10ms |
| `0x103` | `log_enabled` | HID 设备日志开关 |
| `0x104` | `deep_sleep_min` | LIGHT 后到 DEEP 的分钟数，0=禁用 |
| `0x105` | `os_mode` | 0=Win，1=Mac |
| `0x106`～`0x13F` | `reserved` | 58B 保留 |

Studio 的 CH592 RGB 读写帧会把 `auto_sleep_min` 和 `deep_sleep_min` 附带在 RGB 配置后面传输，但它们实际持久化在 system 结构中。

### 按键映射 `kbd_keymap_t`

| 槽内偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0x200` | 1B | `num_layers` | 实际层数：5KEY=5，KNOB=4 |
| `0x201` | 1B | `current_layer` | 配置中的基础值；启动后可被 runtime 覆盖 |
| `0x202` | 1B | `default_layer` | 默认层 |
| `0x203` | 1B | `reserved` | 保留 |
| `0x204` | 160B | `layers[5]` | 5 个 32B 层结构 |

每层固定 8 个 `kbd_action_t`，每个动作 4B：

| 字节 | 字段 | 说明 |
| :--- | :--- | :--- |
| 0 | `type` | 键盘、鼠标、媒体、宏、层等动作类型 |
| 1 | `modifier` | 修饰键、层操作或宏触发类型 |
| 2 | `param1` | 主参数 |
| 3 | `param2` | 次参数 |

实际有效键位数由型号决定：5KEY 为 5；KNOB 为 7 个逻辑键位，其中 4～6 是旋钮虚拟动作。

### FN 配置 `kbd_fnkey_config_t`

结构包含 4 个 8B 条目，当前硬件实际使用 FN1 和 FN2；FN3、FN4 保留。

| 条目内偏移 | 大小 | 字段 |
| :--- | :--- | :--- |
| 0 | 1B | `click_action` |
| 1 | 1B | `click_param` |
| 2 | 1B | `long_action` |
| 3 | 1B | `long_param` |
| 4 | 2B | `long_press_ms`，小端序 |
| 6 | 2B | `reserved` |

### RGB 配置 `kbd_rgb_config_t`

| 槽内偏移 | 字段 | 说明 |
| :--- | :--- | :--- |
| `0x340` | `enabled` | 按键 RGB 总开关 |
| `0x341` | `mode` | 0=关，1=静态，2=呼吸，3=闪烁，4=彩虹，5=仅指示灯 |
| `0x342` | `brightness` | 按键灯亮度 0～255 |
| `0x343` | `speed` | 动画速度 0～255 |
| `0x344`～`0x346` | `color_r/g/b` | 静态 / 呼吸颜色 |
| `0x347` | `indicator_enabled` | 状态指示开关 |
| `0x348` | `indicator_brightness` | 指示灯亮度 0～255 |
| `0x349` | `press_effect` | 0=无，1=亮起渐灭，2=熄灭渐亮 |
| `0x34A`～`0x35F` | `reserved` | 22B 保留 |

## runtime 热数据 `0x0C00`～`0x0FFF`

runtime 区由 4 个 256B 页组成。每次写入下一个页，通过 `seq` 选择最新有效页。

### `kbd_runtime_page_t`（256B）

| 页内偏移 | 大小 | 字段 | 说明 |
| :--- | :--- | :--- | :--- |
| `0x00` | 4B | `magic` | `0x52554E54`，即 `RUNT` |
| `0x04` | 2B | `version` | 当前 `0x0001` |
| `0x06` | 2B | `flags` | 保留 |
| `0x08` | 4B | `seq` | 单调递增序号 |
| `0x0C` | 1B | `current_layer` | 当前层 |
| `0x0D` | 1B | `last_mode` | 0=USB，1=BLE，`0xFF`=未知 |
| `0x0E`～`0xFB` | 238B | `reserved` | 保留 |
| `0xFC` | 4B | `crc32` | 对前 252B 的 CRC32 |

层或模式变化后默认延迟约 200ms 保存；快速连续操作会合并为最后一个状态。模式切换、进入睡眠等关键路径会立即 flush。

## 动态 MeowFS 宏区 `0x1000`～`0x2FFF`

宏区总计 8KB。条目从头连续排列，没有固定槽地址，也不在设备端保存名称。

### 单个条目

| 相对偏移 | 大小 | 内容 |
| :--- | :--- | :--- |
| 0 | 1B | marker：`0xAA`=有效，`0x00`=已删除，`0xFF`=结束 / 空白 |
| 1 | 1B | `action_count`，0～255 |
| 2～ | `action_count × 2B` | `kbd_macro_action_t` 数组 |

每个宏动作由 1B 类型和 1B 参数组成。单宏最多 255 个动作，宏数量由每个宏的大小和 8KB 总容量共同决定。

Studio 保存 CH592 宏时会：

1. 读取当前 MeowFS 目录。
2. 在内存中新增、替换或删除条目。
3. 校验每个宏的动作数和序列化后的总大小。
4. 擦除宏区并按 58B HID 分块重写紧凑结果。

固件底层按 256B 页执行读改写；恢复出厂会擦除全部 32 个宏页。

## BLE SNV

`ble_config.h` 当前定义：

```c
#define BLE_SNV_ADDR  (0x77000 - FLASH_ROM_MAX_SIZE)
#define BLE_SNV_BLOCK 256
#define BLE_SNV_NUM   1
```

在当前 `FLASH_ROM_MAX_SIZE = 0x70000` 下，SNV 偏移为 `0x7000`，使用 256B 保存绑定记录。

CH592 的写回实现会以 `addr & 0xFFFFF000` 擦除 4KB 扇区，因此应用代码应把 `0x7000`～`0x7FFF` 整段视为 BLE 专用保留区，不能在同一扇区另存应用数据。

清除蓝牙配对只影响 SNV；普通 `CFG_RESET` 不会替代清除绑定。

## 保存与恢复行为

### 普通配置

- `KEYMAP_SET`、`FNKEY_SET`、`RGB_SET`、`CFG_OS_SET` 先修改 RAM 配置。
- Studio 随后调用 `CFG_SAVE`，写入下一个有效配置槽。
- 如果 CRC 与当前槽完全相同，固件跳过重复写入。

### 当前层与工作模式

- 不写完整配置槽，只进入 runtime 页环。
- 默认延迟保存以合并快速变化。

### 恢复出厂

`CFG_RESET` 会加载默认 system、keymap、FN、RGB，擦除整个 8KB MeowFS，并保存新配置。当前实现不会擦除 runtime 页环或 BLE SNV；清除绑定仍需走 BLE 清配对动作。

## 一致性与掉电保护

- 配置槽使用 `magic + version + CRC32 + save_count` 校验。
- 新配置先写 payload，最后写 header，掉电时仍可回退到旧槽。
- runtime 页使用独立 `magic + version + seq + CRC32`。
- Studio 会拒绝损坏的 MeowFS 元数据、不完整的分块读取和超出容量的宏写入。

调试原始 DataFlash 前应先通过“配置库”导出备份。直接写裸字节可能同时破坏配置轮转、runtime 状态或宏目录。
