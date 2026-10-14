/* null_backend.c — DE_GPU_NULL: a backend without a device (DENG-281).
 *
 * For headless code paths and tests. UPLOAD and READBACK buffers are CPU memory (map works);
 * GPU_ONLY buffers and textures are metadata only; command recording is a no-op (every cmd_*
 * entry is NULL, so the front-end validates and records nothing); a submit completes at once,
 * signalling its fence and its queue timeline. Caps describe a Base-tier capable device named
 * "null" with mesh shaders and without ray tracing.
 *
 * Device memory (DENG-282): heap_create only records the size (no memory behind a null heap);
 * placed buffers and textures record their heap and offset (placed UPLOAD/READBACK buffers still
 * get CPU memory, so map works). Requirements: buffers round to 256 bytes, textures sum their
 * mips at the D3D12 64 KiB placement alignment.
 *
 * Test hook: with manual completion (de__gpu_null_set_manual_completion), the queue timelines
 * complete only as far as de__gpu_null_complete says (or fully on wait_idle), so deferred
 * destruction can be observed step by step. User fences still signal at submit.
 *
 * Command log (DENG-172, gpu_backend.h): with de__gpu_null_log_enable, barrier calls and submits
 * are recorded as entries (one per command, in order) for golden tests; DENG-173 adds more ops.
 * The log is off by default and then costs one atomic load per barrier call and submit.
 */
#include "gpu/gpu_backend.h"

#include <string.h>

typedef struct null_device {
    de_atomic_u64 submitted[DE_QUEUE_COUNT_]; /* last internal value submitted */
    de_atomic_u64 released[DE_QUEUE_COUNT_];  /* manual mode: completed up to here */
    de_atomic_u32 manual;
    de_atomic_u32 heaps_live, heaps_created;
    de_atomic_u64 heap_bytes;
    /* DENG-172: the command log (gpu_backend.h). Arrays on the device allocator, under log_lock. */
    de_atomic_u32 log_on;
    de_mutex log_lock;
    de__gpu_null_entry *log;
    u32 log_count, log_cap;
    de_gpu_barrier *log_barriers;
    u32 log_barrier_count, log_barrier_cap;
    de_gpu_wait *log_waits;
    u32 log_wait_count, log_wait_cap;
} null_device;

typedef struct null_buffer {
    void *mem;          /* UPLOAD/READBACK only */
    de__gpu_heap *heap; /* placed: where (DENG-282) */
    u64 offset;
} null_buffer;

typedef struct null_texture {
    de__gpu_heap *heap; /* placed: where (DENG-282) */
    u64 offset;
} null_texture;

typedef struct null_heap {
    u64 bytes;
} null_heap;

typedef struct null_fence {
    de_atomic_u64 value;
} null_fence;

static null_device *nd(de_gpu_device *d) {
    return (null_device *)de__gpu_device_payload(d);
}

static de_result null_device_create(de_gpu_device *d, const de_gpu_device_desc *desc,
                                    de_gpu_caps *caps) {
    (void)desc;
    null_device *n = nd(d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de_atomic_init_u64(&n->submitted[q], 0);
        de_atomic_init_u64(&n->released[q], 0);
    }
    de_atomic_init_u32(&n->manual, 0);
    de_atomic_init_u32(&n->heaps_live, 0);
    de_atomic_init_u32(&n->heaps_created, 0);
    de_atomic_init_u64(&n->heap_bytes, 0);
    de_atomic_init_u32(&n->log_on, 0);
    de_mutex_init(&n->log_lock);
    n->log = NULL;
    n->log_barriers = NULL;
    n->log_waits = NULL;
    n->log_count = n->log_cap = n->log_barrier_count = n->log_barrier_cap = 0;
    n->log_wait_count = n->log_wait_cap = 0;

    memset(caps, 0, sizeof *caps);
    caps->backend = DE_GPU_NULL;
    memcpy(caps->device_name, "null", 5);
    caps->raytracing = DE_RT_NONE;
    caps->mesh_shaders = true;
    caps->async_compute = true;
    caps->wave_size_min = caps->wave_size_max = 32;
    caps->max_bindless_textures = 1000000;
    caps->max_bindless_buffers = 1000000;
    caps->max_bindless_storage_textures = 1000000;
    caps->max_bindless_samplers = 2048;
    caps->max_bindless_accels = 0;
    caps->max_push_constant_bytes = DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    caps->timestamp_frequency = 1000000000u;
    caps->mesh_max_output_vertices = 256;
    caps->mesh_max_output_primitives = 256;
    caps->base_tier_ok = true;
    return DE_OK;
}

