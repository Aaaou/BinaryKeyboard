#ifndef RECEIVER_LOG_H
#define RECEIVER_LOG_H

#include <stdint.h>

/* Receiver lifecycle event codes carried in KBD_LOG_SYSTEM_EVENT. */
#define RX_LOG_BOOT                 0x80u
#define RX_LOG_USB_CONFIGURED       0x81u
#define RX_LOG_TIME_INIT_BEGIN      0x82u
#define RX_LOG_TIME_INIT_OK         0x83u
#define RX_LOG_RF_LIB_INIT_BEGIN    0x84u
#define RX_LOG_RF_LIB_INIT_OK       0x85u
#define RX_LOG_HOST_INIT_BEGIN      0x86u
#define RX_LOG_HOST_INIT_OK         0x87u
#define RX_LOG_HOST_INIT_FAIL       0x88u
#define RX_LOG_PAIR_START           0x89u
#define RX_LOG_PAIR_SUCCESS         0x8Au
#define RX_LOG_PAIR_TIMEOUT         0x8Bu
#define RX_LOG_PAIR_FAILURE         0x8Cu
#define RX_LOG_RF_FRAME_OK           0x8Du
#define RX_LOG_LINK_LOST            0x8Eu
#define RX_LOG_LINK_TIMEOUT         0x8Fu
#define RX_LOG_HID_RELEASE_QUEUED   0x90u
#define RX_LOG_HID_RELEASE_SENT     0x91u
#define RX_LOG_HID_RELEASE_BUSY     0x92u
/* Remote management tunnel diagnostics. result carries the command or
 * protocol status so LOG_GET can identify where a transaction stopped. */
#define RX_LOG_MGMT_REQUEST         0x93u
#define RX_LOG_MGMT_TX_QUEUED       0x94u
#define RX_LOG_MGMT_RESPONSE        0x95u
#define RX_LOG_MGMT_TIMEOUT         0x96u
#define RX_LOG_MGMT_REJECT          0x97u
#define RX_LOG_MGMT_TX_SUBMITTED    0x98u
#define RX_LOG_MGMT_RX_FIRST        0x99u
#define RX_LOG_MGMT_RX_FRAGMENT     0x9Au
#define RX_LOG_MGMT_RX_COMPLETE     0x9Bu
#define RX_LOG_MGMT_SEND_BUSY       0x9Cu
#define RX_LOG_MGMT_RX_CRC_ERROR    0x9Du
#define RX_LOG_MGMT_RX_FRAGMENT_ERR 0x9Eu

void Receiver_Log_Init(void);
void Receiver_Log_Event(uint8_t event, uint8_t stage, uint8_t result);
/* Last lifecycle checkpoint which actually returned at runtime. */
void Receiver_Log_SetCompletedStage(uint8_t stage);
uint8_t Receiver_Log_GetCompletedStage(void);
void Receiver_Log_MarkHostSeen(void);
/* Removes one retained lifecycle record. Returns 1 when an entry was read. */
uint8_t Receiver_Log_Pop(uint8_t *event, uint8_t *stage, uint8_t *result);
void Receiver_Log_Flush(void);

#endif
