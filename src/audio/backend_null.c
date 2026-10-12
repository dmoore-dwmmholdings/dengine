/* backend_null.c: the null output backend (DENG-308), always compiled in.
 *
 * Realtime mode: an open stream runs a thread (`de-audio-null`) that pulls one block from the
 * mixer every block period and drops it, so the mixer, the clock and the underrun counter behave
 * as with a device. It paces with a timed wait on its own semaphore against absolute deadlines
 * (Windows rounds timed waits to the 15.6 ms tick, so there it pulls a few blocks at once; with
 * the default 3-block ring that can underrun, which is fine for a null device). After a stall of
 * more than 250 ms (a debugger) it starts a new timeline instead of catching up.
 *
 * Manual mode (de__audio_create_ex with null_manual): no stream and no mixer thread;
 * de__audio_null_pump renders and reads blocks on the caller's thread. Deterministic, for tests.
 *
 * The device list is simulated: one device "null" (the default) at start; the test hooks below
 * add and remove devices and change the default, and notify like an OS would.
 */
#include "audio/audio_internal.h"

#include <stdio.h>
#include <string.h>

#define NULL_MAX_DEVICES 8u
#define NULL_RESYNC_NS   250000000ull

typedef struct null_backend {
    de_allocator alloc;
    de__audio_notify_fn notify;
    void *notify_user;
    de_mutex lock; /* the device list; never taken on a stream thread */
    de_audio_device_info devices[NULL_MAX_DEVICES];
    u32 count;
    de_atomic_u64 frames_played;
} null_backend;

typedef struct null_stream {
    de__audio_stream base; /* first */
    null_backend *b;
    de__audio_stream_desc desc;
    f32 *buf;
    size_t buf_bytes;
    de_thread *thread;
    de_sem wake;
    de_atomic_u32 quit;
} null_stream;

static void set_device(de_audio_device_info *d, const char *id, const char *name, bool def) {
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "%s", id);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->channels = DE__AUDIO_CHANNELS;
    d->is_default = def;
}

static i32 find_device(const null_backend *b, const char *id) {
    for (u32 i = 0; i < b->count; ++i)
        if (strcmp(b->devices[i].id, id) == 0)
            return (i32)i;
    return -1;
}

static de_result null_create(const de__audio_backend_desc *desc, void **out) {
    null_backend *b = DE_NEW(desc->allocator, null_backend, DE_MEM_AUDIO);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "audio: cannot allocate the null backend");
    memset(b, 0, sizeof *b);
    b->alloc = *desc->allocator;
    b->notify = desc->notify;
    b->notify_user = desc->notify_user;
    de_mutex_init(&b->lock);
    set_device(&b->devices[0], "null", "Null output", true);
    b->count = 1;
    de_atomic_init_u64(&b->frames_played, 0);
    *out = b;
    return DE_OK;
}

static void null_destroy(void *backend) {
    null_backend *b = (null_backend *)backend;
    if (b == NULL)
        return;
    de_mutex_destroy(&b->lock);
    de_allocator a = b->alloc;
    DE_FREE(&a, b, sizeof *b);
}

static u32 null_enumerate(void *backend, de_audio_device_info *out, u32 max) {
    null_backend *b = (null_backend *)backend;
    de_mutex_lock(&b->lock);
    u32 n = b->count;
    for (u32 i = 0; i < n && i < max && out != NULL; ++i)
        out[i] = b->devices[i];
    de_mutex_unlock(&b->lock);
    return n;
}

static i32 null_stream_main(void *user) {
    null_stream *s = (null_stream *)user;
    const u32 frames = s->desc.block_frames;
    const u64 rate = s->desc.sample_rate;
    u64 t0 = de_time_now_ns();
    u64 done = 0; /* frames delivered since t0 */
    while (de_atomic_load_u32(&s->quit, DE_ATOMIC_ACQUIRE) == 0) {
        const u64 due = t0 + (done + frames) * 1000000000ull / rate;
        const u64 now = de_time_now_ns();
        if (now < due) {
            de_sem_wait_timeout(&s->wake, due - now);
            continue;
        }
        if (now - due > NULL_RESYNC_NS) {
            t0 = now;
            done = 0;
        }
        s->desc.render(s->desc.user, s->buf, frames);
        done += frames;
        de_atomic_fetch_add_u64(&s->b->frames_played, frames, DE_ATOMIC_RELAXED);
    }
    return 0;
}

