/*
 * rg_alias.c: transient memory of a compiled render graph (SPEC §12.1 "transient render-graph
 * resources are aliased", §12.3; DENG-172).
 *
 * Requirements. Each used transient asks the device what it needs (de__gpu_texture_requirements /
 * de__gpu_buffer_requirements: size, alignment, the memory classes it may live in) and goes in
 * the bucket of the lowest class in its mask. D3D12 heap tier 1 and Vulkan put buffers, textures
 * and render targets in different classes, so the buckets follow the backend's rules.
 *
 * Packing (greedy, per class). Transients are placed in the order of their first step (then
 * declaration order), each at the lowest offset (aligned) that does not overlap any transient
 * already placed that it may not alias. Transient B may reuse the bytes of A when both are
 * aliasable (textures that are not exported: the issue limits aliasing to textures, and exported
 * resources are outputs) and A's last use on every queue completes before B's first use on every
 * queue starts (de__rg_step_before). On one queue that is "an earlier step", because B's first
 * barrier is from NONE, which waits for all earlier work of the queue on every backend; across
 * queues it needs a wait in the schedule (the vector clock). So a lifetime used on two queues
 * lasts until the later of its last uses, as seen by the queue that wants the memory.
 *
 * Pool. A graph keeps one transient region per memory class (de__gpu_transient_region_create),
 * grown to the largest need of any plan; growing replaces the region (the old one is freed once
 * its placed resources are reclaimed) and bumps the pool generation, after which each plan
 * places its resources again on its next use. A plan owns one placed texture or buffer per
 * transient (de__gpu_place_transient_*), reused every frame it is bound, so a cache hit creates
 * nothing.
 */
#include "render/graph/rg_internal.h"

#include <string.h>

typedef struct alias_info {
    u32 first_on[DE_QUEUE_COUNT_]; /* first step on each queue, DE__RG_NONE = unused there */
    u32 last_on[DE_QUEUE_COUNT_];
    u32 first; /* min of first_on */
    bool aliasable;
    bool placed;
} alias_info;

bool de__rg_step_before(const de__rg_cbuild *b, u32 a, u32 s) {
    if (a >= s)
        return false;
    const u32 qa = b->steps[a].queue, qs = b->steps[s].queue;
    if (qa == qs)
        return true;
    return b->clock_in[(size_t)s * DE_QUEUE_COUNT_ + qa] >= a + 1u;
}

/* True when every last use of A completes before every first use of B. */
static bool ends_before(const de__rg_cbuild *b, const alias_info *A, const alias_info *B) {
    for (u32 qa = 0; qa < DE_QUEUE_COUNT_; ++qa) {
        if (A->last_on[qa] == DE__RG_NONE)
            continue;
        for (u32 qb = 0; qb < DE_QUEUE_COUNT_; ++qb)
            if (B->first_on[qb] != DE__RG_NONE &&
                !de__rg_step_before(b, A->last_on[qa], B->first_on[qb]))
                return false;
    }
    return true;
}

static u64 align_up(u64 v, u64 a) {
    return a > 1 ? (v + (a - 1u)) / a * a : v;
}

static u32 lowest_class(u32 mask) {
    for (u32 c = 0; c < DE__GPU_MEM_CLASS_MAX; ++c)
        if (mask & (1u << c))
            return c;
    return DE__RG_NONE;
}

