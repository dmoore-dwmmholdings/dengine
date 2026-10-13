/*
 * ccd.c: continuous collision hooks of de_physics_step (DENG-219; replaces the DENG-217 stub):
 * per-body predictions for speculative contacts (de__ccd_prepare) and the swept time of impact
 * of bullets (de__ccd_solve). See ccd_internal.h for the rules; step.c calls both hooks.
 *
 * Bullet sweep, in detail. The bullet's center of mass moves from c0 to c0 + dc (dc = the solver
 * body's dp) and its rotation from q0 to dq * q0, with dq the step's rotation taken as the
 * shortest arc. In between, the model pose at t in [0, 1] is
 *
 *     c(t) = c0 + t dc,   q(t) = nlerp(identity, dq, t) * q0.
 *
 * The rotation angle of nlerp(identity, dq, t) grows at most at 4 |dq.xyz| / (1 + dq.w) per
 * unit t (the rate of normalized interpolation peaks at t = 1/2; no trigonometry needed), so a
 * point of the bullet's core at distance r from the center of mass moves toward a plane with
 * normal n at most at -dot(n, dc) + rate * r. Conservative advancement then steps t by
 * (gap - target) / that bound, recomputing the gap with GJK distance (narrow/gjk.c) on the
 * shapes' cores (sphere and capsule radii kept out, as the narrowphase and the shape cast do).
 * The step never passes the contact, so the first t at which the gap is within the tolerance of
 * DE__CCD_LINEAR_SLOP is a safe time of impact. Targets hold still at their end poses (bullets);
 * de__ccd_pair_may_touch runs the same sweep with both bodies moving by their predicted motion.
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt / de_dsqrt64 / de_dnormalize_quat only, f64 for
 * poses and offsets, bounded loops, bodies in ascending index; the result of a bullet is the
 * minimum over its candidates, so the broadphase visiting order does not change it.
 */
#include "physics/ccd_internal.h"

#include "physics/broadphase/broadphase.h"
#include "physics/narrow/narrowphase.h"
#include "physics/query/query_internal.h"
#include "physics/solver/solver_internal.h"

#include <dengine/de_debug.h>

/* ================================ State ================================================ */

static void ext_destroy(de_physics_world *p, void *state) {
    (void)p;
    de__ccd *c = (de__ccd *)state;
    DE_FREE(c->alloc, c->block, c->block_size);
}

static const de__phys_ext_vtbl k_ccd_vtbl = {ext_destroy, NULL, NULL};

static de_cvar float_cvar(const char *name, const char *help, f64 def) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_FLOAT;
    d.default_number = def;
    d.min = 0.0;
    d.max = 100.0;
    return de_cvar_register(&d);
}

static f32 cvar_f32(de_cvar cv, f32 def) {
    if (!DE_HANDLE_VALID(cv))
        return def;
    const f32 v = (f32)de_cvar_float(cv);
    return de__f32_finite(v) && v >= 0.0f ? v : def;
}

/* Registration takes the cvar registry lock: only at create. */
static void read_settings(de__ccd_settings *out) {
    de_cvar_desc ed;
    memset(&ed, 0, sizeof ed);
    ed.name = DE__CCD_ENABLED_CVAR;
    ed.help = "Physics: sweep bodies with DE_BODY_FLAG_CCD (bullets) to their time of impact "
              "(read when the CCD state is created)";
    ed.type = DE_CVAR_BOOL;
    ed.default_number = 1.0;
    const de_cvar en = de_cvar_register(&ed);
    const de_cvar sm = float_cvar(DE__CCD_SPECULATIVE_MAX_CVAR,
                                  "Physics: largest speculative contact distance (m) of a fast "
                                  "pair (read when the CCD state is created)",
                                  (f64)DE__CCD_DEFAULT_SPECULATIVE_MAX);
    const de_cvar pm = float_cvar(DE__CCD_PREDICT_MAX_CVAR,
                                  "Physics: largest predictive growth (m) of a moving body's "
                                  "broadphase box (read when the CCD state is created)",
                                  (f64)DE__CCD_DEFAULT_PREDICT_MAX);
    out->enabled = DE_HANDLE_VALID(en) ? de_cvar_bool(en) : true;
    out->speculative_max = cvar_f32(sm, DE__CCD_DEFAULT_SPECULATIVE_MAX);
    out->predict_max = cvar_f32(pm, DE__CCD_DEFAULT_PREDICT_MAX);
}

#define CCD_ALIGN 16u

static size_t ccd_align_up(size_t x) {
    return (x + (CCD_ALIGN - 1u)) & ~(size_t)(CCD_ALIGN - 1u);
}

