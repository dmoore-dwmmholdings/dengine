/* gpu_heap.c — device memory heaps, the per-class sub-allocator and transient regions
 * (DENG-282). See gpu_heap.h for the model and the backend contract.
 */
#include <dengine/de_debug.h>

#include "gpu/gpu_backend.h"
#include "gpu/memory/gpu_heap.h"
#include "gpu/memory/gpu_tlsf.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

#define GRAN           DE__GPU_TLSF_GRANULARITY
#define CLASS_NAME_MAX 32u
#define KIND_COUNT     3u /* de_gpu_memory values */

typedef struct heap_class {
    de_mutex lock;
    de__gpu_mem_class_desc desc; /* defaults applied; desc.name points at name_buf */
    char name_buf[CLASS_NAME_MAX];
    de__gpu_heap **heaps; /* table with NULL holes; de__gpu_heap.index is the slot */
    u32 heap_count, heap_cap;
    u32 empty; /* block heaps without allocations */
    u32 block_heaps, dedicated_heaps, allocations;
    u64 used, reserved;
} heap_class;

struct de__gpu_transient_region {
    de_gpu_device *d;
    de__gpu_alloc alloc;
    u64 bytes;
    u32 cls;
    de_atomic_u32 refs;   /* 1 for the creator until destroy + 1 per placed resource */
    de_atomic_u32 placed; /* placed resources not yet reclaimed */
    de_atomic_u32 destroyed;
    de__gpu_transient_region *prev, *next; /* device list, for shutdown */
};

typedef struct de__gpu_heaps {
    u32 class_count;
    heap_class classes[DE__GPU_MEM_CLASS_MAX];
    de_atomic_u64 tag_used[DE_MEM_TAG_MAX];
    de_atomic_u64 kind_used[KIND_COUNT];
    de_atomic_u64 kind_reserved[KIND_COUNT];
    de_mutex region_lock;
    de__gpu_transient_region *regions;
} de__gpu_heaps;

static const char *const k_kind_name[KIND_COUNT] = {"gpu_only", "upload", "readback"};
#ifdef DE_PROFILE
/* Profiler plot names must outlive every plot call: static strings. */
static const char *const k_plot_used[KIND_COUNT] = {"gpu.mem.gpu_only.used", "gpu.mem.upload.used",
                                                    "gpu.mem.readback.used"};
static const char *const k_plot_reserved[KIND_COUNT] = {
    "gpu.mem.gpu_only.reserved", "gpu.mem.upload.reserved", "gpu.mem.readback.reserved"};
#endif

static inline u64 align_up(u64 v, u64 a) {
    return (v + (a - 1)) & ~(a - 1);
}

static inline bool is_pow2(u64 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---------- Classes ---------- */

static void set_class_desc(heap_class *c, const de__gpu_mem_class_desc *desc, u32 cls) {
    c->desc = *desc;
    if ((u32)c->desc.memory >= KIND_COUNT)
        c->desc.memory = DE_MEM_GPU_ONLY;
    const char *name = desc->name;
    char fallback[CLASS_NAME_MAX];
    if (!name || !name[0]) {
        if (cls < DE__GPU_MEM_CLASS_BASE_COUNT)
            name = k_kind_name[cls];
        else {
            snprintf(fallback, sizeof fallback, "%s.%u", k_kind_name[c->desc.memory], cls);
            name = fallback;
        }
    }
    size_t n = strlen(name);
    if (n >= CLASS_NAME_MAX)
        n = CLASS_NAME_MAX - 1;
    memmove(c->name_buf, name, n);
    c->name_buf[n] = '\0';
    c->desc.name = c->name_buf;
    c->desc.block_size =
        align_up(desc->block_size ? desc->block_size : DE__GPU_HEAP_DEFAULT_BLOCK_SIZE, GRAN);
    if (c->desc.block_size > DE__GPU_TLSF_MAX_CAPACITY)
        c->desc.block_size = DE__GPU_TLSF_MAX_CAPACITY;
    c->desc.dedicated_threshold =
        desc->dedicated_threshold ? desc->dedicated_threshold : DE__GPU_HEAP_DEFAULT_DEDICATED_SIZE;
}

de_result de__gpu_mem_add_class(de_gpu_device *d, const de__gpu_mem_class_desc *desc,
                                u32 *out_class) {
    if (!d || !d->heaps || !desc || !out_class)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_mem_add_class: NULL argument");
    de__gpu_heaps *hs = d->heaps;
    if (hs->class_count >= DE__GPU_MEM_CLASS_MAX)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                            "de__gpu_mem_add_class: all %u memory classes are in use",
                            DE__GPU_MEM_CLASS_MAX);
    const u32 cls = hs->class_count;
    heap_class *c = &hs->classes[cls];
    de_mutex_init(&c->lock);
    set_class_desc(c, desc, cls);
    hs->class_count = cls + 1;
    *out_class = cls;
    return DE_OK;
}

