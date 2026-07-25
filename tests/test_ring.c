/* SPSC ring buffer: capacity, wraparound, short read/write, threaded producer
   and consumer (the real usage — PipeWire RT thread vs codec thread). */
#include "audio_ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define TOTAL 500000

static AudioRing ring;

static void *producer(void *arg) {
    (void)arg;
    int32_t buf[64];
    int32_t next = 0;
    while (next < TOTAL) {
        int n = 0;
        while (n < 64 && next + n < TOTAL) { buf[n] = next + n; n++; }
        uint32_t w = 0;
        while (w < (uint32_t)n)
            w += audio_ring_write(&ring, buf + w, (uint32_t)n - w);
        next += n;
    }
    return NULL;
}

int main(void) {
    /* --- basic semantics ------------------------------------------------- */
    AudioRing r;
    assert(audio_ring_init(&r, 8) == 0);
    assert(r.capacity == 8);
    assert(audio_ring_available(&r) == 0);
    assert(audio_ring_space(&r) == 8);

    int32_t in[8]  = {1,2,3,4,5,6,7,8};
    int32_t out[8] = {0};
    assert(audio_ring_write(&r, in, 8) == 8);
    assert(audio_ring_write(&r, in, 1) == 0);        // full -> short write
    assert(audio_ring_available(&r) == 8);
    assert(audio_ring_read(&r, out, 8) == 8);
    assert(memcmp(in, out, sizeof(in)) == 0);
    assert(audio_ring_read(&r, out, 1) == 0);        // empty -> short read

    /* wraparound: write/read repeatedly across the boundary */
    for (int pass = 0; pass < 100; pass++) {
        assert(audio_ring_write(&r, in, 5) == 5);
        assert(audio_ring_read(&r, out, 5) == 5);
        assert(memcmp(in, out, 5 * sizeof(int32_t)) == 0);
    }
    audio_ring_free(&r);
    printf("\xE2\x9C\x93 ring: capacity, wraparound, short read/write OK\n");

    /* --- threaded SPSC: every value arrives exactly once, in order -------- */
    assert(audio_ring_init(&ring, 1024) == 0);
    pthread_t th;
    assert(pthread_create(&th, NULL, producer, NULL) == 0);

    int32_t rbuf[128];
    int32_t expect = 0;
    while (expect < TOTAL) {
        uint32_t n = audio_ring_read(&ring, rbuf, 128);
        for (uint32_t i = 0; i < n; i++) {
            assert(rbuf[i] == expect);   // ordering + no loss/duplication
            expect++;
        }
    }
    pthread_join(th, NULL);
    assert(expect == TOTAL);
    audio_ring_free(&ring);

    printf("\xE2\x9C\x93 ring: %d values through threaded SPSC, in order, no loss\n",
           TOTAL);
    return 0;
}
