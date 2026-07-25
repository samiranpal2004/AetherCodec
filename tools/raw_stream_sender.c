/* Reads raw PCM on stdin, sends it uncompressed over RFCOMM.
   Laptop A:
     sox input.flac -t raw -r 96000 -b 16 -c 2 -e signed - \
       | ./raw_stream_sender AA:BB:CC:DD:EE:FF */

#define FRAME_SAMPLES 2048
#define FRAME_BYTES   (FRAME_SAMPLES * 2 * 2)  // 16-bit stereo

#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 2) { fprintf(stderr, "Usage: %s BT_ADDR\n", argv[0]); return 1; }

    RFCOMMTransport *t = rfcomm_connect(argv[1], RFCOMM_CHANNEL);
    if (!t) return 1;

    uint8_t pcm_buf[FRAME_BYTES];
    uint32_t seq = 0;

    while (fread(pcm_buf, 1, FRAME_BYTES, stdin) == FRAME_BYTES) {
        AetherPacket pkt = {0};
        pkt.hdr.magic        = AETHER_MAGIC;
        pkt.hdr.sequence     = seq++;
        pkt.hdr.timestamp_us = (uint32_t)aether_timestamp_us();
        pkt.hdr.mode         = AETHER_MODE_NL;   // raw PCM for now
        pkt.hdr.sample_rate  = AETHER_RATE_96000;
        pkt.hdr.bit_depth    = 16;
        pkt.hdr.channels     = 2;
        pkt.hdr.payload_size = FRAME_BYTES;
        memcpy(pkt.payload, pcm_buf, FRAME_BYTES);
        if (rfcomm_send_packet(t, &pkt) < 0) break;
    }

    rfcomm_client_close(t);
    return 0;
}
