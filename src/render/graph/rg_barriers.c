/*
 * rg_barriers.c: the barrier plan of a compiled render graph (SPEC §12.1, §12.3, DENG-172).
 *
 * Passes never issue barriers; the graph does, from the declared accesses. The compiler emits
 * only abstract de_gpu_barrier values (before/after de_gpu_access bits, a mip/layer range, the
 * two queues); each backend lowers them (D3D12 enhanced or legacy, Vulkan sync2 layouts, Metal
 * fence epochs).
 *
 * State. Every used resource is cut into cells: the mip and layer boundaries of all its live
 * accesses split the texture into a grid (one cell for a buffer, and one for a texture that is
 * only accessed whole). Each cell has a state: the access bits of its last use, the queue that
 * owns it, and the step of that use. Imports start in their `current_access`, owned by the
 * graphics queue (state from the prologue step); transients start with no state.
 *
 * For each live pass, in schedule order, a cell the pass touches gets the union of the pass's
 * accesses on it (a read-modify-write pass is in RENDER_TARGET or DEPTH_READ|DEPTH_WRITE, ...)
 * and a transition:
 *   - no earlier state (a transient's first use, or an import in NONE): NONE -> new. NONE before
 *     discards the contents; every backend makes it wait for earlier work on the queue, and D3D12
 *     initialises placed render targets and depth buffers (DISCARD). This is also what makes an
 *     aliased transient safe after the one that used its memory before.
 *   - another queue owns it: an ownership transfer {old -> new, src_queue, dst_queue}, recorded
 *     twice: the release after the last step that used it (on the old queue, slot POST) and the
 *     acquire before this step (slot PRE). The step also depends on the releasing step, which
 *     rg_schedule.c turns into a wait and a signal.
 *   - same queue, different bits, or a write on either side: old -> new before the step (WAR and
 *     WAW need the execution dependency even when the bits are equal).
 *   - same queue, the same read bits: nothing.
 * The transitions of one resource and step are merged over neighbouring cells into the fewest
 * ranges (mip runs per layer row, then equal runs of consecutive rows), and every barrier of a
 * step is recorded with one de_gpu_cmd_barriers call (batched per pass).
 *
 * End of frame (the epilogue, a graphics step after every pass): imports and exported resources
 * go to their final state on the graphics queue: the de_rg_export `final_access` when it is not
 * NONE, else the access of their last use (so an import always ends in one state, which
 * de__rg_end_access reports for the next frame's import). Queue capability: barriers recorded on
 * the copy queue only change COPY_* states, except the acquire half of a transfer, whose
 * source half the backends do not record there.
 */
#include "render/graph/rg_internal.h"

#include <string.h>

#define NOQ ((u32)DE_QUEUE_COUNT_) /* a cell no queue owns yet */

typedef struct cell_state {
    u32 access, queue, last;
} cell_state;

/* A cell's transition in the current step; `valid` false = no barrier. */
typedef struct cell_tr {
    bool valid;
    u32 before, after, src, dst, rel;
} cell_tr;

static bool tr_equal(const cell_tr *a, const cell_tr *b) {
    return a->valid == b->valid && a->before == b->before && a->after == b->after &&
           a->src == b->src && a->dst == b->dst && a->rel == b->rel;
}

/* A merged rectangle of cells [m0, m1) x [l0, l1) with one transition. */
typedef struct cell_rect {
    u32 m0, m1, l0, l1;
    cell_tr tr;
} cell_rect;

typedef struct res_ctx {
    de__rg_cbuild *b;
    u32 resource;
    const u32 *ms, *ls; /* split points */
    u32 nm, nl;         /* cells per row / rows */
    u32 cap_m, cap_l;   /* real counts (transient textures) or UINT32_MAX */
    cell_state *st;
    cell_tr *tr;
    cell_rect *rects;
} res_ctx;

