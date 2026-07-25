/* aether_sender — Laptop A.

   Registers "AetherCodec Hi-Res BT" as a system audio output. Anything played
   into it is encoded and pushed over RFCOMM to Laptop B.

     aether_sender --target BB:BB:BB:BB:BB:BB [--mode nl|hq|auto]
     aether_sender --loopback [--mode nl|hq|auto] [--no-play]

   Threading (RFCOMM path): the capture ring is drained by the encode loop,
   which hands finished packets to a bounded queue; a dedicated SEND thread
   drains that queue to the socket. Decoupling the two matters because
   rfcomm_send_packet blocks when the link is busy — and NL frames are large, so
   a blocking send used to stall the encode loop, drop captured audio, and make
   delivery lumpy (audible as stutter). The queue absorbs the bursts, and its
   depth is the backpressure signal the ABR engine needs: RSSI can be perfect
   while the link still can't carry NL-96k.

   --mode auto enables the adaptive bitrate engine: RSSI/loss + send-queue
   backpressure drive the ladder NL-96k / NL-48k / HQ-96k / HQ-48k.

   --loopback skips Bluetooth entirely: encode -> decode -> local playback. */
#include "aether_encoder.h"
#include "aether_decoder.h"
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include "audio_ring.h"
#include "pw_io.h"
#include "abr_ctrl.h"
#include "bt_rssi.h"
#include "resample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#define SAMPLE_RATE 96000
#define CHANNELS    2
#define BIT_DEPTH   24
#define RING_FRAMES 64
#define ABR_POLL_MS 500

/* Send queue: bounded FIFO of packets between the encode and send threads.
   Bounded by QUEUED AUDIO TIME, not by slot count. Sizing it in packets was a
   bug in disguise: an NL frame is 2048 samples (21 ms) but an HQ frame is only
   512 (5.3 ms), so 24 slots meant 512 ms of slack in NL and just 128 ms in HQ.
   Any Bluetooth hiccup longer than that — retransmits, WiFi coexistence, a page
   scan — overflowed the queue and dropped frames even though HQ's *average*
   rate fits the link comfortably. That is exactly the observed pattern: bursts
   of drops separated by long clean stretches. */
#define SENDQ_CAP       160   /* slot ceiling; the real bound is SENDQ_MAX_MS  */
#define SENDQ_MAX_MS    500   /* drop the oldest beyond this much queued audio */
#define SENDQ_HIGH_MS   200   /* above this the link is treated as congested   */

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* Set by the ABR thread, consumed by the encode loop at a frame boundary. */
static _Atomic int pending_state = -1;

/* ---- send queue --------------------------------------------------------- */

typedef struct {
    AetherPacket   *slots;
    float          *slot_ms;      /* audio duration carried by each slot */
    int             head, tail, count;
    float           queued_ms;
    int             closed;
    unsigned long   dropped;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} SendQueue;

static SendQueue g_sendq;

/* An AetherPacket is ~64 KB of which only payload_size bytes are live. At
   187 packets/s in HQ a full-struct memcpy on both push and pop burns ~24 MB/s
   for nothing. */
static void packet_copy(AetherPacket *dst, const AetherPacket *src) {
    dst->hdr = src->hdr;
    memcpy(dst->payload, src->payload, src->hdr.payload_size);
    dst->payload_crc32 = src->payload_crc32;
}

static int sendq_init(SendQueue *q) {
    q->slots   = calloc(SENDQ_CAP, sizeof(AetherPacket));
    q->slot_ms = calloc(SENDQ_CAP, sizeof(float));
    if (!q->slots || !q->slot_ms) return -1;
    q->head = q->tail = q->count = 0;
    q->queued_ms = 0.0f;
    q->closed = 0; q->dropped = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv, NULL);
    return 0;
}

static void sendq_drop_oldest(SendQueue *q) {
    q->queued_ms -= q->slot_ms[q->tail];
    if (q->queued_ms < 0.0f) q->queued_ms = 0.0f;
    q->tail = (q->tail + 1) % SENDQ_CAP;
    q->count--;
    q->dropped++;
}