static void null_log_free(de_gpu_device *d);

static void null_device_destroy(de_gpu_device *d) {
    null_log_free(d);
    de_mutex_destroy(&nd(d)->log_lock);
}

static void null_wait_idle(de_gpu_device *d) {
    null_device *n = nd(d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        de_atomic_store_u64(&n->released[q],
                            de_atomic_load_u64(&n->submitted[q], DE_ATOMIC_ACQUIRE),
                            DE_ATOMIC_RELEASE);
}

static de_result null_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                                    const de_gpu_buffer_desc *desc) {
    (void)desc;
    if (b->memory == DE_MEM_GPU_ONLY)
        return DE_OK;
    null_buffer *nb = DE__GPU_PAYLOAD(null_buffer, b);
    nb->mem = DE_ALLOC(d->alloc, (size_t)b->size, 16, DE_MEM_RENDER);
    if (!nb->mem)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "null backend: %llu bytes for buffer \"%s\"",
                            (unsigned long long)b->size, b->name);
    memset(nb->mem, 0, (size_t)b->size);
    b->mapped = nb->mem;
    return DE_OK;
}

static void null_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b) {
    null_buffer *nb = DE__GPU_PAYLOAD(null_buffer, b);
    if (nb->mem)
        DE_FREE(d->alloc, nb->mem, (size_t)b->size);
    nb->mem = NULL;
    b->mapped = NULL;
}

static de_result null_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                     const de_gpu_texture_desc *desc) {
    (void)d;
    (void)t;
    (void)desc;
    return DE_OK;
}

static de_result null_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                     const de_gpu_sampler_desc *desc) {
    (void)d;
    (void)s;
    (void)desc;
    return DE_OK;
}

static de_result null_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                      const de_gpu_pipeline_desc *desc) {
    (void)d;
    (void)p;
    (void)desc;
    return DE_OK;
}

static de_result null_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                        const de_gpu_query_heap_desc *desc) {
    (void)d;
    (void)h;
    (void)desc;
    return DE_OK;
}

static de_result null_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial) {
    (void)d;
    de_atomic_init_u64(&DE__GPU_PAYLOAD(null_fence, f)->value, initial);
    return DE_OK;
}

static u64 null_fence_value(de_gpu_device *d, de__gpu_fence *f) {
    (void)d;
    return de_atomic_load_u64(&DE__GPU_PAYLOAD(null_fence, f)->value, DE_ATOMIC_ACQUIRE);
}

static de_result null_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns) {
    /* Only a submit on another thread can advance a null fence; poll until the deadline. */
    const u64 deadline = de_time_now_ns() + timeout_ns;
    for (;;) {
        if (null_fence_value(d, f) >= value)
            return DE_OK;
        if (timeout_ns == 0 || de_time_now_ns() >= deadline)
            return DE_FAIL(DE_ERR_TIMEOUT, "de_gpu_fence_wait: null fence is at %llu, not %llu",
                           (unsigned long long)null_fence_value(d, f), (unsigned long long)value);
        de_sleep_ns(100000); /* 0.1 ms */
    }
}

static void fence_signal(null_fence *nf, u64 value) {
    u64 cur = de_atomic_load_u64(&nf->value, DE_ATOMIC_RELAXED);
    while (cur < value &&
           !de_atomic_cas_weak_u64(&nf->value, &cur, value, DE_ATOMIC_RELEASE, DE_ATOMIC_RELAXED)) {
    }
}

static void null_log_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s);

static de_result null_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                   u64 queue_value) {
    null_device *n = nd(d);
    if (de_atomic_load_u32(&n->log_on, DE_ATOMIC_ACQUIRE))
        null_log_submit(d, q, s);
    if (s->signal.id) {
        de__gpu_fence *f = de__gpu_get_fence(d, s->signal);
        if (f)
            fence_signal(DE__GPU_PAYLOAD(null_fence, f), s->signal_value);
    }
    de_atomic_store_u64(&n->submitted[q], queue_value, DE_ATOMIC_RELEASE);
    if (!de_atomic_load_u32(&n->manual, DE_ATOMIC_ACQUIRE))
        de_atomic_store_u64(&n->released[q], queue_value, DE_ATOMIC_RELEASE);
    return DE_OK;
}

