#include "jitter_buf.h"
#include <stdlib.h>
#include <string.h>

/* Reorder buffer keyed on packet sequence number.
   Slots hold whole AetherPackets (~64 KB each), so the depth is 32 rather than
   the HLD's 128 — 128 slots would cost ~8 MB for buffering we never need
   (32 * 21 ms = 672 ms, far beyond the 40 ms target). */
#define JB_SLOTS 32

struct JitterBuf {
    AetherPacket *slots;
    uint8_t      *valid;
    uint32_t     *seq;
    int           target_ms;
    int           sample_rate;
    int           prefill;        // frames to buffer before playout starts
    int           started;
    uint32_t      next_seq;       // sequence we expect to play next
    int           count;          // packets currently held
};

JitterBuf* jitter_buf_create(int target_ms, int sample_rate) {
    JitterBuf *jb = calloc(1, sizeof(*jb));
    if (!jb) return NULL;
    jb->slots = calloc(JB_SLOTS, sizeof(AetherPacket));
    jb->valid = calloc(JB_SLOTS, 1);
    jb->seq   = calloc(JB_SLOTS, sizeof(uint32_t));
    if (!jb->slots || !jb->valid || !jb->seq) {
        jitter_buf_destroy(jb);
        return NULL;
    }
    jb->target_ms   = target_ms;
    jb->sample_rate = sample_rate;

    /* NL frames are ~21 ms at 96 kHz; buffer at least 2 so a single late
       packet does not immediately starve playout. */
    int frames = target_ms / 21;
    if (frames < 2) frames = 2;
    if (frames > JB_SLOTS / 2) frames = JB_SLOTS / 2;
    jb->prefill = frames;
    return jb;
}

void jitter_buf_insert(JitterBuf *jb, const AetherPacket *pkt) {
    uint32_t s = pkt->hdr.sequence;

    if (!jb->started && jb->count == 0)
        jb->next_seq = s;               // anchor on the first packet seen

    /* Drop packets that are already too late to play. */
    if (jb->started && (int32_t)(s - jb->next_seq) < 0) return;

    int idx = (int)(s % JB_SLOTS);
    if (jb->valid[idx]) {
        if (jb->seq[idx] == s) return;  // duplicate
        jb->count--;                    // evict the stale occupant
    }
    memcpy(&jb->slots[idx], pkt, sizeof(AetherPacket));
    jb->valid[idx] = 1;
    jb->seq[idx]   = s;
    jb->count++;
}

const AetherPacket* jitter_buf_pop_ex(JitterBuf *jb, int *lost) {
    if (lost) *lost = 0;

    if (!jb->started) {
        if (jb->count < jb->prefill) return NULL;   // still filling
        jb->started = 1;
    }
    if (jb->count == 0) return NULL;

    int idx = (int)(jb->next_seq % JB_SLOTS);
    if (jb->valid[idx] && jb->seq[idx] == jb->next_seq) {
        jb->valid[idx] = 0;
        jb->count--;
        jb->next_seq++;
        return &jb->slots[idx];
    }

    /* Expected packet is absent. If anything newer is buffered it is genuinely
       lost (not merely late), so skip it and let the caller conceal. */
    for (int i = 0; i < JB_SLOTS; i++) {
        if (jb->valid[i] && (int32_t)(jb->seq[i] - jb->next_seq) > 0) {
            jb->next_seq++;
            if (lost) *lost = 1;
            return NULL;
        }
    }
    return NULL;    // nothing newer yet — wait rather than declare loss
}

const AetherPacket* jitter_buf_pop(JitterBuf *jb) {
    return jitter_buf_pop_ex(jb, NULL);
}

int jitter_buf_level_ms(const JitterBuf *jb) {
    /* Frame duration depends on mode; 2048 samples is the NL frame. */
    if (jb->sample_rate <= 0) return 0;
    double frame_ms = 2048.0 * 1000.0 / jb->sample_rate;
    return (int)(jb->count * frame_ms);
}

void jitter_buf_destroy(JitterBuf *jb) {
    if (!jb) return;
    free(jb->slots);
    free(jb->valid);
    free(jb->seq);
    free(jb);
}
