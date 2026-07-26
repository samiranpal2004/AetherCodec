/* PipeWire playback stream: pulls decoded PCM out of a ring and hands it to the
   default output device (the 3.5mm jack on Laptop B). */
#include "pw_io.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aether_pw_init_once(void);   /* defined in pw_sink.c */

/* Frames per graph cycle we ask PipeWire to run (≈10.7 ms at 96 kHz). We force
   this both as the node latency AND as the buffer size, so d->maxsize equals
   exactly one quantum. Without that, PipeWire allocates maxsize for its
   *maximum* quantum (often 8192) while running a smaller one, and filling
   maxsize each callback over-drains the ring several-fold — the buffer then
   sits empty and playback stutters (observed: ~4x over-drain on 0.3.48, which
   has no pw_buffer.requested to tell us the real amount). */
#define PLAY_QUANTUM 1024

/* Prebuffer cushion, in milliseconds, before playout starts (and rebuilt after
   any full drain). This MUST exceed the sender's send-queue hold
   (SENDQ_MAX_MS, 500 ms in aether_sender.c): the sender will delay a packet by
   up to that long during a Bluetooth stall before it drops anything, so a
   smaller cushion here underruns on every such stall *even though no packet is
   lost* (observed as `underruns` climbing in chunks while `lost=0 dropped=0`).
   Sized above 500 ms so a burst the sender rides out by queuing is one the
   receiver rides out by buffering; a stall longer than the sender's hold makes
   it drop (bounding the delivery delay), and the receiver conceals that gap.
   Trades latency for smoothness — the right call for music, not gaming. */
#define PLAY_PREBUFFER_MS 600

struct PwPlay {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;
    struct spa_hook        listener;
    AudioRing             *ring;
    int                    channels;
    unsigned long          underruns;
    int                    primed;         /* prebuffer cushion reached? */
    int                    started;        /* has playout ever begun? */
    uint32_t               target_frames;  /* cushion before playout starts */
};

static void play_on_process(void *userdata) {
    PwPlay *p = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data) { pw_stream_queue_buffer(p->stream, b); return; }

    uint32_t stride    = sizeof(int32_t) * (uint32_t)p->channels;
    uint32_t maxframes = d->maxsize / stride;

    /* Produce exactly one quantum, never the whole buffer. Prefer the size
       PipeWire actually asks for (0.3.49+); otherwise our forced buffer size
       makes maxframes == PLAY_QUANTUM, but clamp anyway so a version that
       ignores the request can never make us over-drain. */
    uint32_t want = maxframes;
#if PW_CHECK_VERSION(0, 3, 49)
    if (b->requested) want = (uint32_t)b->requested;
#endif
    if (want > maxframes)    want = maxframes;
    if (want > PLAY_QUANTUM) want = PLAY_QUANTUM;

    /* Prebuffer: don't start draining until a cushion has built up, so bursty
       RFCOMM delivery and small A/B clock drift don't bounce the ring off
       empty. If we ever drain completely, re-prime rather than trickle. */
    if (!p->primed &&
        audio_ring_available(p->ring) >= p->target_frames * (uint32_t)p->channels) {
        p->primed  = 1;
        p->started = 1;
    }

    uint32_t nvals = want * (uint32_t)p->channels;
    int32_t *dst = (int32_t *)d->data;

    /* All-or-nothing: only drain when a whole quantum is available. Taking a
       partial read instead (the previous behaviour, which only re-primed on a
       *fully* empty ring) leaves the ring hovering near empty and hands the DAC
       a stream of half-filled buffers — continuous crackle rather than one
       clean gap followed by a rebuilt cushion. */
    uint32_t got = 0;
    if (p->primed) {
        if (audio_ring_available(p->ring) >= nvals)
            got = audio_ring_read(p->ring, dst, nvals);
        else
            p->primed = 0;                 /* starved — rebuild the cushion */
    }
    for (uint32_t i = 0; i < got; i++)
        dst[i] <<= 8;                      /* 24-bit range -> S32 */
    if (got < nvals) {
        memset(dst + got, 0, (nvals - got) * sizeof(int32_t));
        /* Silence before the very first prime is expected start-up latency, not
           an underrun. Silence after that — including a re-prime's cushion — is
           audio the listener should have heard, so count all of it. */
        if (p->started) p->underruns += (nvals - got) / (uint32_t)p->channels;
    }

    d->chunk->offset = 0;
    d->chunk->stride = (int32_t)stride;
    d->chunk->size   = nvals * sizeof(int32_t);

    pw_stream_queue_buffer(p->stream, b);
}

static const struct pw_stream_events play_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = play_on_process,
};

PwPlay* pw_play_start(const char *name, int rate, int channels, AudioRing *ring) {
    aether_pw_init_once();

    PwPlay *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->ring     = ring;
    p->channels = channels;
    /* Cushion before playout starts (see PLAY_PREBUFFER_MS). Deep on purpose:
       Bluetooth delivers in bursts, and this cushion is what keeps playback
       continuous across a stall the sender rode out by queuing. */
    p->target_frames = (uint32_t)rate * PLAY_PREBUFFER_MS / 1000u;

    p->loop = pw_thread_loop_new("aether-play", NULL);
    if (!p->loop) { free(p); return NULL; }

    pw_thread_loop_lock(p->loop);

    char latency[32];
    snprintf(latency, sizeof(latency), "%d/%d", PLAY_QUANTUM, rate);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_NODE_NAME,      name,
        PW_KEY_NODE_LATENCY,   latency,   /* ask the graph to run this quantum */
        NULL);

    p->stream = pw_stream_new_simple(pw_thread_loop_get_loop(p->loop),
                                     name, props, &play_events, p);
    if (!p->stream) {
        pw_thread_loop_unlock(p->loop);
        pw_thread_loop_destroy(p->loop);
        free(p);
        return NULL;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    struct spa_audio_info_raw info = {
        .format   = SPA_AUDIO_FORMAT_S32,
        .channels = (uint32_t)channels,
        .rate     = (uint32_t)rate,
    };
    params[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info);

    /* Pin the buffer to exactly one quantum so d->maxsize == PLAY_QUANTUM
       frames — this is what stops the over-drain on PipeWire builds that lack
       pw_buffer.requested. */
    uint32_t stride = sizeof(int32_t) * (uint32_t)channels;
    params[1] = spa_pod_builder_add_object(&pb,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, 8),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,    SPA_POD_Int((int)(PLAY_QUANTUM * stride)),
        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int((int)stride));

    if (pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 2) < 0) {
        pw_stream_destroy(p->stream);
        pw_thread_loop_unlock(p->loop);
        pw_thread_loop_destroy(p->loop);
        free(p);
        return NULL;
    }

    pw_thread_loop_unlock(p->loop);
    if (pw_thread_loop_start(p->loop) < 0) {
        pw_play_stop(p);
        return NULL;
    }
    return p;
}

unsigned long pw_play_underruns(const PwPlay *p) {
    return p ? p->underruns : 0;
}

void pw_play_stop(PwPlay *p) {
    if (!p) return;
    if (p->loop) pw_thread_loop_stop(p->loop);
    if (p->stream) {
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }
    if (p->loop) pw_thread_loop_destroy(p->loop);
    free(p);
}
