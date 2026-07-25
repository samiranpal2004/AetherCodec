#ifndef AETHER_ENCODER_H
#define AETHER_ENCODER_H

#include <stdint.h>
#include "aether_packet.h"

/* Codec mode constants mirror AETHER_MODE_* in aether_packet.h */
#define AETHER_ENCODER_MODE_NL  AETHER_MODE_NL
#define AETHER_ENCODER_MODE_HQ  AETHER_MODE_HQ

typedef struct AetherEncoder AetherEncoder;

/* Create encoder.
   mode:        AETHER_MODE_NL or AETHER_MODE_HQ
   sample_rate: 44100, 48000, 88200, 96000
   bit_depth:   16 or 24
   channels:    1 or 2 */
AetherEncoder* aether_encoder_create(int mode, int sample_rate,
                                     int bit_depth, int channels);

/* Encode one frame of PCM samples.
   pcm_in:        interleaved samples, frame_samples count
   pkt_out:       caller-allocated AetherPacket buffer
   Returns: 0 on success (payload_size set on pkt_out), or -1 on error. */
int aether_encoder_encode(AetherEncoder *enc, const int32_t *pcm_in,
                          int frame_samples, AetherPacket *pkt_out);

/* Switch mode mid-stream (ABR callback). Takes effect from next frame. */
void aether_encoder_set_mode(AetherEncoder *enc, int new_mode);

void aether_encoder_destroy(AetherEncoder *enc);

#endif /* AETHER_ENCODER_H */
