/* Transport throughput benchmark: 1000 packets of 1400-byte payload.
   Laptop B: ./rfcomm_bench server [--l2cap | --tcp]
   Laptop A: ./rfcomm_bench client AA:BB:CC:DD:EE:FF [--l2cap]
             ./rfcomm_bench client 192.168.1.42 --tcp

   --l2cap benches the L2CAP SOCK_SEQPACKET transport instead of RFCOMM, and
   --tcp benches the TCP/Wi-Fi transport (both ends must use the same one).
   Compare the numbers on the same pair of laptops to pick a transport for the
   daemons — TCP should read multi-Mbps where Bluetooth reads hundreds of kbps.

   For the Bluetooth transports remember this is a best-case burst: sustained
   streaming throughput (the sender's `link=` stat) typically runs well below
   it, especially with 2.4 GHz Wi-Fi active on either machine. */
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PACKETS  1000
#define PAYLOAD_SIZE 1400   // ~1 frame of compressed audio

int main(int argc, char *argv[]) {
    int use_l2cap = 0, use_tcp = 0;
    const char *role = NULL, *addr = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--l2cap"))            use_l2cap = 1;
        else if (!strcmp(argv[i], "--tcp"))         use_tcp = 1;
        else if (!role)                             role = argv[i];
        else if (!addr)                             addr = argv[i];
    }
    if (!role || (use_tcp && use_l2cap)) {
        fprintf(stderr, "Usage: %s server|client [addr] [--l2cap | --tcp]\n",
                argv[0]);
        return 1;
    }
    const char *tname = use_tcp ? "TCP" : use_l2cap ? "L2CAP" : "RFCOMM";

    /* --tcp: addr is "IP" or "IP:PORT". */
    char     tcp_host[64] = {0};
    uint16_t tcp_port     = AETHER_TCP_PORT;
    if (use_tcp && addr &&
        tcp_parse_target(addr, tcp_host, sizeof(tcp_host), &tcp_port) < 0) {
        fprintf(stderr, "bad address '%s' (want IP or IP:PORT)\n", addr);
        return 1;
    }

    if (strcmp(role, "server") == 0) {
        RFCOMMTransport *t = use_tcp   ? tcp_listen(tcp_port)
                           : use_l2cap ? l2cap_listen(AETHER_L2CAP_PSM)
                                       : rfcomm_listen(RFCOMM_CHANNEL);
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

        printf("[server] Received %d packets in %.3fs (%s)\n",
               count, elapsed_s, tname);
        printf("[server] Throughput: %.1f kbps\n", kbps);
        rfcomm_server_close(t);

    } else if (addr) {
        RFCOMMTransport *t = use_tcp   ? tcp_connect(tcp_host, tcp_port)
                           : use_l2cap ? l2cap_connect(addr, AETHER_L2CAP_PSM)
                                       : rfcomm_connect(addr, RFCOMM_CHANNEL);
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
        printf("[client] Sent %d packets in %.3fs (%.1f kbps, %s)\n",
               NUM_PACKETS, elapsed_us / 1e6,
               (NUM_PACKETS * PAYLOAD_SIZE * 8.0) / 1000.0 / (elapsed_us / 1e6),
               tname);
        rfcomm_client_close(t);
    } else {
        fprintf(stderr, "Usage: %s server|client [addr] [--l2cap | --tcp]\n",
                argv[0]);
        return 1;
    }
    return 0;
}
