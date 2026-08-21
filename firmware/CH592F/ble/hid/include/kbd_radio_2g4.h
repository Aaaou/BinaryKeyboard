#ifndef __KBD_RADIO_2G4_H
#define __KBD_RADIO_2G4_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { KBD_RADIO_PAIR_UNBOUND = 0, KBD_RADIO_PAIRING = 1, KBD_RADIO_PAIR_BOUND = 2, KBD_RADIO_PAIR_CONNECTED = 3, KBD_RADIO_PAIR_INCONSISTENT = 4, KBD_RADIO_PAIR_UNSUPPORTED = 5 } kbd_radio_pair_state_t;
bool KBD_Radio2G4_IsEnabled(void);
void KBD_Radio2G4_TmosInit(void);
int KBD_Radio2G4_Init(void);
void KBD_Radio2G4_Stop(void);
int KBD_Radio2G4_StartPairing(void);
int KBD_Radio2G4_CancelPairing(void);
int KBD_Radio2G4_ClearPairing(bool force);
kbd_radio_pair_state_t KBD_Radio2G4_GetPairState(void);
uint8_t KBD_Radio2G4_GetDeviceId(void);
bool KBD_Radio2G4_HasPeer(void);
void KBD_Radio2G4_GetLocalId(uint8_t out[6]);
void KBD_Radio2G4_GetPeerId(uint8_t out[6]);
uint32_t KBD_Radio2G4_GetPairFingerprint(void);
uint32_t KBD_Radio2G4_GetSession(void);
int KBD_Radio2G4_SendKeyboardReport(uint8_t modifier, const uint8_t *keys, uint8_t count);
int KBD_Radio2G4_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
int KBD_Radio2G4_SendConsumerReport(uint16_t key);
void KBD_Radio2G4_Process(void);
#ifdef __cplusplus
}
#endif
#endif
