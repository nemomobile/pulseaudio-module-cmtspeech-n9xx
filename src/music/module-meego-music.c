/*
 * This file is part of pulseaudio-meego
 *
 * Copyright (C) 2008, 2009 Nokia Corporation. All rights reserved.
 *
 * Contact: Maemo Multimedia <multimedia@maemo.org>
 *
 * This software, including documentation, is protected by copyright
 * controlled by Nokia Corporation. All rights are reserved.
 *
 * Copying, including reproducing, storing, adapting or translating,
 * any or all of this material requires the prior written consent of
 * Nokia Corporation. This material also contains confidential
 * information which may not be disclosed to others without the prior
 * written consent of Nokia.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <pulse/volume.h>
#include <pulse/xmalloc.h>
#include <pulse/proplist.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/mutex.h>
#include <pulsecore/atomic.h>
#include <pulsecore/thread.h>
#include <pulsecore/sample-util.h>

#include "module-meego-music-symdef.h"

#include <src/common/proplist-meego.h>
#include <src/common/algorithm-hook.h>

#include "module-music-api.h"

PA_MODULE_AUTHOR("Jyri Sarha");
PA_MODULE_DESCRIPTION("Nokia music module");
PA_MODULE_USAGE("master_sink=<sink to connect to> "
                "sink_name=<name of created sink>");
PA_MODULE_VERSION(PACKAGE_VERSION);

static const char* const valid_modargs[] = {
    "master_sink",
    "sink_name",
    NULL,
};

#if defined(DEBUG)
FILE* inputFile;
FILE* outputFile;
#endif

#define SAMPLE_RATE_HW_HZ (48000)
#define PROPLIST_SINK "sink.hw0"

struct userdata {
    pa_core *core;
    pa_module *module;

    size_t window_size;

    pa_sink *master_sink;
    pa_sink *sink;

    pa_sink_input *sink_input;
    pa_memchunk silence_memchunk;

    algorithm_hook *algorithm;

    pa_hook *hook_algorithm;
    pa_hook *hook_volume;
};


static void get_max_input_volume(pa_sink *s, pa_cvolume *max_volume, const pa_channel_map *channel_map) {
    pa_sink_input *i;
    uint32_t idx;
    pa_assert(max_volume);
    pa_sink_assert_ref(s);

    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        pa_cvolume remapped_volume;

        if (i->origin_sink) {
            /* go recursively on slaved flatten sink
             * and ignore this intermediate sink-input. (This is not really needed) */
            get_max_input_volume(i->origin_sink, max_volume, channel_map);
            continue;
        }

        remapped_volume = i->volume;
        pa_cvolume_remap(&remapped_volume, &i->channel_map, channel_map);
        pa_cvolume_merge(max_volume, max_volume, &remapped_volume);
    }
}

static void update_mdrc_volume(struct userdata *u) {
    pa_cvolume max_input_volume;
    float volume;
    pa_assert(u);

    pa_cvolume_mute(&max_input_volume, u->sink->channel_map.channels);
    get_max_input_volume(u->sink, &max_input_volume, &u->sink->channel_map);

    volume = pa_sw_volume_to_dB(pa_cvolume_avg(&max_input_volume));

    pa_hook_fire(u->hook_volume, &volume);
}

/*** sink callbacks ***/

/* Called from I/O thread context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t usec = 0;

            if ((u->master_sink == NULL) || PA_MSGOBJECT(u->master_sink)->process_msg(
                        PA_MSGOBJECT(u->master_sink), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
                usec = 0;

            *((pa_usec_t*) data) = usec;
            return 0;
        }
        case PA_SINK_MESSAGE_SET_VOLUME: {
            update_mdrc_volume(u);
            // Pass trough to pa_sink_process_msg
            break;
        }
        case PA_SINK_MESSAGE_ADD_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(data);
            pa_assert(i != u->sink_input);
            // Pass trough to pa_sink_process_msg
            break;
        }

    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from main context */
