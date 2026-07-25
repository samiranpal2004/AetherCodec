#include "aether_encoder.h"
#include "codec_lpc.h"
#include <stdlib.h>
#include <string.h>

/* Payload layout (Near-Lossless):
     [frame_samples : u16]
     per channel:
       [order : u8][rice_k : u8]
       [coeffs : order * i32][warmup : order * i32]
       [rice_len : u16][rice_bytes : rice_len]
   channels comes from the packet header; frame_samples from the prefix, so the
   stream is self-describing and not locked to a fixed frame size. */

struct AetherEncoder {
    int      mode;
    int      sample_rate;
    int      bit_depth;
    int      channels;
    uint32_t sequence;
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
    return enc;
}

/* Encode one channel into p (with `remaining` bytes free).
   Returns bytes written, or -1 if it would not fit. */
static int encode_channel_nl(uint8_t *p, int remaining,
                             const int32_t *chan, int n) {
    LPCFrameHeader h = {0};
    int32_t res[LPC_FRAME_SIZE] = {0};
    lpc_encode_frame(chan, n, &h, res);
    int k = rice_select_param(res + h.order, n - h.order);

    int fixed = 2 + h.order * 8 + 2;      // order+k + coeffs + warmup + rice_len
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

int aether_encoder_encode(AetherEncoder *enc, const int32_t *pcm,
                          int frame_samples, AetherPacket *pkt_out) {
    if (enc->mode != AETHER_MODE_NL) return -1;   // Phase 2: NL only (HQ later)
    if (frame_samples <= 0 || frame_samples > LPC_FRAME_SIZE) return -1;
    if (enc->channels != 1 && enc->channels != 2) return -1;

    memset(pkt_out, 0, sizeof(*pkt_out));
    pkt_out->hdr.magic        = AETHER_MAGIC;
    pkt_out->hdr.sequence     = enc->sequence++;
    pkt_out->hdr.timestamp_us = (uint32_t)aether_timestamp_us();
    pkt_out->hdr.mode         = (uint8_t)enc->mode;
    pkt_out->hdr.sample_rate  = rate_to_code(enc->sample_rate);
    pkt_out->hdr.bit_depth    = (uint8_t)enc->bit_depth;
    pkt_out->hdr.channels     = (uint8_t)enc->channels;

    uint8_t *p   = pkt_out->payload;
    int remaining = (int)sizeof(pkt_out->payload);

    uint16_t fs = (uint16_t)frame_samples;
    memcpy(p, &fs, 2); p += 2; remaining -= 2;

    if (enc->channels == 2) {
        int32_t left[LPC_FRAME_SIZE], right[LPC_FRAME_SIZE];
        for (int i = 0; i < frame_samples; i++) {
            left[i]  = pcm[i * 2];
            right[i] = pcm[i * 2 + 1];
        }
        int nl_ = encode_channel_nl(p, remaining, left, frame_samples);
        if (nl_ < 0) return -1;
        p += nl_; remaining -= nl_;
        int nr = encode_channel_nl(p, remaining, right, frame_samples);
        if (nr < 0) return -1;
        p += nr; remaining -= nr;
    } else {
        int nc = encode_channel_nl(p, remaining, pcm, frame_samples);
        if (nc < 0) return -1;
        p += nc;
    }

    pkt_out->hdr.payload_size = (uint16_t)(p - pkt_out->payload);
    return 0;
}

void aether_encoder_set_mode(AetherEncoder *enc, int new_mode) {
    enc->mode = new_mode;
}

void aether_encoder_destroy(AetherEncoder *enc) { free(enc); }
