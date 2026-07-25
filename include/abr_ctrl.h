#ifndef ABR_CTRL_H
#define ABR_CTRL_H

typedef struct ABRCtrl ABRCtrl;

/* Called when the ABR engine decides a mode/sample-rate switch. */
typedef void (*ABRCallback)(int new_mode, int new_sample_rate, void *userdata);

ABRCtrl* abr_ctrl_create(ABRCallback cb, void *userdata);

/* Feed link stats (called by stats_thread every 500ms). */
void abr_update(ABRCtrl *abr, int rssi_dbm, float packet_loss_pct,
                int jitter_buf_level_ms);

void abr_ctrl_destroy(ABRCtrl *abr);

#endif /* ABR_CTRL_H */