static bool push_barrier(de__rg_cbuild *b, const de__rg_cbarrier *cb, u32 step, u32 slot) {
    de__rg_tbarrier *arr =
        de__rg_cgrow(b, b->tb, b->tb_count, &b->tb_cap, b->tb_count + 1u, sizeof *arr);
    if (!arr)
        return false;
    b->tb = arr;
    arr[b->tb_count++] = (de__rg_tbarrier){*cb, step, slot};
    return true;
}

static bool push_dep(de__rg_cbuild *b, u32 from, u32 to) {
    if (b->dep_count && b->deps[b->dep_count - 1u].from == from &&
        b->deps[b->dep_count - 1u].to == to)
        return true;
    de__rg_dep *arr =
        de__rg_cgrow(b, b->deps, b->dep_count, &b->dep_cap, b->dep_count + 1u, sizeof *arr);
    if (!arr)
        return false;
    b->deps = arr;
    arr[b->dep_count++] = (de__rg_dep){from, to};
    return true;
}

/* Sorted unique boundaries of the live accesses of resource `r` in one dimension, clamped to
 * `cap`; always holds 0 and cap. */
static u32 *collect_splits(de__rg_cbuild *b, const de__rg_resource *r, bool mips, u32 cap,
                           u32 *count) {
    de_rg *g = b->g;
    u32 n = 2;
    for (u32 a = r->access_first; a != DE__RG_NONE; a = g->accesses[a].res_next)
        n += 2;
    u32 *v = DE_ARENA_PUSH(b->arena, u32, n);
    if (!v)
        return NULL;
    u32 k = 0;
    v[k++] = 0;
    v[k++] = cap;
    for (u32 a = r->access_first; a != DE__RG_NONE; a = g->accesses[a].res_next) {
        const de__rg_access *x = &g->accesses[a];
        if (g->passes[x->pass]->culled)
            continue;
        u32 lo = mips ? x->range.mip_first : x->range.layer_first;
        u32 hi = mips ? x->range.mip_end : x->range.layer_end;
        v[k++] = lo < cap ? lo : cap;
        v[k++] = hi < cap ? hi : cap;
    }
    /* Insertion sort (short arrays), then drop duplicates. */
    for (u32 i = 1; i < k; ++i) {
        u32 x = v[i], j = i;
        while (j > 0 && v[j - 1u] > x) {
            v[j] = v[j - 1u];
            --j;
        }
        v[j] = x;
    }
    u32 u = 0;
    for (u32 i = 0; i < k; ++i)
        if (u == 0 || v[i] != v[u - 1u])
            v[u++] = v[i];
    *count = u;
    return v;
}

static de__rg_range cell_range(const res_ctx *c, u32 m, u32 l) {
    return (de__rg_range){c->ms[m], c->ms[m + 1u], c->ls[l], c->ls[l + 1u]};
}

/* The transition of one cell to `want` on queue `q` at step `s`. `final`: the epilogue, where a
 * cell already in the wanted state needs nothing even after a write. */
static cell_tr transition(const cell_state *st, u32 want, u32 q, u32 s, bool final) {
    cell_tr t = {0};
    if (st->queue == NOQ || st->access == DE_ACCESS_NONE) {
        if (final && want == DE_ACCESS_NONE)
            return t;
        t = (cell_tr){true, DE_ACCESS_NONE, want, q, q, s};
    } else if (st->queue != q) {
        t = (cell_tr){true, st->access, want, st->queue, q, st->last};
    } else if (st->access != want ||
               (!final && ((st->access | want) & (u32)DE__RG_WRITE_BITS) != 0)) {
        t = (cell_tr){true, st->access, want, q, q, s};
    }
    return t;
}

static u32 range_count(u32 first, u32 end, u32 cap) {
    return end >= cap ? 0u : end - first;
}

