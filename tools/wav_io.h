#ifndef AETHER_WAV_IO_H
#define AETHER_WAV_IO_H

/* Minimal canonical-PCM WAV reader/writer for the Phase 6 file-based tools
   (aether_encode / aether_decode). Not a general media library: handles
   16/24/32-bit integer PCM, mono or stereo, and skips unknown chunks
   (LIST/fact/etc) between "fmt " and "data". No float (format 3) or
   WAVE_FORMAT_EXTENSIBLE non-PCM subformats. */

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int32_t *samples;      /* interleaved, caller frees with free() */
    size_t   frames;       /* samples per channel */
    int      channels;
    int      sample_rate;
    int      bit_depth;    /* 16, 24, or 32 as found in the file */
} WavPCM;

/* Reads path into *out (out->samples malloc'd). Returns 0 on success,
   -1 on error (message printed to stderr). */
int wav_read(const char *path, WavPCM *out);

/* Writes interleaved int32 samples as canonical PCM WAV. bit_depth must be
   16 or 24; values are clamped to that range. Returns 0 on success. */
int wav_write(const char *path, const int32_t *samples, size_t frames,
             int channels, int sample_rate, int bit_depth);

void wav_free(WavPCM *w);

#endif /* AETHER_WAV_IO_H */
