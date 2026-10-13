/*
 * pair_cache.c: the per-step contact pipeline after the broadphase (DENG-135): the narrowphase
 * over the pair list in fixed parallel ranges, and the serial merge into the sorted manifold
 * array (begin / persist / end, warm-start carry-over, accumulation, ids, per-body lists). See
 * manifold.h.
 */
#include "physics/contact/manifold.h"

#include <dengine/de_debug.h>
#include <dengine/de_task.h>

/* ---- Narrowphase pass ----------------------------------------------------------------------- */

static u64 ab_key(u32 a, u32 b) {
    return ((u64)a << 32) | (u64)b;
}

/* Kind of the shape that touches: the compound child when there is one. */
static de_shape_kind leaf_kind(const de__shape *s, u32 child) {
    if (s->kind == DE_SHAPE_COMPOUND && child != DE__SUB_NONE && child < s->u.compound.child_count)
        return s->u.compound.children[child].shape->kind;
    return s->kind;
}

/* GJK single-point pairs: a cylinder against anything but a sphere (sphere contacts are single
 * points by nature), reported with a curved-shape feature. */
static bool wants_accumulation(const de__shape *sa, const de__shape *sb, const de__contact_set *s) {
    if (s->point_count != 1)
        return false;
    const de_shape_kind ka = leaf_kind(sa, s->child_a), kb = leaf_kind(sb, s->child_b);
    if (ka == DE_SHAPE_SPHERE || kb == DE_SHAPE_SPHERE)
        return false;
    if (ka != DE_SHAPE_CYLINDER && kb != DE_SHAPE_CYLINDER)
        return false;
    const u32 f = s->points[0].feature_id;
    return de__feat_type(de__feature_a(f)) == DE__FEAT_SHAPE ||
           de__feat_type(de__feature_b(f)) == DE__FEAT_SHAPE;
}

/* Reserves n pool entries: in pair order on the serial pass, by atomic add on the parallel one
 * (the position is never observed). UINT32_MAX when they do not fit. */
static u32 reserve(de__contacts *c, u32 n) {
    if (c->pass_serial) {
        const u64 first = de_atomic_load_u64(&c->pool_next, DE_ATOMIC_RELAXED);
        /* Once full, every later pair gets nothing: the lowest keys are kept. */
        if (de_atomic_load_u32(&c->pool_full, DE_ATOMIC_RELAXED) != 0 ||
            first + n > c->max_manifolds) {
            de_atomic_store_u32(&c->pool_full, 1u, DE_ATOMIC_RELAXED);
            c->pass_dropped += n;
            return UINT32_MAX;
        }
        de_atomic_store_u64(&c->pool_next, first + n, DE_ATOMIC_RELAXED);
        return (u32)first;
    }
    const u64 first = de_atomic_fetch_add_u64(&c->pool_next, n, DE_ATOMIC_RELAXED);
    if (first + n > c->max_manifolds) {
        de_atomic_store_u32(&c->pool_full, 1u, DE_ATOMIC_RELAXED);
        return UINT32_MAX;
    }
    return (u32)first;
}

