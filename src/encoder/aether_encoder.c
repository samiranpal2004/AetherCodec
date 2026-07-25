#include "aether_encoder.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Payload layouts.

   Near-Lossless (mode 0):
     [frame_samples : u16]
     per channel:
       [order : u8][rice_k : u8]
       [coeffs : order * i32][warmup : order * i32]
       [rice_len : u16][rice_bytes : rice_len]

   Perceptual HQ (mode 1):
     [frame_samples : u16]   (== MDCT_HOP)
     per channel:
       [sf_rice_k : u8][sf_len : u16][sf_bytes]    Rice-coded scalefactor deltas
       [cf_rice_k : u8][cf_len : u16][cf_bytes]    Rice-coded quantised coeffs

   Both are self-describing given the packet header (mode, channels).

   HQ entropy stage: the HLD calls for Huffman with "fixed tables v1.0", but the
   docs never specify those tables and untrained ones would be arbitrary. The
   already-proven Rice coder is used instead — it is adaptive per frame, needs
   no shipped tables, and suits the near-Laplacian distribution of quantised
   MDCT coefficients. */

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

static int encode_channel_nl(uint8_t *p, int remaining,
                             const int32_t *chan, int n) {
    LPCFrameHeader h = {0};
    int32_t res[LPC_FRAME_SIZE] = {0};
    lpc_encode_frame(chan, n, &h, res);
    int k = rice_select_param(res + h.order, n - h.order);

    int fixed = 2 + h.order * 8 + 2;
    if (remaining < fixed) return -1;

    uint8_t *start = p;
    *p++ = (uint8_t)h.order;
    *p++ = (uint8_t)k;
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

/* win_buf: MDCT_SIZE already-windowed samples for this channel. */
static int encode_channel_hq(uint8_t *p, int remaining, const float *win_buf) {
    float coeffs[MDCT_COEFFS], mask[BARK_BANDS], step[BARK_BANDS];
    mdct_forward(win_buf, coeffs);
    mdct_masking_threshold(coeffs, mask);
    mdct_band_steps(mask, step);

    int32_t sf_delta[BARK_BANDS];
    int32_t q[MDCT_COEFFS];
    int prev_sf = 0;

    for (int b = 0; b < BARK_BANDS; b++) {
        int   sf = mdct_step_to_sf(step[b]);
        /* Quantise with the *reconstructed* step so the decoder matches bit for
           bit; using the unrounded step would desynchronise the two sides. */
        float s  = mdct_sf_to_step(sf);
        sf_delta[b] = sf - prev_sf;
        prev_sf = sf;

        for (int k = mdct_band_start(b); k < mdct_band_start(b + 1); k++) {
            float v = coeffs[k] / s;
            if (v >  8388607.0f) v =  8388607.0f;
            if (v < -8388608.0f) v = -8388608.0f;
            q[k] = (int32_t)lrintf(v);
        }
    }

    int sfk = rice_select_param(sf_delta, BARK_BANDS);
    int cfk = rice_select_param(q, MDCT_COEFFS);

    uint8_t *start = p;
    if (remaining < 6) return -1;

    /* scalefactor block */
    *p++ = (uint8_t)sfk;
    int cap = remaining - (int)(p - start) - 2;
    int sfbytes = rice_encode(sf_delta, BARK_BANDS, sfk, p + 2, cap);
    if (sfbytes < 0) return -1;
    uint16_t sl = (uint16_t)sfbytes;
    memcpy(p, &sl, 2); p += 2 + sfbytes;

    /* coefficient block */
    if (remaining - (int)(p - start) < 3) return -1;
    *p++ = (uint8_t)cfk;
    cap = remaining - (int)(p - start) - 2;
    int cfbytes = rice_encode(q, MDCT_COEFFS, cfk, p + 2, cap);
    if (cfbytes < 0) return -1;
    uint16_t cl = (uint16_t)cfbytes;
    memcpy(p, &cl, 2); p += 2 + cfbytes;

    return (int)(p - start);
}

/* ---- Public API --------------------------------------------------------- */

int aether_encoder_encode(AetherEncoder *enc, const int32_t *pcm,
                          int frame_samples, AetherPacket *pkt_out) {
    if (enc->channels != 1 && enc->channels != 2) return -1;
    if (frame_samples <= 0) return -1;
    if (enc->mode == AETHER_MODE_NL  && frame_samples > LPC_FRAME_SIZE) return -1;
    if (enc->mode == AETHER_MODE_HQ  && frame_samples != MDCT_HOP)      return -1;
    if (enc->mode != AETHER_MODE_NL && enc->mode != AETHER_MODE_HQ)     return -1;

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
        int32_t chan[LPC_FRAME_SIZE];
        for (int c = 0; c < enc->channels; c++) {
            for (int i = 0; i < frame_samples; i++)
                chan[i] = pcm[i * enc->channels + c];
            int n = encode_channel_nl(p, remaining, chan, frame_samples);
            if (n < 0) return -1;
            p += n; remaining -= n;
        }
    } else {  /* AETHER_MODE_HQ */
        const float *w = mdct_window();
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

            int n = encode_channel_hq(p, remaining, buf);
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