static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    if (PA_SINK_IS_LINKED(state) && u->sink_input && PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(u->sink_input)))
        pa_sink_input_cork(u->sink_input, state == PA_SINK_SUSPENDED);

    pa_log_debug("sink_set_state() called with %d", state);
    return 0;
}

/* Called from I/O thread context */
static void sink_request_rewind(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master sink */
    pa_sink_input_request_rewind(u->sink_input, s->thread_info.rewind_nbytes, TRUE, FALSE, FALSE);

}

/* Called from I/O thread context */
static void sink_update_requested_latency(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    /* Just hand this one over to the master sink */
    pa_sink_input_set_requested_latency_within_thread(
            u->sink_input,
            pa_sink_get_requested_latency_within_thread(s));
}

/*** sink_input callbacks ***/
static int sink_input_pop_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    u = i->userdata;
    pa_assert(chunk && u);

    if (u->sink->thread_info.rewind_requested)
        pa_sink_process_rewind(u->sink, 0);

    if (!PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
	/* There are no clients playing to music sink,
	   let's just cut out silence and be done with it. */
	pa_silence_memchunk_get(&u->core->silence_cache,
				u->core->mempool,
				chunk,
				&i->sample_spec,
				length);
    } else {
        pa_sink_render_full(u->sink, u->window_size, chunk);

        /* pa_log("chunk length: %d", chunk->length); */
        pa_hook_fire(u->hook_algorithm, chunk);
    }

    return 0;
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    size_t amount = 0;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_OPENED(u->sink->thread_info.state))
        return;

    if (u->sink->thread_info.rewind_nbytes > 0) {

        amount = PA_MIN(u->sink->thread_info.rewind_nbytes, nbytes);
        u->sink->thread_info.rewind_nbytes = 0;
    }

    pa_sink_process_rewind(u->sink, amount);
}

/* Called from I/O thread context */
static void sink_input_update_max_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_max_rewind_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_max_request_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_max_request_within_thread(u->sink, nbytes);
}

/* Called from I/O thread context */
static void sink_input_update_sink_latency_range_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
}

/* Called from I/O thread context */
static void sink_input_update_sink_fixed_latency_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
}

static void sink_inputs_may_move(pa_sink *s, pa_bool_t move) {
    pa_sink_input *i;
    uint32_t idx;

    for (i = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); i; i = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {
        if (move)
            i->flags &= ~PA_SINK_INPUT_DONT_MOVE;
        else
            i->flags |= PA_SINK_INPUT_DONT_MOVE;
    }
}


/* Called from I/O thread context */
static void sink_input_detach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (PA_SINK_IS_LINKED(u->sink->thread_info.state))
        pa_sink_detach_within_thread(u->sink);
    else
        pa_log("fixme: !PA_SINK_IS_LINKED ?");

    pa_sink_set_rtpoll(u->sink, NULL);
    sink_inputs_may_move(u->sink, FALSE);
}

/* Called from I/O thread context */
static void sink_input_attach_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->sink || !PA_SINK_IS_LINKED(u->sink->thread_info.state))
        return;

    sink_inputs_may_move(u->sink, TRUE);
    pa_sink_set_rtpoll(u->sink, i->sink->thread_info.rtpoll);
    pa_sink_attach_within_thread(u->sink);

    pa_sink_set_latency_range_within_thread(u->sink, i->sink->thread_info.min_latency, i->sink->thread_info.max_latency);
    pa_sink_set_fixed_latency_within_thread(u->sink, i->sink->thread_info.fixed_latency);
    pa_sink_set_max_request_within_thread(u->sink, pa_sink_input_get_max_request(i));
    pa_sink_set_max_rewind_within_thread(u->sink, i->sink->thread_info.max_rewind);
}