static void sendq_push(SendQueue *q, const AetherPacket *pkt, float dur_ms) {
    pthread_mutex_lock(&q->mtx);
    /* Drop the oldest to bound latency; the receiver conceals the gap.
       Dropping is itself the congestion signal (see sendq_dropped). */
    while (q->count == SENDQ_CAP ||
           (q->count > 0 && q->queued_ms + dur_ms > (float)SENDQ_MAX_MS))
        sendq_drop_oldest(q);
    packet_copy(&q->slots[q->head], pkt);
    q->slot_ms[q->head] = dur_ms;
    q->head = (q->head + 1) % SENDQ_CAP;
    q->count++;
    q->queued_ms += dur_ms;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
}

static int sendq_pop(SendQueue *q, AetherPacket *out) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->cv, &q->mtx);
    if (q->count == 0) { pthread_mutex_unlock(&q->mtx); return -1; }
    packet_copy(out, &q->slots[q->tail]);
    q->queued_ms -= q->slot_ms[q->tail];
    if (q->queued_ms < 0.0f) q->queued_ms = 0.0f;
    q->tail = (q->tail + 1) % SENDQ_CAP;
    q->count--;
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

static void sendq_close(SendQueue *q) {
    pthread_mutex_lock(&q->mtx);
    q->closed = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
}

static int sendq_depth_ms(SendQueue *q) {
    pthread_mutex_lock(&q->mtx);
    int ms = (int)q->queued_ms;
    pthread_mutex_unlock(&q->mtx);
    return ms;
}

static unsigned long sendq_dropped(SendQueue *q) {
    pthread_mutex_lock(&q->mtx);
    unsigned long d = q->dropped;
    pthread_mutex_unlock(&q->mtx);
    return d;
}

static void sendq_free(SendQueue *q) {
    free(q->slots);   q->slots   = NULL;
    free(q->slot_ms); q->slot_ms = NULL;
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->cv);
}

struct send_args { RFCOMMTransport *t; };

static void *send_thread(void *arg) {
    struct send_args *sa = arg;
    static AetherPacket pkt;   /* 64 KB — keep off the thread stack */
    while (sendq_pop(&g_sendq, &pkt) == 0) {
        if (rfcomm_send_packet(sa->t, &pkt) < 0) {
            fprintf(stderr, "[sender] send failed — link dropped\n");
            running = 0;
            break;
        }
    }
    return NULL;
}

/* ---- ABR ---------------------------------------------------------------- */

static ABRCtrl *g_abr;

static void abr_on_switch(int mode, int rate, void *ud) {
    (void)mode; (void)rate;
    atomic_store(&pending_state, *(int *)ud);
}

static int g_abr_state_scratch;

struct abr_thread_args {
    const char *target;
    int         demo;
    int         loopback;
    int         verbose;
};