static u64 null_queue_completed(de_gpu_device *d, de_gpu_queue q) {
    null_device *n = nd(d);
    u64 sub = de_atomic_load_u64(&n->submitted[q], DE_ATOMIC_ACQUIRE);
    u64 rel = de_atomic_load_u64(&n->released[q], DE_ATOMIC_ACQUIRE);
    return rel < sub ? rel : sub;
}

/* ---------- Device memory (DENG-282) ---------- */

static de_result null_heap_create(de_gpu_device *d, de__gpu_heap *h,
                                  const de__gpu_mem_class_desc *cls) {
    (void)cls;
    null_device *n = nd(d);
    DE__GPU_PAYLOAD(null_heap, h)->bytes = h->size;
    de_atomic_fetch_add_u32(&n->heaps_live, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&n->heaps_created, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&n->heap_bytes, h->size, DE_ATOMIC_RELAXED);
    return DE_OK;
}

static void null_heap_destroy(de_gpu_device *d, de__gpu_heap *h) {
    null_device *n = nd(d);
    de_atomic_fetch_sub_u32(&n->heaps_live, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_sub_u64(&n->heap_bytes, DE__GPU_PAYLOAD(null_heap, h)->bytes,
                            DE_ATOMIC_RELAXED);
}

static u64 null_align_up(u64 v, u64 a) {
    return (v + (a - 1)) & ~(a - 1);
}

static de_result null_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                          de__gpu_mem_req *out) {
    (void)d;
    out->size = null_align_up(desc->size, 256);
    out->align = 256;
    out->class_mask = 1u << (u32)desc->memory; /* base class ids equal de_gpu_memory values */
    return DE_OK;
}

static de_result null_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                           de__gpu_mem_req *out) {
    (void)d;
    const bool bc = de__gpu_format_is_compressed(desc->format);
    const u64 block = de__gpu_format_block_bytes(desc->format);
    u64 bytes = 0;
    for (u32 m = 0; m < desc->mip_count; ++m) {
        u64 w = desc->width >> m, h = desc->height >> m;
        w = w ? w : 1;
        h = h ? h : 1;
        if (bc) {
            w = (w + 3) / 4;
            h = (h + 3) / 4;
        }
        u64 z = desc->depth_or_layers;
        if (desc->dim == DE_TEX_3D) {
            z >>= m;
            z = z ? z : 1;
        }
        bytes += w * h * z * block * desc->sample_count;
    }
    out->align = desc->sample_count > 1 ? (u64)4 << 20 : (u64)64 << 10;
    out->size = null_align_up(bytes ? bytes : 1, out->align);
    out->class_mask = 1u << DE__GPU_MEM_CLASS_GPU_ONLY;
    return DE_OK;
}

static de_result null_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                           const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                           u64 offset) {
    de_result r = null_buffer_create(d, b, desc);
    if (r != DE_OK)
        return r;
    null_buffer *nb = DE__GPU_PAYLOAD(null_buffer, b);
    nb->heap = heap;
    nb->offset = offset;
    return DE_OK;
}

static de_result null_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                            const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                            u64 offset) {
    (void)d;
    (void)desc;
    null_texture *nt = DE__GPU_PAYLOAD(null_texture, t);
    nt->heap = heap;
    nt->offset = offset;
    return DE_OK;
}

static void null_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);

const de__gpu_backend_vtable de__gpu_null_vtable = {
    .name = "null",
    .device_payload_size = sizeof(null_device),
    .cmd_payload_size = 0,
    .payload_size =
        {
            [DE__GPU_BUFFER] = sizeof(null_buffer),
            [DE__GPU_TEXTURE] = sizeof(null_texture),
            [DE__GPU_FENCE] = sizeof(null_fence),
        },
    .device_create = null_device_create,
    .device_destroy = null_device_destroy,
    .wait_idle = null_wait_idle,
    .buffer_create = null_buffer_create,
    .buffer_destroy = null_buffer_destroy,
    .texture_create = null_texture_create,
    .sampler_create = null_sampler_create,
    .pipeline_create = null_pipeline_create,
    .queue_submit = null_queue_submit,
    .queue_completed = null_queue_completed,
    .fence_create = null_fence_create,
    .fence_value = null_fence_value,
    .fence_wait = null_fence_wait,
    .query_heap_create = null_query_heap_create,
    .heap_payload_size = sizeof(null_heap),
    .heap_create = null_heap_create,
    .heap_destroy = null_heap_destroy,
    .buffer_requirements = null_buffer_requirements,
    .texture_requirements = null_texture_requirements,
    .buffer_create_placed = null_buffer_create_placed,
    .texture_create_placed = null_texture_create_placed,
    .cmd_barriers = null_cmd_barriers, /* DENG-172: only feeds the command log */
};

