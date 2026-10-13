/*
 * manifold.c: the contacts state (create, destroy, accessors) and the per-manifold rules:
 * warm-start carry-over by feature id and GJK single-point accumulation (DENG-135). See manifold.h.
 */
#include "physics/contact/manifold.h"

#include "physics/ccd_internal.h"

#include <dengine/de_debug.h>

/* ---- Per-manifold rules ------------------------------------------------------------------- */

void de__manifold_carry(de__manifold *cur, const de__manifold *old) {
    cur->id = old->id;
    cur->serial = old->serial;
    cur->flags = (cur->flags & ~(u32)DE__MANIFOLD_BEGIN) | (u32)DE__MANIFOLD_PERSIST;
    bool used[DE__MANIFOLD_MAX_POINTS] = {false};
    bool matched[DE__MANIFOLD_MAX_POINTS] = {false};
    const u32 n_old =
        old->point_count < DE__MANIFOLD_MAX_POINTS ? old->point_count : DE__MANIFOLD_MAX_POINTS;
    /* 1. By feature id. */
    for (u32 i = 0; i < cur->point_count; ++i) {
        de__contact *c = &cur->points[i];
        c->normal_impulse = 0.0f;
        c->tangent_impulse[0] = 0.0f;
        c->tangent_impulse[1] = 0.0f;
        for (u32 j = 0; j < n_old; ++j) {
            const de__contact *o = &old->points[j];
            if (!used[j] && o->feature_id == c->feature_id) {
                c->normal_impulse = o->normal_impulse;
                c->tangent_impulse[0] = o->tangent_impulse[0];
                c->tangent_impulse[1] = o->tangent_impulse[1];
                used[j] = true;
                matched[i] = true;
                break;
            }
        }
    }
    /* 2. DENG-217: a point whose feature id changed (two equal aligned boxes clip to vertex or
     * edge features by a hair) takes the impulses of the nearest unused old point within
     * DE__CONTACT_CARRY_DIST on A; ties go to the lower old index. Without it the warm start of
     * such corners is lost every few ticks and stacks jitter. */
    for (u32 i = 0; i < cur->point_count; ++i) {
        if (matched[i])
            continue;
        de__contact *c = &cur->points[i];
        u32 best = DE__MANIFOLD_MAX_POINTS;
        f32 best_d2 = DE__CONTACT_CARRY_DIST * DE__CONTACT_CARRY_DIST;
        for (u32 j = 0; j < n_old; ++j) {
            if (used[j])
                continue;
            const de_vec3 d = de__v3_sub(c->local_a, old->points[j].local_a);
            const f32 d2 = de__v3_dot(d, d);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = j;
            }
        }
        if (best < DE__MANIFOLD_MAX_POINTS) {
            const de__contact *o = &old->points[best];
            c->normal_impulse = o->normal_impulse;
            c->tangent_impulse[0] = o->tangent_impulse[0];
            c->tangent_impulse[1] = o->tangent_impulse[1];
            used[best] = true;
        }
    }
}

static de_dvec3 to_world(const de__dmat3 *r, de_dvec3 pos, de_vec3 local) {
    return de__d3_add(pos, de__dmat3_mul(r, de__d3_from(local)));
}

/* The feature id `f` with its curved-shape half (type DE__FEAT_SHAPE; A's when both are) replaced
 * by serial index s (1..16383; the narrowphase itself always uses index 0 there). */
static u32 tag_feature(u32 f, u32 s) {
    const u32 half = de__feat(DE__FEAT_SHAPE, 1u + s % 0x3FFFu);
    if (de__feat_type(de__feature_a(f)) == DE__FEAT_SHAPE)
        return de__feature_id(half, de__feature_b(f));
    if (de__feat_type(de__feature_b(f)) == DE__FEAT_SHAPE)
        return de__feature_id(de__feature_a(f), half);
    return f;
}

static bool feature_used(const de__contact *pts, u32 n, u32 f) {
    for (u32 i = 0; i < n; ++i)
        if (pts[i].feature_id == f)
            return true;
    return false;
}

