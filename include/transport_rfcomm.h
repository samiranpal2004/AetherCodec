#ifndef TRANSPORT_RFCOMM_H
#define TRANSPORT_RFCOMM_H

#include "aether_packet.h"

#define RFCOMM_CHANNEL  1

typedef struct RFCOMMTransport RFCOMMTransport;

/* Server side (Laptop B — receiver) */
RFCOMMTransport* rfcomm_listen(uint8_t channel);   // blocks until connected
void             rfcomm_server_close(RFCOMMTransport *t);

/* Client side (Laptop A — sender) */
RFCOMMTransport* rfcomm_connect(const char *target_addr, uint8_t channel);
void             rfcomm_client_close(RFCOMMTransport *t);

/* Send/receive packets (blocking) */
int  rfcomm_send_packet(RFCOMMTransport *t, const AetherPacket *pkt);
int  rfcomm_recv_packet(RFCOMMTransport *t, AetherPacket *pkt_out);

/* Get raw socket fd (for select/poll) */
int  rfcomm_get_fd(const RFCOMMTransport *t);

/* Total bytes handed to the socket and accepted by it. send() blocks once the
   kernel's RFCOMM buffer is full, so the rate of change of this counter is the
   link's actual sustained throughput — which is the number that matters, and in
   practice runs well below a short rfcomm_bench burst. */
unsigned long rfcomm_tx_bytes(const RFCOMMTransport *t);

#endif /* TRANSPORT_RFCOMM_H */
