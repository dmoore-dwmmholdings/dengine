/* gpu.c — de_gpu front-end: backend selection, devices, object lifetime, bindless indices,
 * command-list recycling and per-command validation (DENG-281). See gpu_backend.h for the
 * backend contract and de_gpu.h for the API.
 */
#include "gpu/gpu_backend.h"

#include <string.h>

/* ---------- Backend table ---------- */

typedef struct backend_entry {
    de_gpu_backend backend;
    const de__gpu_backend_vtable *vt;
} backend_entry;

/* The backends compiled into this build. Each backend task appends one entry, guarded by the
 * DE_HAS_GPU_<NAME> define of its module (cmake/DeOptions.cmake), for example:
 *     #if DE_HAS_GPU_METAL
 *         {DE_GPU_METAL, &de__gpu_metal_vtable},
 *     #endif
 * and declares the vtable in gpu_backend.h. */
static const backend_entry k_backends[] = {
    {DE_GPU_NULL, &de__gpu_null_vtable},
#if DE_HAS_GPU_VULKAN
    {DE_GPU_VULKAN, &de__gpu_vulkan_vtable},
#endif
#if DE_HAS_GPU_METAL
    {DE_GPU_METAL, &de__gpu_metal_vtable},
#endif
#if DE_HAS_GPU_D3D12
    {DE_GPU_D3D12, &de__gpu_d3d12_vtable},
#endif
};

/* Run-time overrides (de__gpu_register_backend). Static storage starts as NULL. */
static de_atomic_ptr g_override[DE_GPU_BACKEND_COUNT_];

const char *de_gpu_backend_name(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_DEFAULT:
        return "default";
    case DE_GPU_D3D12:
        return "d3d12";
    case DE_GPU_VULKAN:
        return "vulkan";
    case DE_GPU_METAL:
        return "metal";
    case DE_GPU_NULL:
        return "null";
    default:
        return "unknown";
    }
}

de_gpu_backend de_gpu_platform_default_backend(void) {
#if defined(_WIN32)
    return DE_GPU_D3D12;
#elif defined(__APPLE__)
    return DE_GPU_METAL;
#else
    return DE_GPU_VULKAN;
#endif
}

static de_gpu_backend resolve_backend(de_gpu_backend b) {
    return b == DE_GPU_DEFAULT ? de_gpu_platform_default_backend() : b;
}

void de__gpu_register_backend(de_gpu_backend backend, const de__gpu_backend_vtable *vt) {
    if (backend == DE_GPU_DEFAULT || (u32)backend >= DE_GPU_BACKEND_COUNT_)
        return;
    de_atomic_store_ptr(&g_override[backend], (void *)(uintptr_t)vt, DE_ATOMIC_RELEASE);
}

const de__gpu_backend_vtable *de__gpu_find_backend(de_gpu_backend backend) {
    if ((u32)backend >= DE_GPU_BACKEND_COUNT_)
        return NULL;
    backend = resolve_backend(backend);
    const de__gpu_backend_vtable *vt =
        (const de__gpu_backend_vtable *)de_atomic_load_ptr(&g_override[backend], DE_ATOMIC_ACQUIRE);
    if (vt)
        return vt;
    for (size_t i = 0; i < sizeof k_backends / sizeof k_backends[0]; ++i)
        if (k_backends[i].backend == backend)
            return k_backends[i].vt;
    return NULL;
}

/* ---------- Object kinds ---------- */

static const size_t k_rec_size[DE__GPU_KIND_COUNT] = {
    [DE__GPU_BUFFER] = sizeof(de__gpu_buffer),   [DE__GPU_TEXTURE] = sizeof(de__gpu_texture),
    [DE__GPU_SAMPLER] = sizeof(de__gpu_sampler), [DE__GPU_PIPELINE] = sizeof(de__gpu_pipeline),
    [DE__GPU_ACCEL] = sizeof(de__gpu_accel),     [DE__GPU_SWAPCHAIN] = sizeof(de__gpu_swapchain),
    [DE__GPU_FENCE] = sizeof(de__gpu_fence),     [DE__GPU_QUERY_HEAP] = sizeof(de__gpu_query_heap),
};

/* Slots per kind. Buffers, textures and acceleration structures can use every handle index;
 * the rest are bounded far below what any frame needs, which keeps their chunk tables small. */
static const u32 k_max_slots[DE__GPU_KIND_COUNT] = {
    [DE__GPU_BUFFER] = DE__GPU_POOL_MAX_SLOTS,
    [DE__GPU_TEXTURE] = DE__GPU_POOL_MAX_SLOTS,
    [DE__GPU_SAMPLER] = 4096,
    [DE__GPU_PIPELINE] = 65536,
    [DE__GPU_ACCEL] = DE__GPU_POOL_MAX_SLOTS,
    [DE__GPU_SWAPCHAIN] = 64,
    [DE__GPU_FENCE] = 4096,
    [DE__GPU_QUERY_HEAP] = 4096,
};

static void copy_name(char dst[DE__GPU_NAME_MAX], const char *src) {
    size_t n = 0;
    if (src)
        while (n < DE__GPU_NAME_MAX - 1 && src[n])
            ++n;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static void *lookup(de_gpu_device *d, de__gpu_kind kind, u32 id) {
    return de__gpu_pool_get(&d->pools[kind], id);
}

de__gpu_buffer *de__gpu_get_buffer(de_gpu_device *d, de_gpu_buffer h) {
    return (de__gpu_buffer *)lookup(d, DE__GPU_BUFFER, h.id);
}
de__gpu_texture *de__gpu_get_texture(de_gpu_device *d, de_gpu_texture h) {
    return (de__gpu_texture *)lookup(d, DE__GPU_TEXTURE, h.id);
}
de__gpu_sampler *de__gpu_get_sampler(de_gpu_device *d, de_gpu_sampler h) {
    return (de__gpu_sampler *)lookup(d, DE__GPU_SAMPLER, h.id);
}
de__gpu_pipeline *de__gpu_get_pipeline(de_gpu_device *d, de_gpu_pipeline h) {
    return (de__gpu_pipeline *)lookup(d, DE__GPU_PIPELINE, h.id);
}
de__gpu_accel *de__gpu_get_accel(de_gpu_device *d, de_gpu_accel h) {
    return (de__gpu_accel *)lookup(d, DE__GPU_ACCEL, h.id);
}
de__gpu_fence *de__gpu_get_fence(de_gpu_device *d, de_gpu_fence h) {
    return (de__gpu_fence *)lookup(d, DE__GPU_FENCE, h.id);
}
de__gpu_query_heap *de__gpu_get_query_heap(de_gpu_device *d, de_gpu_query_heap h) {
    return (de__gpu_query_heap *)lookup(d, DE__GPU_QUERY_HEAP, h.id);
}

/* Lookup that reports a bad handle (error context in every build, log in Debug/Dev). */
static void *resolve(de_gpu_device *d, de__gpu_kind kind, u32 id, const char *func) {
    void *rec = lookup(d, kind, id);
    if (!rec)
        de__gpu_fail_handle(d, func, kind, id);
    return rec;
}

/* ---------- Reclaiming and deferred destruction ---------- */

static void free_texture_indices(de_gpu_device *d, de__gpu_texture *t) {
    de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_SAMPLED_TEXTURE, t->sampled);
    for (u32 m = 0; m < DE_GPU_MAX_MIPS; ++m)
        de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_STORAGE_TEXTURE, t->storage[m]);
    t->sampled = 0;
    memset(t->storage, 0, sizeof t->storage);
}

