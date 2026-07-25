#ifndef JITTER_BUF_H
#define JITTER_BUF_H

#include "aether_packet.h"

typedef struct JitterBuf JitterBuf;

/* target_ms: desired playout delay (recommended: 40ms) */
JitterBuf* jitter_buf_create(int target_ms, int sample_rate);

/* Insert received packet (can be out of order) */
void jitter_buf_insert(JitterBuf *jb, const AetherPacket *pkt);

/* Pop next frame for playout (blocks until due time).
   Returns: pointer to packet, or NULL if packet lost (conceal). */
const AetherPacket* jitter_buf_pop(JitterBuf *jb);

/* Get current buffer level in ms (for stats reporting) */
int jitter_buf_level_ms(const JitterBuf *jb);

void jitter_buf_destroy(JitterBuf *jb);

#endif /* JITTER_BUF_H */
