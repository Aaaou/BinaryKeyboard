/**
 * @file    kbd_macro.c
 * @brief   MeowKeyboard 宏回放引擎实现
 * @author  MeowKJ
 *
 * @details
 * 基于 TMOS 定时器的非阻塞宏回放引擎。
 * 支持 4 种触发模式：单次、按住-立即停、按住-跑完停、切换循环。
 * 每步按需从 Flash 读取 2 字节动作，静态 RAM 开销 ~50 字节。
 */

#include "kbd_macro.h"
#include "kbd_storage.h"
#include "kbd_mode.h"
#include "kbd_log.h"
#include "CH59x_common.h"
#include "ble_config.h"
#include "debug.h"

#define TAG "MACRO"

/*============================================================================*/
/* TMOS 事件                                                                   */
/*============================================================================*/

#define MACRO_STEP_EVT 0x0001
#define MACRO_2G4_REPORT_GAP_MS 10u
#define MACRO_2G4_RELEASE_GAP_MS 10u
#define MACRO_2G4_RELEASE_REPEATS 3u
#define MACRO_REPORT_RETRY_MS 4u

/*============================================================================*/
/* 状态定义                                                                    */
/*============================================================================*/

enum {
    MACRO_IDLE = 0,
    MACRO_RUNNING = 1,
    MACRO_RELEASING = 2,
};

/*============================================================================*/
/* 私有变量                                                                    */
/*============================================================================*/

static tmosTaskID s_task_id;
static uint8_t    s_state = MACRO_IDLE;

/** 当前回放的宏 */
static uint8_t              s_slot;
static kbd_macro_trigger_t  s_trigger;
static kbd_macro_header_t   s_header;
static uint16_t             s_action_idx;

/** 触发键状态 */
static bool s_key_released;
static bool s_cancel_req;

/** 宏独立的 HID 按键状态 */
static uint8_t s_mod_mask;
static uint8_t s_keys[6];
static uint8_t s_key_count;
static uint8_t s_mouse_buttons;
static bool s_consumer_release_pending;
static uint8_t s_release_repeats_remaining;
static uint16_t s_retry_logged_action;
#if KBD_RADIO_2G4_ENABLED
static volatile uint16_t s_poll_delay_ms;
static volatile bool s_poll_pending;
#endif

/*============================================================================*/
/* 私有函数声明                                                                */
/*============================================================================*/

#if !KBD_RADIO_2G4_ENABLED
static uint16_t KBD_Macro_ProcessEvent(uint8_t task_id, uint16_t events);
#endif
static void MacroStepActions(void);
static bool ShouldLoop(void);
static void MacroAddKey(uint8_t keycode);
static void MacroRemoveKey(uint8_t keycode);
static int MacroSendKeyboardReport(void);
static void MacroReleaseAll(void);
static void MacroBeginReliableRelease(void);
static void MacroStepReliableRelease(void);
static void MacroScheduleStep(uint16_t delay_ms);
static bool MacroPace2G4Report(void);

/*============================================================================*/
/* 公共函数                                                                    */
/*============================================================================*/

void KBD_Macro_Init(void)
{
#if KBD_RADIO_2G4_ENABLED
    /* RFBound owns the TMOS timer used by the standalone 2.4G image. Use the
     * keyboard's independent 1 ms timer instead, as the RGB scheduler does. */
    s_task_id = TASK_NO_TASK;
    s_poll_delay_ms = 0u;
    s_poll_pending = false;
#else
    s_task_id = TMOS_ProcessEventRegister(KBD_Macro_ProcessEvent);
#endif
    s_state = MACRO_IDLE;
    s_mouse_buttons = 0;
    s_consumer_release_pending = false;
    s_release_repeats_remaining = 0;
    s_retry_logged_action = 0xFFFFu;
    LOG_I(TAG, "Macro engine initialized");
}

