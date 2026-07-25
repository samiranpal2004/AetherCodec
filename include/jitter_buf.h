#ifndef JITTER_BUF_H
#define JITTER_BUF_H

#include "aether_packet.h"

typedef struct JitterBuf JitterBuf;

/* target_ms: desired playout delay (recommended: 40ms) */
JitterBuf* jitter_buf_create(int target_ms, int sample_rate);

/* Insert received packet (can be out of order) */
void jitter_buf_insert(JitterBuf *jb, const AetherPacket *pkt);

/* Pop next frame for playout.
   Returns: pointer to packet, or NULL if nothing is due yet OR the packet was
   lost. Non-blocking (the HLD describes this as blocking, but the receiver
   drives playout from the PipeWire clock, so a blocking pop would stall the
   recv thread); use jitter_buf_pop_ex to tell "not ready" from "lost". */
const AetherPacket* jitter_buf_pop(JitterBuf *jb);

/* As jitter_buf_pop, but sets *lost to 1 when NULL means a genuinely missing
   packet that the caller should conceal (vs. simply not enough buffered yet). */
const AetherPacket* jitter_buf_pop_ex(JitterBuf *jb, int *lost);

/* Get current buffer level in ms (for stats reporting) */
int jitter_buf_level_ms(const JitterBuf *jb);

void jitter_buf_destroy(JitterBuf *jb);

#endif /* JITTER_BUF_H */
