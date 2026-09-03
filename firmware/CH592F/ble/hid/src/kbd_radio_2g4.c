#include "kbd_radio_2g4.h"
#include "kbd_mode_config.h"

#if KBD_RADIO_2G4_ENABLED
#include "kbd_radio_protocol.h"
#include "kbd_command.h"
#include "kbd_storage.h"
#include "kbd_types.h"
#include "CH59x_common.h"
#include "RF.h"
#include "kbd_rgb.h"
#include "kbd_log.h"
#include "ws2812.h"
#include <string.h>

#define KBD_RF_BIND_ADDR 0x5000u
#define KBD_RF_BIND_MAGIC 0x3244424Bu
#define KBD_RF_DEVICE_TYPE 1u
#define KBD_RF_KEYBOARD_ID 0u
/* Allow RFBound's delayed-disconnect path to settle before updating the
 * application-visible state. Application traffic is not a link watchdog. */
#define KBD_RF_DISCONNECT_GRACE_TICKS ((32768u * 400u) / 1000u)
#define KBD_RF_FAILURE_FLASH_TICKS ((32768u * 300u) / 1000u)
#define KBD_RF_PAIR_WINDOW_TICKS (60u * 32768u)
#define KBD_RF_CAPABILITY_PERIOD_TICKS (1u * 32768u)
#define KBD_MGMT_TIMEOUT_TICKS (3u * 32768u)
#define KBD_MGMT_TX_GAP_TICKS ((4u * 32768u) / 1000u)
#define KBD_MGMT_TX_REPEATS 3u
#define KBD_HID_HELD_REFRESH_TICKS ((50u * 32768u) / 1000u)

/* A non-zero keyboard snapshot is refreshed at a bounded 20 Hz. The receiver
 * treats it as a lease and releases USB HID if the lease expires. Unlike the
 * old all-state keepalive experiment, zero/idle reports are never repeated,
 * management traffic has priority, and a busy descriptor is not retried from
 * every main-loop turn. */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t peer[6];
    uint8_t device_id;
    uint8_t reserved;
    uint16_t crc;
} kbd_rf_nv_t;

static volatile kbd_radio_pair_state_t s_state = KBD_RADIO_PAIR_UNBOUND;
static uint8_t s_local_id[6], s_peer_id[6], s_device_id = KBD_RF_KEYBOARD_ID;
/* The standalone RF build has no BLE scheduler to provide TMOS storage. */
static __attribute__((aligned(4))) uint8_t s_tmos_memory[1024];
static uint32_t s_session;
static uint32_t s_sequence;
static bool s_initialized;
/* Pairing is transactional: keep the previous profile until the new
 * RFBound transaction has completed successfully.  This mirrors the
 * profile/bond behavior used by mature wireless keyboard stacks. */
static uint8_t s_pair_backup_peer[6];
static uint8_t s_pair_backup_device_id;
static bool s_pair_backup_valid;
static uint32_t s_pair_started;
static volatile bool s_rf_restart_pending;
/* RFBound uses the same callback for an explicit bind and an automatic
 * reconnect. Keep those sessions distinct: timeout is recoverable during a
 * reconnect and must never open the 60 s pairing window. */
static bool s_manual_pairing;
static volatile uint32_t s_tx_enqueued;
static volatile uint32_t s_tx_busy;
static volatile uint32_t s_tx_finished;
static uint32_t s_last_tx;
static uint32_t s_last_capability;
static uint8_t s_keyboard_snapshot[8];
static bool s_keyboard_snapshot_held;
static uint32_t s_last_keyboard_tx;
/* RFBound callbacks run in RF context. Defer role restart work to the
 * keyboard main loop and expose only stable states to the RGB scheduler. */
static volatile uint8_t s_indicator_event;
static bool s_failure_indicated;
static bool s_failure_flash_active;
static uint32_t s_failure_flash_started;
static volatile bool s_rf_ready;
static volatile bool s_rf_rx_pending;
static volatile bool s_disconnect_pending;
static uint32_t s_disconnect_started;
extern RF_DMADESCTypeDef *pDMATxGet;
extern RF_DMADESCTypeDef DMATxDscrTab[RF_TXBUFNB];
extern RF_DMADESCTypeDef *pDMARxGet;

static struct {
    bool active;
    uint8_t transaction, command, sub, next_fragment, fragments;
    uint16_t length;
    uint8_t data[61];
} s_mgmt_rx;
static bool s_mgmt_command_pending;
static bool s_mgmt_deferred_pending;
static bool s_mgmt_response_expected;
static uint8_t s_mgmt_response_transaction;
static uint32_t s_mgmt_started;
static bool s_mgmt_last_request_valid;
static uint8_t s_mgmt_last_transaction;
static uint8_t s_mgmt_last_command;
static uint8_t s_mgmt_last_sub;
static uint32_t s_mgmt_last_completed;
static uint8_t s_mgmt_last_response[64];
static uint8_t s_mgmt_last_response_len;
static struct {
    bool active, waiting_ack;
    uint8_t frame[64], length, transaction, fragment, fragments, repeat;
    uint32_t last_send;
} s_mgmt_tx;
static bool s_mgmt_ack_pending;
static kbd_radio_mgmt_ack_t s_mgmt_ack;
static volatile bool s_mgmt_session_reset_pending;
static uint16_t s_mgmt_rx_count;
static uint16_t s_mgmt_exec_count;
static uint16_t s_mgmt_response_tx_count;
static uint32_t rf_rtc_elapsed(uint32_t now, uint32_t then);
static int rf_send(const uint8_t *data, uint8_t len);
static int rf_send_management(const uint8_t *data, uint8_t len);

static void mgmt_reset_session(void)
{
    memset(&s_mgmt_rx, 0, sizeof(s_mgmt_rx));
    memset(&s_mgmt_tx, 0, sizeof(s_mgmt_tx));
    s_mgmt_command_pending = false;
    s_mgmt_deferred_pending = false;
    s_mgmt_response_expected = false;
    s_mgmt_response_transaction = 0u;
    s_mgmt_ack_pending = false;
    memset(&s_mgmt_ack, 0, sizeof(s_mgmt_ack));
    s_mgmt_started = 0u;
    s_mgmt_last_request_valid = false;
    s_mgmt_last_transaction = 0u;
    s_mgmt_last_command = 0u;
    s_mgmt_last_sub = 0u;
    s_mgmt_last_completed = 0u;
    s_mgmt_last_response_len = 0u;
    memset(s_mgmt_last_response, 0, sizeof(s_mgmt_last_response));
}

