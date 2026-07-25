/* Receives raw PCM over RFCOMM, writes it to stdout for playback.
   Laptop B:
     ./raw_stream_receiver | aplay -r 96000 -f S16_LE -c 2 */
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>

int main(void) {
    RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
    if (!t) return 1;

    AetherPacket pkt = {0};
    while (rfcomm_recv_packet(t, &pkt) == 0) {
        fwrite(pkt.payload, 1, pkt.hdr.payload_size, stdout);
        fflush(stdout);
    }

    rfcomm_server_close(t);
    return 0;
}
