# TMOS 使用说明（CH592F）

> 注意：独立 2.4G RF-only 镜像由 RFBound 使用 Timer3/TMOS 调度链。该镜像的宏延时和
> RGB 周期任务必须使用独立的 TMR0 1 ms tick，并在主循环执行实际工作；USB/BLE 镜像
> 才使用本文后续的 TMOS 任务模式。不要把 RF-only 的延时动作重新注册到 TMOS。

基于 WCH BLE 库的 `TMOS`（任务/事件调度）在本项目中的用法说明，重点覆盖：

- `task + event` 事件模型
- 定时事件与消息事件
- 本项目现有调用路径
- 用 TMOS 做高频状态（如层号）延迟保存

> 本页面向项目开发，术语与接口以 `CH59xBLE_LIB.h` 和当前固件代码为准。

## 什么是 TMOS

TMOS 是 WCH BLE 库内置的轻量任务调度系统，核心是：

- `Task`：任务（注册后获得 `taskID`）
- `Event`：事件位图（每个 bit 表示一种事件）
- `Timer`：延时触发某个事件
- `Message`：任务消息队列（触发 `SYS_EVENT_MSG`）

任务处理函数原型（项目实际使用）：

```c
uint16_t Task_ProcessEvent(uint8_t task_id, uint16_t events);
```

处理完某个事件后，返回 `events ^ EVT_xxx`（清除已处理事件位）。

## 核心规则（很重要）

### 1. 事件是位图

- 一个任务一次可能收到多个事件（`events` 是位图）
- 用 `if (events & EVT_xxx)` 逐个处理
- 每个事件用独立 bit，避免冲突

### 2. `SYS_EVENT_MSG` 是系统保留事件

- `0x8000`（最高位）由 TMOS 用于“消息到达”
- 用户自定义事件不要占用 `0x8000`
- 你自己的事件建议从 `0x0001`、`0x0002`、`0x0004`... 开始

### 3. 任务优先级与注册顺序有关

- 先注册的任务优先级更高
- 优先级相同时按注册顺序处理

### 4. 事件处理函数要短小

- 不要在事件回调里做长时间阻塞操作
- 慢操作（如 Flash 擦写）应通过“延时事件 + 合并写入”处理

## 常用 API（项目里会用到）

接口声明见 `firmware/CH592F/ble/lib/CH59xBLE_LIB.h`。

| API | 作用 | 备注 |
| :-- | :-- | :-- |
| `TMOS_ProcessEventRegister(cb)` | 注册任务处理函数 | 返回 `taskID` |
| `TMOS_SystemProcess()` | 处理系统任务/事件 | 主循环里持续调用 |
| `tmos_set_event(task, evt)` | 立即触发事件 | 异步执行 |
| `tmos_start_task(task, evt, time)` | 延时触发一次事件 | 常用防抖/定时 |
| `tmos_start_reload_task(task, evt, time)` | 周期事件 | 自动重装载 |
| `tmos_stop_task(task, evt)` | 停止某事件定时器 | 取消延时任务 |
| `tmos_get_task_timer(task, evt)` | 查询事件剩余定时 | 调试用 |
| `tmos_msg_allocate(len)` | 分配消息缓冲 | 从 TMOS 内存池分配 |
| `tmos_msg_send(task, msg)` | 投递消息到任务 | 会触发 `SYS_EVENT_MSG` |
| `tmos_msg_receive(task)` | 读取一条消息 | 在 `SYS_EVENT_MSG` 中调用 |
| `tmos_msg_deallocate(msg)` | 释放消息 | 收到消息后要释放 |

## 项目中的 TMOS 调用路径

### 主循环驱动 TMOS

`firmware/CH592F/app/Main.c`

```c
while (1) {
    TMOS_SystemProcess();
    KBD_Mode_Process();
    KBD_Core_Process();
    KBD_Log_Flush();
}
```

说明：

- `TMOS_SystemProcess()` 负责分发 TMOS 任务事件
- 你的业务循环（模式/按键/日志）继续在主循环执行
- 这意味着 TMOS 事件处理和业务主循环是协作式的，事件回调越短越好

### 示例 1：RGB 周期更新（定时事件）

`firmware/CH592F/keyboard/src/kbd_rgb.c`

