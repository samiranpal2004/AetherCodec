#ifndef CODEC_MDCT_H
#define CODEC_MDCT_H

#include <stdint.h>

#define MDCT_SIZE     1024   // one channel, one frame (samples)
#define MDCT_OVERLAP  512    // 50% overlap
#define MDCT_COEFFS   (MDCT_SIZE / 2)  // 512 output coefficients
#define BARK_BANDS    64     // Bark-scale critical bands

/* --- Window / transform setup (idempotent, call once) --- */
void mdct_init_window(void);
void mdct_init_bark_table(int sample_rate);

/* Forward MDCT: N windowed samples -> N/2 frequency coefficients. */
void mdct_transform(const float *windowed, int N, float *out);

/* Inverse MDCT: N/2 coefficients -> N samples (caller does overlap-add). */
void imdct_transform(const float *coeffs, int N, float *out);

/* --- Psychoacoustic model --- */
/* mdct_coeffs: MDCT_COEFFS floats; mask_out: BARK_BANDS masking thresholds. */
void compute_masking_threshold(const float *mdct_coeffs, float *mask_out);

/* Allocate up to `total_bits` across bands (0..15 bits each). */
void allocate_bits(const float *band_energy, const float *mask,
                   int total_bits, int *bits_per_band);

/* Quantize one band in place; writes the quant step used. */
void quantize_band(float *coeffs, int start, int end,
                   float band_energy, int bits, float *quant_step_out);

#endif /* CODEC_MDCT_H */
