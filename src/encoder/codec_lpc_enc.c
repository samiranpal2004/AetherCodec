#include "codec_lpc.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

int lpc_select_order(const int32_t *samples, int n) {
    /* Estimate signal complexity via zero-crossing rate */
    int zcr = 0;
    for (int i = 1; i < n; i++)
        if ((samples[i] >= 0) != (samples[i-1] >= 0)) zcr++;

    double zcr_rate = (double)zcr / n;
    if (zcr_rate > 0.3)  return 32;   // high frequency content
    if (zcr_rate > 0.15) return 16;
    return 8;                          // smooth / bass-heavy
}

int lpc_encode_frame(const int32_t *samples, int n,
                     LPCFrameHeader *hdr_out, int32_t *residuals) {
    int order = lpc_select_order(samples, n);
    if (order > n) order = n;          // guard tiny frames
    hdr_out->order = order;

    /* Store warm-up samples verbatim (needed for decoder) */
    for (int i = 0; i < order; i++)
        hdr_out->warmup[i] = samples[i];

    /* Step 1: Autocorrelation R[0..order].
       int64 products accumulated in double; only affects predictor quality,
       never losslessness (residuals are computed with exact integer math). */
    double R[LPC_MAX_ORDER + 1] = {0};
    for (int lag = 0; lag <= order; lag++) {
        double sum = 0;
        for (int i = lag; i < n; i++)
            sum += (double)samples[i] * (double)samples[i - lag];
        R[lag] = sum;
    }

    if (R[0] == 0.0) {
        /* Silence frame — all residuals are zero */
        memset(residuals, 0, n * sizeof(int32_t));
        memset(hdr_out->coeffs, 0, order * sizeof(int32_t));
        return 0;
    }

    /* Step 2: Levinson-Durbin recursion → LPC coefficients */
    double a[LPC_MAX_ORDER + 1] = {0};  // double precision during solve
    double err = R[0];

    for (int i = 1; i <= order; i++) {
        double lambda = 0;
        for (int j = 1; j <= i - 1; j++)
            lambda += a[j] * R[i - j];
        lambda = (R[i] - lambda) / err;

        a[i] = lambda;
        for (int j = 1; j <= i / 2; j++) {
            double tmp = a[j] - lambda * a[i - j];
            a[i - j]   = a[i - j] - lambda * a[j];
            a[j]       = tmp;
        }
        err *= 1.0 - lambda * lambda;
        if (err <= 0.0) { order = i - 1; hdr_out->order = order; break; }
    }

    /* Step 3: Quantize coefficients to Q15 fixed-point */
    for (int i = 0; i < order; i++)
        hdr_out->coeffs[i] = (int32_t)(a[i + 1] * (1 << LPC_COEFF_SHIFT));

    /* Step 4: Residuals  e[n] = x[n] - round(sum(a[k]*x[n-k]) >> SHIFT) */
    for (int i = 0; i < order; i++)
        residuals[i] = 0;  // warm-up residuals not transmitted

    for (int i = order; i < n; i++) {
        int64_t predicted = 0;
        for (int k = 0; k < order; k++)
            predicted += (int64_t)hdr_out->coeffs[k] * (int64_t)samples[i - k - 1];
        predicted >>= LPC_COEFF_SHIFT;
        residuals[i] = samples[i] - (int32_t)predicted;
    }

    return 0;
}

/* ---- Rice entropy coding (encode side) ---------------------------------- */

/* Zigzag: n >= 0 -> 2n,  n < 0 -> 2|n|-1  (maps signed to unsigned) */
static inline uint32_t zigzag_encode(int32_t n) {
    return (n >= 0) ? (uint32_t)n * 2u : (uint32_t)(-(n + 1)) * 2u + 1u;
}

int rice_select_param(const int32_t *residuals, int n) {
    if (n <= 0) return 0;
    /* Optimal Rice param k ≈ log2(mean(|residuals|)) */
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += fabs((double)residuals[i]);
    double mean = sum / n;
    if (mean < 1.0) return 0;
    int k = (int)(log2(mean) - 0.5);
    if (k < 0)  k = 0;
    if (k > LPC_RICE_MAX_PARAM) k = LPC_RICE_MAX_PARAM;
    return k;
}

/* Simple MSB-first bit-packing writer */
typedef struct { uint8_t *buf; int cap; int byte_pos; int bit_pos; } BitWriter;

static void bw_init(BitWriter *bw, uint8_t *buf, int cap) {
    bw->buf = buf; bw->cap = cap; bw->byte_pos = 0; bw->bit_pos = 0;
}

static int bw_write_bit(BitWriter *bw, int bit) {
    if (bw->byte_pos >= bw->cap) return -1;
    if (bit) bw->buf[bw->byte_pos] |= (uint8_t)(1 << (7 - bw->bit_pos));
    if (++bw->bit_pos == 8) { bw->bit_pos = 0; bw->byte_pos++; }
    return 0;
}

static int bw_flush(BitWriter *bw) {
    if (bw->bit_pos > 0) bw->byte_pos++;
    return bw->byte_pos;
}

int rice_encode(const int32_t *residuals, int n, int k,
                uint8_t *out, int max_bytes) {
    BitWriter bw;
    bw_init(&bw, out, max_bytes);
    memset(out, 0, max_bytes);

    const uint32_t mask = (k < 32) ? ((1u << k) - 1u) : 0xFFFFFFFFu;

    for (int i = 0; i < n; i++) {
        uint32_t u = zigzag_encode(residuals[i]);
        uint32_t q = u >> k;      // quotient (unary coded)
        uint32_t r = u & mask;    // remainder (k-bit binary)

        /* q ones then a zero (unary) */
        for (uint32_t j = 0; j < q; j++)
            if (bw_write_bit(&bw, 1) < 0) return -1;
        if (bw_write_bit(&bw, 0) < 0) return -1;

        /* k-bit remainder, MSB first */
        for (int b = k - 1; b >= 0; b--)
            if (bw_write_bit(&bw, (r >> b) & 1) < 0) return -1;
    }

    return bw_flush(&bw);
}
