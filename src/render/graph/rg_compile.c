/*
 * rg_compile.c: the render graph compiler's driver (SPEC §12.3, DENG-172). See rg_internal.h for
 * the plan layout and the frame flow.
 *
 * de__rg_compile, after de__rg_finalize:
 *   1. builds the cache key of the frame's declarations (rg_cache.c) on the frame arena;
 *   2. on a hit reuses the cached plan; on a miss compiles one on the frame arena, then copies it
 *      into one heap block and caches it:
 *        - steps and queues (rg_schedule.c),
 *        - barriers, transfers and cross-queue dependencies (rg_barriers.c),
 *        - batches, waits and signals (rg_schedule.c),
 *        - transient memory classes and offsets (rg_alias.c);
 *   3. makes sure the transient pool is large enough and the plan's transients are placed in it
 *      (rg_alias.c), and binds this frame's physical resources: the placed transients into the
 *      graph's resources (so de_rg_texture / de_rg_buffer return them) and every barrier of the
 *      plan into g->phys, a de_gpu_barrier table on the frame arena with this frame's handles
 *      (imports change every frame; they are bound by slot).
 * A hit allocates nothing on the heap: the key and the barrier table are on the frame arena.
 */
#include "render/graph/rg_internal.h"

#include <string.h>

static de_result compile_plan(de_rg *g, bool async, const u8 *key, u32 key_size, u64 hash,
                              de__rg_plan **out) {
    de__rg_cbuild b;
    memset(&b, 0, sizeof b);
    b.g = g;
    b.arena = g->arena;
    b.async_compute = async;
    if (!de__rg_schedule_steps(&b) || !de__rg_plan_barriers(&b) || !de__rg_schedule_batches(&b))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_compile: render graph \"%s\": the frame "
                       "arena is full",
                       g->name);
    de_result r = de__rg_alias_transients(&b);
    if (r == DE_ERR_OUT_OF_MEMORY)
        return DE_FAIL(r, "de__rg_compile: render graph \"%s\": the frame arena is full", g->name);
    if (r != DE_OK)
        return r;
    de__rg_plan *plan = de__rg_plan_create(g, &b, key, key_size, hash);
    if (!plan)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_compile: render graph \"%s\": no memory "
                       "for the compiled plan",
                       g->name);
    r = de__rg_cache_insert(g, plan);
    if (r != DE_OK) {
        de__rg_plan_destroy(g, plan);
        return r;
    }
    *out = plan;
    return DE_OK;
}

de_result de__rg_compile(de_rg *g) {
    if (!g)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_compile: the graph is NULL");
    g->plan = NULL;
    g->phys = NULL;
    if (!g->finalized)
        return DE_FAIL(DE_ERR_BUSY, "de__rg_compile: render graph \"%s\" is not finalized",
                       g->name);
    if (g->error_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__rg_compile: render graph \"%s\" has %u invalid declaration%s", g->name,
                       g->error_count, g->error_count == 1 ? "" : "s");
    const bool async = !g->no_async_compute && g->device && de_gpu_get_caps(g->device) &&
                       de_gpu_get_caps(g->device)->async_compute;
    u8 *key = NULL;
    u32 key_size = 0;
    u64 hash = 0;
    if (!de__rg_cache_key(g, async, &key, &key_size, &hash))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_compile: render graph \"%s\": the frame arena is full", g->name);
    de__rg_plan *plan = de__rg_cache_find(g, key, key_size, hash);
    if (plan) {
        ++g->stat_hits;
    } else {
        de_result r = compile_plan(g, async, key, key_size, hash, &plan);
        if (r != DE_OK)
            return r;
        ++g->stat_compiles;
    }
    de_result r = de__rg_pool_bind(g, plan);
    if (r != DE_OK)
        return r;

    /* Bind this frame's physical resources. */
    for (u32 i = 0; i < plan->transient_count; ++i) {
        de__rg_resource *res = &g->resources[plan->transients[i].resource];
        if (plan->transients[i].buffer)
            res->buffer.id = plan->placed[i];
        else
            res->texture.id = plan->placed[i];
    }
    de_gpu_barrier *phys =
        plan->barrier_count ? DE_ARENA_PUSH(g->arena, de_gpu_barrier, plan->barrier_count) : NULL;
    if (plan->barrier_count && !phys)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_compile: render graph \"%s\": the frame arena is full", g->name);
    for (u32 i = 0; i < plan->barrier_count; ++i) {
        const de__rg_cbarrier *c = &plan->barriers[i];
        const de__rg_resource *res = &g->resources[c->resource];
        de_gpu_barrier *b = &phys[i];
        memset(b, 0, sizeof *b);
        if (res->kind == DE__RG_TEXTURE)
            b->texture = res->texture;
        else
            b->buffer = res->buffer;
        b->before = c->before;
        b->after = c->after;
        b->mip_first = c->mip_first;
        b->mip_count = c->mip_count;
        b->layer_first = c->layer_first;
        b->layer_count = c->layer_count;
        b->src_queue = c->src_queue;
        b->dst_queue = c->dst_queue;
    }
    g->phys = phys;
    g->plan = plan;
    return DE_OK;
}

void de__rg_record_barriers(const de_rg *g, u32 step, u32 slot, de_gpu_cmd *cmd) {
    if (!g || !g->plan || step >= g->plan->step_count || slot > DE__RG_POST)
        return;
    const de__rg_step *s = &g->plan->steps[step];
    if (s->barrier_count[slot])
        de_gpu_cmd_barriers(cmd, &g->phys[s->barrier_first[slot]], s->barrier_count[slot]);
}

u32 de__rg_end_access(const de_rg *g, de_rg_resource r) {
    u32 i = de__rg_resource_index(g, r);
    if (!g || !g->plan || i == DE__RG_NONE || i >= g->plan->resource_count)
        return DE_ACCESS_NONE;
    return g->plan->end_access[i];
}

void de__rg_set_async_compute(de_rg *g, bool allow) {
    if (g)
        g->no_async_compute = !allow;
}

de__rg_stats de__rg_compile_stats(const de_rg *g) {
    de__rg_stats s = {0};
    if (g) {
        s.compiles = g->stat_compiles;
        s.hits = g->stat_hits;
        s.evictions = g->stat_evictions;
        s.cached = g->cache_count;
    }
    return s;
}

void de__rg_compile_frame_end(de_rg *g) {
    if (!g)
        return;
    g->plan = NULL;
    g->phys = NULL;
}

void de__rg_compile_shutdown(de_rg *g) {
    if (!g)
        return;
    for (u32 i = 0; i < g->cache_count; ++i)
        de__rg_plan_destroy(g, g->cache[i]);
    if (g->cache)
        DE_FREE(g->alloc, g->cache, sizeof *g->cache * g->cache_cap);
    g->cache = NULL;
    g->cache_count = g->cache_cap = 0;
    g->plan = NULL;
    g->phys = NULL;
    de__rg_pool_release(g);
}