/* Called from main context */
static void sink_input_moving_cb(pa_sink_input *i, pa_sink *dest){
    struct userdata *u;
    pa_proplist *p;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!dest)
        return; /* The sink input is going to be killed, don't do anything. */

    u->master_sink = dest;
    pa_sink_update_flags(u->sink, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY, dest->flags);
    pa_sink_set_asyncmsgq(u->sink, i->sink->asyncmsgq);

    p = pa_proplist_new();
    pa_proplist_setf(p, PA_PROP_DEVICE_DESCRIPTION, "%s connected to %s", u->sink->name, u->master_sink->name);
    pa_proplist_sets(p, PA_PROP_DEVICE_MASTER_DEVICE, u->master_sink->name);
    pa_sink_update_proplist(u->sink, PA_UPDATE_REPLACE, p);
    pa_proplist_free(p);

    u->sink->flat_volume_sink = u->master_sink;
}

/* Called from main context */
static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_unlink(u->sink);

    /* FIXME: this is sort-of understandable with the may_move hack... we avoid abort in free() here */
    u->sink_input->thread_info.attached = FALSE;
    pa_sink_input_unlink(u->sink_input);

    pa_sink_unref(u->sink);
    u->sink = NULL;
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_module_unload_request(u->module, TRUE);
}

/* Called from IO thread context */
static void sink_input_state_change_cb(pa_sink_input *i, pa_sink_input_state_t state) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    /* If we are added for the first time, ask for a rewinding so that
     * we are heard right-away. */
    if (PA_SINK_INPUT_IS_LINKED(state) &&
        i->thread_info.state == PA_SINK_INPUT_INIT &&
        PA_SINK_INPUT_IS_LINKED(i->thread_info.state)) {
        pa_log_debug("Requesting rewind due to state change.");
        pa_sink_input_request_rewind(i, 0, FALSE, TRUE, FALSE);
    }
}

static void set_hooks(struct userdata *u) {
    u->algorithm = algorithm_hook_get(u->core);
    u->hook_algorithm   = algorithm_hook_init(u->algorithm, MUSIC_HOOK_DYNAMIC_ENHANCE);
    u->hook_volume      = algorithm_hook_init(u->algorithm, MUSIC_HOOK_DYNAMIC_ENHANCE_VOLUME);
}

