/* MDCT analysis + psychoacoustic model.
   Also hosts the shared tables (window, Bark map, ATH) and the DCT-IV plan used
   by both the encoder and the decoder — they link into the same aether_codec
   library, so the decoder pulls mdct_window()/mdct_dct4() from here. */
#include "codec_mdct.h"
#include <fftw3.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MDCT_M  MDCT_COEFFS        // 512
#define MDCT_H  (MDCT_SIZE / 4)    // 256, quarter length used by the fold

/* Signal-to-mask ratio — the operating point of the whole HQ mode.
   The masking threshold is placed SMR dB below the spread masker energy, so
   this constant is effectively the rate/quality control: each 6 dB costs about
   1 bit per significant coefficient.
   The HLD suggests ~12 dB, but that yields only ~13 dB SNR at ~490 kbps —
   barely half the PRD's 900-1,100 kbps HQ budget, i.e. it discards quality the
   Bluetooth link can comfortably carry. 30 dB puts the measured bitrate inside
   the target band (see tests/test_codec_hq.c). */
#define MDCT_SMR_DB     30.0f
#define MDCT_SMR_LINEAR 1.0e-3f    /* 10^(-30/10) */

static float kbd_window[MDCT_SIZE];
static int   bark_of_bin[MDCT_COEFFS];
static int   band_start[BARK_BANDS + 1];
static float ath_energy[BARK_BANDS];      // absolute threshold, energy units
static int   mdct_ready = 0;

/* DCT-IV plan + its dedicated buffers (single-threaded use). */
static float      *dct_in, *dct_out;
static fftwf_plan  dct_plan;

/* Modified Bessel function of the first kind, order 0.
   I0(x) = sum_k ((x/2)^k / k!)^2.  The version printed in the implementation
   doc squares `term` in place each iteration, which is wrong (it computes a
   doubly-squared series); this is the correct recurrence. */
static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k <= 50; k++) {
        term *= (x / 2.0) / k;      // term = (x/2)^k / k!
        sum  += term * term;        // add ((x/2)^k / k!)^2
    }
    return sum;
}

/* Kaiser-Bessel Derived window.
   Built from a symmetric Kaiser kernel of length N/2+1 whose running sum is
   normalised and square-rooted; that construction is what guarantees
   w[n]^2 + w[n+N/2]^2 == 1 (Princen-Bradley), required for exact
   reconstruction after overlap-add. */
static void init_kbd_window(void) {
    const int   M    = MDCT_SIZE / 2;   // 512
    const double beta = 4.0 * M_PI;     // AAC long-window alpha=4 -> beta=4*pi
    double kaiser[MDCT_SIZE / 2 + 1];
    double denom = bessel_i0(beta);

    for (int j = 0; j <= M; j++) {
        double r   = (2.0 * j / M) - 1.0;         // -1 .. +1, symmetric
        double arg = beta * sqrt(1.0 - r * r);
        kaiser[j]  = bessel_i0(arg) / denom;
    }

    double total = 0.0;
    for (int j = 0; j <= M; j++) total += kaiser[j];

    double run = 0.0;
    for (int n = 0; n < M; n++) {
        run += kaiser[n];
        float w = (float)sqrt(run / total);
        kbd_window[n]                = w;   // rising half
        kbd_window[MDCT_SIZE - 1 - n] = w;  // falling half (symmetric)
    }
}

/* Map each MDCT bin to a Bark band, then record band start bins.
   The literal Bark formula saturates near 24 Bark around 15.5 kHz, so at 96 kHz
   (Nyquist 48 kHz) scaling by a fixed /24 would dump ~2/3 of the spectrum into
   the final band. We normalise by the Bark value at Nyquist instead, so the 64
   bands span the actual represented range. */