de_result de__ccd_ensure(de_physics_world *p, de__ccd **out) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__ccd_ensure: world is NULL");
    de__ccd *c = de__ccd_get(p);
    if (c == NULL) {
        const u32 cap = p->bodies.capacity;
        /* One block: the state, then velocity[cap], reach[cap]. */
        const size_t off_v = ccd_align_up(sizeof(de__ccd));
        const size_t off_r = ccd_align_up(off_v + sizeof(de_vec3) * (size_t)cap);
        const size_t size = ccd_align_up(off_r + sizeof(f32) * (size_t)cap);
        u8 *block = (u8 *)DE_ALLOC(p->alloc, size, CCD_ALIGN, DE_MEM_PHYSICS);
        if (block == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__ccd_ensure: %zu bytes", size);
        memset(block, 0, size);
        c = (de__ccd *)(void *)block;
        c->alloc = p->alloc;
        c->capacity = cap;
        c->velocity = (de_vec3 *)(void *)(block + off_v);
        c->reach = (f32 *)(void *)(block + off_r);
        c->block = block;
        c->block_size = size;
        read_settings(&c->settings);
        de__phys_ext_set(p, DE__PHYS_EXT_CCD, c, &k_ccd_vtbl);
    }
    if (out != NULL)
        *out = c;
    return DE_OK;
}

void de__ccd_stats_get(const de_physics_world *p, de__ccd_stats *out) {
    memset(out, 0, sizeof *out);
    const de__ccd *c = p != NULL ? de__ccd_get(p) : NULL;
    if (c != NULL)
        *out = c->stats;
}

/* ================================ Prepare ============================================== */

static f32 len3(de_vec3 v) {
    return de_dsqrt(de__v3_dot(v, v));
}

void de__ccd_prepare(de_physics_world *p, f32 dt) {
    de__ccd *c = NULL;
    if (de__ccd_ensure(p, &c) != DE_OK)
        return; /* no memory: the fixed margin of the contacts applies (error context set) */
    /* The broadphase takes the growth; the collision pipeline would create it next anyway. */
    de__broadphase *bp = NULL;
    (void)de__broadphase_ensure(p, &bp);
    c->dt = dt;
    c->tick = p->tick;
    c->prepared = true;
    memset(&c->stats, 0, sizeof c->stats);

    const de__body_pool *b = &p->bodies;
    const f32 pm = c->settings.predict_max;
    const de_vec3 zero = {0.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i])
            continue;
        const de__shape *s = de__shape_get(p, b->shape[i]);
        if (b->type[i] == (u8)DE_BODY_STATIC || s == NULL) {
            c->velocity[i] = zero;
            c->reach[i] = 0.0f;
            de__broadphase_set_predict(p, i, zero, zero);
            continue;
        }
        de_vec3 v = b->linear_velocity[i];
        if (b->type[i] == (u8)DE_BODY_DYNAMIC) /* one step of gravity: the end-of-step speed */
            v = de__v3_add(v, de__v3_scale(p->gravity, dt * b->gravity_scale[i]));
        /* Farthest point of the shape from the center of mass. */
        const f32 radius = len3(b->local_com[i]) + s->bounding_radius;
        const f32 reach = len3(b->angular_velocity[i]) * radius;
        c->velocity[i] = v;
        c->reach[i] = reach;

        de_vec3 lo = zero, hi = zero;
        if (pm > 0.0f) {
            de_vec3 d = de__v3_scale(v, dt);
            const f32 dl = len3(d);
            if (dl > pm)
                d = de__v3_scale(d, pm / dl);
            /* A rotating point moves at most reach * dt, and never more than the diameter. */
            const f32 a = de__minf(de__minf(reach * dt, 2.0f * radius), pm);
            lo = de__v3(de__maxf(-d.x, 0.0f) + a, de__maxf(-d.y, 0.0f) + a,
                        de__maxf(-d.z, 0.0f) + a);
            hi = de__v3(de__maxf(d.x, 0.0f) + a, de__maxf(d.y, 0.0f) + a, de__maxf(d.z, 0.0f) + a);
            if (lo.x > 0.0f || lo.y > 0.0f || lo.z > 0.0f || hi.x > 0.0f || hi.y > 0.0f ||
                hi.z > 0.0f)
                ++c->stats.predicted;
        }
        de__broadphase_set_predict(p, i, lo, hi);
    }
}

/* ================================ Motion =============================================== */