/* Destroys the backend object and frees its bindless indices and slot. The slot is retired. */
static void reclaim(void *user, u32 kind, u32 index) {
    de_gpu_device *d = (de_gpu_device *)user;
    const de__gpu_backend_vtable *vt = d->vt;
    void *rec = de__gpu_pool_at(&d->pools[kind], index);
    switch ((de__gpu_kind)kind) {
    case DE__GPU_BUFFER: {
        de__gpu_buffer *b = (de__gpu_buffer *)rec;
        if (vt->buffer_destroy)
            vt->buffer_destroy(d, b);
        de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_BUFFER, b->bindless);
        if (b->region)
            de__gpu_transient_region_release(b->region);
        b->region = NULL;
        break;
    }
    case DE__GPU_TEXTURE: {
        de__gpu_texture *t = (de__gpu_texture *)rec;
        if (vt->texture_destroy)
            vt->texture_destroy(d, t);
        free_texture_indices(d, t);
        if (t->region)
            de__gpu_transient_region_release(t->region);
        t->region = NULL;
        break;
    }
    case DE__GPU_SAMPLER: {
        de__gpu_sampler *s = (de__gpu_sampler *)rec;
        if (vt->sampler_destroy)
            vt->sampler_destroy(d, s);
        de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_SAMPLER, s->bindless);
        break;
    }
    case DE__GPU_PIPELINE:
        if (vt->pipeline_destroy)
            vt->pipeline_destroy(d, (de__gpu_pipeline *)rec);
        break;
    case DE__GPU_ACCEL: {
        de__gpu_accel *a = (de__gpu_accel *)rec;
        if (vt->accel_destroy)
            vt->accel_destroy(d, a);
        de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_ACCEL, a->bindless);
        break;
    }
    case DE__GPU_SWAPCHAIN:
        if (vt->swapchain_destroy)
            vt->swapchain_destroy(d, (de__gpu_swapchain *)rec);
        break;
    case DE__GPU_FENCE:
        if (vt->fence_destroy)
            vt->fence_destroy(d, (de__gpu_fence *)rec);
        break;
    case DE__GPU_QUERY_HEAP:
        if (vt->query_heap_destroy)
            vt->query_heap_destroy(d, (de__gpu_query_heap *)rec);
        break;
    default:
        DE_ASSERT(!"unknown gpu object kind");
        break;
    }
    de__gpu_pool_release(&d->pools[kind], index);
}

static void submitted_values(de_gpu_device *d, u64 out[DE_QUEUE_COUNT_]) {
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        out[q] = de_atomic_load_u64(&d->submitted[q], DE_ATOMIC_ACQUIRE);
}

static void destroy_object(de_gpu_device *d, de__gpu_kind kind, u32 id, const char *func) {
    if (!d || id == 0)
        return;
    u32 index = de__gpu_pool_retire(&d->pools[kind], id);
    if (index == DE__GPU_POOL_MAX_SLOTS) {
        de__gpu_fail_handle(d, func, kind, id);
        return;
    }
    u64 values[DE_QUEUE_COUNT_];
    submitted_values(d, values);
    if (de__gpu_deferred_push(&d->deferred, (u32)kind, index, values) != DE_OK) {
        /* No memory to queue it: wait for the GPU and release it now. */
        d->vt->wait_idle(d);
        reclaim(d, (u32)kind, index);
    }
}

/* Takes a slot back that never reached the GPU (a create that failed half-way). */
static void undo_slot(de_gpu_device *d, de__gpu_kind kind, u32 id) {
    u32 index = de__gpu_pool_retire(&d->pools[kind], id);
    if (index != DE__GPU_POOL_MAX_SLOTS)
        de__gpu_pool_release(&d->pools[kind], index);
}

static void recycle_cmds(de_gpu_device *d, const u64 done[DE_QUEUE_COUNT_]) {
    de_mutex_lock(&d->cmd_lock);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de_gpu_cmd *c = d->cmd_inflight_head[q];
        while (c && c->queue_value <= done[q]) {
            de_gpu_cmd *next = c->next;
            c->state = DE__GPU_CMD_FREE;
            c->next = d->cmd_free[q];
            d->cmd_free[q] = c;
            c = next;
        }
        d->cmd_inflight_head[q] = c;
        if (!c)
            d->cmd_inflight_tail[q] = NULL;
    }
    de_mutex_unlock(&d->cmd_lock);
}

u32 de__gpu_collect(de_gpu_device *d) {
    u64 done[DE_QUEUE_COUNT_];
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        done[q] = d->vt->queue_completed(d, (de_gpu_queue)q);
    u32 n = de__gpu_deferred_collect(&d->deferred, done, reclaim, d);
    recycle_cmds(d, done);
    return n;
}

u32 de__gpu_deferred_count(de_gpu_device *d) {
    return de__gpu_deferred_pending(&d->deferred);
}

u32 de__gpu_live_count(de_gpu_device *d, de__gpu_kind kind) {
    return de__gpu_pool_get_stats(&d->pools[kind]).live;
}

u32 de__gpu_bindless_count(de_gpu_device *d, de__gpu_bindless_space space) {
    return de__gpu_bindless_live(&d->bindless, space);
}

/* ---------- Device ---------- */

static void free_cmds(de_gpu_device *d) {
    de_gpu_cmd *c = d->cmd_all;
    while (c) {
        de_gpu_cmd *next = c->all_next;
        if (d->vt->cmd_fini)
            d->vt->cmd_fini(d, c);
        DE_FREE(d->alloc, c, d->cmd_bytes);
        c = next;
    }
    d->cmd_all = NULL;
}

/* Frees everything device_create set up. `stage`: 0 = before the backend device, 1 = backend
 * device exists, 2 = bindless and pools set up too. */
static void device_free(de_gpu_device *d, int stage) {
    if (stage >= 2) {
        for (u32 k = 0; k < DE__GPU_KIND_COUNT; ++k)
            de__gpu_pool_destroy(&d->pools[k]);
        de__gpu_bindless_destroy(&d->bindless);
    }
    de__gpu_heaps_shutdown(d, stage >= 1);
    if (stage >= 1)
        d->vt->device_destroy(d);
    de__gpu_deferred_destroy(&d->deferred);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        de_mutex_destroy(&d->submit_lock[q]);
    de_mutex_destroy(&d->cmd_lock);
    const de_allocator *a = d->alloc;
    DE_FREE(a, d, d->bytes);
}

de_result de_gpu_device_create(const de_gpu_device_desc *desc, de_gpu_device **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_device_create: out is NULL");
    *out = NULL;
    de_gpu_device_desc dd;
    if (desc)
        dd = *desc;
    else
        memset(&dd, 0, sizeof dd);
    if ((u32)dd.backend >= DE_GPU_BACKEND_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_gpu_device_create: backend %d is not a de_gpu_backend value",
                       (int)dd.backend);
    const de_gpu_backend backend = resolve_backend(dd.backend);
    const de__gpu_backend_vtable *vt = de__gpu_find_backend(backend);
    if (!vt)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_gpu_device_create: the %s backend%s is not compiled into this build",
                       de_gpu_backend_name(backend),
                       dd.backend == DE_GPU_DEFAULT ? " (the platform default)" : "");
    if (!vt->device_create || !vt->device_destroy || !vt->wait_idle || !vt->queue_submit ||
        !vt->queue_completed)
        return DE_FAIL(DE_ERR_INTERNAL,
                       "de_gpu_device_create: the %s backend's vtable lacks a required entry",
                       vt->name ? vt->name : de_gpu_backend_name(backend));

    const de_allocator *a = dd.allocator ? dd.allocator : de_heap_allocator();
    dd.allocator = a;
    size_t bytes = DE__GPU_ALIGN16(sizeof(struct de_gpu_device)) + vt->device_payload_size;
    de_gpu_device *d = (de_gpu_device *)DE_ALLOC(a, bytes, 16, DE_MEM_RENDER);
    if (!d)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_device_create: device (%zu bytes)",
                            bytes);
    memset(d, 0, bytes);
    d->vt = vt;
    d->alloc = a;
    d->desc = dd;
    d->bytes = bytes;
    d->cmd_bytes = DE__GPU_ALIGN16(sizeof(struct de_gpu_cmd)) + vt->cmd_payload_size;
    d->log = de_log_register_category("gpu", DE_LOG_INFO);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de_mutex_init(&d->submit_lock[q]);
        de_atomic_init_u64(&d->submitted[q], 0);
    }
    de_mutex_init(&d->cmd_lock);
    de__gpu_deferred_init(&d->deferred, a);

    /* Heaps first: the backend's device_create registers its memory classes. */
    de_result r = de__gpu_heaps_init(d);
    if (r != DE_OK) {
        device_free(d, 0);
        return r;
    }
    r = vt->device_create(d, &dd, &d->caps);
    if (r != DE_OK) {
        device_free(d, 0);
        return DE_FAIL_WRAP(r, "de_gpu_device_create: %s device", de_gpu_backend_name(backend));
    }
    d->caps.backend = backend;

    de__gpu_bindless_init(&d->bindless, a, &d->caps);
    if (vt->bindless_shared_resource_heap)
        de__gpu_bindless_share_resource_numbering(&d->bindless);
    for (u32 k = 0; k < DE__GPU_KIND_COUNT; ++k) {
        size_t elem = DE__GPU_ALIGN16(k_rec_size[k]) + vt->payload_size[k];
        r = de__gpu_pool_init(&d->pools[k], a, de__gpu_kind_name((de__gpu_kind)k), (u32)elem,
                              k_max_slots[k]);
        if (r != DE_OK) {
            device_free(d, 2);
            return r;
        }
    }
    DE_INFO(d->log, "gpu device: %s backend, \"%s\"", de_gpu_backend_name(backend),
            d->caps.device_name);
    *out = d;
    return DE_OK;
}

