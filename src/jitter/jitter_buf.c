#include "jitter_buf.h"
#include "codec_lpc.h"
#include <stdlib.h>
#include <string.h>

/* Reorder buffer keyed on packet sequence number.
   Slots hold whole AetherPackets (~64 KB each), so the depth is 32 rather than
   the HLD's 128 — 128 slots would cost ~8 MB for buffering we never need
   (32 * 21 ms = 672 ms, far beyond the 40 ms target). */
#define JB_SLOTS 32

/* Sequence gap beyond which we stop trying to reconcile and just re-anchor.
   Two situations need this and both were previously unrecoverable:
     - the sender's counter restarts (daemon restart, or — until it was fixed —
       an ABR mode switch). Every packet then looks older than next_seq and is
       discarded forever, so playout freezes silently.
     - a very long outage. Walking next_seq forward one frame at a time would
       emit thousands of concealment frames in a tight loop.
   128 packets is ~2.7 s of NL-96k or ~0.7 s of HQ — far past anything the
   reorder window can legitimately span. */
#define JB_RESYNC_GAP 128

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
    int           frame_samples;  // last seen frame length, for level_ms
    int           frame_rate;     // last seen stream rate, for level_ms
    unsigned long resyncs;
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
    jb->frame_samples = LPC_FRAME_SIZE;
    jb->frame_rate    = sample_rate;
    return jb;
}

static void jb_reset(JitterBuf *jb, uint32_t anchor) {
    memset(jb->valid, 0, JB_SLOTS);
    jb->count    = 0;
    jb->started  = 0;
    jb->next_seq = anchor;
}

void jitter_buf_insert(JitterBuf *jb, const AetherPacket *pkt) {
    uint32_t s = pkt->hdr.sequence;

    /* Track the real frame geometry so jitter_buf_level_ms() means something in
       HQ (512-sample frames) as well as NL (2048). frame_samples is the first
       payload field in both modes. */
    if (pkt->hdr.payload_size >= 2) {
        uint16_t fs;
        memcpy(&fs, pkt->payload, 2);
        if (fs > 0) jb->frame_samples = fs;
    }
    jb->frame_rate = (pkt->hdr.sample_rate == AETHER_RATE_48000) ? 48000
                   : (pkt->hdr.sample_rate == AETHER_RATE_44100) ? 44100
                   : (pkt->hdr.sample_rate == AETHER_RATE_88200) ? 88200
                                                                 : 96000;

    if (!jb->started && jb->count == 0)
        jb->next_seq = s;               // anchor on the first packet seen

    /* Sequence discontinuity too large to be reordering or loss — re-anchor
       rather than discarding the rest of the stream (see JB_RESYNC_GAP). */
    int32_t delta = (int32_t)(s - jb->next_seq);
    if (jb->started && (delta < -JB_RESYNC_GAP || delta > JB_RESYNC_GAP)) {
        jb_reset(jb, s);
        jb->resyncs++;
    }

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
    /* Frame duration depends on mode AND rate (NL 2048 vs HQ 512 samples), so
       use what the stream actually carries rather than a hardcoded NL frame. */
    int rate = jb->frame_rate > 0 ? jb->frame_rate : jb->sample_rate;
    if (rate <= 0) return 0;
    double frame_ms = (double)jb->frame_samples * 1000.0 / rate;
    return (int)(jb->count * frame_ms);
}

unsigned long jitter_buf_resyncs(const JitterBuf *jb) {
    return jb ? jb->resyncs : 0;
}

void jitter_buf_destroy(JitterBuf *jb) {
    if (!jb) return;
    free(jb->slots);
    free(jb->valid);
    free(jb->seq);
    free(jb);
}
