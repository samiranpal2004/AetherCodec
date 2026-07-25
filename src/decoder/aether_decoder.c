#include "aether_decoder.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Payload layouts are documented in src/encoder/aether_encoder.c. */

#define AETHER_FLAG_MID_SIDE 0x01

struct AetherDecoder {
    int   last_mode;
    int   mdct_started;
    float ola[2][MDCT_HOP];   // HQ: overlap-add tail per channel (L/R domain)
};

AetherDecoder* aether_decoder_create(void) {
    AetherDecoder *dec = calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    dec->last_mode = -1;
    return dec;
}

static int rate_from_code(uint8_t code) {
    switch (code) {
        case AETHER_RATE_44100: return 44100;
        case AETHER_RATE_48000: return 48000;
        case AETHER_RATE_88200: return 88200;
        default:                return 96000;
    }
}

/* ---- Near-Lossless ------------------------------------------------------ */

static const uint8_t* decode_channel_nl(const uint8_t *p, const uint8_t *end,
                                        int n, int32_t *chan_out) {
    if (end - p < 3) return NULL;
    LPCFrameHeader h = {0};
    h.order    = *p++;
    int k      = *p++;
    int wasted = *p++;
    if (h.order < 0 || h.order > LPC_MAX_ORDER || h.order > n) return NULL;
    if (wasted < 0 || wasted > 30) return NULL;

    long need = (long)h.order * 8 + 2;
    if (end - p < need) return NULL;
    memcpy(h.coeffs, p, (size_t)h.order * 4); p += h.order * 4;
    memcpy(h.warmup, p, (size_t)h.order * 4); p += h.order * 4;

    uint16_t rl;
    memcpy(&rl, p, 2); p += 2;
    if (end - p < rl) return NULL;

    int32_t res[LPC_FRAME_SIZE] = {0};
    if (rice_decode(p, rl, k, n - h.order, res + h.order) < 0) return NULL;
    p += rl;

    lpc_decode_frame(&h, res, n, chan_out);

    /* Undo the wasted-bits shift AFTER synthesis: encoder and decoder run the
       identical integer recursion in the shifted domain, so the final shift is
       the only place the scale is restored — exact by construction. */
    if (wasted)
        for (int i = 0; i < n; i++)
            chan_out[i] <<= wasted;

    return p;
}

/* ---- Perceptual HQ ------------------------------------------------------ */

/* Parse + dequantise one channel's coefficient block. No transform here: any
   mid/side reconstruction must happen on coefficients, before the IMDCT, so
   the overlap-add history stays in L/R space. */
static const uint8_t* parse_channel_hq(const uint8_t *p, const uint8_t *end,
                                       float *coeffs) {
    /* Band mask: bit b set means band b was coded. Unset bands are all-zero and
       carry neither a scalefactor nor any coefficients (see the encoder). */
    if (end - p < 8 + 3) return NULL;
    uint64_t band_mask = 0;
    for (int i = 0; i < 8; i++) band_mask |= (uint64_t)(*p++) << (8 * i);

    int nsf = 0, nq = 0;
    for (int b = 0; b < BARK_BANDS; b++) {
        if (!((band_mask >> b) & 1)) continue;
        nsf++;
        nq += mdct_band_start(b + 1) - mdct_band_start(b);
    }

    int sfk = *p++;
    uint16_t sl; memcpy(&sl, p, 2); p += 2;
    if (end - p < sl) return NULL;

    int32_t sf_delta[BARK_BANDS];
    if (nsf > 0 && rice_decode(p, sl, sfk, nsf, sf_delta) < 0) return NULL;
    p += sl;

    if (end - p < 3) return NULL;
    int cfk = *p++;
    uint16_t cl; memcpy(&cl, p, 2); p += 2;
    if (end - p < cl) return NULL;

    int32_t q[MDCT_COEFFS];
    if (nq > 0 && rice_decode(p, cl, cfk, nq, q) < 0) return NULL;
    p += cl;

    /* Dequantise the coded bands using the cumulative scalefactors; the rest
       stay zero. */
    memset(coeffs, 0, sizeof(float) * MDCT_COEFFS);
    int sf = 0, si = 0, qi = 0;
    for (int b = 0; b < BARK_BANDS; b++) {
        if (!((band_mask >> b) & 1)) continue;
        sf += sf_delta[si++];
        if (sf < 0)   sf = 0;
        if (sf > 255) sf = 255;
        float s = mdct_sf_to_step(sf);
        for (int k = mdct_band_start(b); k < mdct_band_start(b + 1); k++)
            coeffs[k] = (float)q[qi++] * s;
    }

    return p;
}

/* Decode one hop (flags byte + per-channel blocks) and emit MDCT_HOP
   reconstructed interleaved samples (delayed by one hop relative to the
   encoder's input, which is inherent to overlap-add). */
