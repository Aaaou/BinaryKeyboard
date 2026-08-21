#ifndef RECEIVER_RADIO_H
#define RECEIVER_RADIO_H
#include <stdint.h>
#include "kbd_radio_2g4.h"
void Receiver_Radio_Init(void);
void Receiver_Radio_RfLibraryInit(void);
void Receiver_Radio_Process(void);
int Receiver_Radio_StartPairing(void);
int Receiver_Radio_CancelPairing(void);
int Receiver_Radio_ClearPairing(void);
kbd_radio_pair_state_t Receiver_Radio_GetState(void);
uint16_t Receiver_Radio_GetPollRate(void);
int Receiver_Radio_SetPollRate(uint16_t rate);
bool Receiver_Radio_TakeControlResult(int *result);
#endif