/* a * b (apply b, then a) in f64. */
static void quat_mul_d(const f64 a[4], const f64 b[4], f64 out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static de_quat quat_from_d(const f64 q[4]) {
    de_quat r;
    r.x = (f32)q[0];
    r.y = (f32)q[1];
    r.z = (f32)q[2];
    r.w = (f32)q[3];
    return de_dnormalize_quat(r);
}

static void quat_to_d(de_quat q, f64 out[4]) {
    out[0] = q.x;
    out[1] = q.y;
    out[2] = q.z;
    out[3] = q.w;
}

static f64 dlen(de_dvec3 v) {
    return de_dsqrt64(de__d3_dot(v, v));
}

/* The origin position that puts the center of mass (local_com) at `com` with rotation q. */
static de_dvec3 origin_for_com(de_dvec3 com, de_quat q, de_vec3 local_com) {
    const de__dmat3 r = de__dmat3_from_quat(q);
    return de__d3_sub(com, de__dmat3_mul(&r, de__d3_from(local_com)));
}

/*
 * A body's motion over the step (see the file comment): the center of mass from c0 to c0 + dc,
 * the rotation from q0 to dq * q0 along nlerp(identity, dq, t), dq the shortest arc.
 */
typedef struct ccd_motion {
    de_dvec3 c0;  /* center of mass at t = 0 (world) */
    de_dvec3 dc;  /* center-of-mass displacement over the step */
    f64 q0[4];    /* rotation at t = 0 */
    f64 dq[4];    /* step rotation, shortest arc (dq[3] >= 0) */
    f64 omega;    /* bound on the rotation rate per unit t (rad) */
    de_vec3 lcom; /* center of mass in the body frame */
} ccd_motion;

static void motion_init(ccd_motion *m, de__pose start, de_vec3 lcom, de_dvec3 dc, de_quat dq) {
    m->lcom = lcom;
    const de__dmat3 r0 = de__dmat3_from_quat(start.rotation);
    m->c0 = de__d3_add(start.position, de__dmat3_mul(&r0, de__d3_from(lcom)));
    m->dc = dc;
    quat_to_d(start.rotation, m->q0);
    quat_to_d(dq, m->dq);
    if (m->dq[3] < 0.0) /* the shortest arc of the same rotation */
        for (u32 k = 0; k < 4; ++k)
            m->dq[k] = -m->dq[k];
    const f64 v = de_dsqrt64(m->dq[0] * m->dq[0] + m->dq[1] * m->dq[1] + m->dq[2] * m->dq[2]);
    /* The angle rate of nlerp(identity, dq, t) peaks at t = 1/2 at 4 sin(a) / (1 + cos(a)),
     * with sin(a) = |dq.xyz| and cos(a) = dq.w (a = half the step angle). */
    m->omega = 4.0 * v / (1.0 + m->dq[3]);
}

/* nlerp(identity, dq, t), normalized, in f64. */
static void motion_dq(const ccd_motion *m, f64 t, f64 out[4]) {
    out[0] = t * m->dq[0];
    out[1] = t * m->dq[1];
    out[2] = t * m->dq[2];
    out[3] = (1.0 - t) + t * m->dq[3];
    const f64 n2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3];
    const f64 k = n2 > 0.0 ? 1.0 / de_dsqrt64(n2) : 1.0;
    for (u32 i = 0; i < 4; ++i)
        out[i] *= k;
}

/* The body origin's pose at t. */
static de__pose motion_pose(const ccd_motion *m, f64 t) {
    f64 qt[4], q[4];
    motion_dq(m, t, qt);
    quat_mul_d(qt, m->q0, q);
    de__pose x;
    x.rotation = quat_from_d(q);
    x.position = origin_for_com(de__d3_add(m->c0, de__d3_scale(m->dc, t)), x.rotation, m->lcom);
    return x;
}

static bool motion_moves(const ccd_motion *m) {
    return m->omega > 0.0 || m->dc.x != 0.0 || m->dc.y != 0.0 || m->dc.z != 0.0;
}

/* ================================ Sweep ================================================ */

/* A convex shape (or a mesh / heightfield of a target) placed in its body's frame: the body's own
 * shape, or a compound child at (pos, rot). `rcore`: the largest distance of its core from the
 * body's center of mass (rotation bound; 0 when unused). */
typedef struct ccd_piece {
    const de__shape *shape;
    bool child;
    de_vec3 pos;
    de_quat rot;
    f64 rcore;
} ccd_piece;

static de__pose piece_pose(const ccd_piece *pc, de__pose body) {
    if (pc == NULL || !pc->child)
        return body;
    const de__dmat3 r = de__dmat3_from_quat(body.rotation);
    de__pose x;
    x.position = de__d3_add(body.position, de__dmat3_mul(&r, de__d3_from(pc->pos)));
    f64 a[4], b[4], q[4];
    quat_to_d(body.rotation, a);
    quat_to_d(pc->rot, b);
    quat_mul_d(a, b, q);
    x.rotation = quat_from_d(q);
    return x;
}

/* One side of a sweep: a body's motion and shape. */
typedef struct ccd_side {
    ccd_motion m;
    const de__shape *shape;
    bool moves;
    f64 radius; /* largest distance of the shape from the center of mass */
} ccd_side;

static void side_init(ccd_side *s, const de__shape *shape, de__pose start, de_vec3 lcom,
                      de_dvec3 dc, de_quat dq) {
    motion_init(&s->m, start, lcom, dc, dq);
    s->shape = shape;
    s->moves = motion_moves(&s->m);
    s->radius = dlen(de__d3_from(lcom)) + (f64)shape->bounding_radius;
}

/* Largest distance of the core of a convex piece from the body's center of mass. */
static f64 piece_rcore(const de__shape *shape, bool child, de_vec3 pos, de_vec3 lcom) {
    const de_dvec3 off = child ? de__d3_sub(de__d3_from(pos), de__d3_from(lcom))
                               : de__d3_scale(de__d3_from(lcom), -1.0);
    return dlen(off) + (f64)(shape->bounding_radius - shape->margin);
}

/*
 * A sweep of a mover (a convex shape or a compound of convex children) against a target body
 * (any shape kind), both moving by their ccd_motion over t in [0, 1]. Finds the earliest t at
 * which a piece of the mover comes within `gap` of a piece of the target (`best`, 1 = none).
 */
