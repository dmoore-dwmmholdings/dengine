/*
 * rg_cache.c: the compiled-graph cache (SPEC §12.3 "compiled graphs are cached while the pass set
 * is unchanged"; DENG-172).
 *
 * Key. A byte string written from the frame's declarations in declaration order (never a hash
 * map's order): whether async compute is on; per resource its kind, the imported/exported flags,
 * the import's `current_access`, the export's final state and, for transients, the desc fields
 * (not the name: names are only for messages); per pass its name, queue, side-effect flag and its
 * accesses (resource index, bits, range, read or write). Imports are keyed by their slot (their
 * index), never by the physical handle, so a swapchain image or a per-frame history texture does
 * not change the key: their handles are bound each frame. The callbacks and user pointers are not
 * in the key either: a hit runs this frame's callbacks.
 *
 * Lookup compares the 64-bit hash, then the whole key, so a hash collision cannot reuse a wrong
 * plan. The cache holds at most r.rg_cache_size plans (default 8) and evicts the least recently
 * used. Plans and the key copy are one heap block of the graph's allocator; nothing is
 * allocated on a hit.
 */
#include "render/graph/rg_internal.h"

#include <string.h>

#define RG_CACHE_DEFAULT 8u
#define RG_CACHE_MAX     64u
#define RG_KEY_VERSION   0x52474b31u /* "RGK1" */

typedef struct key_writer {
    u8 *p;
    u32 size, cap;
} key_writer;

static void put_u32(key_writer *w, u32 v) {
    DE_ASSERT(w->size + 4u <= w->cap);
    w->p[w->size++] = (u8)v;
    w->p[w->size++] = (u8)(v >> 8);
    w->p[w->size++] = (u8)(v >> 16);
    w->p[w->size++] = (u8)(v >> 24);
}

static void put_str(key_writer *w, const char *s) {
    const u32 n = (u32)strlen(s);
    put_u32(w, n);
    DE_ASSERT(w->size + n <= w->cap);
    memcpy(w->p + w->size, s, n);
    w->size += n;
}

bool de__rg_cache_key(de_rg *g, bool async_compute, u8 **key, u32 *size, u64 *hash) {
    /* Upper bound of the key size, so the key is one arena push. */
    u64 cap = 4u * 8u + (u64)g->resource_count * 4u * 20u + (u64)g->access_count * 4u * 8u;
    for (u32 pi = 0; pi < g->pass_count; ++pi)
        cap += 4u * 6u + strlen(g->passes[pi]->name);
    if (cap > UINT32_MAX)
        return false;
    key_writer w = {de_arena_push(g->arena, (size_t)cap, 16), 0, (u32)cap};
    if (!w.p)
        return false;
    put_u32(&w, RG_KEY_VERSION);
    put_u32(&w, async_compute ? 1u : 0u);
    put_u32(&w, g->resource_count);
    put_u32(&w, g->pass_count);
    for (u32 i = 0; i < g->resource_count; ++i) {
        const de__rg_resource *r = &g->resources[i];
        put_u32(&w, (u32)r->kind);
        put_u32(&w, r->flags & (DE__RG_RES_IMPORTED | DE__RG_RES_EXPORTED));
        put_u32(&w, r->initial_access);
        put_u32(&w, r->final_access);
        if (r->flags & DE__RG_RES_IMPORTED)
            continue;
        if (r->kind == DE__RG_TEXTURE) {
            const de_gpu_texture_desc *t = &r->tex;
            put_u32(&w, (u32)t->dim);
            put_u32(&w, (u32)t->format);
            put_u32(&w, t->width);
            put_u32(&w, t->height);
            put_u32(&w, t->depth_or_layers);
            put_u32(&w, t->mip_count);
            put_u32(&w, t->sample_count);
            put_u32(&w, t->usage);
        } else {
            const de_gpu_buffer_desc *bd = &r->buf;
            put_u32(&w, (u32)bd->size);
            put_u32(&w, (u32)(bd->size >> 32));
            put_u32(&w, bd->usage);
            put_u32(&w, (u32)bd->memory);
            put_u32(&w, bd->stride);
        }
    }
    for (u32 pi = 0; pi < g->pass_count; ++pi) {
        const de_rg_pass *p = g->passes[pi];
        put_str(&w, p->name);
        put_u32(&w, (u32)p->queue);
        put_u32(&w, p->side_effect ? 1u : 0u);
        put_u32(&w, p->access_count);
        for (u32 a = p->access_first; a != DE__RG_NONE; a = g->accesses[a].pass_next) {
            const de__rg_access *x = &g->accesses[a];
            put_u32(&w, x->resource);
            put_u32(&w, x->access);
            put_u32(&w, x->range.mip_first);
            put_u32(&w, x->range.mip_end);
            put_u32(&w, x->range.layer_first);
            put_u32(&w, x->range.layer_end);
            put_u32(&w, x->write ? 1u : 0u);
        }
    }
    *key = w.p;
    *size = w.size;
    *hash = de_hash64(w.p, w.size, 0x9e3779b97f4a7c15ull);
    return true;
}

