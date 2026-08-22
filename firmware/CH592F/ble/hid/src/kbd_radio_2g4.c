#include "kbd_radio_2g4.h"
#include "kbd_mode_config.h"

#if KBD_RADIO_2G4_ENABLED
#include "kbd_radio_protocol.h"
#include "CH59x_common.h"
#include "RF.h"
#include "kbd_rgb.h"
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

/*
 * RELEASE LIMITATION (intentional and documented):
 *
 * We tried three application-side ways to stop a held key after a receiver
 * disappears: periodic KEEPALIVE frames, a short application watchdog, and a
 * timer-driven RF role restart after bleTimeout.  They were removed because
 * they filled the WCH RFBound TX ring or interrupted RFBound's own recovery.
 * The WCH reference device has the same boundary: bleTimeout is an internal
 * recovery state and only FAILURE restarts the bound role.  Therefore this
 * firmware cannot guarantee a sub-second stop when the keyboard loses power
 * before sending a release report.  A host may continue repeating the last
 * HID state for about five seconds until RFBound reports a terminal failure
 * and the receiver's HID release path runs.  Do not describe this as fixed.
 */

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
/* RFBound callbacks run in RF context. Defer role restart work to the
 * keyboard main loop and expose only stable states to the RGB scheduler. */
static volatile uint8_t s_indicator_event;
static bool s_failure_indicated;
static bool s_failure_flash_active;
static uint32_t s_failure_flash_started;
static volatile bool s_rf_ready;
static volatile bool s_disconnect_pending;
static uint32_t s_disconnect_started;
extern RF_DMADESCTypeDef *pDMATxGet;
extern RF_DMADESCTypeDef DMATxDscrTab[RF_TXBUFNB];

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
    if (!s_rf_ready) return -1;
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    RF_DMADESCTypeDef *next = (RF_DMADESCTypeDef *)pDMATxGet->NextDescAddr;
    /* WCH's reference trans.c applies this exact capacity rule: the current
     * application descriptor and its successor must both be free. Keeping
     * the successor reserved prevents application HID/heartbeat traffic from
     * consuming the RF library's communication headroom. */
    if ((pDMATxGet->Status & STA_DMA_ENABLE) != 0u ||
        (next->Status & STA_DMA_ENABLE) != 0u) {
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
    if (s_state != KBD_RADIO_PAIR_CONNECTED) return -1;
    if (count > 6) count = 6;
    if (keys && count) memcpy(&report[2], keys, count);
    kbd_radio_frame_t frame;
    uint16_t len = KBD_RadioProtocol_Encode(&frame, KBD_RADIO_FRAME_KEYBOARD,
                                            s_session, ++s_sequence, report, sizeof(report));
    return len && len <= 64u ? rf_send((const uint8_t *)&frame, (uint8_t)len) : -1;
}
static int rf_send_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
    if (s_state != KBD_RADIO_PAIR_CONNECTED) return -1;
    kbd_radio_frame_t frame;
    uint16_t len = KBD_RadioProtocol_Encode(&frame, type, s_session, ++s_sequence, payload, payload_len);
    return len && len <= 64u ? rf_send((const uint8_t *)&frame, (uint8_t)len) : -1;
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
#endif