static void unset_hooks(struct userdata *u) {
    algorithm_hook_done(u->algorithm, MUSIC_HOOK_DYNAMIC_ENHANCE);
    algorithm_hook_done(u->algorithm, MUSIC_HOOK_DYNAMIC_ENHANCE_VOLUME);

    algorithm_hook_unref(u->algorithm);
    u->algorithm = NULL;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *sink_name, *master_sink_name;
    pa_sink *master_sink;
    pa_sample_spec ss;
    pa_channel_map map;
    char t[256];
    pa_sink_input_new_data sink_input_data;
    pa_sink_new_data sink_data;

#if defined(DEBUG)
    if ((inputFile = fopen("/tmp/input_data", "wb")) == NULL) {
        printf("Error opening: %s\n", "input_data");
        return -1;
    }
    if ((outputFile = fopen("/tmp/output_data", "wb")) == NULL) {
        printf("Error opening: %s\n", "output_data");
        return -1;
    }
#endif
    u = pa_xnew0(struct userdata, 1);

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    sink_name = pa_modargs_get_value(ma, "sink_name", NULL);
    master_sink_name = pa_modargs_get_value(ma, "master_sink", NULL);

    pa_log_debug("Got arguments: sink_name=\"%s\" master_sink=\"%s\".",
                 sink_name, master_sink_name);

    if (!(master_sink = pa_namereg_get(m->core, master_sink_name, PA_NAMEREG_SINK))) {
        pa_log("Master sink \"%s\" not found", master_sink_name);
        goto fail;
    }

    /*
      ss = master_sink->sample_spec;
      map = master_sink->channel_map;
    */

    ss.format = PA_SAMPLE_S16LE;

    //ss.format = master_sink->sample_spec.format;
    ss.rate = SAMPLE_RATE_HW_HZ;
    ss.channels = 2;
    pa_channel_map_init_stereo(&map);

    m->userdata = u;
    u->core = m->core;

    set_hooks(u);

    u->module = m;
    // The result is rounded down incorrectly thus 5001...
    // 5000 us = 5 ms
    // 20000 us = 20 ms
    u->window_size = pa_usec_to_bytes(20001, &ss);
    //u->window_size = 160;
    //u->window_size = 960;
    pa_log_debug("window size: %d frame size: %d",  u->window_size, pa_frame_size(&ss));
    u->master_sink = master_sink;
    u->sink = NULL;
    u->sink_input = NULL;
    pa_silence_memchunk_get(&u->core->silence_cache,
                            u->core->mempool,
                            &u->silence_memchunk,
                            &ss,
                            u->window_size);

    pa_sink_new_data_init(&sink_data);
    sink_data.module = m;
    sink_data.driver = __FILE__;
    sink_data.flat_volume_sink = master_sink;
      pa_sink_new_data_set_name(&sink_data, sink_name);
    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);
    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "%s connected to %s", sink_name, master_sink->name);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_MASTER_DEVICE, master_sink->name);
    pa_proplist_sets(sink_data.proplist, "module-suspend-on-idle.timeout", "1");
    pa_proplist_sets(sink_data.proplist,
                     PA_PROP_SINK_MUSIC_API_EXTENSION_PROPERTY_NAME,
                     PA_PROP_SINK_MUSIC_API_EXTENSION_PROPERTY_VALUE);

    /* Create sink */
    u->sink = pa_sink_new(m->core, &sink_data,
                          master_sink->flags & (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY));
    pa_sink_new_data_done(&sink_data);
    if (!u->sink) {
      pa_log("Failed to create sink.");
      goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state = sink_set_state;
    u->sink->update_requested_latency = sink_update_requested_latency;
    u->sink->request_rewind = sink_request_rewind;
    u->sink->userdata = u;
    pa_memblock_ref(u->silence_memchunk.memblock);
    u->sink->silence = u->silence_memchunk;

    pa_sink_set_asyncmsgq(u->sink, u->master_sink->asyncmsgq);
    pa_sink_set_rtpoll(u->sink, u->master_sink->thread_info.rtpoll);

    pa_sink_input_new_data_init(&sink_input_data);
    sink_input_data.flags = 0; // PA_SINK_INPUT_DONT_MOVE
    snprintf(t, sizeof(t), "output of %s", sink_name);
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_MEDIA_NAME, t);
    pa_proplist_sets(sink_input_data.proplist, PA_PROP_APPLICATION_NAME, t); /* this is the default value used by PA modules */
    sink_input_data.sink = master_sink;
    sink_input_data.driver = __FILE__;
    sink_input_data.module = m;
    sink_input_data.origin_sink = u->sink;
    pa_sink_input_new_data_set_sample_spec(&sink_input_data, &ss);
    pa_sink_input_new_data_set_channel_map(&sink_input_data, &map);

    pa_sink_input_new(&u->sink_input, m->core, &sink_input_data);
    pa_sink_input_new_data_done(&sink_input_data);
    if (!u->sink_input) {
        pa_log("Failed to create sink input.");
        goto fail;
    }

    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->update_max_rewind = sink_input_update_max_rewind_cb;
    u->sink_input->update_max_request = sink_input_update_max_request_cb;
    u->sink_input->update_sink_latency_range = sink_input_update_sink_latency_range_cb;
    u->sink_input->update_sink_fixed_latency = sink_input_update_sink_fixed_latency_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->attach = sink_input_attach_cb;
    u->sink_input->detach = sink_input_detach_cb;
    u->sink_input->state_change = sink_input_state_change_cb;
    u->sink_input->moving = sink_input_moving_cb;
    u->sink_input->userdata = u;

    pa_sink_put(u->sink);
    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    return 0;

 fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    unset_hooks(u);

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }

    if (u->sink) {
        pa_sink_unlink(u->sink);
        pa_sink_unref(u->sink);
    }

    if (u->silence_memchunk.memblock)
        pa_memblock_unref(u->silence_memchunk.memblock);

#if defined(DEBUG)
    fclose(inputFile);
    fclose(outputFile);
#endif

    pa_xfree(u);
}