u32 de__rg_cache_limit(de_rg *g) {
    if (!DE_HANDLE_VALID(g->cache_size_cvar))
        g->cache_size_cvar = de_cvar_register(&(de_cvar_desc){
            .name = "r.rg_cache_size",
            .help = "Compiled render graphs kept per graph (LRU); a frame with the same passes and "
                    "resources as a cached one skips compilation",
            .type = DE_CVAR_INT,
            .default_number = RG_CACHE_DEFAULT,
            .min = 1,
            .max = RG_CACHE_MAX,
        });
    const i64 n = DE_HANDLE_VALID(g->cache_size_cvar) ? de_cvar_int(g->cache_size_cvar)
                                                      : (i64)RG_CACHE_DEFAULT;
    if (n < 1)
        return 1u;
    return n > (i64)RG_CACHE_MAX ? RG_CACHE_MAX : (u32)n;
}

de__rg_plan *de__rg_cache_find(de_rg *g, const u8 *key, u32 size, u64 hash) {
    for (u32 i = 0; i < g->cache_count; ++i) {
        de__rg_plan *p = g->cache[i];
        if (p->hash == hash && p->key_size == size && memcmp(p->key, key, size) == 0) {
            p->last_use = ++g->cache_clock;
            return p;
        }
    }
    return NULL;
}

/* Carves `n` elements of `elem` bytes from the plan block at 16-byte alignment. */
static void *carve(u8 **at, size_t n, size_t elem) {
    void *p = *at;
    *at += (n * elem + 15u) & ~(size_t)15u;
    return n ? p : NULL;
}

static size_t carve_size(size_t n, size_t elem) {
    return (n * elem + 15u) & ~(size_t)15u;
}

