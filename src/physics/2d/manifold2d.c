/*
 * manifold2d.c: the 2D contacts state and the per-step contact pipeline after the broadphase
 * (DENG-253): chain midphase into sorted sub-pairs, the narrowphase in fixed parallel ranges into
 * per-sub-pair slots, the serial merge into the sorted manifold array (begin / persist / end,
 * warm-start carry-over, ids, per-body lists) and end-touch. See manifold2d.h.
 */
#include "physics/2d/manifold2d.h"

#include <dengine/de_debug.h>
#include <dengine/de_task.h>

/* Slot states (one per sub-pair). */
enum { SLOT_NONE = 0, SLOT_SOLID = 1, SLOT_TRIGGER = 2 };

struct de__contacts2d {
    const de_allocator *alloc;
    f32 speculative;
    u32 max_manifolds; /**< capacity of each manifold array and of the end records */
    u32 max_subpairs;  /**< capacity of the sub-pairs and their slots */
    u32 body_capacity; /**< max_bodies */

    de__contact_key2d *sub; /**< [max_subpairs] this tick's sub-pairs, sorted */
    de__contact_set2d *set; /**< [max_subpairs] slot i: the contact set of sub-pair i */
    u8 *state;              /**< [max_subpairs] SLOT_* */
    u32 sub_count;
    u32 sub_dropped; /**< 1 when the sub-pairs did not fit this tick (the rest were skipped) */

    de__manifold2d *live[2]; /**< live[cur]: the manifolds of the last step, sorted by key */
    u32 cur;
    u32 count;

    de__contact_end2d *ended; /**< [max_manifolds] */
    u32 ended_count;
    bool ended_released; /**< the ids of `ended` are back on the free list */

    u32 *free_ids; /**< LIFO stack of free manifold ids [2 * max_manifolds] */
    u32 free_count;

    u32 *body_start; /**< [body_capacity + 1] */
    u32 *body_items; /**< [2 * max_manifolds] */
    u32 body_count;

    const de_physics2d_world *pass_world; /**< the narrowphase pass in progress */

    de__contact2d_stats stats;
    bool overflow_logged;

    void *block;
    size_t block_size;
};

/* ---- Per-manifold rules --------------------------------------------------------------------- */

void de__manifold2d_carry(de__manifold2d *cur, const de__manifold2d *old) {
    cur->id = old->id;
    cur->flags = (cur->flags & ~(u32)DE__MANIFOLD2D_BEGIN) | (u32)DE__MANIFOLD2D_PERSIST;
    bool used[DE__P2D_MAX_POINTS] = {false};
    bool matched[DE__P2D_MAX_POINTS] = {false};
    const u32 n_old = old->point_count < DE__P2D_MAX_POINTS ? old->point_count : DE__P2D_MAX_POINTS;
    for (u32 i = 0; i < cur->point_count; ++i) {
        de__contact2d *c = &cur->points[i];
        c->normal_impulse = 0.0f;
        c->tangent_impulse = 0.0f;
        for (u32 j = 0; j < n_old; ++j) {
            const de__contact2d *o = &old->points[j];
            if (!used[j] && o->feature_id == c->feature_id) {
                c->normal_impulse = o->normal_impulse;
                c->tangent_impulse = o->tangent_impulse;
                used[j] = true;
                matched[i] = true;
                break;
            }
        }
    }
    for (u32 i = 0; i < cur->point_count; ++i) {
        if (matched[i])
            continue;
        de__contact2d *c = &cur->points[i];
        u32 best = DE__P2D_MAX_POINTS;
        f32 best_d2 = DE__CONTACT2D_CARRY_DIST * DE__CONTACT2D_CARRY_DIST;
        for (u32 j = 0; j < n_old; ++j) {
            if (used[j])
                continue;
            const de_vec2 d = de__v2_sub(c->local_a, old->points[j].local_a);
            const f32 d2 = de__v2_dot(d, d);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = j;
            }
        }
        if (best < DE__P2D_MAX_POINTS) {
            c->normal_impulse = old->points[best].normal_impulse;
            c->tangent_impulse = old->points[best].tangent_impulse;
            used[best] = true;
        }
    }
}

/* ---- Create / destroy ----------------------------------------------------------------------- */

static void contacts_free(de__contacts2d *c) {
    const de_allocator *a = c->alloc;
    if (c->block != NULL)
        DE_FREE(a, c->block, c->block_size);
    DE_FREE(a, c, sizeof *c);
}

