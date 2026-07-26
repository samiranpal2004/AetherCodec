#ifndef AETHER_ENCODER_H
#define AETHER_ENCODER_H

#include <stdint.h>
#include "aether_packet.h"

/* Codec mode constants mirror AETHER_MODE_* in aether_packet.h */
#define AETHER_ENCODER_MODE_NL  AETHER_MODE_NL
#define AETHER_ENCODER_MODE_HQ  AETHER_MODE_HQ

/* HQ frames (MDCT hops) carried per packet by the streaming daemons.

   This was briefly 4, to amortise the 24-byte header+CRC: one hop per packet
   costs 187 packets/s at 96 kHz, ~36 kbps, which is a real 15% of a weak
   Bluetooth link. It is still a BAD trade, and measurably so — it was the
   cause of the HQ/auto stutter reported on 2026-07-26.

   The reason is loss granularity. A packet is the unit of concealment: when
   the send queue sheds one (which is exactly what a saturated link makes it
   do), the receiver fills that packet's whole duration with silence. At 4
   hops each drop is a 21.3 ms hole instead of 5.3 ms — four times the audible
   gap, for a 14% bitrate saving that does not prevent the drop in the first
   place. Dropped packets are what listeners hear; header bytes are not.

   NL is unaffected either way because its frame has always been 2048 samples,
   which is why the regression showed up only in HQ and in `auto` (whose lower
   rungs are HQ). Keep this at 1 unless the link is known lossless — the
   encoder still accepts any multiple of MDCT_HOP up to LPC_FRAME_SIZE, and
   test_codec_hq covers the batched path, so raising it stays a one-line
   change. */
#define AETHER_HQ_HOPS_PER_PKT  1

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

/* Switch mode AND sample rate mid-stream, preserving the packet sequence
   counter. Use this for every ABR transition instead of destroy + create:
   `sequence` belongs to the stream, not to the codec instance, and restarting
   it at 0 makes the receiver's jitter buffer discard every subsequent packet
   as "already played" until the counter climbs back past where it was. */
void aether_encoder_reconfigure(AetherEncoder *enc, int mode, int sample_rate);

void aether_encoder_destroy(AetherEncoder *enc);

#endif /* AETHER_ENCODER_H */
