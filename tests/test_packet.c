#include "aether_packet.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    AetherPacket pkt = {0};
    pkt.hdr.magic        = AETHER_MAGIC;
    pkt.hdr.sequence     = 42;
    pkt.hdr.timestamp_us = (uint32_t)aether_timestamp_us();
    pkt.hdr.mode         = AETHER_MODE_NL;
    pkt.hdr.sample_rate  = AETHER_RATE_96000;
    pkt.hdr.bit_depth    = 24;
    pkt.hdr.channels     = 2;

    const char *msg = "Hello AetherCodec";
    memcpy(pkt.payload, msg, strlen(msg));
    pkt.hdr.payload_size = (uint16_t)strlen(msg);

    uint8_t buf[4096];
    int n = aether_packet_pack(&pkt, buf, sizeof(buf));
    assert(n > 0);

    AetherPacket unpacked = {0};
    int r = aether_packet_unpack(buf, n, &unpacked);
    assert(r == 0);
    assert(unpacked.hdr.sequence == 42);
    assert(unpacked.hdr.magic == AETHER_MAGIC);
    assert(memcmp(unpacked.payload, msg, strlen(msg)) == 0);

    /* Test corruption detection */
    buf[30] ^= 0xFF;  // corrupt payload
    r = aether_packet_unpack(buf, n, &unpacked);
    assert(r == -3);  // payload CRC should fail

    printf("\xE2\x9C\x93 Packet pack/unpack/CRC: PASS\n");
    return 0;
}
