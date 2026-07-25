/* 2:1 decimation / interpolation for the ABR ladder's 48 kHz states.
   Lives under abr/ because rate switching exists purely to serve the adaptive
   bitrate engine. */
#include "resample.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int32_t clamp24(float v) {
    long r = lrintf(v);
    if (r >  8388607L) r =  8388607L;
    if (r < -8388608L) r = -8388608L;
    return (int32_t)r;
}

void resample_init(Resampler *r, int channels) {
    r->channels = (channels == 1) ? 1 : 2;

    /* Windowed sinc, cutoff fc = 0.25 * fs (i.e. half of the half-rate Nyquist),
       Hamming window, then normalised for unity DC gain. */
    const double fc = 0.25;
    const int    c  = RS_TAPS / 2;
    double sum = 0.0;

    for (int n = 0; n < RS_TAPS; n++) {
        double m = n - c;
        double s = (m == 0.0) ? 2.0 * fc
                              : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / (RS_TAPS - 1));
        double v = s * w;
        r->h[n] = (float)v;
        sum += v;
    }
    for (int n = 0; n < RS_TAPS; n++) r->h[n] = (float)(r->h[n] / sum);

    resample_reset(r);
}

void resample_reset(Resampler *r) {
    memset(r->hist, 0, sizeof(r->hist));
}

static inline void push(float *hist, float v) {
    memmove(hist + 1, hist, (RS_TAPS - 1) * sizeof(float));
    hist[0] = v;
}

static inline float filt(const float *h, const float *hist) {
    float acc = 0.0f;
    for (int k = 0; k < RS_TAPS; k++) acc += h[k] * hist[k];
    return acc;
}

int resample_down2(Resampler *r, const int32_t *in, int in_frames, int32_t *out) {
    const int ch = r->channels;
    int outn = 0;

    for (int i = 0; i < in_frames; i++) {
        for (int c = 0; c < ch; c++)
            push(r->hist[c], (float)in[i * ch + c]);

        if (i & 1) {                       /* keep every second sample */
            for (int c = 0; c < ch; c++)
                out[outn * ch + c] = clamp24(filt(r->h, r->hist[c]));
            outn++;
        }
    }
    return outn;
}

int resample_up2(Resampler *r, const int32_t *in, int in_frames, int32_t *out) {
    const int ch = r->channels;
    int outn = 0;

    for (int i = 0; i < in_frames; i++) {
        for (int phase = 0; phase < 2; phase++) {
            for (int c = 0; c < ch; c++)
                push(r->hist[c], phase == 0 ? (float)in[i * ch + c] : 0.0f);
            for (int c = 0; c < ch; c++)
                /* x2 compensates the energy lost to zero-stuffing */
                out[outn * ch + c] = clamp24(filt(r->h, r->hist[c]) * 2.0f);
            outn++;
        }
    }
    return outn;
}