void de_gpu_device_destroy(de_gpu_device *d) {
    if (!d)
        return;
    d->vt->wait_idle(d);
    /* Swapchains first: their images are textures they destroy themselves. */
    for (u32 k = 0; k < DE__GPU_KIND_COUNT; ++k) {
        u32 order = k == 0 ? DE__GPU_SWAPCHAIN : (k == DE__GPU_SWAPCHAIN ? 0u : k);
        de__gpu_pool *p = &d->pools[order];
        u32 used = de__gpu_pool_get_stats(p).used;
        u32 leaked = 0;
        for (u32 i = 0; i < used; ++i) {
            if (!de__gpu_pool_is_live(p, i))
                continue;
            de__gpu_texture *t =
                order == DE__GPU_TEXTURE ? (de__gpu_texture *)de__gpu_pool_at(p, i) : NULL;
            if (!(t && t->external))
                ++leaked;
            destroy_object(d, (de__gpu_kind)order, de__gpu_pool_id_at(p, i),
                           "de_gpu_device_destroy");
        }
        if (leaked)
            DE_WARN(d->log, "de_gpu_device_destroy: %u %s object%s still alive; destroyed now",
                    leaked, de__gpu_kind_name((de__gpu_kind)order), leaked == 1 ? "" : "s");
        while (de__gpu_deferred_pending(&d->deferred) > 0)
            de__gpu_deferred_collect(&d->deferred, NULL, reclaim, d);
    }
    free_cmds(d);
    device_free(d, 2);
}

const de_gpu_caps *de_gpu_get_caps(const de_gpu_device *d) {
    return d ? &d->caps : NULL;
}

void de_gpu_wait_idle(de_gpu_device *d) {
    if (!d)
        return;
    d->vt->wait_idle(d);
    de__gpu_collect(d);
}

/* ---------- Buffers ---------- */

#define BUFFER_BINDLESS_USAGE                                                                      \
    (DE_USAGE_VERTEX | DE_USAGE_INDEX | DE_USAGE_CONSTANT | DE_USAGE_STORAGE)

/* de_gpu_buffer_create, or placed creation at (heap, offset) when `region` is set. */
static de_gpu_buffer buffer_create(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                   de__gpu_heap *heap, u64 offset,
                                   de__gpu_transient_region *region) {
    de_gpu_buffer h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_buffer_create: device is NULL");
        return h;
    }
    if (de__gpu_validate_buffer_desc(d, desc) != DE_OK)
        return h;
    if (region ? !d->vt->buffer_create_placed : !d->vt->buffer_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED, "de_gpu_buffer_create: the %s backend has no %sbuffers",
                     d->vt->name, region ? "placed " : "");
        return h;
    }
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_BUFFER], &e);
    if (!id)
        return h;
    de__gpu_buffer *b = (de__gpu_buffer *)e;
    b->size = desc->size;
    b->usage = desc->usage;
    b->memory = desc->memory;
    b->stride = desc->stride;
    copy_name(b->name, desc->name);
    if (desc->usage & BUFFER_BINDLESS_USAGE) {
        b->bindless = de__gpu_bindless_alloc(&d->bindless, DE__GPU_BINDLESS_BUFFER);
        if (!b->bindless) {
            undo_slot(d, DE__GPU_BUFFER, id);
            return h;
        }
    }
    b->region = region;
    de_result r = region ? d->vt->buffer_create_placed(d, b, desc, heap, offset)
                         : d->vt->buffer_create(d, b, desc);
    if (r != DE_OK) {
        de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_BUFFER, b->bindless);
        b->region = NULL;
        undo_slot(d, DE__GPU_BUFFER, id);
        return h;
    }
    if (b->bindless && d->vt->bindless_write_buffer)
        d->vt->bindless_write_buffer(d, b->bindless, b);
    h.id = id;
    return h;
}

de_gpu_buffer de_gpu_buffer_create(de_gpu_device *d, const de_gpu_buffer_desc *desc) {
    return buffer_create(d, desc, NULL, 0, NULL);
}

de_gpu_buffer de__gpu_buffer_create_placed(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                           de__gpu_heap *heap, u64 offset,
                                           de__gpu_transient_region *region) {
    DE_ASSERT(heap && region);
    return buffer_create(d, desc, heap, offset, region);
}

void de_gpu_buffer_destroy(de_gpu_device *d, de_gpu_buffer b) {
    destroy_object(d, DE__GPU_BUFFER, b.id, "de_gpu_buffer_destroy");
}

void *de_gpu_buffer_map(de_gpu_device *d, de_gpu_buffer h) {
    if (!d)
        return NULL;
    de__gpu_buffer *b = (de__gpu_buffer *)resolve(d, DE__GPU_BUFFER, h.id, "de_gpu_buffer_map");
    if (!b)
        return NULL;
    if (b->memory == DE_MEM_GPU_ONLY) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                     "de_gpu_buffer_map(\"%s\"): the buffer is DE_MEM_GPU_ONLY; only "
                     "DE_MEM_UPLOAD and DE_MEM_READBACK buffers can be mapped",
                     b->name);
        return NULL;
    }
    return b->mapped;
}

u32 de_gpu_buffer_bindless(de_gpu_device *d, de_gpu_buffer h) {
    if (!d)
        return DE_GPU_BINDLESS_INVALID;
    de__gpu_buffer *b =
        (de__gpu_buffer *)resolve(d, DE__GPU_BUFFER, h.id, "de_gpu_buffer_bindless");
    if (!b)
        return DE_GPU_BINDLESS_INVALID;
    if (!b->bindless)
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                     "de_gpu_buffer_bindless(\"%s\"): no bindless index; it needs VERTEX, INDEX, "
                     "CONSTANT or STORAGE usage",
                     b->name);
    return b->bindless;
}

/* ---------- Textures ---------- */

/* Validates, takes a slot and the bindless indices, fills the record with defaults applied. */
static u32 texture_alloc(de_gpu_device *d, const de_gpu_texture_desc *desc, de__gpu_texture **out) {
    *out = NULL;
    if (de__gpu_validate_texture_desc(d, desc) != DE_OK)
        return 0;
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_TEXTURE], &e);
    if (!id)
        return 0;
    de__gpu_texture *t = (de__gpu_texture *)e;
    t->dim = desc->dim;
    t->format = desc->format;
    t->width = desc->width;
    t->height = desc->height ? desc->height : 1u;
    t->depth_or_layers =
        desc->depth_or_layers ? desc->depth_or_layers : (desc->dim == DE_TEX_CUBE ? 6u : 1u);
    t->mip_count = desc->mip_count ? desc->mip_count : 1u;
    t->sample_count = desc->sample_count ? desc->sample_count : 1u;
    t->usage = desc->usage;
    t->transient = desc->transient;
    copy_name(t->name, desc->name);
    bool ok = true;
    if (t->usage & DE_USAGE_SAMPLED) {
        t->sampled = de__gpu_bindless_alloc(&d->bindless, DE__GPU_BINDLESS_SAMPLED_TEXTURE);
        ok = t->sampled != 0;
    }
    if (ok && (t->usage & DE_USAGE_STORAGE)) {
        for (u32 m = 0; m < t->mip_count && ok; ++m) {
            t->storage[m] = de__gpu_bindless_alloc(&d->bindless, DE__GPU_BINDLESS_STORAGE_TEXTURE);
            ok = t->storage[m] != 0;
        }
    }
    if (!ok) {
        free_texture_indices(d, t);
        undo_slot(d, DE__GPU_TEXTURE, id);
        return 0;
    }
    *out = t;
    return id;
}

