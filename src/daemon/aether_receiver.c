/* aether_receiver — Laptop B.

   Listens on RFCOMM, feeds arriving packets through the jitter buffer, decodes
   them and plays the PCM out of the default output (the 3.5mm jack).

     aether_receiver [--verbose] */
#include "aether_decoder.h"
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include "jitter_buf.h"
#include "codec_lpc.h"
#include "audio_ring.h"
#include "pw_io.h"
#include "resample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define SAMPLE_RATE 96000
#define CHANNELS    2
#define JITTER_MS   40
#define RING_FRAMES 64

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

int main(int argc, char *argv[]) {
    int verbose = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--verbose")) verbose = 1;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    AudioRing play_ring;
    if (audio_ring_init(&play_ring, LPC_FRAME_SIZE * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }

    PwPlay *play = pw_play_start("aether_receiver", SAMPLE_RATE, CHANNELS, &play_ring);
    if (!play) {
        fprintf(stderr, "[receiver] playback init failed (is PipeWire running?)\n");
        return 1;
    }

    AetherDecoder *dec = aether_decoder_create();
    JitterBuf     *jb  = jitter_buf_create(JITTER_MS, SAMPLE_RATE);
    if (!dec || !jb) { fprintf(stderr, "init failed\n"); return 1; }

    printf("[receiver] Playback ready. Waiting for RFCOMM connection...\n");
    RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
    if (!t) { fprintf(stderr, "[receiver] RFCOMM listen failed\n"); return 1; }

    /* The playback stream is always 96 kHz; when ABR drops the stream to 48 kHz
       we interpolate back up so the OS-facing format never changes. */
    Resampler up;
    resample_init(&up, CHANNELS);
    int last_rate = SAMPLE_RATE;

    AetherPacket pkt;
    int32_t out[LPC_FRAME_SIZE * CHANNELS];
    int32_t up_out[LPC_FRAME_SIZE * 2 * CHANNELS];
    unsigned long got = 0, played = 0, lost = 0;

    while (running) {
        if (rfcomm_recv_packet(t, &pkt) < 0) {
            printf("[receiver] link closed\n");
            break;
        }
        got++;
        jitter_buf_insert(jb, &pkt);

        /* Drain everything currently playable. */
        for (;;) {
            int was_lost = 0;
            const AetherPacket *p = jitter_buf_pop_ex(jb, &was_lost);
            if (!p) {
                if (was_lost) {
                    /* Concealment strategy 1 (HLD 12.1): insert silence. */
                    lost++;
                    memset(out, 0, sizeof(int32_t) * LPC_FRAME_SIZE * CHANNELS);
                    audio_ring_write(&play_ring, out, LPC_FRAME_SIZE * CHANNELS);
                    continue;
                }
                break;
            }
            int n = aether_decoder_decode(dec, p, out, LPC_FRAME_SIZE * CHANNELS);
            if (n > 0) {
                int rate = (p->hdr.sample_rate == AETHER_RATE_48000) ? 48000
                                                                     : SAMPLE_RATE;
                if (rate != last_rate) {
                    printf("[receiver] stream rate -> %d Hz, mode=%s\n", rate,
                           p->hdr.mode == AETHER_MODE_NL ? "NL" : "HQ");
                    resample_reset(&up);
                    last_rate = rate;
                }
                if (rate != SAMPLE_RATE) {
                    int m = resample_up2(&up, out, n / CHANNELS, up_out);
                    audio_ring_write(&play_ring, up_out, (uint32_t)m * CHANNELS);
                } else {
                    audio_ring_write(&play_ring, out, (uint32_t)n);
                }
                played++;
            }
        }

        if (verbose && (got % 100) == 0)
            printf("[stats] recv=%lu played=%lu lost=%lu buffer=%dms underruns=%lu\n",
                   got, played, lost, jitter_buf_level_ms(jb),
                   pw_play_underruns(play));
    }

    printf("[receiver] recv=%lu played=%lu lost=%lu underruns=%lu\n",
           got, played, lost, pw_play_underruns(play));

    rfcomm_server_close(t);
    jitter_buf_destroy(jb);
    aether_decoder_destroy(dec);
    pw_play_stop(play);
    audio_ring_free(&play_ring);
    return 0;
}
