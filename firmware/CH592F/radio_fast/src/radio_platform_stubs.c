#include "CH59x_common.h"

volatile uint32_t RTCTigFlag;

void HAL_Init(void) {}
void HAL_SleepInit(void) {}
void CH59x_LowPowerSetEnabled(uint8_t enabled) { (void)enabled; }
