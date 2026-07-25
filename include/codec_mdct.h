#ifndef CODEC_MDCT_H
#define CODEC_MDCT_H

#include <stdint.h>

/* Perceptual HQ mode geometry (HLD 5.2):
   window length N = 1024, hop = N/2 = 512 new samples per frame,
   producing N/2 = 512 MDCT coefficients. */
#define MDCT_SIZE     1024                 // window length N
#define MDCT_HOP      (MDCT_SIZE / 2)      // new samples consumed per frame
#define MDCT_COEFFS   (MDCT_SIZE / 2)      // coefficients produced per frame
#define BARK_BANDS    64                   // Bark-scale critical bands

/* One-shot init (idempotent): KBD window, Bark table, absolute-hearing-
   threshold table and the FFT plan. Must be called before any other function.
   NOTE: not thread-safe (FFTW plan creation isn't); the codec is single
   threaded today. */
void mdct_init(int sample_rate);

/* KBD analysis/synthesis window, MDCT_SIZE entries.
   Satisfies the Princen-Bradley condition w[n]^2 + w[n+N/2]^2 == 1, which is
   what makes windowed MDCT -> IMDCT -> overlap-add reconstruct exactly. */
const float* mdct_window(void);

/* First bin of Bark band b. mdct_band_start(BARK_BANDS) == MDCT_COEFFS. */
int mdct_band_start(int b);

/* DCT-IV of MDCT_COEFFS points. Shared by the forward and inverse transforms
   (MDCT = fold + DCT-IV; IMDCT = DCT-IV + scale + unfold). */
void mdct_dct4(const float *in, float *out);

/* Forward MDCT: MDCT_SIZE windowed samples -> MDCT_COEFFS coefficients. */
void mdct_forward(const float *windowed, float *coeffs_out);

/* Inverse MDCT: MDCT_COEFFS coefficients -> MDCT_SIZE time-aliased samples.
   The caller must multiply by mdct_window() and overlap-add successive frames;
   a single frame alone does NOT reconstruct the input (that is inherent to
   MDCT's time-domain alias cancellation, not a defect). */
void mdct_inverse(const float *coeffs, float *aliased_out);

/* Psychoacoustics: per-band masking threshold (energy units) from the frame's
   MDCT coefficients — spreading function + absolute threshold in quiet. */
void mdct_masking_threshold(const float *coeffs, float *mask_out);

/* Per-band uniform quantizer step sized so quantization noise sits at the
   masking threshold. */
void mdct_band_steps(const float *mask, float *step_out);

/* Scale factor <-> step conversion (step is stored on the wire as a u8 index). */
float mdct_sf_to_step(int sf_index);
int   mdct_step_to_sf(float step);

/* Signal-to-mask ratio in dB — HQ's single rate/quality knob (~6 dB per bit per
   significant coefficient). Clamped to [6, 30]; default 30. Encoder-side only:
   the wire format carries the resulting scalefactors, so the decoder neither
   knows nor needs this. The sender adjusts it in a closed loop against link
   congestion, because a fixed quality means an uncontrolled bitrate — and
   dropped frames are far more audible than coarser quantisation. */
void  mdct_set_smr_db(float db);
float mdct_get_smr_db(void);

#endif /* CODEC_MDCT_H */
