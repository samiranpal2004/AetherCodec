#ifndef AETHER_PACKET_H
#define AETHER_PACKET_H

#include <stdint.h>
#include <stddef.h>

#define AETHER_MAGIC        0xAE7EC0DE
/* Packed on-wire header size. The AetherHeader fields sum to exactly 20 bytes
   (see packet diagram in HLD 8.1 — payload begins at byte 20). The PRD/HLD
   prose that says "24 bytes" is inconsistent with the field layout; 20 is
   authoritative and matches the struct below under #pragma pack(1). */
#define AETHER_HEADER_SIZE  20
#define AETHER_MAX_PAYLOAD  65535
#define AETHER_RFCOMM_MTU   672   // typical RFCOMM MTU

/* Codec modes */
#define AETHER_MODE_NL      0x00  // Near-Lossless (LPC + Rice)
#define AETHER_MODE_HQ      0x01  // High Quality Perceptual (MDCT)
#define AETHER_MODE_CTRL    0xFF  // Control packet

/* Sample rate codes */
#define AETHER_RATE_44100   0x00
#define AETHER_RATE_48000   0x01
#define AETHER_RATE_88200   0x02
#define AETHER_RATE_96000   0x03

/* Control packet subtypes (stored in payload[0]) */
#define CTRL_HANDSHAKE      0x01
#define CTRL_HANDSHAKE_ACK  0x02
#define CTRL_CODEC_CHANGE   0x03
#define CTRL_STATS_REQUEST  0x04
#define CTRL_STATS_REPLY    0x05
#define CTRL_TEARDOWN       0x06

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         // always AETHER_MAGIC
    uint32_t sequence;      // packet sequence number
    uint32_t timestamp_us;  // sender timestamp in microseconds
    uint8_t  mode;          // AETHER_MODE_*
    uint8_t  sample_rate;   // AETHER_RATE_*
    uint8_t  bit_depth;     // 16 or 24
    uint8_t  channels;      // 1 or 2
    uint16_t payload_size;  // bytes in payload
    uint16_t header_crc16;  // CRC-16 of bytes 0..17
} AetherHeader;
#pragma pack(pop)

_Static_assert(sizeof(AetherHeader) == AETHER_HEADER_SIZE,
               "AetherHeader must be exactly AETHER_HEADER_SIZE bytes on the wire");

typedef struct {
    AetherHeader hdr;
    uint8_t  payload[AETHER_MAX_PAYLOAD];
    uint32_t payload_crc32; // CRC-32 of payload bytes
} AetherPacket;

/* CTRL_STATS_REPLY payload (HLD 8.2): the receiver reports link quality back
   to the sender every ~500 ms over the same (bidirectional) socket. This is
   what lets the ABR engine act on what the RECEIVER actually experienced —
   real sequence-gap loss and buffer state — instead of inferring everything
   from its own send queue. */
#pragma pack(push, 1)
typedef struct {
    uint8_t  type;         // CTRL_STATS_REPLY
    uint16_t loss_x10;     // packet loss over the report window, percent * 10
    uint16_t buffer_ms;    // receiver-side jitter buffer depth
    uint32_t underruns;    // cumulative playback underrun frames
    uint32_t recv_total;   // cumulative packets received (sanity/debug)
} AetherStatsReply;
#pragma pack(pop)

/* Utility functions */
uint16_t aether_crc16(const uint8_t *data, size_t len);
uint32_t aether_crc32(const uint8_t *data, size_t len);

int  aether_packet_pack(const AetherPacket *pkt, uint8_t *buf, size_t buf_size);
int  aether_packet_unpack(const uint8_t *buf, size_t buf_len, AetherPacket *pkt_out);
int  aether_packet_validate(const AetherPacket *pkt);

uint64_t aether_timestamp_us(void);  // monotonic clock in microseconds

#endif /* AETHER_PACKET_H */
