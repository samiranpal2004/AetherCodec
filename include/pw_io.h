#ifndef PW_IO_H
#define PW_IO_H

#include "audio_ring.h"

/* PipeWire integration.

   Architecture note: the HLD/implementation plan is self-contradictory here —
   Step 4.1 puts the virtual sink on Laptop B (receiver), but Step 4.4 then runs
   `pw-play --target aether_codec_sink` on Laptop A. Only one arrangement
   matches the stated product goal ("stream from any app on Laptop A, play on
   Laptop B's headphones"):

     Laptop A (sender)   : virtual SINK  — apps play into it, we capture+encode
     Laptop B (receiver) : PLAYBACK client — we decode and push to the DAC

   That is what is implemented.

   Samples are exchanged with PipeWire as S32 but the codec works in 24-bit
   range, so the conversion is a plain >>8 / <<8 (exact, no float rounding). */

typedef struct PwSink PwSink;   /* virtual sink: applications -> us */
typedef struct PwPlay PwPlay;   /* playback stream: us -> speakers  */

/* Registers a node named `node_name` (shown as `desc`) that appears in the
   system's audio output list. Captured samples are pushed into `ring`. */
PwSink* pw_sink_start(const char *node_name, const char *desc,
                      int rate, int channels, AudioRing *ring);
void    pw_sink_stop(PwSink *s);

/* Connects to the default output; pulls samples from `ring` (silence on
   underrun). */
PwPlay* pw_play_start(const char *name, int rate, int channels, AudioRing *ring);
void    pw_play_stop(PwPlay *p);

/* Total frames the playback stream had to fill with silence (underruns). */
unsigned long pw_play_underruns(const PwPlay *p);

#endif /* PW_IO_H */