void de__gpu_null_set_manual_completion(de_gpu_device *d, bool manual) {
    if (!d || d->vt != &de__gpu_null_vtable)
        return;
    null_device *n = nd(d);
    if (!manual)
        null_wait_idle(d);
    de_atomic_store_u32(&n->manual, manual ? 1u : 0u, DE_ATOMIC_RELEASE);
}

void de__gpu_null_complete(de_gpu_device *d, de_gpu_queue q, u64 value) {
    if (!d || d->vt != &de__gpu_null_vtable || (u32)q >= DE_QUEUE_COUNT_)
        return;
    de_atomic_store_u64(&nd(d)->released[q], value, DE_ATOMIC_RELEASE);
}

void de__gpu_null_heap_counts(de_gpu_device *d, u32 *live, u64 *bytes, u32 *created) {
    const bool ok = d && d->vt == &de__gpu_null_vtable;
    null_device *n = ok ? nd(d) : NULL;
    if (live)
        *live = n ? de_atomic_load_u32(&n->heaps_live, DE_ATOMIC_RELAXED) : 0;
    if (bytes)
        *bytes = n ? de_atomic_load_u64(&n->heap_bytes, DE_ATOMIC_RELAXED) : 0;
    if (created)
        *created = n ? de_atomic_load_u32(&n->heaps_created, DE_ATOMIC_RELAXED) : 0;
}

bool de__gpu_null_placement(de_gpu_device *d, de__gpu_kind kind, u32 id, de__gpu_heap **heap,
                            u64 *offset) {
    if (!d || d->vt != &de__gpu_null_vtable)
        return false;
    de__gpu_heap *h = NULL;
    u64 off = 0;
    if (kind == DE__GPU_BUFFER) {
        de__gpu_buffer *b = de__gpu_get_buffer(d, (de_gpu_buffer){id});
        if (!b || !b->region)
            return false;
        h = DE__GPU_PAYLOAD(null_buffer, b)->heap;
        off = DE__GPU_PAYLOAD(null_buffer, b)->offset;
    } else if (kind == DE__GPU_TEXTURE) {
        de__gpu_texture *t = de__gpu_get_texture(d, (de_gpu_texture){id});
        if (!t || !t->region)
            return false;
        h = DE__GPU_PAYLOAD(null_texture, t)->heap;
        off = DE__GPU_PAYLOAD(null_texture, t)->offset;
    } else {
        return false;
    }
    if (heap)
        *heap = h;
    if (offset)
        *offset = off;
    return true;
}

/* ---------- Command log (DENG-172) ---------- */

/* Grows a log array to hold `need` elements; false on out of memory (the entry is dropped). */
static bool log_grow(de_gpu_device *d, void **arr, u32 count, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return true;
    u32 nc = *cap ? *cap * 2u : 64u;
    while (nc < need)
        nc *= 2u;
    void *p = DE_ALLOC(d->alloc, (size_t)nc * elem, 16, DE_MEM_RENDER);
    if (!p)
        return false;
    if (count)
        memcpy(p, *arr, (size_t)count * elem);
    if (*arr)
        DE_FREE(d->alloc, *arr, (size_t)*cap * elem);
    *arr = p;
    *cap = nc;
    return true;
}

