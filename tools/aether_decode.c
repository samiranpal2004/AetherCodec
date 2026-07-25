/* aether_decode — Phase 6 file-to-file decoder. Counterpart to aether_encode:
   reads a .aether bitstream (the same framing aether_encode writes / RFCOMM
   carries) and writes a WAV file, so the result can be diffed or spectrally
   compared against the original with compare_spectra.py.

     aether_decode input.aether output.wav

   Mode, sample rate, bit depth and channel count are read back from the first
   packet's header (every AetherHeader carries them) — nothing besides the
   original frame count needs to travel out-of-band. */
#include "aether_decoder.h"
#include "aether_packet.h"
#include "codec_lpc.h"
#include "wav_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AETHER_FILE_MAGIC 0x48544541u   /* "AETH" little-endian */

static int read_packet(FILE *f, uint8_t *buf, size_t buf_cap, AetherPacket *pkt) {
    size_t got = fread(buf, 1, AETHER_HEADER_SIZE, f);
    if (got == 0) return 0;             /* clean EOF */
    if (got != AETHER_HEADER_SIZE) return -1;

    AetherHeader hdr;
    memcpy(&hdr, buf, AETHER_HEADER_SIZE);
    if (hdr.magic != AETHER_MAGIC) return -1;

    size_t total = (size_t)AETHER_HEADER_SIZE + hdr.payload_size + 4;
    if (total > buf_cap) return -1;
    if (fread(buf + AETHER_HEADER_SIZE, 1, total - AETHER_HEADER_SIZE, f)
        != total - AETHER_HEADER_SIZE) return -1;

    return aether_packet_unpack(buf, total, pkt) == 0 ? 1 : -1;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s input.aether output.wav\n", argv[0]);
        return 1;
    }
    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "aether_decode: cannot open %s\n", in_path); return 1; }

    uint8_t fhdr[8];
    if (fread(fhdr, 1, 8, in) != 8) {
        fprintf(stderr, "aether_decode: %s too short for file header\n", in_path);
        fclose(in); return 1;
    }
    uint32_t magic = (uint32_t)fhdr[0] | (uint32_t)fhdr[1] << 8 |
                    (uint32_t)fhdr[2] << 16 | (uint32_t)fhdr[3] << 24;
    uint32_t total_frames = (uint32_t)fhdr[4] | (uint32_t)fhdr[5] << 8 |
                            (uint32_t)fhdr[6] << 16 | (uint32_t)fhdr[7] << 24;
    if (magic != AETHER_FILE_MAGIC) {
        fprintf(stderr, "aether_decode: %s is not an .aether file\n", in_path);
        fclose(in); return 1;
    }

    AetherDecoder *dec = aether_decoder_create();
    if (!dec) { fclose(in); return 1; }

    static uint8_t wire[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4];
    AetherPacket pkt;

    size_t cap = (size_t)total_frames + LPC_FRAME_SIZE;   /* pad slack */
    int channels = 0, sample_rate = 0, bit_depth = 0;
    int32_t *out_pcm = NULL;
    size_t out_frames = 0;
    int32_t frame_buf[LPC_FRAME_SIZE * 2];

    int r;
    unsigned long packets = 0;
    while ((r = read_packet(in, wire, sizeof(wire), &pkt)) == 1) {
        if (channels == 0) {
            channels    = pkt.hdr.channels;
            bit_depth   = pkt.hdr.bit_depth;
            sample_rate = (pkt.hdr.sample_rate == 1) ? 48000
                        : (pkt.hdr.sample_rate == 2) ? 88200
                        : (pkt.hdr.sample_rate == 0) ? 44100 : 96000;
            out_pcm = malloc(cap * (size_t)channels * sizeof(int32_t));
            if (!out_pcm) { fprintf(stderr, "aether_decode: OOM\n"); break; }
        }

        int n = aether_decoder_decode(dec, &pkt, frame_buf,
                                      LPC_FRAME_SIZE * channels);
        if (n < 0) {
            fprintf(stderr, "aether_decode: decode error at packet %lu\n", packets);
            break;
        }
        size_t n_frames = (size_t)n / (size_t)channels;
        if (out_frames + n_frames > cap) {
            cap = (out_frames + n_frames) * 2;
            int32_t *grown = realloc(out_pcm, cap * (size_t)channels * sizeof(int32_t));
            if (!grown) { fprintf(stderr, "aether_decode: OOM\n"); break; }
            out_pcm = grown;
        }
        memcpy(out_pcm + out_frames * channels, frame_buf,
               n_frames * channels * sizeof(int32_t));
        out_frames += n_frames;
        packets++;
    }
    fclose(in);
    aether_decoder_destroy(dec);

    if (r < 0)
        fprintf(stderr, "aether_decode: stream ended with a framing/CRC error "
                        "after %lu good packets\n", packets);

    if (!out_pcm) {
        fprintf(stderr, "aether_decode: no packets decoded\n");
        return 1;
    }

    /* Truncate to the original frame count — the encoder zero-pads the last
       partial frame to fill LPC_FRAME_SIZE / MDCT_HOP. */
    if (out_frames > total_frames) out_frames = total_frames;

    if (wav_write(out_path, out_pcm, out_frames, channels, sample_rate,
                 bit_depth == 16 ? 16 : 24) < 0) {
        free(out_pcm);
        return 1;
    }

    printf("[aether_decode] %s -> %s\n", in_path, out_path);
    printf("  %lu packets, %zu frames @ %d Hz, %d ch, %d-bit\n",
           packets, out_frames, sample_rate, channels, bit_depth);

    free(out_pcm);
    return 0;
}
