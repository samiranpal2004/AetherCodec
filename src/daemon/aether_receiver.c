/* aether_receiver — Laptop B.

   Listens on RFCOMM, feeds arriving packets through the jitter buffer, decodes
   them and plays the PCM out of the default output (the 3.5mm jack).

   Every ~500 ms it also sends a CTRL_STATS_REPLY back up the same socket:
   its measured sequence-gap loss, buffer depth and underrun count. The sender
   feeds that into the ABR engine, which until this existed had to infer
   everything from its own send queue.

     aether_receiver [--l2cap | --tcp [--port N]] [--verbose] */
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
   playback prebuffer (PLAY_PREBUFFER_MS, 600 ms) plus the largest burst the
   sender's send queue can deliver at once (SENDQ_MAX_MS, 500 ms), or bursts get
   truncated on write — which is a click. 2000 ms leaves generous headroom. */
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

/* Compose and send one CTRL_STATS_REPLY. Runs on the recv thread between
   packets, so it shares the socket safely with the recv path (opposite
   directions, separate transport buffers). */
static void send_stats_reply(RFCOMMTransport *t, uint32_t *ctrl_seq,
                             unsigned long got, unsigned long lost,
                             unsigned long wgot, unsigned long wlost,
                             int buffer_ms, unsigned long underruns) {
    unsigned long dg = got - wgot, dl = lost - wlost;
    float loss_pct = (dg + dl) ? 100.0f * (float)dl / (float)(dg + dl) : 0.0f;

    AetherStatsReply sr = {
        .type      = CTRL_STATS_REPLY,
        .loss_x10  = (uint16_t)(loss_pct * 10.0f + 0.5f),
        .buffer_ms = (uint16_t)(buffer_ms < 0 ? 0 : buffer_ms),
        .underruns = (uint32_t)underruns,
        .recv_total = (uint32_t)got,
    };

    AetherPacket pkt;
    memset(&pkt, 0, sizeof(pkt.hdr));
    pkt.hdr.magic        = AETHER_MAGIC;
    pkt.hdr.sequence     = (*ctrl_seq)++;
    pkt.hdr.timestamp_us = (uint32_t)aether_timestamp_us();
    pkt.hdr.mode         = AETHER_MODE_CTRL;
    pkt.hdr.payload_size = (uint16_t)sizeof(sr);
    memcpy(pkt.payload, &sr, sizeof(sr));
    rfcomm_send_packet(t, &pkt);   /* best-effort; a failure ends the stream anyway */
}

int main(int argc, char *argv[]) {
    int verbose = 0, use_l2cap = 0, use_tcp = 0;
    uint16_t tcp_port = AETHER_TCP_PORT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose"))    verbose = 1;
        else if (!strcmp(argv[i], "--l2cap")) use_l2cap = 1;
        else if (!strcmp(argv[i], "--tcp"))   use_tcp = 1;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            long p = strtol(argv[++i], NULL, 10);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "[receiver] bad --port '%s'\n", argv[i]);
                return 1;
            }
            tcp_port = (uint16_t)p;
        }
    }
    if (use_tcp && use_l2cap) {
        fprintf(stderr, "--tcp and --l2cap are mutually exclusive\n");
        return 1;
    }

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

    printf("[receiver] Playback ready. Waiting for %s connection...\n",
           use_tcp ? "TCP" : use_l2cap ? "L2CAP" : "RFCOMM");
    RFCOMMTransport *t = use_tcp   ? tcp_listen(tcp_port)
                       : use_l2cap ? l2cap_listen(AETHER_L2CAP_PSM)
                                   : rfcomm_listen(RFCOMM_CHANNEL);
    if (!t) { fprintf(stderr, "[receiver] listen failed\n"); return 1; }

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

    /* Stats back-channel state: report every ~500 ms, loss measured over the
       window since the previous report. */
    uint32_t      ctrl_seq = 0;
    uint64_t      last_stats_ms = aether_timestamp_us() / 1000ULL;
    unsigned long wgot = 0, wlost = 0;

    while (running) {
        if (rfcomm_recv_packet(t, &pkt) < 0) {
            printf("[receiver] link closed\n");
            break;
        }
        /* Control packets carry no audio and must never enter the jitter
           buffer (their sequence numbering is independent of the stream's). */
        if (pkt.hdr.mode == AETHER_MODE_CTRL) continue;
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

        uint64_t now_ms = aether_timestamp_us() / 1000ULL;
        if (now_ms - last_stats_ms >= 500) {
            send_stats_reply(t, &ctrl_seq, got, lost, wgot, wlost,
                             jitter_buf_level_ms(jb), pw_play_underruns(play));
            wgot = got; wlost = lost;
            last_stats_ms = now_ms;
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