void de__manifold_accumulate(de__manifold *cur, const de__manifold *old, de__pose xa, de__pose xb,
                             f32 speculative) {
    DE_ASSERT(cur->point_count == 1);
    if (cur->point_count != 1)
        return;
    const de__dmat3 ra = de__dmat3_from_quat(xa.rotation);
    const de__dmat3 rb = de__dmat3_from_quat(xb.rotation);
    const de_dvec3 n = de__d3_from(cur->normal);
    de__contact fresh = cur->points[0];
    const de_dvec3 fresh_a = to_world(&ra, xa.position, fresh.local_a);

    /* 1. Old points that are still valid, re-measured at the current poses. */
    de__contact pts[DE__MANIFOLD_MAX_POINTS + 1];
    de_dvec3 world_a[DE__MANIFOLD_MAX_POINTS];
    u32 count = 0;
    const f64 drift2 = (f64)DE__CONTACT_DRIFT * (f64)DE__CONTACT_DRIFT;
    const u32 old_count = old != NULL ? old->point_count : 0u;
    for (u32 i = 0; i < old_count; ++i) {
        de__contact o = old->points[i];
        const de_dvec3 wa = to_world(&ra, xa.position, o.local_a);
        const de_dvec3 wb = to_world(&rb, xb.position, o.local_b);
        const de_dvec3 d = de__d3_sub(wb, wa);
        const f64 sep = de__d3_dot(d, n);
        const de_dvec3 t = de__d3_sub(d, de__d3_scale(n, sep));
        if (de__d3_dot(t, t) > drift2 || sep > (f64)speculative)
            continue;
        o.separation = (f32)sep;
        world_a[count] = wa;
        pts[count++] = o;
    }

    /* 2. The closest surviving point within DE__CONTACT_MERGE_DIST is the same contact: the new
     *    point replaces it and keeps its feature id and impulses. */
    const f64 merge2 = (f64)DE__CONTACT_MERGE_DIST * (f64)DE__CONTACT_MERGE_DIST;
    u32 merged = UINT32_MAX;
    f64 merged_d2 = merge2;
    for (u32 i = 0; i < count; ++i) {
        const de_dvec3 dm = de__d3_sub(world_a[i], fresh_a);
        const f64 d2 = de__d3_dot(dm, dm);
        if (d2 <= merged_d2 && (merged == UINT32_MAX || d2 < merged_d2)) {
            merged = i;
            merged_d2 = d2;
        }
    }
    u32 serial = cur->serial;
    if (merged != UINT32_MAX) {
        fresh.feature_id = pts[merged].feature_id;
        fresh.normal_impulse = pts[merged].normal_impulse;
        fresh.tangent_impulse[0] = pts[merged].tangent_impulse[0];
        fresh.tangent_impulse[1] = pts[merged].tangent_impulse[1];
        for (u32 i = merged; i + 1 < count; ++i)
            pts[i] = pts[i + 1];
        --count;
    } else {
        /* A fresh id, unique among the kept points (at most 4 are taken). */
        u32 f = tag_feature(fresh.feature_id, serial++);
        for (u32 tries = 0; tries < 8u && feature_used(pts, count, f); ++tries)
            f = tag_feature(fresh.feature_id, serial++);
        fresh.feature_id = f;
        fresh.normal_impulse = 0.0f;
        fresh.tangent_impulse[0] = 0.0f;
        fresh.tangent_impulse[1] = 0.0f;
    }
    pts[count++] = fresh;
    cur->serial = serial;

    /* 3. At most four: the deepest and the largest patch (in A's frame). */
    if (count > DE__MANIFOLD_MAX_POINTS) {
        const de_dvec3 nl = de__dmat3_tmul(&ra, n);
        const de_vec3 na = de__v3((f32)nl.x, (f32)nl.y, (f32)nl.z);
        de_vec3 pos[DE__MANIFOLD_MAX_POINTS + 1];
        f32 sep[DE__MANIFOLD_MAX_POINTS + 1];
        u32 feat[DE__MANIFOLD_MAX_POINTS + 1];
        for (u32 i = 0; i < count; ++i) {
            pos[i] = pts[i].local_a;
            sep[i] = pts[i].separation;
            feat[i] = pts[i].feature_id;
        }
        u32 keep[DE__MANIFOLD_MAX_POINTS];
        const u32 k = de__contact_reduce(pos, sep, feat, count, na, keep);
        for (u32 i = 0; i < k; ++i)
            cur->points[i] = pts[keep[i]];
        cur->point_count = k;
    } else {
        for (u32 i = 0; i < count; ++i)
            cur->points[i] = pts[i];
        cur->point_count = count;
    }
}

