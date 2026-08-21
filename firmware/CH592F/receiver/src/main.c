#include "CH59x_common.h"
#include "usb_device.h"
#include "receiver_radio.h"
#include "receiver_log.h"
#include "RF.h"

void Receiver_Command_ProcessDeferred(void);

int main(void)
{
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    Receiver_Log_Init();
    Receiver_Log_Event(RX_LOG_BOOT, KBD_RECEIVER_STARTUP_STAGE, 0u);
    USB_Device_Init();

    /* Bring USB up before the 32 kHz/TMOS/RF initialization. A receiver has
     * no useful radio role without a host, and USB enumeration must remain
     * available even if board-specific low-speed clock startup fails. */
    while (g_USB_DeviceState != USB_STATE_CONFIGURED) {
        __asm__ volatile ("nop");
    }
    Receiver_Log_Event(RX_LOG_USB_CONFIGURED, KBD_RECEIVER_STARTUP_STAGE, 0u);

#if KBD_RECEIVER_STARTUP_STAGE >= 1
    Receiver_Log_Event(RX_LOG_TIME_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    HAL_TimeInit();
    Receiver_Log_Event(RX_LOG_TIME_INIT_OK, KBD_RECEIVER_STARTUP_STAGE, 0u);
#endif
#if KBD_RECEIVER_STARTUP_STAGE == 2
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Radio_RfLibraryInit();
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_OK, KBD_RECEIVER_STARTUP_STAGE, 0u);
#endif
#if KBD_RECEIVER_STARTUP_STAGE >= 3
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Radio_RfLibraryInit();
    Receiver_Log_Event(RX_LOG_RF_LIB_INIT_OK, KBD_RECEIVER_STARTUP_STAGE, 0u);
    Receiver_Log_Event(RX_LOG_HOST_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
    int radio_result = Receiver_Radio_Init();
    Receiver_Log_Event(radio_result == 0 ? RX_LOG_HOST_INIT_OK : RX_LOG_HOST_INIT_FAIL,
                       KBD_RECEIVER_STARTUP_STAGE, (uint8_t)radio_result);
#endif
    while (1) {
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