de_result de__gpu_mem_configure_class(de_gpu_device *d, u32 cls,
                                      const de__gpu_mem_class_desc *desc) {
    if (!d || !d->heaps || !desc || cls >= d->heaps->class_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__gpu_mem_configure_class: bad device, desc or class %u", cls);
    heap_class *c = &d->heaps->classes[cls];
    if (c->heap_count)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "de__gpu_mem_configure_class: class \"%s\" already has heaps",
                            c->name_buf);
    set_class_desc(c, desc, cls);
    return DE_OK;
}

u32 de__gpu_mem_class_count(de_gpu_device *d) {
    return d && d->heaps ? d->heaps->class_count : 0;
}

const de__gpu_mem_class_desc *de__gpu_mem_class_get(de_gpu_device *d, u32 cls) {
    if (!d || !d->heaps || cls >= d->heaps->class_count)
        return NULL;
    return &d->heaps->classes[cls].desc;
}

/* ---------- Heaps ---------- */

static size_t heap_bytes(const de_gpu_device *d) {
    return DE__GPU_ALIGN16(sizeof(de__gpu_heap)) + d->vt->heap_payload_size;
}

static void account_reserved(de_gpu_device *d, const heap_class *c, u64 bytes, bool add) {
    de_atomic_u64 *r = &d->heaps->kind_reserved[c->desc.memory];
    u64 now = add ? de_atomic_fetch_add_u64(r, bytes, DE_ATOMIC_RELAXED) + bytes
                  : de_atomic_fetch_sub_u64(r, bytes, DE_ATOMIC_RELAXED) - bytes;
#ifdef DE_PROFILE
    DE_PROFILE_PLOT(k_plot_reserved[c->desc.memory], now);
#endif
    (void)now;
}

static void account_used(de_gpu_device *d, const heap_class *c, u32 tag, u64 bytes, bool add) {
    de__gpu_heaps *hs = d->heaps;
    de_atomic_u64 *u = &hs->kind_used[c->desc.memory];
    u64 now;
    if (add) {
        de_atomic_fetch_add_u64(&hs->tag_used[tag], bytes, DE_ATOMIC_RELAXED);
        now = de_atomic_fetch_add_u64(u, bytes, DE_ATOMIC_RELAXED) + bytes;
    } else {
        de_atomic_fetch_sub_u64(&hs->tag_used[tag], bytes, DE_ATOMIC_RELAXED);
        now = de_atomic_fetch_sub_u64(u, bytes, DE_ATOMIC_RELAXED) - bytes;
    }
#ifdef DE_PROFILE
    DE_PROFILE_PLOT(k_plot_used[c->desc.memory], now);
#endif
    (void)now;
}

/* A free slot in the class's table (grows it), or U32_MAX without memory. Under the lock. */
static u32 heap_slot(de_gpu_device *d, heap_class *c) {
    for (u32 i = 0; i < c->heap_count; ++i)
        if (!c->heaps[i])
            return i;
    if (c->heap_count == c->heap_cap) {
        u32 cap = c->heap_cap ? c->heap_cap * 2 : 8;
        de__gpu_heap **t =
            (de__gpu_heap **)DE_REALLOC(d->alloc, c->heaps, sizeof *t * c->heap_cap,
                                        sizeof *t * cap, DE_ALIGNOF(void *), DE_MEM_RENDER);
        if (!t)
            return UINT32_MAX;
        c->heaps = t;
        c->heap_cap = cap;
    }
    c->heaps[c->heap_count] = NULL;
    return c->heap_count++;
}

