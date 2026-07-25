#include "audio_ring.h"
#include <stdlib.h>
#include <string.h>

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

int audio_ring_init(AudioRing *r, uint32_t capacity_samples) {
    uint32_t cap = next_pow2(capacity_samples ? capacity_samples : 2);
    r->data = calloc(cap, sizeof(int32_t));
    if (!r->data) return -1;
    r->capacity = cap;
    r->mask     = cap - 1;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    return 0;
}

void audio_ring_free(AudioRing *r) {
    free(r->data);
    r->data = NULL;
}

void audio_ring_reset(AudioRing *r) {
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
}

uint32_t audio_ring_available(const AudioRing *r) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    return head - tail;
}

uint32_t audio_ring_space(const AudioRing *r) {
    return r->capacity - audio_ring_available(r);
}

uint32_t audio_ring_write(AudioRing *r, const int32_t *src, uint32_t n) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t space = r->capacity - (head - tail);
    if (n > space) n = space;

    for (uint32_t i = 0; i < n; i++)
        r->data[(head + i) & r->mask] = src[i];

    atomic_store_explicit(&r->head, head + n, memory_order_release);
    return n;
}

uint32_t audio_ring_read(AudioRing *r, int32_t *dst, uint32_t n) {
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t avail = head - tail;
    if (n > avail) n = avail;

    for (uint32_t i = 0; i < n; i++)
        dst[i] = r->data[(tail + i) & r->mask];

    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    return n;
}