static void *abr_thread(void *arg) {
    struct abr_thread_args *a = arg;
    int warned = 0;
    int demo_rssi = -50, demo_dir = -1;
    unsigned long last_dropped = 0;

    while (running) {
        usleep(ABR_POLL_MS * 1000);
        if (!running) break;

        int rssi;
        if (a->demo) {
            demo_rssi += demo_dir * 3;
            if (demo_rssi <= -95) demo_dir =  1;
            if (demo_rssi >= -50) demo_dir = -1;
            rssi = demo_rssi;
        } else if (bt_read_rssi(a->target, &rssi) < 0) {
            if (!warned) {
                fprintf(stderr, "[abr] cannot read RSSI (needs an active link and "
                                "CAP_NET_RAW: sudo setcap cap_net_raw+ep ./aether_sender)"
                                " — driving on send backpressure only\n");
                warned = 1;
            }
            rssi = -50;   /* neutral: let backpressure make the decision */
        }

        /* Backpressure: the send queue backing up (or dropping) means the link
           cannot carry the current mode, regardless of how strong RSSI is. */
        int congested = 0;
        if (!a->loopback) {
            int depth_ms = sendq_depth_ms(&g_sendq);
            unsigned long d = sendq_dropped(&g_sendq);
            congested = (depth_ms > SENDQ_HIGH_MS) || (d > last_dropped);
            last_dropped = d;
        }

        ABRState before = abr_current_state(g_abr);
        abr_update_congested(g_abr, rssi, 0.0f, congested,
                             aether_timestamp_us() / 1000ULL);
        ABRState after = abr_current_state(g_abr);

        if (after != before) {
            g_abr_state_scratch = (int)after;
            atomic_store(&pending_state, (int)after);
            printf("[abr] %s -> %s (RSSI=%d dBm%s)\n",
                   abr_state_name(before), abr_state_name(after), rssi,
                   congested ? ", congested" : "");
            fflush(stdout);
        } else if (a->verbose) {
            printf("[abr] holding %s (RSSI=%d dBm%s)\n",
                   abr_state_name(after), rssi, congested ? ", congested" : "");
            fflush(stdout);
        }
    }
    return NULL;
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --target BT_ADDR [--mode nl|hq|auto] [--verbose]\n"
        "       %s --loopback       [--mode nl|hq|auto] [--verbose] [--no-play]\n"
        "\n"
        "  --mode auto  adaptive bitrate: RSSI/loss + send-queue backpressure\n"
        "  --abr-demo   with --mode auto, sweep a simulated RSSI (single machine)\n"
        "  --no-play    loopback without a playback stream (avoids feeding our own\n"
        "               default sink back into itself on a machine with no real\n"
        "               audio output)\n",
        argv0, argv0);
}

