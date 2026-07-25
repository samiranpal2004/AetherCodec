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
    return abr;
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

void abr_ctrl_destroy(ABRCtrl *abr) { free(abr); }
