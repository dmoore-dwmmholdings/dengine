#include "audio/source/stream_internal.h"
#include "audio/source/stream_ring.h"
#include "asset/asset_internal.h"
#include <de_stb_vorbis.h>
#include <string.h>
#define INPUT_BYTES (1024u * 1024u)
static void io_complete(de_io_request *request, de_result result, u64 bytes) {
    de__sound_decoder *d = request->user;
    d->io_result = result;
    d->io_bytes = bytes;
    de_sem_post(&d->io_complete, 1);
}
de_result de__decoder_io(de__sound_decoder *d, u64 offset, void *out, u32 bytes) {
    memset(&d->request, 0, sizeof d->request);
    d->request.file = d->file;
    d->request.offset = offset;
    d->request.bytes = bytes;
    d->request.dst = out;
    d->request.user = d;
    d->request.on_complete = io_complete;
    d->request.flags = DE_IO_CALLBACK_ON_IO_THREAD;
    u64 started = d->measure_io ? de_time_now_ns() : 0;
    DE_TRY(de_io_submit(&d->request));
    ++d->io_requests;
    /* Decoder work runs off the mixer thread. Park until the actual completion
     * instead of polling de_sleep_ns: sub-millisecond sleeps busy-spin. A counted
     * semaphore also retains a completion that arrives before this wait. */
    de_sem_wait(&d->io_complete);
    if (d->measure_io)
        d->io_ns += de_time_now_ns() - started;
    if (d->io_result != DE_OK || d->io_bytes != bytes)
        return DE_FAIL(DE_ERR_IO, "short compressed sound read");
    return DE_OK;
}
de_result de__decoder_create(const de_allocator *a, de__sound_asset *asset,
                             de__sound_decoder **out) {
    *out = NULL;
    de_result r = de_io_init(NULL);
    if (r != DE_OK && r != DE_ERR_ALREADY_EXISTS)
        return r;
    de__sound_decoder *d = DE_ALLOC(a, sizeof *d, DE_ALIGNOF(de__sound_decoder), DE_MEM_AUDIO);
    if (!d)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "stream decoder");
    memset(d, 0, sizeof *d);
    d->alloc = a;
    d->asset = asset;
    de__sound_asset_ref(asset);
    r = de_sem_init(&d->io_complete, 0);
    if (r != DE_OK) {
        de__decoder_destroy(d);
        return r;
    }
    d->io_complete_initialized = true;
    d->input = DE_ALLOC(a, INPUT_BYTES, 16, DE_MEM_AUDIO);
    if (!d->input) {
        de__decoder_destroy(d);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "stream compressed buffer");
    }
    r = de_file_open(asset->path, DE_FILE_READ, &d->file);
    if (r == DE_OK)
        r = asset->view.codec == DE__DSND_OPUS ? de__opus_init(d) : de__vorbis_init(d);
    if (r != DE_OK) {
        de__decoder_destroy(d);
        return r;
    }
    *out = d;
    return DE_OK;
}
void de__decoder_destroy(de__sound_decoder *d) {
    if (!d)
        return;
    if (d->codec && d->asset->view.codec == DE__DSND_VORBIS)
        stb_vorbis_close(d->codec);
    if (d->file)
        de_file_close(d->file);
    if (d->io_complete_initialized)
        de_sem_destroy(&d->io_complete);
    if (d->input)
        DE_FREE(d->alloc, d->input, INPUT_BYTES);
    if (d->scratch)
        DE_FREE(d->alloc, d->scratch, d->scratch_bytes);
    de__sound_asset_release(d->asset);
    DE_FREE(d->alloc, d, sizeof *d);
}
u32 de__decoder_read(de__sound_decoder *d, f32 *out, u32 frames) {
    return d->asset->view.codec == DE__DSND_OPUS ? de__opus_read(d, out, frames)
                                                 : de__vorbis_read(d, out, frames);
}
bool de__decoder_seek(de__sound_decoder *d, u64 frame) {
    return d->asset->view.codec == DE__DSND_OPUS ? de__opus_seek(d, frame)
                                                 : de__vorbis_seek(d, frame);
}
typedef struct stream_source stream_source;
#define STREAM_TRACE_CAPACITY 4096u
typedef struct stream_trace_slot {
    de__stream_trace_event event;
    de_atomic_u32 ready;
} stream_trace_slot;
typedef struct stream_trace {
    de_atomic_u32 count;
    stream_trace_slot slots[STREAM_TRACE_CAPACITY];
} stream_trace;
typedef struct stream_manager {
    de_audio *audio;
    de_mutex lock;
    de_cond idle;
    de_sem work;
    de_thread *workers[2];
    bool stopping; /* Protected by lock. */
    stream_source *head, *queued_head, *queued_tail;
    de_atomic_u32 starve;
    de_atomic_ptr trace;
    u32 next_source; /* Protected by lock. */
} stream_manager;
struct stream_source {
    de__audio_source base;
    de_audio *audio;
    stream_manager *manager;
    stream_source *next, *queued_next;
    bool active; /* Queued/running, protected by the manager lock. */
    de__sound_decoder *decoder;
    f32 *ring;
    u32 capacity;
    de_atomic_u64 read_pos, write_pos, seek_target, seek_read_pos;
    u32 read_seek_ack; /* Reader-owned generation of the last cursor flush. */
    de_atomic_u32 need, closing, eof, seek_request, seek_ack;
    void (*test_read_hook)(de__audio_source *, void *);
    void *test_read_user;
    u32 trace_id;
};
static stream_manager *manager(de_audio *a) {
    return de_atomic_load_ptr(&a->stream_manager, DE_ATOMIC_ACQUIRE);
}
static void trace_event(stream_source *s, stream_trace *t, u32 kind, u64 time_ns, bool decoder) {
    if (!t || de_atomic_load_u32(&t->count, DE_ATOMIC_RELAXED) >= STREAM_TRACE_CAPACITY)
        return;
    u32 i = de_atomic_fetch_add_u32(&t->count, 1, DE_ATOMIC_RELAXED);
    if (i >= STREAM_TRACE_CAPACITY)
        return;
    stream_trace_slot *slot = &t->slots[i];
    slot->event =
        (de__stream_trace_event){.time_ns = time_ns,
                                 .source = s->trace_id,
                                 .kind = kind,
                                 .read_pos = de_atomic_load_u64(&s->read_pos, DE_ATOMIC_ACQUIRE),
                                 .write_pos = de_atomic_load_u64(&s->write_pos, DE_ATOMIC_ACQUIRE),
                                 .decoder_frame = decoder ? s->decoder->frame : 0,
                                 .io_ns = decoder ? s->decoder->io_ns : 0,
                                 .io_requests = decoder ? s->decoder->io_requests : 0};
    de_atomic_store_u32(&slot->ready, 1, DE_ATOMIC_RELEASE);
}
static void refill(void *user);
static i32 stream_worker(void *user) {
    stream_manager *m = user;
    de_fp_env_init_thread();
    for (;;) {
        de_sem_wait(&m->work);
        /* A low-watermark wake goes straight here. Device enumeration/recovery
         * on the control thread must not stand between the mixer and a refill. */
        de__stream_service(m->audio);
        de_mutex_lock(&m->lock);
        stream_source *s = m->queued_head;
        if (s) {
            m->queued_head = s->queued_next;
            if (!m->queued_head)
                m->queued_tail = NULL;
            s->queued_next = NULL;
        }
        bool stop = m->stopping;
        de_mutex_unlock(&m->lock);
        if (!s) {
            if (stop)
                return 0;
            continue;
        }
        stream_trace *trace = de_atomic_load_ptr(&m->trace, DE_ATOMIC_ACQUIRE);
        if (trace) {
            s->decoder->measure_io = true;
            trace_event(s, trace, DE__STREAM_START, de_time_now_ns(), true);
        }
        refill(s);
        if (trace)
            trace_event(s, trace, DE__STREAM_FINISH, de_time_now_ns(), true);
        de_mutex_lock(&m->lock);
        s->active = false;
        /* A second worker may have consumed a wake while this source was
         * active. Hand that coalesced request back after publishing idle. */
        bool again = de_atomic_load_u32(&s->need, DE_ATOMIC_ACQUIRE) != 0 &&
                     !de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE);
        de_cond_broadcast(&m->idle);
        de_mutex_unlock(&m->lock);
        if (again)
            de_sem_post(&m->work, 1);
    }
}
static void manager_destroy(stream_manager *m) {
    de_mutex_lock(&m->lock);
    m->stopping = true;
    de_mutex_unlock(&m->lock);
    de_sem_post(&m->work, 2);
    for (u32 i = 0; i < 2; ++i)
        if (m->workers[i])
            de_thread_join(m->workers[i]);
    de_sem_destroy(&m->work);
    de_cond_destroy(&m->idle);
    de_mutex_destroy(&m->lock);
    stream_trace *trace = de_atomic_load_ptr(&m->trace, DE_ATOMIC_RELAXED);
    if (trace)
        DE_FREE(&m->audio->alloc, trace, sizeof *trace);
    DE_FREE(&m->audio->alloc, m, sizeof *m);
}
static de_result ensure_manager(de_audio *a, stream_manager **out) {
    *out = manager(a);
    if (*out)
        return DE_OK;
    stream_manager *m = DE_ALLOC(&a->alloc, sizeof *m, DE_ALIGNOF(stream_manager), DE_MEM_AUDIO);
    if (!m)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "stream manager");
    memset(m, 0, sizeof *m);
    m->audio = a;
    de_mutex_init(&m->lock);
    de_cond_init(&m->idle);
    de_atomic_init_u32(&m->starve, 0);
    de_atomic_init_ptr(&m->trace, NULL);
    de_result r = de_sem_init(&m->work, 0);
    if (r != DE_OK) {
        de_cond_destroy(&m->idle);
        de_mutex_destroy(&m->lock);
        DE_FREE(&a->alloc, m, sizeof *m);
        return r;
    }
    /* Refill deadlines must not depend on unrelated long jobs or the general
     * background pool's below-normal priority/boost budget. */
    for (u32 i = 0; i < 2; ++i) {
        r = de_thread_create(stream_worker, m, "de-audio-stream", DE_THREAD_NORMAL, 0,
                             &m->workers[i]);
        if (r != DE_OK) {
            manager_destroy(m);
            return r;
        }
    }
    void *expected = NULL;
    if (!de_atomic_cas_ptr(&a->stream_manager, &expected, m, DE_ATOMIC_ACQ_REL,
                           DE_ATOMIC_ACQUIRE)) {
        manager_destroy(m);
        m = expected;
    }
    *out = m;
    return DE_OK;
}
static void refill(void *user) {
    stream_source *s = user;
    u32 request = de_atomic_load_u32(&s->seek_request, DE_ATOMIC_ACQUIRE);
    if (de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE) ||
        de_atomic_load_u32(&s->manager->starve, DE_ATOMIC_ACQUIRE))
        return;
    if (request != de_atomic_load_u32(&s->seek_ack, DE_ATOMIC_ACQUIRE)) {
        u64 frame = de_atomic_load_u64(&s->seek_target, DE_ATOMIC_ACQUIRE);
        if (!de__decoder_seek(s->decoder, frame)) {
            de_atomic_store_u32(&s->eof, 1, DE_ATOMIC_RELEASE);
            return;
        }
        u64 write = de_atomic_load_u64(&s->write_pos, DE_ATOMIC_ACQUIRE);
        /* Publish a boundary; only the reader may advance its SPSC read cursor. */
        de_atomic_store_u64(&s->seek_read_pos, write, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&s->eof, 0, DE_ATOMIC_RELEASE);
        de_atomic_store_u32(&s->seek_ack, request, DE_ATOMIC_RELEASE);
    }
    f32 pcm[1024 * 8];
    for (u32 pass = 0; pass < 1024 && !de_atomic_load_u32(&s->closing, DE_ATOMIC_ACQUIRE); ++pass) {
        if (request != de_atomic_load_u32(&s->seek_request, DE_ATOMIC_ACQUIRE))
            break;
        u64 read = de_atomic_load_u64(&s->read_pos, DE_ATOMIC_ACQUIRE),
            write = de_atomic_load_u64(&s->write_pos, DE_ATOMIC_RELAXED);
        u32 room = s->capacity - (u32)(write - read);
        if (!room)
            break;
        u32 n = room < 1024 ? room : 1024;
        const de__sound_view *v = &s->decoder->asset->view;
        bool loop = de_atomic_load_u32(&s->base.loop_enabled, DE_ATOMIC_ACQUIRE) != 0;
        u64 end = loop && (v->flags & DE__DSND_LOOP) ? v->loop_end : v->frames;
        if (s->decoder->frame >= end) {
            if (!loop) {
                de_atomic_store_u32(&s->eof, 1, DE_ATOMIC_RELEASE);
                break;
            }
            if (!de__decoder_seek(s->decoder, (v->flags & DE__DSND_LOOP) ? v->loop_start : 0)) {
                de_atomic_store_u32(&s->eof, 1, DE_ATOMIC_RELEASE);
                break;
            }
        }
        if (n > end - s->decoder->frame)
            n = (u32)(end - s->decoder->frame);
        u32 got = de__decoder_read(s->decoder, pcm, n);
        if (!got) {
            de_atomic_store_u32(&s->eof, 1, DE_ATOMIC_RELEASE);
            break;
        }
        if (request != de_atomic_load_u32(&s->seek_request, DE_ATOMIC_ACQUIRE))
            break;
        de__stream_ring_write(s->ring, s->capacity, v->channels, write, pcm, got);
        de_atomic_store_u64(&s->write_pos, write + got, DE_ATOMIC_RELEASE);
    }
}
void de__stream_service(de_audio *a) {
    stream_manager *m = manager(a);
    if (!m)
        return;
    de_mutex_lock(&m->lock);
    for (stream_source *s = m->head; s; s = s->next) {
        if (!de_atomic_load_u32(&s->need, DE_ATOMIC_ACQUIRE))
            continue;
        if (!s->active) {
            s->active = true;
            de_atomic_store_u32(&s->need, 0, DE_ATOMIC_RELEASE);
            if (m->queued_tail)
                m->queued_tail->queued_next = s;
            else
                m->queued_head = s;
            m->queued_tail = s;
            stream_trace *trace = de_atomic_load_ptr(&m->trace, DE_ATOMIC_ACQUIRE);
            if (trace)
                trace_event(s, trace, DE__STREAM_QUEUED, de_time_now_ns(), false);
            de_sem_post(&m->work, 1);
        }
    }
    de_mutex_unlock(&m->lock);
}
static void request_refill(stream_source *s) {
    stream_trace *trace = de_atomic_load_ptr(&s->manager->trace, DE_ATOMIC_ACQUIRE);
    u64 now = trace ? de_time_now_ns() : 0;
    if (de_atomic_exchange_u32(&s->need, 1, DE_ATOMIC_ACQ_REL) == 0) {
        trace_event(s, trace, DE__STREAM_DEMAND, now, false);
        de_sem_post(&s->manager->work, 1);
    }
}
static u32 stream_read(de__audio_source *base, f32 *out, u32 frames, u32 channels) {
    stream_source *s = (stream_source *)base;
    u64 read = de_atomic_load_u64(&s->read_pos, DE_ATOMIC_RELAXED),
        write = de_atomic_load_u64(&s->write_pos, DE_ATOMIC_ACQUIRE);
    if (s->test_read_hook)
        s->test_read_hook(base, s->test_read_user);
    u32 request = de_atomic_load_u32(&s->seek_request, DE_ATOMIC_ACQUIRE);
    u32 ack = de_atomic_load_u32(&s->seek_ack, DE_ATOMIC_ACQUIRE);
    bool seeking = request != ack;
    if (!seeking && s->read_seek_ack != ack) {
        /* Ack can arrive after the initial snapshots: discard them before reading.
         * Its acquire also publishes the worker's seek boundary. */
        read = de_atomic_load_u64(&s->seek_read_pos, DE_ATOMIC_RELAXED);
        de_atomic_store_u64(&s->read_pos, read, DE_ATOMIC_RELEASE);
        s->read_seek_ack = ack;
        write = de_atomic_load_u64(&s->write_pos, DE_ATOMIC_ACQUIRE);
    }
    u32 available = seeking ? 0 : (u32)(write - read);
    u32 n = frames < available ? frames : available;
    de__stream_ring_read(out, s->ring, s->capacity, channels, read, n);
    if (n)
        de_atomic_store_u64(&s->read_pos, read + n, DE_ATOMIC_RELEASE);
    if (available - n < s->capacity / 2)
        request_refill(s);
    if (n < frames) {
        bool loop = de_atomic_load_u32(&base->loop_enabled, DE_ATOMIC_ACQUIRE) != 0;
        if (!seeking && !loop && de_atomic_load_u32(&s->eof, DE_ATOMIC_ACQUIRE))
            return n;
        memset(out + (size_t)n * channels, 0, (size_t)(frames - n) * channels * 4);
        de_atomic_fetch_add_u32(&s->audio->underruns, 1, DE_ATOMIC_RELAXED);
        stream_trace *trace = de_atomic_load_ptr(&s->manager->trace, DE_ATOMIC_ACQUIRE);
        if (trace)
            trace_event(s, trace, DE__STREAM_UNDERRUN, de_time_now_ns(), false);
        return frames;
    }
    return n;
}
static bool stream_seek(de__audio_source *base, u64 frame) {
    stream_source *s = (stream_source *)base;
    if (frame > s->decoder->asset->view.frames)
        return false;
    de_atomic_store_u64(&s->seek_target, frame, DE_ATOMIC_RELEASE);
    de_atomic_fetch_add_u32(&s->seek_request, 1, DE_ATOMIC_ACQ_REL);
    request_refill(s);
    return true;
}
static void stream_close(de__audio_source *base) {
    stream_source *s = (stream_source *)base;
    stream_manager *m = s->manager;
    de_mutex_lock(&m->lock);
    stream_source **link = &m->head;
    while (*link && *link != s)
        link = &(*link)->next;
    if (*link)
        *link = s->next;
    de_atomic_store_u32(&s->closing, 1, DE_ATOMIC_RELEASE);
    while (s->active)
        de_cond_wait(&m->idle, &m->lock);
    de_mutex_unlock(&m->lock);
    de__decoder_destroy(s->decoder);
    DE_FREE(&s->audio->alloc, s->ring, (size_t)s->capacity * base->channels * 4);
    DE_FREE(&s->audio->alloc, s, sizeof *s);
}
static const de__audio_source_vtbl stream_vt = {stream_read, stream_seek, stream_close};
de_result de__source_stream(de_audio *a, de__sound_asset *asset, de__audio_source **out) {
    *out = NULL;
    stream_manager *m = NULL;
    DE_TRY(ensure_manager(a, &m));
    stream_source *s = DE_ALLOC(&a->alloc, sizeof *s, DE_ALIGNOF(stream_source), DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "stream source");
    memset(s, 0, sizeof *s);
    de_atomic_init_u64(&s->read_pos, 0);
    de_atomic_init_u64(&s->write_pos, 0);
    de_atomic_init_u64(&s->seek_target, 0);
    de_atomic_init_u64(&s->seek_read_pos, 0);
    de_atomic_init_u32(&s->need, 0);
    de_atomic_init_u32(&s->closing, 0);
    de_atomic_init_u32(&s->eof, 0);
    de_atomic_init_u32(&s->seek_request, 0);
    de_atomic_init_u32(&s->seek_ack, 0);
    de_atomic_init_u32(&s->base.loop_enabled, 0);
    s->audio = a;
    s->manager = m;
    s->capacity = asset->view.rate / 2;
    s->base.vt = &stream_vt;
    s->base.channels = asset->view.channels;
    s->base.rate = asset->view.rate;
    s->base.length_frames = asset->view.frames;
    s->base.flags = DE__SOURCE_STREAMING | DE__SOURCE_SEEKABLE | DE__SOURCE_LOOP_AWARE;
    s->ring = DE_ALLOC(&a->alloc, (size_t)s->capacity * s->base.channels * 4, 16, DE_MEM_AUDIO);
    de_result r = s->ring ? de__decoder_create(&a->alloc, asset, &s->decoder)
                          : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PCM stream ring");
    if (r != DE_OK) {
        if (s->ring)
            DE_FREE(&a->alloc, s->ring, (size_t)s->capacity * s->base.channels * 4);
        DE_FREE(&a->alloc, s, sizeof *s);
        return r;
    }
    /* Initial prefill is already a blocking control-side operation. */
    refill(s);
    de_mutex_lock(&m->lock);
    s->trace_id = ++m->next_source;
    s->next = m->head;
    m->head = s;
    de_mutex_unlock(&m->lock);
    *out = &s->base;
    return DE_OK;
}
void de__stream_shutdown(de_audio *a) {
    stream_manager *m = de_atomic_exchange_ptr(&a->stream_manager, NULL, DE_ATOMIC_ACQ_REL);
    if (!m)
        return;
    manager_destroy(m);
}
void de__stream_test_starve(de_audio *a, bool starve) {
    stream_manager *m = manager(a);
    if (m)
        de_atomic_store_u32(&m->starve, starve, DE_ATOMIC_RELEASE);
}