/* Creates a heap of `size` bytes through the backend and files it. Under the lock. */
static de_result heap_new(de_gpu_device *d, heap_class *c, u32 cls, u64 size, bool dedicated,
                          de__gpu_heap **out) {
    *out = NULL;
    const u32 slot = heap_slot(d, c);
    if (slot == UINT32_MAX)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "gpu heap: class \"%s\" heap table",
                            c->name_buf);
    const size_t bytes = heap_bytes(d);
    de__gpu_heap *h = (de__gpu_heap *)DE_ALLOC(d->alloc, bytes, 16, DE_MEM_RENDER);
    if (!h)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "gpu heap: class \"%s\" heap record",
                            c->name_buf);
    memset(h, 0, bytes);
    h->mem_class = cls;
    h->index = slot;
    h->size = size;
    h->dedicated = dedicated;
    if (!dedicated) {
        de_result r = de__gpu_tlsf_create(d->alloc, size, &h->tlsf);
        if (r != DE_OK) {
            DE_FREE(d->alloc, h, bytes);
            return r;
        }
    }
    de_result r = d->vt->heap_create(d, h, &c->desc);
    if (r != DE_OK) {
        de__gpu_tlsf_destroy(h->tlsf);
        DE_FREE(d->alloc, h, bytes);
        return de__gpu_fail(d, r,
                            "gpu heap: the %s backend could not create a %llu-byte %sheap "
                            "of class \"%s\"",
                            d->vt->name, (unsigned long long)size, dedicated ? "dedicated " : "",
                            c->name_buf);
    }
    c->heaps[slot] = h;
    c->reserved += size;
    if (dedicated)
        ++c->dedicated_heaps;
    else
        ++c->block_heaps;
    account_reserved(d, c, size, true);
    *out = h;
    return DE_OK;
}

/* Gives a heap back to the backend and drops it from the table. Under the lock. */
static void heap_release(de_gpu_device *d, heap_class *c, de__gpu_heap *h, bool backend_alive) {
    if (backend_alive && d->vt->heap_destroy)
        d->vt->heap_destroy(d, h);
    c->heaps[h->index] = NULL;
    while (c->heap_count && !c->heaps[c->heap_count - 1])
        --c->heap_count;
    c->reserved -= h->size;
    if (h->dedicated)
        --c->dedicated_heaps;
    else
        --c->block_heaps;
    account_reserved(d, c, h->size, false);
    de__gpu_tlsf_destroy(h->tlsf);
    DE_FREE(d->alloc, h, heap_bytes(d));
}

/* ---------- Allocation ---------- */

