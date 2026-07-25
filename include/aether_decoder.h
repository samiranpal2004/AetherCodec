#ifndef AETHER_DECODER_H
#define AETHER_DECODER_H

#include <stdint.h>
#include "aether_packet.h"

typedef struct AetherDecoder AetherDecoder;

AetherDecoder* aether_decoder_create(void);  // auto-detects mode from packets

/* Decode one AetherPacket.
   pcm_out:    caller-allocated, must hold max_samples int32_t values
   Returns: samples written, or -1 on error. */
int aether_decoder_decode(AetherDecoder *dec, const AetherPacket *pkt,
                          int32_t *pcm_out, int max_samples);

void aether_decoder_flush(AetherDecoder *dec);   // call on mode switch
void aether_decoder_destroy(AetherDecoder *dec);

#endif /* AETHER_DECODER_H */