typedef struct ccd_sweep {
    ccd_side mover, target;
    f64 gap;
    f64 best;
    f32 fallback_radius; /* the center-of-mass sphere of the start-touching fallback */
    bool fallback;       /* bullets: a piece within the gap at t = 0 falls back to that sphere */
    bool start_is_hit;   /* pair check: within the gap at t = 0 is a hit */
    bool stop_on_hit;    /* pair check: any hit answers the question */
    bool end_check;      /* bullets: a hit counts only when the end pose needs the clamp */
    de_dvec3 end_dv;     /* bullets: end velocity of the bullet minus the target's (end check) */
    f64 dt;              /* bullets: the step (end check) */
    bool done;
    de__ccd_stats *stats; /* NULL: count nothing (the pair check runs on many threads) */
} ccd_sweep;

enum { CA_MISS = 0, CA_HIT = 1, CA_START = 2 };

static void count_iter(ccd_sweep *sw, u32 it) {
    if (sw->stats == NULL)
        return;
    ++sw->stats->iterations;
    if (it + 1u > sw->stats->max_iterations)
        sw->stats->max_iterations = it + 1u;
}

/* GJK between mover piece `mp` (NULL: the fallback sphere) and the target proxy at t: the gap
 * (radii included; -1 when the cores overlap, or minus the EPA depth when `depth`) and the unit
 * normal, target toward mover, in world axes (zero when the cores overlap without `depth`). */
static f64 gap_at(const ccd_sweep *sw, const ccd_piece *mp, const ccd_piece *tp,
                  const de__gjk_proxy *pa, f32 ra, f64 t, bool depth, de_dvec3 *normal) {
    const f64 rb = mp != NULL ? (f64)de__shape_margin(mp->shape) : (f64)sw->fallback_radius;
    const de__pose xm = piece_pose(mp, motion_pose(&sw->mover.m, t));
    const de__pose xt = piece_pose(tp, motion_pose(&sw->target.m, sw->target.moves ? t : 0.0));
    de__np_rel rel;
    de__np_rel_init(&rel, xt, xm, 0.0f);
    de__gjk_proxy pb;
    if (mp != NULL)
        de__gjk_proxy_shape(&pb, mp->shape, true, &rel);
    else
        de__gjk_proxy_points(&pb, &sw->mover.m.lcom, 1, &rel);
    de__gjk_result g;
    de__gjk_distance(pa, &pb, &g);
    if (g.overlap || !(g.distance > 0.0)) {
        *normal = de__d3(0.0, 0.0, 0.0);
        if (!depth || !g.overlap)
            return -1.0;
        de__epa_result e;
        de__epa_penetration(pa, &pb, &g, rel.up, &e);
        *normal = de__dmat3_mul(&rel.wa, e.normal);
        return -(e.depth + (f64)ra + rb);
    }
    *normal =
        de__dmat3_mul(&rel.wa, de__d3_scale(de__d3_sub(g.point_b, g.point_a), 1.0 / g.distance));
    return g.distance - ((f64)ra + rb);
}

/*
 * Conservative advancement of mover piece `mp` (NULL: the fallback sphere at the center of mass)
 * against the target proxy `pa` (core, in the frame of target piece `tp`) of radius `ra`, over
 * t in [0, sw->best). Stops when the gap is within `target` + `tol`: CA_HIT with the time
 * and the last separating normal (world, target toward mover), or CA_START at t = 0.
 */
static int ca_sweep(ccd_sweep *sw, const ccd_piece *mp, const ccd_piece *tp,
                    const de__gjk_proxy *pa, f32 ra, f64 t_rcore, f64 target, f64 tol, f64 *t_hit,
                    de_dvec3 *n_hit) {
    const f64 ang = (mp != NULL ? sw->mover.m.omega * mp->rcore : 0.0) +
                    (sw->target.moves ? sw->target.m.omega * t_rcore : 0.0);
    const de_dvec3 dc = de__d3_sub(sw->mover.m.dc, sw->target.m.dc);
    f64 t = 0.0;
    de_dvec3 last = de__d3(0.0, 0.0, 0.0); /* the last separating plane's normal */
    for (u32 it = 0; it < DE__CCD_MAX_ITER; ++it) {
        de_dvec3 n;
        const f64 sep = gap_at(sw, mp, tp, pa, ra, t, false, &n);
        count_iter(sw, it);
        if (sep <= target + tol) {
            if (it == 0)
                return CA_START;
            *t_hit = t;
            *n_hit = last;
            return CA_HIT;
        }
        last = n;
        const f64 speed = -de__d3_dot(n, dc) + ang;
        if (!(speed > 0.0))
            return CA_MISS; /* the plane separates them for the rest of the step */
        t += (sep - target) / speed;
        if (!(t < sw->best))
            return CA_MISS;
    }
    /* Out of steps: t never passed the contact, so it is a safe (early) time of impact. */
    if (sw->stats != NULL)
        ++sw->stats->capped;
    *t_hit = t;
    *n_hit = last;
    return CA_HIT;
}

