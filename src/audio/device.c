/* device.c: backend selection and the device manager (DENG-308).
 *
 * The device manager owns the backend instance and the open stream. Everything it does runs on
 * non-real-time threads under `dev.lock`: the control thread (`de-audio-ctl`), and callers of
 * de_audio_set_device / de_audio_list_devices. The mixer never waits for it.
 *
 * The control thread sleeps on `dev.wake`. Backends wake it through de__audio_device_notify (any
 * thread; it only ORs event bits and posts), and the mixer wakes it after a block that ended
 * voices. Each round it closes the sources of ended voices (de__audio_collect) and then handles
 * the device events:
 *   - the stream failed (STREAM_LOST): reopen the same target;
 *   - the default changed and the output follows the default: reopen the default;
 *   - the device list changed: an explicitly chosen device that went away falls back to the
 *     default, and comes back when it returns; output on the fallback stream retries the backend.
 *
 * A switch (reopen) stops and closes the old stream first; once `stop` returned, no render call
 * runs, so this thread takes the consumer side of the output ring and flushes it (the clock jumps
 * forward over the dropped audio). Then it opens and starts the new stream. The mixer thread keeps
 * running the whole time: it just finds the ring full until the new stream reads. When the backend
 * cannot open anything, output goes to a null stream (`stream_is_fallback`), so the clock and the
 * mixer keep going at the real-time rate.
 */
#include "audio/audio_internal.h"

#include <stdio.h>
#include <string.h>

#define DE__CTL_SAFETY_WAIT_NS 100000000ull /* the control thread also wakes every 100 ms */
#define DE__MAX_LISTED_DEVICES 32u

/* ---- Backend selection ----------------------------------------------------------------------- */

static const de__audio_backend_vtbl *backend_for(de_audio_backend kind) {
    switch (kind) {
    case DE_AUDIO_NULL:
        return &de__audio_backend_null;
#if DE_AUDIO_HAS_WASAPI
    case DE_AUDIO_WASAPI:
        return &de__audio_backend_wasapi;
#endif
#if DE_AUDIO_HAS_PIPEWIRE
    case DE_AUDIO_PIPEWIRE:
        return &de__audio_backend_pipewire;
#endif
#if DE_AUDIO_HAS_ALSA
    case DE_AUDIO_ALSA:
        return &de__audio_backend_alsa;
#endif
#if DE_AUDIO_HAS_COREAUDIO
    case DE_AUDIO_COREAUDIO:
        return &de__audio_backend_coreaudio;
#endif
    default:
        return NULL;
    }
}

/* The OS backends DE_AUDIO_DEFAULT tries, in order. */
static const de_audio_backend k_default_order[] = {
#if defined(_WIN32)
    DE_AUDIO_WASAPI,
#elif defined(__APPLE__)
    DE_AUDIO_COREAUDIO,
#elif defined(__linux__)
    DE_AUDIO_PIPEWIRE,
    DE_AUDIO_ALSA,
#endif
    DE_AUDIO_NULL,
};

/* ---- Streams --------------------------------------------------------------------------------- */

static void backend_underrun(void *user) {
    de_audio *a = user;
    de_atomic_fetch_add_u32(&a->underruns, 1, DE_ATOMIC_RELAXED);
}

static de__audio_stream_desc stream_desc(de_audio *a) {
    de__audio_stream_desc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = a->sample_rate;
    d.channels = a->channels;
    d.block_frames = a->block_frames;
    d.render = de__audio_ring_read;
    d.user = a;
    d.underrun = backend_underrun;
    return d;
}

/* dev.lock held. Stops and closes the current stream; this thread then owns the ring's consumer
 * side and flushes it. */
static void close_stream(de_audio *a) {
    de__audio_devmgr *d = &a->dev;
    if (d->stream == NULL)
        return;
    const de__audio_backend_vtbl *vt = d->stream->vt;
    vt->stop(d->stream);
    vt->close(d->stream);
    d->stream = NULL;
    d->stream_is_fallback = false;
    de__audio_ring_flush(a);
}

static de_result open_on(de_audio *a, const de__audio_backend_vtbl *vt, void *backend,
                         const char *id) {
    de__audio_stream_desc sd = stream_desc(a);
    de__audio_stream *s = NULL;
    de_result r = vt->open(backend, id, &sd, &s);
    if (r != DE_OK)
        return r;
    if (s->vt == NULL)
        s->vt = vt;
    r = s->vt->start(s);
    if (r != DE_OK) {
        s->vt->close(s);
        return r;
    }
    a->dev.stream = s;
    return DE_OK;
}

/* dev.lock held. (Re)opens the output: the requested device, else the default, else the null
 * fallback. Returns the result for the requested target (DE_OK when it is what plays now). */