/* The record with defaults applied, as a desc for the backend. */
static de_gpu_texture_desc texture_desc_of(const de__gpu_texture *t, const char *name) {
    de_gpu_texture_desc nd;
    memset(&nd, 0, sizeof nd);
    nd.dim = t->dim;
    nd.format = t->format;
    nd.width = t->width;
    nd.height = t->height;
    nd.depth_or_layers = t->depth_or_layers;
    nd.mip_count = t->mip_count;
    nd.sample_count = t->sample_count;
    nd.usage = t->usage;
    nd.transient = t->transient;
    nd.name = name;
    return nd;
}

void de__gpu_texture_wrap_finish(de_gpu_device *d, de__gpu_texture *t) {
    if (!d->vt->bindless_write_texture)
        return;
    if (t->sampled)
        d->vt->bindless_write_texture(d, t->sampled, t, 0, false);
    for (u32 m = 0; m < t->mip_count; ++m)
        if (t->storage[m])
            d->vt->bindless_write_texture(d, t->storage[m], t, m, true);
}

/* de_gpu_texture_create, or placed creation at (heap, offset) when `region` is set. */
static de_gpu_texture texture_create(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                     de__gpu_heap *heap, u64 offset,
                                     de__gpu_transient_region *region) {
    de_gpu_texture h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_texture_create: device is NULL");
        return h;
    }
    if (region ? !d->vt->texture_create_placed : !d->vt->texture_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                     "de_gpu_texture_create: the %s backend has no %stextures", d->vt->name,
                     region ? "placed " : "");
        return h;
    }
    de__gpu_texture *t;
    u32 id = texture_alloc(d, desc, &t);
    if (!id)
        return h;
    de_gpu_texture_desc nd = texture_desc_of(t, desc->name);
    t->region = region;
    de_result r = region ? d->vt->texture_create_placed(d, t, &nd, heap, offset)
                         : d->vt->texture_create(d, t, &nd);
    if (r != DE_OK) {
        free_texture_indices(d, t);
        t->region = NULL;
        undo_slot(d, DE__GPU_TEXTURE, id);
        return h;
    }
    de__gpu_texture_wrap_finish(d, t);
    h.id = id;
    return h;
}

de_gpu_texture de_gpu_texture_create(de_gpu_device *d, const de_gpu_texture_desc *desc) {
    return texture_create(d, desc, NULL, 0, NULL);
}

de_gpu_texture de__gpu_texture_create_placed(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                             de__gpu_heap *heap, u64 offset,
                                             de__gpu_transient_region *region) {
    DE_ASSERT(heap && region);
    return texture_create(d, desc, heap, offset, region);
}

de_gpu_texture de__gpu_texture_wrap(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                    de__gpu_texture **out) {
    de_gpu_texture h = {0};
    de__gpu_texture *t;
    u32 id = texture_alloc(d, desc, &t);
    *out = t;
    if (!id)
        return h;
    t->external = true;
    h.id = id;
    return h;
}

void de_gpu_texture_destroy(de_gpu_device *d, de_gpu_texture t) {
    destroy_object(d, DE__GPU_TEXTURE, t.id, "de_gpu_texture_destroy");
}

u32 de_gpu_texture_bindless(de_gpu_device *d, de_gpu_texture h, u32 mip, bool storage) {
    static const char F[] = "de_gpu_texture_bindless";
    if (!d)
        return DE_GPU_BINDLESS_INVALID;
    de__gpu_texture *t = (de__gpu_texture *)resolve(d, DE__GPU_TEXTURE, h.id, F);
    if (!t)
        return DE_GPU_BINDLESS_INVALID;
    if (!storage) {
        if (!(t->usage & DE_USAGE_SAMPLED)) {
            de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                         "%s(\"%s\"): no sampled view; the texture lacks DE_USAGE_SAMPLED", F,
                         t->name);
            return DE_GPU_BINDLESS_INVALID;
        }
        if (mip != 0) {
            de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                         "%s(\"%s\"): mip %u; the sampled view covers every mip, pass 0", F,
                         t->name, mip);
            return DE_GPU_BINDLESS_INVALID;
        }
        return t->sampled;
    }
    if (!(t->usage & DE_USAGE_STORAGE)) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                     "%s(\"%s\"): no storage view; the texture lacks DE_USAGE_STORAGE", F, t->name);
        return DE_GPU_BINDLESS_INVALID;
    }
    if (mip >= t->mip_count) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): mip %u, but the texture has %u", F,
                     t->name, mip, t->mip_count);
        return DE_GPU_BINDLESS_INVALID;
    }
    return t->storage[mip];
}

/* ---------- Samplers ---------- */

de_gpu_sampler de_gpu_sampler_create(de_gpu_device *d, const de_gpu_sampler_desc *desc) {
    de_gpu_sampler h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_sampler_create: device is NULL");
        return h;
    }
    if (de__gpu_validate_sampler_desc(d, desc) != DE_OK)
        return h;
    if (!d->vt->sampler_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                     "de_gpu_sampler_create: the %s backend has no "
                     "samplers",
                     d->vt->name);
        return h;
    }
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_SAMPLER], &e);
    if (!id)
        return h;
    de__gpu_sampler *s = (de__gpu_sampler *)e;
    s->desc = *desc;
    s->bindless = de__gpu_bindless_alloc(&d->bindless, DE__GPU_BINDLESS_SAMPLER);
    if (!s->bindless) {
        undo_slot(d, DE__GPU_SAMPLER, id);
        return h;
    }
    if (d->vt->sampler_create(d, s, desc) != DE_OK) {
        de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_SAMPLER, s->bindless);
        undo_slot(d, DE__GPU_SAMPLER, id);
        return h;
    }
    if (d->vt->bindless_write_sampler)
        d->vt->bindless_write_sampler(d, s->bindless, s);
    h.id = id;
    return h;
}

void de_gpu_sampler_destroy(de_gpu_device *d, de_gpu_sampler s) {
    destroy_object(d, DE__GPU_SAMPLER, s.id, "de_gpu_sampler_destroy");
}

u32 de_gpu_sampler_bindless(de_gpu_device *d, de_gpu_sampler h) {
    if (!d)
        return DE_GPU_BINDLESS_INVALID;
    de__gpu_sampler *s =
        (de__gpu_sampler *)resolve(d, DE__GPU_SAMPLER, h.id, "de_gpu_sampler_bindless");
    return s ? s->bindless : DE_GPU_BINDLESS_INVALID;
}

/* ---------- Pipelines ---------- */

de_gpu_pipeline de_gpu_pipeline_create(de_gpu_device *d, const de_gpu_pipeline_desc *desc) {
    de_gpu_pipeline h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_pipeline_create: device is NULL");
        return h;
    }
    if (de__gpu_validate_pipeline_desc(d, desc) != DE_OK)
        return h;
    if (!d->vt->pipeline_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                     "de_gpu_pipeline_create: the %s backend has no "
                     "pipelines yet",
                     d->vt->name);
        return h;
    }
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_PIPELINE], &e);
    if (!id)
        return h;
    de__gpu_pipeline *p = (de__gpu_pipeline *)e;
    p->kind = desc->kind;
    p->push_constant_bytes = desc->push_constant_bytes;
    p->color_count = desc->kind == DE_PIPE_COMPUTE ? 0u : desc->color_count;
    for (u32 i = 0; i < p->color_count; ++i)
        p->color_formats[i] = desc->color_formats[i];
    p->depth_format = desc->kind == DE_PIPE_COMPUTE ? DE_FMT_UNKNOWN : desc->depth_format;
    copy_name(p->name, desc->name);
    if (d->vt->pipeline_create(d, p, desc) != DE_OK) {
        undo_slot(d, DE__GPU_PIPELINE, id);
        return h;
    }
    h.id = id;
    return h;
}

void de_gpu_pipeline_destroy(de_gpu_device *d, de_gpu_pipeline p) {
    destroy_object(d, DE__GPU_PIPELINE, p.id, "de_gpu_pipeline_destroy");
}

/* ---------- Acceleration structures (S16) ---------- */

de_gpu_accel de_gpu_accel_create(de_gpu_device *d, const de_gpu_accel_desc *desc) {
    de_gpu_accel h = {0};
    (void)desc;
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_accel_create: device is NULL");
        return h;
    }
    de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                 "de_gpu_accel_create: acceleration structures are not available before S16");
    return h;
}