void de__stream_test_read_hook(de__audio_source *base, void (*hook)(de__audio_source *, void *),
                               void *user) {
    stream_source *s = (stream_source *)base;
    s->test_read_hook = hook;
    s->test_read_user = user;
}
void de__stream_test_refill_now(de__audio_source *base) {
    stream_source *s = (stream_source *)base;
    stream_manager *m = s->manager;
    de_mutex_lock(&m->lock);
    while (s->active)
        de_cond_wait(&m->idle, &m->lock);
    u32 starved = de_atomic_exchange_u32(&m->starve, 0, DE_ATOMIC_ACQ_REL);
    refill(s);
    de_atomic_store_u32(&m->starve, starved, DE_ATOMIC_RELEASE);
    de_mutex_unlock(&m->lock);
}
bool de__stream_test_trace_enable(de_audio *a) {
    stream_manager *m = manager(a);
    if (!m)
        return false;
    if (de_atomic_load_ptr(&m->trace, DE_ATOMIC_ACQUIRE))
        return true;
    stream_trace *trace =
        DE_ALLOC(&a->alloc, sizeof *trace, DE_ALIGNOF(stream_trace), DE_MEM_AUDIO);
    if (!trace)
        return false;
    memset(trace, 0, sizeof *trace);
    de_atomic_init_u32(&trace->count, 0);
    for (u32 i = 0; i < STREAM_TRACE_CAPACITY; ++i)
        de_atomic_init_u32(&trace->slots[i].ready, 0);
    de_atomic_store_ptr(&m->trace, trace, DE_ATOMIC_RELEASE);
    return true;
}
u32 de__stream_test_trace_count(de_audio *a, bool *full) {
    stream_manager *m = manager(a);
    stream_trace *trace = m ? de_atomic_load_ptr(&m->trace, DE_ATOMIC_ACQUIRE) : NULL;
    u32 count = trace ? de_atomic_load_u32(&trace->count, DE_ATOMIC_ACQUIRE) : 0;
    if (full)
        *full = count >= STREAM_TRACE_CAPACITY;
    return count < STREAM_TRACE_CAPACITY ? count : STREAM_TRACE_CAPACITY;
}
bool de__stream_test_trace_read(de_audio *a, u32 index, de__stream_trace_event *out) {
    stream_manager *m = manager(a);
    stream_trace *trace = m ? de_atomic_load_ptr(&m->trace, DE_ATOMIC_ACQUIRE) : NULL;
    if (!trace || !out || index >= STREAM_TRACE_CAPACITY ||
        !de_atomic_load_u32(&trace->slots[index].ready, DE_ATOMIC_ACQUIRE))
        return false;
    *out = trace->slots[index].event;
    return true;
}
