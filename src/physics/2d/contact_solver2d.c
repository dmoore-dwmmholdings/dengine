/*
 * contact_solver2d.c: the contact constraint class of the 2D soft-step solver (DENG-254; see
 * constraint2d.h). The 2D port of physics/solver/contact_solver.c (DENG-217, DENG-222, DENG-404):
 * one constraint per manifold with points (triggers have none), gathered in manifold key order
 * (record id = index in de__contacts2d_manifolds).
 *
 * Per point (Box2D v3 soft step):
 *   - Anchors. The manifold point's anchors on A and B are formed in f64 relative to the island's
 *     solve origin; their midpoint is the contact point, and ra / rb (f32) are its offsets from
 *     the centers of mass, each rounded once (de__p2d_relative). The Jacobians use these fixed
 *     anchors for the whole step. A side in the static slot (static, sleeping) gets ra = 0: it
 *     has no velocity, no inverse mass and never turns, so its offset changes nothing, and a long
 *     static chain far from its body origin needs no large f32 offset. The current separation
 *     moves the *core* anchors by each body's dp and dq:
 *         s = dot(dpB - dpA, n) + dot(dqB cb - cb, n) - dot(dqA ca - ca, n) + separation,
 *         ca = ra - radius_a n,  cb = rb + radius_b n,
 *     where radius is the shape's convex radius (circle, capsule, polygon rounding; 0 for
 *     segments and chains). This is the 3D formula (DENG-217) with its terms regrouped: the
 *     same value in exact arithmetic, but only rotation deltas are rounded, never the difference
 *     of two large offsets. A rolling circle's core anchor is its center, so rolling does not
 *     lift its contact; a tipping box's anchors are its corners and follow the rotation.
 *   - Normal. s > 0 (speculative): bias = s / h, so the gap may close but no more. s <= 0 with
 *     bias: soft step, bias = max(bias_rate s, -push_out), mass and impulse scaled. Without bias
 *     (relax): rigid. The accumulated impulse stays >= 0.
 *   - Friction (solved before the normal, so non-penetration has the last word): Coulomb along
 *     the tangent t = right perpendicular of n, |lambda_t| <= mu lambda_n, mu = sqrt(mu_a mu_b)
 *     (de_dsqrt).
 *   - Restitution e = max(e_a, e_b), after the substeps: a point that pushed and approached faster
 *     than the threshold gets vn = -e * approach, where approach is the speed at impact (the
 *     relative normal velocity right before any iteration that pushed; plus one gravity step when
 *     the relax pass caught a speculative point).
 *   - Warm start: the manifold's impulses (carried by feature id or by proximity, DENG-253)
 *     scaled by warm_start_scale; stored back after restitution. Impulses are per substep.
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt only; fixed point order.
 */
#include "physics/2d/manifold2d.h"
#include "physics/2d/solver2d_internal.h"

typedef struct cs2_point {
    de_vec2 ra, rb; /**< anchors from the centers of mass (plane axes, step start) */
    de_vec2 ca, cb; /**< core anchors */
    f32 separation; /**< the manifold point's separation at the start of the step */
    f32 normal_mass;
    f32 tangent_mass;
    f32 normal_impulse;
    f32 tangent_impulse;
    f32 max_normal_impulse; /**< largest normal impulse step of this step (0 = never pushed) */
    f32 approach;           /**< relative normal velocity for restitution (negative = closing) */
} cs2_point;

typedef struct cs2_contact {
    u32 manifold;
    u32 point_count;
    de_vec2 normal;  /**< from A toward B */
    de_vec2 tangent; /**< right perpendicular of the normal */
    f32 friction;
    f32 restitution;
    u32 soft_static; /**< 1: a side has infinite mass (ctx->static_soft) */
    cs2_point points[DE__P2D_MAX_POINTS];
} cs2_contact;

typedef struct cs2_state {
    const de_allocator *alloc;
    cs2_contact *contacts; /**< [capacity] */
    u32 capacity;
    de__manifold2d *manifolds; /**< this step's manifold array (gather) */
    u32 manifold_count;
    u32 points; /**< points gathered this step */
} cs2_state;

/* ================================ Setup ================================================ */

static void gather(de__solver2d *s, const de__solver2d_ctx *ctx, void *state) {
    (void)ctx;
    cs2_state *st = (cs2_state *)state;
    u32 count = 0;
    de__manifold2d *m = de__contacts2d_manifolds_mut(s->p, &count);
    st->manifolds = m;
    st->manifold_count = count;
    st->points = 0;
    for (u32 i = 0; i < count; ++i) {
        if ((m[i].flags & DE__MANIFOLD2D_TRIGGER) != 0u || m[i].point_count == 0u)
            continue;
        de__constraint2d *c = de__solver2d_add_constraint(s, DE__CONSTRAINT2D_CLASS_CONTACT, i,
                                                          m[i].key.a, m[i].key.b);
        if (c != NULL)
            st->points += m[i].point_count;
    }
}

