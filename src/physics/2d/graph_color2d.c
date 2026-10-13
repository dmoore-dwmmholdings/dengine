/*
 * graph_color2d.c: deterministic greedy graph coloring of the 2D solver's constraint records and
 * the colored, chunked execution of the solver stages and per-body kernels (DENG-254; the 2D port
 * of physics/solver/graph_color.c and solver_parallel.c, DENG-218 and DENG-398).
 *
 * Coloring. One pass over the records in record order (classes ascending; contacts in sorted
 * manifold order, joints in stable id order). Color k has a bitset over the solver slots; a record
 * takes the first color whose bitset has none of its writable sides, and sets them. A side the
 * record may not write (the static slot, a kinematic or sleeping body, the world) is only read
 * during a stage and never conflicts. Records that fit none of the DE__GRAPH2D_MAX_COLORS colors
 * form the overflow set. A counting sort by color then gives `order`, each color's records in
 * record order. The coloring depends only on the record order and the slots, which depend only on
 * body indices and constraint ids.
 *
 * Execution. A stage runs color 0, 1, ... and then the overflow set (serially, on the calling
 * thread). The records of one color write disjoint solver bodies and their own data, so a color's
 * chunks (fixed `grain` records, independent of the worker count) may run on any worker in any
 * order and give the same bits. With tasks == NULL the same colored order runs inline. The
 * per-body kernels run in fixed ranges of `body_grain` slots.
 */
#include "physics/2d/solver2d_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_task.h>

/* ================================ Layout =============================================== */

#define GC2_ALIGN 16u

static size_t gc2_align(size_t x) {
    return (x + (GC2_ALIGN - 1u)) & ~(size_t)(GC2_ALIGN - 1u);
}

static u32 gc2_words(u32 slots) {
    return (slots + 63u) / 64u;
}

size_t de__coloring2d_bytes(u32 slot_capacity) {
    size_t off = gc2_align(sizeof(de__coloring2d));
    off = gc2_align(off + sizeof(u64) * (size_t)DE__GRAPH2D_MAX_COLORS * gc2_words(slot_capacity));
    off = gc2_align(off + sizeof(u32) * (size_t)slot_capacity);
    return off;
}

static u32 read_grain(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__SOLVER2D_GRAIN_CVAR;
    d.help = "2D physics: constraints per parallel chunk of a solver color (read when the solver "
             "is created; never changes results)";
    d.type = DE_CVAR_INT;
    d.default_number = (f64)DE__SOLVER2D_DEFAULT_GRAIN;
    d.min = 1.0;
    d.max = 1048576.0;
    const de_cvar cv = de_cvar_register(&d);
    if (!DE_HANDLE_VALID(cv))
        return DE__SOLVER2D_DEFAULT_GRAIN;
    const i64 v = de_cvar_int(cv);
    return v >= 1 && v <= 1048576 ? (u32)v : DE__SOLVER2D_DEFAULT_GRAIN;
}

de__coloring2d *de__coloring2d_init(void *mem, u32 slot_capacity) {
    u8 *m = (u8 *)mem;
    de__coloring2d *c = (de__coloring2d *)mem;
    memset(c, 0, sizeof *c);
    c->slot_capacity = slot_capacity;
    c->words = gc2_words(slot_capacity);
    size_t off = gc2_align(sizeof(de__coloring2d));
    c->bits = (u64 *)(void *)(m + off);
    off = gc2_align(off + sizeof(u64) * (size_t)DE__GRAPH2D_MAX_COLORS * c->words);
    c->stamp = (u32 *)(void *)(m + off);
    c->grain = read_grain();
    c->body_grain = DE__SOLVER2D_BODY_GRAIN;
    return c;
}

/* ================================ Coloring ============================================= */

static bool bit_test(const u64 *bits, u32 slot) {
    return ((bits[slot >> 6] >> (slot & 63u)) & 1u) != 0u;
}

static void bit_set(u64 *bits, u32 slot) {
    bits[slot >> 6] |= (u64)1u << (slot & 63u);
}

