#ifndef __KBD_RADIO_PROTOCOL_H
#define __KBD_RADIO_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#define KBD_RADIO_PROTOCOL_VERSION 1u
#define KBD_RADIO_ACCESS_ADDRESS 0x71763988u
#define KBD_RADIO_MAX_PAYLOAD 32u
#define KBD_RADIO_PAIR_WINDOW_MS 60000u
#define KBD_RADIO_MAGIC 0xB4u

typedef enum {
    KBD_RADIO_FRAME_PAIR_HELLO = 1,
    KBD_RADIO_FRAME_PAIR_CHALLENGE = 2,
    KBD_RADIO_FRAME_PAIR_COMMIT = 3,
    KBD_RADIO_FRAME_PAIR_ACK = 4,
    KBD_RADIO_FRAME_KEYBOARD = 5,
    KBD_RADIO_FRAME_UNPAIR_REQUEST = 6,
    KBD_RADIO_FRAME_UNPAIR_ACK = 7,
    /* Reserved from the rejected application-watchdog experiment. Current
     * firmware deliberately does not emit or supervise KEEPALIVE frames;
     * RFBound owns link liveness. Keep the value for wire compatibility only. */
    KBD_RADIO_FRAME_KEEPALIVE = 8,
    KBD_RADIO_FRAME_MOUSE = 9,
    KBD_RADIO_FRAME_CONSUMER = 10,
    /* Bidirectional management tunnel between receiver and keyboard. */
    KBD_RADIO_FRAME_MGMT_REQUEST = 11,
    KBD_RADIO_FRAME_MGMT_RESPONSE = 12,
    /* Periodic keyboard identity/capability announcement. Unlike HID input,
     * this frame is emitted while idle so the receiver can identify its RF
     * target without waiting for a key press. */
    KBD_RADIO_FRAME_CAPABILITY = 13,
    /* Stop-and-wait acknowledgement for one management fragment. */
    KBD_RADIO_FRAME_MGMT_ACK = 14,
} kbd_radio_frame_type_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t transaction;
    uint8_t frame_type;
    uint8_t fragment;
} kbd_radio_mgmt_ack_t;

#define KBD_RADIO_MGMT_VERSION 1u
#define KBD_RADIO_MGMT_MAX_DATA 20u

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t transaction;
    uint8_t command;
    uint8_t sub;
    uint8_t fragment;
    uint8_t fragments;
    uint8_t length;
    uint8_t flags;
    uint8_t data[KBD_RADIO_MGMT_MAX_DATA];
} kbd_radio_mgmt_packet_t;

enum {
    KBD_RADIO_MGMT_FLAG_FIRST = 0x01,
    KBD_RADIO_MGMT_FLAG_LAST = 0x02,
    KBD_RADIO_MGMT_FLAG_ERROR = 0x04,
};

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint32_t session;
    uint32_t sequence;
} kbd_radio_frame_header_t;

typedef struct __attribute__((packed)) {
    kbd_radio_frame_header_t header;
    uint8_t payload[KBD_RADIO_MAX_PAYLOAD];
    uint16_t crc16;
} kbd_radio_frame_t;

typedef struct {
    uint8_t local_id[6];
    uint8_t peer_id[6];
    uint32_t session;
    uint8_t generation;
    uint8_t valid;
} kbd_radio_binding_t;

uint16_t KBD_RadioProtocol_Crc16(const uint8_t *data, uint16_t len);
uint16_t KBD_RadioProtocol_Encode(kbd_radio_frame_t *frame, uint8_t type,
                                  uint32_t session, uint32_t sequence,
                                  const uint8_t *payload, uint16_t length);
bool KBD_RadioProtocol_Validate(const kbd_radio_frame_t *frame, uint16_t frame_len);

/* Encode/decode one management fragment. The returned length includes the
 * 8-byte management header and payload, but not the outer radio frame header/CRC. */
uint8_t KBD_RadioMgmt_Encode(kbd_radio_mgmt_packet_t *packet,
                             uint8_t transaction, uint8_t command, uint8_t sub,
                             uint8_t fragment, uint8_t fragments, uint8_t flags,
                             const uint8_t *data, uint8_t length);
bool KBD_RadioMgmt_Validate(const kbd_radio_mgmt_packet_t *packet, uint8_t length);

#endif
