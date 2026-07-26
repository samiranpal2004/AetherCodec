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
    uint64_t     probe_ms;         /* current probe interval; doubles on failure */
    uint64_t     last_congest_ms;  /* when congestion was last seen */
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
        /* Two-tier increase: an essentially empty queue means the link is not
           the constraint at all, so climb fast; once the queue shows depth we
           are near capacity and ease off to the gentle step. */
        smr_db += (queue_ms <= ABR_SMR_IDLE_MS) ? ABR_SMR_UP_FAST_DB
                                                : ABR_SMR_UP_DB;
    }
    if (smr_db < ABR_SMR_MIN_DB) smr_db = ABR_SMR_MIN_DB;
    if (smr_db > ABR_SMR_MAX_DB) smr_db = ABR_SMR_MAX_DB;
    return smr_db;
}

void smr_ctrl_init(SmrCtrl *c, float start_db) {
    if (!c) return;
    if (start_db < ABR_SMR_MIN_DB) start_db = ABR_SMR_MIN_DB;
    if (start_db > ABR_SMR_MAX_DB) start_db = ABR_SMR_MAX_DB;
    c->smr         = start_db;
    c->ceiling     = ABR_SMR_MAX_DB;   /* fully open until the link says otherwise */
    c->clean_ticks = 0;
}

float smr_ctrl_step(SmrCtrl *c, int queue_ms, int dropped) {
    int congested = dropped || queue_ms > ABR_QUEUE_HIGH_MS;

    if (congested) {
        /* Record the failure. The level we were sitting at is demonstrably too
           high for this link, so cap below it; the queue lags the bitrate that
           caused it, hence a margin rather than the exact value. Repeated
           congestion ratchets the cap down further, so it converges from above
           on whatever the link genuinely holds. */
        float cap = c->smr - ABR_SMR_BACKOFF_DB;
        if (cap < ABR_SMR_MIN_DB) cap = ABR_SMR_MIN_DB;
        if (cap < c->ceiling) c->ceiling = cap;
        c->clean_ticks = 0;
    } else if (queue_ms < ABR_QUEUE_LOW_MS) {
        /* Probe upward only after a sustained clean stretch, and only a little:
           a probe that fails costs a dropout, so it must be rare and small.
           This is the slow path back up if the link genuinely improves. */
        if (++c->clean_ticks >= ABR_SMR_PROBE_TICKS &&
            c->ceiling < ABR_SMR_MAX_DB) {
            c->ceiling += ABR_SMR_PROBE_DB;
            if (c->ceiling > ABR_SMR_MAX_DB) c->ceiling = ABR_SMR_MAX_DB;
            c->clean_ticks = 0;
        }
    }

    c->smr = abr_smr_step(c->smr, queue_ms, dropped);
    if (c->smr > c->ceiling) c->smr = c->ceiling;
    return c->smr;
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
    abr->probe_ms = ABR_PROBE_INTERVAL_MS;
    return abr;
}

void abr_set_headroom(ABRCtrl *abr, int headroom) {
    if (abr) abr->headroom = headroom ? 1 : 0;
}

void abr_start_at(ABRCtrl *abr, ABRState state, uint64_t now_ms) {
    if (!abr) return;
    abr->current        = state;
    /* Also the ceiling: states above the start point must be *earned* through
       the headroom-gated relax path (one rung per ABR_PROBE_INTERVAL_MS), not
       granted instantly because the RSSI looks good — RSSI says nothing about
       throughput, and jumping straight to NL-96k on a link that cannot carry
       it is exactly the startup flood this function exists to prevent. */
    abr->min_state      = state;
    abr->probe_ms       = ABR_PROBE_INTERVAL_MS;
    abr->relax_ms       = now_ms + abr->probe_ms;
    abr->last_switch_ms = now_ms;
    abr->have_switched  = 1;   /* the first upgrade honours the normal hold */
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
    /* A long clean stretch means conditions may genuinely have changed (the
       laptops moved closer, interference stopped), so forget the accumulated
       backoff and be willing to probe promptly again. */
    if (abr->last_congest_ms && !congested &&
        now_ms - abr->last_congest_ms >= ABR_PROBE_RESET_MS)
        abr->probe_ms = ABR_PROBE_INTERVAL_MS;

    if (abr->min_state > ABR_STATE_NL_96K && now_ms >= abr->relax_ms &&
        abr->headroom && !congested) {
        abr->min_state--;
        abr->relax_ms = now_ms + abr->probe_ms;
    }

    ABRState target = abr_classify(rssi_dbm, loss_pct);

    if (congested) {
        /* Current mode doesn't fit — forbid it and everything better, and hold
           that ceiling for a while so we don't immediately probe back into it. */
        ABRState worse = (ABRState)(abr->current + 1);
        if (worse > ABR_STATE_HQ_48K) worse = ABR_STATE_HQ_48K;
        if (worse > abr->min_state) abr->min_state = worse;
        /* Each failed probe doubles the wait before the next one. Without this
           a rung that simply does not fit is retried on a fixed timer forever,
           and every retry is an audible break. */
        abr->probe_ms *= 2;
        if (abr->probe_ms > ABR_PROBE_MAX_MS) abr->probe_ms = ABR_PROBE_MAX_MS;
        abr->relax_ms       = now_ms + abr->probe_ms;
        abr->last_congest_ms = now_ms;
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
