#ifndef TRANSPORT_RFCOMM_H
#define TRANSPORT_RFCOMM_H

#include "aether_packet.h"

#define RFCOMM_CHANNEL  1

/* Dynamic PSM for the L2CAP variant (BR/EDR PSMs must be odd, >= 0x1001). */
#define AETHER_L2CAP_PSM  0x1001

/* Default listen/connect port for the TCP variant. */
#define AETHER_TCP_PORT   7331

typedef struct RFCOMMTransport RFCOMMTransport;

/* Server side (Laptop B — receiver) */
RFCOMMTransport* rfcomm_listen(uint8_t channel);   // blocks until connected
void             rfcomm_server_close(RFCOMMTransport *t);

/* Client side (Laptop A — sender) */
RFCOMMTransport* rfcomm_connect(const char *target_addr, uint8_t channel);
void             rfcomm_client_close(RFCOMMTransport *t);

/* L2CAP SOCK_SEQPACKET variant of the same transport: one AetherPacket per
   L2CAP SDU, no RFCOMM framing or credit flow control in the path. Same
   send/recv/close functions apply to the returned handle. Experimental: both
   ends must use the same variant, and sustained-throughput gain over RFCOMM
   is hardware-dependent — benchmark with `rfcomm_bench --l2cap` first. */
RFCOMMTransport* l2cap_listen(uint16_t psm);
RFCOMMTransport* l2cap_connect(const char *target_addr, uint16_t psm);

/* TCP variant (Wi-Fi instead of Bluetooth). A reliable ordered byte stream like
   RFCOMM, so it uses the *identical* header-then-payload framing — same wire
   bytes, no format change. Exists because Bluetooth Classic cannot carry
   hi-res lossless: NL-96k needs ~2-3 Mbps on real music against a measured
   ~200 kbps RFCOMM link. Same send/recv/close functions apply. */
RFCOMMTransport* tcp_listen(uint16_t port);
RFCOMMTransport* tcp_connect(const char *host, uint16_t port);

/* Parse "IP" or "IP:PORT" from a --target argument. *port_out keeps its
   incoming value (the caller's default) when no ":PORT" is given.
   Returns 0 on success, -1 on a malformed target. */
int tcp_parse_target(const char *target, char *host_out, size_t host_size,
                     uint16_t *port_out);

/* Send/receive packets (blocking) */
int  rfcomm_send_packet(RFCOMMTransport *t, const AetherPacket *pkt);
int  rfcomm_recv_packet(RFCOMMTransport *t, AetherPacket *pkt_out);

/* Non-blocking send: transmits only if the socket can take the whole packet
   right now, otherwise gives up and reports it. Returns 0 sent, 1 skipped
   (would have blocked), -1 on a real error.

   This exists for the receiver's CTRL_STATS_REPLY. That report is produced on
   the same thread that drains audio, so a blocking send there stalls packet
   intake whenever the reverse direction is congested — and on Bluetooth the
   reverse direction shares airtime with the audio stream, so it congests
   exactly when the stream is already struggling. The stall then backs up the
   sender's socket, its send queue sheds frames, and the listener hears a gap:
   a stats report causing the very loss it is meant to report. Skipping a
   report is free by comparison (the sender treats anything older than 2 s as
   "no data" and falls back to queue backpressure). */
int  rfcomm_try_send_packet(RFCOMMTransport *t, const AetherPacket *pkt);

/* Get raw socket fd (for select/poll) */
int  rfcomm_get_fd(const RFCOMMTransport *t);

/* Total bytes handed to the socket and accepted by it. send() blocks once the
   kernel's RFCOMM buffer is full, so the rate of change of this counter is the
   link's actual sustained throughput — which is the number that matters, and in
   practice runs well below a short rfcomm_bench burst. */
unsigned long rfcomm_tx_bytes(const RFCOMMTransport *t);

#endif /* TRANSPORT_RFCOMM_H */
