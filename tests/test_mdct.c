/* MDCT correctness: window validity, perfect reconstruction, Bark table sanity.
   NOTE on the Phase 3 checkpoint wording "inverse_mdct(mdct(x)) ~= x": that is
   not achievable for a single MDCT frame — the IMDCT of one frame is
   time-domain *aliased* by construction. Perfect reconstruction is a property
   of windowed overlap-add across successive frames (TDAC), which is what this
   test verifies. */
#include "codec_mdct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define SAMPLE_RATE 96000
#define NFRAMES     12

int main(void) {
    srand(123);
    mdct_init(SAMPLE_RATE);
    const float *w = mdct_window();

    /* --- 1. Princen-Bradley: w[n]^2 + w[n+N/2]^2 == 1 --------------------- */
    double worst_pb = 0.0;
    for (int n = 0; n < MDCT_SIZE / 2; n++) {
        double s = (double)w[n] * w[n]
                 + (double)w[n + MDCT_SIZE / 2] * w[n + MDCT_SIZE / 2];
        double err = fabs(s - 1.0);
        if (err > worst_pb) worst_pb = err;
    }
    printf("Princen-Bradley max deviation: %.3e\n", worst_pb);
    assert(worst_pb < 1e-6);

    /* --- 2. Bark table sanity -------------------------------------------- */
    assert(mdct_band_start(0) == 0);
    assert(mdct_band_start(BARK_BANDS) == MDCT_COEFFS);
    for (int b = 0; b < BARK_BANDS; b++)
        assert(mdct_band_start(b) <= mdct_band_start(b + 1));  // non-decreasing
    printf("Bark table: %d bands span %d bins, non-decreasing: OK\n",
           BARK_BANDS, MDCT_COEFFS);

    /* --- 3. Perfect reconstruction via windowed overlap-add --------------- */
    static float input[NFRAMES * MDCT_HOP];
    for (int i = 0; i < NFRAMES * MDCT_HOP; i++)
        input[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;

    float hist[MDCT_HOP];   // encoder: previous hop of input
    float ola[MDCT_HOP];    // decoder: overlap-add tail
    memset(hist, 0, sizeof(hist));
    memset(ola,  0, sizeof(ola));

    static float recon[NFRAMES * MDCT_HOP];
    memset(recon, 0, sizeof(recon));

    for (int f = 0; f < NFRAMES; f++) {
        const float *block = input + f * MDCT_HOP;

        /* analysis */
        float buf[MDCT_SIZE], windowed[MDCT_SIZE], coeffs[MDCT_COEFFS];
        memcpy(buf, hist, sizeof(hist));
        memcpy(buf + MDCT_HOP, block, MDCT_HOP * sizeof(float));
        for (int n = 0; n < MDCT_SIZE; n++) windowed[n] = buf[n] * w[n];
        mdct_forward(windowed, coeffs);
        memcpy(hist, block, sizeof(hist));

        /* synthesis */
        float y[MDCT_SIZE];
        mdct_inverse(coeffs, y);
        /* frame f emits the hop that arrived one frame earlier */
        if (f >= 1) {
            float *out = recon + (f - 1) * MDCT_HOP;
            for (int n = 0; n < MDCT_HOP; n++)
                out[n] = ola[n] + y[n] * w[n];
        }
        for (int n = 0; n < MDCT_HOP; n++)
            ola[n] = y[MDCT_HOP + n] * w[MDCT_HOP + n];
    }

    /* Compare the fully-overlapped region: hops 0 .. NFRAMES-2 */
    double max_err = 0.0;
    for (int i = 0; i < (NFRAMES - 1) * MDCT_HOP; i++) {
        double e = fabs((double)input[i] - (double)recon[i]);
        if (e > max_err) max_err = e;
    }
    printf("MDCT/IMDCT overlap-add max reconstruction error: %.3e\n", max_err);
    assert(max_err < 1e-5);

    printf("\n\xE2\x9C\x93 MDCT: window valid, transform reconstructs to %.1e (< 1e-5)\n",
           max_err);
    return 0;
}