/* Merges the step's cell transitions into rectangles and emits their barriers. */
static bool emit_rects(res_ctx *c, u32 step) {
    de__rg_cbuild *b = c->b;
    u32 nr = 0;
    for (u32 l = 0; l < c->nl; ++l) {
        u32 m = 0;
        while (m < c->nm) {
            const cell_tr *t = &c->tr[l * c->nm + m];
            if (!t->valid) {
                ++m;
                continue;
            }
            u32 m1 = m + 1u;
            while (m1 < c->nm && tr_equal(&c->tr[l * c->nm + m1], t))
                ++m1;
            /* Extend a rectangle of the previous row with the same run, else start one. */
            bool merged = false;
            for (u32 i = 0; i < nr && !merged; ++i) {
                cell_rect *r = &c->rects[i];
                if (r->l1 == l && r->m0 == m && r->m1 == m1 && tr_equal(&r->tr, t)) {
                    r->l1 = l + 1u;
                    merged = true;
                }
            }
            if (!merged)
                c->rects[nr++] = (cell_rect){m, m1, l, l + 1u, *t};
            m = m1;
        }
    }
    const bool buffer = b->g->resources[c->resource].kind == DE__RG_BUFFER;
    for (u32 i = 0; i < nr; ++i) {
        const cell_rect *r = &c->rects[i];
        de__rg_cbarrier cb = {
            .resource = c->resource,
            .before = r->tr.before,
            .after = r->tr.after,
            .src_queue = (de_gpu_queue)r->tr.src,
            .dst_queue = (de_gpu_queue)r->tr.dst,
        };
        if (!buffer) {
            cb.mip_first = c->ms[r->m0];
            cb.mip_count = range_count(c->ms[r->m0], c->ms[r->m1], c->cap_m);
            cb.layer_first = c->ls[r->l0];
            cb.layer_count = range_count(c->ls[r->l0], c->ls[r->l1], c->cap_l);
        }
        if (r->tr.src != r->tr.dst) {
            /* Release on the old queue after its last use, acquire before this step. */
            if (!push_barrier(b, &cb, r->tr.rel, DE__RG_POST) || !push_dep(b, r->tr.rel, step))
                return false;
        }
        if (!push_barrier(b, &cb, step, DE__RG_PRE))
            return false;
    }
    return true;
}

/* Applies `want_of(cell)` at step `s` on queue `q` to every cell that wants something. */
static bool apply_step(res_ctx *c, const u32 *want, u32 s, u32 q, bool final) {
    const u32 cells = c->nm * c->nl;
    bool any = false;
    for (u32 i = 0; i < cells; ++i) {
        c->tr[i] = (cell_tr){0};
        if (!final && want[i] == DE_ACCESS_NONE)
            continue;
        c->tr[i] = transition(&c->st[i], want[i], q, s, final);
        any |= c->tr[i].valid;
    }
    if (any && !emit_rects(c, s))
        return false;
    for (u32 i = 0; i < cells; ++i) {
        if (!final && want[i] == DE_ACCESS_NONE)
            continue;
        if (final && !c->tr[i].valid)
            continue;
        c->st[i] = (cell_state){want[i], q, s};
    }
    return true;
}