static void init_bark_table(int sample_rate) {
    const double nyq = sample_rate / 2.0;
    const double bark_max = 13.0 * atan(0.76 * nyq / 1000.0)
                          + 3.5 * atan((nyq / 7500.0) * (nyq / 7500.0));

    for (int k = 0; k < MDCT_COEFFS; k++) {
        double f    = (k + 0.5) * sample_rate / (double)MDCT_SIZE;
        double bark = 13.0 * atan(0.76 * f / 1000.0)
                    + 3.5 * atan((f / 7500.0) * (f / 7500.0));
        int band = (int)(bark / bark_max * BARK_BANDS);
        if (band < 0) band = 0;
        if (band >= BARK_BANDS) band = BARK_BANDS - 1;
        bark_of_bin[k] = band;
    }

    /* band_start[b] = first bin of band b; empty bands collapse to the next
       start so band_start stays non-decreasing and the [start,end) loops below
       simply iterate zero bins. */
    int k = 0;
    for (int b = 0; b < BARK_BANDS; b++) {
        while (k < MDCT_COEFFS && bark_of_bin[k] < b) k++;
        band_start[b] = k;
    }
    band_start[BARK_BANDS] = MDCT_COEFFS;
}

/* Absolute threshold of hearing, converted from dB SPL to coefficient-energy
   units by pinning digital full scale (2^23) to ~96 dB SPL. The raw formula
   explodes past Nyquist at 96 kHz (1e-3 * f_kHz^4 reaches ~5300 dB at 48 kHz,
   which overflows powf), so the dB value is clamped before conversion. */
static void init_ath_table(int sample_rate) {
    const double fs_energy = (double)(1 << 23) * (double)(1 << 23);

    for (int b = 0; b < BARK_BANDS; b++) {
        int s = band_start[b], e = band_start[b + 1];
        int mid = (e > s) ? (s + e) / 2 : s;
        double f = (mid + 0.5) * sample_rate / (double)MDCT_SIZE / 1000.0; // kHz
        if (f < 0.02) f = 0.02;

        double db = 3.64 * pow(f, -0.8)
                  - 6.5 * exp(-0.6 * (f - 3.3) * (f - 3.3))
                  + 1e-3 * pow(f, 4.0);
        if (db < -30.0) db = -30.0;
        if (db > 150.0) db = 150.0;

        ath_energy[b] = (float)(fs_energy * pow(10.0, (db - 96.0) / 10.0));
    }
}

/* Rebuilds the rate-dependent tables whenever the sample rate changes.

   This used to bail out on `if (mdct_ready) return;` — i.e. the Bark map and
   ATH latched to whichever rate happened to be requested first and were never
   rebuilt. Under ABR that is wrong on both sides and in two different ways:

     - Encoder: created at 96 kHz, so after a switch to HQ-48k it kept the
       96 kHz ATH. The threshold that should zero content above ~17.6 kHz then
       lands on bin indices that are 48 kHz bins, killing everything above
       ~8.8 kHz — an audible low-pass on every 48 kHz HQ state.
     - Encoder vs decoder: the decoder calls mdct_init() from the first HQ
       packet it sees. If the sender started in NL-96k and the first HQ packet
       to arrive was HQ-48k, the two ends latched *different* band tables, so
       band_start[] disagreed and dequantisation applied the wrong step to the
       wrong bins — decoded noise, not a subtle artifact.

   The window and the FFTW plan are rate-independent, so those stay one-shot. */
void mdct_init(int sample_rate) {
    static int mdct_rate = 0;

    if (!mdct_ready) {
        init_kbd_window();
        dct_in  = (float*)fftwf_malloc(sizeof(float) * MDCT_M);
        dct_out = (float*)fftwf_malloc(sizeof(float) * MDCT_M);
        /* REDFT11 == DCT-IV. FFTW's REDFT11 carries an extra factor of 2, i.e.
           it computes 2*D where D is the orthogonal-up-to-scale DCT-IV matrix. */
        dct_plan = fftwf_plan_r2r_1d(MDCT_M, dct_in, dct_out,
                                     FFTW_REDFT11, FFTW_ESTIMATE);
        mdct_ready = 1;
    }
    if (sample_rate != mdct_rate) {
        init_bark_table(sample_rate);
        init_ath_table(sample_rate);
        mdct_rate = sample_rate;
    }
}

