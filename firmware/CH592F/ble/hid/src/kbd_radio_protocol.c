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

uint8_t KBD_RadioMgmt_Encode(kbd_radio_mgmt_packet_t *packet,
                             uint8_t transaction, uint8_t command, uint8_t sub,
                             uint8_t fragment, uint8_t fragments, uint8_t flags,
                             const uint8_t *data, uint8_t length)
{
    if (!packet || fragments == 0u || fragment >= fragments ||
        length > KBD_RADIO_MGMT_MAX_DATA) return 0u;
    memset(packet, 0, sizeof(*packet));
    packet->version = KBD_RADIO_MGMT_VERSION;
    packet->transaction = transaction;
    packet->command = command;
    packet->sub = sub;
    packet->fragment = fragment;
    packet->fragments = fragments;
    packet->length = length;
    packet->flags = flags;
    if (data && length) memcpy(packet->data, data, length);
    return (uint8_t)(8u + length);
}

bool KBD_RadioMgmt_Validate(const kbd_radio_mgmt_packet_t *packet, uint8_t length)
{
    if (!packet || length < 8u || length > (uint8_t)sizeof(*packet)) return false;
    if (packet->version != KBD_RADIO_MGMT_VERSION || packet->fragments == 0u ||
        packet->fragment >= packet->fragments ||
        packet->length > KBD_RADIO_MGMT_MAX_DATA ||
        packet->length != (uint8_t)(length - 8u)) return false;
    /* FIRST/LAST are structural flags, not hints.  Reject both missing and
     * misplaced flags so a malformed fragment cannot reserve a transaction
     * indefinitely on either endpoint. */
    const uint8_t first = (packet->fragment == 0u) ? KBD_RADIO_MGMT_FLAG_FIRST : 0u;
    const uint8_t last = (packet->fragment + 1u == packet->fragments) ? KBD_RADIO_MGMT_FLAG_LAST : 0u;
    if ((packet->flags & (KBD_RADIO_MGMT_FLAG_FIRST | KBD_RADIO_MGMT_FLAG_LAST)) !=
        (first | last)) return false;
    if ((packet->flags & (uint8_t)~(KBD_RADIO_MGMT_FLAG_FIRST |
                                    KBD_RADIO_MGMT_FLAG_LAST |
                                    KBD_RADIO_MGMT_FLAG_ERROR)) != 0u) return false;
    return true;
}
