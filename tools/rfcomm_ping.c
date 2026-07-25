/* Raw RFCOMM ping test.
   Usage:
     Laptop B: ./rfcomm_ping server
     Laptop A: ./rfcomm_ping client AA:BB:CC:DD:EE:FF
*/
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s server|client [addr]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
        if (!t) return 1;
        for (int i = 0; i < 10; i++) {
            AetherPacket pkt = {0};
            int r = rfcomm_recv_packet(t, &pkt);
            if (r < 0) { fprintf(stderr, "recv error\n"); break; }
            printf("[server] Got seq=%u payload='%.*s'\n",
                   pkt.hdr.sequence, pkt.hdr.payload_size, pkt.payload);
        }
        rfcomm_server_close(t);

    } else if (argc == 3) {
        RFCOMMTransport *t = rfcomm_connect(argv[2], RFCOMM_CHANNEL);
        if (!t) return 1;
        for (int i = 0; i < 10; i++) {
            AetherPacket pkt = {0};
            pkt.hdr.magic        = AETHER_MAGIC;
            pkt.hdr.sequence     = i;
            pkt.hdr.timestamp_us = (uint32_t)aether_timestamp_us();
            pkt.hdr.mode         = AETHER_MODE_CTRL;
            char msg[32];
            snprintf(msg, sizeof(msg), "ping %d", i);
            memcpy(pkt.payload, msg, strlen(msg));
            pkt.hdr.payload_size = (uint16_t)strlen(msg);
            rfcomm_send_packet(t, &pkt);
            printf("[client] Sent seq=%d\n", i);
        }
        rfcomm_client_close(t);
    } else {
        fprintf(stderr, "Usage: %s server|client [addr]\n", argv[0]);
        return 1;
    }
    return 0;
}
