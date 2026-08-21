#include "CH59x_common.h"
#include "usb_device.h"
#include "receiver_radio.h"
#include "RF.h"

void Receiver_Command_ProcessDeferred(void);

int main(void)
{
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    USB_Device_Init();

    /* Bring USB up before the 32 kHz/TMOS/RF initialization. A receiver has
     * no useful radio role without a host, and USB enumeration must remain
     * available even if board-specific low-speed clock startup fails. */
    while (g_USB_DeviceState != USB_STATE_CONFIGURED) {
        __asm__ volatile ("nop");
    }

#if KBD_RECEIVER_STARTUP_STAGE >= 1
    HAL_TimeInit();
#endif
#if KBD_RECEIVER_STARTUP_STAGE == 2
    Receiver_Radio_RfLibraryInit();
#endif
#if KBD_RECEIVER_STARTUP_STAGE >= 3
    Receiver_Radio_Init();
#endif
    while (1) {
#if KBD_RECEIVER_STARTUP_STAGE >= 2
        TMOS_SystemProcess();
#endif
#if KBD_RECEIVER_STARTUP_STAGE >= 3
        Receiver_Radio_Process();
#endif
        Receiver_Command_ProcessDeferred();
    }
}