int KBD_Macro_Execute(uint8_t slot, kbd_macro_trigger_t trigger)
{
    /* 若已在运行，先取消 */
    if (s_state != MACRO_IDLE) {
        KBD_Macro_Cancel();
    }

    /* 读取宏头部 */
    int ret = Kbd_Macro_GetInfo(slot, &s_header);
    if (ret != 0 || s_header.valid != KBD_MACRO_VALID_MAGIC) {
        LOG_W(TAG, "Slot %d invalid or read fail", slot);
        return -2;
    }

    if (s_header.action_count == 0) {
        LOG_D(TAG, "Slot %d empty", slot);
        return 0;
    }

    /* 初始化回放状态 */
    s_slot = slot;
    s_trigger = trigger;
    s_action_idx = 0;
    s_key_released = false;
    s_cancel_req = false;
    s_mod_mask = 0;
    s_key_count = 0;
    s_mouse_buttons = 0;
    s_consumer_release_pending = false;
    s_release_repeats_remaining = 0;
    for (uint8_t i = 0; i < 6; i++) {
        s_keys[i] = 0;
    }

    s_state = MACRO_RUNNING;
    KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_START, slot,
                       (uint8_t)s_header.action_count);
    LOG_I(TAG, "Execute slot %d, trigger %d, %d actions",
          slot, trigger, s_header.action_count);

    /* 立即开始第一步 */
#if KBD_RADIO_2G4_ENABLED
    MacroScheduleStep(0u);
#else
    tmos_set_event(s_task_id, MACRO_STEP_EVT);
#endif
    return 0;
}

void KBD_Macro_Poll(void)
{
#if KBD_RADIO_2G4_ENABLED
    if (!s_poll_pending || s_poll_delay_ms != 0u) return;
    s_poll_pending = false;
    if (s_state == MACRO_RELEASING)
        MacroStepReliableRelease();
    else
        MacroStepActions();
#endif
}

void KBD_Macro_TimerTick1ms(void)
{
#if KBD_RADIO_2G4_ENABLED
    if (s_poll_pending && s_poll_delay_ms != 0u) s_poll_delay_ms--;
#endif
}

void KBD_Macro_OnKeyRelease(void)
{
    if (s_state != MACRO_RUNNING)
        return;

    s_key_released = true;

    if (s_trigger == KBD_MACRO_TRIG_HOLD_ABORT) {
        /* 按住-立即停: 松开立即中断 */
        KBD_Macro_Cancel();
    }
    /* HOLD_FINISH / TOGGLE: 由 ShouldLoop() 在当轮结束时判定 */
}

void KBD_Macro_Cancel(void)
{
    if (s_state == MACRO_IDLE)
        return;

#if KBD_RADIO_2G4_ENABLED
    s_poll_pending = false;
    s_poll_delay_ms = 0u;
#else
    tmos_stop_task(s_task_id, MACRO_STEP_EVT);
#endif
    MacroReleaseAll();
    s_state = MACRO_IDLE;
    LOG_D(TAG, "Cancelled");
}

bool KBD_Macro_IsRunning(void)
{
    return s_state != MACRO_IDLE;
}

/*============================================================================*/
/* TMOS 事件处理                                                               */
/*============================================================================*/

#if !KBD_RADIO_2G4_ENABLED
static uint16_t KBD_Macro_ProcessEvent(uint8_t task_id, uint16_t events)
{
    if (events & MACRO_STEP_EVT) {
        if (s_state == MACRO_RELEASING)
            MacroStepReliableRelease();
        else
            MacroStepActions();
        return (events ^ MACRO_STEP_EVT);
    }
    return 0;
}
#endif

/*============================================================================*/
/* 核心步进逻辑                                                                */
/*============================================================================*/

