/* TCP transport — the same AetherPacket stream over Wi-Fi instead of Bluetooth.

   Why this exists: Bluetooth Classic physically cannot carry hi-res lossless.
   NL-96k needs ~2-3 Mbps on real music and HQ ~1 Mbps, while a measured RFCOMM
   link sustained ~200 kbps. The codec was always transport-agnostic (HLD
   principle #1, "runs over any byte stream"), so moving the audio onto TCP is
   purely a socket change — no codec, packet format or jitter-buffer impact.

   TCP is a reliable ordered byte stream exactly like RFCOMM, so these
   constructors build the standard handle with seqpacket = 0 and every packet
   then goes through the identical header-then-payload framing loop in
   transport_rfcomm.c. Nothing else in the stack knows the difference — the
   CTRL_STATS_REPLY reverse channel included, since the socket is bidirectional
   just like the RFCOMM one. */
#include "transport_rfcomm.h"
#include "transport_internal.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Nagle batches small writes waiting for an ACK, which on an audio stream turns
   into tens of ms of added, jittery latency for no bandwidth gain (our packets
   are already frame-sized). Disable it on both ends. */
static void tcp_set_nodelay(int fd) {
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
        perror("[tcp] TCP_NODELAY (continuing)");
}

RFCOMMTransport* tcp_listen(uint16_t port) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->server_fd = -1;
    t->conn_fd   = -1;
    t->seqpacket = 0;   /* byte stream: identical framing to RFCOMM */

    t->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->server_fd < 0) { perror("socket(tcp)"); free(t); return NULL; }

    /* Without this a restart within the TIME_WAIT window fails to bind. */
    int one = 1;
    if (setsockopt(t->server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        perror("[tcp] SO_REUSEADDR (continuing)");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(t->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(tcp)"); close(t->server_fd); free(t); return NULL;
    }
    if (listen(t->server_fd, 1) < 0) {
        perror("listen(tcp)"); close(t->server_fd); free(t); return NULL;
    }
    printf("[tcp] Waiting for connection on port %u...\n", (unsigned)port);
    fflush(stdout);

    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    t->conn_fd = accept(t->server_fd, (struct sockaddr *)&peer, &plen);
    if (t->conn_fd < 0) {
        perror("accept(tcp)"); close(t->server_fd); free(t); return NULL;
    }
    tcp_set_nodelay(t->conn_fd);

    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    printf("[tcp] Connected from %s:%u\n", ip, (unsigned)ntohs(peer.sin_port));
    return t;
}

RFCOMMTransport* tcp_connect(const char *host, uint16_t port) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->server_fd = -1;
    t->conn_fd   = -1;
    t->seqpacket = 0;

    t->conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->conn_fd < 0) { perror("socket(tcp)"); free(t); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "[tcp] '%s' is not a valid IPv4 address\n", host);
        close(t->conn_fd); free(t); return NULL;
    }

    printf("[tcp] Connecting to %s:%u...\n", host, (unsigned)port);
    fflush(stdout);
    if (connect(t->conn_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect(tcp)"); close(t->conn_fd); free(t); return NULL;
    }
    tcp_set_nodelay(t->conn_fd);
    printf("[tcp] Connected.\n");
    return t;
}

/* Split "1.2.3.4" or "1.2.3.4:9000" into host + port. Writes at most
   host_size-1 bytes to host_out; port_out is left untouched when no ":PORT" is
   present, so the caller's default survives. Returns 0 on success. */
int tcp_parse_target(const char *target, char *host_out, size_t host_size,
                     uint16_t *port_out) {
    if (!target || !host_out || host_size == 0) return -1;

    const char *colon = strrchr(target, ':');
    size_t hlen = colon ? (size_t)(colon - target) : strlen(target);
    if (hlen == 0 || hlen >= host_size) return -1;

    memcpy(host_out, target, hlen);
    host_out[hlen] = '\0';

    if (colon) {
        char *endp = NULL;
        long p = strtol(colon + 1, &endp, 10);
        if (!endp || *endp != '\0' || p <= 0 || p > 65535) return -1;
        if (port_out) *port_out = (uint16_t)p;
    }
    return 0;
}