static de_vec2 rel_velocity(const de__solver_body2d *A, const de__solver_body2d *B, de_vec2 ra,
                            de_vec2 rb) {
    return de__v2_sub(de__solver_body2d_point_velocity(B, rb),
                      de__solver_body2d_point_velocity(A, ra));
}

/* Convex radius of a body's shape (0 for segments and chains). */
static f32 side_radius(const de_physics2d_world *p, u32 body) {
    const de__shape2d *s = de__shape2d_get(p, p->bodies.shape[body]);
    if (s == NULL || s->kind == DE_SHAPE2D_CHAIN || s->kind == DE_SHAPE2D_SEGMENT)
        return 0.0f;
    return s->radius;
}

static void prepare(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c) {
    cs2_state *st = (cs2_state *)state;
    const de_physics2d_world *p = ctx->p;
    const de__body2d_pool *bp = &p->bodies;
    const de__manifold2d *m = &st->manifolds[c->id];
    cs2_contact *cc = &st->contacts[c->data];
    const de__solver_body2d *A = &ctx->bodies[c->solver_a];
    const de__solver_body2d *B = &ctx->bodies[c->solver_b];
    const u32 a = c->body_a, b = c->body_b;

    cc->manifold = c->id;
    cc->point_count = m->point_count;
    cc->normal = m->normal;
    cc->tangent = de__v2_right_perp(m->normal);
    const f32 fa = de__maxf(bp->material[a].friction, 0.0f);
    const f32 fb = de__maxf(bp->material[b].friction, 0.0f);
    cc->friction = de_dsqrt(fa * fb);
    cc->restitution =
        de__maxf(de__maxf(bp->material[a].restitution, bp->material[b].restitution), 0.0f);
    const f32 ma = A->inv_mass, mb = B->inv_mass;
    const f32 ia = A->inv_inertia, ib = B->inv_inertia;
    cc->soft_static = (ma == 0.0f || mb == 0.0f) ? 1u : 0u;

    /* Anchors: world points in f64 relative to the island's solve origin, so every f64 term is
     * small; only the offsets from the centers of mass are rounded to f32, once. */
    const de__d2 o = de__solver2d_constraint_origin(ctx->solver, c);
    const de__d2 xa = de__d2_sub(bp->position[a], o);
    const de__d2 xb = de__d2_sub(bp->position[b], o);
    const de_rot2d qa = bp->rotation[a], qb = bp->rotation[b];
    const de__d2 ca = de__d2_add(xa, de__rot2d_apply_d(qa, de__d2_from(bp->local_com[a])));
    const de__d2 cb = de__d2_add(xb, de__rot2d_apply_d(qb, de__d2_from(bp->local_com[b])));
    const de_vec2 n = cc->normal, t = cc->tangent;
    const f32 rad_a = side_radius(p, a);
    const f32 rad_b = side_radius(p, b);
    const bool static_a = c->solver_a == DE__SOLVER2D_STATIC_SLOT;
    const bool static_b = c->solver_b == DE__SOLVER2D_STATIC_SLOT;
    for (u32 k = 0; k < m->point_count; ++k) {
        const de__contact2d *mp = &m->points[k];
        cs2_point *cp = &cc->points[k];
        const de__d2 pa = de__d2_add(xa, de__rot2d_apply_d(qa, de__d2_from(mp->local_a)));
        const de__d2 pb = de__d2_add(xb, de__rot2d_apply_d(qb, de__d2_from(mp->local_b)));
        const de__d2 mid = de__d2_scale(de__d2_add(pa, pb), 0.5);
        const de_vec2 zero = de__v2(0.0f, 0.0f);
        cp->ra = static_a ? zero : de__p2d_relative(mid, ca);
        cp->rb = static_b ? zero : de__p2d_relative(mid, cb);
        cp->ca = static_a ? zero : de__v2_sub(cp->ra, de__v2_scale(n, rad_a));
        cp->cb = static_b ? zero : de__v2_add(cp->rb, de__v2_scale(n, rad_b));
        cp->separation = mp->separation;

        const f32 rna = de__v2_cross(cp->ra, n), rnb = de__v2_cross(cp->rb, n);
        const f32 kn = ma + mb + ia * rna * rna + ib * rnb * rnb;
        cp->normal_mass = kn > 0.0f ? 1.0f / kn : 0.0f;
        const f32 rta = de__v2_cross(cp->ra, t), rtb = de__v2_cross(cp->rb, t);
        const f32 kt = ma + mb + ia * rta * rta + ib * rtb * rtb;
        cp->tangent_mass = kt > 0.0f ? 1.0f / kt : 0.0f;
        cp->normal_impulse = ctx->warm_start_scale * mp->normal_impulse;
        cp->tangent_impulse = ctx->warm_start_scale * mp->tangent_impulse;
        cp->max_normal_impulse = 0.0f;
        cp->approach = de__v2_dot(rel_velocity(A, B, cp->ra, cp->rb), n);
    }
}