/*
 * Bullets: whether a hit at the time of impact still needs the clamp. The solver's speculative
 * contacts already stop most impacts at the surface; the pose at t = 1 then lies on the same side
 * of the target as at the impact (separated, with a normal that does not point against the
 * normal at the impact), and moving the bullet back would only leave it short of the surface.
 * The clamp is needed when the bullet ends overlapping the target by more than the slop (EPA
 * depth), on its other side, or still closing on it by more than the slop per step (the solver's
 * soft contacts let a body that starts a step touching sink in for a substep).
 */
static bool needs_clamp(const ccd_sweep *sw, const ccd_piece *mp, const ccd_piece *tp,
                        const de__gjk_proxy *pa, f32 ra, de_dvec3 n_hit) {
    de_dvec3 n_end;
    const f64 sep = gap_at(sw, mp, tp, pa, ra, 1.0, true, &n_end);
    /* Deeper than the slop: the contacts did not hold it. The fallback sphere lies inside the
     * bullet and may not enter the target at all. */
    if (sep < (mp != NULL ? -(f64)DE__CCD_LINEAR_SLOP : 0.0))
        return true;
    if (!(de__d3_dot(n_end, n_hit) > 0.0))
        return true; /* on the other side */
    /* Still closing fast: a contact that starts touching is soft, and the bullet would sink in
     * (or through a thin target) next step; the clamp gives it a speculative gap instead. */
    const f64 approach = -de__d3_dot(n_end, sw->end_dv);
    return approach * sw->dt > (f64)DE__CCD_LINEAR_SLOP;
}

static void take_hit(ccd_sweep *sw, f64 t) {
    if (t < sw->best)
        sw->best = t;
    if (sw->stop_on_hit)
        sw->done = true;
}

/* Every piece of the mover against one target proxy. */
static void sweep_proxy(ccd_sweep *sw, const ccd_piece *tp, const de__gjk_proxy *pa, f32 ra,
                        f64 t_rcore) {
    const de__shape *ms = sw->mover.shape;
    const de_vec3 lcom = sw->mover.m.lcom;
    const f64 tol = 0.25 * (f64)DE__CCD_LINEAR_SLOP;
    bool touched = false;
    const u32 pieces = ms->kind == DE_SHAPE_COMPOUND ? ms->u.compound.child_count : 1u;
    for (u32 k = 0; k < pieces && !sw->done; ++k) {
        ccd_piece mp;
        if (ms->kind == DE_SHAPE_COMPOUND) {
            const de__compound_child *ch = &ms->u.compound.children[k];
            if (!de__shape_kind_is_convex(ch->shape->kind))
                continue;
            mp.shape = ch->shape;
            mp.child = true;
            mp.pos = ch->position;
            mp.rot = ch->rotation;
        } else {
            mp.shape = ms;
            mp.child = false;
            mp.pos = de__v3(0.0f, 0.0f, 0.0f);
            mp.rot.x = mp.rot.y = mp.rot.z = 0.0f;
            mp.rot.w = 1.0f;
        }
        mp.rcore = piece_rcore(mp.shape, mp.child, mp.pos, lcom);
        f64 t = 0.0;
        de_dvec3 n;
        const int r = ca_sweep(sw, &mp, tp, pa, ra, t_rcore, sw->gap, tol, &t, &n);
        if (r == CA_START) {
            if (sw->start_is_hit)
                take_hit(sw, 0.0);
            else
                touched = true;
        } else if (r == CA_HIT && (!sw->end_check || needs_clamp(sw, &mp, tp, pa, ra, n))) {
            take_hit(sw, t);
        }
    }
    if (touched && sw->fallback && !sw->done) {
        if (sw->stats != NULL)
            ++sw->stats->fallbacks;
        /* The fallback sphere lies deep inside the bullet and only has to stay out of the
         * target: it may close up to half its starting gap (at most a quarter of the slop). It
         * is skipped only when it already overlaps the target. */
        de_dvec3 n0;
        const f64 sep0 = gap_at(sw, NULL, tp, pa, ra, 0.0, false, &n0);
        if (sep0 > 0.0) {
            const f64 target = de__mind(0.25 * sw->gap, 0.5 * sep0);
            const f64 ftol = de__mind(tol, 0.25 * sep0);
            f64 t = 0.0;
            de_dvec3 n;
            if (ca_sweep(sw, NULL, tp, pa, ra, t_rcore, target, ftol, &t, &n) == CA_HIT &&
                (!sw->end_check || needs_clamp(sw, NULL, tp, pa, ra, n)))
                take_hit(sw, t);
        }
    }
}

/* The box around the mover's center-of-mass path, in the frame of target piece `tp` at t = 0,
 * grown by the mover's radius, the gap and (when the target moves) a bound on its motion. */