static void mgmt_send_response(const uint8_t *frame, uint8_t len)
{
    if (!frame || len < 3u || len > 64u) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, 0, 1);
        return;
    }
    if (!s_mgmt_response_expected) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, frame[0], 9u);
        return;
    }
    if (s_mgmt_tx.active) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, frame[0], 2);
        return;
    }
    memcpy(s_mgmt_tx.frame, frame, len);
    memcpy(s_mgmt_last_response, frame, len);
    s_mgmt_last_response_len = len;
    s_mgmt_tx.length = len;
    s_mgmt_tx.transaction = s_mgmt_response_transaction;
    s_mgmt_tx.fragment = 0u;
    s_mgmt_tx.fragments = (uint8_t)((len + KBD_RADIO_MGMT_MAX_DATA - 1u) / KBD_RADIO_MGMT_MAX_DATA);
    if (!s_mgmt_tx.fragments) s_mgmt_tx.fragments = 1u;
    s_mgmt_tx.repeat = 0u;
    s_mgmt_tx.last_send = 0u;
    s_mgmt_tx.active = true;
    s_mgmt_response_expected = false;
    s_mgmt_deferred_pending = false;
    KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_TX, frame[0], 0);
}

static void mgmt_send_next_response(void)
{
    if (!s_mgmt_tx.active) return;
    uint32_t now = RTC_GetCycle32k();
    if (s_mgmt_tx.waiting_ack && s_mgmt_tx.repeat >= KBD_MGMT_TX_REPEATS) return;
    if (s_mgmt_tx.last_send != 0u &&
        rf_rtc_elapsed(now, s_mgmt_tx.last_send) < KBD_MGMT_TX_GAP_TICKS) return;
    uint8_t offset = (uint8_t)(s_mgmt_tx.fragment * KBD_RADIO_MGMT_MAX_DATA);
    uint8_t part = (uint8_t)((s_mgmt_tx.length - offset) > KBD_RADIO_MGMT_MAX_DATA ? KBD_RADIO_MGMT_MAX_DATA : s_mgmt_tx.length - offset);
    kbd_radio_mgmt_packet_t mgmt;
    uint8_t flags = (s_mgmt_tx.fragment == 0u ? KBD_RADIO_MGMT_FLAG_FIRST : 0u) |
                    (s_mgmt_tx.fragment + 1u == s_mgmt_tx.fragments ? KBD_RADIO_MGMT_FLAG_LAST : 0u);
    uint8_t ml = KBD_RadioMgmt_Encode(&mgmt, s_mgmt_tx.transaction, s_mgmt_tx.frame[0], s_mgmt_tx.frame[1], s_mgmt_tx.fragment, s_mgmt_tx.fragments, flags, s_mgmt_tx.frame + offset, part);
    kbd_radio_frame_t radio;
    uint16_t rl = KBD_RadioProtocol_Encode(&radio, KBD_RADIO_FRAME_MGMT_RESPONSE, s_session, ++s_sequence, (const uint8_t *)&mgmt, ml);
    int send_result = (rl && rl <= 64u) ? rf_send_management((const uint8_t *)&radio, (uint8_t)rl) : -1;
    if (send_result == 0) {
        if (s_mgmt_response_tx_count != 0xFFFFu) s_mgmt_response_tx_count++;
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_TX, mgmt.command, s_mgmt_tx.fragment);
        s_mgmt_tx.last_send = now;
        s_mgmt_tx.waiting_ack = true;
        s_mgmt_tx.repeat++;
    } else {
        /* Keep the failure visible on the keyboard side. A receiver-side
         * tx=1/1, rx=0/0 snapshot cannot distinguish a missing request from
         * a keyboard response that was blocked by its own TX descriptors. */
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, mgmt.command,
                               (uint8_t)(send_result < 0 ? 0xFFu : send_result));
    }
}

