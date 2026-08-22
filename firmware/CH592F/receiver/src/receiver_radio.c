#include "receiver_radio.h"
#include "kbd_radio_protocol.h"
#include "usb_device.h"
#include "usb_hid.h"
#include "receiver_log.h"
#include "RF.h"
#include "CH59x_common.h"
#include <string.h>

#define RX_NV_SLOT_A_ADDR 0x5000u
#define RX_NV_SLOT_B_ADDR 0x5100u
#define RX_NV_MAGIC 0x47524442u
#define RX_NV_VERSION 1u
#define RX_PAIR_WINDOW_TICKS (60u * 32768u)
#define RX_KEYBOARD_DEVICE_TYPE 1u
#define RX_REPORT_QUEUE_DEPTH 16u
/* RFBound reports transport timeout independently of application traffic.
 * Allow a short reconnect window, then release USB HID from an independent
 * 1 ms timer so RF/TMOS work cannot delay stuck-key protection. */
#define RX_RF_DISCONNECT_GRACE_TICKS ((32768u * 400u) / 1000u)
#define RX_RF_ACTIVITY_TIMEOUT_TICKS ((32768u * 600u) / 1000u)
#define RX_RELEASE_REPEAT_TICKS ((32768u * 200u) / 1000u)
#define RX_RELEASE_KEYBOARD 0x01u
#define RX_RELEASE_MOUSE    0x02u
#define RX_RELEASE_CONSUMER 0x04u
#define RX_RELEASE_ALL      0x07u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;
    uint8_t peer[6];
    uint8_t device_id;
    uint8_t reserved;
    uint16_t poll_rate;
    uint16_t crc;
} receiver_nv_t;

typedef enum {
    RX_CONTROL_NONE = 0,
    RX_CONTROL_PAIR_START,
    RX_CONTROL_PAIR_CANCEL,
    RX_CONTROL_PAIR_CLEAR,
    RX_CONTROL_POLL_RATE,
} receiver_control_t;

typedef enum {
    RX_FILTER_BINDABLE = 0,
    RX_FILTER_RECONNECT,
    RX_FILTER_ACTIVE,
} receiver_filter_mode_t;