- 注册任务：`TMOS_ProcessEventRegister(KBD_RGB_ProcessEvent)`
- 启动定时：`tmos_start_task(..., RGB_UPDATE_EVT, ...)`
- 在事件中处理后再次 `tmos_start_task(...)`，形成周期调度

这是项目里最清晰的 TMOS 用法模板，适合复用到 runtime 保存任务。

### 示例 2：BLE/HAL 消息事件（`SYS_EVENT_MSG`）

`firmware/CH592F/ble/core/src/ble_mcu.c` 与 `firmware/CH592F/ble/hid/src/ble_hid.c`

典型模式：

```c
if (events & SYS_EVENT_MSG) {
    uint8_t *msg = tmos_msg_receive(task_id);
    if (msg) {
        // 处理消息
        tmos_msg_deallocate(msg);
    }
    return (events ^ SYS_EVENT_MSG);
}
```

重点：

- 处理完消息必须 `tmos_msg_deallocate()`
- `SYS_EVENT_MSG` 是系统事件，优先处理通常更稳

## runtime 延迟保存的当前实现

`firmware/CH592F/keyboard/src/kbd_storage.c` 已实现 runtime 保存任务。它保存 `current_layer` 和 `last_mode`，不在按键路径直接擦写 DataFlash。

| 项目 | 当前实现 |
| :--- | :--- |
| 任务事件 | `KBD_STORAGE_RUNTIME_SAVE_EVT = 0x0001` |
| 延迟 | 默认 200ms |
| 失败重试 | 默认 100ms |
| 存储位置 | `0x0C00`～`0x0FFF` 的 4 页轮转区 |
| 写入时机 | 层或工作模式变化后；切换模式、进入休眠时立即 flush |

调用链如下：

1. `KBD_SetCurrentLayer()` 或 `KBD_SetLastMode()` 更新 RAM 中的待保存状态。
2. `KBD_Storage_RequestRuntimeSave()` 停止旧定时器并重新开始 200ms 计时。
3. 任务到期后调用 `SaveRuntimeState()` 写入下一张 256B 页。
4. 写失败时重新安排短延时重试。

自定义事件不得使用 `0x8000`，该位保留给 `SYS_EVENT_MSG`。

## TMOS 内存池（项目注意点）

`firmware/CH592F/app/Main.c` 中定义了 TMOS/BLE 使用的内存池：

```c
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];
```

保持 4 字节对齐。`BLE_MEMHEAP_SIZE` 不足时，消息分配和协议栈行为会异常；新增消息任务前应测量内存余量。

## 常见问题

### 1. 在高频路径直接写 Flash

问题：

- 层切换/按键路径里直接 `EEPROM_ERASE/WRITE` 会放大延迟

处理：使用现有 runtime 延迟保存路径，不要在按键路径写 Flash。

### 2. 忘记释放消息

问题：

- `tmos_msg_receive()` 后不 `tmos_msg_deallocate()` 会泄漏内存池

处理：在 `SYS_EVENT_MSG` 分支中按“receive → process → deallocate”顺序释放消息。

### 3. 事件位冲突

问题：

- 同一任务里多个事件用到相同 bit，会出现逻辑串扰

处理：在每个任务附近集中定义事件位，使用 `0x0001/0x0002/0x0004...`。

### 4. 使用 `0x8000` 作为自定义事件

问题：

- 与 `SYS_EVENT_MSG` 冲突

处理：保留 `0x8000` 给系统消息事件。

### 5. 事件处理函数过长

问题：

- 会影响 BLE/USB 响应及时性

处理：事件回调只做状态推进和短操作；长操作拆分或延后。

## 调试

- 给每个 TMOS 任务记录 `taskID`（日志里打印一次）
- 对关键事件打印节流日志（不要每次都打印）
- 调试“事件没触发”时优先检查：
  - 任务是否已注册
  - 事件 bit 是否冲突
  - 是否误用了 `0x8000`
  - `tmos_start_task()` 的 `taskID` 是否正确
  - 主循环是否持续调用 `TMOS_SystemProcess()`

## 参考资料

- WCH 产品页（CH592，官方资料入口）：<https://www.wch.cn/products/CH592.html>
- 《CH58x BLE 软件开发参考手册》TMOS 章节（镜像，便于检索）：<https://manuals.plus/vi/bez-imeni/ch58x-ble-software-development-manual>

本文档以当前 CH592F 固件调用方式为准；WCH API 细节以官方资料为准。
