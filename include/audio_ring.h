#ifndef AUDIO_RING_H
#define AUDIO_RING_H

#include <stdint.h>
#include <stdatomic.h>

/* Lock-free single-producer / single-consumer ring buffer (HLD 11.2).
   Sits between the PipeWire realtime callback and the codec thread so neither
   ever blocks the other. Indices increase monotonically and wrap naturally on
   uint32 overflow; capacity is a power of two so masking replaces modulo. */
typedef struct {
    int32_t          *data;
    uint32_t          capacity;   // in int32 values, power of two
    uint32_t          mask;
    _Atomic uint32_t  head;       // producer writes here
    _Atomic uint32_t  tail;       // consumer reads here
} AudioRing;

/* capacity_samples is rounded up to the next power of two. Returns 0 on ok. */
int  audio_ring_init(AudioRing *r, uint32_t capacity_samples);
void audio_ring_free(AudioRing *r);

/* Returns how many values were actually written / read (may be short). */
uint32_t audio_ring_write(AudioRing *r, const int32_t *src, uint32_t n);
uint32_t audio_ring_read (AudioRing *r, int32_t *dst, uint32_t n);

uint32_t audio_ring_available(const AudioRing *r);  // readable values
uint32_t audio_ring_space(const AudioRing *r);      // writable values
void     audio_ring_reset(AudioRing *r);

#endif /* AUDIO_RING_H */
