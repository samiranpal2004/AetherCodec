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

    /* After the probe interval, it may cautiously climb one rung again. The
       wait is not the base interval: each congestion event doubles it, and two
       have happened above (20s -> 40s -> 80s), so allow 4x the base. */
    t += 4 * ABR_PROBE_INTERVAL_MS;
    for (int i = 0; i < 5; i++) { abr_update_congested(a, -50, 0.0f, 0, t); t += TICK_MS; }
    assert(abr_current_state(a) < ABR_STATE_HQ_96K);
    printf("\xE2\x9C\x93 abr: probes back up after the backoff interval\n");
    abr_ctrl_destroy(a);

    /* --- repeated failure must back off, not retry forever ----------------
       Regression for the `HQ-48k -> HQ-96k -> congested -> HQ-48k` loop seen on
       a real link: a rung that does not fit was retried on a FIXED 20 s timer
       for the whole session, and every retry flushed the send queue and broke
       the audio. Each failed probe must now buy a longer silence before the
       next attempt. Measured as: how many probe attempts happen in a fixed
       stretch of wall-clock time where the upper rung always re-congests. */
    a = abr_ctrl_create(NULL, NULL);
    t = 0;
    int attempts = 0;
    ABRState prev = abr_current_state(a);
    /* 10 minutes of a link that always fails the rung above. */
    for (uint64_t end = t + 600000; t < end; t += TICK_MS) {
        ABRState before_tick = abr_current_state(a);
        /* Congested only when sitting above the bottom rung — i.e. the probe
           immediately fails, exactly like the logged HQ-96k behaviour. */
        int congested = (before_tick < ABR_STATE_HQ_48K);
        abr_update_congested(a, -50, 0.0f, congested, t);
        ABRState now = abr_current_state(a);
        if (now < before_tick) attempts++;      /* climbed = a probe attempt */
        prev = now;
    }
    (void)prev;
    /* A fixed 20 s timer would give ~30 attempts (and ~30 audible breaks) in
       10 minutes; doubling backoff must keep it to a handful. */
    assert(attempts <= 6);
    printf("\xE2\x9C\x93 abr: a rung that keeps failing is retried %d times in 10 min "
           "(fixed timer would be ~30)\n", attempts);
    abr_ctrl_destroy(a);

    /* --- headroom gates the probe ----------------------------------------
       Relaxing the ceiling purely on the timer means re-probing a mode the link
       has already proven it cannot carry, forever, at one audible blip per
       interval. With no reported headroom the ceiling must hold. */
    a = abr_ctrl_create(NULL, NULL);
    t = 0;
    abr_update_congested(a, -50, 0.0f, 1, t); t += TICK_MS;
    abr_update_congested(a, -50, 0.0f, 1, t); t += TICK_MS;
    ABRState settled = abr_current_state(a);
    assert(settled > ABR_STATE_NL_96K);

    abr_set_headroom(a, 0);
    for (int i = 0; i < 200; i++) {          /* far past several probe intervals */
        abr_update_congested(a, -50, 0.0f, 0, t);
        t += TICK_MS;
    }
    assert(abr_current_state(a) == settled);
    printf("\xE2\x9C\x93 abr: no headroom -> ceiling holds, no periodic re-probe\n");

    abr_set_headroom(a, 1);                  /* capacity really did free up */
    for (int i = 0; i < 200; i++) {
        abr_update_congested(a, -50, 0.0f, 0, t);
        t += TICK_MS;
    }
    assert(abr_current_state(a) == ABR_STATE_NL_96K);
    printf("\xE2\x9C\x93 abr: headroom restored -> climbs back to best quality\n");
    abr_ctrl_destroy(a);

    /* --- abr_start_at: the start rung is also the initial ceiling ---------
       `auto` starts at HQ-96k (NL-96k needs ~3 Mbps on real music — no RFCOMM
       link carries that, so the optimistic start flooded the queue every
       session). Strong RSSI alone must NOT jump the ladder above the start
       rung; that has to be earned through the headroom-gated probe. */
    a = abr_ctrl_create(NULL, NULL);
    t = 0;
    abr_start_at(a, ABR_STATE_HQ_96K, t);
    assert(abr_current_state(a) == ABR_STATE_HQ_96K);
    for (int i = 0; i < 20; i++) {   /* RSSI classifies NL-96k the whole time */
        abr_update_congested(a, -50, 0.0f, 0, t); t += TICK_MS;
    }
    assert(abr_current_state(a) == ABR_STATE_HQ_96K);
    /* Congestion still downgrades immediately from the start rung. */
    abr_update_congested(a, -50, 0.0f, 1, t); t += TICK_MS;
    assert(abr_current_state(a) == ABR_STATE_HQ_48K);
    printf("\xE2\x9C\x93 abr: start-at holds the start rung against strong RSSI, "
           "still downgrades on congestion\n");
    abr_ctrl_destroy(a);

    /* With real headroom the ceiling relaxes one rung per probe interval, so a
       link that genuinely has spare capacity can still reach the NL rungs. */
    a = abr_ctrl_create(NULL, NULL);
    t = 0;
    abr_start_at(a, ABR_STATE_HQ_96K, t);
    abr_set_headroom(a, 1);
    t += ABR_PROBE_INTERVAL_MS;
    for (int i = 0; i < 10; i++) { abr_update_congested(a, -50, 0.0f, 0, t); t += TICK_MS; }
    assert(abr_current_state(a) == ABR_STATE_NL_48K);
    printf("\xE2\x9C\x93 abr: start-at + proven headroom climbs one rung per "
           "probe interval\n");
    abr_ctrl_destroy(a);

    /* --- SMR recovery is fast while the queue is idle ---------------------
       After a congestion cut the controller used to crawl back at 0.4 dB per
       tick (~26 s from the floor to the ceiling) even with a bone-dry queue.
       The idle fast tier must recover in well under half that. */
    {
        float smr = ABR_SMR_MIN_DB;
        int ticks = 0;
        while (smr < ABR_SMR_MAX_DB && ticks < 1000) {
            smr = abr_smr_step(smr, 0, 0);   /* queue empty, nothing dropped */
            ticks++;
        }
        int max_ticks = (int)((ABR_SMR_MAX_DB - ABR_SMR_MIN_DB)
                              / ABR_SMR_UP_FAST_DB) + 1;
        assert(ticks <= max_ticks);
        printf("\xE2\x9C\x93 abr: idle-queue SMR recovery %g -> %g dB in %d ticks "
               "(%.1f s at 250 ms)\n",
               (double)ABR_SMR_MIN_DB, (double)ABR_SMR_MAX_DB,
               ticks, ticks * 0.25);
    }

    /* --- HQ rate controller converges ------------------------------------
       Simulate a link with a fixed capacity and let the AIMD loop find it. The
       model: bitrate rises ~25 kbps per dB of SMR, anything over capacity
       accumulates in the queue, anything under drains it. The loop must settle
       without pinning the queue and without collapsing to the floor. */
    const float kbps_at_min = 200.0f;        /* rate at ABR_SMR_MIN_DB          */
    const float kbps_per_db = 25.0f;
    for (int c = 0; c < 3; c++) {
        const float capacity[3] = { 900.0f, 520.0f, 300.0f };
        float smr = ABR_SMR_MAX_DB;
        float queue = 0.0f;                  /* ms of audio backed up           */
        int   drops = 0, settled_ticks = 0;

        for (int i = 0; i < 400; i++) {
            float kbps = kbps_at_min + (smr - ABR_SMR_MIN_DB) * kbps_per_db;
            /* Over/under capacity translates directly into queue growth. */
            queue += (kbps - capacity[c]) / capacity[c] * TICK_MS;
            if (queue < 0.0f) queue = 0.0f;
            drops = 0;
            if (queue > 500.0f) { queue = 500.0f; drops = 1; }
            smr = abr_smr_step(smr, (int)queue, drops);
            if (i > 200 && queue < ABR_QUEUE_HIGH_MS) settled_ticks++;
        }
        /* Settled: the queue spends the back half of the run drained, and the
           controller has not bottomed out unless the link genuinely demands it. */
        assert(settled_ticks > 150);
        assert(smr >= ABR_SMR_MIN_DB && smr <= ABR_SMR_MAX_DB);
        float final_kbps = kbps_at_min + (smr - ABR_SMR_MIN_DB) * kbps_per_db;
        assert(final_kbps <= capacity[c] * 1.15f);
        printf("\xE2\x9C\x93 abr: rate control settles at SMR %4.1f dB "
               "(~%.0f kbps) on a %.0f kbps link\n",
               smr, final_kbps, capacity[c]);
    }

    /* --- the SMR controller must SETTLE, not oscillate --------------------
       The convergence check above passes even for a controller that sawtooths,
       because the queue is drained most of the time either way — which is
       exactly how the real fault hid. On a real 700 kbps RFCOMM link the raw
       AIMD loop ran SMR 12 -> 54 -> flood -> 12 on a ~25 s cycle for the whole
       session, and every peak dropped a burst of packets: a break in the music
       roughly twice a minute. So assert on SMR STABILITY, which is what the
       listener actually experiences.

       Same link model as above, but measuring the spread of SMR over the back
       half of the run once the controller has had time to learn. */
    for (int c = 0; c < 3; c++) {
        const float capacity[3] = { 900.0f, 520.0f, 300.0f };
        SmrCtrl ctl;
        smr_ctrl_init(&ctl, ABR_SMR_ENTRY_DB);
        float queue = 0.0f;
        float lo = 1e9f, hi = -1e9f;
        int   floods = 0;

        for (int i = 0; i < 1200; i++) {          /* 1200 ticks = 5 min */
            float kbps = kbps_at_min + (ctl.smr - ABR_SMR_MIN_DB) * kbps_per_db;
            queue += (kbps - capacity[c]) / capacity[c] * TICK_MS;
            if (queue < 0.0f) queue = 0.0f;
            int drops = 0;
            if (queue > 500.0f) { queue = 500.0f; drops = 1; }
            smr_ctrl_step(&ctl, (int)queue, drops);

            if (i > 600) {                        /* back half: must be settled */
                if (ctl.smr < lo) lo = ctl.smr;
                if (ctl.smr > hi) hi = ctl.smr;
                if (drops) floods++;
            }
        }
        /* A sawtooth spans tens of dB; a settled controller barely moves. */
        assert(hi - lo <= 8.0f);
        /* And it must stop flooding the link once it has learned the limit. */
        assert(floods == 0);
        printf("\xE2\x9C\x93 abr: SMR settles within %.1f dB and floods 0 times over "
               "the last 2.5 min on a %.0f kbps link (ceiling learned %.1f dB)\n",
               hi - lo, capacity[c], ctl.ceiling);
    }

    printf("\nAll ABR tests passed.\n");
    return 0;
}
