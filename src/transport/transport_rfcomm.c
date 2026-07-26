#include "transport_rfcomm.h"
#include "transport_internal.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/l2cap.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

RFCOMMTransport* rfcomm_listen(uint8_t channel) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->server_fd = -1;
    t->conn_fd   = -1;

    t->server_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (t->server_fd < 0) { perror("socket"); free(t); return NULL; }

    struct sockaddr_rc addr = {
        .rc_family  = AF_BLUETOOTH,
        .rc_bdaddr  = *BDADDR_ANY,
        .rc_channel = channel
    };

    if (bind(t->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(t->server_fd); free(t); return NULL;
    }

    if (listen(t->server_fd, 1) < 0) {
        perror("listen"); close(t->server_fd); free(t); return NULL;
    }
    printf("[rfcomm] Waiting for connection on channel %d...\n", channel);

    struct sockaddr_rc client_addr = {0};
    socklen_t opt = sizeof(client_addr);
    t->conn_fd = accept(t->server_fd, (struct sockaddr*)&client_addr, &opt);
    if (t->conn_fd < 0) {
        perror("accept"); close(t->server_fd); free(t); return NULL;
    }

    char addr_str[18];
    ba2str(&client_addr.rc_bdaddr, addr_str);
    printf("[rfcomm] Connected from %s\n", addr_str);
    return t;
}

RFCOMMTransport* rfcomm_connect(const char *target_addr, uint8_t channel) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->server_fd = -1;
    t->conn_fd   = -1;

    t->conn_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (t->conn_fd < 0) { perror("socket"); free(t); return NULL; }

    struct sockaddr_rc addr = { .rc_family = AF_BLUETOOTH, .rc_channel = channel };
    str2ba(target_addr, &addr.rc_bdaddr);

    printf("[rfcomm] Connecting to %s channel %d...\n", target_addr, channel);
    if (connect(t->conn_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(t->conn_fd); free(t); return NULL;
    }
    printf("[rfcomm] Connected.\n");
    return t;
}

/* ---- L2CAP SOCK_SEQPACKET variant --------------------------------------- */

/* Ask for the largest MTU the kernel allows (imtu/omtu are u16). The audio
   payloads that matter are well under this — a worst-case NL noise frame is
   ~13 KB — but the default 672 would reject them outright. Must be set on the
   socket BEFORE bind/connect; an accepted connection inherits the listener's. */
static void l2cap_request_big_mtu(int fd) {
    struct l2cap_options opts;
    socklen_t len = sizeof(opts);
    if (getsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &len) < 0) return;
    opts.imtu = 65535;
    opts.omtu = 65535;
    if (setsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts)) < 0)
        perror("[l2cap] setsockopt L2CAP_OPTIONS (continuing with default MTU)");
}

RFCOMMTransport* l2cap_listen(uint16_t psm) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->server_fd = -1;
    t->conn_fd   = -1;
    t->seqpacket = 1;

    t->server_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (t->server_fd < 0) { perror("socket(l2cap)"); free(t); return NULL; }
    l2cap_request_big_mtu(t->server_fd);

    struct sockaddr_l2 addr = {
        .l2_family = AF_BLUETOOTH,
        .l2_bdaddr = *BDADDR_ANY,
        .l2_psm    = htobs(psm)
    };

    if (bind(t->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind(l2cap)"); close(t->server_fd); free(t); return NULL;
    }
    if (listen(t->server_fd, 1) < 0) {
        perror("listen(l2cap)"); close(t->server_fd); free(t); return NULL;
    }
    printf("[l2cap] Waiting for connection on PSM 0x%04x...\n", psm);

    struct sockaddr_l2 client_addr = {0};
    socklen_t opt = sizeof(client_addr);
    t->conn_fd = accept(t->server_fd, (struct sockaddr*)&client_addr, &opt);
    if (t->conn_fd < 0) {
        perror("accept(l2cap)"); close(t->server_fd); free(t); return NULL;
    }

    char addr_str[18];
    ba2str(&client_addr.l2_bdaddr, addr_str);
    printf("[l2cap] Connected from %s\n", addr_str);
    return t;
}

RFCOMMTransport* l2cap_connect(const char *target_addr, uint16_t psm) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->server_fd = -1;
    t->conn_fd   = -1;
    t->seqpacket = 1;

    t->conn_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (t->conn_fd < 0) { perror("socket(l2cap)"); free(t); return NULL; }
    l2cap_request_big_mtu(t->conn_fd);

    struct sockaddr_l2 addr = { .l2_family = AF_BLUETOOTH,
                                .l2_psm    = htobs(psm) };
    str2ba(target_addr, &addr.l2_bdaddr);

    printf("[l2cap] Connecting to %s PSM 0x%04x...\n", target_addr, psm);
    if (connect(t->conn_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect(l2cap)"); close(t->conn_fd); free(t); return NULL;
    }
    printf("[l2cap] Connected.\n");
    return t;
}

