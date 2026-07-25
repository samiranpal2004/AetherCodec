/* aether_sender — Laptop A.

   Registers "AetherCodec Hi-Res BT" as a system audio output. Anything played
   into it is encoded and pushed over RFCOMM to Laptop B.

     aether_sender --target BB:BB:BB:BB:BB:BB [--mode nl|hq]
     aether_sender --loopback [--mode nl|hq]

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define SAMPLE_RATE 96000
#define CHANNELS    2
#define BIT_DEPTH   24
#define RING_FRAMES 64

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --target BT_ADDR [--mode nl|hq] [--verbose]\n"
        "       %s --loopback       [--mode nl|hq] [--verbose] [--no-play]\n"
        "\n"
        "  --no-play   loopback without opening a playback stream. Use this when\n"
        "              the machine has no real audio output: our virtual sink\n"
        "              would then be the default output and the playback stream\n"
        "              would auto-connect straight back into it, forming a\n"
        "              feedback loop.\n", argv0, argv0);
}

int main(int argc, char *argv[]) {
    const char *target = NULL;
    int mode = AETHER_MODE_NL, loopback = 0, verbose = 0, no_play = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc)      target = argv[++i];
        else if (!strcmp(argv[i], "--loopback"))               loopback = 1;
        else if (!strcmp(argv[i], "--no-play"))                no_play = 1;
        else if (!strcmp(argv[i], "--verbose"))                verbose = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "nl")) mode = AETHER_MODE_NL;
            else if (!strcmp(m, "hq")) mode = AETHER_MODE_HQ;
            else { fprintf(stderr, "unknown mode '%s'\n", m); return 1; }
        } else { usage(argv[0]); return 1; }
    }
    if (!target && !loopback) { usage(argv[0]); return 1; }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    const int frame = (mode == AETHER_MODE_NL) ? LPC_FRAME_SIZE : MDCT_HOP;

    AudioRing cap_ring, play_ring;
    if (audio_ring_init(&cap_ring, (uint32_t)frame * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }
    if (loopback && audio_ring_init(&play_ring, (uint32_t)frame * CHANNELS * RING_FRAMES)) {
        fprintf(stderr, "ring alloc failed\n"); return 1;
    }

    RFCOMMTransport *t = NULL;
    if (!loopback) {
        t = rfcomm_connect(target, RFCOMM_CHANNEL);
        if (!t) { fprintf(stderr, "[sender] RFCOMM connect failed\n"); return 1; }
    }

    AetherEncoder *enc = aether_encoder_create(mode, SAMPLE_RATE, BIT_DEPTH, CHANNELS);
    AetherDecoder *dec = loopback ? aether_decoder_create() : NULL;
    if (!enc || (loopback && !dec)) { fprintf(stderr, "codec init failed\n"); return 1; }

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

    printf("[sender] mode=%s %s\n", mode == AETHER_MODE_NL ? "NL" : "HQ",
           loopback ? "(loopback — no Bluetooth)" : "");
    printf("[sender] Sink registered as \"AetherCodec Hi-Res BT\".\n");
    printf("[sender] Play audio into it, e.g.:\n");
    printf("           pw-play --target aether_codec_sink yourfile.wav\n");
    printf("[sender] Ctrl+C to stop.\n");

    int32_t pcm[LPC_FRAME_SIZE * CHANNELS];
    int32_t out[LPC_FRAME_SIZE * CHANNELS];
    AetherPacket pkt;
    unsigned long frames = 0, bytes = 0;

    while (running) {
        if (audio_ring_available(&cap_ring) < (uint32_t)(frame * CHANNELS)) {
            usleep(2000);
            continue;
        }
        audio_ring_read(&cap_ring, pcm, (uint32_t)frame * CHANNELS);

        if (aether_encoder_encode(enc, pcm, frame, &pkt) < 0) {
            fprintf(stderr, "[sender] encode error (frame %lu)\n", frames);
            continue;
        }
        frames++;
        bytes += pkt.hdr.payload_size;

        if (loopback) {
            int n = aether_decoder_decode(dec, &pkt, out, frame * CHANNELS);
            if (n < 0) fprintf(stderr, "[sender] decode error\n");
            else if (play) audio_ring_write(&play_ring, out, (uint32_t)n);
        } else if (rfcomm_send_packet(t, &pkt) < 0) {
            fprintf(stderr, "[sender] send failed — link dropped\n");
            break;
        }

        if (verbose && (frames % 100) == 0) {
            double secs = (double)frames * frame / SAMPLE_RATE;
            printf("[stats] frames=%lu  %.1fs  %.0f kbps\n",
                   frames, secs, bytes * 8.0 / 1000.0 / secs);
        }
    }

    printf("\n[sender] stopping after %lu frames\n", frames);
    if (play) pw_play_stop(play);
    pw_sink_stop(sink);
    aether_encoder_destroy(enc);
    if (dec) aether_decoder_destroy(dec);
    if (t) rfcomm_client_close(t);
    audio_ring_free(&cap_ring);
    if (loopback) audio_ring_free(&play_ring);
    return 0;
}
