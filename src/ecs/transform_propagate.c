/* transform_propagate.c: the built-in transform propagation system (DENG-351, SPEC §7.1, §5.5,
 * §10.2 step 4).
 *
 * de.transform_propagate runs in DE_SCHEDULE_POST_UPDATE. Its first query is the roots
 * (DeTransform, optional DeChildren, without DeChildOf), split by de_query_par_each into fixed
 * ranges. A range walks the subtree of each of its roots depth first, in child order, with an
 * explicit stack of (parent global, next child, dirty) frames in thread scratch, so deep
 * hierarchies cannot overflow the call stack. A subtree is only ever touched by its root's range,
 * so the bits written do not depend on the worker count.
 *
 * The second query is never iterated: it declares the access the walk has beyond the roots
 * (DeGlobalTransform write, DeChildOf read), so the scheduler keeps writers of those components
 * apart from this system. The walk reads and writes table rows directly (no access check per
 * entity) and stamps the changed tick of each global it writes with the run's tick, so untouched
 * globals are not reported to DE_CHANGED readers.
 *
 * Arithmetic: de_transform_mul only (+ - * / sqrt; f64 translation), so the module's
 * deterministic build rules hold (SPEC §5.7). */
#include "hierarchy_internal.h"

#include "change_internal.h"
#include "query_internal.h"
#include "system_internal.h"

/** Fewest roots per par_each range. */
#define PROP_MIN_GRAIN 256u
/** Most par_each ranges per run: each range takes a command buffer for the run (DENG-350 keeps
 * at most 4094 per schedule run). The grain grows with the root count above this. */
#define PROP_MAX_RANGES 1024u
/** Initial depth of a range's walk stack (grown by doubling in thread scratch). */
#define PROP_STACK_INITIAL 64u

typedef struct prop_frame {
    de_transform global; /* the parent's global transform */
    de_entity next;      /* the next child to visit */
    bool dirty;          /* the parent was recomputed: its children must be */
} prop_frame;

typedef struct prop_run {
    de_world *w;
    ecs_hier_state *st;
    u32 transform, global, child_of, children; /* component ids */
    u32 last_run, this_run;                    /* change window of the run */
    bool window;                               /* false: every tick counts as changed */
    u32 stamp;                                 /* changed tick of written globals */
} prop_run;

/* The components of one entity that the walk uses (NULL when absent). */
typedef struct node_view {
    const DeTransform *local;
    u32 local_changed;
    DeGlobalTransform *global;
    u32 *global_changed;
    const DeChildOf *child_of;
    u32 child_of_changed;
    const DeChildren *children;
} node_view;

static bool tick_changed(const prop_run *r, u32 tick) {
    return !r->window || ecs_tick_in_window(tick, r->last_run, r->this_run);
}

/* Fills `v` for entity `e`; false if `e` is not alive. */
static bool node_get(const prop_run *r, de_entity e, node_view *v) {
    memset(v, 0, sizeof *v);
    const ecs_record *rec = ecs_entity_record(r->w, e);
    if (rec == NULL)
        return false;
    const ecs_archetype *a = ecs_archetype_at(r->w, rec->archetype);
    const ecs_chunk *ch = ecs_table_chunk(a, rec->chunk);
    const u32 row = rec->row;
    u32 col = ecs_archetype_column(a, r->transform);
    if (col != ECS_INVALID_INDEX) {
        v->local = (const DeTransform *)ecs_chunk_column(a, ch, col) + row;
        v->local_changed = ecs_chunk_changed(a, ch, col)[row];
    }
    col = ecs_archetype_column(a, r->global);
    if (col != ECS_INVALID_INDEX) {
        v->global = (DeGlobalTransform *)ecs_chunk_column(a, ch, col) + row;
        v->global_changed = ecs_chunk_changed(a, ch, col) + row;
    }
    col = ecs_archetype_column(a, r->child_of);
    if (col != ECS_INVALID_INDEX) {
        v->child_of = (const DeChildOf *)ecs_chunk_column(a, ch, col) + row;
        v->child_of_changed = ecs_chunk_changed(a, ch, col)[row];
    }
    col = ecs_archetype_column(a, r->children);
    if (col != ECS_INVALID_INDEX)
        v->children = (const DeChildren *)ecs_chunk_column(a, ch, col) + row;
    return true;
}

/* Per-range counters, added to the world's once per slice. */
typedef struct prop_counts {
    u64 visited, recomputed, added;
} prop_counts;

/* Stores global transform `g` of `e`: in place (stamped changed), or as a deferred add of
 * DeGlobalTransform when `e` has none. */
static void write_global(const prop_run *r, de_entity e, const node_view *v, const de_transform *g,
                         de_commands *cmds, prop_counts *n) {
    n->recomputed += 1u;
    if (v->global != NULL) {
        *v->global = *g;
        *v->global_changed = r->stamp;
        return;
    }
    if (cmds == NULL)
        return; /* outside a system there is no buffer; the global is added on a system run */
    void *payload = de_cmd_add(cmds, e, (de_component){r->global});
    if (payload != NULL) {
        memcpy(payload, g, sizeof *g);
        n->added += 1u;
    }
}

/* Room for one more frame; false (subtree skipped) if the scratch arena is exhausted. */
static bool stack_reserve(de_arena *arena, prop_frame **stack, u32 *cap, u32 n) {
    if (n < *cap)
        return true;
    prop_frame *grown = DE_ARENA_PUSH(arena, prop_frame, (size_t)*cap * 2u);
    if (grown == NULL)
        return false;
    memcpy(grown, *stack, (size_t)n * sizeof(prop_frame));
    *stack = grown;
    *cap *= 2u;
    return true;
}

