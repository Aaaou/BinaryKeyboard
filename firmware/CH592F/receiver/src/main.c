#include "CH59x_common.h"
#include "usb_device.h"
#include "receiver_radio.h"
#include "receiver_log.h"
#include "RF.h"

void Receiver_Command_ProcessDeferred(void);

int main(void)
{
    bool usb_configured_logged = false;
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    Receiver_Log_Init();
    Receiver_Log_Event(RX_LOG_BOOT, KBD_RECEIVER_STARTUP_STAGE, 0u);
    USB_Device_Init();

    /* USB enumeration is asynchronous. RF must start even when no USB host is
     * present; the management/HID endpoints become usable once configured. */
    if (g_USB_DeviceState == USB_STATE_CONFIGURED) {
        Receiver_Log_Event(RX_LOG_USB_CONFIGURED, KBD_RECEIVER_STARTUP_STAGE, 0u);
        usb_configured_logged = true;
    }

#if KBD_RECEIVER_STARTUP_STAGE >= 1
    /* WCH RF Host requires TMOS task/message storage before HAL_TimeInit and
     * RFBound_StartHost.  The RF-only keyboard gets this from its BLE task
     * scheduler; the standalone USB receiver must provide it explicitly. */
    Receiver_Radio_TmosInit();
    Receiver_Log_Event(RX_LOG_TIME_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    HAL_TimeInit();
    Receiver_Log_Event(RX_LOG_TIME_INIT_OK, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Log_SetCompletedStage(1u);
#endif
#if KBD_RECEIVER_STARTUP_STAGE == 2
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Radio_RfLibraryInit();
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_OK, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Log_SetCompletedStage(2u);
#endif
#if KBD_RECEIVER_STARTUP_STAGE >= 3
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Radio_RfLibraryInit();
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_OK, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Log_SetCompletedStage(2u);
#if !defined(KBD_RECEIVER_MANUAL_HOST_DIAGNOSTIC)
    int radio_result = Receiver_Radio_Init();
    /* Host startup is queued for the main loop, matching the WCH dongle
     * reference and keeping USB management alive during RF setup. */
    if (radio_result != 0) {
        Receiver_Log_Event(RX_LOG_HOST_INIT_FAIL, KBD_RECEIVER_STARTUP_STAGE,
                           (uint8_t)radio_result);
    }
#endif
#endif
    while (1) {
        if (!usb_configured_logged && g_USB_DeviceState == USB_STATE_CONFIGURED) {
            Receiver_Log_Event(RX_LOG_USB_CONFIGURED, KBD_RECEIVER_STARTUP_STAGE, 0u);
            usb_configured_logged = true;
        }
#if KBD_RECEIVER_STARTUP_STAGE >= 2
        TMOS_SystemProcess();
#endif
#if KBD_RECEIVER_STARTUP_STAGE >= 3
        Receiver_Radio_Process();
#endif
        Receiver_Command_ProcessDeferred();
        Receiver_Log_Flush();
    }
}
