/* null_backend.c — DE_GPU_NULL: a backend without a device (DENG-281).
 *
 * For headless code paths and tests. UPLOAD and READBACK buffers are CPU memory (map works);
 * GPU_ONLY buffers and textures are metadata only; command recording is a no-op (every cmd_*
 * entry is NULL, so the front-end validates and records nothing); a submit completes at once,
 * signalling its fence and its queue timeline. Caps describe a Base-tier capable device named
 * "null" with mesh shaders and without ray tracing.
 *
 * Test hook: with manual completion (de__gpu_null_set_manual_completion), the queue timelines
 * complete only as far as de__gpu_null_complete says (or fully on wait_idle), so deferred
 * destruction can be observed step by step. User fences still signal at submit.
 */
#include "gpu/gpu_backend.h"

#include <string.h>

typedef struct null_device {
    de_atomic_u64 submitted[DE_QUEUE_COUNT_]; /* last internal value submitted */
    de_atomic_u64 released[DE_QUEUE_COUNT_];  /* manual mode: completed up to here */
    de_atomic_u32 manual;
} null_device;

typedef struct null_buffer {
    void *mem; /* UPLOAD/READBACK only */
} null_buffer;

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

static void null_device_destroy(de_gpu_device *d) {
    (void)d;
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

static de_result null_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                   u64 queue_value) {
    null_device *n = nd(d);
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

const de__gpu_backend_vtable de__gpu_null_vtable = {
    .name = "null",
    .device_payload_size = sizeof(null_device),
    .cmd_payload_size = 0,
    .payload_size =
        {
            [DE__GPU_BUFFER] = sizeof(null_buffer),
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
