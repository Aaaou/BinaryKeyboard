/********************************** USB HID Functions Implementation ***********
 * File Name          : usb_hid.c
 * Author             : Custom USB Library
 * Version            : V2.0
 * Date               : 2024/11/07
 * Description        : USB HID 功能实现（基于 WCH 官方库）
 *******************************************************************************/

#include "usb_hid.h"
#include "CH59x_usbdev.h"
#include "kbd_command.h"
#include "kbd_types.h"
#include "debug.h"
#include <stdbool.h>
#include <string.h>

#define TAG "USB"
/* EP4 bInterval = 10ms，TMOS 上下文发送响应时需等待 host 读取上一包。
 * 60 MHz RISC-V 下 ~10 cycles/iter → 200 000 iter ≈ 33 ms，覆盖 2 个
 * 轮询周期，避免 KBD_Log_Flush 占用 EP4 IN 时 MACRO_SET 响应被丢弃。 */
#define USB_EP_READY_TIMEOUT 200000U
#define USB_CONFIG_QUEUE_DEPTH 4u

typedef struct {
    kbd_cmd_frame_t frame;
    uint8_t valid;
} usb_config_rx_item_t;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[63];
} usb_config_tx_item_t;

static usb_config_rx_item_t s_config_rx_queue[USB_CONFIG_QUEUE_DEPTH];
static usb_config_tx_item_t s_config_tx_queue[USB_CONFIG_QUEUE_DEPTH];
static volatile uint8_t s_config_rx_head;
static volatile uint8_t s_config_rx_tail;
static volatile uint8_t s_config_tx_head;
static volatile uint8_t s_config_tx_tail;

/* ==================== Global Variables ==================== */
USB_KeyboardReport_t g_KeyboardReport = {0};
USB_MouseReport_t    g_MouseReport = {0};
USB_ConsumerReport_t g_ConsumerReport = {0};
USB_ConfigReport_t   g_ConfigReport = {0};

uint8_t g_KeyboardLEDs = 0;

static bool USB_WaitEPInReady(uint8_t ep)
{
    uint32_t timeout = USB_EP_READY_TIMEOUT;

    while (timeout--) {
        switch (ep) {
        case 1:
            if (EP1_GetINSta()) return true;
            break;
        case 2:
            if (EP2_GetINSta()) return true;
            break;
        case 3:
            if (EP3_GetINSta()) return true;
            break;
        case 4:
            if (EP4_GetINSta()) return true;
            break;
        default:
            return false;
        }
    }

    LOG_W(TAG, "EP%d IN busy timeout", ep);
    return false;
}

/* ==================== Keyboard Functions ==================== */

/**
 * @brief 初始化键盘
 */
void USB_Keyboard_Init(void)
{
    memset(&g_KeyboardReport, 0, sizeof(USB_KeyboardReport_t));
    g_KeyboardLEDs = 0;
}

/**
 * @brief 按下按键
 */
void USB_Keyboard_Press(uint8_t modifier, uint8_t *keys, uint8_t num_keys)
{
    g_KeyboardReport.modifier = modifier;
    
    uint8_t count = (num_keys > 6) ? 6 : num_keys;
    for(uint8_t i = 0; i < count; i++) {
        g_KeyboardReport.keycode[i] = keys[i];
    }
    for(uint8_t i = count; i < 6; i++) {
        g_KeyboardReport.keycode[i] = 0;
    }
    
    USB_Keyboard_SendReport();
}

/**
 * @brief 释放所有按键
 */
void USB_Keyboard_Release(void)
{
    memset(&g_KeyboardReport, 0, sizeof(USB_KeyboardReport_t));
    USB_Keyboard_SendReport();
}

/**
 * @brief 按下并释放一个按键
 */
void USB_Keyboard_Type(uint8_t modifier, uint8_t key)
{
    uint8_t keys[1] = {key};
    USB_Keyboard_Press(modifier, keys, 1);
    mDelaymS(20);
    USB_Keyboard_Release();
    mDelaymS(20);
}

/**
 * @brief 发送键盘报告
 */
void USB_Keyboard_SendReport(void)
{
    // 等待上一次传输完成（带超时，避免异常状态卡死）
    if (!USB_WaitEPInReady(1)) {
        return;
    }
    
    memcpy(pEP1_IN_DataBuf, &g_KeyboardReport, sizeof(USB_KeyboardReport_t));
    DevEP1_IN_Deal(sizeof(USB_KeyboardReport_t));
}

bool USB_Keyboard_TrySend(const USB_KeyboardReport_t *report)
{
    if (!report || !EP1_GetINSta()) return false;
    memcpy(pEP1_IN_DataBuf, report, sizeof(*report));
    DevEP1_IN_Deal(sizeof(*report));
    return true;
}

/**
 * @brief 设置键盘 LED 状态
 */
