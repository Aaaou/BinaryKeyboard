#ifndef RECEIVER_RADIO_H
#define RECEIVER_RADIO_H
#include <stdint.h>
#include "kbd_radio_2g4.h"
int Receiver_Radio_Init(void);
/* Host startup diagnostic: 0=not attempted, 1=calling, 2=started, 3=failed. */
uint8_t Receiver_Radio_GetHostStartupState(void);
uint8_t Receiver_Radio_GetHostStartupResult(void);
void Receiver_Radio_RfLibraryInit(void);
void Receiver_Radio_TmosInit(void);
void Receiver_Radio_Process(void);
int Receiver_Radio_StartPairing(void);
int Receiver_Radio_CancelPairing(void);
int Receiver_Radio_ClearPairing(void);
kbd_radio_pair_state_t Receiver_Radio_GetState(void);
uint8_t Receiver_Radio_GetPeerDeviceId(void);
bool Receiver_Radio_HasPeer(void);
uint8_t Receiver_Radio_GetDeviceId(void);
void Receiver_Radio_GetLocalId(uint8_t out[6]);
void Receiver_Radio_GetPeerId(uint8_t out[6]);
uint32_t Receiver_Radio_GetPairFingerprint(void);
uint32_t Receiver_Radio_GetPairGeneration(void);
uint32_t Receiver_Radio_GetLastValidAge(void);
uint16_t Receiver_Radio_GetPollRate(void);
int Receiver_Radio_SetPollRate(uint16_t rate);
bool Receiver_Radio_TakeControlResult(int *result);
#endif