de_result de__rg_alias_transients(de__rg_cbuild *b) {
    de_rg *g = b->g;
    const u32 rn = g->resource_count;
    b->transient_of = rn ? DE_ARENA_PUSH(b->arena, u32, rn) : NULL;
    if (rn && !b->transient_of)
        return DE_ERR_OUT_OF_MEMORY;
    u32 count = 0;
    for (u32 i = 0; i < rn; ++i) {
        const de__rg_resource *r = &g->resources[i];
        b->transient_of[i] = DE__RG_NONE;
        if ((r->flags & DE__RG_RES_USED) && !(r->flags & DE__RG_RES_IMPORTED))
            ++count;
    }
    memset(b->class_bytes, 0, sizeof b->class_bytes);
    b->transient_count = count;
    if (!count)
        return DE_OK;
    if (!g->device)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__rg_compile: render graph \"%s\" uses transient resources but has no "
                       "device (de__rg_desc.device)",
                       g->name);
    b->transients = DE_ARENA_PUSH(b->arena, de__rg_transient, count);
    alias_info *info = DE_ARENA_PUSH(b->arena, alias_info, count);
    u32 *order = DE_ARENA_PUSH(b->arena, u32, count);
    if (!b->transients || !info || !order)
        return DE_ERR_OUT_OF_MEMORY;

    u32 t = 0;
    for (u32 i = 0; i < rn; ++i) {
        const de__rg_resource *r = &g->resources[i];
        if (!(r->flags & DE__RG_RES_USED) || (r->flags & DE__RG_RES_IMPORTED))
            continue;
        de__gpu_mem_req req;
        de_result res = r->kind == DE__RG_TEXTURE
                            ? de__gpu_texture_requirements(g->device, &r->tex, &req)
                            : de__gpu_buffer_requirements(g->device, &r->buf, &req);
        if (res != DE_OK)
            return DE_FAIL_WRAP(res, "de__rg_compile: render graph \"%s\": transient \"%s\"",
                                g->name, r->name);
        const u32 cls = lowest_class(req.class_mask);
        if (cls == DE__RG_NONE)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "de__rg_compile: render graph \"%s\": transient \"%s\" fits no memory "
                           "class",
                           g->name, r->name);
        de__rg_transient *tr = &b->transients[t];
        memset(tr, 0, sizeof *tr);
        tr->resource = i;
        tr->mem_class = cls;
        tr->size = req.size ? req.size : 1u;
        tr->align = req.align ? req.align : 256u;
        tr->buffer = r->kind == DE__RG_BUFFER;
        alias_info *ai = &info[t];
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
            ai->first_on[q] = ai->last_on[q] = DE__RG_NONE;
        ai->first = DE__RG_NONE;
        for (u32 a = r->access_first; a != DE__RG_NONE; a = g->accesses[a].res_next) {
            const u32 s = b->step_of_pass[g->accesses[a].pass];
            if (s == DE__RG_NONE)
                continue;
            const u32 q = b->steps[s].queue;
            if (ai->first_on[q] == DE__RG_NONE || s < ai->first_on[q])
                ai->first_on[q] = s;
            if (ai->last_on[q] == DE__RG_NONE || s > ai->last_on[q])
                ai->last_on[q] = s;
            if (ai->first == DE__RG_NONE || s < ai->first)
                ai->first = s;
        }
        if (r->flags & DE__RG_RES_EXPORTED) /* lives to the end of the frame */
            ai->last_on[DE_QUEUE_GRAPHICS] = b->step_count - 1u;
        ai->aliasable = r->kind == DE__RG_TEXTURE && !(r->flags & DE__RG_RES_EXPORTED);
        b->transient_of[i] = t;
        /* Insert into `order` by (first step, declaration order): stable. */
        u32 k = t;
        while (k > 0 && info[order[k - 1u]].first > ai->first) {
            order[k] = order[k - 1u];
            --k;
        }
        order[k] = t;
        ++t;
    }

    /* Greedy first-fit packing per class. `cand` holds the candidate offsets of one placement. */
    u64 *cand = DE_ARENA_PUSH(b->arena, u64, count + 1u);
    if (!cand)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 oi = 0; oi < count; ++oi) {
        const u32 bi = order[oi];
        de__rg_transient *B = &b->transients[bi];
        u32 nc = 0;
        cand[nc++] = 0;
        for (u32 oj = 0; oj < oi; ++oj) {
            const u32 ai = order[oj];
            const de__rg_transient *A = &b->transients[ai];
            if (A->mem_class != B->mem_class)
                continue;
            if (info[ai].aliasable && info[bi].aliasable && ends_before(b, &info[ai], &info[bi]))
                continue;
            cand[nc++] = align_up(A->offset + A->size, B->align);
        }
        /* Ascending, so the first fit is the lowest offset. */
        for (u32 i = 1; i < nc; ++i) {
            u64 x = cand[i];
            u32 j = i;
            while (j > 0 && cand[j - 1u] > x) {
                cand[j] = cand[j - 1u];
                --j;
            }
            cand[j] = x;
        }
        u64 chosen = 0;
        for (u32 ci = 0; ci < nc; ++ci) {
            const u64 off = cand[ci];
            bool fits = true;
            for (u32 oj = 0; oj < oi && fits; ++oj) {
                const u32 ai = order[oj];
                const de__rg_transient *A = &b->transients[ai];
                if (A->mem_class != B->mem_class)
                    continue;
                if (info[ai].aliasable && info[bi].aliasable &&
                    ends_before(b, &info[ai], &info[bi]))
                    continue;
                fits = off >= A->offset + A->size || A->offset >= off + B->size;
            }
            if (fits) {
                chosen = off;
                break;
            }
        }
        B->offset = chosen;
        info[bi].placed = true;
        for (u32 oj = 0; oj < oi; ++oj) {
            de__rg_transient *A = &b->transients[order[oj]];
            if (A->mem_class == B->mem_class && chosen < A->offset + A->size &&
                A->offset < chosen + B->size)
                A->aliased = B->aliased = true;
        }
        const u64 end = chosen + B->size;
        if (end > b->class_bytes[B->mem_class])
            b->class_bytes[B->mem_class] = end;
    }
    return DE_OK;
}

