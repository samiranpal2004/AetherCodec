/* 2:1 resampler used by the ABR 48 kHz states.
   Checks frame counts, that a low tone survives a down/up round trip, and that
   content above the 48 kHz Nyquist is actually removed (which is the point of
   the anti-alias filter). */
#include "resample.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR     96000
#define FRAMES 8192
#define CH     2

static int32_t in[FRAMES * CH];
static int32_t mid[FRAMES * CH];
static int32_t out[FRAMES * CH];

static void gen_tone(double hz, double amp) {
    for (int i = 0; i < FRAMES; i++) {
        double v = amp * sin(2.0 * M_PI * hz * i / SR) * 8000000.0;
        in[i * CH] = in[i * CH + 1] = (int32_t)v;
    }
}

/* RMS over the steady-state region, skipping filter warm-up on both ends. */
static double rms(const int32_t *x, int frames, int skip) {
    double s = 0; int n = 0;
    for (int i = skip; i < frames - skip; i++) { s += (double)x[i*CH] * x[i*CH]; n++; }
    return n ? sqrt(s / n) : 0.0;
}

int main(void) {
    Resampler down, up;
    resample_init(&down, CH);
    resample_init(&up,   CH);

    /* --- frame counts ---------------------------------------------------- */
    gen_tone(1000.0, 0.5);
    int nd = resample_down2(&down, in, FRAMES, mid);
    assert(nd == FRAMES / 2);
    int nu = resample_up2(&up, mid, nd, out);
    assert(nu == FRAMES);
    printf("\xE2\x9C\x93 resample: %d -> %d -> %d frames\n", FRAMES, nd, nu);

    /* --- a 1 kHz tone survives the round trip ---------------------------- */
    double r_in  = rms(in,  FRAMES, 256);
    double r_out = rms(out, FRAMES, 256);
    double ratio = r_out / (r_in + 1e-9);
    printf("  1 kHz round-trip amplitude ratio: %.3f\n", ratio);
    assert(ratio > 0.90 && ratio < 1.10);

    /* --- content above 24 kHz must be rejected, not aliased -------------- */
    resample_reset(&down);
    gen_tone(35000.0, 0.5);                 /* well above 48k Nyquist */
    double r_hi_in = rms(in, FRAMES, 256);
    nd = resample_down2(&down, in, FRAMES, mid);
    double r_hi_out = rms(mid, nd, 256);
    double atten_db = 20.0 * log10((r_hi_out + 1e-9) / (r_hi_in + 1e-9));
    printf("  35 kHz attenuation through decimator: %.1f dB\n", atten_db);
    assert(atten_db < -40.0);               /* stopband, not folded back in */

    printf("\nAll resampler tests passed.\n");
    return 0;
}
