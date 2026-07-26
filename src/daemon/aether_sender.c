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
#include <sys/socket.h>
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

/* How often the HQ rate controller runs. The controller itself is
   abr_smr_step() — see abr_ctrl.h for why HQ needs one at all. It runs on the
   encode thread, which is also the thread that reads the psychoacoustic state,
   so no locking is needed. */
#define RATE_TICK_MS      250

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* Set by the ABR thread, consumed by the encode loop at a frame boundary. */
static _Atomic int pending_state = -1;

/* Set by the encode loop's rate controller, read by the ABR thread: the link is
   keeping up AND the encoder has no quality left to spend, i.e. there is real
   spare capacity. Without this, ABR relaxes its congestion ceiling purely on a
   timer and re-probes a mode the link demonstrably cannot carry every 20 s —
   each probe being an audible blip. */
static _Atomic int link_headroom = 0;

/* Latest CTRL_STATS_REPLY from the receiver (see stats_recv_thread). Loss is
   the receiver's own sequence-gap measurement — the signal abr_classify's loss
   thresholds were designed for but never had until this back-channel existed.
   A reading older than STATS_FRESH_MS is treated as "no data" (loss 0), so a
   stalled reverse path can never wedge ABR in a degraded state. */
#define STATS_FRESH_MS 2000
static _Atomic int      rx_loss_x10   = 0;    /* percent * 10 */
static _Atomic int      rx_buffer_ms  = -1;
static _Atomic uint64_t rx_stats_ms   = 0;    /* when it arrived (ms) */

static float rx_loss_pct_fresh(void) {
    uint64_t now = aether_timestamp_us() / 1000ULL;
    uint64_t at  = atomic_load(&rx_stats_ms);
    if (at == 0 || now - at > STATS_FRESH_MS) return 0.0f;
    return (float)atomic_load(&rx_loss_x10) / 10.0f;
}

/* ---- send queue --------------------------------------------------------- */

