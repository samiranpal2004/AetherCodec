#include "wav_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

int wav_read(const char *path, WavPCM *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "wav_read: cannot open %s\n", path); return -1; }

    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "wav_read: %s is not a RIFF/WAVE file\n", path);
        fclose(f); return -1;
    }

    int found_fmt = 0, found_data = 0;
    uint16_t audio_format = 0, channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;
    uint8_t *data = NULL;
    uint32_t data_bytes = 0;

    uint8_t chdr[8];
    while (fread(chdr, 1, 8, f) == 8) {
        uint32_t csize = rd_u32le(chdr + 4);

        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t fb[40];
            uint32_t toread = csize < sizeof(fb) ? csize : sizeof(fb);
            if (fread(fb, 1, toread, f) != toread) break;
            if (csize > toread) fseek(f, (long)(csize - toread), SEEK_CUR);

            audio_format    = rd_u16le(fb + 0);
            channels        = rd_u16le(fb + 2);
            sample_rate     = rd_u32le(fb + 4);
            bits_per_sample = rd_u16le(fb + 14);
            /* WAVE_FORMAT_EXTENSIBLE (0xFFFE): real format is in the
               sub-format GUID at offset 24; PCM sub-format starts 0x01. */
            if (audio_format == 0xFFFE && toread >= 26) audio_format = 1;
            found_fmt = 1;
        } else if (memcmp(chdr, "data", 4) == 0) {
            data = malloc(csize);
            if (!data) { fclose(f); return -1; }
            if (fread(data, 1, csize, f) != csize) {
                fprintf(stderr, "wav_read: truncated data chunk in %s\n", path);
                free(data); fclose(f); return -1;
            }
            data_bytes = csize;
            found_data = 1;
            if (csize & 1) fseek(f, 1, SEEK_CUR);   /* chunk padding */
        } else {
            fseek(f, (long)csize + (long)(csize & 1), SEEK_CUR);
        }
    }
    fclose(f);

    if (!found_fmt || !found_data) {
        fprintf(stderr, "wav_read: %s missing fmt/data chunk\n", path);
        free(data); return -1;
    }
    if (audio_format != 1) {
        fprintf(stderr, "wav_read: %s uses unsupported format %u "
                        "(only integer PCM is supported)\n", path, audio_format);
        free(data); return -1;
    }
    if (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "wav_read: %s has unsupported bit depth %u\n",
                path, bits_per_sample);
        free(data); return -1;
    }

    int bytes_per_sample = bits_per_sample / 8;
    size_t total_samples = data_bytes / (size_t)bytes_per_sample;
    int32_t *samples = malloc(total_samples * sizeof(int32_t));
    if (!samples) { free(data); return -1; }

    for (size_t i = 0; i < total_samples; i++) {
        const uint8_t *s = data + i * bytes_per_sample;
        int32_t v;
        if (bits_per_sample == 16) {
            v = (int16_t)rd_u16le(s);
        } else if (bits_per_sample == 24) {
            uint32_t u = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
            v = (u & 0x800000) ? (int32_t)(u | 0xFF000000u) : (int32_t)u;
        } else {
            v = (int32_t)rd_u32le(s);
        }
        samples[i] = v;
    }
    free(data);

    out->samples     = samples;
    out->frames       = channels ? total_samples / channels : 0;
    out->channels     = channels;
    out->sample_rate  = (int)sample_rate;
    out->bit_depth    = bits_per_sample;
    return 0;
}

static void wr_u32le(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void wr_u16le(FILE *f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

int wav_write(const char *path, const int32_t *samples, size_t frames,
             int channels, int sample_rate, int bit_depth) {
    if (bit_depth != 16 && bit_depth != 24) {
        fprintf(stderr, "wav_write: bit_depth must be 16 or 24 (got %d)\n", bit_depth);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "wav_write: cannot create %s\n", path); return -1; }

    int bytes_per_sample = bit_depth / 8;
    uint32_t data_bytes  = (uint32_t)(frames * (size_t)channels * (size_t)bytes_per_sample);
    uint32_t byte_rate   = (uint32_t)(sample_rate * channels * bytes_per_sample);
    uint16_t block_align = (uint16_t)(channels * bytes_per_sample);

    fwrite("RIFF", 1, 4, f);
    wr_u32le(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    wr_u32le(f, 16);
    wr_u16le(f, 1);                       /* PCM */
    wr_u16le(f, (uint16_t)channels);
    wr_u32le(f, (uint32_t)sample_rate);
    wr_u32le(f, byte_rate);
    wr_u16le(f, block_align);
    wr_u16le(f, (uint16_t)bit_depth);

    fwrite("data", 1, 4, f);
    wr_u32le(f, data_bytes);

    size_t total = frames * (size_t)channels;
    for (size_t i = 0; i < total; i++) {
        int32_t v = samples[i];
        if (bit_depth == 16) {
            if (v >  32767)  v =  32767;
            if (v < -32768)  v = -32768;
            wr_u16le(f, (uint16_t)(int16_t)v);
        } else {
            if (v >  8388607)  v =  8388607;
            if (v < -8388608)  v = -8388608;
            uint8_t b[3] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16) };
            fwrite(b, 1, 3, f);
        }
    }

    fclose(f);
    return 0;
}

void wav_free(WavPCM *w) {
    free(w->samples);
    w->samples = NULL;
}
