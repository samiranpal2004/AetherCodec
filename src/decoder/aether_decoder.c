#include "aether_decoder.h"
#include "codec_lpc.h"
#include <stdlib.h>
#include <string.h>

struct AetherDecoder {
    int last_mode;
};

AetherDecoder* aether_decoder_create(void) {
    AetherDecoder *dec = calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    dec->last_mode = -1;
    return dec;
}

/* Decode one channel block starting at p (end = payload end).
   Writes n samples to chan_out. Returns new read pointer, or NULL on error. */
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

/* Returns the number of interleaved int32 values written to pcm_out
   (frame_samples * channels), or -1 on error. */
int aether_decoder_decode(AetherDecoder *dec, const AetherPacket *pkt,
                          int32_t *pcm_out, int max_samples) {
    if (pkt->hdr.mode != AETHER_MODE_NL) return -1;   // Phase 2: NL only
    dec->last_mode = pkt->hdr.mode;

    const uint8_t *p   = pkt->payload;
    const uint8_t *end = pkt->payload + pkt->hdr.payload_size;
    int channels = pkt->hdr.channels;
    if (channels != 1 && channels != 2) return -1;
    if (end - p < 2) return -1;

    uint16_t fs;
    memcpy(&fs, p, 2); p += 2;
    int n = fs;
    if (n <= 0 || n > LPC_FRAME_SIZE) return -1;
    if ((long)n * channels > max_samples) return -1;

    if (channels == 2) {
        int32_t left[LPC_FRAME_SIZE], right[LPC_FRAME_SIZE];
        p = decode_channel_nl(p, end, n, left);
        if (!p) return -1;
        p = decode_channel_nl(p, end, n, right);
        if (!p) return -1;
        for (int i = 0; i < n; i++) {
            pcm_out[i * 2]     = left[i];
            pcm_out[i * 2 + 1] = right[i];
        }
    } else {
        p = decode_channel_nl(p, end, n, pcm_out);
        if (!p) return -1;
    }

    return n * channels;
}

void aether_decoder_flush(AetherDecoder *dec) { dec->last_mode = -1; }

void aether_decoder_destroy(AetherDecoder *dec) { free(dec); }