static de_result reopen_inner(de_audio *a) {
    de__audio_devmgr *d = &a->dev;
    close_stream(a);
    const char *target = d->follow_default ? NULL : d->requested_id;
    de_result r = open_on(a, d->vt, d->backend, target);
    if (r == DE_OK) {
        snprintf(d->current_id, sizeof d->current_id, "%s", target != NULL ? target : "");
        return DE_OK;
    }
    if (target != NULL) {
        DE_WARN(a->log, "audio: cannot open device '%s' (%s); using the default", target,
                de_result_str(r));
        if (open_on(a, d->vt, d->backend, NULL) == DE_OK) {
            d->current_id[0] = '\0';
            return r;
        }
    }
    DE_WARN(a->log,
            "audio: no output device can be opened on '%s'; output goes to the null "
            "device",
            d->vt->name);
    const de__audio_backend_vtbl *fvt = d->fallback_vt != NULL ? d->fallback_vt : d->vt;
    void *fb = d->fallback_vt != NULL ? d->fallback : d->backend;
    de_result rf = open_on(a, fvt, fb, NULL);
    if (rf == DE_OK) {
        d->stream_is_fallback = true;
        d->current_id[0] = '\0';
    }
    return r;
}

static de_result reopen(de_audio *a) {
    de_result r = reopen_inner(a);
    /* Release: what the switch wrote (current_id, ...) is visible to whoever sees the count. */
    if (a->dev.stream != NULL)
        de_atomic_fetch_add_u32(&a->dev.opens, 1, DE_ATOMIC_RELEASE);
    return r;
}

static bool device_exists(de__audio_devmgr *d, const char *id) {
    de_audio_device_info list[DE__MAX_LISTED_DEVICES];
    u32 n = d->vt->enumerate(d->backend, list, DE__MAX_LISTED_DEVICES);
    if (n > DE__MAX_LISTED_DEVICES)
        n = DE__MAX_LISTED_DEVICES;
    for (u32 i = 0; i < n; ++i)
        if (strcmp(list[i].id, id) == 0)
            return true;
    return false;
}

static void handle_events(de_audio *a, u32 ev) {
    de__audio_devmgr *d = &a->dev;
    de_mutex_lock(&d->lock);
    if (!a->manual) {
        bool again = (ev & DE__AUDIO_EV_STREAM_LOST) != 0;
        if ((ev & DE__AUDIO_EV_DEFAULT_CHANGED) != 0 &&
            (d->follow_default || d->stream_is_fallback))
            again = true;
        if ((ev & DE__AUDIO_EV_DEVICES_CHANGED) != 0) {
            if (d->stream_is_fallback) {
                again = true; /* something may open now */
            } else if (!d->follow_default) {
                const bool on_requested = strcmp(d->current_id, d->requested_id) == 0;
                const bool present = device_exists(d, d->requested_id);
                if (on_requested != present)
                    again = true; /* it went away, or it came back */
            } else if (d->current_id[0] != '\0' && !device_exists(d, d->current_id)) {
                again = true;
            }
        }
        if (again)
            reopen(a);
    }
    de_mutex_unlock(&d->lock);
}

/* ---- Control thread -------------------------------------------------------------------------- */

static i32 ctl_main(void *user) {
    de_audio *a = (de_audio *)user;
    de__audio_devmgr *d = &a->dev;
    for (;;) {
        de_sem_wait_timeout(&d->wake, DE__CTL_SAFETY_WAIT_NS);
        if (de_atomic_load_u32(&d->quit, DE_ATOMIC_ACQUIRE) != 0)
            break;
        de__audio_collect(a);
        if (d->vt->poll_devices)
            d->vt->poll_devices(d->backend);
        u32 ev = de_atomic_exchange_u32(&d->events, 0, DE_ATOMIC_ACQ_REL);
        if (ev != 0)
            handle_events(a, ev);
    }
    return 0;
}

void de__audio_device_notify(void *user, u32 events) {
    de_audio *a = (de_audio *)user;
    de_atomic_fetch_or_u32(&a->dev.events, events, DE_ATOMIC_RELEASE);
    de_sem_post(&a->dev.wake, 1);
}

void de__audio_device_wake(de_audio *a) {
    de_sem_post(&a->dev.wake, 1);
}

u32 de__audio_device_opens(const de_audio *a) {
    return a != NULL ? de_atomic_load_u32(&a->dev.opens, DE_ATOMIC_ACQUIRE) : 0;
}

/* ---- Init and shutdown ----------------------------------------------------------------------- */