/* ---- Shared send/recv ---------------------------------------------------- */

int rfcomm_send_packet(RFCOMMTransport *t, const AetherPacket *pkt) {
    int n = aether_packet_pack(pkt, t->tx_buf, sizeof(t->tx_buf));
    if (n < 0) return -1;

    if (t->seqpacket) {
        /* One SDU per packet — L2CAP preserves message boundaries, and the
           controller fragments to the baseband on its own. A short write is
           not possible on SOCK_SEQPACKET: it's all or error. */
        for (;;) {
            int r = (int)send(t->conn_fd, t->tx_buf, (size_t)n, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) { perror("send(l2cap)"); return -1; }
            break;
        }
        t->tx_bytes += (unsigned long)n;
        return 0;
    }

    /* One send() for the whole packet. Hand-chunking at a hardcoded 672 was
       counterproductive: BlueZ's rfcomm_sock_sendmsg already fragments at the
       *negotiated* DLC MTU, so pre-splitting only forces extra RFCOMM frames
       whenever that MTU is larger than our guess. The loop below exists solely
       to handle a short write on a stream socket. */
    int sent = 0;
    while (sent < n) {
        int r = (int)send(t->conn_fd, t->tx_buf + sent, (size_t)(n - sent), 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            perror("send");
            return -1;
        }
        sent += r;
    }
    t->tx_bytes += (unsigned long)n;
    return 0;
}

int rfcomm_try_send_packet(RFCOMMTransport *t, const AetherPacket *pkt) {
    int n = aether_packet_pack(pkt, t->tx_buf, sizeof(t->tx_buf));
    if (n < 0) return -1;

    /* One shot, non-blocking. A partial write is not an option on a stream
       socket here: finishing it would mean blocking, which is the whole thing
       we are avoiding, and half a packet would desynchronise the peer's
       framing. So send atomically or not at all — MSG_DONTWAIT gives us
       exactly that for a packet this small (tens of bytes, far under any
       socket buffer). */
    for (;;) {
        int r = (int)send(t->conn_fd, t->tx_buf, (size_t)n,
                          MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        if (r < 0) return -1;
        if (r != n) return 1;    /* truncated: treat as skipped, never resume */
        break;
    }
    t->tx_bytes += (unsigned long)n;
    return 0;
}

unsigned long rfcomm_tx_bytes(const RFCOMMTransport *t) {
    return t ? t->tx_bytes : 0;
}

int rfcomm_recv_packet(RFCOMMTransport *t, AetherPacket *pkt_out) {
    if (t->seqpacket) {
        /* One recv() returns one whole SDU (the buffer exceeds any packet we
           can produce, so truncation cannot occur). */
        int r;
        for (;;) {
            r = (int)recv(t->conn_fd, t->rx_buf, sizeof(t->rx_buf), 0);
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        if (r <= 0) return -1;
        if (r < AETHER_HEADER_SIZE) return -1;
        return aether_packet_unpack(t->rx_buf, (size_t)r, pkt_out);
    }

    /* Stream socket: read header first */
    int received = 0;
    while (received < AETHER_HEADER_SIZE) {
        int r = recv(t->conn_fd, t->rx_buf + received,
                     AETHER_HEADER_SIZE - received, 0);
        if (r <= 0) return -1;
        received += r;
    }

    /* Parse payload size from header */
    AetherHeader hdr;
    memcpy(&hdr, t->rx_buf, AETHER_HEADER_SIZE);
    if (hdr.magic != AETHER_MAGIC) return -1;

    int total = AETHER_HEADER_SIZE + hdr.payload_size + 4;
    if (total > (int)sizeof(t->rx_buf)) return -1;

    /* Read remaining bytes */
    while (received < total) {
        int r = recv(t->conn_fd, t->rx_buf + received, total - received, 0);
        if (r <= 0) return -1;
        received += r;
    }

    return aether_packet_unpack(t->rx_buf, total, pkt_out);
}

int rfcomm_get_fd(const RFCOMMTransport *t) { return t->conn_fd; }

void rfcomm_server_close(RFCOMMTransport *t) {
    if (!t) return;
    if (t->conn_fd >= 0)   close(t->conn_fd);
    if (t->server_fd >= 0) close(t->server_fd);
    free(t);
}

void rfcomm_client_close(RFCOMMTransport *t) {
    if (!t) return;
    if (t->conn_fd >= 0) close(t->conn_fd);
    free(t);
}
