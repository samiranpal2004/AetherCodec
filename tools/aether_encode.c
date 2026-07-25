/* aether_encode — Phase 6 file-to-file encoder.

   Encodes a WAV file straight through AetherEncoder into a .aether bitstream
   (the same wire format aether_sender feeds to RFCOMM), without needing
   PipeWire, Bluetooth, or a second laptop. Together with aether_decode and
   compare_spectra.py this is what "not built yet" in MANUAL_TESTING.md #4
   refers to — the file-to-file measurement path for Phase 6.

     aether_encode input.wav output.aether [--mode nl|hq]

   Output file: [magic:u32][total_frames:u32] then one packed AetherPacket
   per encoded frame, back to back (identical framing to what goes over
   RFCOMM — aether_decode reads it exactly like transport_rfcomm.c does). */
#include "aether_encoder.h"
#include "aether_packet.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include "wav_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AETHER_FILE_MAGIC 0x48544541u   /* "AETH" little-endian */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s input.wav output.aether [--mode nl|hq]\n", argv0);
}

int main(int argc, char *argv[]) {
    if (argc < 3) { usage(argv[0]); return 1; }
    const char *in_path  = argv[1];
    const char *out_path = argv[2];
    int mode = AETHER_MODE_NL;

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "nl")) mode = AETHER_MODE_NL;
            else if (!strcmp(m, "hq")) mode = AETHER_MODE_HQ;
            else { fprintf(stderr, "unknown mode '%s'\n", m); return 1; }
        } else { usage(argv[0]); return 1; }
    }

    WavPCM wav;
    if (wav_read(in_path, &wav) < 0) return 1;
    if (wav.channels != 1 && wav.channels != 2) {
        fprintf(stderr, "aether_encode: only mono/stereo supported (got %d ch)\n",
                wav.channels);
        wav_free(&wav); return 1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "aether_encode: cannot create %s\n", out_path);
        wav_free(&wav); return 1;
    }

    uint8_t fhdr[8];
    fhdr[0] = (uint8_t)(AETHER_FILE_MAGIC);       fhdr[1] = (uint8_t)(AETHER_FILE_MAGIC >> 8);
    fhdr[2] = (uint8_t)(AETHER_FILE_MAGIC >> 16); fhdr[3] = (uint8_t)(AETHER_FILE_MAGIC >> 24);
    uint32_t total_frames = (uint32_t)wav.frames;
    fhdr[4] = (uint8_t)total_frames;       fhdr[5] = (uint8_t)(total_frames >> 8);
    fhdr[6] = (uint8_t)(total_frames >> 16); fhdr[7] = (uint8_t)(total_frames >> 24);
    fwrite(fhdr, 1, 8, out);

    int frame_samples = (mode == AETHER_MODE_NL) ? LPC_FRAME_SIZE : MDCT_HOP;

    AetherEncoder *enc = aether_encoder_create(mode, wav.sample_rate,
                                               wav.bit_depth, wav.channels);
    if (!enc) {
        fprintf(stderr, "aether_encode: encoder init failed\n");
        fclose(out); wav_free(&wav); return 1;
    }

    static int32_t chunk[LPC_FRAME_SIZE * 2 /* max channels */];
    static uint8_t wire[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4];

    size_t frame_pos = 0;
    unsigned long frames_written = 0;
    long payload_bytes = 0;

    while (frame_pos < wav.frames) {
        size_t have = wav.frames - frame_pos;
        size_t take = have < (size_t)frame_samples ? have : (size_t)frame_samples;

        memset(chunk, 0, sizeof(int32_t) * (size_t)frame_samples * wav.channels);
        memcpy(chunk, wav.samples + frame_pos * wav.channels,
               take * wav.channels * sizeof(int32_t));

        AetherPacket pkt;
        if (aether_encoder_encode(enc, chunk, frame_samples, &pkt) < 0) {
            fprintf(stderr, "aether_encode: encode error at frame %lu\n",
                    frames_written);
            break;
        }
        int n = aether_packet_pack(&pkt, wire, sizeof(wire));
        if (n < 0) {
            fprintf(stderr, "aether_encode: pack error at frame %lu\n",
                    frames_written);
            break;
        }
        fwrite(wire, 1, (size_t)n, out);
        payload_bytes += n;
        frames_written++;
        frame_pos += take;
    }

    fclose(out);
    aether_encoder_destroy(enc);

    double raw_bytes = (double)wav.frames * wav.channels * (wav.bit_depth / 8);
    double secs      = (double)wav.frames / wav.sample_rate;
    double kbps      = payload_bytes * 8.0 / 1000.0 / secs;

    printf("[aether_encode] %s -> %s  mode=%s\n", in_path, out_path,
           mode == AETHER_MODE_NL ? "NL" : "HQ");
    printf("  %zu frames @ %d Hz, %d ch, %d-bit  (%.2fs)\n",
           wav.frames, wav.sample_rate, wav.channels, wav.bit_depth, secs);
    printf("  raw: %.0f bytes | encoded: %ld bytes | ratio: %.2fx | %.0f kbps\n",
           raw_bytes, payload_bytes, raw_bytes / (double)payload_bytes, kbps);

    wav_free(&wav);
    return 0;
}