static de_result null_open(void *backend, const char *device_id, const de__audio_stream_desc *desc,
                           de__audio_stream **out) {
    null_backend *b = (null_backend *)backend;
    *out = NULL;
    if (device_id != NULL) {
        de_mutex_lock(&b->lock);
        i32 i = find_device(b, device_id);
        de_mutex_unlock(&b->lock);
        if (i < 0)
            return DE_FAIL(DE_ERR_NOT_FOUND, "audio: no null device '%s'", device_id);
    }
    null_stream *s = DE_NEW(&b->alloc, null_stream, DE_MEM_AUDIO);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "audio: cannot allocate a null stream");
    memset(s, 0, sizeof *s);
    s->base.vt = &de__audio_backend_null;
    s->base.backend = b;
    s->b = b;
    s->desc = *desc;
    s->buf_bytes = (size_t)desc->block_frames * desc->channels * sizeof(f32);
    s->buf = (f32 *)DE_ALLOC(&b->alloc, s->buf_bytes, 64, DE_MEM_AUDIO);
    if (s->buf == NULL) {
        DE_FREE(&b->alloc, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "audio: cannot allocate a null stream buffer");
    }
    de_result r = de_sem_init(&s->wake, 0);
    if (r != DE_OK) {
        DE_FREE(&b->alloc, s->buf, s->buf_bytes);
        DE_FREE(&b->alloc, s, sizeof *s);
        return r;
    }
    de_atomic_init_u32(&s->quit, 0);
    *out = &s->base;
    return DE_OK;
}

static de_result null_start(de__audio_stream *base) {
    null_stream *s = (null_stream *)base;
    if (s->thread != NULL)
        return DE_OK;
    de_atomic_store_u32(&s->quit, 0, DE_ATOMIC_RELAXED);
    de_result r =
        de_thread_create(null_stream_main, s, "de-audio-null", DE_THREAD_HIGH, 0, &s->thread);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "audio: cannot start the null stream thread");
    return DE_OK;
}

static void null_stop(de__audio_stream *base) {
    null_stream *s = (null_stream *)base;
    if (s->thread == NULL)
        return;
    de_atomic_store_u32(&s->quit, 1, DE_ATOMIC_RELEASE);
    de_sem_post(&s->wake, 1);
    de_thread_join(s->thread); /* after the join no render call runs */
    s->thread = NULL;
}

static void null_close(de__audio_stream *base) {
    null_stream *s = (null_stream *)base;
    if (s == NULL)
        return;
    null_stop(base);
    de_sem_destroy(&s->wake);
    null_backend *b = s->b;
    DE_FREE(&b->alloc, s->buf, s->buf_bytes);
    DE_FREE(&b->alloc, s, sizeof *s);
}

const de__audio_backend_vtbl de__audio_backend_null = {
    "null",    DE_AUDIO_NULL, null_create, null_destroy, null_enumerate,
    null_open, null_start,    null_stop,   null_close,
};

/* ---- Manual pump ----------------------------------------------------------------------------- */

void de__audio_null_pump(de_audio *a, u32 frames, f32 *capture) {
    DE_ASSERT(a != NULL && a->manual);
    if (a == NULL || !a->manual)
        return;
    de__rt_guard_enter();
    while (frames > 0) {
        if (de_atomic_load_u64(&a->ring.write_pos, DE_ATOMIC_RELAXED) ==
            de_atomic_load_u64(&a->ring.read_pos, DE_ATOMIC_RELAXED))
            de__mixer_render_block(a);
        u32 n = a->block_frames - a->ring.read_offset;
        if (n > frames)
            n = frames;
        /* src_buf is the audio thread's scratch, and this thread is the audio thread here. */
        f32 *dst = capture != NULL ? capture : a->src_buf;
        de__audio_ring_read(a, dst, n);
        if (capture != NULL)
            capture += (size_t)n * a->channels;
        frames -= n;
    }
    de__rt_guard_leave();
}