static void mgmt_process_frame(const kbd_radio_frame_t *radio)
{
    if (!radio || radio->header.length < 8u) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, 0u, 6u);
        return;
    }
    const kbd_radio_mgmt_packet_t *mgmt = (const kbd_radio_mgmt_packet_t *)radio->payload;
    if (!KBD_RadioMgmt_Validate(mgmt, (uint8_t)radio->header.length)) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, 0, 3);
        return;
    }
    KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_RX, mgmt->command, mgmt->transaction);
    if (s_mgmt_rx_count != 0xFFFFu) s_mgmt_rx_count++;
    if (s_mgmt_last_request_valid &&
        rf_rtc_elapsed(RTC_GetCycle32k(), s_mgmt_last_completed) < KBD_MGMT_TIMEOUT_TICKS &&
        mgmt->transaction == s_mgmt_last_transaction &&
        mgmt->command == s_mgmt_last_command && mgmt->sub == s_mgmt_last_sub) {
        /* The receiver deliberately repeats request fragments. If the first
         * response was lost, replay the cached response instead of executing
         * a write command twice or silently waiting for the receiver timeout. */
        if (mgmt->fragment == 0u && !s_mgmt_tx.active && s_mgmt_last_response_len != 0u) {
            s_mgmt_response_transaction = mgmt->transaction;
            s_mgmt_response_expected = true;
            mgmt_send_response(s_mgmt_last_response, s_mgmt_last_response_len);
        }
        return;
    }
    /* After an RF session reset the storage task can still be finishing an
     * operation owned by the old session. Keep new transactions out until
     * that callback has been discarded, otherwise it could answer with the
     * new session's transaction number. */
    if (s_mgmt_deferred_pending || Kbd_Macro_IsBusy()) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, mgmt->command, 10u);
        return;
    }
    if (mgmt->fragment == 0u) {
        if (s_mgmt_rx.active) {
            /* Receiver repeats each request fragment. A duplicate first
             * fragment is expected while the previous copy is in flight. */
            if (mgmt->transaction == s_mgmt_rx.transaction &&
                mgmt->command == s_mgmt_rx.command && mgmt->sub == s_mgmt_rx.sub) {
                s_mgmt_ack = (kbd_radio_mgmt_ack_t){ KBD_RADIO_MGMT_VERSION, mgmt->transaction,
                    KBD_RADIO_FRAME_MGMT_REQUEST, mgmt->fragment };
                s_mgmt_ack_pending = true;
                return;
            }
            KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, mgmt->command, 7u);
            return;
        }
        memset(&s_mgmt_rx, 0, sizeof(s_mgmt_rx));
        s_mgmt_rx.active = true;
        s_mgmt_rx.transaction = mgmt->transaction;
        s_mgmt_rx.command = mgmt->command;
        s_mgmt_rx.sub = mgmt->sub;
        s_mgmt_rx.fragments = mgmt->fragments;
        s_mgmt_started = RTC_GetCycle32k();
    }
    if (!s_mgmt_rx.active || mgmt->transaction != s_mgmt_rx.transaction ||
        mgmt->command != s_mgmt_rx.command || mgmt->sub != s_mgmt_rx.sub ||
        mgmt->fragments != s_mgmt_rx.fragments || mgmt->fragment != s_mgmt_rx.next_fragment) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, mgmt->command, 8u);
        return;
    }
    if ((uint16_t)(s_mgmt_rx.length + mgmt->length) > sizeof(((kbd_cmd_frame_t *)0)->data)) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, mgmt->command, 4);
        return;
    }
    memcpy(&s_mgmt_rx.data[s_mgmt_rx.length], mgmt->data, mgmt->length);
    s_mgmt_rx.length += mgmt->length;
    s_mgmt_rx.next_fragment++;
    if (s_mgmt_rx.next_fragment == s_mgmt_rx.fragments) {
        /* The command response is the acknowledgement for the final request
         * fragment. Sending a standalone ACK and then the response in two
         * adjacent RFBound device packets is unreliable: the host often sees
         * the ACK (and advances TX to 1/1) but never receives the response.
         * Intermediate fragments still need an explicit ACK so the receiver
         * can advance a multi-fragment write request. */
        kbd_cmd_frame_t command = {0};
        if (s_mgmt_rx.length < 3u || s_mgmt_rx.data[2] > 61u ||
            (uint16_t)s_mgmt_rx.data[2] + 3u > s_mgmt_rx.length) {
            s_mgmt_rx.active = false;
            return;
        }
        command.cmd = s_mgmt_rx.data[0]; command.sub = s_mgmt_rx.data[1]; command.len = s_mgmt_rx.data[2];
        memcpy(command.data, &s_mgmt_rx.data[3], command.len);
        /* The response cache belongs to the previous completed transaction.
         * Invalidate it before publishing the new transaction as the latest
         * request. Otherwise a repeated final request fragment can replay an
         * unrelated response while an asynchronous Flash operation is still
         * pending, and the receiver correctly rejects the command mismatch. */
        s_mgmt_last_response_len = 0u;
        memset(s_mgmt_last_response, 0, sizeof(s_mgmt_last_response));
        s_mgmt_last_request_valid = true;
        s_mgmt_last_transaction = s_mgmt_rx.transaction;
        s_mgmt_last_command = s_mgmt_rx.command;
        s_mgmt_last_sub = s_mgmt_rx.sub;
        s_mgmt_last_completed = RTC_GetCycle32k();
        s_mgmt_rx.active = false;
        s_mgmt_command_pending = true;
        if (s_mgmt_exec_count != 0xFFFFu) s_mgmt_exec_count++;
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_EXEC, command.cmd, command.len);
    } else {
        s_mgmt_ack = (kbd_radio_mgmt_ack_t){ KBD_RADIO_MGMT_VERSION, mgmt->transaction,
            KBD_RADIO_FRAME_MGMT_REQUEST, mgmt->fragment };
        s_mgmt_ack_pending = true;
    }
}

static void mgmt_process_pending_command(void)
{
    if (!s_mgmt_command_pending || s_mgmt_rx.active || s_mgmt_tx.active) return;
    kbd_cmd_frame_t command = {0};
    command.cmd = s_mgmt_rx.data[0];
    command.sub = s_mgmt_rx.data[1];
    command.len = s_mgmt_rx.data[2];
    memcpy(command.data, &s_mgmt_rx.data[3], command.len);
    s_mgmt_command_pending = false;
    s_mgmt_response_transaction = s_mgmt_rx.transaction;
    s_mgmt_response_expected = true;
    KBD_Command_SetResponseSender(mgmt_send_response);
    KBD_Command_Process(&command);
    KBD_Command_SetResponseSender(NULL);
    if (s_mgmt_response_expected) {
        if (Kbd_Macro_IsBusy()) {
            s_mgmt_deferred_pending = true;
        } else {
            /* Every synchronous command must respond while the override is
             * installed. Do not leave an orphaned transaction blocking RF. */
            s_mgmt_response_expected = false;
        }
    }
}

static void mgmt_expire(void)
{
    if (!s_mgmt_rx.active && !s_mgmt_command_pending &&
        !s_mgmt_deferred_pending && !s_mgmt_tx.active) return;
    if (rf_rtc_elapsed(RTC_GetCycle32k(), s_mgmt_started) < KBD_MGMT_TIMEOUT_TICKS) return;
    memset(&s_mgmt_rx, 0, sizeof(s_mgmt_rx));
    memset(&s_mgmt_tx, 0, sizeof(s_mgmt_tx));
    s_mgmt_command_pending = false;
    s_mgmt_deferred_pending = false;
    s_mgmt_response_expected = false;
    s_mgmt_last_request_valid = false;
    s_mgmt_last_response_len = 0u;
    memset(s_mgmt_last_response, 0, sizeof(s_mgmt_last_response));
}

