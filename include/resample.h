#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stdint.h>

/* 2:1 resampling, used only to implement the 48 kHz half of the ABR ladder.
   The capture and playback ends of the pipeline always run at 96 kHz; when ABR
   selects a 48 kHz state the sender decimates before encoding and the receiver
   interpolates after decoding, so the OS-facing format never changes.

   Linear-phase windowed-sinc FIR, cutoff at fs/4 (24 kHz at 96 kHz input),
   which is Nyquist for the 48 kHz rate.

   Note: NL mode at 48 kHz is still bit-exact *with respect to the decimated
   signal*, but the 96->48 conversion itself discards the top octave. NL-48K is
   a deliberately degraded fallback state, not a lossless path end to end. */

#define RS_TAPS 63

typedef struct {
    float h[RS_TAPS];
    float hist[2][RS_TAPS];   // per-channel delay line (max 2 channels)
    int   channels;
} Resampler;

void resample_init(Resampler *r, int channels);
void resample_reset(Resampler *r);

/* Interleaved int32 (24-bit range) in and out.
   down2: in_frames must be even; writes in_frames/2 frames, returns that count.
   up2:   writes in_frames*2 frames, returns that count. */
int resample_down2(Resampler *r, const int32_t *in, int in_frames, int32_t *out);
int resample_up2  (Resampler *r, const int32_t *in, int in_frames, int32_t *out);

#endif /* RESAMPLE_H */