static void collide_pair(de__contacts *c, u32 i) {
    const de_physics_world *p = c->pass_world;
    const de__bp_pair pr = c->pass_pairs[i];
    de__contact_slot *slot = &c->slots[i];
    slot->first = 0;
    slot->count = 0;
    const de__body_pool *b = &p->bodies;
    const de__shape *sa = de__shape_get(p, b->shape[pr.a]);
    const de__shape *sb = de__shape_get(p, b->shape[pr.b]);
    if (sa == NULL || sb == NULL)
        return;
    const bool trigger = ((b->flags[pr.a] | b->flags[pr.b]) & (u32)DE_BODY_FLAG_TRIGGER) != 0;
    de__pose xa, xb;
    xa.position = b->position[pr.a];
    xa.rotation = b->rotation[pr.a];
    xb.position = b->position[pr.b];
    xb.rotation = b->rotation[pr.b];

    de__contact_set sets[DE__CONTACT_PAIR_MAX_SETS];
    de__contact_buffer buf;
    buf.sets = sets;
    buf.capacity = DE__CONTACT_PAIR_MAX_SETS;
    buf.count = 0;
    buf.overflow = false;
    /* Triggers report overlap only: no speculative distance. */
    const u32 n = de__collide(sa, xa, sb, xb, trigger ? 0.0f : c->speculative, &buf);
    if (buf.overflow)
        (void)de_atomic_fetch_add_u32(&c->sets_dropped, 1u, DE_ATOMIC_RELAXED);
    u32 m = 0;
    for (u32 k = 0; k < n; ++k)
        if (sets[k].point_count > 0)
            ++m;
    if (m == 0)
        return;
    const u32 first = reserve(c, m);
    if (first == UINT32_MAX)
        return;

    /* A's rotation in f64, for the normal in A's frame (reduction works on A's anchors). */
    const de__dmat3 ra = de__dmat3_from_quat(xa.rotation);
    de__manifold *out = c->pool + first;
    u32 w = 0;
    for (u32 k = 0; k < n; ++k) {
        const de__contact_set *s = &sets[k];
        if (s->point_count == 0)
            continue;
        de__manifold *mf = &out[w++];
        memset(mf, 0, sizeof *mf);
        mf->key.a = pr.a;
        mf->key.b = pr.b;
        mf->key.child_a = s->child_a;
        mf->key.sub_a = s->sub_a;
        mf->key.child_b = s->child_b;
        mf->key.sub_b = s->sub_b;
        mf->gen_a = b->generation[pr.a];
        mf->gen_b = b->generation[pr.b];
        mf->normal = s->normal;
        if (trigger) {
            mf->flags = DE__MANIFOLD_TRIGGER;
            continue;
        }
        if (wants_accumulation(sa, sb, s))
            mf->flags |= DE__MANIFOLD_ACCUMULATE;
        u32 keep[DE__MANIFOLD_MAX_POINTS];
        u32 kept = s->point_count;
        if (kept > DE__MANIFOLD_MAX_POINTS) {
            de_vec3 pos[DE__CONTACT_MAX_POINTS];
            f32 sep[DE__CONTACT_MAX_POINTS];
            u32 feat[DE__CONTACT_MAX_POINTS];
            for (u32 j = 0; j < s->point_count; ++j) {
                pos[j] = s->points[j].local_a;
                sep[j] = s->points[j].separation;
                feat[j] = s->points[j].feature_id;
            }
            const de_dvec3 nl = de__dmat3_tmul(&ra, de__d3_from(s->normal));
            kept = de__contact_reduce(pos, sep, feat, s->point_count,
                                      de__v3((f32)nl.x, (f32)nl.y, (f32)nl.z), keep);
        } else {
            for (u32 j = 0; j < kept; ++j)
                keep[j] = j;
        }
        mf->point_count = kept;
        for (u32 j = 0; j < kept; ++j) {
            const de__contact_point *src = &s->points[keep[j]];
            de__contact *dst = &mf->points[j];
            dst->local_a = src->local_a;
            dst->local_b = src->local_b;
            dst->separation = src->separation;
            dst->feature_id = src->feature_id;
        }
    }

    /* The narrowphase visits sub-shapes in its canonical order; manifolds are kept in key order.
     * Insertion sort: stable, and already-sorted runs (the usual case) cost one pass. */
    for (u32 x = 1; x < m; ++x) {
        if (de__contact_key_cmp(&out[x - 1].key, &out[x].key) <= 0)
            continue;
        const de__manifold t = out[x];
        u32 y = x;
        while (y > 0 && de__contact_key_cmp(&out[y - 1].key, &t.key) > 0) {
            out[y] = out[y - 1];
            --y;
        }
        out[y] = t;
    }
    slot->first = first;
    slot->count = m;
}

static void pair_range(u32 begin, u32 end, void *user) {
    de__contacts *c = (de__contacts *)user;
    for (u32 i = begin; i < end; ++i)
        collide_pair(c, i);
}

void de__contacts_collide(de_physics_world *p, const de__bp_pair *pairs, u32 count) {
    de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (c == NULL)
        return;
    DE_ASSERT(count <= c->max_pairs);
    if (count > c->max_pairs)
        count = c->max_pairs;
    c->pass_world = p;
    c->pass_pairs = pairs;
    de_atomic_store_u64(&c->pool_next, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&c->pool_full, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&c->sets_dropped, 0, DE_ATOMIC_RELAXED);
    c->pass_dropped = 0;
    c->stats.pairs = count;

    if (p->tasks != NULL && count > DE__CONTACT_PAIR_GRAIN) {
        c->pass_serial = false;
        de_task_parallel_for(p->tasks, count, DE__CONTACT_PAIR_GRAIN, pair_range, c);
        if (de_atomic_load_u32(&c->pool_full, DE_ATOMIC_RELAXED) == 0)
            return;
        /* Which pairs lost their reservation depends on thread timing: redo the pass in pair
         * order so the lowest keys are kept, exactly as without a task system. */
        ++c->stats.serial_reruns;
        de_atomic_store_u64(&c->pool_next, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&c->pool_full, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&c->sets_dropped, 0, DE_ATOMIC_RELAXED);
    }
    c->pass_serial = true;
    pair_range(0, count, c);
}