de_result de__gpu_mem_alloc(de_gpu_device *d, u32 cls, u64 size, u64 align, u32 tag,
                            de__gpu_alloc *out) {
    if (!d || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_mem_alloc: NULL argument");
    memset(out, 0, sizeof *out);
    de__gpu_heaps *hs = d->heaps;
    if (cls >= hs->class_count)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "de__gpu_mem_alloc: memory class %u does not exist (%u classes)", cls,
                            hs->class_count);
    if (align == 0)
        align = GRAN;
    if (!is_pow2(align))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "de__gpu_mem_alloc: alignment %llu is not a power of two",
                            (unsigned long long)align);
    if (size == 0 || size > DE__GPU_TLSF_MAX_CAPACITY)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "de__gpu_mem_alloc: size %llu is outside 1 B .. 1 TiB",
                            (unsigned long long)size);
    if (!d->vt->heap_create)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "de__gpu_mem_alloc: the %s backend has no heaps",
                            d->vt->name);
    if (tag >= DE_MEM_TAG_MAX)
        tag = DE_MEM_GENERAL;
    if (align < GRAN)
        align = GRAN;

    heap_class *c = &hs->classes[cls];
    const u64 rounded = align_up(size, GRAN);
    de__gpu_heap *h = NULL;
    de__gpu_tlsf_block *blk = NULL;
    u64 offset = 0;
    de_result r = DE_OK;

    de_mutex_lock(&c->lock);
    if (size > c->desc.dedicated_threshold) {
        /* Its own heap; backends align a heap's start to every placement alignment. */
        r = heap_new(d, c, cls, rounded, true, &h);
    } else {
        for (u32 i = 0; i < c->heap_count && !blk; ++i) {
            de__gpu_heap *e = c->heaps[i];
            if (e && !e->dedicated) {
                blk = de__gpu_tlsf_alloc(e->tlsf, rounded, align, &offset);
                h = blk ? e : NULL;
            }
        }
        if (!blk) {
            /* A new block heap; as large as the request when that exceeds the block size (the
             * heap starts at offset 0, aligned for every placement, and the TLSF's exact-fit
             * fallback serves a request of the whole range). */
            u64 hsize = rounded > c->desc.block_size ? rounded : c->desc.block_size;
            r = heap_new(d, c, cls, hsize, false, &h);
            if (r == DE_OK) {
                ++c->empty;
                blk = de__gpu_tlsf_alloc(h->tlsf, rounded, align, &offset);
                if (!blk) {
                    --c->empty;
                    heap_release(d, c, h, true);
                    h = NULL;
                    r = de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                                     "de__gpu_mem_alloc: no block nodes for class \"%s\"",
                                     c->name_buf);
                }
            }
        }
        if (h && h->live == 0)
            --c->empty;
    }
    if (r == DE_OK) {
        ++h->live;
        ++c->allocations;
        c->used += rounded;
        out->heap = h;
        out->offset = offset;
        out->size = rounded;
        out->heap_index = h->index;
        out->mem_class = (u16)cls;
        out->tag = (u8)tag;
        out->dedicated = h->dedicated ? 1 : 0;
        out->block = blk;
    }
    de_mutex_unlock(&c->lock);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de__gpu_mem_alloc: %llu bytes (align %llu) of class \"%s\"",
                            (unsigned long long)size, (unsigned long long)align, c->name_buf);
    account_used(d, c, tag, rounded, true);
    return DE_OK;
}

void de__gpu_mem_free(de_gpu_device *d, de__gpu_alloc *a) {
    if (!d || !a || !a->heap)
        return;
    heap_class *c = &d->heaps->classes[a->mem_class];
    de__gpu_heap *h = a->heap;
    de_mutex_lock(&c->lock);
    DE_ASSERT(h->live > 0 && c->heaps[h->index] == h);
    --h->live;
    --c->allocations;
    c->used -= a->size;
    if (h->dedicated) {
        heap_release(d, c, h, true);
    } else {
        de__gpu_tlsf_free(h->tlsf, a->block);
        if (h->live == 0) {
            /* Keep one empty heap per class as the spare; release any beyond it. */
            if (c->empty >= 1)
                heap_release(d, c, h, true);
            else
                ++c->empty;
        }
    }
    de_mutex_unlock(&c->lock);
    account_used(d, c, a->tag, a->size, false);
    memset(a, 0, sizeof *a);
}

void *de__gpu_mem_mapped(const de__gpu_alloc *a) {
    if (!a || !a->heap || !a->heap->mapped)
        return NULL;
    return (u8 *)a->heap->mapped + a->offset;
}

/* ---------- Statistics ---------- */

de_result de__gpu_mem_class_stats(de_gpu_device *d, u32 cls, de__gpu_mem_stats *out) {
    if (!d || !out || !d->heaps || cls >= d->heaps->class_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_mem_class_stats: bad device or class %u",
                       cls);
    memset(out, 0, sizeof *out);
    heap_class *c = &d->heaps->classes[cls];
    u64 block_free = 0;
    de_mutex_lock(&c->lock);
    out->used = c->used;
    out->reserved = c->reserved;
    out->free = c->reserved - c->used;
    out->allocations = c->allocations;
    out->heaps = c->block_heaps;
    out->dedicated_heaps = c->dedicated_heaps;
    out->empty_heaps = c->empty;
    for (u32 i = 0; i < c->heap_count; ++i) {
        const de__gpu_heap *h = c->heaps[i];
        if (!h || h->dedicated)
            continue;
        de__gpu_tlsf_stats ts;
        de__gpu_tlsf_get_stats(h->tlsf, &ts);
        block_free += ts.free;
        out->free_blocks += ts.free_blocks;
        if (ts.largest_free > out->largest_free)
            out->largest_free = ts.largest_free;
    }
    de_mutex_unlock(&c->lock);
    if (block_free && out->largest_free < block_free)
        out->fragmentation = 1.0f - (f32)((double)out->largest_free / (double)block_free);
    return DE_OK;
}