static const uint8_t* decode_hq_hop(AetherDecoder *dec,
                                    const uint8_t *p, const uint8_t *end,
                                    int channels, int32_t *pcm_out) {
    if (end - p < 1) return NULL;
    uint8_t flags = *p++;

    static float coef[2][MDCT_COEFFS];   /* single-threaded codec */
    for (int c = 0; c < channels; c++) {
        p = parse_channel_hq(p, end, coef[c]);
        if (!p) return NULL;
    }

    /* Mid/side back to left/right in the coefficient domain: the exact inverse
       of the encoder's m=(L+R)/2, s=(L-R)/2 is L=m+s, R=m-s. */
    if (channels == 2 && (flags & AETHER_FLAG_MID_SIDE)) {
        for (int k = 0; k < MDCT_COEFFS; k++) {
            float m = coef[0][k], s = coef[1][k];
            coef[0][k] = m + s;
            coef[1][k] = m - s;
        }
    }

    const float *w = mdct_window();
    for (int c = 0; c < channels; c++) {
        float y[MDCT_SIZE], out[MDCT_HOP];
        mdct_inverse(coef[c], y);
        for (int n = 0; n < MDCT_HOP; n++)
            out[n] = dec->ola[c][n] + y[n] * w[n];
        for (int n = 0; n < MDCT_HOP; n++)
            dec->ola[c][n] = y[MDCT_HOP + n] * w[MDCT_HOP + n];

        for (int i = 0; i < MDCT_HOP; i++) {
            float v = out[i];
            if (v >  8388607.0f) v =  8388607.0f;
            if (v < -8388608.0f) v = -8388608.0f;
            pcm_out[i * channels + c] = (int32_t)lrintf(v);
        }
    }

    return p;
}

/* ---- Public API --------------------------------------------------------- */

/* Returns the number of interleaved int32 values written to pcm_out
   (frame_samples * channels), or -1 on error. */
int aether_decoder_decode(AetherDecoder *dec, const AetherPacket *pkt,
                          int32_t *pcm_out, int max_samples) {
    int mode = pkt->hdr.mode;
    if (mode != AETHER_MODE_NL && mode != AETHER_MODE_HQ) return -1;

    /* A mode change invalidates the overlap history. */
    if (dec->last_mode != -1 && mode != dec->last_mode)
        aether_decoder_flush(dec);
    dec->last_mode = mode;

    const uint8_t *p   = pkt->payload;
    const uint8_t *end = pkt->payload + pkt->hdr.payload_size;
    int channels = pkt->hdr.channels;
    if (channels != 1 && channels != 2) return -1;
    if (end - p < 2) return -1;

    uint16_t fs;
    memcpy(&fs, p, 2); p += 2;
    int n = fs;
    if (n <= 0) return -1;
    if ((long)n * channels > max_samples) return -1;

    if (mode == AETHER_MODE_NL) {
        if (n > LPC_FRAME_SIZE) return -1;
        if (end - p < 1) return -1;
        uint8_t flags = *p++;

        static int32_t chan[2][LPC_FRAME_SIZE];   /* single-threaded codec */
        for (int c = 0; c < channels; c++) {
            p = decode_channel_nl(p, end, n, chan[c]);
            if (!p) return -1;
        }

        if (channels == 2 && (flags & AETHER_FLAG_MID_SIDE)) {
            /* m=(l+r)>>1, s=l-r. l+r = 2m + parity, and (l+r) and (l-r) share
               parity, so the lost low bit is s&1: l = m + ((s + (s&1)) >> 1),
               r = l - s. Pure integer arithmetic — exact. */
            for (int i = 0; i < n; i++) {
                int32_t m = chan[0][i], s = chan[1][i];
                int32_t l = m + ((s + (s & 1)) >> 1);
                pcm_out[i * channels]     = l;
                pcm_out[i * channels + 1] = l - s;
            }
        } else {
            for (int c = 0; c < channels; c++)
                for (int i = 0; i < n; i++)
                    pcm_out[i * channels + c] = chan[c][i];
        }
    } else {  /* AETHER_MODE_HQ — one or more hops back to back */
        if (n % MDCT_HOP != 0 || n > LPC_FRAME_SIZE) return -1;
        mdct_init(rate_from_code(pkt->hdr.sample_rate));
        int hops = n / MDCT_HOP;
        for (int h = 0; h < hops; h++) {
            p = decode_hq_hop(dec, p, end, channels,
                              pcm_out + (size_t)h * MDCT_HOP * channels);
            if (!p) return -1;
        }
    }

    return n * channels;
}

void aether_decoder_flush(AetherDecoder *dec) {
    dec->last_mode = -1;
    memset(dec->ola, 0, sizeof(dec->ola));
}

void aether_decoder_destroy(AetherDecoder *dec) { free(dec); }