/* ---- Merge ---------------------------------------------------------------------------------- */

typedef struct merge_ctx {
    de__contacts *c;
    const de_physics_world *p;
    de__manifold *out;
    u32 n_out;
    u32 dropped;
} merge_ctx;

static void end_manifold(merge_ctx *x, const de__manifold *old, u32 flags) {
    de__contacts *c = x->c;
    DE_ASSERT(c->ended_count < c->max_manifolds);
    de__contact_end *e = &c->ended[c->ended_count++];
    e->key = old->key;
    e->gen_a = old->gen_a;
    e->gen_b = old->gen_b;
    e->id = old->id;
    e->flags =
        flags | ((old->flags & (u32)DE__MANIFOLD_TRIGGER) ? (u32)DE__CONTACT_END_TRIGGER : 0u);
}

/* Appends candidate `cand`, matched with last tick's `old` (NULL for a new key). */
static void emit(merge_ctx *x, const de__manifold *cand, const de__manifold *old) {
    de__contacts *c = x->c;
    if (x->n_out >= c->max_manifolds) {
        ++x->dropped;
        if (old != NULL)
            end_manifold(x, old, DE__CONTACT_END_OVERFLOW);
        return;
    }
    de__manifold *m = &x->out[x->n_out++];
    *m = *cand;
    m->flags &= (u32)(DE__MANIFOLD_TRIGGER | DE__MANIFOLD_ACCUMULATE);
    if (old != NULL) {
        de__manifold_carry(m, old);
    } else {
        DE_ASSERT(c->free_count > 0);
        m->id = c->free_ids[--c->free_count];
        m->serial = 0;
        m->flags |= DE__MANIFOLD_BEGIN;
    }
    if (m->flags & (u32)DE__MANIFOLD_ACCUMULATE) {
        const de__body_pool *b = &x->p->bodies;
        de__pose xa, xb;
        xa.position = b->position[m->key.a];
        xa.rotation = b->rotation[m->key.a];
        xb.position = b->position[m->key.b];
        xb.rotation = b->rotation[m->key.b];
        de__manifold_accumulate(m, old, xa, xb, c->speculative);
    }
}

/* One body pair: last tick's manifolds old[0..no) and this tick's candidates cand[0..nc), both
 * sorted by key. */
static void merge_pair(merge_ctx *x, u32 a, u32 b, const de__manifold *old, u32 no,
                       const de__manifold *cand, u32 nc) {
    const de_physics_world *p = x->p;
    const u32 out_before = x->n_out;
    const u32 ended_before = x->c->ended_count;
    /* Last tick's manifolds belong to the same two bodies only when both are alive with the same
     * generations; otherwise a body was destroyed (and maybe its index reused). */
    const bool old_valid = no > 0 && de__body_alive(p, a) && de__body_alive(p, b) &&
                           p->bodies.generation[a] == old[0].gen_a &&
                           p->bodies.generation[b] == old[0].gen_b;
    if (!old_valid) {
        for (u32 i = 0; i < no; ++i)
            end_manifold(x, &old[i], DE__CONTACT_END_REMOVED);
        for (u32 j = 0; j < nc; ++j)
            emit(x, &cand[j], NULL);
    } else {
        u32 i = 0, j = 0;
        while (i < no || j < nc) {
            int r;
            if (i == no)
                r = 1;
            else if (j == nc)
                r = -1;
            else
                r = de__contact_key_cmp(&old[i].key, &cand[j].key);
            if (r == 0 && ((old[i].flags ^ cand[j].flags) & (u32)DE__MANIFOLD_TRIGGER) == 0) {
                emit(x, &cand[j++], &old[i++]);
            } else if (r <= 0) {
                /* Gone, or switched between trigger and solid (an end, then a begin). */
                end_manifold(x, &old[i++], 0u);
            } else {
                emit(x, &cand[j++], NULL);
            }
        }
    }
    const bool has = x->n_out > out_before;
    if (has && !old_valid)
        x->out[out_before].flags |= DE__MANIFOLD_PAIR_BEGIN;
    if ((!has || !old_valid) && x->c->ended_count > ended_before)
        x->c->ended[ended_before].flags |= DE__CONTACT_END_PAIR;
}

