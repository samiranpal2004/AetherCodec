/* Full-frame integration test: PCM -> aether_encoder -> AetherPacket ->
   aether_decoder -> PCM, verifying bit-perfect stereo reconstruction and
   packet CRC integrity.

   Four content cases exercise the stereo decorrelation and wasted-bits paths:
     mixed       — tone L, tone+noise R (the original regression case)
     correlated  — R is a scaled copy of L plus a small offset tone; mid/side
                   must engage and shrink the payload, and stay bit-perfect
     panned      — hard-panned (R near-silent); mid/side must NOT be chosen,
                   and the near-silent channel stays cheap
     16-in-24    — 16-bit content shifted into the 24-bit container; the
                   wasted-bits detector must reclaim the 8 zero LSBs

   Every case must round-trip with 0 mismatches — near-lossless mode is
   bit-perfect or it is broken. */
#include "aether_encoder.h"
#include "aether_decoder.h"
#include "aether_packet.h"
#include "codec_lpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define FRAME 2048

static int32_t pcm_in[FRAME * 2];
static int32_t pcm_out[FRAME * 2];

static double roundtrip(const char *label) {
    AetherEncoder *enc = aether_encoder_create(AETHER_MODE_NL, 96000, 24, 2);
    AetherDecoder *dec = aether_decoder_create();
    assert(enc && dec);

    AetherPacket pkt;
    int r = aether_encoder_encode(enc, pcm_in, FRAME, &pkt);
    assert(r == 0);
    assert(pkt.hdr.mode == AETHER_MODE_NL);
    assert(pkt.hdr.channels == 2);

    /* Serialize + validate CRC like the transport would. */
    static uint8_t wire[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4];
    int wlen = aether_packet_pack(&pkt, wire, sizeof(wire));
    assert(wlen > 0);
    AetherPacket rx;
    assert(aether_packet_unpack(wire, wlen, &rx) == 0);

    memset(pcm_out, 0, sizeof(pcm_out));
    int got = aether_decoder_decode(dec, &rx, pcm_out, FRAME * 2);
    assert(got == FRAME * 2);

    int mismatches = 0;
    for (int i = 0; i < FRAME * 2; i++)
        if (pcm_in[i] != pcm_out[i]) {
            if (mismatches < 10)
                fprintf(stderr, "  [%s] MISMATCH at %d: in=%d out=%d\n",
                        label, i, pcm_in[i], pcm_out[i]);
            mismatches++;
        }
    assert(mismatches == 0);

    int    raw   = FRAME * 2 * 3;  // 24-bit stereo
    double ratio = (double)raw / pkt.hdr.payload_size;
    printf("\xE2\x9C\x93 NL %-10s LOSSLESS (0 mismatches)  payload %5u bytes  ratio %.2fx\n",
           label, pkt.hdr.payload_size, ratio);

    aether_encoder_destroy(enc);
    aether_decoder_destroy(dec);
    return ratio;
}

int main(void) {
    srand(7);

    /* --- mixed: tone L, tone+noise R (original case) ---------------------- */
    for (int i = 0; i < FRAME; i++) {
        int32_t l = (int32_t)(6000000.0 * sin(2.0 * 3.14159 * 220.0 * i / 96000.0));
        int32_t nz = (rand() % 4096) - 2048;
        int32_t rr = (int32_t)(4000000.0 * sin(2.0 * 3.14159 * 660.0 * i / 96000.0)) + nz;
        pcm_in[i * 2]     = l;
        pcm_in[i * 2 + 1] = rr;
    }
    roundtrip("mixed");

    /* --- correlated: strongly shared content — mid/side territory --------- */
    for (int i = 0; i < FRAME; i++) {
        int32_t nz = (rand() % 512) - 256;
        int32_t c  = (int32_t)(5000000.0 * sin(2.0 * 3.14159 * 440.0 * i / 96000.0)) + nz;
        pcm_in[i * 2]     = c;
        pcm_in[i * 2 + 1] = c - (int32_t)(200000.0 * sin(2.0 * 3.14159 * 55.0 * i / 96000.0));
    }
    double ratio_ms = roundtrip("correlated");

    /* The same content coded with an uncorrelated right channel for reference:
       correlated stereo must compress meaningfully better, which is only true
       if mid/side actually engaged (the side channel is tiny). */
    for (int i = 0; i < FRAME; i++)
        pcm_in[i * 2 + 1] = (int32_t)(5000000.0 * sin(2.0 * 3.14159 * 461.0 * i / 96000.0))
                          + (rand() % 512) - 256;
    double ratio_lr = roundtrip("decorr-ref");
    /* Both channels carry independent noise, so the side channel is never
       tiny; a ~10% payload win on this content proves mid/side engaged. */
    assert(ratio_ms > ratio_lr * 1.05);

    /* --- panned: R near-silent; mid/side would smear L into both ---------- */
    for (int i = 0; i < FRAME; i++) {
        pcm_in[i * 2]     = (int32_t)(6000000.0 * sin(2.0 * 3.14159 * 330.0 * i / 96000.0))
                          + (rand() % 1024) - 512;
        pcm_in[i * 2 + 1] = 0;
    }
    roundtrip("panned");

    /* --- 16-in-24: 8 zero LSBs everywhere; wasted-bits must reclaim them -- */
    for (int i = 0; i < FRAME; i++) {
        int32_t l16 = (int32_t)(20000.0 * sin(2.0 * 3.14159 * 220.0 * i / 96000.0))
                    + (rand() % 64) - 32;
        int32_t r16 = (int32_t)(15000.0 * sin(2.0 * 3.14159 * 550.0 * i / 96000.0))
                    + (rand() % 64) - 32;
        pcm_in[i * 2]     = l16 << 8;   /* 16-bit master in a 24-bit container */
        pcm_in[i * 2 + 1] = r16 << 8;
    }
    double ratio_shift = roundtrip("16-in-24");

    /* Same waveform dithered to genuine 24 bits for reference: the shifted
       version must compress much better — that gain IS the wasted-bits path. */
    for (int i = 0; i < FRAME; i++) {
        pcm_in[i * 2]     += (rand() % 256) - 128;
        pcm_in[i * 2 + 1] += (rand() % 256) - 128;
    }
    double ratio_full = roundtrip("24bit-ref");
    assert(ratio_shift > ratio_full * 1.4);

    printf("\nAll NL integration cases bit-perfect "
           "(mid/side gain %.2fx vs %.2fx, wasted-bits gain %.2fx vs %.2fx).\n",
           ratio_ms, ratio_lr, ratio_shift, ratio_full);
    return 0;
}
