/* aether_sender — Laptop A.

   Registers "AetherCodec Hi-Res BT" as a system audio output. Anything played
   into it is encoded and pushed over RFCOMM to Laptop B.

     aether_sender --target BB:BB:BB:BB:BB:BB [--mode nl|hq|auto]
     aether_sender --loopback [--mode nl|hq|auto] [--no-play]

   --mode auto enables the adaptive bitrate engine (Phase 5): an RSSI/loss poll
   every 500 ms drives the quality ladder NL-96k / NL-48k / HQ-96k / HQ-48k.

   --loopback skips Bluetooth entirely: encode -> decode -> local playback. It
   exercises the whole audio path (virtual sink, capture, codec, playback) on a
   single machine, which is the only way to test Phase 4 without Laptop B. */
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

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* Set by the ABR thread, consumed by the encode loop at a frame boundary. */
static _Atomic int pending_state = -1;

static void abr_on_switch(int mode, int rate, void *ud) {
    (void)mode; (void)rate;
    atomic_store(&pending_state, *(int *)ud);
}

struct abr_thread_args {
    const char *target;
    int         demo;
    int         verbose;
};

static ABRCtrl *g_abr;
static int      g_abr_state_scratch;

static void *abr_thread(void *arg) {
    struct abr_thread_args *a = arg;
    int warned = 0;
    int demo_rssi = -50, demo_dir = -1;

    while (running) {
        usleep(ABR_POLL_MS * 1000);
        if (!running) break;

        int rssi;
        if (a->demo) {
            /* Sweep the signal down and back so the ladder can be observed
               without physically walking away from a second laptop. */
            demo_rssi += demo_dir * 3;
            if (demo_rssi <= -95) demo_dir =  1;
            if (demo_rssi >= -50) demo_dir = -1;
            rssi = demo_rssi;
        } else if (bt_read_rssi(a->target, &rssi) < 0) {
            if (!warned) {
                fprintf(stderr, "[abr] cannot read RSSI (needs an active link and "
                                "CAP_NET_RAW: sudo setcap cap_net_raw+ep ./aether_sender)"
                                " — holding current quality\n");
                warned = 1;
            }
            continue;
        }

        ABRState before = abr_current_state(g_abr);
        /* Packet loss would come from the receiver's CTRL_STATS_REPLY; that
           back-channel is not implemented, so only RSSI drives the ladder. */
        abr_update(g_abr, rssi, 0.0f, 0);
        ABRState after = abr_current_state(g_abr);

        if (after != before) {
            g_abr_state_scratch = (int)after;
            atomic_store(&pending_state, (int)after);
            printf("[abr] %s -> %s (RSSI=%d dBm)\n",
                   abr_state_name(before), abr_state_name(after), rssi);
            fflush(stdout);
        } else if (a->verbose) {
            printf("[abr] holding %s (RSSI=%d dBm)\n",
                   abr_state_name(after), rssi);
            fflush(stdout);
        }
    }
    return NULL;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --target BT_ADDR [--mode nl|hq|auto] [--verbose]\n"
        "       %s --loopback       [--mode nl|hq|auto] [--verbose] [--no-play]\n"
        "\n"
        "  --mode auto  enable the adaptive bitrate engine (RSSI driven)\n"
        "  --abr-demo   with --mode auto, sweep a simulated RSSI so the ladder\n"
        "               can be watched on one machine (no second laptop needed)\n"
        "  --no-play    loopback without a playback stream. Use this when the\n"
        "               machine has no real audio output: our virtual sink would\n"
        "               then be the default output and the playback stream would\n"
        "               auto-connect back into it, forming a feedback loop.\n",
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

    /* Capture ring is sized for the largest frame the ladder can ask for:
       NL at 48 kHz consumes 2 * LPC_FRAME_SIZE samples of 96 kHz audio. */
    AudioRing cap_ring, play_ring;
    if (audio_ring_init(&cap_ring, LPC_FRAME_SIZE * 2 * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }
    if (loopback && audio_ring_init(&play_ring,
                                    LPC_FRAME_SIZE * 2 * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }

    RFCOMMTransport *t = NULL;
    if (!loopback) {
        t = rfcomm_connect(target, RFCOMM_CHANNEL);
        if (!t) { fprintf(stderr, "[sender] RFCOMM connect failed\n"); return 1; }
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
    struct abr_thread_args aargs = { target, abr_demo, verbose };
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

    static int32_t pcm[LPC_FRAME_SIZE * 2 * CHANNELS];   /* captured @96k */
    static int32_t enc_in[LPC_FRAME_SIZE * CHANNELS];    /* @ coding rate */
    static int32_t dec_out[LPC_FRAME_SIZE * CHANNELS];
    static int32_t up_out[LPC_FRAME_SIZE * 2 * CHANNELS];
    AetherPacket pkt;
    unsigned long frames = 0, bytes = 0;

    while (running) {
        /* Apply any pending ABR decision on a frame boundary. */
        int ps = atomic_exchange(&pending_state, -1);
        if (ps >= 0) {
            int new_mode = abr_state_mode((ABRState)ps);
            int new_rate = abr_state_rate((ABRState)ps);
            if (new_mode != mode || new_rate != rate) {
                mode = new_mode; rate = new_rate;
                /* Rebuild the encoder: mode and rate are both baked into its
                   state (MDCT overlap history, rate code). HLD 6.4 accepts a
                   one-frame gap at a switch. */
                aether_encoder_destroy(enc);
                enc = aether_encoder_create(mode, rate, BIT_DEPTH, CHANNELS);
                if (!enc) { fprintf(stderr, "[sender] re-init failed\n"); break; }
                resample_reset(&down);
                resample_reset(&up);
                if (dec) aether_decoder_flush(dec);
            }
        }

        const int coded_frame = (mode == AETHER_MODE_NL) ? LPC_FRAME_SIZE : MDCT_HOP;
        const int ratio       = SAMPLE_RATE / rate;            /* 1 or 2 */
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
        bytes += pkt.hdr.payload_size;

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
        } else if (rfcomm_send_packet(t, &pkt) < 0) {
            fprintf(stderr, "[sender] send failed — link dropped\n");
            break;
        }

        if (verbose && (frames % 100) == 0) {
            double secs = (double)frames * coded_frame / rate;
            printf("[stats] frames=%lu  %.1fs  %.0f kbps  mode=%s rate=%d\n",
                   frames, secs, bytes * 8.0 / 1000.0 / secs,
                   mode == AETHER_MODE_NL ? "NL" : "HQ", rate);
            fflush(stdout);
        }
    }

    printf("\n[sender] stopping after %lu frames\n", frames);
    running = 0;
    if (abr_running) pthread_join(abr_tid, NULL);
    if (g_abr) abr_ctrl_destroy(g_abr);
    if (play) pw_play_stop(play);
    pw_sink_stop(sink);
    aether_encoder_destroy(enc);
    if (dec) aether_decoder_destroy(dec);
    if (t) rfcomm_client_close(t);
    audio_ring_free(&cap_ring);
    if (loopback) audio_ring_free(&play_ring);
    return 0;
}