/* ================================ Per substep ========================================== */

static void apply(de__solver_body2d *A, de__solver_body2d *B, de_vec2 ra, de_vec2 rb, de_vec2 P) {
    A->v = de__v2_sub(A->v, de__v2_scale(P, A->inv_mass));
    A->w = A->w - A->inv_inertia * de__v2_cross(ra, P);
    B->v = de__v2_add(B->v, de__v2_scale(P, B->inv_mass));
    B->w = B->w + B->inv_inertia * de__v2_cross(rb, P);
}

/* Local copies of the two solver bodies; written back only for writable sides (a static,
 * kinematic or sleeping side is never written, so parallel batches never share it). */
static void load(const de__solver2d_ctx *ctx, const de__constraint2d *c, de__solver_body2d *A,
                 de__solver_body2d *B) {
    *A = ctx->bodies[c->solver_a];
    *B = ctx->bodies[c->solver_b];
}

static void store_bodies(const de__solver2d_ctx *ctx, const de__constraint2d *c,
                         const de__solver_body2d *A, const de__solver_body2d *B) {
    if (c->flags & DE__CONSTRAINT2D_WRITE_A) {
        ctx->bodies[c->solver_a].v = A->v;
        ctx->bodies[c->solver_a].w = A->w;
    }
    if (c->flags & DE__CONSTRAINT2D_WRITE_B) {
        ctx->bodies[c->solver_b].v = B->v;
        ctx->bodies[c->solver_b].w = B->w;
    }
}

static void warm_start(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c) {
    cs2_state *st = (cs2_state *)state;
    const cs2_contact *cc = &st->contacts[c->data];
    de__solver_body2d A, B;
    load(ctx, c, &A, &B);
    for (u32 k = 0; k < cc->point_count; ++k) {
        const cs2_point *cp = &cc->points[k];
        const de_vec2 P = de__v2_add(de__v2_scale(cc->normal, cp->normal_impulse),
                                     de__v2_scale(cc->tangent, cp->tangent_impulse));
        apply(&A, &B, cp->ra, cp->rb, P);
    }
    store_bodies(ctx, c, &A, &B);
}

static void solve(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c, bool use_bias) {
    cs2_state *st = (cs2_state *)state;
    cs2_contact *cc = &st->contacts[c->data];
    de__solver_body2d A, B;
    load(ctx, c, &A, &B);
    const de_vec2 n = cc->normal, t = cc->tangent;
    const de__soft soft = cc->soft_static ? ctx->static_soft : ctx->contact_soft;
    const de_vec2 dp = de__v2_sub(B.dp, A.dp);
    const u32 pc = cc->point_count;

    /* Friction first (bounded by the accumulated normal impulse): non-penetration below has the
     * last word. */
    for (u32 k = 0; k < pc; ++k) {
        cs2_point *cp = &cc->points[k];
        const de_vec2 dv = rel_velocity(&A, &B, cp->ra, cp->rb);
        const f32 old = cp->tangent_impulse;
        const f32 lim = cc->friction * cp->normal_impulse;
        f32 l = old - cp->tangent_mass * de__v2_dot(dv, t);
        l = de__maxf(-lim, de__minf(l, lim));
        cp->tangent_impulse = l;
        apply(&A, &B, cp->ra, cp->rb, de__v2_scale(t, l - old));
    }

    for (u32 k = 0; k < pc; ++k) {
        cs2_point *cp = &cc->points[k];
        const de_vec2 pra = de__v2_sub(de__rot2d_apply(A.dq, cp->ca), cp->ca);
        const de_vec2 prb = de__v2_sub(de__rot2d_apply(B.dq, cp->cb), cp->cb);
        const de_vec2 d = de__v2_add(dp, de__v2_sub(prb, pra));
        const f32 s = de__v2_dot(d, n) + cp->separation;
        f32 bias = 0.0f, mass_scale = 1.0f, impulse_scale = 0.0f;
        if (s > 0.0f) {
            bias = s * ctx->inv_h; /* speculative: the gap may close, no more */
        } else if (use_bias) {
            bias = de__maxf(soft.bias_rate * s, -ctx->max_bias_velocity);
            mass_scale = soft.mass_scale;
            impulse_scale = soft.impulse_scale;
        }
        const f32 vn = de__v2_dot(rel_velocity(&A, &B, cp->ra, cp->rb), n);
        f32 impulse =
            -cp->normal_mass * mass_scale * (vn + bias) - impulse_scale * cp->normal_impulse;
        const f32 next = de__maxf(cp->normal_impulse + impulse, 0.0f);
        impulse = next - cp->normal_impulse;
        cp->normal_impulse = next;
        cp->max_normal_impulse = de__maxf(cp->max_normal_impulse, impulse);
        if (next > 0.0f) {
            /* The speed at impact. A speculative point stopped in the relax pass would have
             * crossed in the next substep, after one more gravity step. */
            f32 impact = vn;
            if (s > 0.0f && !use_bias)
                impact += de__v2_dot(de__v2_sub(B.gravity_dv, A.gravity_dv), n);
            cp->approach = de__minf(cp->approach, impact);
        }
        apply(&A, &B, cp->ra, cp->rb, de__v2_scale(n, impulse));
    }
    store_bodies(ctx, c, &A, &B);
}

