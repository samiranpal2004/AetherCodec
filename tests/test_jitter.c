/* Jitter buffer: prefill, in-order playout, reordering, duplicate rejection,
   and loss detection (so the receiver knows when to conceal). */
#include "jitter_buf.h"
#include "aether_packet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static AetherPacket *mkpkt(uint32_t seq) {
    static AetherPacket p;
    memset(&p, 0, sizeof(p));
    p.hdr.magic        = AETHER_MAGIC;
    p.hdr.sequence     = seq;
    p.hdr.mode         = AETHER_MODE_NL;
    p.hdr.channels     = 2;
    p.hdr.payload_size = 4;
    memcpy(p.payload, &seq, 4);      // marker so we can verify identity
    return &p;
}

static uint32_t payload_seq(const AetherPacket *p) {
    uint32_t v; memcpy(&v, p->payload, 4); return v;
}

int main(void) {
    /* --- prefill: nothing plays until enough is buffered ------------------ */
    JitterBuf *jb = jitter_buf_create(40, 96000);
    assert(jb);
    assert(jitter_buf_pop(jb) == NULL);          // empty
    jitter_buf_insert(jb, mkpkt(0));
    assert(jitter_buf_pop(jb) == NULL);          // still below prefill
    jitter_buf_insert(jb, mkpkt(1));

    const AetherPacket *p = jitter_buf_pop(jb);
    assert(p && payload_seq(p) == 0);
    p = jitter_buf_pop(jb);
    assert(p && payload_seq(p) == 1);
    assert(jitter_buf_pop(jb) == NULL);          // drained
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: prefill + in-order playout OK\n");

    /* --- reordering: out-of-order arrival still plays in order ------------ */
    jb = jitter_buf_create(40, 96000);
    jitter_buf_insert(jb, mkpkt(5));
    jitter_buf_insert(jb, mkpkt(7));
    jitter_buf_insert(jb, mkpkt(6));             // arrives late, before playout
    for (uint32_t want = 5; want <= 7; want++) {
        p = jitter_buf_pop(jb);
        assert(p && payload_seq(p) == want);
    }
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: out-of-order 5,7,6 played as 5,6,7\n");

    /* --- duplicates are ignored ------------------------------------------ */
    jb = jitter_buf_create(40, 96000);
    jitter_buf_insert(jb, mkpkt(0));
    jitter_buf_insert(jb, mkpkt(0));
    jitter_buf_insert(jb, mkpkt(1));
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 0);
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 1);
    assert(jitter_buf_pop(jb) == NULL);
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: duplicate sequence rejected\n");

    /* --- loss: gap is reported so the caller can conceal ------------------ */
    jb = jitter_buf_create(40, 96000);
    jitter_buf_insert(jb, mkpkt(10));
    jitter_buf_insert(jb, mkpkt(12));            // 11 never arrives
    jitter_buf_insert(jb, mkpkt(13));

    int lost = 0;
    p = jitter_buf_pop_ex(jb, &lost);
    assert(p && payload_seq(p) == 10 && !lost);

    p = jitter_buf_pop_ex(jb, &lost);            // 11 missing, 12/13 present
    assert(p == NULL && lost == 1);

    p = jitter_buf_pop_ex(jb, &lost);
    assert(p && payload_seq(p) == 12 && !lost);
    p = jitter_buf_pop_ex(jb, &lost);
    assert(p && payload_seq(p) == 13 && !lost);
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: missing packet reported as loss, stream resumes\n");

    /* --- late packet (already played past) is dropped --------------------- */
    jb = jitter_buf_create(40, 96000);
    jitter_buf_insert(jb, mkpkt(20));
    jitter_buf_insert(jb, mkpkt(21));
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 20);
    jitter_buf_insert(jb, mkpkt(20));            // far too late
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 21);
    assert(jitter_buf_pop(jb) == NULL);
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: stale packet discarded\n");

    /* --- sender restarts its sequence: must re-anchor, not wedge ----------
       Regression: a sequence reset (sender restart, or an ABR switch back when
       switching recreated the encoder) made every new packet look older than
       next_seq, so the buffer discarded the entire rest of the stream and
       playout froze until the counter climbed back past where it had been. */
    jb = jitter_buf_create(40, 96000);
    for (uint32_t s = 1000; s < 1010; s++) jitter_buf_insert(jb, mkpkt(s));
    for (uint32_t s = 1000; s < 1010; s++) {
        p = jitter_buf_pop(jb);
        assert(p && payload_seq(p) == s);
    }
    assert(jitter_buf_resyncs(jb) == 0);

    jitter_buf_insert(jb, mkpkt(0));             // sender counter restarted
    jitter_buf_insert(jb, mkpkt(1));
    assert(jitter_buf_resyncs(jb) == 1);
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 0);
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 1);
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: re-anchors after a sequence restart\n");

    /* --- outage far past the reorder window also re-anchors ---------------
       Walking next_seq forward one frame at a time would emit thousands of
       concealment frames in a tight loop; re-anchor instead. */
    jb = jitter_buf_create(40, 96000);
    jitter_buf_insert(jb, mkpkt(0));
    jitter_buf_insert(jb, mkpkt(1));
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 0);
    jitter_buf_insert(jb, mkpkt(50000));         // huge forward jump
    assert(jitter_buf_resyncs(jb) == 1);
    jitter_buf_insert(jb, mkpkt(50001));
    p = jitter_buf_pop(jb); assert(p && payload_seq(p) == 50000);
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: re-anchors after a long outage\n");

    /* --- level_ms tracks the real frame geometry, not a hardcoded 2048 ---- */
    jb = jitter_buf_create(40, 96000);
    AetherPacket hq;
    memset(&hq, 0, sizeof(hq));
    hq.hdr.magic = AETHER_MAGIC; hq.hdr.mode = AETHER_MODE_HQ;
    hq.hdr.channels = 2; hq.hdr.sample_rate = AETHER_RATE_96000;
    hq.hdr.payload_size = 2;
    uint16_t hop = 512; memcpy(hq.payload, &hop, 2);
    for (uint32_t s = 0; s < 4; s++) { hq.hdr.sequence = s; jitter_buf_insert(jb, &hq); }
    /* 4 HQ frames * 512 / 96000 = 21 ms, not the 85 ms a 2048-sample frame
       would imply. */
    assert(jitter_buf_level_ms(jb) == 21);
    jitter_buf_destroy(jb);
    printf("\xE2\x9C\x93 jitter: level_ms follows the stream's frame size\n");

    printf("\nAll jitter buffer tests passed.\n");
    return 0;
}
