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

#if defined(KBD_RECEIVER_USB_DIAGNOSTIC) && KBD_RECEIVER_USB_DIAGNOSTIC
    /* Keep the vendor HID available while isolating RF/TMOS startup faults. */
    while (1) {
        Receiver_Command_ProcessDeferred();
    }
#else
    HAL_TimeInit();
    Receiver_Radio_Init();
    while (1) {
        TMOS_SystemProcess();
        Receiver_Radio_Process();
        Receiver_Command_ProcessDeferred();
    }
#endif
}