static void restitution(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c) {
    cs2_state *st = (cs2_state *)state;
    cs2_contact *cc = &st->contacts[c->data];
    const f32 e = cc->restitution;
    if (!(e > 0.0f))
        return;
    de__solver_body2d A, B;
    load(ctx, c, &A, &B);
    const de_vec2 n = cc->normal;
    for (u32 k = 0; k < cc->point_count; ++k) {
        cs2_point *cp = &cc->points[k];
        if (cp->approach > -ctx->restitution_threshold || cp->max_normal_impulse == 0.0f)
            continue;
        const f32 vn = de__v2_dot(rel_velocity(&A, &B, cp->ra, cp->rb), n);
        f32 impulse = -cp->normal_mass * (vn + e * cp->approach);
        const f32 next = de__maxf(cp->normal_impulse + impulse, 0.0f);
        impulse = next - cp->normal_impulse;
        cp->normal_impulse = next;
        cp->max_normal_impulse = de__maxf(cp->max_normal_impulse, impulse);
        apply(&A, &B, cp->ra, cp->rb, de__v2_scale(n, impulse));
    }
    store_bodies(ctx, c, &A, &B);
}

static void store(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c) {
    (void)ctx;
    cs2_state *st = (cs2_state *)state;
    const cs2_contact *cc = &st->contacts[c->data];
    de__manifold2d *m = &st->manifolds[cc->manifold];
    for (u32 k = 0; k < cc->point_count; ++k) {
        m->points[k].normal_impulse = cc->points[k].normal_impulse;
        m->points[k].tangent_impulse = cc->points[k].tangent_impulse;
    }
}

/* ================================ Registration ========================================= */

static void destroy(de_physics2d_world *p, void *state) {
    (void)p;
    cs2_state *st = (cs2_state *)state;
    if (st == NULL)
        return;
    const de_allocator *a = st->alloc;
    if (st->contacts != NULL)
        DE_FREE(a, st->contacts, sizeof *st->contacts * (size_t)st->capacity);
    DE_FREE(a, st, sizeof *st);
}

static const de__constraint2d_class k_contact2d_class = {
    "contact2d", gather, prepare, warm_start, solve, NULL, restitution, store, destroy};

de_result de__contact_solver2d_register(de_physics2d_world *p) {
    de__contacts2d *contacts = NULL;
    DE_TRY(de__contacts2d_ensure(p, &contacts));
    de__contact2d_stats cst;
    de__contacts2d_stats_get(p, &cst);
    const u32 cap = cst.max_manifolds > 0u ? cst.max_manifolds : 1u;
    const de_allocator *a = p->alloc;
    cs2_state *st = (cs2_state *)DE_ALLOC(a, sizeof *st, DE_ALIGNOF(cs2_state), DE_MEM_PHYSICS);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "2D contact solver: state");
    memset(st, 0, sizeof *st);
    st->alloc = a;
    st->capacity = cap;
    st->contacts = (cs2_contact *)DE_ALLOC(a, sizeof *st->contacts * (size_t)cap,
                                           DE_ALIGNOF(cs2_contact), DE_MEM_PHYSICS);
    if (st->contacts == NULL) {
        DE_FREE(a, st, sizeof *st);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "2D contact solver: %u contacts", cap);
    }
    const de_result r =
        de__solver2d_register_class(p, DE__CONSTRAINT2D_CLASS_CONTACT, &k_contact2d_class, st, cap);
    if (r != DE_OK)
        destroy(p, st);
    return r;
}

u32 de__contact_solver2d_points(const de_physics2d_world *p) {
    const de__solver2d *s = de__solver2d_get(p);
    if (s == NULL || s->classes[DE__CONSTRAINT2D_CLASS_CONTACT].cls == NULL)
        return 0;
    return ((const cs2_state *)s->classes[DE__CONSTRAINT2D_CLASS_CONTACT].state)->points;
}
