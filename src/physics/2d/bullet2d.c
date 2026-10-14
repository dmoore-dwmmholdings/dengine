/*
 * bullet2d.c: time of impact of 2D bullets (DENG-254; the 2D port of the bullet pass of
 * physics/ccd.c, DENG-219). Called after the solver stored its impulses and before finalize.
 *
 * Every awake dynamic body with DE_BODY2D_FLAG_BULLET (not a trigger), in ascending body index, is
 * swept from its start pose to its end pose: its center of mass along the line c0 -> c0 + dp, its
 * angle at a constant rate from 0 to `da` (the angle the solver turned it). The targets are the
 * static and kinematic bodies whose broadphase box meets the path (kinematic bodies at their end
 * pose), each held still; chains segment by segment (one-sided: a segment the bullet starts behind
 * is skipped). Bullets are not swept against dynamic bodies; their contacts handle those.
 *
 * Conservative advancement on the exact distance of the two cores (each shape is a convex core of
 * 1 to 8 points plus a radius; the distance is the minimum over core edge pairs of the f64 segment
 * distance, 0 when a core point lies inside the other core) finds the first time t the gap falls
 * to DE__BULLET2D_LINEAR_SLOP, in at most DE__BULLET2D_MAX_ITER steps per pair. Each step advances
 * t by gap / (closing speed along the normal + |da| * the largest core distance from the center
 * of mass), which never passes the contact.
 *
 * A hit counts unless the solver already held the bullet: its end pose is on the same side of the
 * target (same separating direction), at most the slop deep, and not closing on it by more than the
 * slop per step. If the earliest counted t < 1, the bullet's solver body is moved back to its pose
 * at t (dp and da scaled by t) and keeps its velocity; the next step's speculative contact (a
 * positive gap closed rigidly by the solver) stops it.
 *
 * A target already within the slop at t = 0 is swept against a small circle at the bullet's center
 * of mass instead (radius DE__BULLET2D_FALLBACK_SCALE times the smallest half extent, Box2D v3),
 * which may close half its starting gap and may not enter the target. So bullets resting or
 * sliding on something keep moving, and a bullet pressed into a thin wall still cannot get its
 * center through.
 *
 * Determinism (SPEC §5.7): f64 + - * /, de_dsqrt64 and de_dsincos; bullets in ascending index; the
 * earliest time over the candidates does not depend on the broadphase visiting order. Runs on the
 * step's thread; allocates nothing.
 */
#include "physics/2d/broadphase2d.h"
#include "physics/2d/narrow2d.h"
#include "physics/2d/solver2d_internal.h"

/* ================================ Cores ================================================ */

/* A convex core in plane coordinates (f64): 1 point (circle), 2 (capsule, segment, chain
 * segment) or 3..8 (polygon, CCW) points, and the radius around it. */
typedef struct b2_core {
    de__d2 v[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    u32 count;
    f64 radius;
} b2_core;

/* The core of convex shape `s` (or chain segment `seg` of a chain) in its body frame (f32). */
static u32 local_core(const de__shape2d *s, const de__chain_segment *seg, de_vec2 *pts, f32 *r) {
    *r = 0.0f;
    if (seg != NULL) {
        pts[0] = seg->p1;
        pts[1] = seg->p2;
        return 2;
    }
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE:
        pts[0] = s->u.circle.center;
        *r = s->u.circle.radius;
        return 1;
    case DE_SHAPE2D_CAPSULE:
        pts[0] = s->u.capsule.p1;
        pts[1] = s->u.capsule.p2;
        *r = s->u.capsule.radius;
        return 2;
    case DE_SHAPE2D_SEGMENT:
        pts[0] = s->u.segment.p1;
        pts[1] = s->u.segment.p2;
        return 2;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON: {
        const de__polygon2d *poly = &s->u.polygon;
        for (u32 i = 0; i < poly->count; ++i)
            pts[i] = poly->vertices[i];
        *r = poly->radius;
        return poly->count;
    }
    default:
        return 0;
    }
}

static void core_place(const de_vec2 *pts, u32 n, f32 r, de__d2 pos, de_rot2d q, b2_core *out) {
    memset(out, 0, sizeof *out); /* every point defined, also past n (GCC -Wmaybe-uninitialized) */
    for (u32 i = 0; i < n; ++i)
        out->v[i] = de__d2_add(pos, de__rot2d_apply_d(q, de__d2_from(pts[i])));
    out->count = n;
    out->radius = (f64)r;
}

