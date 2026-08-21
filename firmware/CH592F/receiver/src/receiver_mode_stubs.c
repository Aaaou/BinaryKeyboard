#include "kbd_mode.h"
#include "debug.h"
#include "usb_device.h"

static bool s_sleep;
bool KBD_Mode_IsInSleep(void) { return s_sleep; }
kbd_work_mode_t KBD_Mode_Get(void) { return KBD_WORK_MODE_2G4; }
void KBD_Mode_RequestWake(void) { s_sleep=false; }
void KBD_Mode_RecordActivity(void) { s_sleep=false; }
void KBD_Mode_SuppressWakeForFn(uint8_t fn_id) { (void)fn_id; }
bool KBD_Mode_USB_IsPlugged(void) { return true; }
int KBD_Mode_USB_Wakeup(void) { USB_Device_Wakeup(); return 0; }

void Log_Output(const char *level, const char *tag, const char *fmt, ...) { (void)level;(void)tag;(void)fmt; }