u64 de__gpu_mem_tag_used(de_gpu_device *d, u32 tag) {
    if (!d || !d->heaps || tag >= DE_MEM_TAG_MAX)
        return 0;
    return de_atomic_load_u64(&d->heaps->tag_used[tag], DE_ATOMIC_RELAXED);
}

u64 de__gpu_mem_kind_used(de_gpu_device *d, de_gpu_memory memory) {
    if (!d || !d->heaps || (u32)memory >= KIND_COUNT)
        return 0;
    return de_atomic_load_u64(&d->heaps->kind_used[memory], DE_ATOMIC_RELAXED);
}

u64 de__gpu_mem_kind_reserved(de_gpu_device *d, de_gpu_memory memory) {
    if (!d || !d->heaps || (u32)memory >= KIND_COUNT)
        return 0;
    return de_atomic_load_u64(&d->heaps->kind_reserved[memory], DE_ATOMIC_RELAXED);
}

/* ---------- Requirements ---------- */

de_result de__gpu_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                      de__gpu_mem_req *out) {
    if (!d || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_buffer_requirements: NULL argument");
    memset(out, 0, sizeof *out);
    de_result r = de__gpu_validate_buffer_desc(d, desc);
    if (r != DE_OK)
        return r;
    if (!d->vt->buffer_requirements)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de__gpu_buffer_requirements: the %s backend cannot tell", d->vt->name);
    return d->vt->buffer_requirements(d, desc, out);
}

/* The desc with the defaults the front-end applies (height, layers, mips, samples). */
static de_gpu_texture_desc texture_defaults(const de_gpu_texture_desc *desc) {
    de_gpu_texture_desc nd = *desc;
    if (!nd.height)
        nd.height = 1;
    if (!nd.depth_or_layers)
        nd.depth_or_layers = nd.dim == DE_TEX_CUBE ? 6u : 1u;
    if (!nd.mip_count)
        nd.mip_count = 1;
    if (!nd.sample_count)
        nd.sample_count = 1;
    return nd;
}

de_result de__gpu_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                       de__gpu_mem_req *out) {
    if (!d || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_texture_requirements: NULL argument");
    memset(out, 0, sizeof *out);
    de_result r = de__gpu_validate_texture_desc(d, desc);
    if (r != DE_OK)
        return r;
    if (!d->vt->texture_requirements)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de__gpu_texture_requirements: the %s backend cannot tell",
                            d->vt->name);
    de_gpu_texture_desc nd = texture_defaults(desc);
    return d->vt->texture_requirements(d, &nd, out);
}

/* ---------- Transient regions ---------- */

static void region_free(de__gpu_transient_region *r) {
    de_gpu_device *d = r->d;
    de__gpu_heaps *hs = d->heaps;
    de_mutex_lock(&hs->region_lock);
    if (r->prev)
        r->prev->next = r->next;
    else
        hs->regions = r->next;
    if (r->next)
        r->next->prev = r->prev;
    de_mutex_unlock(&hs->region_lock);
    de__gpu_mem_free(d, &r->alloc);
    DE_FREE(d->alloc, r, sizeof *r);
}

static void region_unref(de__gpu_transient_region *r) {
    if (de_atomic_fetch_sub_u32(&r->refs, 1, DE_ATOMIC_ACQ_REL) == 1)
        region_free(r);
}