static de_aabb cull_box(const ccd_sweep *sw, const ccd_piece *tp) {
    const de__pose x = piece_pose(tp, motion_pose(&sw->target.m, 0.0));
    const de__dmat3 r = de__dmat3_from_quat(x.rotation);
    const de_dvec3 c0 = sw->mover.m.c0, c1 = de__d3_add(c0, sw->mover.m.dc);
    const de_dvec3 a = de__dmat3_tmul(&r, de__d3_sub(c0, x.position));
    const de_dvec3 b = de__dmat3_tmul(&r, de__d3_sub(c1, x.position));
    f64 g = sw->mover.radius + sw->gap + 2.0 * (f64)DE__CCD_LINEAR_SLOP;
    if (sw->target.moves) {
        /* A world point drifts in the target's frame by at most |dc_t| + omega_t * its distance
         * from the target's center of mass (which itself moves by |dc_t|). */
        const f64 dct = dlen(sw->target.m.dc);
        const f64 far =
            dlen(de__d3_sub(c0, sw->target.m.c0)) + dlen(sw->mover.m.dc) + dct + sw->mover.radius;
        g += dct + sw->target.m.omega * far;
    }
    de_aabb box;
    box.min.x = de__f32_round_down(de__mind(a.x, b.x) - g);
    box.min.y = de__f32_round_down(de__mind(a.y, b.y) - g);
    box.min.z = de__f32_round_down(de__mind(a.z, b.z) - g);
    box.max.x = de__f32_round_up(de__maxd(a.x, b.x) + g);
    box.max.y = de__f32_round_up(de__maxd(a.y, b.y) + g);
    box.max.z = de__f32_round_up(de__maxd(a.z, b.z) + g);
    return box;
}

typedef struct ccd_tri_ctx {
    ccd_sweep *sw;
    const ccd_piece *tp;
} ccd_tri_ctx;

static bool tri_cb(void *user, u32 sub, de_vec3 a, de_vec3 b, de_vec3 c) {
    ccd_tri_ctx *x = (ccd_tri_ctx *)user;
    (void)sub;
    const de_vec3 pts[3] = {a, b, c};
    de__gjk_proxy pa;
    de__gjk_proxy_points(&pa, pts, 3, NULL);
    f64 rc = 0.0;
    if (x->sw->target.moves) {
        /* Corners in the target's body frame, from its center of mass. */
        const de_vec3 lcom = x->sw->target.m.lcom;
        for (u32 k = 0; k < 3; ++k) {
            de_dvec3 v = de__d3_from(pts[k]);
            if (x->tp->child) {
                const de__dmat3 r = de__dmat3_from_quat(x->tp->rot);
                v = de__d3_add(de__d3_from(x->tp->pos), de__dmat3_mul(&r, v));
            }
            rc = de__maxd(rc, dlen(de__d3_sub(v, de__d3_from(lcom))));
        }
    }
    sweep_proxy(x->sw, x->tp, &pa, 0.0f, rc);
    return !x->sw->done;
}

/* The mover against target piece `tp` (convex, mesh or heightfield). */
static void sweep_target_piece(ccd_sweep *sw, const ccd_piece *tp) {
    const de__shape *ts = tp->shape;
    if (ts->kind == DE_SHAPE_MESH || ts->kind == DE_SHAPE_HEIGHTFIELD) {
        ccd_tri_ctx x;
        x.sw = sw;
        x.tp = tp;
        de__q_triangles(ts, cull_box(sw, tp), tri_cb, &x);
        return;
    }
    if (!de__shape_kind_is_convex(ts->kind))
        return;
    de__gjk_proxy pa;
    de__gjk_proxy_shape(&pa, ts, true, NULL);
    sweep_proxy(sw, tp, &pa, de__shape_margin(ts), tp->rcore);
}

/* The mover against the whole target (compound children in ascending index). */
static void sweep_run(ccd_sweep *sw) {
    const de__shape *ts = sw->target.shape;
    const de_vec3 lcom = sw->target.m.lcom;
    if (ts->kind != DE_SHAPE_COMPOUND) {
        ccd_piece tp;
        tp.shape = ts;
        tp.child = false;
        tp.pos = de__v3(0.0f, 0.0f, 0.0f);
        tp.rot.x = tp.rot.y = tp.rot.z = 0.0f;
        tp.rot.w = 1.0f;
        tp.rcore = sw->target.moves && de__shape_kind_is_convex(ts->kind)
                       ? piece_rcore(ts, false, tp.pos, lcom)
                       : 0.0;
        sweep_target_piece(sw, &tp);
        return;
    }
    ccd_piece whole;
    memset(&whole, 0, sizeof whole);
    whole.rot.w = 1.0f;
    const de_aabb box = cull_box(sw, &whole);
    const de__compound *cp = &ts->u.compound;
    for (u32 i = 0; i < cp->child_count && !sw->done; ++i) {
        const de__compound_child *ch = &cp->children[i];
        if (!de__aabb_overlap(&ch->bounds, &box))
            continue;
        ccd_piece tp;
        tp.shape = ch->shape;
        tp.child = true;
        tp.pos = ch->position;
        tp.rot = ch->rotation;
        tp.rcore = sw->target.moves && de__shape_kind_is_convex(ch->shape->kind)
                       ? piece_rcore(ch->shape, true, ch->position, lcom)
                       : 0.0;
        sweep_target_piece(sw, &tp);
    }
}

/* true for a convex shape or a compound whose children are all convex (a dynamic body's
 * shape). */
