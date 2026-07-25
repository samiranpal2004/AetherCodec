#include "aether_encoder.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Payload layouts.

   Near-Lossless (mode 0):
     [frame_samples : u16][flags : u8]
       flags bit0: 1 = channels are mid/side, 0 = left/right (mono: always 0)
     per coded channel:
       [order : u8][rice_k : u8][wasted : u8]
       [coeffs : order * i32][warmup : order * i32]
       [rice_len : u16][rice_bytes : rice_len]

     `wasted` is the count of low-order zero bits shared by every sample in the
     channel (FLAC's "wasted bits"). Many 24-bit masters are 16-20 real bits
     padded with zeros; shifting them out before LPC removes 4-8 bits/sample
     from every residual at exactly zero cost to losslessness.

     Mid/side is the lossless integer pair m=(l+r)>>1, s=l-r (the parity of s
     recovers the bit dropped by the shift). It is chosen per frame by a bit
     estimate — correlated stereo drops 10-30%, hard-panned or uncorrelated
     material simply stays L/R, so it can never lose.

   Perceptual HQ (mode 1):
     [frame_samples : u16]   (total samples: 1..4 hops, multiple of MDCT_HOP)
     per hop:
       [flags : u8]          bit0: 1 = coefficients are mid/side
       per coded channel:
         [band_mask : u64 LE]                        bit b set = band b is coded
         [sf_rice_k : u8][sf_len : u16][sf_bytes]    scalefactor deltas, coded bands
         [cf_rice_k : u8][cf_len : u16][cf_bytes]    quantised coeffs, coded bands

     Batching several hops into one packet exists because HQ at one hop per
     packet sends 187 packets/s at 96 kHz; each carries 24 bytes of header+CRC,
     ~36 kbps of pure overhead — 15%+ of a weak link. Four hops per packet
     (matching NL's 2048-sample duration) cuts that to ~9 kbps and quarters the
     per-packet transport cost.

     HQ mid/side is applied in the TRANSFORM domain (m=(L+R)/2, s=(L-R)/2 on
     MDCT coefficients), not on samples: the transform is linear so the two are
     equivalent, but transform-domain switching keeps the decoder's overlap-add
     history in L/R space, so the per-frame decision can flip freely without
     smearing representations across the window boundary.

   Both are self-describing given the packet header (mode, channels).

   HQ entropy stage: the HLD calls for Huffman with "fixed tables v1.0", but the
   docs never specify those tables and untrained ones would be arbitrary. The
   already-proven Rice coder is used instead — it is adaptive per frame, needs
   no shipped tables, and suits the near-Laplacian distribution of quantised
   MDCT coefficients.

   The band mask is not in the HLD. It is needed because Rice never codes a
   symbol in zero bits: a quantised-to-zero coefficient still costs the unary
   terminator. At 96 kHz the absolute threshold of hearing correctly zeroes
   everything above ~17.6 kHz — roughly 63% of the 512 bins — so those dead bins
   put a hard ~500 kbps floor under HQ-96k that coarsening the quantiser could
   not get below, which is more than a real RFCOMM link was carrying. Skipping
   them costs 8 bytes per channel per frame and cut the measured broadband rate
   from ~1050 to ~595 kbps at identical SNR. */

#define AETHER_FLAG_MID_SIDE 0x01

struct AetherEncoder {
    int      mode;
    int      sample_rate;
    int      bit_depth;
    int      channels;
    uint32_t sequence;
    float    hist[2][MDCT_HOP];   // HQ: previous hop per channel (overlap)
};

static uint8_t rate_to_code(int sample_rate) {
    switch (sample_rate) {
        case 44100: return AETHER_RATE_44100;
        case 48000: return AETHER_RATE_48000;
        case 88200: return AETHER_RATE_88200;
        default:    return AETHER_RATE_96000;
    }
}

AetherEncoder* aether_encoder_create(int mode, int sample_rate,
                                     int bit_depth, int channels) {
    AetherEncoder *enc = calloc(1, sizeof(*enc));
    if (!enc) return NULL;
    enc->mode        = mode;
    enc->sample_rate = sample_rate;
    enc->bit_depth   = bit_depth;
    enc->channels    = channels;
    enc->sequence    = 0;
    mdct_init(sample_rate);   // idempotent; needed if/when HQ is selected
    return enc;
}

/* ---- Near-Lossless ------------------------------------------------------ */

/* Low-order zero bits common to every sample: content mastered at fewer real
   bits than the container carries (16-in-24 is common). 0 for silence. */
static int nl_wasted_bits(const int32_t *x, int n) {
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) acc |= (uint32_t)x[i];
    if (acc == 0) return 0;
    int w = __builtin_ctz(acc);
    return w > 30 ? 30 : w;
}

/* Approximate Rice bit cost of a channel under a cheap fixed 2nd-order
   predictor. Only used to CHOOSE between L/R and M/S — the real LPC then runs
   on the winning pair — so the absolute value doesn't matter, only the
   ordering, and both candidates are measured identically. */
static double nl_est_cost(const int32_t *x, int n) {
    double sum = 0.0;
    for (int i = 2; i < n; i++)
        sum += fabs((double)x[i] - 2.0 * x[i - 1] + x[i - 2]);
    double mean = n > 2 ? sum / (n - 2) : 0.0;
    return (double)n * (log2(mean + 1.0) + 1.6);
}

static int encode_channel_nl(uint8_t *p, int remaining,
                             const int32_t *chan, int n, int wasted) {
    LPCFrameHeader h = {0};
    int32_t res[LPC_FRAME_SIZE] = {0};
    lpc_encode_frame(chan, n, &h, res);
    int k = rice_select_param(res + h.order, n - h.order);

    int fixed = 3 + h.order * 8 + 2;
    if (remaining < fixed) return -1;

    uint8_t *start = p;
    *p++ = (uint8_t)h.order;
    *p++ = (uint8_t)k;
    *p++ = (uint8_t)wasted;
    memcpy(p, h.coeffs, (size_t)h.order * 4); p += h.order * 4;
    memcpy(p, h.warmup, (size_t)h.order * 4); p += h.order * 4;

    int rice_cap = remaining - fixed;
    int rbytes = rice_encode(res + h.order, n - h.order, k, p + 2, rice_cap);
    if (rbytes < 0) return -1;
    uint16_t rl = (uint16_t)rbytes;
    memcpy(p, &rl, 2);
    p += 2 + rbytes;
    return (int)(p - start);
}

/* ---- Perceptual HQ ------------------------------------------------------ */

/* Entropy-code one channel's MDCT coefficients (already transformed, possibly
   mid/side). */
static int encode_channel_hq(uint8_t *p, int remaining, const float *coeffs) {
    float mask[BARK_BANDS], step[BARK_BANDS];
    mdct_masking_threshold(coeffs, mask);
    mdct_band_steps(mask, step);

    int32_t  q[MDCT_COEFFS];
    int32_t  sf_all[BARK_BANDS];
    uint64_t band_mask = 0;

    for (int b = 0; b < BARK_BANDS; b++) {
        int   sf = mdct_step_to_sf(step[b]);
        /* Quantise with the *reconstructed* step so the decoder matches bit for
           bit; using the unrounded step would desynchronise the two sides. */
        float s  = mdct_sf_to_step(sf);
        sf_all[b] = sf;

        int nz = 0;
        for (int k = mdct_band_start(b); k < mdct_band_start(b + 1); k++) {
            float v = coeffs[k] / s;
            if (v >  8388607.0f) v =  8388607.0f;
            if (v < -8388608.0f) v = -8388608.0f;
            q[k] = (int32_t)lrintf(v);
            if (q[k]) nz = 1;
        }
        if (nz) band_mask |= (uint64_t)1 << b;
    }

    /* Send only the bands that survived quantisation (see the header comment
       on the band mask). */
    int32_t sf_delta[BARK_BANDS];
    int32_t qc[MDCT_COEFFS];
    int nsf = 0, nq = 0, prev_sf = 0;
    for (int b = 0; b < BARK_BANDS; b++) {
        if (!((band_mask >> b) & 1)) continue;
        sf_delta[nsf++] = sf_all[b] - prev_sf;
        prev_sf = sf_all[b];
        for (int k = mdct_band_start(b); k < mdct_band_start(b + 1); k++)
            qc[nq++] = q[k];
    }

    uint8_t *start = p;
    if (remaining < 8 + 3 + 3) return -1;

    /* band mask, little-endian */
    for (int i = 0; i < 8; i++) *p++ = (uint8_t)(band_mask >> (8 * i));

    /* scalefactor block (coded bands only) */
    int sfk = rice_select_param(sf_delta, nsf);
    *p++ = (uint8_t)sfk;
    int cap = remaining - (int)(p - start) - 2;
    int sfbytes = rice_encode(sf_delta, nsf, sfk, p + 2, cap);
    if (sfbytes < 0) return -1;
    uint16_t sl = (uint16_t)sfbytes;
    memcpy(p, &sl, 2); p += 2 + sfbytes;

    /* coefficient block (coded bands only) */
    if (remaining - (int)(p - start) < 3) return -1;
    int cfk = rice_select_param(qc, nq);
    *p++ = (uint8_t)cfk;
    cap = remaining - (int)(p - start) - 2;
    int cfbytes = rice_encode(qc, nq, cfk, p + 2, cap);
    if (cfbytes < 0) return -1;
    uint16_t cl = (uint16_t)cfbytes;
    memcpy(p, &cl, 2); p += 2 + cfbytes;

    return (int)(p - start);
}

/* Encode one MDCT hop (flags byte + per-channel blocks). `pcm` points at the
   hop's interleaved samples. */
static int encode_hq_hop(AetherEncoder *enc, uint8_t *p, int remaining,
                         const int32_t *pcm) {
    const float *w = mdct_window();
    static float coef[2][MDCT_COEFFS];   /* single-threaded codec (see mdct.h) */

    for (int c = 0; c < enc->channels; c++) {
        float buf[MDCT_SIZE];
        /* previous hop, then this hop */
        memcpy(buf, enc->hist[c], MDCT_HOP * sizeof(float));
        for (int i = 0; i < MDCT_HOP; i++) {
            float s = (float)pcm[i * enc->channels + c];
            buf[MDCT_HOP + i] = s;
            enc->hist[c][i]   = s;
        }
        for (int n = 0; n < MDCT_SIZE; n++) buf[n] *= w[n];
        mdct_forward(buf, coef[c]);
    }

    uint8_t flags = 0;
    if (enc->channels == 2) {
        /* Estimate Rice cost of both pairings from the mean |coefficient|,
           using the energy-preserving rotation (1/sqrt2) for the estimate so
           neither pairing gets a free scale advantage. */
        double al = 0, ar = 0, am = 0, as2 = 0;
        for (int k = 0; k < MDCT_COEFFS; k++) {
            float l = coef[0][k], r = coef[1][k];
            al  += fabsf(l);
            ar  += fabsf(r);
            am  += fabsf((l + r) * 0.70710678f);
            as2 += fabsf((l - r) * 0.70710678f);
        }
        #define HQ_COST(x) log2((x) / MDCT_COEFFS + 1.0)
        if (HQ_COST(am) + HQ_COST(as2) < HQ_COST(al) + HQ_COST(ar)) {
            flags |= AETHER_FLAG_MID_SIDE;
            for (int k = 0; k < MDCT_COEFFS; k++) {
                float m = (coef[0][k] + coef[1][k]) * 0.5f;
                float s = (coef[0][k] - coef[1][k]) * 0.5f;
                coef[0][k] = m;
                coef[1][k] = s;
            }
        }
        #undef HQ_COST
    }

    uint8_t *start = p;
    if (remaining < 1) return -1;
    *p++ = flags; remaining--;

    for (int c = 0; c < enc->channels; c++) {
        int n = encode_channel_hq(p, remaining, coef[c]);
        if (n < 0) return -1;
        p += n; remaining -= n;
    }
    return (int)(p - start);
}

/* ---- Public API --------------------------------------------------------- */

int aether_encoder_encode(AetherEncoder *enc, const int32_t *pcm,
                          int frame_samples, AetherPacket *pkt_out) {
    if (enc->channels != 1 && enc->channels != 2) return -1;
    if (frame_samples <= 0) return -1;
    if (enc->mode == AETHER_MODE_NL && frame_samples > LPC_FRAME_SIZE) return -1;
    if (enc->mode == AETHER_MODE_HQ &&
        (frame_samples % MDCT_HOP != 0 || frame_samples > LPC_FRAME_SIZE))
        return -1;
    if (enc->mode != AETHER_MODE_NL && enc->mode != AETHER_MODE_HQ) return -1;

    memset(pkt_out, 0, sizeof(*pkt_out));
    pkt_out->hdr.magic        = AETHER_MAGIC;
    pkt_out->hdr.sequence     = enc->sequence++;
    pkt_out->hdr.timestamp_us = (uint32_t)aether_timestamp_us();
    pkt_out->hdr.mode         = (uint8_t)enc->mode;
    pkt_out->hdr.sample_rate  = rate_to_code(enc->sample_rate);
    pkt_out->hdr.bit_depth    = (uint8_t)enc->bit_depth;
    pkt_out->hdr.channels     = (uint8_t)enc->channels;

    uint8_t *p    = pkt_out->payload;
    int remaining = (int)sizeof(pkt_out->payload);

    uint16_t fs = (uint16_t)frame_samples;
    memcpy(p, &fs, 2); p += 2; remaining -= 2;

    if (enc->mode == AETHER_MODE_NL) {
        static int32_t chan[2][LPC_FRAME_SIZE];   /* codec is single-threaded */
        uint8_t flags = 0;

        for (int c = 0; c < enc->channels; c++)
            for (int i = 0; i < frame_samples; i++)
                chan[c][i] = pcm[i * enc->channels + c];

        if (enc->channels == 2) {
            static int32_t mid[LPC_FRAME_SIZE], side[LPC_FRAME_SIZE];
            for (int i = 0; i < frame_samples; i++) {
                int32_t l = chan[0][i], r = chan[1][i];
                mid[i]  = (l + r) >> 1;   /* arithmetic shift, floor */
                side[i] = l - r;          /* parity of s recovers the lost bit */
            }
            double cost_lr = nl_est_cost(chan[0], frame_samples)
                           + nl_est_cost(chan[1], frame_samples);
            double cost_ms = nl_est_cost(mid,  frame_samples)
                           + nl_est_cost(side, frame_samples);
            if (cost_ms < cost_lr) {
                flags |= AETHER_FLAG_MID_SIDE;
                memcpy(chan[0], mid,  (size_t)frame_samples * 4);
                memcpy(chan[1], side, (size_t)frame_samples * 4);
            }
        }

        if (remaining < 1) return -1;
        *p++ = flags; remaining--;

        for (int c = 0; c < enc->channels; c++) {
            int wasted = nl_wasted_bits(chan[c], frame_samples);
            if (wasted) {
                for (int i = 0; i < frame_samples; i++)
                    chan[c][i] >>= wasted;
            }
            int n = encode_channel_nl(p, remaining, chan[c],
                                      frame_samples, wasted);
            if (n < 0) return -1;
            p += n; remaining -= n;
        }
    } else {  /* AETHER_MODE_HQ — one or more hops back to back */
        int hops = frame_samples / MDCT_HOP;
        for (int h = 0; h < hops; h++) {
            int n = encode_hq_hop(enc, p, remaining,
                                  pcm + (size_t)h * MDCT_HOP * enc->channels);
            if (n < 0) return -1;
            p += n; remaining -= n;
        }
    }

    pkt_out->hdr.payload_size = (uint16_t)(p - pkt_out->payload);
    return 0;
}

void aether_encoder_set_mode(AetherEncoder *enc, int new_mode) {
    if (new_mode == enc->mode) return;
    enc->mode = new_mode;
    memset(enc->hist, 0, sizeof(enc->hist));   // overlap state is mode-specific
}

void aether_encoder_reconfigure(AetherEncoder *enc, int mode, int sample_rate) {
    if (!enc) return;
    if (mode == enc->mode && sample_rate == enc->sample_rate) return;
    enc->mode        = mode;
    enc->sample_rate = sample_rate;
    /* Overlap history is both mode- and rate-specific; the sequence counter
       deliberately survives (see the header comment). */
    memset(enc->hist, 0, sizeof(enc->hist));
    mdct_init(sample_rate);   // rebuilds the rate-dependent Bark/ATH tables
}

void aether_encoder_destroy(AetherEncoder *enc) { free(enc); }