void de_gpu_accel_destroy(de_gpu_device *d, de_gpu_accel a) {
    destroy_object(d, DE__GPU_ACCEL, a.id, "de_gpu_accel_destroy");
}

u32 de_gpu_accel_bindless(de_gpu_device *d, de_gpu_accel tlas) {
    if (!d)
        return DE_GPU_BINDLESS_INVALID;
    de__gpu_accel *a = (de__gpu_accel *)resolve(d, DE__GPU_ACCEL, tlas.id, "de_gpu_accel_bindless");
    return a ? a->bindless : DE_GPU_BINDLESS_INVALID;
}

/* ---------- Query heaps ---------- */

de_gpu_query_heap de_gpu_query_heap_create(de_gpu_device *d, const de_gpu_query_heap_desc *desc) {
    de_gpu_query_heap h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_query_heap_create: device is NULL");
        return h;
    }
    if (de__gpu_validate_query_heap_desc(d, desc) != DE_OK)
        return h;
    if (!d->vt->query_heap_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                     "de_gpu_query_heap_create: the %s backend has no "
                     "query heaps",
                     d->vt->name);
        return h;
    }
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_QUERY_HEAP], &e);
    if (!id)
        return h;
    de__gpu_query_heap *q = (de__gpu_query_heap *)e;
    q->type = desc->type;
    q->count = desc->count;
    copy_name(q->name, desc->name);
    if (d->vt->query_heap_create(d, q, desc) != DE_OK) {
        undo_slot(d, DE__GPU_QUERY_HEAP, id);
        return h;
    }
    h.id = id;
    return h;
}

void de_gpu_query_heap_destroy(de_gpu_device *d, de_gpu_query_heap h) {
    destroy_object(d, DE__GPU_QUERY_HEAP, h.id, "de_gpu_query_heap_destroy");
}

/* ---------- Fences and submission ---------- */

de_gpu_fence de_gpu_fence_create(de_gpu_device *d, u64 initial) {
    de_gpu_fence h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_fence_create: device is NULL");
        return h;
    }
    if (!d->vt->fence_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED, "de_gpu_fence_create: the %s backend has no fences",
                     d->vt->name);
        return h;
    }
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_FENCE], &e);
    if (!id)
        return h;
    de__gpu_fence *f = (de__gpu_fence *)e;
    f->initial = initial;
    if (d->vt->fence_create(d, f, initial) != DE_OK) {
        undo_slot(d, DE__GPU_FENCE, id);
        return h;
    }
    h.id = id;
    return h;
}

void de_gpu_fence_destroy(de_gpu_device *d, de_gpu_fence f) {
    destroy_object(d, DE__GPU_FENCE, f.id, "de_gpu_fence_destroy");
}

u64 de_gpu_fence_value(de_gpu_device *d, de_gpu_fence h) {
    if (!d)
        return 0;
    de__gpu_fence *f = (de__gpu_fence *)resolve(d, DE__GPU_FENCE, h.id, "de_gpu_fence_value");
    return f && d->vt->fence_value ? d->vt->fence_value(d, f) : 0;
}

de_result de_gpu_fence_wait(de_gpu_device *d, de_gpu_fence h, u64 value, u64 timeout_ns) {
    if (!d)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_fence_wait: device is NULL");
    de__gpu_fence *f = (de__gpu_fence *)resolve(d, DE__GPU_FENCE, h.id, "de_gpu_fence_wait");
    if (!f)
        return DE_ERR_INVALID_ARGUMENT;
    if (!d->vt->fence_wait)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "de_gpu_fence_wait: no fence support");
    return d->vt->fence_wait(d, f, value, timeout_ns);
}

de_result de_gpu_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s) {
    if (!d)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_queue_submit: device is NULL");
    DE_TRY(de__gpu_validate_submit(d, q, s));
#if DE__GPU_VALIDATE_COMMANDS
    for (u32 i = 0; i < s->cmd_count; ++i)
        if (s->cmds[i]->marker_depth != 0)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "de_gpu_queue_submit: cmds[%u] (\"%s\") has %u open marker_push "
                                "scope(s)",
                                i, s->cmds[i]->name, s->cmds[i]->marker_depth);
#endif
    de_mutex_lock(&d->submit_lock[q]);
    u64 value = de_atomic_load_u64(&d->submitted[q], DE_ATOMIC_RELAXED) + 1u;
    /* Published before the backend call: a destroy racing with this submit must wait for it. */
    de_atomic_store_u64(&d->submitted[q], value, DE_ATOMIC_RELEASE);
    de_result r = d->vt->queue_submit(d, q, s, value);
    de_mutex_lock(&d->cmd_lock);
    for (u32 i = 0; i < s->cmd_count; ++i) {
        de_gpu_cmd *c = s->cmds[i];
        c->state = DE__GPU_CMD_SUBMITTED;
        c->queue_value = value;
        c->next = NULL;
        if (d->cmd_inflight_tail[q])
            d->cmd_inflight_tail[q]->next = c;
        else
            d->cmd_inflight_head[q] = c;
        d->cmd_inflight_tail[q] = c;
    }
    de_mutex_unlock(&d->cmd_lock);
    de_mutex_unlock(&d->submit_lock[q]);
    de__gpu_collect(d);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_gpu_queue_submit: %s backend", d->vt->name);
    return DE_OK;
}

/* ---------- Swapchains ---------- */

de_gpu_swapchain de_gpu_swapchain_create(de_gpu_device *d, const de_gpu_swapchain_desc *desc) {
    de_gpu_swapchain h = {0};
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_swapchain_create: device is NULL");
        return h;
    }
    if (de__gpu_validate_swapchain_desc(d, desc) != DE_OK)
        return h;
    if (!d->vt->swapchain_create) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                     "de_gpu_swapchain_create: the %s backend has no "
                     "swapchain",
                     d->vt->name);
        return h;
    }
    void *e;
    u32 id = de__gpu_pool_alloc(&d->pools[DE__GPU_SWAPCHAIN], &e);
    if (!id)
        return h;
    de__gpu_swapchain *sc = (de__gpu_swapchain *)e;
    sc->desc = *desc;
    if (d->vt->swapchain_create(d, sc, desc) != DE_OK) {
        undo_slot(d, DE__GPU_SWAPCHAIN, id);
        return h;
    }
    h.id = id;
    return h;
}

void de_gpu_swapchain_destroy(de_gpu_device *d, de_gpu_swapchain s) {
    destroy_object(d, DE__GPU_SWAPCHAIN, s.id, "de_gpu_swapchain_destroy");
}

/* NULL with the error set (by the handle check, or here for a NULL device). */
static de__gpu_swapchain *swapchain(de_gpu_device *d, de_gpu_swapchain h, const char *f) {
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: device is NULL", f);
        return NULL;
    }
    return (de__gpu_swapchain *)resolve(d, DE__GPU_SWAPCHAIN, h.id, f);
}

de_result de_gpu_swapchain_acquire(de_gpu_device *d, de_gpu_swapchain s, de_gpu_texture *image) {
    static const char F[] = "de_gpu_swapchain_acquire";
    if (image)
        image->id = 0;
    if (!image)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: image is NULL", F);
    de__gpu_swapchain *sc = swapchain(d, s, F);
    if (!sc)
        return DE_ERR_INVALID_ARGUMENT;
    return d->vt->swapchain_acquire ? d->vt->swapchain_acquire(d, sc, image)
                                    : de__gpu_fail(d, DE_ERR_UNSUPPORTED, "%s: unsupported", F);
}

de_result de_gpu_swapchain_present(de_gpu_device *d, de_gpu_swapchain s) {
    static const char F[] = "de_gpu_swapchain_present";
    de__gpu_swapchain *sc = swapchain(d, s, F);
    if (!sc)
        return DE_ERR_INVALID_ARGUMENT;
    return d->vt->swapchain_present ? d->vt->swapchain_present(d, sc)
                                    : de__gpu_fail(d, DE_ERR_UNSUPPORTED, "%s: unsupported", F);
}