/* ---------- Pool and placement ---------- */

void de__rg_plan_unplace(de_rg *g, de__rg_plan *plan) {
    if (!plan || !plan->placed)
        return;
    for (u32 i = 0; i < plan->transient_count; ++i) {
        if (!plan->placed[i])
            continue;
        if (g->device) {
            if (plan->transients[i].buffer)
                de_gpu_buffer_destroy(g->device, (de_gpu_buffer){plan->placed[i]});
            else
                de_gpu_texture_destroy(g->device, (de_gpu_texture){plan->placed[i]});
        }
        plan->placed[i] = 0;
    }
    plan->placed_gen = 0;
}

void de__rg_pool_release(de_rg *g) {
    for (u32 c = 0; c < DE__GPU_MEM_CLASS_MAX; ++c) {
        if (g->pool[c])
            de__gpu_transient_region_destroy(g->pool[c]);
        g->pool[c] = NULL;
        g->pool_bytes[c] = 0;
    }
}

de_result de__rg_pool_bind(de_rg *g, de__rg_plan *plan) {
    if (!plan->transient_count)
        return DE_OK;
    if (g->pool_gen == 0)
        g->pool_gen = 1;
    bool grew = false;
    for (u32 c = 0; c < DE__GPU_MEM_CLASS_MAX; ++c) {
        const u64 need = plan->class_bytes[c];
        if (need <= g->pool_bytes[c])
            continue;
        de__gpu_transient_region *reg = NULL;
        de_result r = de__gpu_transient_region_create(g->device, c, need, &reg);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "render graph \"%s\": transient pool of %llu bytes (class %u)",
                                g->name, (unsigned long long)need, c);
        if (g->pool[c])
            de__gpu_transient_region_destroy(g->pool[c]);
        g->pool[c] = reg;
        g->pool_bytes[c] = need;
        grew = true;
    }
    if (grew) {
        /* Every plan's placements refer to replaced regions: drop them now so the old memory is
         * freed once the GPU is done; each plan places again on its next use. */
        ++g->pool_gen;
        for (u32 i = 0; i < g->cache_count; ++i)
            de__rg_plan_unplace(g, g->cache[i]);
        de__rg_plan_unplace(g, plan);
    }
    if (plan->placed_gen == g->pool_gen)
        return DE_OK;
    de__rg_plan_unplace(g, plan);
    for (u32 i = 0; i < plan->transient_count; ++i) {
        const de__rg_transient *t = &plan->transients[i];
        const de__rg_resource *r = &g->resources[t->resource];
        u32 id =
            t->buffer
                ? de__gpu_place_transient_buffer(g->pool[t->mem_class], t->offset, &r->buf).id
                : de__gpu_place_transient_texture(g->pool[t->mem_class], t->offset, &r->tex).id;
        if (!id) {
            de__rg_plan_unplace(g, plan);
            return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT,
                                "render graph \"%s\": placing transient \"%s\" at offset %llu",
                                g->name, r->name, (unsigned long long)t->offset);
        }
        plan->placed[i] = id;
    }
    plan->placed_gen = g->pool_gen;
    return DE_OK;
}