static de_rot2d rot_inv(de_rot2d q) {
    de_rot2d r = {q.c, -q.s};
    return r;
}

static f64 clamp01(f64 x) {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

/* Closest points of segments p1-q1 and p2-q2 in f64 (Box2D's b2SegmentDistance). */
static void seg_distance(de__d2 p1, de__d2 q1, de__d2 p2, de__d2 q2, de__d2 *c1, de__d2 *c2) {
    const de__d2 d1 = de__d2_sub(q1, p1), d2 = de__d2_sub(q2, p2), r = de__d2_sub(p1, p2);
    const f64 dd1 = de__d2_dot(d1, d1), dd2 = de__d2_dot(d2, d2);
    const f64 rd1 = de__d2_dot(r, d1), rd2 = de__d2_dot(r, d2);
    const f64 eps2 = 1.0e-24;
    f64 f1 = 0.0, f2 = 0.0;
    if (dd1 < eps2 || dd2 < eps2) {
        if (dd1 >= eps2)
            f1 = clamp01(-rd1 / dd1);
        else if (dd2 >= eps2)
            f2 = clamp01(rd2 / dd2);
    } else {
        const f64 d12 = de__d2_dot(d1, d2);
        const f64 denom = dd1 * dd2 - d12 * d12;
        if (denom != 0.0)
            f1 = clamp01((d12 * rd2 - rd1 * dd2) / denom);
        f2 = (d12 * f1 + rd2) / dd2;
        if (f2 < 0.0) {
            f2 = 0.0;
            f1 = clamp01(-rd1 / dd1);
        } else if (f2 > 1.0) {
            f2 = 1.0;
            f1 = clamp01((d12 - rd1) / dd1);
        }
    }
    *c1 = de__d2_add(p1, de__d2_scale(d1, f1));
    *c2 = de__d2_add(p2, de__d2_scale(d2, f2));
}

/* true when x lies inside (or on) the CCW polygon core c (count >= 3). */
static bool core_contains(const b2_core *c, de__d2 x) {
    for (u32 i = 0; i < c->count; ++i) {
        const de__d2 a = c->v[i], b = c->v[(i + 1u) % c->count];
        if (de__d2_cross(de__d2_sub(b, a), de__d2_sub(x, a)) < 0.0)
            return false;
    }
    return true;
}

/* Distance of the cores of A and B (0 when they overlap), with the closest points. */
static f64 core_distance(const b2_core *A, const b2_core *B, de__d2 *pa, de__d2 *pb) {
    const u32 ea = A->count >= 3u ? A->count : 1u;
    const u32 eb = B->count >= 3u ? B->count : 1u;
    f64 best = -1.0;
    *pa = A->v[0];
    *pb = B->v[0];
    for (u32 i = 0; i < ea; ++i) {
        const de__d2 a1 = A->v[i], a2 = A->v[A->count > 1u ? (i + 1u) % A->count : 0u];
        for (u32 j = 0; j < eb; ++j) {
            const de__d2 b1 = B->v[j], b2 = B->v[B->count > 1u ? (j + 1u) % B->count : 0u];
            de__d2 c1, c2;
            seg_distance(a1, a2, b1, b2, &c1, &c2);
            const de__d2 d = de__d2_sub(c2, c1);
            const f64 d2 = de__d2_dot(d, d);
            if (best < 0.0 || d2 < best) {
                best = d2;
                *pa = c1;
                *pb = c2;
            }
        }
    }
    if ((A->count >= 3u && core_contains(A, B->v[0])) ||
        (B->count >= 3u && core_contains(B, A->v[0])))
        return 0.0;
    return best > 0.0 ? de_dsqrt64(best) : 0.0;
}

/* ================================ Motion =============================================== */

/* The bullet's motion over t in [0, 1]. */
typedef struct b2_motion {
    de__d2 c0;    /* center of mass at t = 0 */
    de__d2 dc;    /* its displacement over the step */
    de_rot2d q0;  /* rotation at t = 0 */
    f64 da;       /* angle turned over the step */
    de_vec2 lcom; /* local center of mass */
} b2_motion;

static de_rot2d rot_at(const b2_motion *m, f64 t) {
    if (m->da == 0.0 || t == 0.0)
        return m->q0;
    f32 s, c;
    de_dsincos((f32)(m->da * t), &s, &c);
    const f64 q0c = m->q0.c, q0s = m->q0.s;
    de_rot2d r;
    r.c = (f32)((f64)c * q0c - (f64)s * q0s);
    r.s = (f32)((f64)s * q0c + (f64)c * q0s);
    return de__rot2d_normalize(r);
}

/* Body origin and rotation at t. */
static de__xf2d pose_at(const b2_motion *m, f64 t) {
    de__xf2d x;
    x.q = rot_at(m, t);
    const de__d2 com = de__d2_add(m->c0, de__d2_scale(m->dc, t));
    x.p = de__d2_sub(com, de__rot2d_apply_d(x.q, de__d2_from(m->lcom)));
    return x;
}

/* ================================ Sweep ================================================ */

enum { CA_MISS = 0, CA_HIT = 1, CA_START = 2 };

typedef struct b2_sweep {
    de_physics2d_world *p;
    const de__solver2d *s;
    de__bullets2d_stats *stats;
    u32 index; /* the bullet */
    const de__shape2d *shape;
    b2_motion m;
    de_vec2 mover_pts[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    u32 mover_count;
    f32 mover_radius;
    f64 rcore; /* largest distance of the mover's core from its center of mass */
    f32 fallback_radius;
    de__d2 v_end;    /* the bullet's velocity at the end of the step */
    de__d2 v_target; /* the current target's end velocity */
    f64 dt;
    f64 best; /* earliest time of impact so far (1 = none) */
    /* The current target. */
    const de__shape2d *target_shape;
    de__xf2d target_pose;
    u32 target_child; /* chain segment, or DE__P2D_CHILD_NONE */
    b2_core target;
} b2_sweep;

/* The mover core at t: the bullet's own core, or (fallback) the point at its center of mass. */
static void mover_core(const b2_sweep *sw, bool fallback, f64 t, b2_core *out) {
    const de__xf2d x = pose_at(&sw->m, t);
    if (fallback)
        core_place(&sw->m.lcom, 1u, sw->fallback_radius, x.p, x.q, out);
    else
        core_place(sw->mover_pts, sw->mover_count, sw->mover_radius, x.p, x.q, out);
}

/* Gap (radii included; -(radii) when the cores overlap) and the unit normal, target toward
 * mover (zero when the cores overlap). */
static f64 gap_at(const b2_sweep *sw, bool fallback, f64 t, de__d2 *normal) {
    b2_core mc;
    mover_core(sw, fallback, t, &mc);
    de__d2 pa = de__d2_make(0.0, 0.0), pb = pa;
    const f64 d = core_distance(&sw->target, &mc, &pa, &pb);
    const f64 radii = sw->target.radius + mc.radius;
    if (!(d > 0.0)) {
        *normal = de__d2_make(0.0, 0.0);
        return -radii;
    }
    *normal = de__d2_scale(de__d2_sub(pb, pa), 1.0 / d);
    return d - radii;
}

static void count_iter(b2_sweep *sw, u32 it) {
    ++sw->stats->iterations;
    if (it + 1u > sw->stats->max_iterations)
        sw->stats->max_iterations = it + 1u;
}

/* Conservative advancement over t in [0, best): CA_HIT with the time and the last separating
 * normal, CA_START when within target + tol at t = 0, else CA_MISS. */
static int ca_sweep(b2_sweep *sw, bool fallback, f64 target, f64 tol, f64 *t_hit, de__d2 *n_hit) {
    const f64 ang = fallback ? 0.0 : de__absd(sw->m.da) * sw->rcore;
    f64 t = 0.0;
    de__d2 last = de__d2_make(0.0, 0.0);
    for (u32 it = 0; it < DE__BULLET2D_MAX_ITER; ++it) {
        de__d2 n;
        const f64 sep = gap_at(sw, fallback, t, &n);
        count_iter(sw, it);
        if (sep <= target + tol) {
            if (it == 0)
                return CA_START;
            *t_hit = t;
            *n_hit = last;
            return CA_HIT;
        }
        last = n;
        const f64 speed = -de__d2_dot(n, sw->m.dc) + ang;
        if (!(speed > 0.0))
            return CA_MISS; /* the line separates them for the rest of the step */
        t += (sep - target) / speed;
        if (!(t < sw->best))
            return CA_MISS;
    }
    /* Out of steps: t never passed the contact, so it is a safe (early) time of impact. */
    ++sw->stats->capped;
    *t_hit = t;
    *n_hit = last;
    return CA_HIT;
}

/* Gap and normal at the end pose, with the real depth when the cores overlap (the narrowphase:
 * deepest point and its normal). */
static f64 end_gap(const b2_sweep *sw, bool fallback, de__d2 *normal) {
    const f64 sep = gap_at(sw, fallback, 1.0, normal);
    if (fallback || normal->x != 0.0 || normal->y != 0.0)
        return sep;
    const de__xf2d xm = pose_at(&sw->m, 1.0);
    de__contact_set2d set;
    memset(&set, 0, sizeof set);
    if (de__collide2d(sw->target_shape, sw->target_pose, sw->shape, xm, sw->target_child, 0.0f,
                      &set) == 0u)
        return sep; /* zero normal: counts as "on the other side" */
    f32 lo = set.points[0].separation;
    for (u32 k = 1; k < set.point_count; ++k)
        lo = de__minf(lo, set.points[k].separation);
    *normal = de__d2_from(set.normal);
    return (f64)lo;
}

/* Whether a hit at the time of impact still needs the clamp: the bullet ends deeper than the slop
 * in the target (the center-of-mass circle: inside it at all), on its other side, or still closing
 * on it by more than the slop per step. Otherwise the solver's contacts already held it. */
static bool needs_clamp(const b2_sweep *sw, bool fallback, de__d2 n_hit) {
    de__d2 n_end;
    const f64 sep = end_gap(sw, fallback, &n_end);
    if (sep < (fallback ? 0.0 : -(f64)DE__BULLET2D_LINEAR_SLOP))
        return true;
    if (!(de__d2_dot(n_end, n_hit) > 0.0))
        return true;
    const f64 approach = -de__d2_dot(n_end, de__d2_sub(sw->v_end, sw->v_target));
    return approach * sw->dt > (f64)DE__BULLET2D_LINEAR_SLOP;
}

/* The bullet against the current target core. */
static void sweep_target(b2_sweep *sw) {
    const f64 gap = (f64)DE__BULLET2D_LINEAR_SLOP;
    const f64 tol = 0.25 * gap;
    ++sw->stats->candidates;
    f64 t = 0.0;
    de__d2 n;
    const int r = ca_sweep(sw, false, gap, tol, &t, &n);
    if (r == CA_HIT) {
        if (needs_clamp(sw, false, n) && t < sw->best)
            sw->best = t;
        return;
    }
    if (r != CA_START)
        return;
    /* Touching at the start: the center-of-mass circle may close up to half its starting gap (at
     * most a quarter of the slop) and may not enter the target. Skipped when it already overlaps
     * it. */
    ++sw->stats->fallbacks;
    de__d2 n0;
    const f64 sep0 = gap_at(sw, true, 0.0, &n0);
    if (!(sep0 > 0.0))
        return;
    const f64 target = de__mind(0.25 * gap, 0.5 * sep0);
    const f64 ftol = de__mind(tol, 0.25 * sep0);
    if (ca_sweep(sw, true, target, ftol, &t, &n) == CA_HIT && needs_clamp(sw, true, n) &&
        t < sw->best)
        sw->best = t;
}

/* ---- Chains: one sweep per segment near the path ---- */

typedef struct b2_chain_ctx {
    b2_sweep *sw;
    const de__chain2d *chain;
} b2_chain_ctx;

static bool chain_segment_cb(void *user, u32 segment) {
    b2_chain_ctx *cc = (b2_chain_ctx *)user;
    b2_sweep *sw = cc->sw;
    const de__chain_segment *seg = &cc->chain->segments[segment];
    /* One-sided: a bullet whose center of mass starts behind the segment passes. */
    const de__d2 c0_local =
        de__rot2d_apply_d(rot_inv(sw->target_pose.q), de__d2_sub(sw->m.c0, sw->target_pose.p));
    const de__d2 rel = de__d2_sub(c0_local, de__d2_from(seg->p1));
    if (de__d2_dot(rel, de__d2_from(seg->normal)) < 0.0)
        return true;
    de_vec2 pts[2];
    f32 r;
    const u32 n = local_core(sw->target_shape, seg, pts, &r);
    core_place(pts, n, r, sw->target_pose.p, sw->target_pose.q, &sw->target);
    sw->target_child = segment;
    sweep_target(sw);
    return true;
}

/* ================================ Candidates =========================================== */

/* Pose of body j at the end of the step (its solver body applied, as finalize will). */
static de__xf2d end_pose(const de_physics2d_world *p, const de__solver2d *s, u32 j, de__d2 *v) {
    const de__body2d_pool *b = &p->bodies;
    de__xf2d x;
    x.p = b->position[j];
    x.q = b->rotation[j];
    *v = de__d2_make(0.0, 0.0);
    const u32 slot = de__solver2d_slot(s, j);
    if (slot == DE__SOLVER2D_STATIC_SLOT)
        return x;
    const de__solver_body2d *sb = &s->bodies[slot];
    *v = de__d2_from(sb->v);
    const de__d2 lcom = de__d2_from(b->local_com[j]);
    const de__d2 com =
        de__d2_add(de__d2_add(x.p, de__rot2d_apply_d(x.q, lcom)), de__d2_from(sb->dp));
    if (sb->dq.c != 1.0f || sb->dq.s != 0.0f) {
        de_rot2d q;
        q.c = (f32)((f64)sb->dq.c * (f64)x.q.c - (f64)sb->dq.s * (f64)x.q.s);
        q.s = (f32)((f64)sb->dq.s * (f64)x.q.c + (f64)sb->dq.c * (f64)x.q.s);
        x.q = de__rot2d_normalize(q);
    }
    x.p = de__d2_sub(com, de__rot2d_apply_d(x.q, lcom));
    return x;
}

typedef struct b2_query {
    b2_sweep *sw;
    de_aabb2d box; /* the path box (plane) */
} b2_query;

static bool candidate_cb(void *user, u32 k) {
    b2_query *q = (b2_query *)user;
    b2_sweep *sw = q->sw;
    const de_physics2d_world *p = sw->p;
    const de__body2d_pool *b = &p->bodies;
    if (k == sw->index || !b->alive[k] || b->type[k] == (u8)DE_BODY_DYNAMIC ||
        !de__bp2d_should_collide(p, sw->index, k) ||
        (b->flags[k] & (u32)DE_BODY_FLAG_TRIGGER) != 0u)
        return true;
    const de__shape2d *shape = de__shape2d_get(p, b->shape[k]);
    if (shape == NULL)
        return true;
    sw->target_shape = shape;
    sw->target_pose = end_pose(p, sw->s, k, &sw->v_target);
    if (shape->kind == DE_SHAPE2D_CHAIN) {
        /* The path box in the chain's frame (f64, rounded outward). */
        const de__d2 lo = de__d2_from(q->box.min), hi = de__d2_from(q->box.max);
        const de_rot2d qi = rot_inv(sw->target_pose.q);
        de__d2 mn = de__d2_make(1.0e300, 1.0e300), mx = de__d2_make(-1.0e300, -1.0e300);
        for (u32 c = 0; c < 4; ++c) {
            const de__d2 w = de__d2_make((c & 1u) ? hi.x : lo.x, (c & 2u) ? hi.y : lo.y);
            const de__d2 l = de__rot2d_apply_d(qi, de__d2_sub(w, sw->target_pose.p));
            mn = de__d2_make(de__mind(mn.x, l.x), de__mind(mn.y, l.y));
            mx = de__d2_make(de__maxd(mx.x, l.x), de__maxd(mx.y, l.y));
        }
        b2_chain_ctx cc = {sw, &shape->u.chain};
        (void)de__chain2d_query(&shape->u.chain, de__aabb2d_from_d(mn, mx), chain_segment_cb, &cc);
        return true;
    }
    if (!de__shape2d_kind_is_convex(shape->kind))
        return true;
    de_vec2 pts[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    f32 r;
    const u32 n = local_core(shape, NULL, pts, &r);
    core_place(pts, n, r, sw->target_pose.p, sw->target_pose.q, &sw->target);
    sw->target_child = DE__P2D_CHILD_NONE;
    sweep_target(sw);
    return true;
}

/* ================================ Bullets ============================================== */

static void sweep_bullet(de_physics2d_world *p, de__solver2d *s, u32 i) {
    const de__body2d_pool *b = &p->bodies;
    const de__shape2d *shape = de__shape2d_get(p, b->shape[i]);
    if (shape == NULL || !de__shape2d_kind_is_convex(shape->kind))
        return;
    de__solver_body2d *sb = &s->bodies[de__solver2d_slot(s, i)];
    b2_sweep sw;
    memset(&sw, 0, sizeof sw);
    sw.p = p;
    sw.s = s;
    sw.stats = &s->bullet_stats;
    sw.index = i;
    sw.shape = shape;
    sw.m.q0 = b->rotation[i];
    sw.m.lcom = b->local_com[i];
    sw.m.c0 = de__d2_add(b->position[i], de__rot2d_apply_d(sw.m.q0, de__d2_from(sw.m.lcom)));
    sw.m.dc = de__d2_from(sb->dp);
    sw.m.da = (f64)sb->da;
    sw.mover_count = local_core(shape, NULL, sw.mover_pts, &sw.mover_radius);
    for (u32 k = 0; k < sw.mover_count; ++k) {
        const de__d2 d = de__d2_sub(de__d2_from(sw.mover_pts[k]), de__d2_from(sw.m.lcom));
        sw.rcore = de__maxd(sw.rcore, de_dsqrt64(de__d2_dot(d, d)));
    }
    /* Skip bullets that cannot move by the slop this step. */
    const f64 motion = de_dsqrt64(de__d2_dot(sw.m.dc, sw.m.dc)) + de__absd(sw.m.da) * sw.rcore;
    if (!(motion > (f64)DE__BULLET2D_LINEAR_SLOP))
        return;
    ++s->bullet_stats.bullets;
    sw.best = 1.0;
    sw.dt = (f64)s->ctx.dt;
    sw.v_end = de__d2_from(sb->v);
    const de_aabb2d *lb = &shape->local_aabb;
    sw.fallback_radius =
        DE__BULLET2D_FALLBACK_SCALE * 0.5f * de__minf(lb->max.x - lb->min.x, lb->max.y - lb->min.y);

    /* Candidates: every body whose broadphase box meets the path of the shape. */
    const de__d2 lc = de__d2_from(sw.m.lcom);
    const f64 reach = de_dsqrt64(de__d2_dot(lc, lc)) + (f64)shape->bounding_radius +
                      2.0 * (f64)DE__BULLET2D_LINEAR_SLOP;
    const de__d2 c0 = sw.m.c0, c1 = de__d2_add(c0, sw.m.dc);
    b2_query q;
    q.sw = &sw;
    q.box =
        de__aabb2d_from_d(de__d2_make(de__mind(c0.x, c1.x) - reach, de__mind(c0.y, c1.y) - reach),
                          de__d2_make(de__maxd(c0.x, c1.x) + reach, de__maxd(c0.y, c1.y) + reach));
    (void)de__bp2d_query_aabb(p, q.box, NULL, candidate_cb, &q);
    if (!(sw.best < 1.0))
        return;

    /* Back to the pose at the time of impact; the velocity stays (the next step's speculative
     * contact stops the bullet). */
    const f64 t = sw.best;
    sb->dp = de__v2((f32)(sw.m.dc.x * t), (f32)(sw.m.dc.y * t));
    sb->dq = de__rot2d_identity();
    if (sw.m.da != 0.0) {
        f32 sn, cs;
        de_dsincos((f32)(sw.m.da * t), &sn, &cs);
        de_rot2d r = {cs, sn};
        sb->dq = de__rot2d_normalize(r);
    }
    sb->da = (f32)(sw.m.da * t);
    ++s->bullet_stats.hits;
}

void de__bullets2d_solve(de_physics2d_world *p, de__solver2d *s) {
    if (p == NULL || s == NULL || !s->settings.bullets)
        return;
    const de__body2d_pool *b = &p->bodies;
    for (u32 k = 0; k < s->awake_count; ++k) {
        const u32 i = s->awake[k];
        if (b->type[i] != (u8)DE_BODY_DYNAMIC || (b->flags[i] & (u32)DE_BODY2D_FLAG_BULLET) == 0u ||
            (b->flags[i] & (u32)DE_BODY_FLAG_TRIGGER) != 0u)
            continue;
        sweep_bullet(p, s, i);
    }
}

void de__bullets2d_stats_get(const de_physics2d_world *p, de__bullets2d_stats *out) {
    const de__solver2d *s = p != NULL ? de__solver2d_get(p) : NULL;
    if (s == NULL)
        memset(out, 0, sizeof *out);
    else
        *out = s->bullet_stats;
}