void USB_Keyboard_SetLEDs(uint8_t leds)
{
    g_KeyboardLEDs = leds;
    
    LOG_D(TAG, "LEDs: %s%s%s",
          (leds & LED_NUM_LOCK) ? "Num " : "",
          (leds & LED_CAPS_LOCK) ? "Caps " : "",
          (leds & LED_SCROLL_LOCK) ? "Scroll" : "");
}

/* ==================== Mouse Functions ==================== */

/**
 * @brief 初始化鼠标
 */
void USB_Mouse_Init(void)
{
    memset(&g_MouseReport, 0, sizeof(USB_MouseReport_t));
}

/**
 * @brief 移动鼠标
 */
void USB_Mouse_Move(int8_t x, int8_t y, int8_t wheel)
{
    // 等待上一次传输完成（带超时，避免异常状态卡死）
    if (!USB_WaitEPInReady(2)) {
        return;
    }
    
    g_MouseReport.x = x;
    g_MouseReport.y = y;
    g_MouseReport.wheel = wheel;
    USB_Mouse_SendReport();
    
    // 发送后清除移动量，保留按键状态
    g_MouseReport.x = 0;
    g_MouseReport.y = 0;
    g_MouseReport.wheel = 0;
}

/**
 * @brief 点击鼠标按键
 */
void USB_Mouse_Click(uint8_t buttons)
{
    USB_Mouse_Press(buttons);
    mDelaymS(50);
    USB_Mouse_Release();
}

/**
 * @brief 按下鼠标按键
 */
void USB_Mouse_Press(uint8_t buttons)
{
    // 等待上一次传输完成（带超时，避免异常状态卡死）
    if (!USB_WaitEPInReady(2)) {
        return;
    }
    
    g_MouseReport.buttons = buttons;
    USB_Mouse_SendReport();
}

/**
 * @brief 释放所有鼠标按键
 */
void USB_Mouse_Release(void)
{
    // 等待上一次传输完成（带超时，避免异常状态卡死）
    if (!USB_WaitEPInReady(2)) {
        return;
    }
    
    g_MouseReport.buttons = 0;
    USB_Mouse_SendReport();
}

/**
 * @brief 发送鼠标报告
 */
void USB_Mouse_SendReport(void)
{
    memcpy(pEP2_IN_DataBuf, &g_MouseReport, sizeof(USB_MouseReport_t));
    DevEP2_IN_Deal(sizeof(USB_MouseReport_t));
}

bool USB_Mouse_TrySend(const USB_MouseReport_t *report)
{
    if (!report || !EP2_GetINSta()) return false;
    memcpy(pEP2_IN_DataBuf, report, sizeof(*report));
    DevEP2_IN_Deal(sizeof(*report));
    return true;
}

/* ==================== Consumer Control Functions ==================== */

/**
 * @brief 初始化多媒体控制
 */
void USB_Consumer_Init(void)
{
    memset(&g_ConsumerReport, 0, sizeof(USB_ConsumerReport_t));
}

/**
 * @brief 按下多媒体键
 */
void USB_Consumer_Press(uint16_t key)
{
    // 等待上一次传输完成（带超时，避免异常状态卡死）
    if (!USB_WaitEPInReady(3)) {
        return;
    }
    
    g_ConsumerReport.key = key;
    USB_Consumer_SendReport();
}

/**
 * @brief 释放多媒体键
 */
void USB_Consumer_Release(void)
{
    // 等待上一次传输完成（带超时，避免异常状态卡死）
    if (!USB_WaitEPInReady(3)) {
        return;
    }
    
    g_ConsumerReport.key = 0;
    USB_Consumer_SendReport();
}

/**
 * @brief 发送多媒体控制报告
 */
void USB_Consumer_SendReport(void)
{
    memcpy(pEP3_IN_DataBuf, &g_ConsumerReport, sizeof(USB_ConsumerReport_t));
    DevEP3_IN_Deal(sizeof(USB_ConsumerReport_t));
}

bool USB_Consumer_TrySend(const USB_ConsumerReport_t *report)
{
    if (!report || !EP3_GetINSta()) return false;
    memcpy(pEP3_IN_DataBuf, report, sizeof(*report));
    DevEP3_IN_Deal(sizeof(*report));
    return true;
}

/* ==================== Config Functions ==================== */

/**
 * @brief 初始化配置接口
 */
void USB_Config_Init(void)
{
    memset(&g_ConfigReport, 0, sizeof(USB_ConfigReport_t));
    USB_Config_ResetQueues();
}

/**
 * @brief 发送配置响应
 */
