#ifndef ABR_CTRL_H
#define ABR_CTRL_H

#include <stdint.h>

/* Adaptive bitrate quality ladder (HLD 6.2 / PRD 4.4).

   IMPORTANT: numerically LOWER == HIGHER quality. A degrading link therefore
   makes classify() return a LARGER value. The implementation plan's Step 5.2
   labels `target > current` as an "upgrade", which is inverted relative to this
   ordering — as written it would raise quality exactly when the link fails.
   The comparisons here are written the correct way round.

   Note also that the ladder's bitrates are not monotonically decreasing
   (1400 -> 800 -> 1000 -> 600 kbps): the NL_48K -> HQ_96K step actually *raises*
   demand on an already-degrading link. That is what the PRD specifies (it is a
   quality-preference ladder — stay lossless by dropping rate first, then accept
   lossy at full rate), so it is implemented as written, but it is worth knowing
   when reading a range test. */
typedef enum {
    ABR_STATE_NL_96K = 0,   /* best  — near-lossless 96 kHz, ~1400 kbps */
    ABR_STATE_NL_48K = 1,   /*       — near-lossless 48 kHz, ~800 kbps  */
    ABR_STATE_HQ_96K = 2,   /*       — perceptual HQ 96 kHz, ~1000 kbps */
    ABR_STATE_HQ_48K = 3    /* worst — perceptual HQ 48 kHz, ~600 kbps  */
} ABRState;

#define ABR_STATE_COUNT 4

/* Hysteresis (HLD 6.3). At the 500 ms poll interval this makes a downgrade
   take <= 1 s and an upgrade at least 3 s. */
#define ABR_DOWNGRADE_CONSECUTIVE 2
#define ABR_UPGRADE_CONSECUTIVE   3
#define ABR_UPGRADE_HOLD_MS       3000

/* After congestion forces a downgrade, how long before the engine probes one
   step back up. Long, so a link that simply can't carry a mode doesn't keep
   retrying it (each retry is an audible blip).

   A FIXED interval is not enough on its own: if the rung above genuinely does
   not fit, a constant timer retries it forever — measured as `HQ-48k -> HQ-96k
   -> congested -> HQ-48k` cycling every ~20 s for the whole session, each
   cycle flushing the send queue and costing an audible break. So the interval
   doubles on every failed probe up to ABR_PROBE_MAX_MS, and resets only after
   the link has stayed clean long enough to suggest conditions really changed.
   A link that can carry the higher rung still gets there; one that cannot
   stops asking. */
#define ABR_PROBE_INTERVAL_MS     20000
#define ABR_PROBE_MAX_MS         320000   /* ~5 min between hopeless retries  */
#define ABR_PROBE_RESET_MS       120000   /* clean this long => forget backoff */

/* ---- HQ rate control ----------------------------------------------------
   The four-rung ladder above is coarse: each step roughly halves the demand.
   Within a rung, HQ's bitrate is whatever the content happens to need at a
   fixed quality, which on a real link drifted from ~440 to ~860 kbps as the
   music got denser, overran the link and dropped ~20% of frames. A dropped
   frame is a hole in the audio; coarser quantisation is not. So HQ closes a
   loop on its signal-to-mask ratio.

   AIMD against send-queue depth: ease quality up while the queue drains
   freely, back off in proportion to how far behind it is. Pure function so the
   convergence behaviour is testable without a Bluetooth link. */
#define ABR_QUEUE_LOW_MS    40   /* below this there is room to spend more bits */
#define ABR_QUEUE_HIGH_MS   90   /* above this we are already behind            */
#define ABR_SMR_UP_DB      0.4f  /* additive increase — deliberately gentle     */
/* When the queue is essentially empty the link is demonstrably not the
   constraint, so climb faster. This matters after a congestion cut: at 0.4 dB
   per 250 ms tick, recovering from 12 dB took ~26 s of audibly degraded
   quality; the fast tier brings that under 10 s while still falling back to
   the gentle step as soon as the queue shows any depth. */
#define ABR_SMR_IDLE_MS     10   /* queue at/below this = link clearly idle     */
#define ABR_SMR_UP_FAST_DB 1.2f
/* Ceiling raised from 30 dB (2026-07-25 run three): at 30 dB HQ-96k settled at
   ~480 kbps against a measured ~900 kbps link — the controller was pinned at
   max with half the link unused, which is exactly the "HQ sounds average"
   report. 54 dB ≈ +4 bits per significant coefficient (~+280 kbps broadband at
   96 kHz), enough that on any realistic RFCOMM link the QUEUE, not this
   constant, is what stops the climb. Audible SNR tracks SMR ~1:1 above the
   floor, so a link with spare capacity now buys real quality with it. */
#define ABR_SMR_MAX_DB    54.0f
/* The codec allows down to 6 dB, but measured audible SNR there is ~2 dB —
   noise, and worse than the dropouts it would be avoiding. 12 dB (~12 dB SNR)
   is the floor worth having. Below it the right move is not to keep crushing
   the quantiser but to halve the sample rate, which buys the same bitrate back
   at full quality — so the controller stops here and lets the ladder take the
   step. In fixed --mode hq there is no ladder, and the sender warns instead. */
#define ABR_SMR_MIN_DB    12.0f