static void build_body_lists(de__contacts *c, const de_physics_world *p) {
    const u32 hw = p->bodies.high_water;
    DE_ASSERT(hw <= c->body_capacity);
    u32 *start = c->body_start;
    memset(start, 0, sizeof(u32) * (hw + 1u));
    const de__manifold *m = c->live[c->cur];
    for (u32 k = 0; k < c->count; ++k) {
        ++start[m[k].key.a + 1u];
        ++start[m[k].key.b + 1u];
    }
    for (u32 i = 0; i < hw; ++i)
        start[i + 1u] += start[i];
    /* Fill in manifold order (each list ascending); start[i] advances to the end of list i ... */
    for (u32 k = 0; k < c->count; ++k) {
        c->body_items[start[m[k].key.a]++] = k;
        c->body_items[start[m[k].key.b]++] = k;
    }
    /* ... which is where list i + 1 begins: shift back. */
    for (u32 i = hw; i > 0; --i)
        start[i] = start[i - 1u];
    start[0] = 0;
    c->body_count = hw;
}

de_result de__contacts_merge(de_physics_world *p, const de__bp_pair *pairs, u32 count) {
    de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (c == NULL)
        return DE_OK;
    if (count > c->max_pairs)
        count = c->max_pairs;
    const de__manifold *old = c->live[c->cur];
    const u32 n_old = c->count;
    merge_ctx x;
    x.c = c;
    x.p = p;
    x.out = c->live[c->cur ^ 1u];
    x.n_out = 0;
    /* The pass ran serially whenever something did not fit, so these counts are exact. */
    x.dropped = de_atomic_load_u32(&c->sets_dropped, DE_ATOMIC_RELAXED) + c->pass_dropped;
    c->ended_count = 0;

    /* Body pairs in ascending (a, b): last tick's manifolds and this tick's pairs. */
    u32 i = 0, k = 0;
    while (i < n_old || k < count) {
        const u64 ko = i < n_old ? ab_key(old[i].key.a, old[i].key.b) : UINT64_MAX;
        const u64 kn = k < count ? ab_key(pairs[k].a, pairs[k].b) : UINT64_MAX;
        const u64 ab = ko < kn ? ko : kn;
        u32 i2 = i;
        while (i2 < n_old && ab_key(old[i2].key.a, old[i2].key.b) == ab)
            ++i2;
        const de__manifold *cand = NULL;
        u32 nc = 0;
        if (kn == ab) {
            cand = c->pool + c->slots[k].first;
            nc = c->slots[k].count;
            ++k;
        }
        merge_pair(&x, (u32)(ab >> 32), (u32)(ab & 0xFFFFFFFFu), old + i, i2 - i, cand, nc);
        i = i2;
    }

    /* Ended ids are free from the next tick on (never reused within the tick that ended them). */
    for (u32 e = 0; e < c->ended_count; ++e) {
        DE_ASSERT(c->free_count < 2u * c->max_manifolds);
        c->free_ids[c->free_count++] = c->ended[e].id;
    }
    c->cur ^= 1u;
    c->count = x.n_out;
    build_body_lists(c, p);

    de__contact_stats *st = &c->stats;
    st->manifolds = c->count;
    st->points = 0;
    st->begins = 0;
    const de__manifold *m = c->live[c->cur];
    for (u32 j = 0; j < c->count; ++j) {
        st->points += m[j].point_count;
        if (m[j].flags & (u32)DE__MANIFOLD_BEGIN)
            ++st->begins;
    }
    st->ends = c->ended_count;
    st->dropped = x.dropped;
    if (x.dropped == 0) {
        c->overflow_logged = false;
        return DE_OK;
    }
    ++st->overflow_ticks;
    if (!c->overflow_logged) {
        c->overflow_logged = true;
        DE_WARN(de__physics_log(),
                "contacts: %u manifolds or contact sets dropped (capacity %u manifolds, %u sets "
                "per pair); raise cvar " DE__CONTACT_MAX_MANIFOLDS_CVAR,
                x.dropped, c->max_manifolds, DE__CONTACT_PAIR_MAX_SETS);
    }
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__contacts_merge: %u manifolds dropped", x.dropped);
}
