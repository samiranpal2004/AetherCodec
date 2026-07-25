#include "aether_decoder.h"
#include "codec_lpc.h"
#include "codec_mdct.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Payload layouts are documented in src/encoder/aether_encoder.c. */

struct AetherDecoder {
    int   last_mode;
    int   mdct_started;
    float ola[2][MDCT_HOP];   // HQ: overlap-add tail per channel
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
    if (end - p < 2) return NULL;
    LPCFrameHeader h = {0};
    h.order = *p++;
    int k   = *p++;
    if (h.order < 0 || h.order > LPC_MAX_ORDER || h.order > n) return NULL;

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
    return p;
}

/* ---- Perceptual HQ ------------------------------------------------------ */

/* Emits MDCT_HOP reconstructed samples for one channel (delayed by one frame
   relative to the encoder's input, which is inherent to overlap-add). */
static const uint8_t* decode_channel_hq(const uint8_t *p, const uint8_t *end,
                                        float *ola, float *out) {
    if (end - p < 3) return NULL;
    int sfk = *p++;
    uint16_t sl; memcpy(&sl, p, 2); p += 2;
    if (end - p < sl) return NULL;

    int32_t sf_delta[BARK_BANDS];
    if (rice_decode(p, sl, sfk, BARK_BANDS, sf_delta) < 0) return NULL;
    p += sl;

    if (end - p < 3) return NULL;
    int cfk = *p++;
    uint16_t cl; memcpy(&cl, p, 2); p += 2;
    if (end - p < cl) return NULL;

    int32_t q[MDCT_COEFFS];
    if (rice_decode(p, cl, cfk, MDCT_COEFFS, q) < 0) return NULL;
    p += cl;

    /* Dequantise band by band using the cumulative scalefactors. */
    float coeffs[MDCT_COEFFS];
    int sf = 0;
    for (int b = 0; b < BARK_BANDS; b++) {
        sf += sf_delta[b];
        if (sf < 0)   sf = 0;
        if (sf > 255) sf = 255;
        float s = mdct_sf_to_step(sf);
        for (int k = mdct_band_start(b); k < mdct_band_start(b + 1); k++)
            coeffs[k] = (float)q[k] * s;
    }

    float y[MDCT_SIZE];
    mdct_inverse(coeffs, y);

    const float *w = mdct_window();
    for (int n = 0; n < MDCT_HOP; n++)
        out[n] = ola[n] + y[n] * w[n];
    for (int n = 0; n < MDCT_HOP; n++)
        ola[n] = y[MDCT_HOP + n] * w[MDCT_HOP + n];

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
        int32_t chan[LPC_FRAME_SIZE];
        for (int c = 0; c < channels; c++) {
            p = decode_channel_nl(p, end, n, chan);
            if (!p) return -1;
            for (int i = 0; i < n; i++)
                pcm_out[i * channels + c] = chan[i];
        }
    } else {  /* AETHER_MODE_HQ */
        if (n != MDCT_HOP) return -1;
        mdct_init(rate_from_code(pkt->hdr.sample_rate));
        float out[MDCT_HOP];
        for (int c = 0; c < channels; c++) {
            p = decode_channel_hq(p, end, dec->ola[c], out);
            if (!p) return -1;
            for (int i = 0; i < n; i++) {
                float v = out[i];
                if (v >  8388607.0f) v =  8388607.0f;
                if (v < -8388608.0f) v = -8388608.0f;
                pcm_out[i * channels + c] = (int32_t)lrintf(v);
            }
        }
    }

    return n * channels;
}

void aether_decoder_flush(AetherDecoder *dec) {
    dec->last_mode = -1;
    memset(dec->ola, 0, sizeof(dec->ola));
}

void aether_decoder_destroy(AetherDecoder *dec) { free(dec); }
