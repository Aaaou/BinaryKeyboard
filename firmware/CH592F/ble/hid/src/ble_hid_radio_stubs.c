#include "ble_hid.h"

int BLE_HID_Init(ble_hid_callbacks_t *callbacks) { (void)callbacks; return -1; }
void BLE_HID_Process(void) {}
void BLE_HID_SetAutoResumeAdvertising(bool enable) { (void)enable; }
bool BLE_HID_IsConnected(void) { return false; }
int BLE_HID_StartAdvertising(void) { return -1; }
int BLE_HID_StopAdvertising(void) { return -1; }
int BLE_HID_Disconnect(void) { return -1; }
int BLE_HID_ClearBonds(void) { return -1; }
uint8_t BLE_HID_GetBondCount(void) { return 0; }
uint8_t BLE_HID_GetKeyboardLEDs(void) { return 0; }
bool BLE_HID_IsKeyboardReady(void) { return false; }
int BLE_HID_SendKeyboardReport(uint8_t modifier, uint8_t *keys, uint8_t count) { (void)modifier; (void)keys; (void)count; return -1; }
int BLE_HID_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel) { (void)buttons; (void)x; (void)y; (void)wheel; return -1; }
int BLE_HID_SendConsumerReport(uint16_t key) { (void)key; return -1; }