static void mgmt_process_rx(void)
{
    uint8_t seen = 0u;
    while ((pDMARxGet->Status & STA_DMA_ENABLE) == 0u && seen++ < 8u) {
        uint16_t raw = (uint16_t)(pDMARxGet->Status & STA_LEN_MASK);
        if (raw >= PKT_HEAD_LEN) {
            uint16_t len = (uint16_t)(raw - PKT_HEAD_LEN);
            const kbd_radio_frame_t *frame = (const kbd_radio_frame_t *)(pDMARxGet->BufferAddr + PKT_HEAD_LEN);
            if (len <= sizeof(kbd_radio_frame_t) && KBD_RadioProtocol_Validate(frame, len)) {
                if (frame->header.type == KBD_RADIO_FRAME_MGMT_REQUEST) mgmt_process_frame(frame);
                else if (frame->header.type == KBD_RADIO_FRAME_MGMT_ACK &&
                         frame->header.length == sizeof(kbd_radio_mgmt_ack_t)) {
                    const kbd_radio_mgmt_ack_t *ack = (const kbd_radio_mgmt_ack_t *)frame->payload;
                    if (ack->version == KBD_RADIO_MGMT_VERSION && s_mgmt_tx.active &&
                        ack->transaction == s_mgmt_tx.transaction &&
                        ack->frame_type == KBD_RADIO_FRAME_MGMT_RESPONSE &&
                        ack->fragment == s_mgmt_tx.fragment) {
                        s_mgmt_tx.waiting_ack = false;
                        s_mgmt_tx.repeat = 0u;
                        s_mgmt_tx.last_send = 0u;
                        if (++s_mgmt_tx.fragment >= s_mgmt_tx.fragments) s_mgmt_tx.active = false;
                    }
                }
            }
        }
        pDMARxGet->Status = STA_DMA_ENABLE;
        pDMARxGet = (RF_DMADESCTypeDef *)pDMARxGet->NextDescAddr;
    }
}

static int rf_start(bool pairing);
static bool rf_has_peer(void);

static void kbd_tmos_enable_irq(void)
{
    PFIC_EnableIRQ(BLEL_IRQn);
    PFIC_EnableIRQ(RTC_IRQn);
}

static void kbd_tmos_disable_irq(void)
{
    PFIC_DisableIRQ(BLEL_IRQn);
    PFIC_DisableIRQ(RTC_IRQn);
}

void KBD_Radio2G4_TmosInit(void)
{
    tmosConfig_t config;
    memset(&config, 0, sizeof(config));
    config.MEMAddr = (uint32_t)s_tmos_memory;
    config.MEMLen = sizeof(s_tmos_memory);
    config.TaskMaxCount = 16u;
    config.enableTmosIrq = kbd_tmos_enable_irq;
    config.disableTmosIrq = kbd_tmos_disable_irq;
    TMOS_Init(&config);
}

static uint32_t rf_rtc_elapsed(uint32_t now, uint32_t then)
{
    return now >= then ? now - then : (RTC_MAX_COUNT - then) + now;
}

static bool rf_has_peer(void)
{
    for (uint8_t i = 0; i < sizeof(s_peer_id); i++) if (s_peer_id[i]) return true;
    return false;
}

static void rf_nv_load(void)
{
    kbd_rf_nv_t nv;
    EEPROM_READ(KBD_RF_BIND_ADDR, &nv, sizeof(nv));
    uint16_t crc = KBD_RadioProtocol_Crc16((const uint8_t *)&nv,
                                           (uint16_t)(sizeof(nv) - sizeof(nv.crc)));
    if (nv.magic == KBD_RF_BIND_MAGIC && nv.crc == crc && nv.device_id <= 6u) {
        memcpy(s_peer_id, nv.peer, sizeof(s_peer_id));
        s_device_id = nv.device_id;
        s_state = KBD_RADIO_PAIR_BOUND;
    }
}

static int rf_nv_save(void)
{
    kbd_rf_nv_t nv = {0};
    nv.magic = KBD_RF_BIND_MAGIC;
    memcpy(nv.peer, s_peer_id, sizeof(nv.peer));
    nv.device_id = s_device_id;
    nv.crc = KBD_RadioProtocol_Crc16((const uint8_t *)&nv,
                                     (uint16_t)(sizeof(nv) - sizeof(nv.crc)));
    if (EEPROM_ERASE(KBD_RF_BIND_ADDR, EEPROM_PAGE_SIZE) != 0) return -1;
    return EEPROM_WRITE(KBD_RF_BIND_ADDR, &nv, sizeof(nv)) == 0 ? 0 : -1;
}

static void rf_bound_cb(staBound_t *status)
{
    if (status->status == SUCCESS) {
        bool binding_changed = s_device_id != status->devId ||
                               memcmp(s_peer_id, status->PeerInfo, sizeof(s_peer_id)) != 0;
        /* RFBound also reports SUCCESS after recovering the same persisted
         * peer. Preserve an in-flight management transaction in that case;
         * its own timeout and transaction number remain authoritative. A
         * manual pairing or a changed peer starts a genuinely new session. */
        if (s_manual_pairing || binding_changed)
            s_mgmt_session_reset_pending = true;
        s_device_id = status->devId;
        memcpy(s_peer_id, status->PeerInfo, sizeof(s_peer_id));
        /* WCH's reference device persists only a changed device ID or peer.
         * Automatic reconnect must not erase and rewrite DataFlash. */
        if (binding_changed) rf_nv_save();
        s_pair_backup_valid = false;
        /* SUCCESS commits the explicit pairing transaction. Any later
         * bleTimeout belongs to normal link supervision/reconnect and must
         * not put the keyboard back into the manual pairing window, where
         * application keepalives are intentionally paused. */
        s_manual_pairing = false;
        s_rf_ready = true;
        s_disconnect_pending = false;
        s_rf_restart_pending = false;
        s_failure_indicated = false;
        s_indicator_event = 2u;
        /* Match WCH's reference device state machine: application traffic is
         * allowed only after RFBound reports SUCCESS. BOUND means a saved
         * peer exists but the RF session is not ready yet. */
        s_state = KBD_RADIO_PAIR_CONNECTED;
    } else if (status->status == bleTimeout) {
        /* bleTimeout is RFBound's internal recovery transition. Flash erase
         * can legitimately make the application silent for about 100 ms;
         * keep management state until recovery or its independent 3 s
         * timeout instead of turning that pause into ERR_INVALID. */
        /* WCH documents bleTimeout as an internal reconnect/bindable
         * transition. Only an explicit pairing session owns the pairing
         * window; an already-bound keyboard remains bound here. */
        s_rf_ready = false;
        if (s_manual_pairing) s_state = KBD_RADIO_PAIRING;
        else {
            /* WCH RFBound owns this reconnect transition. Its reference
             * device does not stop or restart the role on bleTimeout. */
            if (!s_disconnect_pending) {
                s_disconnect_pending = true;
                s_disconnect_started = RTC_GetCycle32k();
            }
        }
    } else {
        s_mgmt_session_reset_pending = true;
        s_rf_ready = false;
        s_disconnect_pending = false;
        if (s_pair_backup_valid) {
            memcpy(s_peer_id, s_pair_backup_peer, sizeof(s_peer_id));
            s_device_id = s_pair_backup_device_id;
            s_pair_backup_valid = false;
        }
        /* Match WCH rf_device.c: FAILURE re-posts RF_START_BOUND_EVENT.
         * The callback runs in RF context, so start from the main loop. */
        s_rf_restart_pending = rf_has_peer();
        s_state = rf_has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        if (!s_failure_indicated) {
            s_failure_indicated = true;
            s_indicator_event = 1u;
        }
    }
}

