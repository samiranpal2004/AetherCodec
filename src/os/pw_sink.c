/* Virtual PipeWire sink: registers "AetherCodec Hi-Res BT" as an audio output
   device. Anything the user plays into it is captured here and handed to the
   encoder through a lock-free ring. */
#include "pw_io.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PwSink {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;
    struct spa_hook        listener;
    AudioRing             *ring;
    int                    channels;
};

/* pw_init must happen exactly once per process. */
static int pw_inited = 0;
void aether_pw_init_once(void) {
    if (!pw_inited) { pw_init(NULL, NULL); pw_inited = 1; }
}

static void sink_on_process(void *userdata) {
    PwSink *s = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data) {
        uint32_t offset = SPA_MIN(d->chunk->offset, d->maxsize);
        uint32_t size   = SPA_MIN(d->chunk->size, d->maxsize - offset);
        const int32_t *src = (const int32_t *)((uint8_t *)d->data + offset);
        uint32_t nvals = size / sizeof(int32_t);

        int32_t tmp[1024];
        uint32_t done = 0;
        while (done < nvals) {
            uint32_t chunk = nvals - done;
            if (chunk > 1024) chunk = 1024;
            for (uint32_t i = 0; i < chunk; i++)
                tmp[i] = src[done + i] >> 8;   /* S32 -> 24-bit range */
            /* Short write on overflow is intentional: dropping the oldest audio
               is better than blocking PipeWire's realtime thread. */
            audio_ring_write(s->ring, tmp, chunk);
            done += chunk;
        }
    }
    pw_stream_queue_buffer(s->stream, b);
}

static void sink_on_state_changed(void *userdata, enum pw_stream_state old,
                                  enum pw_stream_state state, const char *error) {
    (void)userdata;
    fprintf(stderr, "[pw_sink] %s -> %s%s%s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state),
            error ? " : " : "", error ? error : "");
}

static const struct pw_stream_events sink_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = sink_on_state_changed,
    .process       = sink_on_process,
};

PwSink* pw_sink_start(const char *node_name, const char *desc,
                      int rate, int channels, AudioRing *ring) {
    aether_pw_init_once();

    PwSink *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ring     = ring;
    s->channels = channels;

    s->loop = pw_thread_loop_new("aether-sink", NULL);
    if (!s->loop) { free(s); return NULL; }

    pw_thread_loop_lock(s->loop);

    /* media.class = Audio/Sink is what makes this show up as a selectable
       output device rather than just another stream.

       node.virtual marks it as not backed by real hardware. We deliberately do
       NOT set node.driver: a pw_stream provides no timing source, so claiming
       to be a driver can get this node selected to clock the graph, which then
       stalls. Real hardware (or PipeWire's Dummy-Driver) drives us instead. */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,       "Audio",
        PW_KEY_MEDIA_CATEGORY,   "Capture",
        PW_KEY_MEDIA_ROLE,       "Music",
        PW_KEY_MEDIA_CLASS,      "Audio/Sink",
        PW_KEY_NODE_NAME,        node_name,
        PW_KEY_NODE_DESCRIPTION, desc,
        PW_KEY_NODE_VIRTUAL,     "true",
        PW_KEY_NODE_ALWAYS_PROCESS, "true",
        NULL);

    s->stream = pw_stream_new_simple(pw_thread_loop_get_loop(s->loop),
                                     desc, props, &sink_events, s);
    if (!s->stream) {
        pw_thread_loop_unlock(s->loop);
        pw_thread_loop_destroy(s->loop);
        free(s);
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

    if (pw_stream_connect(s->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 1) < 0) {
        pw_stream_destroy(s->stream);
        pw_thread_loop_unlock(s->loop);
        pw_thread_loop_destroy(s->loop);
        free(s);
        return NULL;
    }

    pw_thread_loop_unlock(s->loop);
    if (pw_thread_loop_start(s->loop) < 0) {
        pw_sink_stop(s);
        return NULL;
    }
    return s;
}

void pw_sink_stop(PwSink *s) {
    if (!s) return;
    if (s->loop) pw_thread_loop_stop(s->loop);
    if (s->stream) {
        pw_stream_destroy(s->stream);
        s->stream = NULL;
    }
    if (s->loop) pw_thread_loop_destroy(s->loop);
    free(s);
}