typedef struct {
    AetherPacket   *slots;
    float          *slot_ms;      /* audio duration carried by each slot */
    int             head, tail, count;
    float           queued_ms;
    int             closed;
    unsigned long   dropped;      /* overflow drops — the congestion signal   */
    unsigned long   flushed;      /* discarded on an ABR switch — NOT counted
                                     as congestion, or every flush would read
                                     as fresh backpressure and cascade the
                                     ladder down another rung */
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

/* Everything the receiver will see as a sequence gap (its `lost` counter). */
static unsigned long sendq_dropped_total(SendQueue *q) {
    pthread_mutex_lock(&q->mtx);
    unsigned long d = q->dropped + q->flushed;
    pthread_mutex_unlock(&q->mtx);
    return d;
}

/* Discard everything queued. Called when ABR commits to a new operating point:
   the backlog is audio the old mode over-produced, already ~SENDQ_MAX_MS late.
   Draining it through the link first (a) delays the new mode's packets by up to
   half a second and (b) keeps the queue-depth signal pinned high, which the
   controller reads as the NEW mode also failing — that false signal is what
   cascaded auto from NL-96k all the way to HQ-48k/SMR-12 in the first second of
   the 2026-07-25 run. The receiver conceals the gap and re-anchors. */
static void sendq_flush(SendQueue *q) {
    pthread_mutex_lock(&q->mtx);
    q->flushed  += (unsigned long)q->count;
    q->tail      = q->head;
    q->count     = 0;
    q->queued_ms = 0.0f;
    pthread_mutex_unlock(&q->mtx);
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

/* Drains the reverse direction of the socket. This must run in EVERY mode, not
   just auto: if nobody reads, the receiver's periodic stats replies eventually
   fill the socket buffers and block the receiver's send — stalling its recv
   loop and therefore playback. Runs even when the payload is ignored. */
static void *stats_recv_thread(void *arg) {
    struct send_args *sa = arg;
    static AetherPacket pkt;   /* 64 KB — keep off the thread stack */
    while (running) {
        if (rfcomm_recv_packet(sa->t, &pkt) < 0) break;
        if (pkt.hdr.mode == AETHER_MODE_CTRL &&
            pkt.hdr.payload_size >= sizeof(AetherStatsReply) &&
            pkt.payload[0] == CTRL_STATS_REPLY) {
            AetherStatsReply sr;
            memcpy(&sr, pkt.payload, sizeof(sr));
            atomic_store(&rx_loss_x10,  (int)sr.loss_x10);
            atomic_store(&rx_buffer_ms, (int)sr.buffer_ms);
            atomic_store(&rx_stats_ms, aether_timestamp_us() / 1000ULL);
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
    int         tcp;     /* no HCI link to read RSSI from */
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
        } else if (a->tcp) {
            /* No Bluetooth link exists, so there is no RSSI to read — polling
               it would just fail every 500 ms against an IP address. Feed a
               strong neutral value and let the real signals decide: receiver-
               reported loss (which works fine over TCP) and send-queue
               backpressure. On a clean LAN both are quiet, so `auto` correctly
               sits at the top rung. */
            rssi = -40;
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

        /* Receiver-reported loss (CTRL_STATS_REPLY). Real on-air/queue loss as
           the receiver counted it; 0 when no fresh report exists. */
        float loss = rx_loss_pct_fresh();

        ABRState before = abr_current_state(g_abr);
        abr_set_headroom(g_abr, atomic_load(&link_headroom));
        abr_update_congested(g_abr, rssi, loss, congested,
                             aether_timestamp_us() / 1000ULL);
        ABRState after = abr_current_state(g_abr);

        /* On TCP there is no RSSI, so report what actually drove the decision. */
        char link_desc[48];
        if (a->tcp) snprintf(link_desc, sizeof(link_desc), "loss=%.1f%%", loss);
        else        snprintf(link_desc, sizeof(link_desc), "RSSI=%d dBm", rssi);

        if (after != before) {
            g_abr_state_scratch = (int)after;
            atomic_store(&pending_state, (int)after);
            printf("[abr] %s -> %s (%s%s)\n",
                   abr_state_name(before), abr_state_name(after), link_desc,
                   congested ? ", congested" : "");
            fflush(stdout);
        } else if (a->verbose) {
            printf("[abr] holding %s (%s%s)\n",
                   abr_state_name(after), link_desc, congested ? ", congested" : "");
            fflush(stdout);
        }
    }
    return NULL;
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --target BT_ADDR|IP[:PORT] [--mode nl|hq|auto]\n"
        "                        [--l2cap | --tcp] [--verbose]\n"
        "       %s --loopback    [--mode nl|hq|auto] [--verbose] [--no-play]\n"
        "\n"
        "  --mode auto  adaptive bitrate: RSSI/loss + send-queue backpressure\n"
        "  --tcp        stream over TCP/Wi-Fi instead of Bluetooth; --target is\n"
        "               then an IPv4 address (default port %d). Bluetooth cannot\n"
        "               carry hi-res lossless; TCP can. Receiver needs --tcp too.\n"
        "  --l2cap      use L2CAP SOCK_SEQPACKET instead of RFCOMM (experimental;\n"
        "               receiver must also run with --l2cap)\n"
        "  --abr-demo   with --mode auto, sweep a simulated RSSI (single machine)\n"
        "  --no-play    loopback without a playback stream (avoids feeding our own\n"
        "               default sink back into itself on a machine with no real\n"
        "               audio output)\n",
        argv0, argv0, AETHER_TCP_PORT);
}

int main(int argc, char *argv[]) {
    const char *target = NULL;
    int mode = AETHER_MODE_NL, loopback = 0, verbose = 0, no_play = 0;
    int auto_mode = 0, abr_demo = 0, use_l2cap = 0, use_tcp = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc)      target = argv[++i];
        else if (!strcmp(argv[i], "--loopback"))               loopback = 1;
        else if (!strcmp(argv[i], "--no-play"))                no_play = 1;
        else if (!strcmp(argv[i], "--abr-demo"))               abr_demo = 1;
        else if (!strcmp(argv[i], "--l2cap"))                  use_l2cap = 1;
        else if (!strcmp(argv[i], "--tcp"))                    use_tcp = 1;
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
    if (use_tcp && use_l2cap) {
        fprintf(stderr, "--tcp and --l2cap are mutually exclusive\n");
        return 1;
    }

    /* --tcp: --target is "IP" or "IP:PORT". */
    char     tcp_host[64] = {0};
    uint16_t tcp_port     = AETHER_TCP_PORT;
    if (use_tcp && target &&
        tcp_parse_target(target, tcp_host, sizeof(tcp_host), &tcp_port) < 0) {
        fprintf(stderr, "[sender] bad --target '%s' (want IP or IP:PORT)\n", target);
        return 1;
    }

    /* auto over a real BT link starts at HQ-96k (see abr_start_at below); in
       loopback there is no link to flood, and --abr-demo needs the ladder's
       top so the sweep can exercise the full range. TCP also keeps the
       optimistic start — Wi-Fi carries NL-96k comfortably, and starting low
       there would pin the ladder for ~20 s per rung for no reason. */
    if (auto_mode && !loopback && !use_tcp) mode = AETHER_MODE_HQ;

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
    pthread_t send_tid, stats_tid;
    int send_running = 0, stats_running = 0;
    struct send_args sa;
    if (!loopback) {
        t = use_tcp   ? tcp_connect(tcp_host, tcp_port)
          : use_l2cap ? l2cap_connect(target, AETHER_L2CAP_PSM)
                      : rfcomm_connect(target, RFCOMM_CHANNEL);
        if (!t) { fprintf(stderr, "[sender] %s connect failed\n",
                          use_tcp ? "TCP" : use_l2cap ? "L2CAP" : "RFCOMM");
                  return 1; }
        if (sendq_init(&g_sendq)) { fprintf(stderr, "sendq alloc failed\n"); return 1; }
        sa.t = t;
        if (pthread_create(&send_tid, NULL, send_thread, &sa) == 0)
            send_running = 1;
        else { fprintf(stderr, "send thread failed\n"); return 1; }
        /* Reverse-path reader: consumes the receiver's stats replies (and must
           run regardless of mode so the socket's rx direction never backs up). */
        if (pthread_create(&stats_tid, NULL, stats_recv_thread, &sa) == 0)
            stats_running = 1;
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
    struct abr_thread_args aargs = { target, abr_demo, loopback, verbose, use_tcp };
    int abr_running = 0;
    if (auto_mode) {
        g_abr = abr_ctrl_create(abr_on_switch, &g_abr_state_scratch);
        /* Divergence from the HLD's "start optimistic" NL-96k start: measured
           NL-96k needs ~3 Mbps on real music (LPC gets ~1.5x on 24-bit
           masters), which no RFCOMM link carries. Starting there flooded the
           send queue and cascaded congestion downgrades to HQ-48k/SMR-12
           within the first second of every session — audible as a broken
           first few seconds, every time. Start at HQ-96k (fits any EDR link
           at the default SMR) and let the headroom-gated probe path earn the
           NL rungs if the link really has the capacity. Loopback keeps the
           optimistic start: no real link, and --abr-demo sweeps the ladder.

           TCP also keeps it. abr_start_at() makes the start rung a *ceiling*
           that only relaxes one rung per ABR_PROBE_INTERVAL_MS, so applying it
           to a multi-Mbps Wi-Fi link would hold `auto` at HQ-96k for ~20 s per
           rung purely because the Bluetooth path needed that guard. The
           default state is already NL-96k with no ceiling, which is right
           here: the link genuinely carries it, and congestion/loss will still
           step it down if that turns out to be wrong. */
        if (g_abr && !loopback && !use_tcp)
            abr_start_at(g_abr, ABR_STATE_HQ_96K,
                         aether_timestamp_us() / 1000ULL);
        if (g_abr && pthread_create(&abr_tid, NULL, abr_thread, &aargs) == 0)
            abr_running = 1;
    }

    printf("[sender] mode=%s %s\n",
           auto_mode ? "AUTO (ABR)" : (mode == AETHER_MODE_NL ? "NL" : "HQ"),
           loopback ? "(loopback — no Bluetooth)"
                    : use_tcp ? "(TCP/Wi-Fi)" : use_l2cap ? "(L2CAP)" : "(RFCOMM)");
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

    /* HQ rate controller + link-throughput measurement state. */
    uint64_t      rate_last_us    = seg_start_us;
    unsigned long rate_last_drop  = 0;
    unsigned long link_last_bytes = 0;
    uint64_t      link_last_us    = seg_start_us;
    double        link_kbps       = 0.0;

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
                /* The old mode's backlog must not be dragged into the new
                   operating point — see sendq_flush. On an upgrade the queue
                   is near-empty anyway, so flushing unconditionally is fine. */
                if (!loopback) sendq_flush(&g_sendq);
                seg_frames = 0; seg_bytes = 0;
                seg_start_us = aether_timestamp_us();
            }
        }

        /* ---- rate control tick (encode thread, so touching the encoder's
               psychoacoustic state here is safe) ---------------------------- */
        if (!loopback) {
            uint64_t now_us = aether_timestamp_us();
            if (now_us - rate_last_us >= RATE_TICK_MS * 1000ULL) {
                int depth = sendq_depth_ms(&g_sendq);
                unsigned long drops = sendq_dropped(&g_sendq);
                int behind = (drops > rate_last_drop);
                rate_last_drop = drops;

                if (mode == AETHER_MODE_HQ)
                    mdct_set_smr_db(abr_smr_step(mdct_get_smr_db(), depth, behind));

                /* Real spare capacity: queue empty and nothing left to spend it
                   on. In NL there is no quality knob, so the queue alone says it. */
                atomic_store(&link_headroom,
                             depth < ABR_QUEUE_LOW_MS &&
                             (mode == AETHER_MODE_NL ||
                              mdct_get_smr_db() >= ABR_SMR_MAX_DB - 0.01f));

                /* Sustained link throughput: send() blocks once the kernel
                   buffer is full, so this is what the air interface really
                   carried — not what we hoped to push. */
                unsigned long tx = rfcomm_tx_bytes(t);
                double secs = (now_us - link_last_us) / 1e6;
                if (secs > 0.0)
                    link_kbps = (tx - link_last_bytes) * 8.0 / 1000.0 / secs;
                link_last_bytes = tx;
                link_last_us    = now_us;
                rate_last_us    = now_us;
            }
        }

        /* NL frames are 2048 samples; HQ batches 4 MDCT hops to the same 2048
           so both modes pay one header+CRC per ~21 ms instead of HQ paying
           four times as many (see AETHER_HQ_HOPS_PER_PKT). */
        const int coded_frame = (mode == AETHER_MODE_NL)
                              ? LPC_FRAME_SIZE
                              : MDCT_HOP * AETHER_HQ_HOPS_PER_PKT;
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
            else if (mode == AETHER_MODE_HQ)
                printf("[stats] frames=%lu  %.1fs  %.0f kbps  mode=HQ rate=%d  "
                       "smr=%.0fdB link=%.0f kbps  queue=%dms dropped=%lu  "
                       "rxloss=%.1f%% rxbuf=%dms\n",
                       frames, wall_s, kbps, rate, mdct_get_smr_db(), link_kbps,
                       sendq_depth_ms(&g_sendq), sendq_dropped_total(&g_sendq),
                       rx_loss_pct_fresh(), atomic_load(&rx_buffer_ms));
            else
                printf("[stats] frames=%lu  %.1fs  %.0f kbps  mode=NL rate=%d  "
                       "link=%.0f kbps  queue=%dms dropped=%lu  "
                       "rxloss=%.1f%% rxbuf=%dms\n",
                       frames, wall_s, kbps, rate, link_kbps,
                       sendq_depth_ms(&g_sendq), sendq_dropped_total(&g_sendq),
                       rx_loss_pct_fresh(), atomic_load(&rx_buffer_ms));
            fflush(stdout);

            /* A fixed mode that keeps dropping is simply over the link's
               budget. No amount of queueing fixes that — say so once instead of
               letting the user chase a phantom bug. NL-96k on real 24-bit music
               compresses to ~2.5-3 Mbps (LPC gets ~1.5x, not the PRD's assumed
               3.3x), which is well past what RFCOMM/EDR carries. */
            if (!loopback && !over_budget_warned &&
                sendq_dropped(&g_sendq) > 100) {
                over_budget_warned = 1;
                if (mode == AETHER_MODE_NL)
                    fprintf(stderr,
                        "[sender] WARNING: NL-%dk needs ~%.0f kbps but the link "
                        "is only carrying ~%.0f kbps — frames are being dropped "
                        "and playback will glitch.\n"
                        "         NL is lossless, so there is no quality knob to "
                        "trade away; the bitrate is whatever the music needs.\n"
                        "         Use --mode hq (rate-controlled, adapts to the "
                        "link) or --mode auto.\n",
                        rate / 1000, kbps, link_kbps);
                else
                    fprintf(stderr,
                        "[sender] WARNING: HQ-%dk is at the minimum quality "
                        "(SMR %.0f dB, ~%.0f kbps) and the link is still only "
                        "carrying ~%.0f kbps.\n"
                        "         The rate controller has nothing left to give. "
                        "Check ./tools/rfcomm_bench and for 2.4 GHz Wi-Fi "
                        "interference on either laptop.\n",
                        rate / 1000, mdct_get_smr_db(), kbps, link_kbps);
            }
        }
    }

    printf("\n[sender] stopping after %lu frames\n", frames);
    running = 0;
    if (abr_running) pthread_join(abr_tid, NULL);
    if (g_abr) abr_ctrl_destroy(g_abr);
    if (send_running) { sendq_close(&g_sendq); pthread_join(send_tid, NULL); }
    if (stats_running) {
        /* Unblock the reader's recv() so the join can't hang. */
        if (t) shutdown(rfcomm_get_fd(t), SHUT_RDWR);
        pthread_join(stats_tid, NULL);
    }
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