int main(int argc, char *argv[]) {
    const char *target = NULL;
    int mode = AETHER_MODE_NL, loopback = 0, verbose = 0, no_play = 0;
    int auto_mode = 0, abr_demo = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc)      target = argv[++i];
        else if (!strcmp(argv[i], "--loopback"))               loopback = 1;
        else if (!strcmp(argv[i], "--no-play"))                no_play = 1;
        else if (!strcmp(argv[i], "--abr-demo"))               abr_demo = 1;
        else if (!strcmp(argv[i], "--verbose"))                verbose = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "nl"))   mode = AETHER_MODE_NL;
            else if (!strcmp(m, "hq"))   mode = AETHER_MODE_HQ;
            else if (!strcmp(m, "auto")) { auto_mode = 1; mode = AETHER_MODE_NL; }
            else { fprintf(stderr, "unknown mode '%s'\n", m); return 1; }
        } else { usage(argv[0]); return 1; }
    }
    if (!target && !loopback) { usage(argv[0]); return 1; }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int rate = SAMPLE_RATE;

    AudioRing cap_ring, play_ring;
    if (audio_ring_init(&cap_ring, LPC_FRAME_SIZE * 2 * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }
    if (loopback && audio_ring_init(&play_ring,
                                    LPC_FRAME_SIZE * 2 * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }

    RFCOMMTransport *t = NULL;
    pthread_t send_tid; int send_running = 0;
    struct send_args sa;
    if (!loopback) {
        t = rfcomm_connect(target, RFCOMM_CHANNEL);
        if (!t) { fprintf(stderr, "[sender] RFCOMM connect failed\n"); return 1; }
        if (sendq_init(&g_sendq)) { fprintf(stderr, "sendq alloc failed\n"); return 1; }
        sa.t = t;
        if (pthread_create(&send_tid, NULL, send_thread, &sa) == 0)
            send_running = 1;
        else { fprintf(stderr, "send thread failed\n"); return 1; }
    }

    AetherEncoder *enc = aether_encoder_create(mode, rate, BIT_DEPTH, CHANNELS);
    AetherDecoder *dec = loopback ? aether_decoder_create() : NULL;
    if (!enc || (loopback && !dec)) { fprintf(stderr, "codec init failed\n"); return 1; }

    Resampler down, up;
    resample_init(&down, CHANNELS);
    resample_init(&up,   CHANNELS);

    PwSink *sink = pw_sink_start("aether_codec_sink", "AetherCodec Hi-Res BT",
                                 SAMPLE_RATE, CHANNELS, &cap_ring);
    if (!sink) {
        fprintf(stderr, "[sender] could not register PipeWire sink "
                        "(is PipeWire running?)\n");
        return 1;
    }
    PwPlay *play = NULL;
    if (loopback && !no_play) {
        play = pw_play_start("aether_loopback", SAMPLE_RATE, CHANNELS, &play_ring);
        if (!play) { fprintf(stderr, "[sender] playback init failed\n"); return 1; }
    }

    pthread_t abr_tid;
    struct abr_thread_args aargs = { target, abr_demo, loopback, verbose };
    int abr_running = 0;
    if (auto_mode) {
        g_abr = abr_ctrl_create(abr_on_switch, &g_abr_state_scratch);
        if (g_abr && pthread_create(&abr_tid, NULL, abr_thread, &aargs) == 0)
            abr_running = 1;
    }

    printf("[sender] mode=%s %s\n",
           auto_mode ? "AUTO (ABR)" : (mode == AETHER_MODE_NL ? "NL" : "HQ"),
           loopback ? "(loopback — no Bluetooth)" : "");
    if (auto_mode && abr_demo)
        printf("[sender] ABR demo: sweeping a simulated RSSI\n");
    printf("[sender] Sink registered as \"AetherCodec Hi-Res BT\".\n");
    printf("[sender] Play audio into it, e.g.:\n");
    printf("           pw-play --target aether_codec_sink yourfile.wav\n");
    printf("[sender] Ctrl+C to stop.\n");
    fflush(stdout);

    static int32_t pcm[LPC_FRAME_SIZE * 2 * CHANNELS];
    static int32_t enc_in[LPC_FRAME_SIZE * CHANNELS];
    static int32_t dec_out[LPC_FRAME_SIZE * CHANNELS];
    static int32_t up_out[LPC_FRAME_SIZE * 2 * CHANNELS];
    AetherPacket pkt;
    unsigned long frames = 0;
    /* Bitrate accounting is per operating point and measured against the WALL
       clock. Previously it divided cumulative bytes by cumulative *audio* time
       and never reset on a mode switch, so the first line after each switch
       mixed the old mode's bytes with the new mode's frame duration — that is
       where readings like "3653 kbps" came from. */
    unsigned long seg_frames = 0, seg_bytes = 0;
    uint64_t      seg_start_us = aether_timestamp_us();
    int           over_budget_warned = 0;

    while (running) {
        int ps = atomic_exchange(&pending_state, -1);
        if (ps >= 0) {
            int new_mode = abr_state_mode((ABRState)ps);
            int new_rate = abr_state_rate((ABRState)ps);
            if (new_mode != mode || new_rate != rate) {
                mode = new_mode; rate = new_rate;
                /* Reconfigure in place: destroying the encoder would restart the
                   packet sequence at 0, which wedges the receiver's jitter
                   buffer for as many frames as had already been sent. */
                aether_encoder_reconfigure(enc, mode, rate);
                resample_reset(&down);
                resample_reset(&up);
                if (dec) aether_decoder_flush(dec);
                seg_frames = 0; seg_bytes = 0;
                seg_start_us = aether_timestamp_us();
            }
        }

        const int coded_frame = (mode == AETHER_MODE_NL) ? LPC_FRAME_SIZE : MDCT_HOP;
        const int ratio       = SAMPLE_RATE / rate;
        const int cap_frames  = coded_frame * ratio;

        if (audio_ring_available(&cap_ring) < (uint32_t)(cap_frames * CHANNELS)) {
            usleep(2000);
            continue;
        }
        audio_ring_read(&cap_ring, pcm, (uint32_t)cap_frames * CHANNELS);

        const int32_t *src = pcm;
        if (ratio == 2) {
            resample_down2(&down, pcm, cap_frames, enc_in);
            src = enc_in;
        }

        if (aether_encoder_encode(enc, src, coded_frame, &pkt) < 0) {
            fprintf(stderr, "[sender] encode error (frame %lu)\n", frames);
            continue;
        }
        frames++;
        seg_frames++;
        seg_bytes += AETHER_HEADER_SIZE + pkt.hdr.payload_size + 4;

        if (loopback) {
            int n = aether_decoder_decode(dec, &pkt, dec_out,
                                          LPC_FRAME_SIZE * CHANNELS);
            if (n < 0) fprintf(stderr, "[sender] decode error\n");
            else if (play) {
                if (ratio == 2) {
                    int m = resample_up2(&up, dec_out, n / CHANNELS, up_out);
                    audio_ring_write(&play_ring, up_out, (uint32_t)m * CHANNELS);
                } else {
                    audio_ring_write(&play_ring, dec_out, (uint32_t)n);
                }
            }
        } else {
            /* never blocks the encode loop */
            sendq_push(&g_sendq, &pkt, 1000.0f * coded_frame / rate);
        }

        if (verbose && (frames % 100) == 0) {
            double audio_s = (double)seg_frames * coded_frame / rate;
            double wall_s  = (aether_timestamp_us() - seg_start_us) / 1e6;
            if (wall_s < 1e-6) wall_s = 1e-6;
            double kbps = seg_bytes * 8.0 / 1000.0 / audio_s;
            if (loopback)
                printf("[stats] frames=%lu  %.1fs  %.0f kbps  mode=%s rate=%d\n",
                       frames, wall_s, kbps,
                       mode == AETHER_MODE_NL ? "NL" : "HQ", rate);
            else
                printf("[stats] frames=%lu  %.1fs  %.0f kbps  mode=%s rate=%d  "
                       "queue=%dms dropped=%lu\n",
                       frames, wall_s, kbps,
                       mode == AETHER_MODE_NL ? "NL" : "HQ", rate,
                       sendq_depth_ms(&g_sendq), sendq_dropped(&g_sendq));
            fflush(stdout);

            /* A fixed mode that keeps dropping is simply over the link's
               budget. No amount of queueing fixes that — say so once instead of
               letting the user chase a phantom bug. NL-96k on real 24-bit music
               compresses to ~2.5-3 Mbps (LPC gets ~1.5x, not the PRD's assumed
               3.3x), which is well past what RFCOMM/EDR carries. */
            if (!loopback && !auto_mode && !over_budget_warned &&
                sendq_dropped(&g_sendq) > 100) {
                over_budget_warned = 1;
                fprintf(stderr,
                    "[sender] WARNING: %s-%dk needs ~%.0f kbps but the link "
                    "cannot carry it — frames are being dropped and playback "
                    "will glitch.\n"
                    "         Use --mode auto (picks the best mode the link "
                    "sustains) or --mode hq.\n"
                    "         Compare with ./tools/rfcomm_bench.\n",
                    mode == AETHER_MODE_NL ? "NL" : "HQ", rate / 1000, kbps);
            }
        }
    }

    printf("\n[sender] stopping after %lu frames\n", frames);
    running = 0;
    if (abr_running) pthread_join(abr_tid, NULL);
    if (g_abr) abr_ctrl_destroy(g_abr);
    if (send_running) { sendq_close(&g_sendq); pthread_join(send_tid, NULL); }
    if (!loopback) sendq_free(&g_sendq);
    if (play) pw_play_stop(play);
    pw_sink_stop(sink);
    aether_encoder_destroy(enc);
    if (dec) aether_decoder_destroy(dec);
    if (t) rfcomm_client_close(t);
    audio_ring_free(&cap_ring);
    if (loopback) audio_ring_free(&play_ring);
    return 0;
}