de_result de__gpu_transient_region_create(de_gpu_device *d, u32 cls, u64 bytes,
                                          de__gpu_transient_region **out) {
    if (!d || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_transient_region_create: NULL argument");
    *out = NULL;
    if (bytes == 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "de__gpu_transient_region_create: bytes is 0");
    de__gpu_transient_region *r = (de__gpu_transient_region *)DE_ALLOC(
        d->alloc, sizeof *r, DE_ALIGNOF(de__gpu_transient_region), DE_MEM_RENDER);
    if (!r)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "de__gpu_transient_region_create: record");
    memset(r, 0, sizeof *r);
    de_result res =
        de__gpu_mem_alloc(d, cls, bytes, DE__GPU_TRANSIENT_REGION_ALIGN, DE_MEM_RENDER, &r->alloc);
    if (res != DE_OK) {
        DE_FREE(d->alloc, r, sizeof *r);
        return DE_FAIL_WRAP(res, "de__gpu_transient_region_create: %llu bytes",
                            (unsigned long long)bytes);
    }
    r->d = d;
    r->bytes = bytes;
    r->cls = cls;
    de_atomic_init_u32(&r->refs, 1);
    de_atomic_init_u32(&r->placed, 0);
    de_atomic_init_u32(&r->destroyed, 0);
    de__gpu_heaps *hs = d->heaps;
    de_mutex_lock(&hs->region_lock);
    r->next = hs->regions;
    if (hs->regions)
        hs->regions->prev = r;
    hs->regions = r;
    de_mutex_unlock(&hs->region_lock);
    *out = r;
    return DE_OK;
}

void de__gpu_transient_region_destroy(de__gpu_transient_region *r) {
    if (!r)
        return;
    if (de_atomic_exchange_u32(&r->destroyed, 1, DE_ATOMIC_ACQ_REL) != 0)
        return; /* destroyed twice */
    region_unref(r);
}

void de__gpu_transient_region_release(de__gpu_transient_region *r) {
    de_atomic_fetch_sub_u32(&r->placed, 1, DE_ATOMIC_RELAXED);
    region_unref(r);
}

u64 de__gpu_transient_region_size(const de__gpu_transient_region *r) {
    return r ? r->bytes : 0;
}

u32 de__gpu_transient_region_class(const de__gpu_transient_region *r) {
    return r ? r->cls : 0;
}

const de__gpu_alloc *de__gpu_transient_region_alloc(const de__gpu_transient_region *r) {
    return r ? &r->alloc : NULL;
}

u32 de__gpu_transient_region_placed(const de__gpu_transient_region *r) {
    return r ? de_atomic_load_u32(&r->placed, DE_ATOMIC_RELAXED) : 0;
}

/* Checks a placement and takes a region reference for it. */
static de_result place_check(de__gpu_transient_region *r, u64 offset, const de__gpu_mem_req *req,
                             const char *func, const char *name) {
    de_gpu_device *d = r->d;
    if (!name)
        name = "";
    if (de_atomic_load_u32(&r->destroyed, DE_ATOMIC_ACQUIRE))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): the region is destroyed", func,
                            name);
    if (!(req->class_mask & (1u << r->cls)))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): the resource cannot live in memory class \"%s\" (class "
                            "mask 0x%x)",
                            func, name, d->heaps->classes[r->cls].name_buf, req->class_mask);
    const u64 at = r->alloc.offset + offset;
    if (req->align && (at & (req->align - 1)) != 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): offset %llu is not aligned to %llu bytes", func, name,
                            (unsigned long long)offset, (unsigned long long)req->align);
    if (offset > r->bytes || req->size > r->bytes - offset)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %llu bytes at offset %llu end past the region (%llu "
                            "bytes)",
                            func, name, (unsigned long long)req->size, (unsigned long long)offset,
                            (unsigned long long)r->bytes);
    de_atomic_fetch_add_u32(&r->refs, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&r->placed, 1, DE_ATOMIC_RELAXED);
    return DE_OK;
}