static void rf_irq_cb(rfRole_States_t status, uint8_t id)
{
    (void)id;
    /* RFBound signals received DMA data through RF_STATE_RX. The reference
     * application posts a receive event from this callback; keep the actual
     * frame parsing in the keyboard main loop, outside RF interrupt context. */
    if ((status & RF_STATE_RX) != 0u) s_rf_rx_pending = true;
    /* TX_FINISH only means the DMA/RF engine completed a transmission. It
     * does not prove that the receiver accepted an application HID frame;
     * keep the keyboard in BOUND until the pairing/link state is confirmed. */
    if ((status & RF_STATE_TX_FINISH) != 0u) s_tx_finished++;
}

static int rf_start(bool pairing)
{
    rfBoundDevice_t bound;
    memset(&bound, 0, sizeof(bound));
    bound.devType = KBD_RF_DEVICE_TYPE;
    bound.deviceId = s_device_id;
    bound.speed = 12;
    /* Permit scheduling and channel-hop jitter; valid frames are supervised
     * independently by the receiver's application-level watchdog. */
    /* Match WCH's CH592 reference device (rf_device.c). */
    bound.timeout = 150;
    memcpy(bound.OwnInfo, s_local_id, sizeof(s_local_id));
    memcpy(bound.PeerInfo, s_peer_id, sizeof(s_peer_id));
    bound.rfBoundCB = rf_bound_cb;
    s_manual_pairing = pairing;
    s_rf_ready = false;
    s_disconnect_pending = false;
    s_state = pairing ? KBD_RADIO_PAIRING : KBD_RADIO_PAIR_BOUND;
    if (pairing) s_pair_started = RTC_GetCycle32k();
    return RFBound_StartDevice(&bound) == SUCCESS ? 0 : -1;
}

static int rf_send(const uint8_t *data, uint8_t len)
{
    if (!s_rf_ready) {
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, KBD_CMD_RADIO_REMOTE_CAPS, 5);
        return -1;
    }
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    /* The management response is serialized, so only the descriptor being
     * filled must be free. A permanently armed look-ahead descriptor in
     * RFBound must not starve the response queue. */
    if ((pDMATxGet->Status & STA_DMA_ENABLE) != 0u) {
        s_tx_busy++;
        SYS_RecoverIrq(irq);
        return -1;
    }
    rfPackage_t *packet = (rfPackage_t *)pDMATxGet->BufferAddr;
    packet->type = s_device_id;
    packet->length = (uint8_t)(PKT_ACK_LEN + len);
    memcpy((uint8_t *)pDMATxGet->BufferAddr + PKT_HEAD_LEN, data, len);
    pDMATxGet->BufferSize = (uint32_t)(PKT_HEAD_LEN + len);
    pDMATxGet->Status = STA_DMA_ENABLE;
    pDMATxGet = (RF_DMADESCTypeDef *)pDMATxGet->NextDescAddr;
    s_tx_enqueued++;
    s_last_tx = RTC_GetCycle32k();
    SYS_RecoverIrq(irq);
    /* DMA acceptance is not peer acknowledgement. The keyboard remains
     * BOUND; only the receiver can claim an application-confirmed link. */
    return 0;
}

static int rf_send_management(const uint8_t *data, uint8_t len)
{
    if (!s_rf_ready || !data || len == 0u || len > RF_TX_BUF_SZE) return -1;
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    if ((pDMATxGet->Status & STA_DMA_ENABLE) != 0u) {
        s_tx_busy++;
        SYS_RecoverIrq(irq);
        return -1;
    }
    rfPackage_t *packet = (rfPackage_t *)pDMATxGet->BufferAddr;
    packet->type = s_device_id;
    packet->length = (uint8_t)(PKT_ACK_LEN + len);
    memcpy((uint8_t *)pDMATxGet->BufferAddr + PKT_HEAD_LEN, data, len);
    pDMATxGet->BufferSize = (uint32_t)(PKT_HEAD_LEN + len);
    pDMATxGet->Status = STA_DMA_ENABLE;
    pDMATxGet = (RF_DMADESCTypeDef *)pDMATxGet->NextDescAddr;
    s_tx_enqueued++;
    s_last_tx = RTC_GetCycle32k();
    SYS_RecoverIrq(irq);
    return 0;
}