/* ---- Per-pair speculative distance (DENG-219) -------------------------------------------- */

f32 de__contacts_pair_speculative(const de_physics_world *p, const de__contacts *c, u32 a, u32 b) {
    const f32 base = c->speculative;
    const de__ccd *ccd = de__ccd_current(p);
    if (ccd == NULL || !(ccd->settings.speculative_max > base))
        return base;
    /* The fastest any point of B can approach any point of A along any direction. */
    const de_vec3 dv = de__v3_sub(ccd->velocity[b], ccd->velocity[a]);
    const f32 bound = de_dsqrt(de__v3_dot(dv, dv)) + ccd->reach[a] + ccd->reach[b];
    const f32 d = bound * ccd->dt;
    if (!(d > base))
        return base;
    return de__minf(d, ccd->settings.speculative_max);
}

/* Velocity of body i's material point at world offset r from its center of mass. */
static de_vec3 point_velocity(const de_physics_world *p, const de__ccd *ccd, u32 i, de_vec3 local) {
    const de__body_pool *b = &p->bodies;
    const de_vec3 w = b->angular_velocity[i];
    const de_vec3 v = ccd->velocity[i];
    if (w.x == 0.0f && w.y == 0.0f && w.z == 0.0f)
        return v;
    const de_vec3 r = de__quat_rotate(b->rotation[i], de__v3_sub(local, b->local_com[i]));
    return de__v3_add(v, de__v3_cross(w, r));
}

void de__contacts_filter_speculative(const de_physics_world *p, const de__contacts *c, u32 a, u32 b,
                                     de__contact_set *sets, u32 n) {
    const de__ccd *ccd = de__ccd_current(p);
    if (ccd == NULL)
        return;
    const f32 base = c->speculative;
    const f32 dt = ccd->dt;
    /* 0 = not asked yet, 1 = they may touch, 2 = they stay apart this step. */
    u32 touch = 0;
    for (u32 s = 0; s < n; ++s) {
        de__contact_set *set = &sets[s];
        const de_vec3 nrm = set->normal; /* from A toward B */
        u32 w = 0;
        for (u32 k = 0; k < set->point_count; ++k) {
            const de__contact_point *pt = &set->points[k];
            bool keep = pt->separation <= base;
            if (!keep) {
                const de_vec3 va = point_velocity(p, ccd, a, pt->local_a);
                const de_vec3 vb = point_velocity(p, ccd, b, pt->local_b);
                const f32 approach = -de__v3_dot(de__v3_sub(vb, va), nrm);
                keep = pt->separation <= approach * dt;
                if (keep) {
                    /* Closing along this normal; do the shapes really get there? */
                    if (touch == 0)
                        touch = de__ccd_pair_may_touch(p, a, b) ? 1u : 2u;
                    keep = touch == 1u;
                }
            }
            if (keep)
                set->points[w++] = *pt;
        }
        set->point_count = w;
    }
}

/* ---- State ---------------------------------------------------------------------------------- */

static void contacts_free(de__contacts *c) {
    const de_allocator *a = c->alloc;
    if (c->block != NULL)
        DE_FREE(a, c->block, c->block_size);
    DE_FREE(a, c, sizeof *c);
}