typedef struct {
    USB_KeyboardReport_t items[RX_REPORT_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
} keyboard_queue_t;

typedef struct {
    USB_MouseReport_t items[RX_REPORT_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
} mouse_queue_t;

typedef struct {
    USB_ConsumerReport_t items[RX_REPORT_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
} consumer_queue_t;

static receiver_nv_t s_nv;
static uint32_t s_nv_active_addr;
static uint8_t s_local[6];
static volatile bool s_rx_pending;
static volatile bool s_binding_pending;
static volatile bool s_release_pending;
static volatile receiver_control_t s_control_pending;
static volatile uint16_t s_requested_poll_rate;
static volatile bool s_control_complete;
static volatile int s_control_result;
static volatile kbd_radio_pair_state_t s_state = KBD_RADIO_PAIR_UNBOUND;
static uint8_t s_pending_peer[6];
static uint8_t s_pending_device_id;
static uint32_t s_pair_started;
static uint32_t s_last_usb_report;
static uint64_t s_usb_poll_phase;
static uint32_t s_last_session;
static uint32_t s_last_sequence;
static uint32_t s_last_valid_rx;
static bool s_has_sequence;
static keyboard_queue_t s_keyboard_queue;
static mouse_queue_t s_mouse_queue;
static consumer_queue_t s_consumer_queue;
static rfRoleList_t s_speed_item;
static rfRoleSpeed_t s_speed_list = {1, &s_speed_item};
/* WCH's RF Host uses TMOS task/message memory after RFBound_StartHost(). */
static __attribute__((aligned(4))) uint8_t s_tmos_memory[1024];
static volatile uint8_t s_host_startup_state;
static volatile uint8_t s_host_startup_result;
static bool s_host_active;
static bool s_boot_host_pending;
static uint32_t s_boot_host_due;
static bool s_pair_success_logged;
static bool s_pair_timeout_logged;
static volatile bool s_host_restart_pending;
static volatile bool s_rf_disconnect_pending;
static uint32_t s_rf_disconnect_started;
static volatile uint8_t s_emergency_release_mask;
static volatile bool s_link_lost_log_pending;
static bool s_release_sent_logged;
static bool s_release_busy_logged;
static volatile bool s_release_repeat_active;
static uint32_t s_release_repeat_started;
static volatile bool s_rx_quarantine;
/* Device-side timestamps avoid confusing LOG_GET retrieval time with event
 * time when diagnosing RF-to-USB disconnect latency. */
static volatile uint32_t s_last_link_timeout;
static volatile uint32_t s_last_rf_activity;
static volatile uint32_t s_last_release_queued;
static volatile uint32_t s_last_release_sent;
static volatile uint16_t s_release_busy_count;
#define RX_DIAG_NONE 0xFFFFFFFFu
extern RF_DMADESCTypeDef *pDMARxGet;

static void receiver_tmos_enable_irq(void)
{
    PFIC_EnableIRQ(BLEL_IRQn);
    PFIC_EnableIRQ(RTC_IRQn);
}

static void receiver_tmos_disable_irq(void)
{
    PFIC_DisableIRQ(BLEL_IRQn);
    PFIC_DisableIRQ(RTC_IRQn);
}

void Receiver_Radio_TmosInit(void)
{
    tmosConfig_t config;
    memset(&config, 0, sizeof(config));
    config.MEMAddr = (uint32_t)s_tmos_memory;
    config.MEMLen = sizeof(s_tmos_memory);
    config.TaskMaxCount = 8u;
    config.enableTmosIrq = receiver_tmos_enable_irq;
    config.disableTmosIrq = receiver_tmos_disable_irq;
    TMOS_Init(&config);
}

static bool valid_poll_rate(uint16_t rate)
{
    return rate == 125u || rate == 250u || rate == 500u || rate == 1000u;
}

static uint32_t rtc_elapsed(uint32_t now, uint32_t then)
{
    return now >= then ? now - then : (RTC_MAX_COUNT - then) + now;
}

static bool has_peer(void)
{
    for (uint8_t i = 0; i < sizeof(s_nv.peer); i++) {
        if (s_nv.peer[i] != 0u) return true;
    }
    return false;
}

static bool nv_valid(const receiver_nv_t *nv)
{
    if (nv->magic != RX_NV_MAGIC || nv->version != RX_NV_VERSION ||
        nv->size != sizeof(*nv) || nv->device_id > RF_ROLE_ID_INVALD ||
        !valid_poll_rate(nv->poll_rate)) return false;
    /* A binding is either completely empty or contains both peer identity and
     * an assigned keyboard slot. Reject half-written/half-erased records. */
    bool peer_present = false;
    for (uint8_t i = 0; i < sizeof(nv->peer); i++) {
        if (nv->peer[i] != 0u) {
            peer_present = true;
            break;
        }
    }
    if (peer_present != (nv->device_id != RF_ROLE_ID_INVALD)) return false;
    return nv->crc == KBD_RadioProtocol_Crc16((const uint8_t *)nv,
                                               (uint16_t)(sizeof(*nv) - sizeof(nv->crc)));
}

static bool generation_newer(uint32_t lhs, uint32_t rhs)
{
    return (int32_t)(lhs - rhs) > 0;
}

static void load_nv(void)
{
    receiver_nv_t a;
    receiver_nv_t b;
    EEPROM_READ(RX_NV_SLOT_A_ADDR, &a, sizeof(a));
    EEPROM_READ(RX_NV_SLOT_B_ADDR, &b, sizeof(b));
    bool a_valid = nv_valid(&a);
    bool b_valid = nv_valid(&b);

    if (a_valid && (!b_valid || generation_newer(a.generation, b.generation))) {
        s_nv = a;
        s_nv_active_addr = RX_NV_SLOT_A_ADDR;
    } else if (b_valid) {
        s_nv = b;
        s_nv_active_addr = RX_NV_SLOT_B_ADDR;
    } else {
        memset(&s_nv, 0, sizeof(s_nv));
        s_nv.version = RX_NV_VERSION;
        s_nv.size = sizeof(s_nv);
        s_nv.poll_rate = 1000u;
        s_nv.device_id = RF_ROLE_ID_INVALD;
        s_nv_active_addr = 0u;
    }
}

static int save_nv(void)
{
    receiver_nv_t verify;
    uint32_t target = s_nv_active_addr == RX_NV_SLOT_A_ADDR
        ? RX_NV_SLOT_B_ADDR : RX_NV_SLOT_A_ADDR;
    s_nv.magic = RX_NV_MAGIC;
    s_nv.version = RX_NV_VERSION;
    s_nv.size = sizeof(s_nv);
    s_nv.generation++;
    s_nv.crc = KBD_RadioProtocol_Crc16((const uint8_t *)&s_nv,
                                       (uint16_t)(sizeof(s_nv) - sizeof(s_nv.crc)));
    if (EEPROM_ERASE(target, EEPROM_PAGE_SIZE) != 0u ||
        EEPROM_WRITE(target, &s_nv, sizeof(s_nv)) != 0u) return -1;
    EEPROM_READ(target, &verify, sizeof(verify));
    if (!nv_valid(&verify) || verify.generation != s_nv.generation) return -1;
    s_nv_active_addr = target;
    return 0;
}

static void apply_filter(receiver_filter_mode_t mode)
{
    memset(&s_speed_item, 0, sizeof(s_speed_item));
    /* WCH's Host first reconnects through binding ID 7 while filtering the
     * saved peer, then switches to the assigned device ID after SUCCESS. */
    s_speed_item.deviceId = mode == RX_FILTER_ACTIVE
        ? s_nv.device_id : RF_ROLE_BOUND_ID;
    s_speed_item.rssi = mode == RX_FILTER_BINDABLE ? -70 : 0;
    /* WCH's reference dongle uses devType=0 in both the bindable and
     * peer-filtered Host lists. Device type remains part of the application
     * frame contract; the RFBound filter must not reject a valid keyboard
     * during the bind transaction. */
    s_speed_item.devType = 0u;
    if (mode != RX_FILTER_BINDABLE) {
        memcpy(s_speed_item.peerInfo, s_nv.peer, sizeof(s_nv.peer));
    }
    RFBound_SetSpeedType(&s_speed_list);
}

static void bound_cb(staBound_t *status)
{
    if (status->status == SUCCESS) {
        s_rf_disconnect_pending = false;
        s_rx_quarantine = false;
        s_release_repeat_active = false;
        memcpy(s_pending_peer, status->PeerInfo, sizeof(s_pending_peer));
        s_pending_device_id = status->devId;
        s_binding_pending = true;
        /* Binding success only confirms the RFBound transaction.  The
         * application link is connected only after process_rf_rx() validates
         * an actual keyboard frame from this peer. */
        s_state = KBD_RADIO_PAIR_BOUND;
        if (!s_pair_success_logged) {
            Receiver_Log_Event(RX_LOG_PAIR_SUCCESS, KBD_RECEIVER_STARTUP_STAGE, status->devId);
            s_pair_success_logged = true;
        }
    } else if (status->status == bleTimeout) {
        s_has_sequence = false;
        /* A late RFBound callback may arrive after the visible state has
         * already fallen back to BOUND. A valid application frame proves that
         * this was an active link, so still arm the HID failsafe release. */
        if ((s_state == KBD_RADIO_PAIR_CONNECTED || s_last_valid_rx != 0u) &&
            !s_rf_disconnect_pending) {
            s_rf_disconnect_pending = true;
            s_rf_disconnect_started = RTC_GetCycle32k();
            s_last_link_timeout = s_rf_disconnect_started;
            s_release_sent_logged = false;
            s_release_busy_logged = false;
            Receiver_Log_Event(RX_LOG_LINK_TIMEOUT, KBD_RECEIVER_STARTUP_STAGE, 0u);
        }
        if (!s_pair_timeout_logged) {
            Receiver_Log_Event(RX_LOG_PAIR_TIMEOUT, KBD_RECEIVER_STARTUP_STAGE, 0u);
            s_pair_timeout_logged = true;
        }
    } else {
        s_has_sequence = false;
        if ((s_state == KBD_RADIO_PAIR_CONNECTED || s_last_valid_rx != 0u) &&
            !s_rf_disconnect_pending) {
            s_rf_disconnect_pending = true;
            s_rf_disconnect_started = RTC_GetCycle32k();
            s_last_link_timeout = s_rf_disconnect_started;
            s_release_sent_logged = false;
            s_release_busy_logged = false;
            Receiver_Log_Event(RX_LOG_LINK_TIMEOUT, KBD_RECEIVER_STARTUP_STAGE,
                               status->status);
        } else if (s_state != KBD_RADIO_PAIR_CONNECTED) {
            s_state = has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        }
        /* Match WCH dongle rf.c: FAILURE clears stale RX data and posts the
         * peer-filtered Host start event. Defer both calls out of RF context. */
        if (has_peer()) s_host_restart_pending = true;
        Receiver_Log_Event(RX_LOG_PAIR_FAILURE, KBD_RECEIVER_STARTUP_STAGE, status->status);
    }
}

static void irq_cb(rfRole_States_t status, uint8_t id)
{
    (void)id;
    if ((status & RF_STATE_RX) != 0u) s_last_rf_activity = RTC_GetCycle32k();
    if ((status & RF_STATE_RX) != 0u) s_rx_pending = true;
}

static int start_host(bool pairing)
{
    rfBoundHost_t host;
    memset(&host, 0, sizeof(host));
    apply_filter(pairing ? RX_FILTER_BINDABLE : RX_FILTER_RECONNECT);
    host.periTime = 8;
    host.hop = RF_HOP_MANUF_MODE;
    /* Match WCH's CH592 reference dongle (dongle/APP/rf.c). */
    host.timeout = 100;
    /* WCH marks the Host devType field as reserved. Device type filtering
     * belongs exclusively to rfRoleList_t, configured by apply_filter(). */
    host.devType = 0u;
    memcpy(host.OwnInfo, s_local, sizeof(s_local));
    /* WCH's dongle reference filters a saved peer through rfRoleList_t and
     * leaves rfBoundHost_t.PeerInfo zeroed. Keep a single source of truth. */
    host.rfBoundCB = bound_cb;
    if (pairing) s_pair_started = RTC_GetCycle32k();
    /* A link-age value belongs to the current RF Host session. Do not carry
     * a timestamp from a previous binding into the next diagnostic view. */
    s_last_valid_rx = 0u;
    s_has_sequence = false;
    s_pair_success_logged = false;
    s_pair_timeout_logged = false;
    s_state = pairing ? KBD_RADIO_PAIRING : KBD_RADIO_PAIR_BOUND;
    if (pairing) s_rx_quarantine = false;
    s_host_startup_state = 1u;
    s_host_startup_result = 0u;
    bStatus_t result = RFBound_StartHost(&host);
    s_host_startup_result = (uint8_t)result;
    if (result != SUCCESS) {
        s_host_startup_state = 3u;
        s_state = has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        return -1;
    }
    s_host_startup_state = 2u;
    s_host_active = true;
    return 0;
}

/* RFRole_Shut is only valid after RFBound_StartHost succeeded.  In the
 * manual-start diagnostic build, the first pairing request has no role to
 * stop yet. */
static void stop_host(void)
{
    if (!s_host_active) return;
    RFRole_Shut();
    s_host_active = false;
    s_host_restart_pending = false;
}

static void keyboard_enqueue(const USB_KeyboardReport_t *report)
{
    if (s_keyboard_queue.count == RX_REPORT_QUEUE_DEPTH) {
        /* Keyboard reports are complete snapshots, not events. Once backlog
         * exists, retaining stale transitions is worse than dropping them:
         * collapse to the newest state so a release cannot sit behind old
         * presses and keep the host logically stuck. */
        s_keyboard_queue.head = 0u;
        s_keyboard_queue.count = 1u;
        memcpy(&s_keyboard_queue.items[0], report, sizeof(*report));
        return;
    }
    uint8_t tail = (uint8_t)((s_keyboard_queue.head + s_keyboard_queue.count) % RX_REPORT_QUEUE_DEPTH);
    memcpy(&s_keyboard_queue.items[tail], report, sizeof(*report));
    s_keyboard_queue.count++;
}

static void mouse_enqueue(const USB_MouseReport_t *report)
{
    if (s_mouse_queue.count == RX_REPORT_QUEUE_DEPTH) {
        uint8_t newest = (uint8_t)((s_mouse_queue.head + s_mouse_queue.count - 1u) % RX_REPORT_QUEUE_DEPTH);
        USB_MouseReport_t *queued = &s_mouse_queue.items[newest];
        int16_t x = (int16_t)queued->x + report->x;
        int16_t y = (int16_t)queued->y + report->y;
        int16_t wheel = (int16_t)queued->wheel + report->wheel;
        queued->buttons = report->buttons;
        queued->x = (int8_t)(x > 127 ? 127 : (x < -127 ? -127 : x));
        queued->y = (int8_t)(y > 127 ? 127 : (y < -127 ? -127 : y));
        queued->wheel = (int8_t)(wheel > 127 ? 127 : (wheel < -127 ? -127 : wheel));
        return;
    }
    uint8_t tail = (uint8_t)((s_mouse_queue.head + s_mouse_queue.count) % RX_REPORT_QUEUE_DEPTH);
    memcpy(&s_mouse_queue.items[tail], report, sizeof(*report));
    s_mouse_queue.count++;
}

static void consumer_enqueue(const USB_ConsumerReport_t *report)
{
    if (s_consumer_queue.count == RX_REPORT_QUEUE_DEPTH) {
        uint8_t newest = (uint8_t)((s_consumer_queue.head + s_consumer_queue.count - 1u) % RX_REPORT_QUEUE_DEPTH);
        memcpy(&s_consumer_queue.items[newest], report, sizeof(*report));
        return;
    }
    uint8_t tail = (uint8_t)((s_consumer_queue.head + s_consumer_queue.count) % RX_REPORT_QUEUE_DEPTH);
    memcpy(&s_consumer_queue.items[tail], report, sizeof(*report));
    s_consumer_queue.count++;
}

static void enqueue_release_all(void)
{
    USB_KeyboardReport_t keyboard = {0};
    USB_MouseReport_t mouse = {0};
    USB_ConsumerReport_t consumer = {0};
    memset(&s_keyboard_queue, 0, sizeof(s_keyboard_queue));
    memset(&s_mouse_queue, 0, sizeof(s_mouse_queue));
    memset(&s_consumer_queue, 0, sizeof(s_consumer_queue));
    keyboard_enqueue(&keyboard);
    mouse_enqueue(&mouse);
    consumer_enqueue(&consumer);
    s_release_pending = false;
}

static bool frame_is_new(const kbd_radio_frame_t *frame)
{
    if (!s_has_sequence || frame->header.session != s_last_session) return true;
    return (int32_t)(frame->header.sequence - s_last_sequence) > 0;
}

static void process_rf_rx(void)
{
    uint8_t descriptors_seen = 0u;
    while ((pDMARxGet->Status & STA_DMA_ENABLE) == 0u) {
        uint16_t raw_len = (uint16_t)(pDMARxGet->Status & STA_LEN_MASK);
        if (!s_rx_quarantine &&
            raw_len >= PKT_HEAD_LEN && raw_len <= pDMARxGet->BufferSize &&
            raw_len <= (uint16_t)(PKT_HEAD_LEN + sizeof(kbd_radio_frame_t))) {
            uint16_t len = (uint16_t)(raw_len - PKT_HEAD_LEN);
            const kbd_radio_frame_t *frame =
                (const kbd_radio_frame_t *)(pDMARxGet->BufferAddr + PKT_HEAD_LEN);
            if (KBD_RadioProtocol_Validate(frame, len) && frame_is_new(frame)) {
                s_last_session = frame->header.session;
                s_last_sequence = frame->header.sequence;
                s_last_valid_rx = RTC_GetCycle32k();
                s_has_sequence = true;
                s_rf_disconnect_pending = false;
                if (frame->header.type == KBD_RADIO_FRAME_KEYBOARD && frame->header.length == 8u) {
                    keyboard_enqueue((const USB_KeyboardReport_t *)frame->payload);
                } else if (frame->header.type == KBD_RADIO_FRAME_MOUSE && frame->header.length == 4u) {
                    mouse_enqueue((const USB_MouseReport_t *)frame->payload);
                } else if (frame->header.type == KBD_RADIO_FRAME_CONSUMER && frame->header.length == 2u) {
                    consumer_enqueue((const USB_ConsumerReport_t *)frame->payload);
                }
                if (s_state != KBD_RADIO_PAIR_CONNECTED) {
                    Receiver_Log_Event(RX_LOG_RF_FRAME_OK, KBD_RECEIVER_STARTUP_STAGE,
                                       (uint8_t)frame->header.type);
                }
                s_state = KBD_RADIO_PAIR_CONNECTED;
            }
        }
        pDMARxGet->Status = STA_DMA_ENABLE;
        pDMARxGet = (RF_DMADESCTypeDef *)pDMARxGet->NextDescAddr;
        if (++descriptors_seen >= 8u) break;
    }
    s_rx_pending = false;
}

static void flush_usb_reports(uint32_t now)
{
    uint32_t elapsed = rtc_elapsed(now, s_last_usb_report);
    s_last_usb_report = now;
    if (g_USB_DeviceState != USB_STATE_CONFIGURED) {
        s_usb_poll_phase = 0u;
        return;
    }
    s_usb_poll_phase += (uint64_t)elapsed * s_nv.poll_rate;
    if (s_usb_poll_phase < 32768u) return;
    s_usb_poll_phase %= 32768u;

    if (s_keyboard_queue.count != 0u &&
        USB_Keyboard_TrySend(&s_keyboard_queue.items[s_keyboard_queue.head])) {
        s_keyboard_queue.head = (uint8_t)((s_keyboard_queue.head + 1u) % RX_REPORT_QUEUE_DEPTH);
        s_keyboard_queue.count--;
    }
    if (s_mouse_queue.count != 0u && USB_Mouse_TrySend(&s_mouse_queue.items[s_mouse_queue.head])) {
        s_mouse_queue.head = (uint8_t)((s_mouse_queue.head + 1u) % RX_REPORT_QUEUE_DEPTH);
        s_mouse_queue.count--;
    }
    if (s_consumer_queue.count != 0u &&
        USB_Consumer_TrySend(&s_consumer_queue.items[s_consumer_queue.head])) {
        s_consumer_queue.head = (uint8_t)((s_consumer_queue.head + 1u) % RX_REPORT_QUEUE_DEPTH);
        s_consumer_queue.count--;
    }
}

static void emergency_release_try(void)
{
    USB_KeyboardReport_t keyboard = {0};
    USB_MouseReport_t mouse = {0};
    USB_ConsumerReport_t consumer = {0};

    uint8_t before = s_emergency_release_mask;
    if ((s_emergency_release_mask & RX_RELEASE_KEYBOARD) != 0u &&
        USB_Keyboard_TrySend(&keyboard)) {
        s_emergency_release_mask &= (uint8_t)~RX_RELEASE_KEYBOARD;
    }
    if ((s_emergency_release_mask & RX_RELEASE_MOUSE) != 0u &&
        USB_Mouse_TrySend(&mouse)) {
        s_emergency_release_mask &= (uint8_t)~RX_RELEASE_MOUSE;
    }
    if ((s_emergency_release_mask & RX_RELEASE_CONSUMER) != 0u &&
        USB_Consumer_TrySend(&consumer)) {
        s_emergency_release_mask &= (uint8_t)~RX_RELEASE_CONSUMER;
    }
    if (s_emergency_release_mask != before) {
        if (!s_release_sent_logged) {
            s_last_release_sent = RTC_GetCycle32k();
            Receiver_Log_Event(RX_LOG_HID_RELEASE_SENT,
                               KBD_RECEIVER_STARTUP_STAGE,
                               (uint8_t)(before ^ s_emergency_release_mask));
            s_release_sent_logged = true;
        }
    } else if (before != 0u && g_USB_DeviceState == USB_STATE_CONFIGURED) {
        if (s_release_busy_count != 0xFFFFu) s_release_busy_count++;
        if (!s_release_busy_logged) {
            Receiver_Log_Event(RX_LOG_HID_RELEASE_BUSY,
                               KBD_RECEIVER_STARTUP_STAGE, before);
            s_release_busy_logged = true;
        }
    }
}

__INTERRUPT
__HIGH_CODE
void TMR2_IRQHandler(void)
{
    uint32_t now = RTC_GetCycle32k();
    TMR2_ClearITFlag(TMR0_3_IT_CYC_END);

    /* RFBound may spend seconds in its internal retry path before calling
     * bleTimeout. Use RF-layer receive activity, rather than application HID
     * edges, for an early stuck-key failsafe. */
    if (!s_rf_disconnect_pending && s_state == KBD_RADIO_PAIR_CONNECTED &&
        s_last_rf_activity != 0u &&
        rtc_elapsed(now, s_last_rf_activity) >= RX_RF_ACTIVITY_TIMEOUT_TICKS) {
        s_rf_disconnect_pending = true;
        /* Reuse the release path but do not add another 400 ms grace period. */
        s_rf_disconnect_started = now - RX_RF_DISCONNECT_GRACE_TICKS;
        s_last_link_timeout = now;
        s_release_sent_logged = false;
        s_release_busy_logged = false;
        Receiver_Log_Event(RX_LOG_RF_ACTIVITY_TIMEOUT,
                           KBD_RECEIVER_STARTUP_STAGE, 0u);
    }

    if (s_rf_disconnect_pending &&
        rtc_elapsed(now, s_rf_disconnect_started) >=
            RX_RF_DISCONNECT_GRACE_TICKS) {
        s_rf_disconnect_pending = false;
        s_has_sequence = false;
        s_state = has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        s_emergency_release_mask = RX_RELEASE_ALL;
        s_release_pending = true;
        s_link_lost_log_pending = true;
        s_release_sent_logged = false;
        s_release_busy_logged = false;
        s_release_repeat_active = true;
        s_release_repeat_started = now;
        s_last_release_queued = s_release_repeat_started;
        s_release_busy_count = 0u;
        s_rx_quarantine = true;
        Receiver_Log_Event(RX_LOG_HID_RELEASE_QUEUED,
                           KBD_RECEIVER_STARTUP_STAGE, RX_RELEASE_ALL);
    }
    /* USB endpoint/DMA access is main-loop owned. Timer2 only raises the mask. */
}

static void process_binding_save(void)
{
    if (!s_binding_pending) return;
    uint8_t pending_peer[6];
    uint8_t pending_device_id;
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    memcpy(pending_peer, s_pending_peer, sizeof(pending_peer));
    pending_device_id = s_pending_device_id;
    s_binding_pending = false;
    SYS_RecoverIrq(irq);
    if (memcmp(s_nv.peer, pending_peer, sizeof(s_nv.peer)) == 0 &&
        s_nv.device_id == pending_device_id) {
        apply_filter(RX_FILTER_ACTIVE);
        s_has_sequence = false;
        s_last_valid_rx = 0u;
        return;
    }
    uint8_t old_peer[6];
    uint8_t old_device_id = s_nv.device_id;
    memcpy(old_peer, s_nv.peer, sizeof(old_peer));
    memcpy(s_nv.peer, pending_peer, sizeof(s_nv.peer));
    s_nv.device_id = pending_device_id;
    if (save_nv() == 0) {
        apply_filter(RX_FILTER_ACTIVE);
        s_has_sequence = false;
        s_last_valid_rx = 0u;
        /* The current RFBound Host remains active. Switching its speed list
         * to the assigned ID is the official transition from binding to the
         * normal peer schedule; restarting Host here would discard it. */
    } else {
        memcpy(s_nv.peer, old_peer, sizeof(s_nv.peer));
        s_nv.device_id = old_device_id;
        RFRole_ClearRxData(pending_device_id);
        s_state = has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
    }
}

static void process_control(void)
{
    receiver_control_t control;
    uint16_t requested_rate;
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    control = s_control_pending;
    requested_rate = s_requested_poll_rate;
    s_control_pending = RX_CONTROL_NONE;
    SYS_RecoverIrq(irq);
    if (control == RX_CONTROL_NONE) return;
    int result = 0;

    if (control == RX_CONTROL_PAIR_START) {
        s_release_pending = true;
        Receiver_Log_Event(RX_LOG_PAIR_START, KBD_RECEIVER_STARTUP_STAGE, 0u);
        stop_host();
        result = start_host(true);
    } else if (control == RX_CONTROL_PAIR_CANCEL) {
        stop_host();
        if (has_peer()) result = start_host(false);
        else s_state = KBD_RADIO_PAIR_UNBOUND;
    } else if (control == RX_CONTROL_PAIR_CLEAR) {
        s_release_pending = true;
        stop_host();
        SYS_DisableAllIrq(&irq);
        s_binding_pending = false;
        SYS_RecoverIrq(irq);
        uint8_t old_peer[6];
        uint8_t old_device_id = s_nv.device_id;
        memcpy(old_peer, s_nv.peer, sizeof(old_peer));
        memset(s_nv.peer, 0, sizeof(s_nv.peer));
        s_nv.device_id = RF_ROLE_ID_INVALD;
        s_has_sequence = false;
        if (save_nv() == 0) {
            if (old_device_id <= 6u) RFRole_ClearRxData(old_device_id);
            s_last_valid_rx = 0u;
            s_state = KBD_RADIO_PAIR_UNBOUND;
            s_host_startup_state = 0u;
            s_host_startup_result = 0u;
            /* Manual clear is a stable state. Do not immediately rebind to
             * a still-powered old keyboard; pairing requires an explicit
             * RADIO_PAIR_START command. */
            result = 0;
        } else {
            memcpy(s_nv.peer, old_peer, sizeof(s_nv.peer));
            s_nv.device_id = old_device_id;
            s_state = has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
            result = -1;
        }
    } else if (control == RX_CONTROL_POLL_RATE && valid_poll_rate(requested_rate)) {
        uint16_t old_rate = s_nv.poll_rate;
        s_nv.poll_rate = requested_rate;
        s_usb_poll_phase = 0u;
        if (save_nv() != 0) { s_nv.poll_rate = old_rate; result = -1; }
    }
    if (control == RX_CONTROL_POLL_RATE && !valid_poll_rate(requested_rate)) result = -1;
    SYS_DisableAllIrq(&irq);
    s_control_result = result;
    s_control_complete = true;
    SYS_RecoverIrq(irq);
}

static int request_control(receiver_control_t control, uint16_t rate)
{
    uint32_t irq;
    int result = 0;
    SYS_DisableAllIrq(&irq);
    if (s_control_pending != RX_CONTROL_NONE || s_control_complete) result = -1;
    else {
        s_requested_poll_rate = rate;
        s_control_complete = false;
        s_control_pending = control;
    }
    SYS_RecoverIrq(irq);
    return result;
}

int Receiver_Radio_Init(void)
{
    /* WCH's reference dongle queues RFBound_StartHost from its TMOS task.
     * Keep RF library setup and Host role startup in separate main-loop
     * turns so USB enumeration/management is not interrupted by the first
     * RF timer/channel setup. */
    s_boot_host_pending = true;
    /* Run on the next main-loop turn.  A wall-clock delay is unnecessary here
     * and is unsafe around the CH592 RTC's non-2^32 wrap value. */
    s_boot_host_due = RTC_GetCycle32k();
    s_host_startup_state = 0u;
    s_host_startup_result = 0u;
    return 0;
}

void Receiver_Radio_RfLibraryInit(void)
{
    load_nv();
    GetMACAddress(s_local);
    s_last_usb_report = RTC_GetCycle32k();
    s_host_startup_state = 0u;
    s_host_startup_result = 0u;
    s_host_active = false;
    s_boot_host_pending = false;
    s_boot_host_due = 0u;
    s_last_link_timeout = RX_DIAG_NONE;
    s_last_rf_activity = 0u;
    s_last_release_queued = RX_DIAG_NONE;
    s_last_release_sent = RX_DIAG_NONE;
    s_release_busy_count = 0u;
    RF_LibInit(irq_cb);
    TMR2_TimerInit(GetSysClock() / 1000u);
    TMR2_ITCfg(ENABLE, TMR0_3_IT_CYC_END);
    PFIC_EnableIRQ(TMR2_IRQn);
}

uint8_t Receiver_Radio_GetHostStartupState(void) { return s_host_startup_state; }
uint8_t Receiver_Radio_GetHostStartupResult(void) { return s_host_startup_result; }

void Receiver_Radio_Process(void)
{
    process_binding_save();
    process_control();
    if (s_host_restart_pending) {
        s_host_restart_pending = false;
        RFRole_ClearRxData(s_nv.device_id);
        start_host(false);
    }
    if (s_boot_host_pending &&
        rtc_elapsed(RTC_GetCycle32k(), s_boot_host_due) < 0x80000000u) {
        s_boot_host_pending = false;
        Receiver_Log_Event(RX_LOG_HOST_INIT_BEGIN, KBD_RECEIVER_STARTUP_STAGE, 0u);
        int result = start_host(!has_peer());
        Receiver_Log_Event(result == 0 ? RX_LOG_HOST_INIT_OK : RX_LOG_HOST_INIT_FAIL,
                           KBD_RECEIVER_STARTUP_STAGE, (uint8_t)result);
        if (result == 0) Receiver_Log_SetCompletedStage(3u);
    }
    /* RF_LibInit has completed, but there is no active descriptor role before
     * RFBound_StartHost succeeds.  Keep the manual diagnostic boot path
     * identical to the validated stage-2 path until pairing is requested. */
    if (!s_host_active) return;
    if (s_rx_pending || (pDMARxGet->Status & STA_DMA_ENABLE) == 0u) process_rf_rx();
    uint32_t now = RTC_GetCycle32k();
    if (s_link_lost_log_pending) {
        s_link_lost_log_pending = false;
        Receiver_Log_Event(RX_LOG_LINK_LOST, KBD_RECEIVER_STARTUP_STAGE, 0u);
    }
    if (s_emergency_release_mask != 0u) s_release_pending = true;
    if (s_release_pending) enqueue_release_all();
    if (s_emergency_release_mask != 0u) emergency_release_try();
    if (s_release_repeat_active) {
        if (rtc_elapsed(now, s_release_repeat_started) < RX_RELEASE_REPEAT_TICKS) {
            /* Repeat all-zero snapshots for a short bounded window. This is
             * deliberately finite: it protects against one missed USB IN
             * transaction without flooding a healthy host forever. */
            s_emergency_release_mask = RX_RELEASE_ALL;
            emergency_release_try();
        } else {
            s_release_repeat_active = false;
        }
    }
    flush_usb_reports(now);
    if (s_state == KBD_RADIO_PAIRING && rtc_elapsed(now, s_pair_started) >= RX_PAIR_WINDOW_TICKS) {
        s_release_pending = true;
        s_has_sequence = false;
        stop_host();
        if (has_peer()) start_host(false);
        else s_state = KBD_RADIO_PAIR_UNBOUND;
    }
}

int Receiver_Radio_StartPairing(void) { return request_control(RX_CONTROL_PAIR_START, 0u); }
int Receiver_Radio_CancelPairing(void) { return request_control(RX_CONTROL_PAIR_CANCEL, 0u); }
int Receiver_Radio_ClearPairing(void) { return request_control(RX_CONTROL_PAIR_CLEAR, 0u); }
kbd_radio_pair_state_t Receiver_Radio_GetState(void) { return s_state; }
uint8_t Receiver_Radio_GetPeerDeviceId(void) { return has_peer() ? s_nv.device_id : RF_ROLE_ID_INVALD; }
bool Receiver_Radio_HasPeer(void) { return has_peer(); }
/* The receiver is the RFBound host and uses the reserved binding slot. */
uint8_t Receiver_Radio_GetDeviceId(void) { return RF_ROLE_BOUND_ID; }
void Receiver_Radio_GetLocalId(uint8_t out[6]) { if (out) memcpy(out, s_local, 6); }
void Receiver_Radio_GetPeerId(uint8_t out[6]) { if (out) memcpy(out, s_nv.peer, 6); }
uint32_t Receiver_Radio_GetPairFingerprint(void)
{
    uint32_t h = 2166136261u;
    /* Canonical order is keyboard ID then receiver ID, matching the
     * keyboard-side fingerprint calculation. */
    for (uint8_t i = 0; i < 6; i++) { h ^= s_nv.peer[i]; h *= 16777619u; }
    for (uint8_t i = 0; i < 6; i++) { h ^= s_local[i]; h *= 16777619u; }
    h ^= s_nv.device_id; h *= 16777619u;
    return has_peer() ? h : 0u;
}
uint32_t Receiver_Radio_GetPairGeneration(void) { return s_nv.generation; }
uint32_t Receiver_Radio_GetLastValidAge(void)
{
    return s_last_valid_rx ? rtc_elapsed(RTC_GetCycle32k(), s_last_valid_rx) : 0xFFFFFFFFu;
}
uint32_t Receiver_Radio_GetLastRfActivityAge(void)
{
    return s_last_rf_activity ? rtc_elapsed(RTC_GetCycle32k(), s_last_rf_activity)
                              : RX_DIAG_NONE;
}
static uint32_t receiver_diag_age(volatile uint32_t timestamp)
{
    return timestamp == RX_DIAG_NONE ? RX_DIAG_NONE : rtc_elapsed(RTC_GetCycle32k(), timestamp);
}
uint32_t Receiver_Radio_GetLastLinkTimeoutAge(void) { return receiver_diag_age(s_last_link_timeout); }
uint32_t Receiver_Radio_GetLastReleaseQueuedAge(void) { return receiver_diag_age(s_last_release_queued); }
uint32_t Receiver_Radio_GetLastReleaseSentAge(void) { return receiver_diag_age(s_last_release_sent); }
uint16_t Receiver_Radio_GetReleaseBusyCount(void) { return s_release_busy_count; }
uint16_t Receiver_Radio_GetPollRate(void) { return s_nv.poll_rate; }
int Receiver_Radio_SetPollRate(uint16_t rate)
{
    if (!valid_poll_rate(rate)) return -1;
    return request_control(RX_CONTROL_POLL_RATE, rate);
}

bool Receiver_Radio_TakeControlResult(int *result)
{
    uint32_t irq;
    bool ready;
    SYS_DisableAllIrq(&irq);
    ready = s_control_complete;
    if (ready) {
        if (result) *result = s_control_result;
        s_control_complete = false;
    }
    SYS_RecoverIrq(irq);
    return ready;
}