bool KBD_Radio2G4_IsEnabled(void) { return true; }
int KBD_Radio2G4_Init(void)
{
    if (s_initialized) return 0;
    GetMACAddress(s_local_id);
    s_session = RTC_GetCycle32k() ^ ((uint32_t)s_local_id[2] << 24) ^
                ((uint32_t)s_local_id[3] << 16) ^
                ((uint32_t)s_local_id[4] << 8) ^ s_local_id[5];
    if (s_session == 0u) s_session = 1u;
    rf_nv_load();
    s_rf_rx_pending = false;
    s_last_capability = 0u;
    memset(s_keyboard_snapshot, 0, sizeof(s_keyboard_snapshot));
    s_keyboard_snapshot_held = false;
    s_last_keyboard_tx = 0u;
    WS2812_Init();
    WS2812_SetIndicatorBrightness(48);
    RF_LibInit(rf_irq_cb);
    s_initialized = true;
    // An unbound keyboard must not enter a short-lived pairing attempt on
    // every power-up. Wait for the explicit web pairing command instead; a
    // previously bound keyboard may reconnect automatically.
    if (rf_has_peer()) {
        return rf_start(false);
    }
    s_state = KBD_RADIO_PAIR_UNBOUND;
    return 0;
}
void KBD_Radio2G4_Stop(void)
{
    if (s_initialized) RFRole_Shut();
    KBD_RGB_CancelFlash();
    s_initialized = false;
    s_manual_pairing = false;
    s_rf_restart_pending = false;
    s_disconnect_pending = false;
    s_rf_rx_pending = false;
    s_last_capability = 0u;
    memset(s_keyboard_snapshot, 0, sizeof(s_keyboard_snapshot));
    s_keyboard_snapshot_held = false;
    s_last_keyboard_tx = 0u;
    s_indicator_event = 0u;
    s_failure_indicated = false;
    s_failure_flash_active = false;
    s_rf_ready = false;
}
int KBD_Radio2G4_StartPairing(void)
{
    if (!s_initialized) return -1;
    RFRole_Shut();
    KBD_RGB_CancelFlash();
    s_disconnect_pending = false;
    s_rf_restart_pending = false;
    s_indicator_event = 0u;
    s_failure_indicated = false;
    s_failure_flash_active = false;
    s_manual_pairing = true;
    memcpy(s_pair_backup_peer, s_peer_id, sizeof(s_peer_id));
    s_pair_backup_device_id = s_device_id;
    s_pair_backup_valid = rf_has_peer();
    memset(s_peer_id, 0, sizeof(s_peer_id));
    s_device_id = KBD_RF_KEYBOARD_ID;
    int result = rf_start(true);
    if (result != 0) {
        if (s_pair_backup_valid) {
            memcpy(s_peer_id, s_pair_backup_peer, sizeof(s_peer_id));
            s_device_id = s_pair_backup_device_id;
            s_pair_backup_valid = false;
            s_state = KBD_RADIO_PAIR_BOUND;
            rf_start(false);
        } else {
            s_state = KBD_RADIO_PAIR_UNBOUND;
        }
    }
    return result;
}
int KBD_Radio2G4_CancelPairing(void)
{
    if (!s_initialized || s_state != KBD_RADIO_PAIRING) return -1;
    RFRole_Shut();
    KBD_RGB_CancelFlash();
    s_manual_pairing = false;
    s_rf_restart_pending = false;
    s_disconnect_pending = false;
    s_indicator_event = 0u;
    s_failure_indicated = false;
    s_failure_flash_active = false;
    if (s_pair_backup_valid) {
        memcpy(s_peer_id, s_pair_backup_peer, sizeof(s_peer_id));
        s_device_id = s_pair_backup_device_id;
        s_pair_backup_valid = false;
    } else {
        memset(s_peer_id, 0, sizeof(s_peer_id));
        s_device_id = KBD_RF_KEYBOARD_ID;
    }
    s_state = KBD_RADIO_PAIR_UNBOUND;
    return rf_has_peer() ? rf_start(false) : 0;
}
int KBD_Radio2G4_ClearPairing(bool force)
{
    (void)force;
    if (!s_initialized) return -1;
    RFRole_Shut(); RFRole_ClearTxData(s_device_id);
    KBD_RGB_CancelFlash();
    s_manual_pairing = false;
    s_pair_backup_valid = false;
    s_rf_restart_pending = false;
    s_disconnect_pending = false;
    s_indicator_event = 0u;
    s_failure_indicated = false;
    s_failure_flash_active = false;
    s_rf_ready = false;
    if (EEPROM_ERASE(KBD_RF_BIND_ADDR, EEPROM_PAGE_SIZE) != 0) return -1;
    memset(s_peer_id, 0, sizeof(s_peer_id)); s_device_id = KBD_RF_KEYBOARD_ID; s_state = KBD_RADIO_PAIR_UNBOUND;
    return 0;
}
kbd_radio_pair_state_t KBD_Radio2G4_GetPairState(void) { return s_state; }
uint8_t KBD_Radio2G4_GetDeviceId(void) { return s_device_id; }
bool KBD_Radio2G4_HasPeer(void) { return rf_has_peer(); }
void KBD_Radio2G4_GetLocalId(uint8_t out[6]) { if (out) memcpy(out, s_local_id, 6); }
void KBD_Radio2G4_GetPeerId(uint8_t out[6]) { if (out) memcpy(out, s_peer_id, 6); }
uint32_t KBD_Radio2G4_GetPairFingerprint(void)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < 6; i++) { h ^= s_local_id[i]; h *= 16777619u; }
    for (uint8_t i = 0; i < 6; i++) { h ^= s_peer_id[i]; h *= 16777619u; }
    h ^= s_device_id; h *= 16777619u;
    return rf_has_peer() ? h : 0u;
}
uint32_t KBD_Radio2G4_GetSession(void) { return s_session; }
uint32_t KBD_Radio2G4_GetTxEnqueued(void) { return s_tx_enqueued; }
uint32_t KBD_Radio2G4_GetTxBusy(void) { return s_tx_busy; }
uint32_t KBD_Radio2G4_GetTxFinished(void) { return s_tx_finished; }
uint32_t KBD_Radio2G4_GetLastTxAge(void)
{
    return s_last_tx ? rf_rtc_elapsed(RTC_GetCycle32k(), s_last_tx) : 0xFFFFFFFFu;
}
uint8_t KBD_Radio2G4_GetTxDescriptorsBusy(void)
{
    uint8_t count = 0u;
    for (uint8_t i = 0; i < RF_TXBUFNB; i++) {
        if ((DMATxDscrTab[i].Status & STA_DMA_ENABLE) != 0u) count++;
    }
    return count;
}
int KBD_Radio2G4_SendKeyboardReport(uint8_t modifier, const uint8_t *keys, uint8_t count)
{
    uint8_t report[8] = { modifier, 0, 0, 0, 0, 0, 0, 0 };
    if (count > 6) count = 6;
    if (keys && count) memcpy(&report[2], keys, count);
    memcpy(s_keyboard_snapshot, report, sizeof(report));
    s_keyboard_snapshot_held = modifier != 0u || count != 0u;
    if (s_state != KBD_RADIO_PAIR_CONNECTED) return -1;
    kbd_radio_frame_t frame;
    uint16_t len = KBD_RadioProtocol_Encode(&frame, KBD_RADIO_FRAME_KEYBOARD,
                                            s_session, ++s_sequence, report, sizeof(report));
    int result = len && len <= 64u
                     ? rf_send((const uint8_t *)&frame, (uint8_t)len)
                     : -1;
    if (result == 0) s_last_keyboard_tx = RTC_GetCycle32k();
    return result;
}
static int rf_send_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
    if (s_state != KBD_RADIO_PAIR_CONNECTED) return -1;
    kbd_radio_frame_t frame;
    uint16_t len = KBD_RadioProtocol_Encode(&frame, type, s_session, ++s_sequence, payload, payload_len);
    return len && len <= 64u ? rf_send((const uint8_t *)&frame, (uint8_t)len) : -1;
}
void KBD_Radio2G4_GetManagementDiagnostics(uint8_t out[13])
{
    if (!out) return;
    out[0] = (s_mgmt_rx.active ? 0x01u : 0u) |
             (s_mgmt_command_pending ? 0x02u : 0u) |
             (s_mgmt_tx.active ? 0x04u : 0u) |
             (s_mgmt_ack_pending ? 0x08u : 0u) |
             (s_mgmt_tx.waiting_ack ? 0x10u : 0u);
    out[1] = s_mgmt_rx.transaction;
    out[2] = s_mgmt_rx.command;
    out[3] = s_mgmt_rx.next_fragment;
    out[4] = s_mgmt_rx.fragments;
    out[5] = s_mgmt_tx.fragment;
    out[6] = s_mgmt_tx.fragments;
    out[7] = (uint8_t)s_mgmt_rx_count;
    out[8] = (uint8_t)(s_mgmt_rx_count >> 8);
    out[9] = (uint8_t)s_mgmt_exec_count;
    out[10] = (uint8_t)(s_mgmt_exec_count >> 8);
    out[11] = (uint8_t)s_mgmt_response_tx_count;
    out[12] = (uint8_t)(s_mgmt_response_tx_count >> 8);
}