void de__graph2d_color(de__solver2d *s) {
    de__coloring2d *g = s->coloring;
    const u32 n = s->constraint_count;
    DE_ASSERT(n <= g->order_capacity);
    DE_ASSERT(s->body_count <= g->slot_capacity);
    const size_t used = gc2_words(s->body_count);
    for (u32 k = 0; k < DE__GRAPH2D_MAX_COLORS; ++k)
        memset(g->bits + (size_t)k * g->words, 0, sizeof(u64) * used);

    u32 count[DE__GRAPH2D_MAX_COLORS + 1u];
    memset(count, 0, sizeof count);
    u32 colors = 0;
    for (u32 i = 0; i < n; ++i) {
        de__constraint2d *c = &s->constraints[i];
        const bool wa = (c->flags & DE__CONSTRAINT2D_WRITE_A) != 0u;
        const bool wb = (c->flags & DE__CONSTRAINT2D_WRITE_B) != 0u;
        u32 color = DE__GRAPH2D_OVERFLOW;
        for (u32 k = 0; k < DE__GRAPH2D_MAX_COLORS; ++k) {
            u64 *bk = g->bits + (size_t)k * g->words;
            if ((wa && bit_test(bk, c->solver_a)) || (wb && bit_test(bk, c->solver_b)))
                continue;
            if (wa)
                bit_set(bk, c->solver_a);
            if (wb)
                bit_set(bk, c->solver_b);
            color = k;
            colors = k + 1u > colors ? k + 1u : colors;
            break;
        }
        c->color = color;
        ++count[color];
    }

    g->start[0] = 0;
    u32 largest = 0;
    for (u32 k = 0; k <= DE__GRAPH2D_OVERFLOW; ++k) {
        g->start[k + 1u] = g->start[k] + count[k];
        if (k < DE__GRAPH2D_MAX_COLORS)
            largest = count[k] > largest ? count[k] : largest;
    }
    u32 next[DE__GRAPH2D_MAX_COLORS + 1u];
    memcpy(next, g->start, sizeof next);
    for (u32 i = 0; i < n; ++i)
        g->order[next[s->constraints[i].color]++] = i;

    g->color_count = colors;
    g->stats.colors = colors;
    g->stats.overflow = count[DE__GRAPH2D_OVERFLOW];
    g->stats.largest = largest;
    const u32 grain = g->grain > 0u ? g->grain : 1u;
    u32 chunks = 0;
    for (u32 k = 0; k < colors; ++k)
        chunks += (count[k] + grain - 1u) / grain;
    g->stats.chunks = chunks;
#if defined(DE_DEBUG)
    DE_ASSERT(de__graph2d_color_validate(s));
#endif
}

bool de__graph2d_color_validate(const de__solver2d *s) {
    const de__coloring2d *g = s->coloring;
    const u32 n = s->constraint_count;
    if (g->start[0] != 0u || g->start[DE__GRAPH2D_OVERFLOW + 1u] != n)
        return false;
    memset(g->stamp, 0, sizeof(u32) * (size_t)s->body_count);
    for (u32 k = 0; k <= DE__GRAPH2D_OVERFLOW; ++k) {
        if (g->start[k + 1u] < g->start[k])
            return false;
        u32 prev = 0;
        for (u32 j = g->start[k]; j < g->start[k + 1u]; ++j) {
            const u32 i = g->order[j];
            /* Ascending within a color: no record twice; with the color check and the total,
             * every record exactly once. */
            if (i >= n || (j > g->start[k] && i <= prev))
                return false;
            prev = i;
            const de__constraint2d *c = &s->constraints[i];
            if (c->color != k)
                return false;
            if (k == DE__GRAPH2D_OVERFLOW)
                continue; /* solved serially */
            const u32 sides[2] = {(c->flags & DE__CONSTRAINT2D_WRITE_A) ? c->solver_a : 0u,
                                  (c->flags & DE__CONSTRAINT2D_WRITE_B) ? c->solver_b : 0u};
            for (u32 e = 0; e < 2; ++e) {
                const u32 slot = sides[e];
                if (slot == DE__SOLVER2D_STATIC_SLOT)
                    continue;
                if (slot >= s->body_count || g->stamp[slot] == k + 1u)
                    return false;
                g->stamp[slot] = k + 1u;
            }
        }
    }
    return true;
}

/* ================================ Stage runner ========================================= */