de_result de_gpu_swapchain_resize(de_gpu_device *d, de_gpu_swapchain s, u32 w, u32 h) {
    static const char F[] = "de_gpu_swapchain_resize";
    de__gpu_swapchain *sc = swapchain(d, s, F);
    if (!sc)
        return DE_ERR_INVALID_ARGUMENT;
    return d->vt->swapchain_resize ? d->vt->swapchain_resize(d, sc, w, h)
                                   : de__gpu_fail(d, DE_ERR_UNSUPPORTED, "%s: unsupported", F);
}

de_gpu_colorspace de_gpu_swapchain_colorspace(de_gpu_device *d, de_gpu_swapchain s) {
    de__gpu_swapchain *sc = swapchain(d, s, "de_gpu_swapchain_colorspace");
    if (!sc)
        return DE_CS_SRGB;
    return d->vt->swapchain_colorspace ? d->vt->swapchain_colorspace(d, sc) : sc->desc.colorspace;
}

/* ---------- Command lists ---------- */

de_gpu_cmd *de_gpu_cmd_begin(de_gpu_device *d, de_gpu_queue q, const char *name) {
    if (!d) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_cmd_begin: device is NULL");
        return NULL;
    }
    if ((u32)q >= DE_QUEUE_COUNT_) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                     "de_gpu_cmd_begin: queue %d is not a "
                     "de_gpu_queue value",
                     (int)q);
        return NULL;
    }
    de_mutex_lock(&d->cmd_lock);
    de_gpu_cmd *c = d->cmd_free[q];
    if (c)
        d->cmd_free[q] = c->next;
    de_mutex_unlock(&d->cmd_lock);
    if (!c) {
        c = (de_gpu_cmd *)DE_ALLOC(d->alloc, d->cmd_bytes, 16, DE_MEM_RENDER);
        if (!c) {
            DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_cmd_begin: command list");
            return NULL;
        }
        memset(c, 0, d->cmd_bytes);
        c->device = d;
        c->queue = q;
        if (d->vt->cmd_init && d->vt->cmd_init(d, c) != DE_OK) {
            DE_FREE(d->alloc, c, d->cmd_bytes);
            return NULL;
        }
        de_mutex_lock(&d->cmd_lock);
        c->all_next = d->cmd_all;
        d->cmd_all = c;
        de_mutex_unlock(&d->cmd_lock);
    }
    c->state = DE__GPU_CMD_RECORDING;
    c->in_render = false;
    c->has_pipeline = false;
    c->push_limit = 0;
    c->marker_depth = 0;
    c->queue_value = 0;
    c->next = NULL;
    copy_name(c->name, name);
    if (d->vt->cmd_begin && d->vt->cmd_begin(c, name) != DE_OK) {
        c->state = DE__GPU_CMD_FREE;
        de_mutex_lock(&d->cmd_lock);
        c->next = d->cmd_free[q];
        d->cmd_free[q] = c;
        de_mutex_unlock(&d->cmd_lock);
        return NULL;
    }
    return c;
}

/* Recording checks: Debug and Dev only. CHECK reports and skips the command. */
#if DE__GPU_VALIDATE_COMMANDS
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            de__gpu_fail(c->device, DE_ERR_INVALID_ARGUMENT, __VA_ARGS__);                         \
            return;                                                                                \
        }                                                                                          \
    } while (0)
#else
#define CHECK(cond, ...) ((void)0)
#endif

/* Every recording call starts here: a NULL list is ignored; a list that is not recording is
 * reported in Debug/Dev. */
#define CMD_ENTER(F)                                                                               \
    (void)(F);                                                                                     \
    if (!c)                                                                                        \
        return;                                                                                    \
    CHECK(c->state == DE__GPU_CMD_RECORDING,                                                       \
          "%s(\"%s\"): the command list is not recording "                                         \
          "(already submitted)",                                                                   \
          F, c->name)

/* Handle -> record inside a recording call; reports and skips on a bad handle. */
#define CMD_RESOLVE(var, T, kind, id, F)                                                           \
    T *var = (T *)lookup(c->device, kind, id);                                                     \
    if (!var) {                                                                                    \
        de__gpu_fail_handle(c->device, F, kind, id);                                               \
        return;                                                                                    \
    }

void de_gpu_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count) {
    static const char F[] = "de_gpu_cmd_barriers";
    CMD_ENTER(F);
    CHECK(count == 0 || b, "%s(\"%s\"): count is %u but barriers is NULL", F, c->name, count);
    CHECK(!c->in_render, "%s(\"%s\"): barriers inside a render pass; end_render first", F, c->name);
#if DE__GPU_VALIDATE_COMMANDS
    for (u32 i = 0; i < count; ++i) {
        const de_gpu_barrier *x = &b[i];
        u32 set = (x->texture.id != 0) + (x->buffer.id != 0) + (x->accel.id != 0);
        CHECK(set == 1, "%s(\"%s\"): barrier %u sets %u of texture/buffer/accel; set exactly one",
              F, c->name, i, set);
        if (x->texture.id) {
            CMD_RESOLVE(t, de__gpu_texture, DE__GPU_TEXTURE, x->texture.id, F);
            CHECK(x->mip_first + x->mip_count <= t->mip_count &&
                      x->layer_first + x->layer_count <= t->depth_or_layers,
                  "%s(\"%s\"): barrier %u mips/layers are outside texture \"%s\"", F, c->name, i,
                  t->name);
        } else if (x->buffer.id) {
            CMD_RESOLVE(bb, de__gpu_buffer, DE__GPU_BUFFER, x->buffer.id, F);
            (void)bb;
        } else {
            CMD_RESOLVE(a, de__gpu_accel, DE__GPU_ACCEL, x->accel.id, F);
            (void)a;
        }
        CHECK((u32)x->src_queue < DE_QUEUE_COUNT_ && (u32)x->dst_queue < DE_QUEUE_COUNT_,
              "%s(\"%s\"): barrier %u queue is out of range", F, c->name, i);
    }
#endif
    if (count && c->device->vt->cmd_barriers)
        c->device->vt->cmd_barriers(c, b, count);
}

void de_gpu_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                             const de_gpu_attachment *depth) {
    static const char F[] = "de_gpu_cmd_begin_render";
    CMD_ENTER(F);
    CHECK(!c->in_render, "%s(\"%s\"): a render pass is already open", F, c->name);
    CHECK(c->queue == DE_QUEUE_GRAPHICS, "%s(\"%s\"): render passes need the graphics queue", F,
          c->name);
    CHECK(color_count <= DE_GPU_MAX_COLOR_ATTACHMENTS,
          "%s(\"%s\"): %u color attachments, more "
          "than %u",
          F, c->name, color_count, DE_GPU_MAX_COLOR_ATTACHMENTS);
    CHECK(color_count == 0 || colors, "%s(\"%s\"): color_count is %u but colors is NULL", F,
          c->name, color_count);
    CHECK(color_count > 0 || depth, "%s(\"%s\"): no attachments", F, c->name);
#if DE__GPU_VALIDATE_COMMANDS
    for (u32 i = 0; i < color_count; ++i) {
        CMD_RESOLVE(t, de__gpu_texture, DE__GPU_TEXTURE, colors[i].texture.id, F);
        CHECK(t->usage & DE_USAGE_RENDER_TARGET,
              "%s(\"%s\"): color %u (\"%s\") lacks DE_USAGE_RENDER_TARGET", F, c->name, i, t->name);
        CHECK(colors[i].mip < t->mip_count && colors[i].layer < t->depth_or_layers,
              "%s(\"%s\"): color %u mip/layer is outside \"%s\"", F, c->name, i, t->name);
    }
    if (depth) {
        CMD_RESOLVE(t, de__gpu_texture, DE__GPU_TEXTURE, depth->texture.id, F);
        CHECK(t->usage & DE_USAGE_DEPTH, "%s(\"%s\"): depth (\"%s\") lacks DE_USAGE_DEPTH", F,
              c->name, t->name);
        CHECK(depth->mip < t->mip_count && depth->layer < t->depth_or_layers,
              "%s(\"%s\"): depth mip/layer is outside \"%s\"", F, c->name, t->name);
    }
#endif
    c->in_render = true;
    if (c->device->vt->cmd_begin_render)
        c->device->vt->cmd_begin_render(c, colors, color_count, depth);
}

