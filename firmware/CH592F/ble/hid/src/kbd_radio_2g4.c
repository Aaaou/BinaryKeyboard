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
static uint32_t s_led_tick;
static bool s_led_phase;
extern RF_DMADESCTypeDef *pDMATxGet;

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
        /* RFBound SUCCESS means the binding transaction completed. It does
         * not prove that an application HID frame has reached the receiver. */
        s_state = KBD_RADIO_PAIR_BOUND;
        KBD_RGB_Flash(180, 120, 0, 250);
    } else if (status->status == bleTimeout) {
        /* RFBound timeout only denotes an internal transaction retry.  Do not
         * turn it into a visible link failure or stop application keepalive. */
        s_state = rf_has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        if (rf_has_peer()) KBD_RGB_Flash(180, 120, 0, 250);
    } else {
        s_state = rf_has_peer() ? KBD_RADIO_PAIR_BOUND : KBD_RADIO_PAIR_UNBOUND;
        KBD_RGB_Flash(180, 0, 0, 1500);
    }
}

static void rf_irq_cb(rfRole_States_t status, uint8_t id)
{
    (void)id;
    if (status & RF_STATE_TX_FINISH) {
        /* A completed RF transmission is the first reliable user-visible
         * indication of an active session. Keep it as a short green pulse. */
        s_state = KBD_RADIO_PAIR_CONNECTED;
        KBD_RGB_Flash(0, 180, 0, 180);
    }
}

static int rf_start(void)
{
    rfBoundDevice_t bound;
    memset(&bound, 0, sizeof(bound));
    bound.devType = KBD_RF_DEVICE_TYPE;
    bound.deviceId = s_device_id;
    bound.speed = 12;
    /* Permit scheduling and channel-hop jitter; valid frames are supervised
     * independently by the receiver's application-level watchdog. */
    bound.timeout = 1500;
    memcpy(bound.OwnInfo, s_local_id, sizeof(s_local_id));
    memcpy(bound.PeerInfo, s_peer_id, sizeof(s_peer_id));
    bound.rfBoundCB = rf_bound_cb;
    s_state = KBD_RADIO_PAIRING;
    KBD_RGB_Flash(0, 0, 180, 60000);
    return RFBound_StartDevice(&bound) == SUCCESS ? 0 : -1;
}

static int rf_send(const uint8_t *data, uint8_t len)
{
    uint32_t irq;
    SYS_DisableAllIrq(&irq);
    if (pDMATxGet->Status & STA_DMA_ENABLE) { SYS_RecoverIrq(irq); return -1; }
    rfPackage_t *packet = (rfPackage_t *)pDMATxGet->BufferAddr;
    packet->type = s_device_id;
    packet->length = (uint8_t)(PKT_ACK_LEN + len);
    memcpy((uint8_t *)pDMATxGet->BufferAddr + PKT_HEAD_LEN, data, len);
    pDMATxGet->BufferSize = (uint32_t)(PKT_HEAD_LEN + len);
    pDMATxGet->Status = STA_DMA_ENABLE;
    pDMATxGet = (RF_DMADESCTypeDef *)pDMATxGet->NextDescAddr;
    SYS_RecoverIrq(irq);
    /* Normal builds of the shared RF library do not expose TX_FINISH in the
     * process mask. Reaching this point means the packet has been accepted by
     * the RF DMA ring, so leave the stale BOUND display state here. The
     * receiver still independently confirms the link from valid frames. */
    s_state = KBD_RADIO_PAIR_CONNECTED;
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
        return rf_start();
    }
    s_state = KBD_RADIO_PAIR_UNBOUND;
    KBD_RGB_Flash(0, 0, 180, 400);
    return 0;
}
void KBD_Radio2G4_Stop(void) { if (s_initialized) RFRole_Shut(); s_initialized = false; }
int KBD_Radio2G4_StartPairing(void)
{
    if (!s_initialized) return -1;
    RFRole_Shut(); memset(s_peer_id, 0, sizeof(s_peer_id)); s_device_id = KBD_RF_KEYBOARD_ID;
    return rf_start();
}
int KBD_Radio2G4_CancelPairing(void)
{
    if (!s_initialized || s_state != KBD_RADIO_PAIRING) return -1;
    RFRole_Shut();
    memset(s_peer_id, 0, sizeof(s_peer_id));
    s_device_id = KBD_RF_KEYBOARD_ID;
    s_state = KBD_RADIO_PAIR_UNBOUND;
    rf_nv_load();
    return rf_has_peer() ? rf_start() : 0;
}
int KBD_Radio2G4_ClearPairing(bool force)
{
    (void)force;
    if (!s_initialized) return -1;
    RFRole_Shut(); RFRole_ClearTxData(s_device_id);
    if (EEPROM_ERASE(KBD_RF_BIND_ADDR, EEPROM_PAGE_SIZE) != 0) return -1;
    memset(s_peer_id, 0, sizeof(s_peer_id)); s_device_id = KBD_RF_KEYBOARD_ID; s_state = KBD_RADIO_PAIR_UNBOUND;
    return 0;
}
kbd_radio_pair_state_t KBD_Radio2G4_GetPairState(void) { return s_state; }
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
    if (s_state == KBD_RADIO_PAIRING) {
        uint32_t now = RTC_GetCycle32k();
        if ((uint32_t)(now - s_led_tick) >= 16384u) {
            s_led_tick = now;
            s_led_phase = !s_led_phase;
            WS2812_Set_Indicator(s_led_phase ? 0 : 0,
                                 s_led_phase ? 0 : 0,
                                 s_led_phase ? 180 : 0);
            WS2812_Update();
        }
        return;
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
#endif