static void send_capability_announcement(void)
{
    uint8_t payload[8] = {
        1u, KBD_VERSION_MAJOR, KBD_VERSION_MINOR, KBD_VERSION_PATCH,
        (uint8_t)KBD_GetType(), KBD_GetTotalKeyCount(), KBD_GetDefaultLayers(),
        (uint8_t)((KBD_FN_NUM_KEYS ? 0x01u : 0u) | (KBD_RADIO_2G4_ENABLED ? 0x02u : 0u)),
    };
    /* Back off after every attempt. Leaving the timestamp unchanged on a busy
     * descriptor retries from every main-loop turn and can starve the newer
     * keyboard snapshot, including the all-zero key-release report. */
    s_last_capability = RTC_GetCycle32k();
    int result = rf_send_frame(KBD_RADIO_FRAME_CAPABILITY, payload, sizeof(payload));
    if (result != 0)
        KBD_Log_RadioMgmtEvent(KBD_LOG_SYS_RF_MGMT_DROP, KBD_RADIO_FRAME_CAPABILITY,
                               (uint8_t)(result < 0 ? 0xFFu : result));
}
int KBD_Radio2G4_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    uint8_t report[4] = { buttons, (uint8_t)x, (uint8_t)y, (uint8_t)wheel };
    return rf_send_frame(KBD_RADIO_FRAME_MOUSE, report, sizeof(report));
}
int KBD_Radio2G4_SendConsumerReport(uint16_t key)
{
    uint8_t report[2] = { (uint8_t)key, (uint8_t)(key >> 8) };
    return rf_send_frame(KBD_RADIO_FRAME_CONSUMER, report, sizeof(report));
}
void KBD_Radio2G4_Process(void)
{
    if (s_mgmt_session_reset_pending) {
        uint32_t irq;
        SYS_DisableAllIrq(&irq);
        s_mgmt_session_reset_pending = false;
        SYS_RecoverIrq(irq);
        mgmt_reset_session();
    }
    /* Read management RX first, then prioritize its ACK. This keeps a
     * previous response from blocking execution of the next browser command. */
    s_rf_rx_pending = false;
    mgmt_process_rx();
    bool mgmt_ack_submitted = false;
    if (s_mgmt_ack_pending) {
        kbd_radio_frame_t ack_frame;
        uint16_t ack_len = KBD_RadioProtocol_Encode(&ack_frame, KBD_RADIO_FRAME_MGMT_ACK,
            s_session, ++s_sequence, (const uint8_t *)&s_mgmt_ack, sizeof(s_mgmt_ack));
        if (ack_len && rf_send_management((const uint8_t *)&ack_frame, (uint8_t)ack_len) == 0) {
            s_mgmt_ack_pending = false;
            mgmt_ack_submitted = true;
        }
    }
    mgmt_process_pending_command();
    if (!mgmt_ack_submitted) mgmt_send_next_response();
    mgmt_expire();
    /* Capability traffic is periodic and expendable. It must not occupy the
     * descriptor needed by an ACK or a command response. */
    if (!s_mgmt_ack_pending && !s_mgmt_rx.active && !s_mgmt_command_pending &&
        !s_mgmt_deferred_pending &&
        !s_mgmt_tx.active && s_rf_ready &&
        (s_last_capability == 0u ||
         rf_rtc_elapsed(RTC_GetCycle32k(), s_last_capability) >= KBD_RF_CAPABILITY_PERIOD_TICKS)) {
        send_capability_announcement();
    }
    /* Refresh only an actively held keyboard state. This gives the receiver
     * enough evidence to distinguish a real hold from a lost release without
     * generating continuous traffic while the keyboard is idle. */
    if (s_keyboard_snapshot_held && s_rf_ready &&
        !s_mgmt_ack_pending && !s_mgmt_rx.active && !s_mgmt_command_pending &&
        !s_mgmt_deferred_pending && !s_mgmt_tx.active &&
        (s_last_keyboard_tx == 0u ||
         rf_rtc_elapsed(RTC_GetCycle32k(), s_last_keyboard_tx) >=
             KBD_HID_HELD_REFRESH_TICKS)) {
        kbd_radio_frame_t frame;
        uint16_t len = KBD_RadioProtocol_Encode(
            &frame, KBD_RADIO_FRAME_KEYBOARD, s_session, ++s_sequence,
            s_keyboard_snapshot, sizeof(s_keyboard_snapshot));
        /* Record the attempt time even when the descriptor is busy. This
         * keeps bounded refresh traffic from degenerating into a tight loop. */
        s_last_keyboard_tx = RTC_GetCycle32k();
        (void)(len && len <= 64u
                   ? rf_send((const uint8_t *)&frame, (uint8_t)len)
                   : -1);
    }
    /* Do not write WS2812 here.  The RGB scheduler owns the strip and maps
     * the radio state to the dedicated indicator LED, preventing two
     * independent loops from fighting over LED0/LED1 during pairing. */
    uint8_t indicator_event = s_indicator_event;
    if (indicator_event != 0u) {
        s_indicator_event = 0u;
        if (indicator_event == 2u) {
            /* Clear both the logical overlay and the physical WS2812 value.
             * Do not depend on the RGB TMOS timer surviving RF recovery. */
            s_failure_flash_active = false;
            KBD_RGB_CancelFlash();
            KBD_RGB_SetState(KBD_STATE_BLE_CONNECTED);
            KBD_RGB_Process();
        } else {
            /* One short failure acknowledgement per outage. The persistent
             * reconnect indication remains the state-machine-owned amber. */
            KBD_RGB_SetState(rf_has_peer() ? KBD_STATE_2G4_BOUND
                                           : KBD_STATE_BLE_ADVERTISING);
            KBD_RGB_Flash(180, 0, 0, 300);
            s_failure_flash_active = true;
            s_failure_flash_started = RTC_GetCycle32k();
        }
    }
    if (s_failure_flash_active &&
        rf_rtc_elapsed(RTC_GetCycle32k(), s_failure_flash_started) >=
            KBD_RF_FAILURE_FLASH_TICKS) {
        s_failure_flash_active = false;
        KBD_RGB_CancelFlash();
        KBD_RGB_SetState(rf_has_peer() ? KBD_STATE_2G4_BOUND
                                       : KBD_STATE_BLE_ADVERTISING);
        KBD_RGB_Process();
    }
    if (s_state == KBD_RADIO_PAIRING) {
        if (rf_rtc_elapsed(RTC_GetCycle32k(), s_pair_started) >= KBD_RF_PAIR_WINDOW_TICKS) {
            RFRole_Shut();
            if (s_pair_backup_valid) {
                memcpy(s_peer_id, s_pair_backup_peer, sizeof(s_peer_id));
                s_device_id = s_pair_backup_device_id;
                s_pair_backup_valid = false;
                s_state = KBD_RADIO_PAIR_BOUND;
                rf_start(false);
            } else {
                s_state = KBD_RADIO_PAIR_UNBOUND;
            }
        }
        return;
    }
    if (s_rf_restart_pending) {
        s_rf_restart_pending = false;
        if (rf_has_peer()) rf_start(false);
    }
    if (s_disconnect_pending &&
        rf_rtc_elapsed(RTC_GetCycle32k(), s_disconnect_started) >=
            KBD_RF_DISCONNECT_GRACE_TICKS) {
        s_disconnect_pending = false;
        s_state = rf_has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
    }
    /* RFBound owns idle supervision. Absence of new application HID frames is
     * normal for a keyboard and must never be treated as a disconnected link. */
}