static bool plan_resource(de__rg_cbuild *b, u32 ri) {
    de_rg *g = b->g;
    const de__rg_resource *r = &g->resources[ri];
    const bool imported = (r->flags & DE__RG_RES_IMPORTED) != 0;
    const bool exported = (r->flags & DE__RG_RES_EXPORTED) != 0;
    b->end_access[ri] = imported ? r->initial_access : DE_ACCESS_NONE;
    if (!(r->flags & DE__RG_RES_USED))
        return true;

    res_ctx c = {.b = b, .resource = ri};
    const bool sized = r->kind == DE__RG_TEXTURE && !imported;
    c.cap_m = sized && r->mip_count ? r->mip_count : UINT32_MAX;
    c.cap_l = sized && r->layer_count ? r->layer_count : UINT32_MAX;
    u32 mn = 0, ln = 0;
    c.ms = collect_splits(b, r, true, c.cap_m, &mn);
    c.ls = collect_splits(b, r, false, c.cap_l, &ln);
    if (!c.ms || !c.ls)
        return false;
    c.nm = mn - 1u;
    c.nl = ln - 1u;
    const u32 cells = c.nm * c.nl;
    c.st = DE_ARENA_PUSH(b->arena, cell_state, cells);
    c.tr = DE_ARENA_PUSH(b->arena, cell_tr, cells);
    c.rects = DE_ARENA_PUSH(b->arena, cell_rect, cells);
    u32 *want = DE_ARENA_PUSH(b->arena, u32, cells);
    if (!c.st || !c.tr || !c.rects || !want)
        return false;
    for (u32 i = 0; i < cells; ++i)
        c.st[i] = imported ? (cell_state){r->initial_access, DE_QUEUE_GRAPHICS, 0}
                           : (cell_state){DE_ACCESS_NONE, NOQ, DE__RG_NONE};

    /* The chain is in pass order, so a pass's accesses of this resource are adjacent. */
    u32 last_bits = DE_ACCESS_NONE;
    u32 a = r->access_first;
    while (a != DE__RG_NONE) {
        const u32 pass = g->accesses[a].pass;
        u32 end = a;
        while (end != DE__RG_NONE && g->accesses[end].pass == pass)
            end = g->accesses[end].res_next;
        if (!g->passes[pass]->culled) {
            const u32 s = b->step_of_pass[pass];
            memset(want, 0, sizeof *want * cells);
            u32 bits = 0;
            for (u32 x = a; x != end; x = g->accesses[x].res_next) {
                const de__rg_access *ax = &g->accesses[x];
                bits |= ax->access;
                for (u32 l = 0; l < c.nl; ++l)
                    for (u32 m = 0; m < c.nm; ++m)
                        if (de__rg_range_overlaps(ax->range, cell_range(&c, m, l)))
                            want[l * c.nm + m] |= ax->access;
            }
            last_bits = bits;
            if (!apply_step(&c, want, s, b->steps[s].queue, false))
                return false;
        }
        a = end;
    }

    if (imported || exported) {
        u32 target = exported && r->final_access ? r->final_access : last_bits;
        for (u32 i = 0; i < cells; ++i)
            want[i] = target;
        if (!apply_step(&c, want, b->step_count - 1u, DE_QUEUE_GRAPHICS, true))
            return false;
        b->end_access[ri] = target;
    }
    return true;
}

bool de__rg_plan_barriers(de__rg_cbuild *b) {
    de_rg *g = b->g;
    b->end_access = g->resource_count ? DE_ARENA_PUSH(b->arena, u32, g->resource_count) : NULL;
    if (g->resource_count && !b->end_access)
        return false;
    for (u32 i = 0; i < g->resource_count; ++i)
        if (!plan_resource(b, i))
            return false;

    /* Group by (step, slot), stable: resources in declaration order within a step. */
    const u32 n = b->step_count;
    u32 *start = DE_ARENA_PUSH(b->arena, u32, 2u * n + 1u);
    de__rg_tbarrier *sorted =
        b->tb_count ? DE_ARENA_PUSH(b->arena, de__rg_tbarrier, b->tb_count) : NULL;
    if (!start || (b->tb_count && !sorted))
        return false;
    memset(start, 0, sizeof *start * (2u * n + 1u));
    for (u32 i = 0; i < b->tb_count; ++i)
        ++start[b->tb[i].step * 2u + b->tb[i].slot + 1u];
    for (u32 k = 0; k < 2u * n; ++k)
        start[k + 1u] += start[k];
    for (u32 s = 0; s < n; ++s)
        for (u32 slot = 0; slot < 2u; ++slot) {
            b->steps[s].barrier_first[slot] = start[s * 2u + slot];
            b->steps[s].barrier_count[slot] = start[s * 2u + slot + 1u] - start[s * 2u + slot];
        }
    for (u32 i = 0; i < b->tb_count; ++i)
        sorted[start[b->tb[i].step * 2u + b->tb[i].slot]++] = b->tb[i];
    b->tb = sorted;
    b->tb_cap = b->tb_count;
    return true;
}
