#include "aether_packet.h"
#include <string.h>
#include <time.h>

/* CRC-16/CCITT-FALSE */
uint16_t aether_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* CRC-32/ISO-HDLC */
uint32_t aether_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return crc ^ 0xFFFFFFFF;
}

uint64_t aether_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/* Serialize packet to byte buffer for sending over RFCOMM.
   Returns total bytes written, or -1 on error. */
int aether_packet_pack(const AetherPacket *pkt, uint8_t *buf, size_t buf_size) {
    size_t total = AETHER_HEADER_SIZE + pkt->hdr.payload_size + 4;
    if (buf_size < total) return -1;

    /* Copy header (compute CRC on all fields except header_crc16 itself) */
    AetherHeader hdr = pkt->hdr;
    hdr.header_crc16 = 0;
    memcpy(buf, &hdr, AETHER_HEADER_SIZE - 2);  // everything before crc16
    uint16_t hcrc = aether_crc16(buf, AETHER_HEADER_SIZE - 2);
    hdr.header_crc16 = hcrc;
    memcpy(buf, &hdr, AETHER_HEADER_SIZE);

    /* Copy payload */
    memcpy(buf + AETHER_HEADER_SIZE, pkt->payload, pkt->hdr.payload_size);

    /* Append payload CRC-32 */
    uint32_t pcrc = aether_crc32(pkt->payload, pkt->hdr.payload_size);
    memcpy(buf + AETHER_HEADER_SIZE + pkt->hdr.payload_size, &pcrc, 4);

    return (int)total;
}

/* Deserialize buffer into AetherPacket.
   Returns 0 on success, -1 on bad magic/short buffer,
   -2 on bad header CRC, -3 on bad payload CRC. */
int aether_packet_unpack(const uint8_t *buf, size_t buf_len, AetherPacket *out) {
    if (buf_len < AETHER_HEADER_SIZE) return -1;

    memcpy(&out->hdr, buf, AETHER_HEADER_SIZE);

    if (out->hdr.magic != AETHER_MAGIC) return -1;

    /* Validate header CRC */
    uint16_t stored_hcrc = out->hdr.header_crc16;
    out->hdr.header_crc16 = 0;
    uint16_t computed_hcrc = aether_crc16(buf, AETHER_HEADER_SIZE - 2);
    out->hdr.header_crc16 = stored_hcrc;
    if (stored_hcrc != computed_hcrc) return -2;

    size_t expected = AETHER_HEADER_SIZE + out->hdr.payload_size + 4;
    if (buf_len < expected) return -1;

    memcpy(out->payload, buf + AETHER_HEADER_SIZE, out->hdr.payload_size);

    /* Validate payload CRC */
    uint32_t stored_pcrc;
    memcpy(&stored_pcrc, buf + AETHER_HEADER_SIZE + out->hdr.payload_size, 4);
    uint32_t computed_pcrc = aether_crc32(out->payload, out->hdr.payload_size);
    out->payload_crc32 = stored_pcrc;
    if (stored_pcrc != computed_pcrc) return -3;

    return 0;
}

/* Validate an already-populated AetherPacket (magic + both CRCs).
   Returns 0 on success, -1 bad magic, -2 bad header CRC, -3 bad payload CRC. */
int aether_packet_validate(const AetherPacket *pkt) {
    if (pkt->hdr.magic != AETHER_MAGIC) return -1;

    uint8_t hbuf[AETHER_HEADER_SIZE];
    AetherHeader h = pkt->hdr;
    uint16_t stored_hcrc = h.header_crc16;
    h.header_crc16 = 0;
    memcpy(hbuf, &h, AETHER_HEADER_SIZE);
    if (aether_crc16(hbuf, AETHER_HEADER_SIZE - 2) != stored_hcrc) return -2;

    if (aether_crc32(pkt->payload, pkt->hdr.payload_size) != pkt->payload_crc32)
        return -3;

    return 0;
}
