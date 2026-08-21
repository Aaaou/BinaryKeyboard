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
    KBD_RADIO_FRAME_KEEPALIVE = 8,
    KBD_RADIO_FRAME_MOUSE = 9,
    KBD_RADIO_FRAME_CONSUMER = 10,
} kbd_radio_frame_type_t;

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

#endif
