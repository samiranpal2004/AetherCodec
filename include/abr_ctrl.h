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

/* Pure classification of a link condition — no hysteresis, no state. */
ABRState abr_classify(int rssi_dbm, float packet_loss_pct);

ABRState    abr_current_state(const ABRCtrl *abr);
const char* abr_state_name(ABRState s);
int         abr_state_mode(ABRState s);   /* AETHER_MODE_NL / AETHER_MODE_HQ */
int         abr_state_rate(ABRState s);   /* 96000 or 48000 */

#endif /* ABR_CTRL_H */
