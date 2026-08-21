#ifndef __KBD_RADIO_2G4_H
#define __KBD_RADIO_2G4_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { KBD_RADIO_PAIR_UNBOUND = 0, KBD_RADIO_PAIRING = 1, KBD_RADIO_PAIR_BOUND = 2, KBD_RADIO_PAIR_CONNECTED = 3, KBD_RADIO_PAIR_INCONSISTENT = 4, KBD_RADIO_PAIR_UNSUPPORTED = 5 } kbd_radio_pair_state_t;
bool KBD_Radio2G4_IsEnabled(void);
int KBD_Radio2G4_Init(void);
void KBD_Radio2G4_Stop(void);
int KBD_Radio2G4_StartPairing(void);
int KBD_Radio2G4_CancelPairing(void);
int KBD_Radio2G4_ClearPairing(bool force);
kbd_radio_pair_state_t KBD_Radio2G4_GetPairState(void);
int KBD_Radio2G4_SendKeyboardReport(uint8_t modifier, const uint8_t *keys, uint8_t count);
void KBD_Radio2G4_Process(void);
#ifdef __cplusplus
}
#endif
#endif
