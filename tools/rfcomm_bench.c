/* RFCOMM throughput benchmark: 1000 packets of 1400-byte payload.
   Laptop B: ./rfcomm_bench server
   Laptop A: ./rfcomm_bench client AA:BB:CC:DD:EE:FF */
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PACKETS  1000
#define PAYLOAD_SIZE 1400   // ~1 frame of compressed audio

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s server|client [addr]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
        if (!t) return 1;

        uint64_t start = aether_timestamp_us();
        int count = 0;
        while (count < NUM_PACKETS) {
            AetherPacket pkt = {0};
            if (rfcomm_recv_packet(t, &pkt) < 0) break;
            count++;
        }
        uint64_t elapsed_us = aether_timestamp_us() - start;
        double elapsed_s    = elapsed_us / 1e6;
        double kbps         = (NUM_PACKETS * PAYLOAD_SIZE * 8.0) / 1000.0 / elapsed_s;

        printf("[server] Received %d packets in %.3fs\n", count, elapsed_s);
        printf("[server] Throughput: %.1f kbps\n", kbps);
        rfcomm_server_close(t);

    } else if (argc == 3) {
        RFCOMMTransport *t = rfcomm_connect(argv[2], RFCOMM_CHANNEL);
        if (!t) return 1;

        uint64_t start = aether_timestamp_us();
        for (int i = 0; i < NUM_PACKETS; i++) {
            AetherPacket pkt = {0};
            pkt.hdr.magic        = AETHER_MAGIC;
            pkt.hdr.sequence     = i;
            pkt.hdr.timestamp_us = (uint32_t)aether_timestamp_us();
            pkt.hdr.mode         = AETHER_MODE_NL;
            pkt.hdr.payload_size = PAYLOAD_SIZE;
            /* payload is zeroed — fine for benchmark */
            rfcomm_send_packet(t, &pkt);
        }
        uint64_t elapsed_us = aether_timestamp_us() - start;
        printf("[client] Sent %d packets in %.3fs (%.1f kbps)\n",
               NUM_PACKETS, elapsed_us / 1e6,
               (NUM_PACKETS * PAYLOAD_SIZE * 8.0) / 1000.0 / (elapsed_us / 1e6));
        rfcomm_client_close(t);
    } else {
        fprintf(stderr, "Usage: %s server|client [addr]\n", argv[0]);
        return 1;
    }
    return 0;
}
