/*
 * solver_parallel.c: colored, chunked execution of the solver stages and fixed-range execution of
 * the per-body kernels (DENG-218, SPEC §13.2, §5.7 rule 5, §8; see graph_color.h).
 *
 * A stage runs color 0, 1, ... in order and then the overflow set. The records of one color write
 * disjoint solver bodies (and their own data), so the chunks of a color can run on any worker in
 * any order and give the same bits; colors are separated by the join of de_task_parallel_for.
 * Chunk boundaries depend only on the record count and the grain, never on the worker count. The
 * overflow set runs serially, in record order, on the calling thread.
 */
#include "physics/solver/graph_color.h"

#include "physics/solver/solver_internal.h"

#include <dengine/de_task.h>

static void run_one(const de__solver_ctx *ctx, const de__solver_class_slot *k,
                    de__solver_stage stage, de__constraint *c) {
    const de__constraint_class *cls = k->cls;
    switch (stage) {
    case DE__STAGE_PREPARE:
        if (cls->prepare != NULL)
            cls->prepare(ctx, k->state, c);
        break;
    case DE__STAGE_WARM_START:
        if (cls->warm_start != NULL)
            cls->warm_start(ctx, k->state, c);
        break;
    case DE__STAGE_SOLVE:
        cls->solve(ctx, k->state, c, true);
        break;
    case DE__STAGE_RELAX:
        if (cls->relax != NULL)
            cls->relax(ctx, k->state, c);
        else
            cls->solve(ctx, k->state, c, false);
        break;
    case DE__STAGE_RESTITUTION:
        if (cls->restitution != NULL)
            cls->restitution(ctx, k->state, c);
        break;
    case DE__STAGE_STORE:
        if (cls->store != NULL)
            cls->store(ctx, k->state, c);
        break;
    }
}

typedef struct sp_run {
    de__solver *s;
    const u32 *order; /**< the color's records */
    de__solver_stage stage;
} sp_run;

static void run_range(u32 begin, u32 end, void *user) {
    const sp_run *r = (const sp_run *)user;
    de__solver *s = r->s;
    const de__solver_ctx *ctx = &s->ctx;
    for (u32 j = begin; j < end; ++j) {
        de__constraint *c = &s->constraints[r->order[j]];
        run_one(ctx, &s->classes[c->class_id], r->stage, c);
    }
}

void de__solver_run_stage(de__solver *s, de__solver_stage stage) {
    const de__coloring *g = s->coloring;
    de_task_system *ts = s->p->tasks;
    const u32 grain = g->grain > 0u ? g->grain : 1u;
    for (u32 k = 0; k <= DE__GRAPH_OVERFLOW; ++k) {
        const u32 first = g->start[k];
        const u32 n = g->start[k + 1u] - first;
        if (n == 0u)
            continue;
        sp_run r = {s, g->order + first, stage};
        if (ts != NULL && n > grain && k != DE__GRAPH_OVERFLOW)
            de_task_parallel_for(ts, n, grain, run_range, &r);
        else
            run_range(0, n, &r);
    }
}

/* First position in order[0, n) whose record's class id is >= class_id (class ids do not decrease
 * along a color: records are added classes ascending and colored in record order). */
static u32 class_lower_bound(const de__solver *s, const u32 *order, u32 n, u32 class_id) {
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

void de__solver_run_stage_extra(de__solver *s, de__solver_stage stage, u32 pass) {
    const de__coloring *g = s->coloring;
    de_task_system *ts = s->p->tasks;
    const u32 grain = g->grain > 0u ? g->grain : 1u;
    for (u32 k = 0; k <= DE__GRAPH_OVERFLOW; ++k) {
        const u32 first = g->start[k];
        const u32 n = g->start[k + 1u] - first;
        if (n == 0u)
            continue;
        const u32 *order = g->order + first;
        for (u32 c = 0; c < DE__CONSTRAINT_CLASS_COUNT; ++c) {
            const de__solver_class_slot *cl = &s->classes[c];
            if (cl->cls == NULL || cl->count == 0u || cl->iterations <= pass)
                continue;
            const u32 lo = class_lower_bound(s, order, n, c);
            const u32 hi = class_lower_bound(s, order, n, c + 1u);
            if (hi <= lo)
                continue;
            /* One color's records write disjoint bodies, so running one class's range of it alone
             * gives the same bits whatever the chunking. */
            sp_run r = {s, order + lo, stage};
            if (ts != NULL && hi - lo > grain && k != DE__GRAPH_OVERFLOW)
                de_task_parallel_for(ts, hi - lo, grain, run_range, &r);
            else
                run_range(0, hi - lo, &r);
        }
    }
}

typedef struct sp_bodies {
    de__solver *s;
    void (*fn)(de__solver *s, u32 begin, u32 end);
} sp_bodies;

static void body_range(u32 begin, u32 end, void *user) {
    const sp_bodies *r = (const sp_bodies *)user;
    r->fn(r->s, begin + 1u, end + 1u); /* slot 0 is the static slot */
}

void de__solver_for_bodies(de__solver *s, void (*fn)(de__solver *s, u32 begin, u32 end)) {
    const u32 n = s->body_count - 1u;
    if (n == 0u)
        return;
    de_task_system *ts = s->p->tasks;
    const u32 grain = s->coloring->body_grain > 0u ? s->coloring->body_grain : 1u;
    if (ts != NULL && n > grain) {
        sp_bodies r = {s, fn};
        de_task_parallel_for(ts, n, grain, body_range, &r);
    } else {
        fn(s, 1u, s->body_count);
    }
}