static void ext_destroy(de_physics_world *p, void *state) {
    (void)p;
    contacts_free((de__contacts *)state);
}

static const de__phys_ext_vtbl k_contacts_vtbl = {ext_destroy, NULL, NULL};

/* Registers (or finds) the cvars; registration takes the registry lock, so only at create. */
static de_cvar speculative_cvar(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__CONTACT_SPECULATIVE_CVAR;
    d.help = "Physics speculative contact margin in meters: points closer than this are kept "
             "(read when the contacts are created)";
    d.type = DE_CVAR_FLOAT;
    d.default_number = (f64)DE__CONTACT_DEFAULT_SPECULATIVE;
    d.min = 0.0;
    d.max = 1.0;
    return de_cvar_register(&d);
}

static de_cvar max_manifolds_cvar(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__CONTACT_MAX_MANIFOLDS_CVAR;
    d.help = "Physics contact manifold capacity (read when the contacts are created; 0 = "
             "max(1024, 2 * max_bodies))";
    d.type = DE_CVAR_INT;
    d.default_number = 0.0;
    d.min = 0.0;
    d.max = 67108864.0;
    return de_cvar_register(&d);
}

#define CT_ALIGN 16u

static size_t align_up(size_t x) {
    return (x + (CT_ALIGN - 1u)) & ~(size_t)(CT_ALIGN - 1u);
}