const float* mdct_window(void) { return kbd_window; }
int mdct_band_start(int b)     { return band_start[b]; }

void mdct_dct4(const float *in, float *out) {
    memcpy(dct_in, in, sizeof(float) * MDCT_M);
    fftwf_execute(dct_plan);
    memcpy(out, dct_out, sizeof(float) * MDCT_M);
}

/* MDCT = time-domain fold (2M -> M) followed by DCT-IV.
   With x split into quarters (a,b,c,d) of length N/4:
       u = [ -c_R - d ,  a - b_R ]
   This is the standard TDAC folding; it is exactly equivalent to the direct
   cosine sum but runs in O(N log N) via FFTW. (The doc's FFT-based sketch used
   a real-to-complex plan with fftwf_creal/fftwf_cimag, which are not FFTW
   APIs, and its pre/post-rotation did not correspond to a valid MDCT.) */
void mdct_forward(const float *x, float *coeffs_out) {
    float u[MDCT_M];
    const int h = MDCT_H;          // N/4
    const int half = MDCT_SIZE / 2; // N/2

    for (int n = 0; n < h; n++) {
        u[n]     = -x[half + h - 1 - n] - x[half + h + n];  // -c_R - d
        u[h + n] =  x[n]                - x[2 * h - 1 - n]; //  a  - b_R
    }
    mdct_dct4(u, coeffs_out);
}

void mdct_masking_threshold(const float *coeffs, float *mask_out) {
    float band_energy[BARK_BANDS];

    for (int b = 0; b < BARK_BANDS; b++) {
        float e = 0.0f;
        for (int k = band_start[b]; k < band_start[b + 1]; k++)
            e += coeffs[k] * coeffs[k];
        band_energy[b] = e;
    }

    /* Spreading function. Upward spread (a masker hides higher frequencies) is
       broad; downward spread is steeper. The doc's constants (-2.5 up, 1.5
       down) make the downward skirt *shallower*, which is backwards versus
       measured psychoacoustics — the downward slope is used here. */
    for (int j = 0; j < BARK_BANDS; j++) {
        float spread = 0.0f;
        for (int i = 0; i < BARK_BANDS; i++) {
            float dz = (float)(j - i);
            float sf = (dz >= 0.0f) ? powf(10.0f, (-2.5f * dz) / 10.0f)
                                    : powf(10.0f, ( 6.0f * dz) / 10.0f);
            spread += band_energy[i] * sf;
        }
        mask_out[j] = spread * MDCT_SMR_LINEAR;
        if (mask_out[j] < ath_energy[j]) mask_out[j] = ath_energy[j];
    }
}

void mdct_band_steps(const float *mask, float *step_out) {
    for (int b = 0; b < BARK_BANDS; b++) {
        int count = band_start[b + 1] - band_start[b];
        if (count <= 0) { step_out[b] = 1.0f; continue; }
        /* Uniform quantiser noise power is step^2/12 per bin; spread the band's
           masking allowance across its bins. */
        float step = sqrtf(12.0f * mask[b] / (float)count);
        if (step < 1e-3f) step = 1e-3f;
        step_out[b] = step;
    }
}

/* step = 2^((idx - 128) / 4)  -> ~1.5 dB resolution in energy, wide range. */
float mdct_sf_to_step(int sf_index) {
    return powf(2.0f, (float)(sf_index - 128) / 4.0f);
}

int mdct_step_to_sf(float step) {
    if (step <= 0.0f) return 0;
    int idx = (int)lrintf(4.0f * log2f(step)) + 128;
    if (idx < 0)   idx = 0;
    if (idx > 255) idx = 255;
    return idx;
}
