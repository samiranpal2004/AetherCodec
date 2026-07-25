/* ABR state machine: classification, downgrade speed, upgrade hysteresis.
   Uses abr_update_at with an injected clock so the 3 s upgrade hold is tested
   deterministically instead of by sleeping. */
#include "abr_ctrl.h"
#include "aether_packet.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int   cb_calls;
static int   cb_mode, cb_rate;

static void on_switch(int mode, int rate, void *ud) {
    (void)ud;
    cb_calls++; cb_mode = mode; cb_rate = rate;
}

/* Poll interval the sender uses; hysteresis is expressed in these ticks. */
#define TICK_MS 500

int main(void) {
    /* --- 1. classification thresholds (HLD 6.2 / PRD 4.4) ---------------- */
    assert(abr_classify(-50, 0.0f) == ABR_STATE_NL_96K);
    assert(abr_classify(-70, 0.5f) == ABR_STATE_NL_48K);
    assert(abr_classify(-78, 2.0f) == ABR_STATE_HQ_96K);
    assert(abr_classify(-90, 0.0f) == ABR_STATE_HQ_48K);
    /* loss alone must be able to force a downgrade even with strong signal */
    assert(abr_classify(-50, 5.0f) == ABR_STATE_HQ_96K);
    assert(abr_classify(-50, 20.0f) == ABR_STATE_HQ_48K);
    printf("\xE2\x9C\x93 abr: link classification matches the spec ladder\n");

    /* --- 2. state/mode/rate mapping -------------------------------------- */
    assert(abr_state_mode(ABR_STATE_NL_96K) == AETHER_MODE_NL);
    assert(abr_state_mode(ABR_STATE_HQ_48K) == AETHER_MODE_HQ);
    assert(abr_state_rate(ABR_STATE_NL_96K) == 96000);
    assert(abr_state_rate(ABR_STATE_NL_48K) == 48000);
    assert(abr_state_rate(ABR_STATE_HQ_96K) == 96000);
    assert(abr_state_rate(ABR_STATE_HQ_48K) == 48000);
    printf("\xE2\x9C\x93 abr: state -> (mode, rate) mapping correct\n");

    /* --- 3. downgrade is fast: <= 2 polls (1 s at 500 ms) ---------------- */
    cb_calls = 0;
    ABRCtrl *a = abr_ctrl_create(on_switch, NULL);
    assert(abr_current_state(a) == ABR_STATE_NL_96K);

    uint64_t t = 0;
    abr_update_at(a, -90, 0.0f, 0, t); t += TICK_MS;
    assert(abr_current_state(a) == ABR_STATE_NL_96K);   /* 1 bad: not yet */
    abr_update_at(a, -90, 0.0f, 0, t); t += TICK_MS;
    assert(abr_current_state(a) == ABR_STATE_HQ_48K);   /* 2 bad: switched */
    assert(cb_calls == 1 && cb_mode == AETHER_MODE_HQ && cb_rate == 48000);
    printf("\xE2\x9C\x93 abr: downgrade after %d polls (%d ms) -> %s\n",
           ABR_DOWNGRADE_CONSECUTIVE, ABR_DOWNGRADE_CONSECUTIVE * TICK_MS,
           abr_state_name(abr_current_state(a)));

    /* --- 4. upgrade is slow: needs N good polls AND the 3 s hold ---------- */
    cb_calls = 0;
    for (int i = 0; i < ABR_UPGRADE_CONSECUTIVE; i++) {
        abr_update_at(a, -50, 0.0f, 0, t); t += TICK_MS;
    }
    /* Consecutive-good satisfied, but only 1.5 s since the downgrade. */
    assert(abr_current_state(a) == ABR_STATE_HQ_48K);
    assert(cb_calls == 0);

    t += ABR_UPGRADE_HOLD_MS;                  /* let the hold expire */
    abr_update_at(a, -50, 0.0f, 0, t); t += TICK_MS;
    assert(abr_current_state(a) == ABR_STATE_NL_96K);
    assert(cb_calls == 1 && cb_mode == AETHER_MODE_NL && cb_rate == 96000);
    printf("\xE2\x9C\x93 abr: upgrade held off until %d ms elapsed, then -> %s\n",
           ABR_UPGRADE_HOLD_MS, abr_state_name(abr_current_state(a)));
    abr_ctrl_destroy(a);

    /* --- 5. a flapping link must not oscillate --------------------------- */
    cb_calls = 0;
    a = abr_ctrl_create(on_switch, NULL);
    t = 0;
    for (int i = 0; i < 20; i++) {
        abr_update_at(a, (i % 2) ? -50 : -90, 0.0f, 0, t);
        t += TICK_MS;
    }
    /* Alternating good/bad never reaches 2 consecutive bad or 3 consecutive
       good, so the engine should hold its initial state. */
    assert(abr_current_state(a) == ABR_STATE_NL_96K);
    assert(cb_calls == 0);
    printf("\xE2\x9C\x93 abr: alternating good/bad link causes no switching\n");
    abr_ctrl_destroy(a);

    /* --- 6. steady state fires no spurious callbacks ---------------------- */
    cb_calls = 0;
    a = abr_ctrl_create(on_switch, NULL);
    t = 0;
    for (int i = 0; i < 50; i++) { abr_update_at(a, -50, 0.0f, 0, t); t += TICK_MS; }
    assert(cb_calls == 0 && abr_current_state(a) == ABR_STATE_NL_96K);
    printf("\xE2\x9C\x93 abr: stable link produces no mode changes\n");
    abr_ctrl_destroy(a);

    /* --- 7. gradual walk-away steps down the ladder in order -------------- */
    a = abr_ctrl_create(NULL, NULL);
    t = 0;
    int rssis[] = { -70, -70, -78, -78, -90, -90 };
    ABRState want[] = { ABR_STATE_NL_96K, ABR_STATE_NL_48K,
                        ABR_STATE_NL_48K, ABR_STATE_HQ_96K,
                        ABR_STATE_HQ_96K, ABR_STATE_HQ_48K };
    for (int i = 0; i < 6; i++) {
        abr_update_at(a, rssis[i], 0.0f, 0, t); t += TICK_MS;
        assert(abr_current_state(a) == want[i]);
    }
    printf("\xE2\x9C\x93 abr: walking away steps NL-96k -> NL-48k -> HQ-96k -> HQ-48k\n");
    abr_ctrl_destroy(a);

    /* --- 8. backpressure: strong RSSI but congested link must step down ---- */
    a = abr_ctrl_create(NULL, NULL);
    t = 0;
    /* RSSI says NL-96k is fine, but the send queue is backing up. */
    abr_update_congested(a, -50, 0.0f, 1, t); t += TICK_MS;
    assert(abr_current_state(a) == ABR_STATE_NL_48K);   /* immediate 1-step drop */
    abr_update_congested(a, -50, 0.0f, 1, t); t += TICK_MS;
    assert(abr_current_state(a) == ABR_STATE_HQ_96K);   /* still congested: again */
    printf("\xE2\x9C\x93 abr: congestion steps down despite strong RSSI\n");

    /* Congestion clears, RSSI still great — must NOT immediately climb back into
       the mode that just failed (would re-congest and blip). */
    for (int i = 0; i < 12; i++) { abr_update_congested(a, -50, 0.0f, 0, t); t += TICK_MS; }
    assert(abr_current_state(a) == ABR_STATE_HQ_96K);
    printf("\xE2\x9C\x93 abr: does not oscillate back into the congested mode\n");

    /* After the probe interval, it may cautiously climb one rung again. */
    t += ABR_PROBE_INTERVAL_MS;
    for (int i = 0; i < 5; i++) { abr_update_congested(a, -50, 0.0f, 0, t); t += TICK_MS; }
    assert(abr_current_state(a) < ABR_STATE_HQ_96K);
    printf("\xE2\x9C\x93 abr: probes back up after the backoff interval\n");
    abr_ctrl_destroy(a);

    printf("\nAll ABR tests passed.\n");
    return 0;
}