/* Appends an entry with `count` data elements (copied from `data`); under log_lock. */
static void log_append(de_gpu_device *d, const de__gpu_null_entry *e, const void *data, u32 count) {
    null_device *n = nd(d);
    de_mutex_lock(&n->log_lock);
    bool ok =
        log_grow(d, (void **)&n->log, n->log_count, &n->log_cap, n->log_count + 1u, sizeof *n->log);
    de__gpu_null_entry x = *e;
    if (ok && e->op == DE__GPU_NULL_OP_BARRIERS) {
        ok = log_grow(d, (void **)&n->log_barriers, n->log_barrier_count, &n->log_barrier_cap,
                      n->log_barrier_count + count, sizeof *n->log_barriers);
        if (ok && count)
            memcpy(&n->log_barriers[n->log_barrier_count], data, sizeof *n->log_barriers * count);
        x.data_first = n->log_barrier_count;
        if (ok)
            n->log_barrier_count += count;
    } else if (ok && e->op == DE__GPU_NULL_OP_SUBMIT) {
        ok = log_grow(d, (void **)&n->log_waits, n->log_wait_count, &n->log_wait_cap,
                      n->log_wait_count + count, sizeof *n->log_waits);
        if (ok && count)
            memcpy(&n->log_waits[n->log_wait_count], data, sizeof *n->log_waits * count);
        x.data_first = n->log_wait_count;
        if (ok)
            n->log_wait_count += count;
    }
    x.data_count = count;
    if (ok)
        n->log[n->log_count++] = x;
    de_mutex_unlock(&n->log_lock);
}

static void copy_list_name(char *dst, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1u < DE__GPU_NAME_MAX; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

static void null_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count) {
    de_gpu_device *d = c->device;
    if (!de_atomic_load_u32(&nd(d)->log_on, DE_ATOMIC_ACQUIRE))
        return;
    de__gpu_null_entry e;
    memset(&e, 0, sizeof e);
    e.op = DE__GPU_NULL_OP_BARRIERS;
    e.queue = c->queue;
    copy_list_name(e.list, c->name);
    log_append(d, &e, b, count);
}

static void null_log_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s) {
    de__gpu_null_entry e;
    memset(&e, 0, sizeof e);
    e.op = DE__GPU_NULL_OP_SUBMIT;
    e.queue = q;
    copy_list_name(e.list, s->cmd_count ? s->cmds[0]->name : "");
    e.a[0] = s->cmd_count;
    e.a[1] = s->wait_count;
    e.a[2] = s->signal.id;
    e.a[3] = s->signal_value;
    log_append(d, &e, s->waits, s->wait_count);
}

static void null_log_free(de_gpu_device *d) {
    null_device *n = nd(d);
    if (n->log)
        DE_FREE(d->alloc, n->log, sizeof *n->log * n->log_cap);
    if (n->log_barriers)
        DE_FREE(d->alloc, n->log_barriers, sizeof *n->log_barriers * n->log_barrier_cap);
    if (n->log_waits)
        DE_FREE(d->alloc, n->log_waits, sizeof *n->log_waits * n->log_wait_cap);
    n->log = NULL;
    n->log_barriers = NULL;
    n->log_waits = NULL;
    n->log_count = n->log_cap = n->log_barrier_count = n->log_barrier_cap = 0;
    n->log_wait_count = n->log_wait_cap = 0;
}

void de__gpu_null_log_enable(de_gpu_device *d, bool on) {
    if (!d || d->vt != &de__gpu_null_vtable)
        return;
    null_device *n = nd(d);
    if (on) {
        de_mutex_lock(&n->log_lock);
        n->log_count = n->log_barrier_count = n->log_wait_count = 0;
        de_mutex_unlock(&n->log_lock);
    }
    de_atomic_store_u32(&n->log_on, on ? 1u : 0u, DE_ATOMIC_RELEASE);
}

u32 de__gpu_null_log_count(de_gpu_device *d) {
    if (!d || d->vt != &de__gpu_null_vtable)
        return 0;
    return nd(d)->log_count;
}

const de__gpu_null_entry *de__gpu_null_log_entry(de_gpu_device *d, u32 i) {
    if (!d || d->vt != &de__gpu_null_vtable || i >= nd(d)->log_count)
        return NULL;
    return &nd(d)->log[i];
}

const de_gpu_barrier *de__gpu_null_log_barriers(de_gpu_device *d, const de__gpu_null_entry *e) {
    if (!d || d->vt != &de__gpu_null_vtable || !e || e->op != DE__GPU_NULL_OP_BARRIERS)
        return NULL;
    const null_device *n = nd(d);
    return n->log_barriers ? n->log_barriers + e->data_first : NULL; /* NULL: nothing logged */
}

const de_gpu_wait *de__gpu_null_log_waits(de_gpu_device *d, const de__gpu_null_entry *e) {
    if (!d || d->vt != &de__gpu_null_vtable || !e || e->op != DE__GPU_NULL_OP_SUBMIT)
        return NULL;
    const null_device *n = nd(d);
    return n->log_waits ? n->log_waits + e->data_first : NULL; /* NULL: no wait logged yet */
}