static void MacroStepActions(void)
{
    if (s_state != MACRO_RUNNING)
        return;

    if (s_consumer_release_pending) {
        if (KBD_Mode_SendConsumerReport(0) != 0) {
            MacroScheduleStep(MACRO_REPORT_RETRY_MS);
            return;
        }
        s_consumer_release_pending = false;
        if (MacroPace2G4Report()) return;
    }

    while (s_action_idx < s_header.action_count) {
        kbd_macro_action_t action;
        uint16_t offset = s_action_idx * sizeof(kbd_macro_action_t);

        int ret = Kbd_Macro_Read(s_slot, offset, (uint8_t *)&action, sizeof(action));
        if (ret < (int)sizeof(action)) {
            LOG_W(TAG, "Flash read fail at idx %d", s_action_idx);
            KBD_Macro_Cancel();
            return;
        }

        s_action_idx++;

        switch (action.type) {
        case KBD_MACRO_KEY_DOWN:
            MacroAddKey(action.param);
            if (MacroSendKeyboardReport() != 0) {
                s_action_idx--;
                if (s_retry_logged_action != s_action_idx) {
                    s_retry_logged_action = s_action_idx;
                    KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_RETRY, action.type,
                                       action.param);
                }
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            s_retry_logged_action = 0xFFFFu;
            KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_ACTION, action.type,
                               action.param);
            if (MacroPace2G4Report()) return;
            break;

        case KBD_MACRO_KEY_UP:
            MacroRemoveKey(action.param);
            if (MacroSendKeyboardReport() != 0) {
                s_action_idx--;
                if (s_retry_logged_action != s_action_idx) {
                    s_retry_logged_action = s_action_idx;
                    KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_RETRY, action.type,
                                       action.param);
                }
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            s_retry_logged_action = 0xFFFFu;
            KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_ACTION, action.type,
                               action.param);
            if (MacroPace2G4Report()) return;
            break;

        case KBD_MACRO_MOD_DOWN:
            s_mod_mask |= action.param;
            if (MacroSendKeyboardReport() != 0) {
                s_action_idx--;
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            if (MacroPace2G4Report()) return;
            break;

        case KBD_MACRO_MOD_UP:
            s_mod_mask &= ~action.param;
            if (MacroSendKeyboardReport() != 0) {
                s_action_idx--;
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            if (MacroPace2G4Report()) return;
            break;

        case KBD_MACRO_DELAY: {
            uint32_t delay_ms = (uint32_t)action.param * 10;
            if (delay_ms == 0) delay_ms = 10;
            MacroScheduleStep((uint16_t)delay_ms);
            return; /* 等待定时器回调继续 */
        }

        case KBD_MACRO_CONSUMER:
            if (KBD_Mode_SendConsumerReport((uint16_t)action.param) != 0) {
                s_action_idx--;
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            s_consumer_release_pending = true;
            MacroScheduleStep(20u);
            return;

        case KBD_MACRO_MOUSE_DOWN:
            s_mouse_buttons |= action.param;
            if (KBD_Mode_SendMouseReport(s_mouse_buttons, 0, 0, 0) != 0) {
                s_action_idx--;
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            if (MacroPace2G4Report()) return;
            break;

        case KBD_MACRO_MOUSE_UP:
            s_mouse_buttons &= (uint8_t)~action.param;
            if (KBD_Mode_SendMouseReport(s_mouse_buttons, 0, 0, 0) != 0) {
                s_action_idx--;
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            if (MacroPace2G4Report()) return;
            break;

        case KBD_MACRO_WHEEL: {
            int8_t wheel = 0;
            if (action.param == KBD_WHEEL_UP)
                wheel = 1;
            else if (action.param == KBD_WHEEL_DOWN)
                wheel = -1;
            if (KBD_Mode_SendMouseReport(s_mouse_buttons, 0, 0, wheel) != 0) {
                s_action_idx--;
                MacroScheduleStep(MACRO_REPORT_RETRY_MS);
                return;
            }
            if (MacroPace2G4Report()) return;
            break;
        }

        case KBD_MACRO_END:
            goto macro_round_done;

        default:
            /* 跳过未知动作 */
            break;
        }
    }

macro_round_done:
    if (ShouldLoop()) {
        /* 重新开始 */
        s_action_idx = 0;
        tmos_set_event(s_task_id, MACRO_STEP_EVT);
        LOG_D(TAG, "Loop restart");
    } else {
        MacroBeginReliableRelease();
        LOG_D(TAG, "Slot %d done", s_slot);
    }
}

/*============================================================================*/
/* 循环判定                                                                    */
/*============================================================================*/

static bool ShouldLoop(void)
{
    switch (s_trigger) {
    case KBD_MACRO_TRIG_ONCE:
        return false;
    case KBD_MACRO_TRIG_HOLD_ABORT:
        /* 若还没松开则继续循环（松开时已经 Cancel 了） */
        return !s_key_released;
    case KBD_MACRO_TRIG_HOLD_FINISH:
        return !s_key_released;
    case KBD_MACRO_TRIG_TOGGLE:
        return !s_cancel_req;
    }
    return false;
}

/*============================================================================*/
/* HID 按键状态管理                                                            */
/*============================================================================*/

static void MacroAddKey(uint8_t keycode)
{
    /* 检查是否已在列表中 */
    for (uint8_t i = 0; i < s_key_count; i++) {
        if (s_keys[i] == keycode) return;
    }
    if (s_key_count < 6) {
        s_keys[s_key_count++] = keycode;
    }
}

static void MacroRemoveKey(uint8_t keycode)
{
    for (uint8_t i = 0; i < s_key_count; i++) {
        if (s_keys[i] == keycode) {
            /* 用最后一个填补 */
            s_keys[i] = s_keys[s_key_count - 1];
            s_keys[s_key_count - 1] = 0;
            s_key_count--;
            return;
        }
    }
}

static int MacroSendKeyboardReport(void)
{
    return KBD_Mode_SendKeyboardReport(s_mod_mask, s_keys, s_key_count);
}

static void MacroScheduleStep(uint16_t delay_ms)
{
#if KBD_RADIO_2G4_ENABLED
    s_poll_delay_ms = delay_ms;
    s_poll_pending = true;
#else
    tmos_start_task(s_task_id, MACRO_STEP_EVT,
                    MS1_TO_SYSTEM_TIME(delay_ms));
#endif
}

static bool MacroPace2G4Report(void)
{
    if (KBD_Mode_Get() != KBD_WORK_MODE_2G4) return false;
    /* Four milliseconds was too short for the RFBound scheduling path and
     * allowed a press to arrive without its following release. Match the
     * firmware's requested 10 ms BLE connection interval; reliability comes
     * from the repeated final release below, not from inflating every action. */
    MacroScheduleStep(MACRO_2G4_REPORT_GAP_MS);
    return true;
}

static void MacroBeginReliableRelease(void)
{
    s_mod_mask = 0;
    s_key_count = 0;
    s_mouse_buttons = 0;
    s_consumer_release_pending = false;
    memset(s_keys, 0, sizeof(s_keys));

    /* USB/BLE must observe one accepted all-zero keyboard report before the
     * macro becomes idle. RF DMA acceptance is not peer acknowledgement, so
     * 2.4G repeats that accepted snapshot across several RF periods. */
    s_release_repeats_remaining =
        KBD_Mode_Get() == KBD_WORK_MODE_2G4
            ? MACRO_2G4_RELEASE_REPEATS
            : 1u;
    s_state = MACRO_RELEASING;
    MacroStepReliableRelease();
}

static void MacroStepReliableRelease(void)
{
    if (s_state != MACRO_RELEASING) return;

    if (s_release_repeats_remaining != 0u) {
        if (MacroSendKeyboardReport() != 0) {
            MacroScheduleStep(MACRO_REPORT_RETRY_MS);
            return;
        }
        s_release_repeats_remaining--;
        if (s_release_repeats_remaining ==
            (MACRO_2G4_RELEASE_REPEATS - 1u)) {
            KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_RELEASE, 1u, 0u);
        }
        if (s_release_repeats_remaining != 0u) {
            MacroScheduleStep(MACRO_2G4_RELEASE_GAP_MS);
            return;
        }
    }

    /* Keyboard release is the safety-critical report. Mouse and consumer
     * releases are still emitted after it without delaying the next macro. */
    KBD_Mode_SendMouseReport(0, 0, 0, 0);
    KBD_Mode_SendConsumerReport(0);
    s_state = MACRO_IDLE;
    KBD_Log_MacroEvent(KBD_LOG_SYS_MACRO_DONE, s_slot, 0u);
}

static void MacroReleaseAll(void)
{
    /* 释放所有键盘键 */
    s_mod_mask = 0;
    s_key_count = 0;
    s_mouse_buttons = 0;
    s_consumer_release_pending = false;
    for (uint8_t i = 0; i < 6; i++) {
        s_keys[i] = 0;
    }
    KBD_Mode_SendKeyboardReport(0, s_keys, 0);

    /* 释放鼠标 */
    KBD_Mode_SendMouseReport(0, 0, 0, 0);

    /* 释放多媒体键 */
    KBD_Mode_SendConsumerReport(0);
}
