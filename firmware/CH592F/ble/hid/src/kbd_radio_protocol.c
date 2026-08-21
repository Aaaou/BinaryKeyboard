#include "kbd_radio_protocol.h"
#include <string.h>

uint16_t KBD_RadioProtocol_Crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

uint16_t KBD_RadioProtocol_Encode(kbd_radio_frame_t *frame, uint8_t type,
                                  uint32_t session, uint32_t sequence,
                                  const uint8_t *payload, uint16_t length)
{
    if (!frame || length > KBD_RADIO_MAX_PAYLOAD) return 0;
    frame->header.magic = KBD_RADIO_MAGIC;
    frame->header.version = KBD_RADIO_PROTOCOL_VERSION;
    frame->header.type = type;
    frame->header.flags = 0;
    frame->header.length = length;
    frame->header.session = session;
    frame->header.sequence = sequence;
    for (uint16_t i = 0; i < length; i++) frame->payload[i] = payload ? payload[i] : 0;
    uint16_t crc_offset = (uint16_t)(sizeof(frame->header) + length);
    uint16_t crc = KBD_RadioProtocol_Crc16((const uint8_t *)frame, crc_offset);
    memcpy((uint8_t *)frame + crc_offset, &crc, sizeof(crc));
    return (uint16_t)(crc_offset + sizeof(crc));
}

bool KBD_RadioProtocol_Validate(const kbd_radio_frame_t *frame, uint16_t frame_len)
{
    if (!frame || frame_len < sizeof(frame->header) + sizeof(frame->crc16)) return false;
    if (frame->header.magic != KBD_RADIO_MAGIC ||
        frame->header.version != KBD_RADIO_PROTOCOL_VERSION ||
        frame->header.length > KBD_RADIO_MAX_PAYLOAD) return false;
    uint16_t expected = (uint16_t)(sizeof(frame->header) + frame->header.length + sizeof(frame->crc16));
    if (frame_len != expected) return false;
    uint16_t crc_offset = (uint16_t)(sizeof(frame->header) + frame->header.length);
    uint16_t received_crc;
    memcpy(&received_crc, (const uint8_t *)frame + crc_offset, sizeof(received_crc));
    return KBD_RadioProtocol_Crc16((const uint8_t *)frame, crc_offset) == received_crc;
}
