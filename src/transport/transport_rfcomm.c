#include "transport_rfcomm.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct RFCOMMTransport {
    int server_fd;   // listening socket (-1 for client)
    int conn_fd;     // active connection socket
    uint8_t tx_buf[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4 + 16];
    uint8_t rx_buf[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4 + 16];
};

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

int rfcomm_send_packet(RFCOMMTransport *t, const AetherPacket *pkt) {
    int n = aether_packet_pack(pkt, t->tx_buf, sizeof(t->tx_buf));
    if (n < 0) return -1;

    /* Send in chunks respecting RFCOMM MTU */
    int sent = 0;
    while (sent < n) {
        int chunk = (n - sent < AETHER_RFCOMM_MTU) ? n - sent : AETHER_RFCOMM_MTU;
        int r = send(t->conn_fd, t->tx_buf + sent, chunk, 0);
        if (r < 0) { perror("send"); return -1; }
        sent += r;
    }
    return 0;
}

int rfcomm_recv_packet(RFCOMMTransport *t, AetherPacket *pkt_out) {
    /* Read header first */
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