/* ---- Simulated hot-plug ---------------------------------------------------------------------- */

static null_backend *null_of(de_audio *a) {
    if (a == NULL || a->dev.vt != &de__audio_backend_null)
        return NULL;
    return (null_backend *)a->dev.backend;
}

de_result de__audio_null_add_device(de_audio *a, const char *id, const char *name,
                                    bool make_default) {
    null_backend *b = null_of(a);
    if (b == NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "audio: not the null backend");
    if (id == NULL || id[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "audio: device id required");
    de_mutex_lock(&b->lock);
    de_result r = DE_OK;
    if (find_device(b, id) >= 0) {
        r = DE_FAIL(DE_ERR_ALREADY_EXISTS, "audio: null device '%s' exists", id);
    } else if (b->count == NULL_MAX_DEVICES) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "audio: at most %u null devices", NULL_MAX_DEVICES);
    } else {
        if (make_default)
            for (u32 i = 0; i < b->count; ++i)
                b->devices[i].is_default = false;
        const bool def = make_default || b->count == 0;
        set_device(&b->devices[b->count], id, name != NULL ? name : id, def);
        ++b->count;
    }
    de_mutex_unlock(&b->lock);
    if (r == DE_OK && b->notify != NULL)
        b->notify(b->notify_user, DE__AUDIO_EV_DEVICES_CHANGED |
                                      (make_default ? DE__AUDIO_EV_DEFAULT_CHANGED : 0u));
    return r;
}

de_result de__audio_null_remove_device(de_audio *a, const char *id) {
    null_backend *b = null_of(a);
    if (b == NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "audio: not the null backend");
    de_mutex_lock(&b->lock);
    i32 i = id != NULL ? find_device(b, id) : -1;
    bool was_default = false;
    if (i >= 0) {
        was_default = b->devices[i].is_default;
        memmove(&b->devices[i], &b->devices[i + 1],
                sizeof(b->devices[0]) * (b->count - (u32)i - 1));
        --b->count;
        if (was_default && b->count > 0)
            b->devices[0].is_default = true;
    }
    de_mutex_unlock(&b->lock);
    if (i < 0)
        return DE_FAIL(DE_ERR_NOT_FOUND, "audio: no null device '%s'", id != NULL ? id : "");
    if (b->notify != NULL)
        b->notify(b->notify_user,
                  DE__AUDIO_EV_DEVICES_CHANGED | (was_default ? DE__AUDIO_EV_DEFAULT_CHANGED : 0u));
    return DE_OK;
}

de_result de__audio_null_set_default(de_audio *a, const char *id) {
    null_backend *b = null_of(a);
    if (b == NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "audio: not the null backend");
    de_mutex_lock(&b->lock);
    i32 i = id != NULL ? find_device(b, id) : -1;
    if (i >= 0)
        for (u32 k = 0; k < b->count; ++k)
            b->devices[k].is_default = (k == (u32)i);
    de_mutex_unlock(&b->lock);
    if (i < 0)
        return DE_FAIL(DE_ERR_NOT_FOUND, "audio: no null device '%s'", id != NULL ? id : "");
    if (b->notify != NULL)
        b->notify(b->notify_user, DE__AUDIO_EV_DEFAULT_CHANGED);
    return DE_OK;
}

u64 de__audio_null_frames_played(const de_audio *a) {
    if (a == NULL || a->dev.vt != &de__audio_backend_null)
        return 0;
    return de_atomic_load_u64(&((const null_backend *)a->dev.backend)->frames_played,
                              DE_ATOMIC_RELAXED);
}
