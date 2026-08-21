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

void Receiver_Log_Init(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_host_seen = 0u;
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

void Receiver_Log_Flush(void)
{
    uint8_t payload[5];
    receiver_log_entry_t entry;

    if (!s_host_seen || s_head == s_tail) return;
    entry = s_queue[s_tail];

    /* [SUB=system][LEN=3][event][startup stage][operation result]. */
    payload[0] = KBD_LOG_SYSTEM_EVENT;
    payload[1] = 3u;
    payload[2] = entry.event;
    payload[3] = entry.stage;
    payload[4] = entry.result;
    USB_Config_SendResponse(KBD_CMD_LOG, payload, sizeof(payload));
    s_tail = (uint8_t)((s_tail + 1u) & RX_LOG_QUEUE_MASK);
}
