/* Mid-stream ABR transitions: what has to stay true when the operating point
   changes underneath a live stream.

   Both checks here are regressions for bugs that made `--mode auto` unlistenable
   on a real two-laptop link:

     1. The packet sequence must be continuous across a switch. The sender used
        to destroy and recreate the encoder, and aether_encoder_create() starts
        `sequence` at 0 — so the receiver's jitter buffer saw a flood of packets
        older than next_seq and discarded every one of them until the counter
        climbed back. Observed as playout frozen for exactly as many frames as
        had been sent before the switch.

     2. The MDCT's rate-dependent tables must follow the rate. mdct_init() used
        to return early once initialised, so the Bark map and absolute hearing
        threshold latched to whichever rate was requested first. Encoder and
        decoder could then disagree on band boundaries — dequantisation applies
        the wrong step to the wrong bins, which is noise, not an artifact. */
#include "aether_encoder.h"
#include "aether_decoder.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define CH 2

static void fill_tone(int32_t *pcm, int frames, int rate, double hz, double phase) {
    for (int i = 0; i < frames; i++) {
        double v = sin(2.0 * M_PI * hz * (i + phase) / rate) * 3000000.0;
        pcm[i * CH + 0] = (int32_t)v;
        pcm[i * CH + 1] = (int32_t)(v * 0.8);
    }
}

int main(void) {
    static int32_t pcm[LPC_FRAME_SIZE * CH];
    static int32_t out[LPC_FRAME_SIZE * CH];
    AetherPacket pkt;

    /* --- 1. sequence survives every rung of the ladder -------------------- */
    AetherEncoder *enc = aether_encoder_create(AETHER_MODE_NL, 96000, 24, CH);
    assert(enc);

    struct { int mode, rate, frame; } ladder[] = {
        { AETHER_MODE_NL, 96000, LPC_FRAME_SIZE },
        { AETHER_MODE_NL, 48000, LPC_FRAME_SIZE },
        { AETHER_MODE_HQ, 96000, MDCT_HOP       },
        { AETHER_MODE_HQ, 48000, MDCT_HOP       },
        { AETHER_MODE_NL, 96000, LPC_FRAME_SIZE },
    };

    uint32_t expect = 0;
    for (size_t r = 0; r < sizeof(ladder) / sizeof(ladder[0]); r++) {
        aether_encoder_reconfigure(enc, ladder[r].mode, ladder[r].rate);
        for (int f = 0; f < 5; f++) {
            fill_tone(pcm, ladder[r].frame, ladder[r].rate, 440.0, f * 100);
            assert(aether_encoder_encode(enc, pcm, ladder[r].frame, &pkt) == 0);
            assert(pkt.hdr.sequence == expect);
            expect++;
        }
    }
    aether_encoder_destroy(enc);
    printf("\xE2\x9C\x93 abr-switch: sequence continuous across %u frames "
           "and 5 operating points\n", expect);

    /* --- 2. HQ decodes correctly after a rate change on BOTH ends ---------
       Encode at 96k, switch to 48k, and check the decoder — which sees 48k
       first, exactly like a receiver that joined during an NL stretch — still
       recovers the signal. With the latched tables this produced garbage. */
    mdct_init(96000);                       /* pre-latch the "wrong" rate */

    enc = aether_encoder_create(AETHER_MODE_HQ, 96000, 24, CH);
    assert(enc);
    aether_encoder_reconfigure(enc, AETHER_MODE_HQ, 48000);

    AetherDecoder *dec = aether_decoder_create();
    assert(dec);

    static int32_t prev[MDCT_HOP * CH];
    double sig = 0.0, err = 0.0;
    for (int f = 0; f < 24; f++) {
        fill_tone(pcm, MDCT_HOP, 48000, 1000.0, f * MDCT_HOP);
        assert(aether_encoder_encode(enc, pcm, MDCT_HOP, &pkt) == 0);
        int n = aether_decoder_decode(dec, &pkt, out, LPC_FRAME_SIZE * CH);
        assert(n == MDCT_HOP * CH);

        /* Decoder output lags the encoder by one hop (inherent to OLA), so
           frame f's output corresponds to frame f-1's input. */
        if (f >= 2) {                       /* skip the overlap-add warm-up */
            for (int i = 0; i < MDCT_HOP * CH; i++) {
                double s = (double)prev[i], e = (double)out[i] - s;
                sig += s * s; err += e * e;
            }
        }
        memcpy(prev, pcm, sizeof(prev));
    }
    /* Only true if both ends agree on the band layout; mismatched tables land
       well below 0 dB. */
    double snr = 10.0 * log10(sig / (err > 0 ? err : 1e-30));
    printf("\xE2\x9C\x93 abr-switch: HQ SNR after 96k->48k switch = %.1f dB\n", snr);
    assert(snr > 15.0);

    aether_decoder_destroy(dec);
    aether_encoder_destroy(enc);

    printf("\nAll ABR mid-stream switch tests passed.\n");
    return 0;
}