static void ext_destroy(de_physics2d_world *p, void *state) {
    (void)p;
    contacts_free((de__contacts2d *)state);
}

static const de__p2d_ext_vtbl k_contacts_vtbl = {ext_destroy, NULL, NULL};

#define CT_ALIGN 16u

static size_t align_up(size_t x) {
    return (x + (CT_ALIGN - 1u)) & ~(size_t)(CT_ALIGN - 1u);
}

de_result de__contacts2d_create(de_physics2d_world *p, const de__contacts2d_desc *desc) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts2d_create: world is NULL");
    if (de__contacts2d_get(p) != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__contacts2d_create: contacts exist");
    de__contacts2d_desc d;
    if (desc != NULL)
        d = *desc;
    else
        memset(&d, 0, sizeof d);
    if (!de__f32_finite(d.speculative) || d.speculative < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts2d_create: bad speculative margin");
    de__bp2d *bp = NULL;
    DE_TRY(de__bp2d_ensure(p, &bp));
    de__bp2d_stats bs;
    de__bp2d_stats_get(p, &bs);

    const u32 capacity = p->bodies.capacity;
    u64 max_m = d.max_manifolds;
    if (max_m == 0) {
        max_m = 2u * (u64)capacity;
        if (max_m < DE__CONTACT2D_MIN_MANIFOLDS)
            max_m = DE__CONTACT2D_MIN_MANIFOLDS;
    }
    const u64 max_s = d.max_subpairs != 0 ? (u64)d.max_subpairs : (u64)bs.max_pairs;
    if (max_m > 0x3FFFFFFFu || max_s > 0x3FFFFFFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts2d_create: capacity too large");

    const de_allocator *a = p->alloc;
    de__contacts2d *c =
        (de__contacts2d *)DE_ALLOC(a, sizeof *c, DE_ALIGNOF(de__contacts2d), DE_MEM_PHYSICS);
    if (c == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__contacts2d_create: state");
    memset(c, 0, sizeof *c);
    c->alloc = a;
    c->speculative = d.speculative > 0.0f ? d.speculative : DE__CONTACT2D_DEFAULT_SPECULATIVE;
    c->max_manifolds = (u32)max_m;
    c->max_subpairs = (u32)max_s;
    c->body_capacity = capacity;
    c->ended_released = true;

    const size_t mm = (size_t)max_m, ms = (size_t)max_s;
    enum { NB = 9 };
    const size_t bytes[NB] = {sizeof(de__contact_key2d) * ms,
                              sizeof(de__contact_set2d) * ms,
                              sizeof(u8) * ms,
                              sizeof(de__manifold2d) * mm,
                              sizeof(de__manifold2d) * mm,
                              sizeof(de__contact_end2d) * mm,
                              sizeof(u32) * 2u * mm,
                              sizeof(u32) * ((size_t)capacity + 1u),
                              sizeof(u32) * 2u * mm};
    size_t off[NB], size = 0;
    for (u32 k = 0; k < NB; ++k) {
        off[k] = align_up(size);
        size = off[k] + bytes[k];
    }
    size = align_up(size);
    u8 *block = (u8 *)DE_ALLOC(a, size, CT_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, c, sizeof *c);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__contacts2d_create: %zu bytes", size);
    }
    memset(block, 0, size);
    c->block = block;
    c->block_size = size;
    c->sub = (de__contact_key2d *)(block + off[0]);
    c->set = (de__contact_set2d *)(block + off[1]);
    c->state = block + off[2];
    c->live[0] = (de__manifold2d *)(block + off[3]);
    c->live[1] = (de__manifold2d *)(block + off[4]);
    c->ended = (de__contact_end2d *)(block + off[5]);
    c->free_ids = (u32 *)(block + off[6]);
    c->body_start = (u32 *)(block + off[7]);
    c->body_items = (u32 *)(block + off[8]);
    /* Ids are handed out from 0 upward: push them in descending order. */
    c->free_count = 2u * c->max_manifolds;
    for (u32 i = 0; i < c->free_count; ++i)
        c->free_ids[i] = c->free_count - 1u - i;
    c->stats.max_manifolds = c->max_manifolds;
    c->stats.max_subpairs = c->max_subpairs;
    c->stats.speculative = c->speculative;
    de__p2d_ext_set(p, DE__P2D_EXT_CONTACTS, c, &k_contacts_vtbl);
    return DE_OK;
}

de_result de__contacts2d_ensure(de_physics2d_world *p, de__contacts2d **out) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts2d_ensure: world is NULL");
    de__contacts2d *c = de__contacts2d_get(p);
    if (c == NULL) {
        DE_TRY(de__contacts2d_create(p, NULL));
        c = de__contacts2d_get(p);
    }
    if (out != NULL)
        *out = c;
    return DE_OK;
}

f32 de__contacts2d_speculative(const de_physics2d_world *p) {
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    return c != NULL ? c->speculative : DE__CONTACT2D_DEFAULT_SPECULATIVE;
}

/* ---- Midphase ------------------------------------------------------------------------------- */

de_aabb2d de__contacts2d_chain_box(const de_physics2d_world *p, u32 chain, u32 other, f32 margin) {
    const de_aabb2d w = de__bp2d_predict_box(p, other);
    const de__d2 pos = p->bodies.position[chain];
    const de_rot2d q = p->bodies.rotation[chain];
    const f64 m = (f64)margin;
    /* Center and half extents of the grown box, then into the chain frame: R^T (c - pos), and
     * the extents through |R^T| (Arvo). */
    const de__d2 c = de__d2_make(((f64)w.min.x + (f64)w.max.x) * 0.5 - pos.x,
                                 ((f64)w.min.y + (f64)w.max.y) * 0.5 - pos.y);
    const f64 ex = ((f64)w.max.x - (f64)w.min.x) * 0.5 + m;
    const f64 ey = ((f64)w.max.y - (f64)w.min.y) * 0.5 + m;
    const f64 qc = (f64)q.c, qs = (f64)q.s;
    const de__d2 lc = de__d2_make(qc * c.x + qs * c.y, -qs * c.x + qc * c.y);
    const f64 ac = de__absd(qc), as = de__absd(qs);
    const f64 lx = ac * ex + as * ey, ly = as * ex + ac * ey;
    return de__aabb2d_from_d(de__d2_make(lc.x - lx, lc.y - ly), de__d2_make(lc.x + lx, lc.y + ly));
}

typedef struct mid_ctx {
    de__contacts2d *c;
    u32 a, b;
} mid_ctx;

static bool push_subpair(de__contacts2d *c, u32 a, u32 b, u32 child) {
    if (c->sub_count >= c->max_subpairs) {
        ++c->sub_dropped;
        return false;
    }
    de__contact_key2d *k = &c->sub[c->sub_count++];
    k->a = a;
    k->b = b;
    k->child = child;
    return true;
}

static bool mid_cb(void *user, u32 segment) {
    mid_ctx *x = (mid_ctx *)user;
    return push_subpair(x->c, x->a, x->b, segment);
}

static void midphase(de_physics2d_world *p, de__contacts2d *c, const de__bp2d_pair *pairs,
                     u32 count) {
    c->sub_count = 0;
    c->sub_dropped = 0;
    const de__body2d_pool *b = &p->bodies;
    for (u32 k = 0; k < count; ++k) {
        const u32 a = pairs[k].a, bb = pairs[k].b;
        const de__shape2d *sa = de__shape2d_get(p, b->shape[a]);
        const de__shape2d *sb = de__shape2d_get(p, b->shape[bb]);
        if (sa == NULL || sb == NULL)
            continue;
        const bool chain_a = sa->kind == DE_SHAPE2D_CHAIN;
        const bool chain_b = sb->kind == DE_SHAPE2D_CHAIN;
        if (chain_a && chain_b)
            continue; /* never collide (and never paired: neither body is dynamic) */
        if (!chain_a && !chain_b) {
            if (!push_subpair(c, a, bb, DE__P2D_CHILD_NONE))
                break;
            continue;
        }
        const u32 chain = chain_a ? a : bb;
        const de__shape2d *cs = chain_a ? sa : sb;
        mid_ctx x = {c, a, bb};
        const de_aabb2d box = de__contacts2d_chain_box(p, chain, chain_a ? bb : a, c->speculative);
        if (!de__chain2d_query(&cs->u.chain, box, mid_cb, &x))
            break;
    }
}

/* ---- Narrowphase pass ----------------------------------------------------------------------- */

static void collide_subpair(de__contacts2d *c, u32 i) {
    const de_physics2d_world *p = c->pass_world;
    const de__contact_key2d k = c->sub[i];
    de__contact_set2d *out = &c->set[i];
    c->state[i] = SLOT_NONE;
    const de__body2d_pool *b = &p->bodies;
    const de__shape2d *sa = de__shape2d_get(p, b->shape[k.a]);
    const de__shape2d *sb = de__shape2d_get(p, b->shape[k.b]);
    if (sa == NULL || sb == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    const bool trigger = ((b->flags[k.a] | b->flags[k.b]) & (u32)DE_BODY_FLAG_TRIGGER) != 0;
    de__xf2d xa, xb;
    xa.p = b->position[k.a];
    xa.q = b->rotation[k.a];
    xb.p = b->position[k.b];
    xb.q = b->rotation[k.b];
    /* DENG-254 may make this per pair (speed-dependent), as DENG-219 did in 3D. Triggers report
     * overlap only: no speculative distance. */
    const f32 spec = trigger ? 0.0f : c->speculative;
    const u32 child = k.child == DE__P2D_CHILD_NONE ? 0u : k.child;
    const u32 n = de__collide2d(sa, xa, sb, xb, child, spec, out);
    if (n == 0)
        return;
    if (!trigger) {
        c->state[i] = SLOT_SOLID;
        return;
    }
    for (u32 j = 0; j < n; ++j)
        if (out->points[j].separation <= 0.0f) {
            c->state[i] = SLOT_TRIGGER;
            return;
        }
}

static void subpair_range(u32 begin, u32 end, void *user) {
    de__contacts2d *c = (de__contacts2d *)user;
    for (u32 i = begin; i < end; ++i)
        collide_subpair(c, i);
}

void de__contacts2d_collide(de_physics2d_world *p) {
    de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (c == NULL)
        return;
    const de__bp2d_pair *pairs = NULL;
    u32 count = 0;
    de__bp2d_pairs(p, &pairs, &count);
    c->stats.pairs = count;
    midphase(p, c, pairs, count);
    c->stats.subpairs = c->sub_count;
    c->pass_world = p;
    if (p->tasks != NULL && c->sub_count > DE__CONTACT2D_PAIR_GRAIN)
        de_task_parallel_for(p->tasks, c->sub_count, DE__CONTACT2D_PAIR_GRAIN, subpair_range, c);
    else
        subpair_range(0, c->sub_count, c);
    c->pass_world = NULL;
}

/* ---- Merge ---------------------------------------------------------------------------------- */

typedef struct merge_ctx {
    de__contacts2d *c;
    const de_physics2d_world *p;
    de__manifold2d *out;
    u32 n_out;
    u32 dropped;
} merge_ctx;

static void end_manifold(merge_ctx *x, const de__manifold2d *old, u32 flags) {
    de__contacts2d *c = x->c;
    DE_ASSERT(c->ended_count < c->max_manifolds);
    de__contact_end2d *e = &c->ended[c->ended_count++];
    e->key = old->key;
    e->gen_a = old->gen_a;
    e->gen_b = old->gen_b;
    e->id = old->id;
    e->flags =
        flags | ((old->flags & (u32)DE__MANIFOLD2D_TRIGGER) ? (u32)DE__CONTACT2D_END_TRIGGER : 0u);
}

/* Appends slot `slot` (sub-pair `key`), matched with last tick's `old` (NULL for a new key). */
static void emit(merge_ctx *x, u32 slot, const de__manifold2d *old) {
    de__contacts2d *c = x->c;
    if (x->n_out >= c->max_manifolds) {
        ++x->dropped;
        if (old != NULL)
            end_manifold(x, old, DE__CONTACT2D_END_OVERFLOW);
        return;
    }
    const de__contact_key2d *k = &c->sub[slot];
    const de__contact_set2d *s = &c->set[slot];
    de__manifold2d *m = &x->out[x->n_out++];
    memset(m, 0, sizeof *m);
    m->key = *k;
    m->gen_a = x->p->bodies.generation[k->a];
    m->gen_b = x->p->bodies.generation[k->b];
    m->normal = s->normal;
    if (c->state[slot] == SLOT_TRIGGER) {
        m->flags = DE__MANIFOLD2D_TRIGGER;
    } else {
        m->point_count = s->point_count;
        for (u32 j = 0; j < s->point_count; ++j) {
            m->points[j].local_a = s->points[j].local_a;
            m->points[j].local_b = s->points[j].local_b;
            m->points[j].separation = s->points[j].separation;
            m->points[j].feature_id = s->points[j].feature_id;
        }
    }
    if (old != NULL) {
        de__manifold2d_carry(m, old);
    } else {
        DE_ASSERT(c->free_count > 0);
        m->id = c->free_ids[--c->free_count];
        m->flags |= DE__MANIFOLD2D_BEGIN;
    }
}

static bool slot_touching(const de__contacts2d *c, u32 slot) {
    return c->state[slot] != SLOT_NONE;
}
static bool slot_trigger(const de__contacts2d *c, u32 slot) {
    return c->state[slot] == SLOT_TRIGGER;
}

/* One body pair: last tick's manifolds old[0..no) and this tick's slots [first, first + ns),
 * both sorted by key. */
static void merge_pair(merge_ctx *x, u32 a, u32 b, const de__manifold2d *old, u32 no, u32 first,
                       u32 ns) {
    de__contacts2d *c = x->c;
    const de_physics2d_world *p = x->p;
    const u32 out_before = x->n_out;
    const u32 ended_before = c->ended_count;
    const bool old_valid = no > 0 && de__body2d_alive(p, a) && de__body2d_alive(p, b) &&
                           p->bodies.generation[a] == old[0].gen_a &&
                           p->bodies.generation[b] == old[0].gen_b;
    if (!old_valid) {
        for (u32 i = 0; i < no; ++i)
            end_manifold(x, &old[i], DE__CONTACT2D_END_REMOVED);
        for (u32 j = first; j < first + ns; ++j)
            if (slot_touching(c, j))
                emit(x, j, NULL);
    } else {
        u32 i = 0, j = first;
        const u32 end = first + ns;
        while (i < no || j < end) {
            if (j < end && !slot_touching(c, j)) {
                ++j;
                continue;
            }
            int r;
            if (i == no)
                r = 1;
            else if (j == end)
                r = -1;
            else
                r = de__contact_key2d_cmp(&old[i].key, &c->sub[j]);
            const bool same_kind =
                r == 0 && ((old[i].flags & (u32)DE__MANIFOLD2D_TRIGGER) != 0) == slot_trigger(c, j);
            if (same_kind) {
                emit(x, j++, &old[i++]);
            } else if (r <= 0) {
                /* Gone, or switched between trigger and solid (an end, then a begin). */
                end_manifold(x, &old[i++], 0u);
            } else {
                emit(x, j++, NULL);
            }
        }
    }
    const bool has = x->n_out > out_before;
    if (has && !old_valid)
        x->out[out_before].flags |= DE__MANIFOLD2D_PAIR_BEGIN;
    if ((!has || !old_valid) && c->ended_count > ended_before)
        c->ended[ended_before].flags |= DE__CONTACT2D_END_PAIR;
}

static void build_body_lists(de__contacts2d *c, const de_physics2d_world *p) {
    const u32 hw = p->bodies.high_water;
    DE_ASSERT(hw <= c->body_capacity);
    u32 *start = c->body_start;
    memset(start, 0, sizeof(u32) * (hw + 1u));
    const de__manifold2d *m = c->live[c->cur];
    for (u32 k = 0; k < c->count; ++k) {
        ++start[m[k].key.a + 1u];
        ++start[m[k].key.b + 1u];
    }
    for (u32 i = 0; i < hw; ++i)
        start[i + 1u] += start[i];
    for (u32 k = 0; k < c->count; ++k) {
        c->body_items[start[m[k].key.a]++] = k;
        c->body_items[start[m[k].key.b]++] = k;
    }
    for (u32 i = hw; i > 0; --i)
        start[i] = start[i - 1u];
    start[0] = 0;
    c->body_count = hw;
}

static u64 ab_key(u32 a, u32 b) {
    return ((u64)a << 32) | (u64)b;
}

static void release_ended(de__contacts2d *c) {
    if (c->ended_released)
        return;
    for (u32 e = 0; e < c->ended_count; ++e) {
        DE_ASSERT(c->free_count < 2u * c->max_manifolds);
        c->free_ids[c->free_count++] = c->ended[e].id;
    }
    c->ended_released = true;
}

de_result de__contacts2d_merge(de_physics2d_world *p) {
    de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (c == NULL)
        return DE_OK;
    /* A tick that skipped end_touch must not lose its ended ids. */
    release_ended(c);
    const de__manifold2d *old = c->live[c->cur];
    const u32 n_old = c->count;
    merge_ctx x;
    x.c = c;
    x.p = p;
    x.out = c->live[c->cur ^ 1u];
    x.n_out = 0;
    x.dropped = c->sub_dropped;
    c->ended_count = 0;
    c->ended_released = false;

    /* Body pairs in ascending (a, b): last tick's manifolds and this tick's sub-pairs. */
    u32 i = 0, k = 0;
    while (i < n_old || k < c->sub_count) {
        const u64 ko = i < n_old ? ab_key(old[i].key.a, old[i].key.b) : UINT64_MAX;
        const u64 kn = k < c->sub_count ? ab_key(c->sub[k].a, c->sub[k].b) : UINT64_MAX;
        const u64 ab = ko < kn ? ko : kn;
        u32 i2 = i;
        while (i2 < n_old && ab_key(old[i2].key.a, old[i2].key.b) == ab)
            ++i2;
        u32 k2 = k;
        while (k2 < c->sub_count && ab_key(c->sub[k2].a, c->sub[k2].b) == ab)
            ++k2;
        merge_pair(&x, (u32)(ab >> 32), (u32)(ab & 0xFFFFFFFFu), old + i, i2 - i, k, k2 - k);
        i = i2;
        k = k2;
    }

    c->cur ^= 1u;
    c->count = x.n_out;
    build_body_lists(c, p);

    de__contact2d_stats *st = &c->stats;
    st->manifolds = c->count;
    st->points = 0;
    st->begins = 0;
    const de__manifold2d *m = c->live[c->cur];
    for (u32 j = 0; j < c->count; ++j) {
        st->points += m[j].point_count;
        if (m[j].flags & (u32)DE__MANIFOLD2D_BEGIN)
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
                "contacts2d: sub-pairs or manifolds dropped (capacity %u manifolds, %u sub-pairs); "
                "raise de__contacts2d_desc",
                c->max_manifolds, c->max_subpairs);
    }
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__contacts2d_merge: %u dropped", x.dropped);
}

void de__contacts2d_end_touch(de_physics2d_world *p) {
    de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (c != NULL)
        release_ended(c);
}

/* ---- Accessors ------------------------------------------------------------------------------ */

void de__contacts2d_manifolds(const de_physics2d_world *p, const de__manifold2d **out, u32 *count) {
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (out != NULL)
        *out = c != NULL ? c->live[c->cur] : NULL;
    if (count != NULL)
        *count = c != NULL ? c->count : 0u;
}

de__manifold2d *de__contacts2d_manifolds_mut(de_physics2d_world *p, u32 *count) {
    de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (count != NULL)
        *count = c != NULL ? c->count : 0u;
    return c != NULL ? c->live[c->cur] : NULL;
}

void de__contacts2d_ended(const de_physics2d_world *p, const de__contact_end2d **out, u32 *count) {
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (out != NULL)
        *out = c != NULL ? c->ended : NULL;
    if (count != NULL)
        *count = c != NULL ? c->ended_count : 0u;
}

void de__contacts2d_body(const de_physics2d_world *p, u32 index, const u32 **out, u32 *count) {
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (c == NULL || index >= c->body_count) {
        if (out != NULL)
            *out = NULL;
        if (count != NULL)
            *count = 0;
        return;
    }
    if (out != NULL)
        *out = c->body_items + c->body_start[index];
    if (count != NULL)
        *count = c->body_start[index + 1u] - c->body_start[index];
}

const de__manifold2d *de__contacts2d_find(const de_physics2d_world *p,
                                          const de__contact_key2d *key) {
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (c == NULL || key == NULL)
        return NULL;
    const de__manifold2d *m = c->live[c->cur];
    u32 lo = 0, hi = c->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        const int r = de__contact_key2d_cmp(&m[mid].key, key);
        if (r == 0)
            return &m[mid];
        if (r < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return NULL;
}

void de__contacts2d_subpairs(const de_physics2d_world *p, const de__contact_key2d **out,
                             u32 *count) {
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (out != NULL)
        *out = c != NULL ? c->sub : NULL;
    if (count != NULL)
        *count = c != NULL ? c->sub_count : 0u;
}

void de__contacts2d_stats_get(const de_physics2d_world *p, de__contact2d_stats *out) {
    memset(out, 0, sizeof *out);
    const de__contacts2d *c = p != NULL ? de__contacts2d_get(p) : NULL;
    if (c != NULL)
        *out = c->stats;
}
