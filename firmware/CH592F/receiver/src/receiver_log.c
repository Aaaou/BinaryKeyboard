#include "receiver_log.h"
#include "kbd_types.h"
#include "usb_hid.h"
#include <string.h>

#define RX_LOG_QUEUE_SIZE 16u
#define RX_LOG_QUEUE_MASK (RX_LOG_QUEUE_SIZE - 1u)

typedef struct {
    uint8_t event;
    uint8_t stage;
    uint8_t result;
} receiver_log_entry_t;

static receiver_log_entry_t s_queue[RX_LOG_QUEUE_SIZE];
static volatile uint8_t s_head;
static volatile uint8_t s_tail;
static volatile uint8_t s_host_seen;
static volatile uint8_t s_completed_stage;

void Receiver_Log_Init(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_host_seen = 0u;
    s_completed_stage = 0u;
}

void Receiver_Log_SetCompletedStage(uint8_t stage)
{
    s_completed_stage = stage;
}

uint8_t Receiver_Log_GetCompletedStage(void)
{
    return s_completed_stage;
}

void Receiver_Log_Event(uint8_t event, uint8_t stage, uint8_t result)
{
    uint8_t next = (uint8_t)((s_head + 1u) & RX_LOG_QUEUE_MASK);
    if (next == s_tail) return;

    s_queue[s_head].event = event;
    s_queue[s_head].stage = stage;
    s_queue[s_head].result = result;
    s_head = next;
}

void Receiver_Log_MarkHostSeen(void)
{
    s_host_seen = 1u;
}

uint8_t Receiver_Log_Pop(uint8_t *event, uint8_t *stage, uint8_t *result)
{
    receiver_log_entry_t entry;

    if (s_head == s_tail) return 0u;
    entry = s_queue[s_tail];
    s_tail = (uint8_t)((s_tail + 1u) & RX_LOG_QUEUE_MASK);
    *event = entry.event;
    *stage = entry.stage;
    *result = entry.result;
    return 1u;
}

void Receiver_Log_Flush(void)
{
    /* Boot records are intentionally pulled with LOG_GET. Pushing them here
     * races a browser's command response and may consume the only evidence
     * before its terminal has subscribed. */
}