de_gpu_buffer de__gpu_place_transient_buffer(de__gpu_transient_region *r, u64 offset,
                                             const de_gpu_buffer_desc *desc) {
    static const char F[] = "de__gpu_place_transient_buffer";
    de_gpu_buffer h = {0};
    if (!r) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: region is NULL", F);
        return h;
    }
    de__gpu_mem_req req;
    if (de__gpu_buffer_requirements(r->d, desc, &req) != DE_OK)
        return h;
    if (place_check(r, offset, &req, F, desc->name) != DE_OK)
        return h;
    h = de__gpu_buffer_create_placed(r->d, desc, r->alloc.heap, r->alloc.offset + offset, r);
    if (!h.id)
        de__gpu_transient_region_release(r);
    return h;
}

de_gpu_texture de__gpu_place_transient_texture(de__gpu_transient_region *r, u64 offset,
                                               const de_gpu_texture_desc *desc) {
    static const char F[] = "de__gpu_place_transient_texture";
    de_gpu_texture h = {0};
    if (!r) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: region is NULL", F);
        return h;
    }
    de__gpu_mem_req req;
    if (de__gpu_texture_requirements(r->d, desc, &req) != DE_OK)
        return h;
    if (place_check(r, offset, &req, F, desc->name) != DE_OK)
        return h;
    de_gpu_texture_desc td = *desc;
    td.transient = true;
    h = de__gpu_texture_create_placed(r->d, &td, r->alloc.heap, r->alloc.offset + offset, r);
    if (!h.id)
        de__gpu_transient_region_release(r);
    return h;
}

/* ---------- Front-end hooks ---------- */

de_result de__gpu_heaps_init(de_gpu_device *d) {
    de__gpu_heaps *hs =
        (de__gpu_heaps *)DE_ALLOC(d->alloc, sizeof *hs, DE_ALIGNOF(de__gpu_heaps), DE_MEM_RENDER);
    if (!hs)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_gpu_device_create: heap manager");
    memset(hs, 0, sizeof *hs);
    for (u32 t = 0; t < DE_MEM_TAG_MAX; ++t)
        de_atomic_init_u64(&hs->tag_used[t], 0);
    for (u32 k = 0; k < KIND_COUNT; ++k) {
        de_atomic_init_u64(&hs->kind_used[k], 0);
        de_atomic_init_u64(&hs->kind_reserved[k], 0);
    }
    de_mutex_init(&hs->region_lock);
    d->heaps = hs;
    for (u32 k = 0; k < DE__GPU_MEM_CLASS_BASE_COUNT; ++k) {
        de__gpu_mem_class_desc cd = {.memory = (de_gpu_memory)k};
        u32 cls;
        de__gpu_mem_add_class(d, &cd, &cls);
        DE_ASSERT(cls == k);
    }
    return DE_OK;
}

void de__gpu_heaps_shutdown(de_gpu_device *d, bool backend_alive) {
    de__gpu_heaps *hs = d->heaps;
    if (!hs)
        return;
    /* Regions the caller never destroyed (placed resources are reclaimed by now). */
    u32 regions = 0;
    while (hs->regions) {
        de__gpu_transient_region *r = hs->regions;
        ++regions;
        region_free(r);
    }
    if (regions)
        DE_WARN(d->log, "de_gpu_device_destroy: %u transient region%s still alive; freed now",
                regions, regions == 1 ? "" : "s");
    for (u32 k = 0; k < hs->class_count; ++k) {
        heap_class *c = &hs->classes[k];
        if (c->allocations)
            DE_WARN(d->log,
                    "de_gpu_device_destroy: %u allocation%s (%llu bytes) of memory class \"%s\" "
                    "still alive; their heaps are released",
                    c->allocations, c->allocations == 1 ? "" : "s", (unsigned long long)c->used,
                    c->name_buf);
        for (u32 i = c->heap_count; i-- > 0;)
            if (c->heaps[i])
                heap_release(d, c, c->heaps[i], backend_alive);
        if (c->heaps)
            DE_FREE(d->alloc, c->heaps, sizeof *c->heaps * c->heap_cap);
        de_mutex_destroy(&c->lock);
    }
    de_mutex_destroy(&hs->region_lock);
    DE_FREE(d->alloc, hs, sizeof *hs);
    d->heaps = NULL;
}
