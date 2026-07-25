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
#include <math.h>

#define SAMPLE_RATE 96000
#define CHANNELS    2
#define JITTER_MS   40

/* Playback ring, in output frames at SAMPLE_RATE. Must comfortably exceed the
   playback prebuffer (250 ms) plus the largest burst the sender's send queue
   can deliver at once, or bursts get truncated on write — which is a click. */
#define RING_MS     2000
#define RING_VALS   (SAMPLE_RATE / 1000 * RING_MS * CHANNELS)

/* Crossfade applied at the edges of a concealed gap. Writing a hard zero after
   a non-zero sample (and a non-zero sample after the zeros) is a step
   discontinuity — i.e. an audible click on every single lost packet. HLD 12.1's
   "insert silence" is kept as the strategy; this just ramps into and out of it. */
#define FADE_MS     5
#define FADE_FRAMES (SAMPLE_RATE / 1000 * FADE_MS)

/* Largest concealment we ever emit: one NL frame at 48 kHz, interpolated to 96. */
#define MAX_OUT_FRAMES (LPC_FRAME_SIZE * 2)

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* Whole-frame-or-nothing write. audio_ring_write() truncates on overflow, and a
   truncated frame tears the stream mid-sample; dropping the frame outright is
   both cleaner and countable. */
static int ring_write_frames(AudioRing *r, const int32_t *pcm, uint32_t frames) {
    uint32_t vals = frames * CHANNELS;
    if (audio_ring_space(r) < vals) return 0;
    audio_ring_write(r, pcm, vals);
    return 1;
}

int main(int argc, char *argv[]) {
    int verbose = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--verbose")) verbose = 1;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    AudioRing play_ring;
    if (audio_ring_init(&play_ring, RING_VALS)) {
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
    int32_t up_out[MAX_OUT_FRAMES * CHANNELS];
    int32_t gap[MAX_OUT_FRAMES * CHANNELS];
    unsigned long got = 0, played = 0, lost = 0, overflow = 0;

    /* How many output frames (at SAMPLE_RATE) one packet is worth. Concealment
       must emit exactly this much or the stream's timebase drifts: an NL frame
       is 2048 samples but an HQ frame is only 512, and a 48 kHz frame doubles
       on the way out. The old code always wrote 2048 frames of silence, so in
       HQ-96k every lost packet injected 4x too much silence — with ~25% loss
       that is tens of seconds of bogus silence stretched through the stream,
       which also kept the ring pinned full so real audio got truncated. */
    uint32_t out_frames = LPC_FRAME_SIZE;
    int32_t  tail[CHANNELS] = {0};   /* last emitted sample, for the fade-out */
    int      in_gap = 0;

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
                    /* Concealment strategy 1 (HLD 12.1): insert silence — but
                       ramped down from the last real sample so the gap does not
                       start with a step discontinuity. */
                    lost++;
                    uint32_t nf = out_frames;
                    uint32_t fade = nf < FADE_FRAMES ? nf : FADE_FRAMES;
                    for (uint32_t i = 0; i < nf; i++) {
                        float g = 0.0f;
                        if (!in_gap && i < fade)
                            g = 0.5f * (1.0f + cosf((float)M_PI * i / fade));
                        for (int c = 0; c < CHANNELS; c++)
                            gap[i * CHANNELS + c] = (int32_t)(tail[c] * g);
                    }
                    if (!ring_write_frames(&play_ring, gap, nf)) overflow++;
                    in_gap = 1;
                    continue;
                }
                break;
            }
            int n = aether_decoder_decode(dec, p, out, LPC_FRAME_SIZE * CHANNELS);
            if (n <= 0) continue;

            int rate = (p->hdr.sample_rate == AETHER_RATE_48000) ? 48000
                                                                 : SAMPLE_RATE;
            if (rate != last_rate) {
                printf("[receiver] stream rate -> %d Hz, mode=%s\n", rate,
                       p->hdr.mode == AETHER_MODE_NL ? "NL" : "HQ");
                resample_reset(&up);
                last_rate = rate;
            }

            int32_t *pcm;
            uint32_t nf;
            if (rate != SAMPLE_RATE) {
                nf  = (uint32_t)resample_up2(&up, out, n / CHANNELS, up_out);
                pcm = up_out;
            } else {
                nf  = (uint32_t)(n / CHANNELS);
                pcm = out;
            }
            out_frames = nf;

            /* Ramp back in after a concealed gap, for the same reason. */
            if (in_gap) {
                uint32_t fade = nf < FADE_FRAMES ? nf : FADE_FRAMES;
                for (uint32_t i = 0; i < fade; i++) {
                    float g = 0.5f * (1.0f - cosf((float)M_PI * i / fade));
                    for (int c = 0; c < CHANNELS; c++)
                        pcm[i * CHANNELS + c] = (int32_t)(pcm[i * CHANNELS + c] * g);
                }
                in_gap = 0;
            }
            for (int c = 0; c < CHANNELS; c++)
                tail[c] = pcm[(nf - 1) * CHANNELS + c];

            if (ring_write_frames(&play_ring, pcm, nf)) played++;
            else overflow++;
        }

        if (verbose && (got % 100) == 0) {
            printf("[stats] recv=%lu played=%lu lost=%lu overflow=%lu "
                   "buffer=%dms underruns=%lu resync=%lu\n",
                   got, played, lost, overflow, jitter_buf_level_ms(jb),
                   pw_play_underruns(play), jitter_buf_resyncs(jb));
            fflush(stdout);
        }
    }

    printf("[receiver] recv=%lu played=%lu lost=%lu overflow=%lu underruns=%lu "
           "resync=%lu\n",
           got, played, lost, overflow, pw_play_underruns(play),
           jitter_buf_resyncs(jb));

    rfcomm_server_close(t);
    jitter_buf_destroy(jb);
    aether_decoder_destroy(dec);
    pw_play_stop(play);
    audio_ring_free(&play_ring);
    return 0;
}
