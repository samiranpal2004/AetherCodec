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

    /* CTRL_STATS_REPLY round-trip: the receiver->sender back-channel rides the
       same pack/unpack path, so prove the struct survives it byte-for-byte. */
    AetherPacket ctrl = {0};
    ctrl.hdr.magic        = AETHER_MAGIC;
    ctrl.hdr.sequence     = 7;
    ctrl.hdr.timestamp_us = (uint32_t)aether_timestamp_us();
    ctrl.hdr.mode         = AETHER_MODE_CTRL;
    AetherStatsReply sr = {
        .type = CTRL_STATS_REPLY, .loss_x10 = 123,   /* 12.3 % */
        .buffer_ms = 250, .underruns = 4242, .recv_total = 99999,
    };
    memcpy(ctrl.payload, &sr, sizeof(sr));
    ctrl.hdr.payload_size = (uint16_t)sizeof(sr);

    n = aether_packet_pack(&ctrl, buf, sizeof(buf));
    assert(n > 0);
    assert(aether_packet_unpack(buf, n, &unpacked) == 0);
    assert(unpacked.hdr.mode == AETHER_MODE_CTRL);
    AetherStatsReply back;
    memcpy(&back, unpacked.payload, sizeof(back));
    assert(back.type == CTRL_STATS_REPLY);
    assert(back.loss_x10 == 123 && back.buffer_ms == 250);
    assert(back.underruns == 4242 && back.recv_total == 99999);

    printf("\xE2\x9C\x93 Packet pack/unpack/CRC + CTRL_STATS_REPLY: PASS\n");
    return 0;
}