#else
bool KBD_Radio2G4_IsEnabled(void) { return false; }
int KBD_Radio2G4_Init(void) { return -1; }
void KBD_Radio2G4_Stop(void) {}
int KBD_Radio2G4_StartPairing(void) { return -1; }
int KBD_Radio2G4_CancelPairing(void) { return -1; }
int KBD_Radio2G4_ClearPairing(bool force) { (void)force; return -1; }
kbd_radio_pair_state_t KBD_Radio2G4_GetPairState(void) { return KBD_RADIO_PAIR_UNSUPPORTED; }
int KBD_Radio2G4_SendKeyboardReport(uint8_t modifier, const uint8_t *keys, uint8_t count) { (void)modifier; (void)keys; (void)count; return -1; }
int KBD_Radio2G4_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel) { (void)buttons; (void)x; (void)y; (void)wheel; return -1; }
int KBD_Radio2G4_SendConsumerReport(uint16_t key) { (void)key; return -1; }
void KBD_Radio2G4_Process(void) {}
uint8_t KBD_Radio2G4_GetDeviceId(void) { return 0; }
bool KBD_Radio2G4_HasPeer(void) { return false; }
void KBD_Radio2G4_GetLocalId(uint8_t out[6]) { if (out) memset(out, 0, 6); }
void KBD_Radio2G4_GetPeerId(uint8_t out[6]) { if (out) memset(out, 0, 6); }
uint32_t KBD_Radio2G4_GetPairFingerprint(void) { return 0; }
uint32_t KBD_Radio2G4_GetSession(void) { return 0; }
uint32_t KBD_Radio2G4_GetTxEnqueued(void) { return 0; }
uint32_t KBD_Radio2G4_GetTxBusy(void) { return 0; }
uint32_t KBD_Radio2G4_GetTxFinished(void) { return 0; }
uint32_t KBD_Radio2G4_GetLastTxAge(void) { return 0xFFFFFFFFu; }
uint8_t KBD_Radio2G4_GetTxDescriptorsBusy(void) { return 0; }
void KBD_Radio2G4_GetManagementDiagnostics(uint8_t out[13]) { if (out) memset(out, 0, 13); }
#endif