void USB_Config_SendResponse(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t copy_len = (len > 63u) ? 63u : len;
    uint8_t next = (uint8_t)((s_config_tx_head + 1u) % USB_CONFIG_QUEUE_DEPTH);
    if (next == s_config_tx_tail) return;

    s_config_tx_queue[s_config_tx_head].cmd = cmd;
    s_config_tx_queue[s_config_tx_head].len = copy_len;
    memset(s_config_tx_queue[s_config_tx_head].data, 0,
           sizeof(s_config_tx_queue[s_config_tx_head].data));
    if (data && copy_len != 0u) {
        memcpy(s_config_tx_queue[s_config_tx_head].data, data, copy_len);
    }
    s_config_tx_head = next;
}

/**
 * @brief 处理配置命令
 */
void USB_Config_ProcessCommand(const USB_ConfigReport_t *report, uint8_t report_len)
{
    uint8_t next = (uint8_t)((s_config_rx_head + 1u) % USB_CONFIG_QUEUE_DEPTH);
    if (!report || next == s_config_rx_tail) return;

    /* A short/long HID report cannot be parsed as a command frame. Preserve
     * only the fields that are physically present and let the main loop send
     * a normal parameter error; never read beyond the received report. */
    if (report_len != sizeof(USB_ConfigReport_t)) {
        usb_config_rx_item_t *item = &s_config_rx_queue[s_config_rx_head];
        memset(item, 0, sizeof(*item));
        if (report_len >= 1u) item->frame.cmd = report->cmd;
        if (report_len >= 2u) item->frame.sub = report->data[0];
        item->valid = 0u;
        s_config_rx_head = next;
        return;
    }

    usb_config_rx_item_t *item = &s_config_rx_queue[s_config_rx_head];
    memset(item, 0, sizeof(*item));
    item->frame.cmd = report->cmd;
    item->frame.sub = report->data[0];
    item->frame.len = report->data[1];
    memcpy(item->frame.data, &report->data[2], sizeof(item->frame.data));
    item->valid = item->frame.len <= sizeof(item->frame.data);
    s_config_rx_head = next;
}

void USB_Config_ProcessPending(void)
{
    if (s_config_rx_head == s_config_rx_tail) return;

    usb_config_rx_item_t item;
    item = s_config_rx_queue[s_config_rx_tail];
    s_config_rx_tail = (uint8_t)((s_config_rx_tail + 1u) % USB_CONFIG_QUEUE_DEPTH);

    if (!item.valid) {
        const uint8_t response = KBD_RESP_ERR_PARAM;
        KBD_Command_SendResponse(item.frame.cmd, item.frame.sub, &response, 1u);
        return;
    }
    KBD_Command_Process(&item.frame);
}

void USB_Config_ProcessPendingTx(void)
{
    if (s_config_tx_head == s_config_tx_tail || !EP4_GetINSta()) return;

    usb_config_tx_item_t item;
    item = s_config_tx_queue[s_config_tx_tail];
    s_config_tx_tail = (uint8_t)((s_config_tx_tail + 1u) % USB_CONFIG_QUEUE_DEPTH);

    g_ConfigReport.cmd = item.cmd;
    memset(g_ConfigReport.data, 0, sizeof(g_ConfigReport.data));
    memcpy(g_ConfigReport.data, item.data, item.len);
    memcpy(pEP4_IN_DataBuf, &g_ConfigReport, sizeof(USB_ConfigReport_t));
    DevEP4_IN_Deal(sizeof(USB_ConfigReport_t));
}

void USB_Config_ResetQueues(void)
{
    s_config_rx_head = 0u;
    s_config_rx_tail = 0u;
    s_config_tx_head = 0u;
    s_config_tx_tail = 0u;
}

/* ==================== USB Device Callbacks ==================== */

/**
 * @brief EP1 IN 回调 (键盘发送完成)
 */
void USB_DevEP1_IN_Callback(void)
{
    // 键盘数据发送完成
}

/**
 * @brief EP2 IN 回调 (鼠标发送完成)
 */
void USB_DevEP2_IN_Callback(void)
{
    // 鼠标数据发送完成
}

/**
 * @brief EP3 IN 回调 (多媒体发送完成)
 */
void USB_DevEP3_IN_Callback(void)
{
    // 多媒体数据发送完成
}

/**
 * @brief EP4 IN 回调 (配置发送完成)
 */
void USB_DevEP4_IN_Callback(void)
{
    // 配置数据发送完成
}

/**
 * @brief EP4 OUT 回调 (接收到配置数据)
 */
void DevEP4_OUT_Deal(uint8_t len)
{
    if (len > 0u && len <= sizeof(USB_ConfigReport_t)) {
        const USB_ConfigReport_t *report = (const USB_ConfigReport_t *)pEP4_OUT_DataBuf;
        USB_Config_ProcessCommand(report, len);
    }
}

/* 其他端点 OUT 处理（空实现） */
void DevEP1_OUT_Deal(uint8_t len) { }
void DevEP2_OUT_Deal(uint8_t len) { }
void DevEP3_OUT_Deal(uint8_t len) { }