de_result de__audio_device_init(de_audio *a, de_audio_backend kind) {
    de__audio_devmgr *d = &a->dev;
    de_mutex_init(&d->lock);
    de_result r = de_sem_init(&d->wake, 0);
    if (r != DE_OK) {
        de_mutex_destroy(&d->lock);
        return r;
    }
    de_atomic_init_u32(&d->events, 0);
    de_atomic_init_u32(&d->quit, 0);
    de_atomic_init_u32(&d->opens, 0);
    d->follow_default = true;

    de__audio_backend_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.allocator = &a->alloc;
    bd.notify = de__audio_device_notify;
    bd.notify_user = a;

    if (kind == DE_AUDIO_DEFAULT) {
        for (u32 i = 0; i < sizeof k_default_order / sizeof k_default_order[0]; ++i) {
            const de__audio_backend_vtbl *vt = backend_for(k_default_order[i]);
            if (vt == NULL)
                continue;
            if (vt->create(&bd, &d->backend) == DE_OK) {
                d->vt = vt;
                break;
            }
            DE_WARN(a->log, "audio: backend '%s' is not available (%s)", vt->name,
                    de_error_last() != NULL ? de_error_last()->message : "?");
        }
        if (d->vt == NULL) {
            r = DE_FAIL(DE_ERR_INTERNAL, "audio: no backend could be created");
            goto fail;
        }
        if (d->vt->kind == DE_AUDIO_NULL && k_default_order[0] != DE_AUDIO_NULL)
            DE_INFO(a->log, "audio: no OS backend in this build; using the null device");
    } else {
        const de__audio_backend_vtbl *vt = backend_for(kind);
        if (vt == NULL) {
            r = DE_FAIL(DE_ERR_UNSUPPORTED, "audio: backend %d is not compiled in", (int)kind);
            goto fail;
        }
        r = vt->create(&bd, &d->backend);
        if (r != DE_OK)
            goto fail;
        d->vt = vt;
    }
    if (d->vt != &de__audio_backend_null) {
        r = de__audio_backend_null.create(&bd, &d->fallback);
        if (r != DE_OK)
            goto fail;
        d->fallback_vt = &de__audio_backend_null;
    }

    r = de_thread_create(ctl_main, a, "de-audio-ctl", DE_THREAD_NORMAL, 0, &d->thread);
    if (r != DE_OK) {
        r = DE_FAIL_WRAP(r, "audio: cannot start the device control thread");
        goto fail;
    }
    return DE_OK;

fail:
    de__audio_device_shutdown(a);
    return r;
}

de_result de__audio_device_start(de_audio *a) {
    if (a->manual)
        return DE_OK;
    de__audio_devmgr *d = &a->dev;
    de_mutex_lock(&d->lock);
    de_result r = reopen(a);
    const bool ok = d->stream != NULL;
    de_mutex_unlock(&d->lock);
    if (ok)
        return DE_OK;
    return r != DE_OK ? r : DE_FAIL(DE_ERR_INTERNAL, "audio: no output stream could be opened");
}

void de__audio_device_shutdown(de_audio *a) {
    de__audio_devmgr *d = &a->dev;
    if (d->thread != NULL) {
        de_atomic_store_u32(&d->quit, 1, DE_ATOMIC_RELEASE);
        de_sem_post(&d->wake, 1);
        de_thread_join(d->thread);
        d->thread = NULL;
    }
    if (d->stream != NULL) {
        const de__audio_backend_vtbl *vt = d->stream->vt;
        vt->stop(d->stream);
        vt->close(d->stream);
        d->stream = NULL;
    }
    if (d->fallback != NULL)
        d->fallback_vt->destroy(d->fallback);
    if (d->backend != NULL && d->vt != NULL)
        d->vt->destroy(d->backend);
    d->fallback = NULL;
    d->backend = NULL;
    de_sem_destroy(&d->wake);
    de_mutex_destroy(&d->lock);
}

/* ---- Public device API ----------------------------------------------------------------------- */

u32 de_audio_list_devices(de_audio *a, de_audio_device_info *out, u32 max) {
    if (a == NULL)
        return 0;
    if (out == NULL)
        max = 0;
    de_mutex_lock(&a->dev.lock);
    u32 n = a->dev.vt->enumerate(a->dev.backend, out, max);
    de_mutex_unlock(&a->dev.lock);
    return n;
}

de_result de_audio_set_device(de_audio *a, const char *id) {
    if (a == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_set_device: a is NULL");
    if (id != NULL && strlen(id) >= sizeof a->dev.requested_id)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_set_device: id too long");
    de__audio_devmgr *d = &a->dev;
    de_mutex_lock(&d->lock);
    if (id != NULL && !device_exists(d, id)) {
        de_mutex_unlock(&d->lock);
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_audio_set_device: no device '%s'", id);
    }
    d->follow_default = id == NULL;
    snprintf(d->requested_id, sizeof d->requested_id, "%s", id != NULL ? id : "");
    de_result r = DE_OK;
    if (a->manual)
        snprintf(d->current_id, sizeof d->current_id, "%s", id != NULL ? id : "");
    else
        r = reopen(a);
    de_mutex_unlock(&d->lock);
    return r;
}