static void root_slice(const de_query_chunk *qc, void *user) {
    const prop_run *r = (const prop_run *)user;
    prop_counts n = {0, 0, 0};
    de_scratch sc = de_scratch_begin(NULL, 0);
    u32 cap = PROP_STACK_INITIAL, depth = 0;
    prop_frame *stack = DE_ARENA_PUSH(sc.arena, prop_frame, cap);
    if (stack == NULL)
        cap = 0;

    for (u32 i = 0; i < qc->count; ++i) {
        const de_entity root = qc->entities[i];
        node_view v;
        if (!node_get(r, root, &v) || v.local == NULL)
            continue;
        n.visited += 1u;
        const bool dirty = tick_changed(r, v.local_changed) || v.global == NULL;
        de_transform g;
        if (dirty) {
            g = *v.local; /* a root's global is its local */
            write_global(r, root, &v, &g, qc->commands, &n);
        } else {
            g = *v.global;
        }
        if (v.children == NULL || v.children->count == 0 || cap == 0)
            continue;
        stack[0].global = g;
        stack[0].next = v.children->first;
        stack[0].dirty = dirty;
        depth = 1;
        while (depth > 0) {
            prop_frame *top = &stack[depth - 1u];
            const de_entity c = top->next;
            if (c.bits == 0) {
                --depth;
                continue;
            }
            node_view cv;
            if (!node_get(r, c, &cv) || cv.child_of == NULL) {
                top->next = DE_ENTITY_NULL; /* a broken list: end this level */
                continue;
            }
            top->next = cv.child_of->next_sibling;
            if (cv.local == NULL)
                continue; /* no DeTransform: its subtree is not propagated */
            n.visited += 1u;
            const bool cdirty = top->dirty || tick_changed(r, cv.local_changed) ||
                                tick_changed(r, cv.child_of_changed) || cv.global == NULL;
            de_transform cg;
            if (cdirty) {
                cg = de_transform_mul(&top->global, cv.local);
                write_global(r, c, &cv, &cg, qc->commands, &n);
            } else {
                cg = *cv.global;
            }
            if (cv.children == NULL || cv.children->count == 0)
                continue;
            if (!stack_reserve(sc.arena, &stack, &cap, depth))
                continue; /* out of scratch memory: this subtree keeps its old globals */
            stack[depth].global = cg;
            stack[depth].next = cv.children->first;
            stack[depth].dirty = cdirty;
            ++depth;
        }
    }
    de_scratch_end(sc);
    ecs_hier_state *st = r->st;
    de_atomic_fetch_add_u64(&st->visited, n.visited, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&st->recomputed, n.recomputed, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&st->globals_added, n.added, DE_ATOMIC_RELAXED);
}

static void propagate_fn(de_system_ctx *ctx) {
    de_world *w = ctx->world;
    ecs_hier_state *st = w->hier;
    const ecs_query *q = ecs_query_get(w, ctx->queries[0]);
    if (st == NULL || q == NULL)
        return;
    prop_run r;
    memset(&r, 0, sizeof r);
    r.w = w;
    r.st = st;
    r.transform = st->ids.transform.id;
    r.global = st->ids.global_transform.id;
    r.child_of = st->ids.child_of.id;
    r.children = st->ids.children.id;
    r.window = q->change_window_set;
    r.last_run = q->last_run_tick;
    r.this_run = q->this_run_tick;
    r.stamp = ecs_query_write_tick(w, q);
    /* The grain depends only on the root count, so the ranges (and the command-buffer keys of
     * deferred adds) are the same on every machine and for every worker count. */
    const u32 roots = de_query_count(w, ctx->queries[0]);
    u32 grain = roots / PROP_MAX_RANGES + 1u;
    if (grain < PROP_MIN_GRAIN)
        grain = PROP_MIN_GRAIN;
    de_query_par_each(w, ctx->queries[0], grain, root_slice, &r);
    de_atomic_fetch_add_u64(&st->runs, 1u, DE_ATOMIC_RELAXED);
}

de_result ecs_transform_propagate_register(de_world *w, de_system *out) {
    const de_builtin_component_ids *ids = &w->hier->ids;
    const de_query_term root_terms[] = {
        {ids->transform, DE_READ},
        {ids->children, DE_OPTIONAL_READ},
    };
    const de_query_filter root_filters[] = {{ids->child_of, DE_WITHOUT}};
    /* Never iterated: declares the walk's access to descendants. */
    const de_query_term walk_terms[] = {
        {ids->transform, DE_READ},
        {ids->global_transform, DE_OPTIONAL_WRITE},
        {ids->child_of, DE_OPTIONAL_READ},
    };
    const de_query_desc queries[] = {
        {root_terms, 2u, root_filters, 1u},
        {walk_terms, 3u, NULL, 0u},
    };
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE_TRANSFORM_PROPAGATE_SYSTEM;
    d.schedule = DE_SCHEDULE_POST_UPDATE;
    d.fn = propagate_fn;
    /* Not DE_SYSTEM_DETERMINISTIC: that flag warns outside FixedUpdate / Startup. The system
     * still follows SPEC §5.7 (fixed ranges, f64 de_transform_mul, no hash map). */
    d.queries = queries;
    d.query_count = 2u;
    *out = de_system_add(w, &d);
    if (!DE_HANDLE_VALID(*out))
        return de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL;
    return DE_OK;
}