static void run_one(const de__solver2d_ctx *ctx, const de__solver2d_class_slot *k,
                    de__solver2d_stage stage, de__constraint2d *c) {
    const de__constraint2d_class *cls = k->cls;
    switch (stage) {
    case DE__STAGE2D_PREPARE:
        if (cls->prepare != NULL)
            cls->prepare(ctx, k->state, c);
        break;
    case DE__STAGE2D_WARM_START:
        if (cls->warm_start != NULL)
            cls->warm_start(ctx, k->state, c);
        break;
    case DE__STAGE2D_SOLVE:
        cls->solve(ctx, k->state, c, true);
        break;
    case DE__STAGE2D_RELAX:
        if (cls->relax != NULL)
            cls->relax(ctx, k->state, c);
        else
            cls->solve(ctx, k->state, c, false);
        break;
    case DE__STAGE2D_RESTITUTION:
        if (cls->restitution != NULL)
            cls->restitution(ctx, k->state, c);
        break;
    case DE__STAGE2D_STORE:
        if (cls->store != NULL)
            cls->store(ctx, k->state, c);
        break;
    }
}

typedef struct sp2_run {
    de__solver2d *s;
    const u32 *order; /**< the records to run */
    de__solver2d_stage stage;
} sp2_run;

static void run_range(u32 begin, u32 end, void *user) {
    const sp2_run *r = (const sp2_run *)user;
    de__solver2d *s = r->s;
    const de__solver2d_ctx *ctx = &s->ctx;
    for (u32 j = begin; j < end; ++j) {
        de__constraint2d *c = &s->constraints[r->order[j]];
        run_one(ctx, &s->classes[c->class_id], r->stage, c);
    }
}

/* Runs order[0, n) of color k: chunked on the task system unless serial. */
static void run_records(de__solver2d *s, de__solver2d_stage stage, const u32 *order, u32 n,
                        bool serial) {
    de_task_system *ts = s->p->tasks;
    const u32 grain = s->coloring->grain > 0u ? s->coloring->grain : 1u;
    sp2_run r = {s, order, stage};
    if (ts != NULL && n > grain && !serial)
        de_task_parallel_for(ts, n, grain, run_range, &r);
    else
        run_range(0, n, &r);
}

void de__solver2d_run_stage(de__solver2d *s, de__solver2d_stage stage) {
    const de__coloring2d *g = s->coloring;
    for (u32 k = 0; k <= DE__GRAPH2D_OVERFLOW; ++k) {
        const u32 first = g->start[k];
        const u32 n = g->start[k + 1u] - first;
        if (n != 0u)
            run_records(s, stage, g->order + first, n, k == DE__GRAPH2D_OVERFLOW);
    }
}

/* First position in order[0, n) whose record's class id is >= class_id (class ids do not decrease
 * along a color: records are added classes ascending and colored in record order). */
static u32 class_lower_bound(const de__solver2d *s, const u32 *order, u32 n, u32 class_id) {
    u32 lo = 0, hi = n;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (s->constraints[order[mid]].class_id < class_id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

void de__solver2d_run_stage_extra(de__solver2d *s, de__solver2d_stage stage, u32 pass) {
    const de__coloring2d *g = s->coloring;
    for (u32 k = 0; k <= DE__GRAPH2D_OVERFLOW; ++k) {
        const u32 first = g->start[k];
        const u32 n = g->start[k + 1u] - first;
        if (n == 0u)
            continue;
        const u32 *order = g->order + first;
        for (u32 c = 0; c < DE__CONSTRAINT2D_CLASS_COUNT; ++c) {
            const de__solver2d_class_slot *cl = &s->classes[c];
            if (cl->cls == NULL || cl->count == 0u || cl->iterations <= pass)
                continue;
            const u32 lo = class_lower_bound(s, order, n, c);
            const u32 hi = class_lower_bound(s, order, n, c + 1u);
            if (hi > lo)
                run_records(s, stage, order + lo, hi - lo, k == DE__GRAPH2D_OVERFLOW);
        }
    }
}

typedef struct sp2_bodies {
    de__solver2d *s;
    void (*fn)(de__solver2d *s, u32 begin, u32 end);
} sp2_bodies;

static void body_range(u32 begin, u32 end, void *user) {
    const sp2_bodies *r = (const sp2_bodies *)user;
    r->fn(r->s, begin + 1u, end + 1u); /* slot 0 is the static slot */
}

void de__solver2d_for_bodies(de__solver2d *s, void (*fn)(de__solver2d *s, u32 begin, u32 end)) {
    const u32 n = s->body_count - 1u;
    if (n == 0u)
        return;
    de_task_system *ts = s->p->tasks;
    const u32 grain = s->coloring->body_grain > 0u ? s->coloring->body_grain : 1u;
    if (ts != NULL && n > grain) {
        sp2_bodies r = {s, fn};
        de_task_parallel_for(ts, n, grain, body_range, &r);
    } else {
        fn(s, 1u, s->body_count);
    }
}
