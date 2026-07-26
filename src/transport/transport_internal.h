/* Private to src/transport/ — not a public interface.

   The transport handle shared by every variant (RFCOMM, L2CAP, TCP). Only the
   constructors differ per transport; send/recv/close live in
   transport_rfcomm.c and operate on this struct for all of them, branching
   solely on `seqpacket`.

   The type is still called RFCOMMTransport because renaming it would touch
   every daemon and tool for no behavioural gain. Read it as "the transport". */
#ifndef TRANSPORT_INTERNAL_H
#define TRANSPORT_INTERNAL_H

#include "transport_rfcomm.h"

struct RFCOMMTransport {
    int server_fd;   // listening socket (-1 for client)
    int conn_fd;     // active connection socket
    int seqpacket;   /* 1 = message-oriented (L2CAP SOCK_SEQPACKET): one packet
                        per SDU. 0 = byte stream (RFCOMM, TCP): header-then-
                        payload framing loop. */
    unsigned long tx_bytes;   // bytes accepted by the socket, for throughput
    uint8_t tx_buf[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4 + 16];
    uint8_t rx_buf[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4 + 16];
};

#endif /* TRANSPORT_INTERNAL_H */
