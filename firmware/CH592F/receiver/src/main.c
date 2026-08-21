#include "CH59x_common.h"
#include "usb_device.h"
#include "receiver_radio.h"
#include "RF.h"

void Receiver_Command_ProcessDeferred(void);

int main(void)
{
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    PWR_DCDCCfg(ENABLE);
    HAL_TimeInit();
    USB_Device_Init();
    Receiver_Radio_Init();
    WWDG_SetCounter(0);
    WWDG_ResetCfg(ENABLE);
    while (1) {
        WWDG_SetCounter(0);
        TMOS_SystemProcess();
        Receiver_Radio_Process();
        Receiver_Command_ProcessDeferred();
    }
}
