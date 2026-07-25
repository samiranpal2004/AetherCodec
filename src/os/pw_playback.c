/* PipeWire playback stream: pulls decoded PCM out of a ring and hands it to the
   default output device (the 3.5mm jack on Laptop B). */
#include "pw_io.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdlib.h>
#include <string.h>

void aether_pw_init_once(void);   /* defined in pw_sink.c */

struct PwPlay {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;
    struct spa_hook        listener;
    AudioRing             *ring;
    int                    channels;
    unsigned long          underruns;
};

static void play_on_process(void *userdata) {
    PwPlay *p = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data) { pw_stream_queue_buffer(p->stream, b); return; }

    uint32_t stride    = sizeof(int32_t) * (uint32_t)p->channels;
    uint32_t maxframes = d->maxsize / stride;
#if PW_CHECK_VERSION(0, 3, 49)
    /* pw_buffer.requested only exists from 0.3.49; before that we simply fill
       the whole buffer, which is the behaviour those versions expect. */
    if (b->requested && b->requested < maxframes)
        maxframes = (uint32_t)b->requested;
#endif

    uint32_t nvals = maxframes * (uint32_t)p->channels;
    int32_t *dst = (int32_t *)d->data;

    uint32_t got = audio_ring_read(p->ring, dst, nvals);
    for (uint32_t i = 0; i < got; i++)
        dst[i] <<= 8;                      /* 24-bit range -> S32 */
    if (got < nvals) {
        memset(dst + got, 0, (nvals - got) * sizeof(int32_t));
        p->underruns += (nvals - got) / (uint32_t)p->channels;
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

    p->loop = pw_thread_loop_new("aether-play", NULL);
    if (!p->loop) { free(p); return NULL; }

    pw_thread_loop_lock(p->loop);

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_NODE_NAME,      name,
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
    const struct spa_pod *params[1];
    struct spa_audio_info_raw info = {
        .format   = SPA_AUDIO_FORMAT_S32,
        .channels = (uint32_t)channels,
        .rate     = (uint32_t)rate,
    };
    params[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 1) < 0) {
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