void de_gpu_cmd_end_render(de_gpu_cmd *c) {
    static const char F[] = "de_gpu_cmd_end_render";
    CMD_ENTER(F);
    CHECK(c->in_render, "%s(\"%s\"): no render pass is open", F, c->name);
    c->in_render = false;
    if (c->device->vt->cmd_end_render)
        c->device->vt->cmd_end_render(c);
}

void de_gpu_cmd_set_pipeline(de_gpu_cmd *c, de_gpu_pipeline h) {
    static const char F[] = "de_gpu_cmd_set_pipeline";
    CMD_ENTER(F);
    CMD_RESOLVE(p, de__gpu_pipeline, DE__GPU_PIPELINE, h.id, F);
    CHECK(!(p->kind == DE_PIPE_COMPUTE && c->in_render),
          "%s(\"%s\"): compute pipeline \"%s\" inside a render pass", F, c->name, p->name);
    CHECK(p->kind == DE_PIPE_COMPUTE || c->queue == DE_QUEUE_GRAPHICS,
          "%s(\"%s\"): raster pipeline \"%s\" on a non-graphics queue", F, c->name, p->name);
    CHECK(c->queue != DE_QUEUE_COPY, "%s(\"%s\"): pipelines cannot be used on the copy queue", F,
          c->name);
    c->has_pipeline = true;
    c->pipeline_kind = p->kind;
    c->push_limit = p->push_constant_bytes;
    if (c->device->vt->cmd_set_pipeline)
        c->device->vt->cmd_set_pipeline(c, p);
}

void de_gpu_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes) {
    static const char F[] = "de_gpu_cmd_push_constants";
    CMD_ENTER(F);
    CHECK(c->has_pipeline, "%s(\"%s\"): no pipeline is bound", F, c->name);
    CHECK(data || bytes == 0, "%s(\"%s\"): data is NULL", F, c->name);
    CHECK(bytes <= c->push_limit,
          "%s(\"%s\"): %u bytes, but the bound pipeline declares "
          "push_constant_bytes = %u",
          F, c->name, bytes, c->push_limit);
    CHECK(bytes % 4u == 0, "%s(\"%s\"): %u bytes is not a multiple of 4", F, c->name, bytes);
    if (bytes && c->device->vt->cmd_push_constants)
        c->device->vt->cmd_push_constants(c, data, bytes);
}

void de_gpu_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h) {
    static const char F[] = "de_gpu_cmd_viewport";
    CMD_ENTER(F);
    CHECK(c->in_render, "%s(\"%s\"): no render pass is open", F, c->name);
    CHECK(w > 0.0f && h > 0.0f, "%s(\"%s\"): %gx%g is empty", F, c->name, (double)w, (double)h);
    if (c->device->vt->cmd_viewport)
        c->device->vt->cmd_viewport(c, x, y, w, h);
}

/* Checks an indirect argument range inside a buffer with INDIRECT usage. */
#define CHECK_INDIRECT(buf, off, bytes, what)                                                      \
    CHECK((buf)->usage &DE_USAGE_INDIRECT,                                                         \
          "%s(\"%s\"): %s buffer \"%s\" lacks "                                                    \
          "DE_USAGE_INDIRECT",                                                                     \
          F, c->name, what, (buf)->name);                                                          \
    CHECK((off) % 4u == 0 && (off) <= (buf)->size && (bytes) <= (buf)->size - (off),               \
          "%s(\"%s\"): %s range [%llu, +%llu) is outside \"%s\" (%llu bytes) or misaligned", F,    \
          c->name, what, (unsigned long long)(off), (unsigned long long)(bytes), (buf)->name,      \
          (unsigned long long)(buf)->size)

void de_gpu_cmd_draw_indexed_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset,
                                      de_gpu_buffer count, u64 count_offset, u32 max_draws) {
    static const char F[] = "de_gpu_cmd_draw_indexed_indirect";
    CMD_ENTER(F);
    CHECK(c->in_render, "%s(\"%s\"): no render pass is open", F, c->name);
    CHECK(c->has_pipeline && c->pipeline_kind == DE_PIPE_GRAPHICS,
          "%s(\"%s\"): no graphics pipeline is bound", F, c->name);
    CMD_RESOLVE(a, de__gpu_buffer, DE__GPU_BUFFER, args.id, F);
    de__gpu_buffer *n = NULL;
    if (count.id) {
        CMD_RESOLVE(cb, de__gpu_buffer, DE__GPU_BUFFER, count.id, F);
        n = cb;
        CHECK_INDIRECT(n, count_offset, 4u, "count");
    }
    CHECK_INDIRECT(a, offset, (u64)max_draws * 20u, "args"); /* 5 u32 per indexed draw */
    if (c->device->vt->cmd_draw_indexed_indirect)
        c->device->vt->cmd_draw_indexed_indirect(c, a, offset, n, count_offset, max_draws);
}

void de_gpu_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset,
                                       de_gpu_buffer count, u64 count_offset, u32 max) {
    static const char F[] = "de_gpu_cmd_dispatch_mesh_indirect";
    CMD_ENTER(F);
    CHECK(c->in_render, "%s(\"%s\"): no render pass is open", F, c->name);
    CHECK(c->has_pipeline && c->pipeline_kind == DE_PIPE_MESH,
          "%s(\"%s\"): no mesh pipeline is bound", F, c->name);
    CMD_RESOLVE(a, de__gpu_buffer, DE__GPU_BUFFER, args.id, F);
    de__gpu_buffer *n = NULL;
    if (count.id) {
        CMD_RESOLVE(cb, de__gpu_buffer, DE__GPU_BUFFER, count.id, F);
        n = cb;
        CHECK_INDIRECT(n, count_offset, 4u, "count");
    }
    CHECK_INDIRECT(a, offset, (u64)max * 12u, "args"); /* 3 u32 per mesh dispatch */
    if (c->device->vt->cmd_dispatch_mesh_indirect)
        c->device->vt->cmd_dispatch_mesh_indirect(c, a, offset, n, count_offset, max);
}

void de_gpu_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    static const char F[] = "de_gpu_cmd_dispatch";
    CMD_ENTER(F);
    CHECK(!c->in_render, "%s(\"%s\"): dispatch inside a render pass", F, c->name);
    CHECK(c->has_pipeline && c->pipeline_kind == DE_PIPE_COMPUTE,
          "%s(\"%s\"): no compute pipeline is bound", F, c->name);
    if (c->device->vt->cmd_dispatch)
        c->device->vt->cmd_dispatch(c, x, y, z);
}

void de_gpu_cmd_dispatch_indirect(de_gpu_cmd *c, de_gpu_buffer args, u64 offset) {
    static const char F[] = "de_gpu_cmd_dispatch_indirect";
    CMD_ENTER(F);
    CHECK(!c->in_render, "%s(\"%s\"): dispatch inside a render pass", F, c->name);
    CHECK(c->has_pipeline && c->pipeline_kind == DE_PIPE_COMPUTE,
          "%s(\"%s\"): no compute pipeline is bound", F, c->name);
    CMD_RESOLVE(a, de__gpu_buffer, DE__GPU_BUFFER, args.id, F);
    CHECK_INDIRECT(a, offset, 12u, "args");
    if (c->device->vt->cmd_dispatch_indirect)
        c->device->vt->cmd_dispatch_indirect(c, a, offset);
}

/* Ray tracing (S16): refused in every build, nothing is recorded. */
static void rt_unsupported(de_gpu_cmd *c, const char *f) {
    if (c)
        de__gpu_fail(c->device, DE_ERR_UNSUPPORTED, "%s: ray tracing is not available before S16",
                     f);
}

void de_gpu_cmd_dispatch_rays(de_gpu_cmd *c, u32 w, u32 h, u32 depth) {
    (void)w;
    (void)h;
    (void)depth;
    rt_unsupported(c, "de_gpu_cmd_dispatch_rays");
}

void de_gpu_cmd_build_blas(de_gpu_cmd *c, de_gpu_accel blas, bool update) {
    (void)blas;
    (void)update;
    rt_unsupported(c, "de_gpu_cmd_build_blas");
}

void de_gpu_cmd_build_tlas(de_gpu_cmd *c, de_gpu_accel tlas, de_gpu_buffer instances, u32 count,
                           bool update) {
    (void)tlas;
    (void)instances;
    (void)count;
    (void)update;
    rt_unsupported(c, "de_gpu_cmd_build_tlas");
}

