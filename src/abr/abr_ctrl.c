#include "abr_ctrl.h"
#include "aether_packet.h"
#include <stdlib.h>
#include <string.h>

struct ABRCtrl {
    ABRState     current;
    int          good_count;
    int          bad_count;
    uint64_t     last_switch_ms;
    int          have_switched;
    ABRState     min_state;        /* best (numerically lowest) state allowed;
                                      raised by congestion, relaxed over time */
    uint64_t     relax_ms;         /* next time min_state may relax one step */
    int          headroom;         /* sender reports real spare capacity */
    ABRCallback  callback;
    void        *userdata;
};

static const char *state_names[ABR_STATE_COUNT] = {
    "NL-96kHz", "NL-48kHz", "HQ-96kHz", "HQ-48kHz"
};

const char* abr_state_name(ABRState s) {
    if (s < 0 || s >= ABR_STATE_COUNT) return "?";
    return state_names[s];
}

int abr_state_mode(ABRState s) {
    return (s == ABR_STATE_NL_96K || s == ABR_STATE_NL_48K)
         ? AETHER_MODE_NL : AETHER_MODE_HQ;
}

int abr_state_rate(ABRState s) {
    return (s == ABR_STATE_NL_96K || s == ABR_STATE_HQ_96K) ? 96000 : 48000;
}

float abr_smr_step(float smr_db, int queue_ms, int dropped) {
    if (dropped || queue_ms > ABR_QUEUE_HIGH_MS) {
        /* Decrease scaled by how far behind we are: a queue merely creeping up
           gets a nudge, one that is nearly full gets cut hard. Proportional
           rather than fixed-step so recovery from a sudden dense passage takes
           a couple of ticks instead of ten. */
        float cut = 1.0f + (float)queue_ms / 100.0f;
        if (cut > 6.0f) cut = 6.0f;
        smr_db -= cut;
    } else if (queue_ms < ABR_QUEUE_LOW_MS) {
        smr_db += ABR_SMR_UP_DB;
    }
    if (smr_db < ABR_SMR_MIN_DB) smr_db = ABR_SMR_MIN_DB;
    if (smr_db > ABR_SMR_MAX_DB) smr_db = ABR_SMR_MAX_DB;
    return smr_db;
}

ABRState abr_classify(int rssi_dbm, float loss_pct) {
    if (rssi_dbm > -65 && loss_pct < 1.0f) return ABR_STATE_NL_96K;
    if (rssi_dbm > -75 && loss_pct < 3.0f) return ABR_STATE_NL_48K;
    if (rssi_dbm > -80 && loss_pct < 8.0f) return ABR_STATE_HQ_96K;
    return ABR_STATE_HQ_48K;
}

ABRCtrl* abr_ctrl_create(ABRCallback cb, void *userdata) {
    ABRCtrl *abr = calloc(1, sizeof(*abr));
    if (!abr) return NULL;
    abr->current  = ABR_STATE_NL_96K;   /* start optimistic, degrade on evidence */
    abr->callback = cb;
    abr->userdata = userdata;
    abr->headroom = 1;                  /* assume spare capacity until told otherwise */
    return abr;
}

void abr_set_headroom(ABRCtrl *abr, int headroom) {
    if (abr) abr->headroom = headroom ? 1 : 0;
}

ABRState abr_current_state(const ABRCtrl *abr) { return abr->current; }

static void commit(ABRCtrl *abr, ABRState target, uint64_t now_ms) {
    abr->current        = target;
    abr->good_count     = 0;
    abr->bad_count      = 0;
    abr->last_switch_ms = now_ms;
    abr->have_switched  = 1;
    if (abr->callback)
        abr->callback(abr_state_mode(target), abr_state_rate(target),
                      abr->userdata);
}

void abr_update_at(ABRCtrl *abr, int rssi_dbm, float loss_pct,
                   int jitter_level_ms, uint64_t now_ms) {
    (void)jitter_level_ms;   /* reserved: buffer depth as a secondary signal */

    ABRState target = abr_classify(rssi_dbm, loss_pct);

    if (target > abr->current) {
        /* Larger value == worse quality: the link degraded. React quickly. */
        abr->good_count = 0;
        if (++abr->bad_count >= ABR_DOWNGRADE_CONSECUTIVE)
            commit(abr, target, now_ms);

    } else if (target < abr->current) {
        /* Link improved. Be cautious: require consecutive good readings AND a
           minimum hold since the last switch, so a flapping link does not
           oscillate. */
        abr->bad_count = 0;
        abr->good_count++;
        int held = !abr->have_switched ||
                   (now_ms - abr->last_switch_ms) >= ABR_UPGRADE_HOLD_MS;
        if (abr->good_count >= ABR_UPGRADE_CONSECUTIVE && held)
            commit(abr, target, now_ms);

    } else {
        abr->good_count = 0;
        abr->bad_count  = 0;
    }
}

void abr_update(ABRCtrl *abr, int rssi_dbm, float loss_pct, int jitter_level_ms) {
    abr_update_at(abr, rssi_dbm, loss_pct, jitter_level_ms,
                  aether_timestamp_us() / 1000ULL);
}

void abr_update_congested(ABRCtrl *abr, int rssi_dbm, float loss_pct,
                          int congested, uint64_t now_ms) {
    /* Relax the congestion ceiling one step at a time so a link that recovers
       can climb back up — but only when the sender reports real spare capacity.
       Relaxing purely on the timer means re-probing a mode the link has already
       proven it cannot carry, every ABR_PROBE_INTERVAL_MS, forever; each probe
       fills the send queue and costs an audible blip before it backs off again.
       `headroom` defaults to 1, so a caller that never sets it keeps the plain
       timer behaviour. */
    if (abr->min_state > ABR_STATE_NL_96K && now_ms >= abr->relax_ms &&
        abr->headroom && !congested) {
        abr->min_state--;
        abr->relax_ms = now_ms + ABR_PROBE_INTERVAL_MS;
    }

    ABRState target = abr_classify(rssi_dbm, loss_pct);

    if (congested) {
        /* Current mode doesn't fit — forbid it and everything better, and hold
           that ceiling for a while so we don't immediately probe back into it. */
        ABRState worse = (ABRState)(abr->current + 1);
        if (worse > ABR_STATE_HQ_48K) worse = ABR_STATE_HQ_48K;
        if (worse > abr->min_state) abr->min_state = worse;
        abr->relax_ms = now_ms + ABR_PROBE_INTERVAL_MS;
    }

    if (target < abr->min_state) target = abr->min_state;   /* clamp to allowed */

    if (target > abr->current) {
        /* Degraded. Congestion commits immediately (audible now); an RSSI-only
           degrade still waits for the usual consecutive-bad hysteresis. */
        abr->good_count = 0;
        if (congested || ++abr->bad_count >= ABR_DOWNGRADE_CONSECUTIVE)
            commit(abr, target, now_ms);
    } else if (target < abr->current) {
        abr->bad_count = 0;
        abr->good_count++;
        int held = !abr->have_switched ||
                   (now_ms - abr->last_switch_ms) >= ABR_UPGRADE_HOLD_MS;
        if (abr->good_count >= ABR_UPGRADE_CONSECUTIVE && held)
            commit(abr, target, now_ms);
    } else {
        abr->good_count = 0;
        abr->bad_count  = 0;
    }
}

void abr_ctrl_destroy(ABRCtrl *abr) { free(abr); }