static bool castable(const de__shape *s) {
    if (de__shape_kind_is_convex(s->kind))
        return true;
    if (s->kind != DE_SHAPE_COMPOUND)
        return false;
    for (u32 i = 0; i < s->u.compound.child_count; ++i)
        if (!de__shape_kind_is_convex(s->u.compound.children[i].shape->kind))
            return false;
    return s->u.compound.child_count > 0;
}

/* ================================ Pair check (speculative points) ====================== */

/* Body i's motion over the prepared step, from its start pose. */
static void side_from_step(ccd_side *s, const de_physics_world *p, const de__ccd *c, u32 i,
                           const de__shape *shape) {
    const de__body_pool *b = &p->bodies;
    de__pose x;
    x.position = b->position[i];
    x.rotation = b->rotation[i];
    const de_vec3 v = c->velocity[i];
    const de_dvec3 dc = de__d3_scale(de__d3_from(v), (f64)c->dt);
    const de_vec3 w = b->angular_velocity[i];
    de_quat dq;
    dq.x = 0.0f;
    dq.y = 0.0f;
    dq.z = 0.0f;
    dq.w = 1.0f;
    if (b->type[i] != (u8)DE_BODY_STATIC && (w.x != 0.0f || w.y != 0.0f || w.z != 0.0f))
        dq = de_dintegrate_quat(dq, w, c->dt);
    side_init(s, shape, x, b->local_com[i], dc, dq);
}

bool de__ccd_pair_may_touch(const de_physics_world *p, u32 a, u32 b) {
    const de__ccd *c = de__ccd_current(p);
    if (c == NULL)
        return true;
    const de__body_pool *bp = &p->bodies;
    const de__shape *sa = de__shape_get(p, bp->shape[a]);
    const de__shape *sb = de__shape_get(p, bp->shape[b]);
    if (sa == NULL || sb == NULL)
        return true;
    /* The mover must be convex (or a compound of convex shapes); B when it can be. */
    u32 mi = b, ti = a;
    const de__shape *ms = sb, *ts = sa;
    if (!castable(sb)) {
        if (!castable(sa))
            return true;
        mi = a;
        ti = b;
        ms = sa;
        ts = sb;
    }
    ccd_sweep sw;
    memset(&sw, 0, sizeof sw);
    side_from_step(&sw.mover, p, c, mi, ms);
    side_from_step(&sw.target, p, c, ti, ts);
    sw.gap = (f64)DE__CCD_LINEAR_SLOP;
    sw.best = 1.0;
    sw.start_is_hit = true;
    sw.stop_on_hit = true;
    sweep_run(&sw);
    return sw.best < 1.0;
}

/* ================================ Bullets ============================================== */

/* Body j's pose at the end of the step: its solver body applied (as finalize will do), or its
 * pool pose when it is not awake. */
static de__pose end_pose(const de_physics_world *p, const de__solver *s, u32 j) {
    const de__body_pool *b = &p->bodies;
    de__pose x;
    x.position = b->position[j];
    x.rotation = b->rotation[j];
    const u32 slot = de__solver_slot(s, j);
    if (slot == DE__SOLVER_STATIC_SLOT)
        return x;
    const de__solver_body *sb = &s->bodies[slot];
    const de__dmat3 r0 = de__dmat3_from_quat(x.rotation);
    const de_dvec3 com =
        de__d3_add(de__d3_add(x.position, de__dmat3_mul(&r0, de__d3_from(b->local_com[j]))),
                   de__d3_from(sb->dp));
    if (sb->dq.x != 0.0f || sb->dq.y != 0.0f || sb->dq.z != 0.0f || sb->dq.w != 1.0f) {
        f64 dq[4], q0[4], q1[4];
        quat_to_d(sb->dq, dq);
        quat_to_d(x.rotation, q0);
        quat_mul_d(dq, q0, q1);
        x.rotation = quat_from_d(q1);
    }
    x.position = origin_for_com(com, x.rotation, b->local_com[j]);
    return x;
}

typedef struct ccd_bullet {
    de_physics_world *p;
    const de__solver *s;
    de__ccd *ccd;
    u32 index;
    de_dvec3 v_end; /* the bullet's velocity at the end of the step */
    ccd_sweep sw;   /* mover = the bullet; the target is filled per candidate */
} ccd_bullet;

static bool candidate_cb(void *user, u32 k) {
    ccd_bullet *bl = (ccd_bullet *)user;
    const de_physics_world *p = bl->p;
    const de__body_pool *b = &p->bodies;
    if (k == bl->index || !b->alive[k] || !de__bp_should_collide(p, bl->index, k))
        return true;
    if ((b->flags[k] & (u32)DE_BODY_FLAG_TRIGGER) != 0u)
        return true;
    /* Bullets are not swept against other awake bullets. */
    if (b->type[k] == (u8)DE_BODY_DYNAMIC && (b->flags[k] & (u32)DE_BODY_FLAG_CCD) != 0u &&
        de__solver_slot(bl->s, k) != DE__SOLVER_STATIC_SLOT)
        return true;
    const de__shape *shape = de__shape_get(p, b->shape[k]);
    if (shape == NULL)
        return true;
    /* The target holds still at its end pose. */
    const de_quat id = {0.0f, 0.0f, 0.0f, 1.0f};
    side_init(&bl->sw.target, shape, end_pose(p, bl->s, k), b->local_com[k], de__d3(0.0, 0.0, 0.0),
              id);
    const u32 slot = de__solver_slot(bl->s, k);
    const de_vec3 vt = slot != DE__SOLVER_STATIC_SLOT ? bl->s->bodies[slot].v : de__v3(0, 0, 0);
    bl->sw.end_dv = de__d3_sub(bl->v_end, de__d3_from(vt));
    ++bl->ccd->stats.candidates;
    sweep_run(&bl->sw);
    return true;
}

