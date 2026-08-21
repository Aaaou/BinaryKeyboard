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
