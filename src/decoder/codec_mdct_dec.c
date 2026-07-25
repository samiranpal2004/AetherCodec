/* MDCT synthesis. The shared window/Bark tables and the DCT-IV plan live in
   codec_mdct_enc.c; both objects link into the aether_codec library. */
#include "codec_mdct.h"
#include <string.h>

#define MDCT_M  MDCT_COEFFS        // 512
#define MDCT_H  (MDCT_SIZE / 4)    // 256

/* IMDCT = DCT-IV, rescale, then unfold (M -> 2M).
   DCT-IV is its own inverse up to scale: D*D = (M/2)*I, and FFTW's REDFT11 is
   2*D, so applying it twice yields 2M*u. Dividing by 2M recovers the folded
   vector, which unfolds to the time-aliased frame:
       u = [p, q]  ->  y = [ q, -q_R, -p_R, -p ]
   Windowing and overlap-adding successive frames then cancels the alias. */
void mdct_inverse(const float *coeffs, float *aliased_out) {
    float u[MDCT_M];
    mdct_dct4(coeffs, u);

    const float scale = 1.0f / (2.0f * (float)MDCT_M);
    for (int n = 0; n < MDCT_M; n++) u[n] *= scale;

    const int h = MDCT_H;   // N/4
    for (int n = 0; n < h; n++) {
        aliased_out[n]                 =  u[h + n];             //  q
        aliased_out[h + n]             = -u[MDCT_M - 1 - n];    // -q_R
        aliased_out[MDCT_M + n]        = -u[h - 1 - n];         // -p_R
        aliased_out[MDCT_M + h + n]    = -u[n];                 // -p
    }
}