/* Sweeps bullet i (awake, dynamic, flagged) and moves it back to its time of impact. */
static void sweep_bullet(de_physics_world *p, de__solver *s, de__ccd *c, u32 i) {
    const de__body_pool *b = &p->bodies;
    const de__shape *shape = de__shape_get(p, b->shape[i]);
    if (shape == NULL || !castable(shape))
        return;
    de__solver_body *sb = &s->bodies[de__solver_slot(s, i)];
    ccd_bullet bl;
    memset(&bl, 0, sizeof bl);
    bl.p = p;
    bl.s = s;
    bl.ccd = c;
    bl.index = i;
    ccd_sweep *sw = &bl.sw;
    de__pose x0;
    x0.position = b->position[i];
    x0.rotation = b->rotation[i];
    side_init(&sw->mover, shape, x0, b->local_com[i], de__d3_from(sb->dp), sb->dq);
    /* Skip bullets that cannot move by the slop this step. */
    f64 rcore = 0.0;
    if (shape->kind == DE_SHAPE_COMPOUND) {
        for (u32 k = 0; k < shape->u.compound.child_count; ++k) {
            const de__compound_child *ch = &shape->u.compound.children[k];
            rcore = de__maxd(rcore, piece_rcore(ch->shape, true, ch->position, b->local_com[i]));
        }
    } else {
        rcore = piece_rcore(shape, false, de__v3(0.0f, 0.0f, 0.0f), b->local_com[i]);
    }
    const f64 motion = dlen(sw->mover.m.dc) + sw->mover.m.omega * rcore;
    if (!(motion > (f64)DE__CCD_LINEAR_SLOP))
        return;
    ++c->stats.bullets;
    sw->gap = (f64)DE__CCD_LINEAR_SLOP;
    sw->best = 1.0;
    sw->fallback = true;
    sw->end_check = true;
    sw->dt = (f64)s->ctx.dt;
    bl.v_end = de__d3_from(sb->v);
    sw->stats = &c->stats;
    const de_aabb *lb = &shape->local_aabb;
    const f32 min_half =
        de__minf(de__minf(lb->max.x - lb->min.x, lb->max.y - lb->min.y), lb->max.z - lb->min.z) *
        0.5f;
    sw->fallback_radius = DE__CCD_FALLBACK_SCALE * min_half;

    /* Candidates: every body whose broadphase box meets the path (world frame). */
    const de_dvec3 c0 = sw->mover.m.c0, c1 = de__d3_add(c0, sw->mover.m.dc);
    const f64 g = sw->mover.radius + 2.0 * (f64)DE__CCD_LINEAR_SLOP;
    de_aabb box;
    box.min.x = de__f32_round_down(de__mind(c0.x, c1.x) - g);
    box.min.y = de__f32_round_down(de__mind(c0.y, c1.y) - g);
    box.min.z = de__f32_round_down(de__mind(c0.z, c1.z) - g);
    box.max.x = de__f32_round_up(de__maxd(c0.x, c1.x) + g);
    box.max.y = de__f32_round_up(de__maxd(c0.y, c1.y) + g);
    box.max.z = de__f32_round_up(de__maxd(c0.z, c1.z) + g);
    (void)de__broadphase_query_aabb(p, box, NULL, candidate_cb, &bl);
    if (!(sw->best < 1.0))
        return;

    /* Back to the pose at the time of impact; the velocity stays (the next step's speculative
     * contact stops the bullet). */
    const f64 t = sw->best;
    const de_dvec3 dc = sw->mover.m.dc;
    sb->dp = de__v3((f32)(dc.x * t), (f32)(dc.y * t), (f32)(dc.z * t));
    f64 qt[4];
    motion_dq(&sw->mover.m, t, qt);
    sb->dq = quat_from_d(qt);
    ++c->stats.hits;
}

void de__ccd_solve(de_physics_world *p, de__solver *s) {
    de__ccd *c = p != NULL ? de__ccd_get(p) : NULL;
    if (s == NULL || c == NULL || !c->settings.enabled)
        return;
    const de__body_pool *b = &p->bodies;
    /* Awake bodies in ascending index. */
    for (u32 k = 0; k < s->awake_count; ++k) {
        const u32 i = s->awake[k];
        if (b->type[i] != (u8)DE_BODY_DYNAMIC || (b->flags[i] & (u32)DE_BODY_FLAG_CCD) == 0u ||
            (b->flags[i] & (u32)DE_BODY_FLAG_TRIGGER) != 0u)
            continue;
        sweep_bullet(p, s, c, i);
    }
}
