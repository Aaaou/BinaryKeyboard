#include "kbd_radio_2g4.h"
#include "kbd_mode_config.h"
bool KBD_Radio2G4_IsEnabled(void) { return KBD_RADIO_2G4_ENABLED != 0; }
int KBD_Radio2G4_Init(void) { return KBD_RADIO_2G4_ENABLED ? 0 : -1; }
void KBD_Radio2G4_Stop(void) {}
int KBD_Radio2G4_StartPairing(void) { return KBD_RADIO_2G4_ENABLED ? 0 : -1; }
int KBD_Radio2G4_CancelPairing(void) { return KBD_RADIO_2G4_ENABLED ? 0 : -1; }
int KBD_Radio2G4_ClearPairing(bool force) { (void)force; return KBD_RADIO_2G4_ENABLED ? 0 : -1; }
kbd_radio_pair_state_t KBD_Radio2G4_GetPairState(void) { return KBD_RADIO_2G4_ENABLED ? KBD_RADIO_PAIR_UNBOUND : KBD_RADIO_PAIR_UNSUPPORTED; }
int KBD_Radio2G4_SendKeyboardReport(uint8_t modifier, const uint8_t *keys, uint8_t count) { (void)modifier; (void)keys; (void)count; return -1; }
void KBD_Radio2G4_Process(void) {}