de_result de__contacts_create(de_physics_world *p, const de__contacts_desc *desc) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts_create: world is NULL");
    if (de__contacts_get(p) != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__contacts_create: contacts exist");
    de__contacts_desc d;
    if (desc != NULL)
        d = *desc;
    else
        memset(&d, 0, sizeof d);
    if (!de__f32_finite(d.speculative) || d.speculative < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts_create: bad speculative margin");

    const de_cvar scv = speculative_cvar();
    const de_cvar mcv = max_manifolds_cvar();
    f32 spec = d.speculative;
    if (spec == 0.0f)
        spec = DE_HANDLE_VALID(scv) ? (f32)de_cvar_float(scv) : DE__CONTACT_DEFAULT_SPECULATIVE;
    if (!de__f32_finite(spec) || spec < 0.0f)
        spec = DE__CONTACT_DEFAULT_SPECULATIVE;
    const u32 capacity = p->bodies.capacity;
    u64 max_m = d.max_manifolds;
    if (max_m == 0 && DE_HANDLE_VALID(mcv) && de_cvar_int(mcv) > 0)
        max_m = (u64)de_cvar_int(mcv);
    if (max_m == 0) {
        max_m = 2u * (u64)capacity;
        if (max_m < DE__CONTACT_MIN_MANIFOLDS)
            max_m = DE__CONTACT_MIN_MANIFOLDS;
    }
    if (max_m > 0x3FFFFFFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts_create: max_manifolds too large");

    de__broadphase *bp = NULL;
    const de_result br = de__broadphase_ensure(p, &bp);
    if (br != DE_OK)
        return br;
    de__bp_stats bs;
    de__broadphase_stats(p, &bs);

    const de_allocator *a = p->alloc;
    de__contacts *c =
        (de__contacts *)DE_ALLOC(a, sizeof *c, DE_ALIGNOF(de__contacts), DE_MEM_PHYSICS);
    if (c == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__contacts_create: state");
    memset(c, 0, sizeof *c);
    c->alloc = a;
    c->speculative = spec;
    c->max_manifolds = (u32)max_m;
    c->max_pairs = bs.max_pairs;
    c->body_capacity = capacity;
    de_atomic_init_u64(&c->pool_next, 0);
    de_atomic_init_u32(&c->pool_full, 0);
    de_atomic_init_u32(&c->sets_dropped, 0);

    /* One block: live[0], live[1], pool, slots, ended, free ids, body start, body items. Large
     * arrays are not cleared: every entry is written before it is read. */
    const size_t mm = (size_t)max_m;
    enum { N = 8 };
    size_t off[N], size = 0;
    const size_t bytes[N] = {sizeof(de__manifold) * mm,     sizeof(de__manifold) * mm,
                             sizeof(de__manifold) * mm,     sizeof(de__contact_slot) * bs.max_pairs,
                             sizeof(de__contact_end) * mm,  sizeof(u32) * 2u * mm,
                             sizeof(u32) * (capacity + 1u), sizeof(u32) * 2u * mm};
    for (u32 k = 0; k < N; ++k) {
        off[k] = align_up(size);
        size = off[k] + bytes[k];
    }
    size = align_up(size);
    u8 *block = (u8 *)DE_ALLOC(a, size, CT_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, c, sizeof *c);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__contacts_create: %zu bytes of manifolds", size);
    }
    c->block = block;
    c->block_size = size;
    c->live[0] = (de__manifold *)(block + off[0]);
    c->live[1] = (de__manifold *)(block + off[1]);
    c->pool = (de__manifold *)(block + off[2]);
    c->slots = (de__contact_slot *)(block + off[3]);
    c->ended = (de__contact_end *)(block + off[4]);
    c->free_ids = (u32 *)(block + off[5]);
    c->body_start = (u32 *)(block + off[6]);
    c->body_items = (u32 *)(block + off[7]);
    /* Ids pop in ascending order: 0 first. */
    c->free_count = 2u * (u32)max_m;
    for (u32 k = 0; k < c->free_count; ++k)
        c->free_ids[k] = c->free_count - 1u - k;
    c->body_start[0] = 0;
    c->stats.max_manifolds = c->max_manifolds;
    c->stats.speculative = spec;

    de__phys_ext_set(p, DE__PHYS_EXT_CONTACTS, c, &k_contacts_vtbl);
    return DE_OK;
}

de_result de__contacts_ensure(de_physics_world *p, de__contacts **out) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__contacts_ensure: world is NULL");
    de__contacts *c = de__contacts_get(p);
    if (c == NULL) {
        const de_result r = de__contacts_create(p, NULL);
        if (r != DE_OK)
            return r;
        c = de__contacts_get(p);
    }
    if (out != NULL)
        *out = c;
    return DE_OK;
}

f32 de__contacts_speculative(const de_physics_world *p) {
    const de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    return c != NULL ? c->speculative : DE__CONTACT_DEFAULT_SPECULATIVE;
}

/* ---- Iteration API ------------------------------------------------------------------------ */

void de__contacts_manifolds(const de_physics_world *p, const de__manifold **out, u32 *count) {
    const de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (out != NULL)
        *out = c != NULL ? c->live[c->cur] : NULL;
    if (count != NULL)
        *count = c != NULL ? c->count : 0u;
}

de__manifold *de__contacts_manifolds_mut(de_physics_world *p, u32 *count) {
    de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (count != NULL)
        *count = c != NULL ? c->count : 0u;
    return c != NULL ? c->live[c->cur] : NULL;
}

void de__contacts_ended(const de_physics_world *p, const de__contact_end **out, u32 *count) {
    const de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (out != NULL)
        *out = c != NULL ? c->ended : NULL;
    if (count != NULL)
        *count = c != NULL ? c->ended_count : 0u;
}

void de__contacts_body(const de_physics_world *p, u32 index, const u32 **out, u32 *count) {
    const de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
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

const de__manifold *de__contacts_find(const de_physics_world *p, const de__contact_key *key) {
    const de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (c == NULL || key == NULL)
        return NULL;
    const de__manifold *m = c->live[c->cur];
    u32 lo = 0, hi = c->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        const int r = de__contact_key_cmp(&m[mid].key, key);
        if (r == 0)
            return &m[mid];
        if (r < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return NULL;
}

void de__contacts_stats(const de_physics_world *p, de__contact_stats *out) {
    memset(out, 0, sizeof *out);
    const de__contacts *c = p != NULL ? de__contacts_get(p) : NULL;
    if (c == NULL) {
        out->speculative = DE__CONTACT_DEFAULT_SPECULATIVE;
        return;
    }
    *out = c->stats;
}
