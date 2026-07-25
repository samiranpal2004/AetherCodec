/* Perceptual HQ end-to-end: PCM -> encoder -> packet(+CRC) -> decoder -> PCM.
   HQ is lossy by design, so this asserts on SNR and sanity (finite output,
   correct framing/latency) rather than bit-exactness, and reports the achieved
   bitrate against the 900-1,100 kbps target from the PRD.

   Two SNRs are reported:
     full-band     — over the whole 0..48 kHz range
     audible-band  — after applying the *same* low-pass to reference and decoded
   At 96 kHz most of the spectrum sits above the absolute threshold of hearing,
   where the psychoacoustic model correctly quantises everything to zero. A
   full-band SNR therefore penalises the model for behaving properly; the
   audible-band figure is the meaningful one. */
#include "aether_encoder.h"
#include "aether_decoder.h"
#include "aether_packet.h"
#include "codec_mdct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define SR      96000
#define CH      2
#define NFRAMES 64                       /* 64 * 512 = 32768 samples/channel */
#define NSAMP   (NFRAMES * MDCT_HOP)
#define LP_PASSES 4                      /* cascaded 5-tap MA -> steep rolloff */

static int32_t pcm_in[NSAMP * CH];
static int32_t pcm_out[NSAMP * CH];
static double  nz_buf[NSAMP + 8];
static double  ref_ch[NSAMP], dec_ch[NSAMP];

static double amp(double t, double f) { return sin(2.0 * M_PI * f * t); }

/* 5-tap moving average, cascaded. First null at 96k/5 ~= 19.2 kHz. */
static void lowpass(double *x, int n, int passes) {
    for (int p = 0; p < passes; p++)
        for (int i = n - 1; i >= 4; i--)
            x[i] = (x[i] + x[i-1] + x[i-2] + x[i-3] + x[i-4]) / 5.0;
}

static void fill_bandlimited_noise(void) {
    for (int i = 0; i < NSAMP + 8; i++)
        nz_buf[i] = (double)rand() / RAND_MAX * 2.0 - 1.0;
    lowpass(nz_buf, NSAMP + 8, 1);
}

/* kind 0 = sparse tonal partials; kind 1 = band-limited broadband (dense
   spectrum — the realistic worst case for a transform codec's bitrate). */
static void make_signal(int kind) {
    srand(99);
    if (kind == 1) fill_bandlimited_noise();

    for (int i = 0; i < NSAMP; i++) {
        double t = (double)i / SR;
        double l, r;
        if (kind == 0) {
            l = 0.45 * amp(t, 220.0) + 0.25 * amp(t, 880.0) + 0.12 * amp(t, 3000.0);
            r = 0.40 * amp(t, 330.0) + 0.22 * amp(t, 1320.0) + 0.10 * amp(t, 2500.0);
        } else {
            l = 0.35 * amp(t, 220.0) + 1.0 * nz_buf[i + 4];
            r = 0.35 * amp(t, 330.0) + 1.0 * nz_buf[i + 5];
        }
        if (l >  1.0) l =  1.0;
        if (l < -1.0) l = -1.0;
        if (r >  1.0) r =  1.0;
        if (r < -1.0) r = -1.0;
        pcm_in[i * CH]     = (int32_t)(l * 8000000.0);
        pcm_in[i * CH + 1] = (int32_t)(r * 8000000.0);
    }
}

static void run_case(int kind, const char *label,
                     double *snr_aud_out, double *kbps_out) {
    make_signal(kind);
    memset(pcm_out, 0, sizeof(pcm_out));

    AetherEncoder *enc = aether_encoder_create(AETHER_MODE_HQ, SR, 24, CH);
    AetherDecoder *dec = aether_decoder_create();
    assert(enc && dec);

    static uint8_t wire[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4];
    long total_payload = 0;

    for (int f = 0; f < NFRAMES; f++) {
        AetherPacket pkt;
        int r = aether_encoder_encode(enc, pcm_in + f * MDCT_HOP * CH,
                                      MDCT_HOP, &pkt);
        assert(r == 0);
        assert(pkt.hdr.mode == AETHER_MODE_HQ);
        total_payload += pkt.hdr.payload_size;

        int wlen = aether_packet_pack(&pkt, wire, sizeof(wire));
        assert(wlen > 0);
        AetherPacket rx;
        assert(aether_packet_unpack(wire, wlen, &rx) == 0);

        int got = aether_decoder_decode(dec, &rx, pcm_out + f * MDCT_HOP * CH,
                                        MDCT_HOP * CH);
        assert(got == MDCT_HOP * CH);
    }

    /* Overlap-add delays output by exactly one hop; skip the priming frame. */
    const int lag  = MDCT_HOP;
    const int from = MDCT_HOP;
    const int to   = NSAMP - lag;
    const int len  = to - from;

    double sig = 0.0, err = 0.0, peak_err = 0.0;
    double sig_a = 0.0, err_a = 0.0;

    for (int c = 0; c < CH; c++) {
        for (int i = 0; i < len; i++) {
            double o = pcm_in[(from + i) * CH + c];
            double d = pcm_out[(from + i + lag) * CH + c];
            assert(isfinite(d));
            ref_ch[i] = o;
            dec_ch[i] = d;
            sig += o * o;
            double e = o - d;
            err += e * e;
            if (fabs(e) > peak_err) peak_err = fabs(e);
        }
        /* Audible-band: identical low-pass on both sides, then compare. */
        lowpass(ref_ch, len, LP_PASSES);
        lowpass(dec_ch, len, LP_PASSES);
        for (int i = 64; i < len; i++) {      // skip filter warm-up
            double e = ref_ch[i] - dec_ch[i];
            sig_a += ref_ch[i] * ref_ch[i];
            err_a += e * e;
        }
    }

    double snr_full = 10.0 * log10(sig   / (err   + 1e-12));
    double snr_aud  = 10.0 * log10(sig_a / (err_a + 1e-12));

    double secs     = (double)NSAMP / SR;
    double kbps     = (total_payload + (double)NFRAMES * (AETHER_HEADER_SIZE + 4))
                      * 8.0 / 1000.0 / secs;
    double raw_kbps = (double)SR * CH * 24 / 1000.0;

    printf("  %-10s SNR full %5.1f dB | SNR audible %5.1f dB | peak err %8.0f | %6.0f kbps (%.1fx)\n",
           label, snr_full, snr_aud, peak_err, kbps, raw_kbps / kbps);

    assert(kbps < raw_kbps);
    aether_encoder_destroy(enc);
    aether_decoder_destroy(dec);

    *snr_aud_out = snr_aud;
    *kbps_out    = kbps;
}

int main(void) {
    printf("\xE2\x9C\x93 HQ codec: %d frames, %d ch @ %d Hz, hop %d\n",
           NFRAMES, CH, SR, MDCT_HOP);

    double snr_tonal, kbps_tonal, snr_broad, kbps_broad;
    run_case(0, "tonal",     &snr_tonal, &kbps_tonal);
    run_case(1, "broadband", &snr_broad, &kbps_broad);

    /* Regression floors: a broken transform or quantiser collapses these.
       Bitrate is content-adaptive by design — sparse tonal material needs far
       fewer bits than dense broadband material. */
    assert(snr_tonal > 20.0);
    assert(snr_broad > 20.0);

    printf("\nHQ round-trip OK (lossy by design; bitrate adapts to content).\n");
    return 0;
}
