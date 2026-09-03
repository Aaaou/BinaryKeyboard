#ifndef RECEIVER_RADIO_H
#define RECEIVER_RADIO_H
#include <stdint.h>
#include <stdbool.h>
#include "kbd_radio_2g4.h"
#include "kbd_types.h"
typedef void (*receiver_radio_mgmt_response_cb_t)(uint8_t transaction, const uint8_t *frame, uint8_t len, bool error);
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
bool Receiver_Radio_HasRemoteCapabilities(void);
void Receiver_Radio_GetRemoteCapabilities(uint8_t out[8]);
uint8_t Receiver_Radio_GetDeviceId(void);
void Receiver_Radio_GetLocalId(uint8_t out[6]);
void Receiver_Radio_GetPeerId(uint8_t out[6]);
uint32_t Receiver_Radio_GetPairFingerprint(void);
uint32_t Receiver_Radio_GetPairGeneration(void);
uint32_t Receiver_Radio_GetLastValidAge(void);
/* Ages, in RTC 32 kHz ticks, of the most recent disconnect diagnostics. */
uint32_t Receiver_Radio_GetLastLinkTimeoutAge(void);
uint32_t Receiver_Radio_GetLastReleaseQueuedAge(void);
uint32_t Receiver_Radio_GetLastReleaseSentAge(void);
uint16_t Receiver_Radio_GetReleaseBusyCount(void);
uint16_t Receiver_Radio_GetPollRate(void);
int Receiver_Radio_SetPollRate(uint16_t rate);
bool Receiver_Radio_TakeControlResult(int *result);
int Receiver_Radio_SendManagement(const kbd_cmd_frame_t *command, uint8_t transaction);
bool Receiver_Radio_ManagementBusy(void);
/* [0] flags, [1] transaction, [2] command, [3] tx fragment,
 * [4] tx fragments, [5] rx fragment, [6] rx fragments. */
void Receiver_Radio_GetManagementDiagnostics(uint8_t out[7]);
/* [0..1] valid RF frames, [2..3] keyboard RF reports,
 * [4..5] keyboard USB submissions, [6..7] keyboard USB busy attempts,
 * [8] RX quarantine flag. Counters saturate at 0xffff. */
void Receiver_Radio_GetHidDiagnostics(uint8_t out[9]);
void Receiver_Radio_SetManagementResponseCallback(receiver_radio_mgmt_response_cb_t callback);
void Receiver_Radio_ResetManagement(void);
#endif