de__rg_plan *de__rg_plan_create(de_rg *g, const de__rg_cbuild *b, const u8 *key, u32 key_size,
                                u64 hash) {
    const size_t rn = g->resource_count, pn = g->pass_count;
    size_t total = carve_size(1, sizeof(de__rg_plan));
    total += carve_size(key_size, 1);
    total += carve_size(b->step_count, sizeof(de__rg_step));
    total += carve_size(pn, sizeof(u32));
    total += carve_size(b->batch_count, sizeof(de__rg_batch));
    total += carve_size(b->step_count, sizeof(u32));
    total += carve_size(b->wait_count, sizeof(de__rg_wait));
    total += carve_size(b->tb_count, sizeof(de__rg_cbarrier));
    total += carve_size(rn, sizeof(u32));
    total += carve_size(b->transient_count, sizeof(de__rg_transient));
    total += carve_size(rn, sizeof(u32));
    total += carve_size(b->transient_count, sizeof(u32));
    u8 *block = (u8 *)DE_ALLOC(g->alloc, total, 16, DE_MEM_RENDER);
    if (!block)
        return NULL;
    memset(block, 0, total);
    u8 *at = block;
    de__rg_plan *p = (de__rg_plan *)carve(&at, 1, sizeof(de__rg_plan));
    p->block_size = total;
    p->hash = hash;
    p->key_size = key_size;
    p->key = (u8 *)carve(&at, key_size, 1);
    if (key_size)
        memcpy(p->key, key, key_size);
    p->async_compute = b->async_compute;
    p->pass_count = (u32)pn;
    p->resource_count = (u32)rn;
    p->step_count = b->step_count;
    p->steps = (de__rg_step *)carve(&at, b->step_count, sizeof(de__rg_step));
    memcpy(p->steps, b->steps, sizeof *p->steps * b->step_count);
    p->step_of_pass = (u32 *)carve(&at, pn, sizeof(u32));
    if (pn)
        memcpy(p->step_of_pass, b->step_of_pass, sizeof(u32) * pn);
    p->batch_count = b->batch_count;
    p->batches = (de__rg_batch *)carve(&at, b->batch_count, sizeof(de__rg_batch));
    memcpy(p->batches, b->batches, sizeof *p->batches * b->batch_count);
    p->batch_steps = (u32 *)carve(&at, b->step_count, sizeof(u32));
    memcpy(p->batch_steps, b->batch_steps, sizeof(u32) * b->step_count);
    p->wait_count = b->wait_count;
    p->waits = (de__rg_wait *)carve(&at, b->wait_count, sizeof(de__rg_wait));
    if (b->wait_count)
        memcpy(p->waits, b->waits, sizeof *p->waits * b->wait_count);
    memcpy(p->signal_count, b->signal_count, sizeof p->signal_count);
    p->barrier_count = b->tb_count;
    p->barriers = (de__rg_cbarrier *)carve(&at, b->tb_count, sizeof(de__rg_cbarrier));
    for (u32 i = 0; i < b->tb_count; ++i)
        p->barriers[i] = b->tb[i].b;
    p->end_access = (u32 *)carve(&at, rn, sizeof(u32));
    if (rn)
        memcpy(p->end_access, b->end_access, sizeof(u32) * rn);
    p->transient_count = b->transient_count;
    p->transients = (de__rg_transient *)carve(&at, b->transient_count, sizeof(de__rg_transient));
    if (b->transient_count)
        memcpy(p->transients, b->transients, sizeof *p->transients * b->transient_count);
    p->transient_of = (u32 *)carve(&at, rn, sizeof(u32));
    if (rn)
        memcpy(p->transient_of, b->transient_of, sizeof(u32) * rn);
    memcpy(p->class_bytes, b->class_bytes, sizeof p->class_bytes);
    p->placed = (u32 *)carve(&at, b->transient_count, sizeof(u32));
    DE_ASSERT((size_t)(at - block) == total);
    return p;
}

void de__rg_plan_destroy(de_rg *g, de__rg_plan *plan) {
    if (!plan)
        return;
    de__rg_plan_unplace(g, plan);
    DE_FREE(g->alloc, plan, plan->block_size);
}

de_result de__rg_cache_insert(de_rg *g, de__rg_plan *plan) {
    const u32 limit = de__rg_cache_limit(g);
    while (g->cache_count >= limit) {
        u32 lru = 0;
        for (u32 i = 1; i < g->cache_count; ++i)
            if (g->cache[i]->last_use < g->cache[lru]->last_use)
                lru = i;
        de__rg_plan *old = g->cache[lru];
        if (old == g->plan)
            g->plan = NULL;
        de__rg_plan_destroy(g, old);
        memmove(&g->cache[lru], &g->cache[lru + 1u],
                sizeof *g->cache * (g->cache_count - lru - 1u));
        --g->cache_count;
        ++g->stat_evictions;
    }
    if (g->cache_count == g->cache_cap) {
        const u32 nc = RG_CACHE_MAX;
        de__rg_plan **arr = (de__rg_plan **)DE_ALLOC(g->alloc, sizeof *arr * nc,
                                                     DE_ALIGNOF(de__rg_plan *), DE_MEM_RENDER);
        if (!arr)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render graph \"%s\": compiled-graph cache",
                           g->name);
        if (g->cache_count)
            memcpy(arr, g->cache, sizeof *arr * g->cache_count);
        if (g->cache)
            DE_FREE(g->alloc, g->cache, sizeof *g->cache * g->cache_cap);
        g->cache = arr;
        g->cache_cap = nc;
    }
    plan->last_use = ++g->cache_clock;
    g->cache[g->cache_count++] = plan;
    return DE_OK;
}
