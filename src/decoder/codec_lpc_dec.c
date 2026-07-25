#include "codec_lpc.h"
#include <string.h>

int lpc_decode_frame(const LPCFrameHeader *hdr, const int32_t *residuals,
                     int n, int32_t *out) {
    int order = hdr->order;

    /* Restore warm-up samples */
    for (int i = 0; i < order; i++)
        out[i] = hdr->warmup[i];

    /* LPC synthesis: x[n] = e[n] + round(sum(a[k]*x[n-k]) >> SHIFT).
       Mirrors the encoder's integer arithmetic exactly -> bit-perfect. */
    for (int i = order; i < n; i++) {
        int64_t predicted = 0;
        for (int k = 0; k < order; k++)
            predicted += (int64_t)hdr->coeffs[k] * (int64_t)out[i - k - 1];
        predicted >>= LPC_COEFF_SHIFT;
        out[i] = residuals[i] + (int32_t)predicted;
    }

    return 0;
}

/* ---- Rice entropy coding (decode side) ---------------------------------- */

static inline int32_t zigzag_decode(uint32_t u) {
    return (u & 1) ? -(int32_t)((u + 1) >> 1) : (int32_t)(u >> 1);
}

typedef struct { const uint8_t *buf; int len; int byte_pos; int bit_pos; } BitReader;

static void br_init(BitReader *br, const uint8_t *buf, int len) {
    br->buf = buf; br->len = len; br->byte_pos = 0; br->bit_pos = 0;
}

static int br_read_bit(BitReader *br) {
    if (br->byte_pos >= br->len) return -1;
    int bit = (br->buf[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    if (++br->bit_pos == 8) { br->bit_pos = 0; br->byte_pos++; }
    return bit;
}

int rice_decode(const uint8_t *bitstream, int byte_len, int k,
                int n, int32_t *out) {
    BitReader br;
    br_init(&br, bitstream, byte_len);

    for (int i = 0; i < n; i++) {
        /* Unary quotient */
        uint32_t q = 0;
        int bit;
        while ((bit = br_read_bit(&br)) == 1) q++;
        if (bit < 0) return -1;

        /* k-bit remainder */
        uint32_t r = 0;
        for (int b = k - 1; b >= 0; b--) {
            bit = br_read_bit(&br);
            if (bit < 0) return -1;
            r |= (uint32_t)bit << b;
        }

        out[i] = zigzag_decode((q << k) | r);
    }
    return 0;
}