void de_gpu_cmd_copy_buffer(de_gpu_cmd *c, de_gpu_buffer dst, u64 dst_off, de_gpu_buffer src,
                            u64 src_off, u64 bytes) {
    static const char F[] = "de_gpu_cmd_copy_buffer";
    CMD_ENTER(F);
    CHECK(!c->in_render, "%s(\"%s\"): copy inside a render pass", F, c->name);
    CMD_RESOLVE(db, de__gpu_buffer, DE__GPU_BUFFER, dst.id, F);
    CMD_RESOLVE(sb, de__gpu_buffer, DE__GPU_BUFFER, src.id, F);
    CHECK(db->usage & DE_USAGE_COPY_DST, "%s(\"%s\"): dst \"%s\" lacks DE_USAGE_COPY_DST", F,
          c->name, db->name);
    CHECK(sb->usage & DE_USAGE_COPY_SRC, "%s(\"%s\"): src \"%s\" lacks DE_USAGE_COPY_SRC", F,
          c->name, sb->name);
    CHECK(dst_off <= db->size && bytes <= db->size - dst_off && src_off <= sb->size &&
              bytes <= sb->size - src_off,
          "%s(\"%s\"): %llu bytes at dst %llu / src %llu run past \"%s\" (%llu) or \"%s\" (%llu)",
          F, c->name, (unsigned long long)bytes, (unsigned long long)dst_off,
          (unsigned long long)src_off, db->name, (unsigned long long)db->size, sb->name,
          (unsigned long long)sb->size);
    CHECK(db != sb || dst_off + bytes <= src_off || src_off + bytes <= dst_off,
          "%s(\"%s\"): source and destination ranges overlap", F, c->name);
    if (bytes && c->device->vt->cmd_copy_buffer)
        c->device->vt->cmd_copy_buffer(c, db, dst_off, sb, src_off, bytes);
}

/* Portable buffer->texture copy layout (D3D12's placed-footprint rules, the strictest). */
#define COPY_ROW_PITCH_ALIGN 256u
#define COPY_OFFSET_ALIGN    512u

void de_gpu_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de_gpu_texture dst, u32 mip, u32 layer,
                                       de_gpu_buffer src, u64 src_off, u32 row_pitch) {
    static const char F[] = "de_gpu_cmd_copy_buffer_to_texture";
    CMD_ENTER(F);
    CHECK(!c->in_render, "%s(\"%s\"): copy inside a render pass", F, c->name);
    CMD_RESOLVE(t, de__gpu_texture, DE__GPU_TEXTURE, dst.id, F);
    CMD_RESOLVE(sb, de__gpu_buffer, DE__GPU_BUFFER, src.id, F);
    CHECK(t->usage & DE_USAGE_COPY_DST, "%s(\"%s\"): dst \"%s\" lacks DE_USAGE_COPY_DST", F,
          c->name, t->name);
    CHECK(sb->usage & DE_USAGE_COPY_SRC, "%s(\"%s\"): src \"%s\" lacks DE_USAGE_COPY_SRC", F,
          c->name, sb->name);
    CHECK(mip < t->mip_count, "%s(\"%s\"): mip %u, but \"%s\" has %u", F, c->name, mip, t->name,
          t->mip_count);
    CHECK(t->dim == DE_TEX_3D || layer < t->depth_or_layers,
          "%s(\"%s\"): layer %u, but \"%s\" has %u", F, c->name, layer, t->name,
          t->depth_or_layers);
#if DE__GPU_VALIDATE_COMMANDS
    {
        const bool bc = de__gpu_format_is_compressed(t->format);
        u32 w = t->width >> mip, h = t->height >> mip;
        w = w ? w : 1u;
        h = h ? h : 1u;
        u32 dz = 1u;
        if (t->dim == DE_TEX_3D) {
            dz = t->depth_or_layers >> mip;
            dz = dz ? dz : 1u;
        }
        const u32 cols = bc ? (w + 3u) / 4u : w;
        const u32 rows = bc ? (h + 3u) / 4u : h;
        const u64 row_bytes = (u64)cols * de__gpu_format_block_bytes(t->format);
        CHECK(row_pitch % COPY_ROW_PITCH_ALIGN == 0 && row_pitch >= row_bytes,
              "%s(\"%s\"): row_pitch %u must be a multiple of %u and at least %llu", F, c->name,
              row_pitch, COPY_ROW_PITCH_ALIGN, (unsigned long long)row_bytes);
        CHECK(src_off % COPY_OFFSET_ALIGN == 0, "%s(\"%s\"): src_off %llu is not a multiple of %u",
              F, c->name, (unsigned long long)src_off, COPY_OFFSET_ALIGN);
        const u64 need = (u64)row_pitch * rows * dz;
        CHECK(src_off <= sb->size && need <= sb->size - src_off,
              "%s(\"%s\"): mip %u needs %llu bytes at %llu; \"%s\" has %llu", F, c->name, mip,
              (unsigned long long)need, (unsigned long long)src_off, sb->name,
              (unsigned long long)sb->size);
    }
#endif
    if (c->device->vt->cmd_copy_buffer_to_texture)
        c->device->vt->cmd_copy_buffer_to_texture(c, t, mip, layer, sb, src_off, row_pitch);
}

void de_gpu_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color) {
    CMD_ENTER("de_gpu_cmd_marker_push");
    ++c->marker_depth;
    if (c->device->vt->cmd_marker_push)
        c->device->vt->cmd_marker_push(c, name ? name : "", color);
}

void de_gpu_cmd_marker_pop(de_gpu_cmd *c) {
    static const char F[] = "de_gpu_cmd_marker_pop";
    CMD_ENTER(F);
    CHECK(c->marker_depth > 0, "%s(\"%s\"): no marker_push to pop", F, c->name);
    if (c->marker_depth > 0)
        --c->marker_depth;
    if (c->device->vt->cmd_marker_pop)
        c->device->vt->cmd_marker_pop(c);
}

void de_gpu_cmd_breadcrumb(de_gpu_cmd *c, u32 id) {
    CMD_ENTER("de_gpu_cmd_breadcrumb");
    if (c->device->vt->cmd_breadcrumb)
        c->device->vt->cmd_breadcrumb(c, id);
}

void de_gpu_cmd_timestamp(de_gpu_cmd *c, de_gpu_query_heap h, u32 index) {
    static const char F[] = "de_gpu_cmd_timestamp";
    CMD_ENTER(F);
    CMD_RESOLVE(q, de__gpu_query_heap, DE__GPU_QUERY_HEAP, h.id, F);
    CHECK(index < q->count, "%s(\"%s\"): query %u, but \"%s\" has %u", F, c->name, index, q->name,
          q->count);
    if (c->device->vt->cmd_timestamp)
        c->device->vt->cmd_timestamp(c, q, index);
}

void de_gpu_cmd_resolve_queries(de_gpu_cmd *c, de_gpu_query_heap h, u32 first, u32 count,
                                de_gpu_buffer dst, u64 dst_off) {
    static const char F[] = "de_gpu_cmd_resolve_queries";
    CMD_ENTER(F);
    CHECK(!c->in_render, "%s(\"%s\"): resolve inside a render pass", F, c->name);
    CMD_RESOLVE(q, de__gpu_query_heap, DE__GPU_QUERY_HEAP, h.id, F);
    CMD_RESOLVE(db, de__gpu_buffer, DE__GPU_BUFFER, dst.id, F);
    CHECK(first <= q->count && count <= q->count - first,
          "%s(\"%s\"): queries [%u, +%u) are outside \"%s\" (%u)", F, c->name, first, count,
          q->name, q->count);
    CHECK(db->usage & DE_USAGE_COPY_DST, "%s(\"%s\"): dst \"%s\" lacks DE_USAGE_COPY_DST", F,
          c->name, db->name);
    CHECK(dst_off % 8u == 0 && dst_off <= db->size && (u64)count * 8u <= db->size - dst_off,
          "%s(\"%s\"): %u u64 values at %llu do not fit \"%s\" (%llu bytes) or are misaligned", F,
          c->name, count, (unsigned long long)dst_off, db->name, (unsigned long long)db->size);
    if (count && c->device->vt->cmd_resolve_queries)
        c->device->vt->cmd_resolve_queries(c, q, first, count, db, dst_off);
}