/* One raw AIMD tick. `queue_ms` is the sender's queued audio depth and
   `dropped` is non-zero if any frame was dropped since the previous tick.
   Returns the new SMR, clamped to [ABR_SMR_MIN_DB, ABR_SMR_MAX_DB].

   Use SmrCtrl below rather than calling this directly: on its own this climbs
   to ABR_SMR_MAX_DB every time the queue drains, which on a link that cannot
   sustain the top of the range means re-discovering the limit by overshooting
   it, forever. */
float abr_smr_step(float smr_db, int queue_ms, int dropped);

/* ---- SMR controller with congestion memory ------------------------------
   Raw AIMD has no memory: whenever the queue drains it climbs back to
   ABR_SMR_MAX_DB, floods the link, drops frames, cuts, and repeats. Measured
   on a real RFCOMM link that was the dominant audible fault — a ~25 s sawtooth
   (SMR 12 -> 54 -> flood -> 12) where every peak cost a burst of dropped
   packets, i.e. a break in the music roughly twice a minute, indefinitely.
   The queue spends most of its time drained either way, so a naive "did the
   queue stay low?" check does NOT catch this; only SMR stability does.

   The cure is to remember the level that failed and refuse to climb back into
   it: on congestion the ceiling drops to a margin below wherever it broke, and
   it is raised again only in small steps after a long, genuinely clean stretch.
   That turns the sawtooth into convergence — it finds the level the link can
   actually hold and stays there, which is what the listener wants. */
/* SMR to (re-)enter a rung at. Deliberately mid-range rather than the top:
   entering high floods the link before the controller's first tick can react,
   and the climb from here is fast while the queue is dry. */
#define ABR_SMR_ENTRY_DB    30.0f
#define ABR_SMR_BACKOFF_DB   3.0f  /* cap this far below where it congested   */
#define ABR_SMR_PROBE_DB     0.5f  /* how much to lift the cap when probing   */
#define ABR_SMR_PROBE_TICKS   40   /* clean ticks required first (~10 s)      */

typedef struct {
    float smr;          /* current signal-to-mask ratio, dB     */
    float ceiling;      /* learned safe cap, <= ABR_SMR_MAX_DB  */
    int   clean_ticks;  /* consecutive ticks with a drained queue */
} SmrCtrl;

/* start_db is the SMR to begin at; the ceiling starts fully open. */
void  smr_ctrl_init(SmrCtrl *c, float start_db);

/* One tick, same inputs as abr_smr_step, but ceiling-aware. */
float smr_ctrl_step(SmrCtrl *c, int queue_ms, int dropped);

typedef struct ABRCtrl ABRCtrl;

/* Fired when the engine commits to a new operating point. */
typedef void (*ABRCallback)(int new_mode, int new_sample_rate, void *userdata);

ABRCtrl* abr_ctrl_create(ABRCallback cb, void *userdata);
void     abr_ctrl_destroy(ABRCtrl *abr);

/* Feed link stats (the sender's ABR thread calls this every 500 ms). */
void abr_update(ABRCtrl *abr, int rssi_dbm, float packet_loss_pct,
                int jitter_buf_level_ms);

/* As abr_update but with an explicit clock, so hysteresis timing can be tested
   deterministically instead of by sleeping. */
void abr_update_at(ABRCtrl *abr, int rssi_dbm, float packet_loss_pct,
                   int jitter_buf_level_ms, uint64_t now_ms);

/* Congestion-aware update. `congested` means the sender cannot push the current
   mode fast enough (its send queue is backing up) — a throughput problem the
   RSSI does not reflect. When set, the engine drops at least one rung
   immediately and refuses to climb back above that rung for ABR_PROBE_INTERVAL_MS,
   so `auto` settles on the best mode the link can actually carry. This is the
   path the sender daemon uses; RSSI/loss still apply on top. */
void abr_update_congested(ABRCtrl *abr, int rssi_dbm, float packet_loss_pct,
                          int congested, uint64_t now_ms);

/* Report whether the sender currently has genuine spare capacity: its send
   queue is draining freely AND the encoder has no quality left to spend on the
   current rung. The congestion ceiling only relaxes while this holds, so `auto`
   settles on a mode that fits instead of re-probing an impossible one on a
   timer forever. Defaults to 1 (plain timer relax) if never called. */
void abr_set_headroom(ABRCtrl *abr, int headroom);

/* Start (or restart) the ladder at a specific rung. Sets both the current
   state and the congestion ceiling, so everything better than `state` must be
   earned via the headroom-gated relax path rather than granted on the first
   strong RSSI reading. The sender uses this to start `auto` at HQ-96k: the
   PRD's "start optimistic" NL-96k start needs ~3 Mbps on real music, which no
   RFCOMM link carries, so starting there guaranteed a ~500 ms queue flood, a
   cascade of congestion downgrades and several seconds of glitching at the
   top of every session. */
void abr_start_at(ABRCtrl *abr, ABRState state, uint64_t now_ms);

/* Pure classification of a link condition — no hysteresis, no state. */
ABRState abr_classify(int rssi_dbm, float packet_loss_pct);

ABRState    abr_current_state(const ABRCtrl *abr);
const char* abr_state_name(ABRState s);
int         abr_state_mode(ABRState s);   /* AETHER_MODE_NL / AETHER_MODE_HQ */
int         abr_state_rate(ABRState s);   /* 96000 or 48000 */

#endif /* ABR_CTRL_H */
