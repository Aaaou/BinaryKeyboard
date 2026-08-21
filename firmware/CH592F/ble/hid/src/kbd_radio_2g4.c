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
/* Keep the application heartbeat comfortably below the RFBound transaction
 * timeout.  A 1 s interval was longer than the old 500 ms library window. */
#define KBD_RF_KEEPALIVE_TICKS 3277u /* about 100 ms at 32.768 kHz */
#define KBD_RF_PAIR_WINDOW_TICKS (60u * 32768u)

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
static uint32_t s_last_keepalive;
static bool s_initialized;
/* Pairing is transactional: keep the previous profile until the new
 * RFBound transaction has completed successfully.  This mirrors the
 * profile/bond behavior used by mature wireless keyboard stacks. */
static uint8_t s_pair_backup_peer[6];
static uint8_t s_pair_backup_device_id;
static bool s_pair_backup_valid;
static uint32_t s_pair_started;
static bool s_pair_restore_pending;
/* RFBound uses the same callback for an explicit bind and an automatic
 * reconnect. Keep those sessions distinct: timeout is recoverable during a
 * reconnect and must never open the 60 s pairing window. */
static bool s_manual_pairing;
static volatile uint32_t s_tx_enqueued;
static volatile uint32_t s_tx_busy;
static volatile uint32_t s_tx_finished;
static uint32_t s_last_tx;
extern RF_DMADESCTypeDef *pDMATxGet;
extern RF_DMADESCTypeDef DMATxDscrTab[RF_TXBUFNB];

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
        s_device_id = status->devId;
        memcpy(s_peer_id, status->PeerInfo, sizeof(s_peer_id));
        rf_nv_save();
        s_pair_backup_valid = false;
        /* SUCCESS commits the explicit pairing transaction. Any later
         * bleTimeout belongs to normal link supervision/reconnect and must
         * not put the keyboard back into the manual pairing window, where
         * application keepalives are intentionally paused. */
        s_manual_pairing = false;
        /* The WCH reference mouse transmits continuously, while an idle
         * keyboard may have no report ready inside the Host's 100 ms
         * supervision window. Queue a keepalive on the next main-loop turn
         * instead of waiting for a full heartbeat interval after SUCCESS. */
        uint32_t now = RTC_GetCycle32k();
        s_last_keepalive = now >= KBD_RF_KEEPALIVE_TICKS
            ? now - KBD_RF_KEEPALIVE_TICKS
            : RTC_MAX_COUNT - (KBD_RF_KEEPALIVE_TICKS - now);
        /* RFBound SUCCESS means the binding transaction completed. It does
         * not prove that an application HID frame has reached the receiver. */
        s_state = KBD_RADIO_PAIR_BOUND;
        KBD_RGB_Flash(180, 120, 0, 250);
    } else if (status->status == bleTimeout) {
        /* WCH documents bleTimeout as an internal reconnect/bindable
         * transition. Only an explicit pairing session owns the pairing
         * window; an already-bound keyboard remains bound here. */
        if (s_manual_pairing) s_state = KBD_RADIO_PAIRING;
        else s_state = rf_has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
    } else {
        if (s_pair_backup_valid) {
            memcpy(s_peer_id, s_pair_backup_peer, sizeof(s_peer_id));
            s_device_id = s_pair_backup_device_id;
            s_pair_backup_valid = false;
        }
        s_pair_restore_pending = true;
        s_state = rf_has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        KBD_RGB_Flash(180, 0, 0, 1500);
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
    s_state = pairing ? KBD_RADIO_PAIRING : KBD_RADIO_PAIR_BOUND;
    if (pairing) {
        s_pair_started = RTC_GetCycle32k();
        KBD_RGB_Flash(0, 0, 180, 60000);
    }
    return RFBound_StartDevice(&bound) == SUCCESS ? 0 : -1;
}

static int rf_send(const uint8_t *data, uint8_t len)
{
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    if (pDMATxGet->Status & STA_DMA_ENABLE) {
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
    s_last_keepalive = RTC_GetCycle32k();
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
    KBD_RGB_Flash(0, 0, 180, 400);
    return 0;
}
void KBD_Radio2G4_Stop(void) { if (s_initialized) RFRole_Shut(); s_initialized = false; }
int KBD_Radio2G4_StartPairing(void)
{
    if (!s_initialized) return -1;
    RFRole_Shut();
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
    s_manual_pairing = false;
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
    s_manual_pairing = false;
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
    if (s_state != KBD_RADIO_PAIR_CONNECTED && s_state != KBD_RADIO_PAIR_BOUND) return -1;
    if (count > 6) count = 6;
    if (keys && count) memcpy(&report[2], keys, count);
    kbd_radio_frame_t frame;
    uint16_t len = KBD_RadioProtocol_Encode(&frame, KBD_RADIO_FRAME_KEYBOARD,
                                            s_session, ++s_sequence, report, sizeof(report));
    return len && len <= 64u ? rf_send((const uint8_t *)&frame, (uint8_t)len) : -1;
}
static int rf_send_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
    if (s_state != KBD_RADIO_PAIR_CONNECTED && s_state != KBD_RADIO_PAIR_BOUND) return -1;
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
    if (s_pair_restore_pending) {
        s_pair_restore_pending = false;
        if (rf_has_peer()) rf_start(false);
    }
    /* A bound device may briefly lose the Host callback before the next
     * packet arrives. Continue keepalive in BOUND state so the RF library can
     * recover the session instead of waiting for a new pairing cycle. */
    if (s_state != KBD_RADIO_PAIR_CONNECTED && s_state != KBD_RADIO_PAIR_BOUND) return;
    uint32_t now = RTC_GetCycle32k();
    if (rf_rtc_elapsed(now, s_last_keepalive) < KBD_RF_KEEPALIVE_TICKS) return;
    if (rf_send_frame(KBD_RADIO_FRAME_KEEPALIVE, NULL, 0u) == 0) s_last_keepalive = now;
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
