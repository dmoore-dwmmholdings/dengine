/*
 * contact_solver.c: the contact constraint class of the soft-step solver (DENG-217, SPEC §13.1;
 * see constraint.h and solver_internal.h). One constraint per manifold with points, gathered in
 * manifold key order (record id = index in de__contacts_manifolds).
 *
 * Per point (Box2D v3 soft step, in 3D):
 *   - Anchors. The world anchor points of the manifold point on A and B are formed in f64
 *     relative to the island's solve origin (DENG-222, physics/large_world.h); their midpoint is
 *     the contact point, and ra / rb (f32) are its offsets from the two centers of mass, each
 *     rounded once (de__phys_relative). The velocity Jacobians use these fixed anchors for the
 *     whole step. The current separation rotates the *core* anchors by each body's dq:
 *         s = dot((dpB - dpA) + (dqB cb - dqA ca), n) + base_separation,
 *         ca = ra - radius_a n,  cb = rb + radius_b n,  base_separation = separation -
 *         dot(cb - ca, n),
 *     where radius is the shape's convex radius (sphere, capsule; 0 otherwise). A rolling sphere's
 *     core anchor is its center, so rolling does not lift its contact; a tipping box's anchors
 *     are its corners and follow the rotation.
 *   - Normal. s > 0 (speculative): bias = s / h, so the gap may close but not more. s <= 0 with
 *     bias: soft step, bias = max(bias_rate * s, -push_out), mass and impulse scaled. Without
 *     bias (relax): rigid, no position bias. Accumulated impulse clamped >= 0.
 *   - Friction (solved before the normal, so non-penetration has the last word). Coulomb over
 *     two tangent directions with their own effective masses; the accumulated pair is kept in
 *     the ellipse (l0 / mu0)^2 + (l1 / mu1)^2 <= normal_impulse^2 (a circle when isotropic) by
 *     the maximum-dissipation projection (friction_clamp).
 *     Mixing: mu = sqrt(mu_a * mu_b) (de_dsqrt). Anisotropic friction (de_body_desc
 *     .friction_axis): the first body (A, then B) with an axis gives tangent 0 = that axis
 *     projected into the contact plane; mu0 mixes the bodies' friction along their axes, mu1
 *     their friction_secondary (an isotropic body uses its friction for both).
 *   - Restitution e = max(e_a, e_b), after the substeps: a point that pushed (max impulse > 0)
 *     and approached faster than the threshold gets vn = -e * approach. `approach` is the
 *     relative normal velocity at prepare, lowered to the velocity right before any solve or
 *     relax iteration that pushed: the speed at impact, not the speed at the start of the step
 *     (a speculative contact is reached in a later substep, after gravity added speed; when the
 *     relax pass is the one that stops it, the next substep's gravity step is added, because
 *     that is the velocity the body would have crossed the surface with).
 *   - Warm start: the manifold's impulses (carried by feature id, DENG-135, or by anchor
 *     proximity when the id changed, de__manifold_carry) scaled by warm_start_scale; stored back
 *     after restitution.
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt only; fixed point order.
 */
#include "physics/solver/solver_internal.h"

#include "physics/contact/manifold.h"
#include "physics/large_world.h"
#include "physics/shapes/shape_internal.h"

typedef struct cs_point {
    de_vec3 ra, rb; /**< anchors from the centers of mass (world axes, step start) */
    de_vec3 ca, cb; /**< core anchors: ra - radius_a n, rb + radius_b n (see the file comment) */
    f32 base_separation; /**< separation - dot(cb - ca, n) */
    f32 normal_mass;
    f32 tangent_mass[2];
    f32 normal_impulse;
    f32 tangent_impulse[2];
    f32 max_normal_impulse; /**< largest normal impulse step of this step (0 = never pushed) */
    f32 approach;           /**< relative normal velocity for restitution (negative = closing) */
} cs_point;

typedef struct cs_contact {
    u32 manifold;
    u32 point_count;
    de_vec3 normal;     /**< from A toward B */
    de_vec3 tangent[2]; /**< tangent[1] = normal x tangent[0] */
    f32 friction[2];    /**< along tangent[0], tangent[1] */
    f32 restitution;
    u32 soft_static; /**< 1: a side has infinite mass (ctx->static_soft) */
    cs_point points[DE__MANIFOLD_MAX_POINTS];
} cs_contact;

typedef struct cs_state {
    const de_allocator *alloc;
    cs_contact *contacts; /**< [capacity] */
    u32 capacity;
    de__manifold *manifolds; /**< this step's manifold array (gather) */
    u32 manifold_count;
    u32 points; /**< points prepared this step */
} cs_state;

/* ================================ Setup ================================================ */

/* A unit tangent of n, a function of n only (stable while the normal is). */
static de_vec3 tangent_of(de_vec3 n) {
    if (de__absf(n.x) >= 0.57735f)
        return de_dnormalize_v3(de__v3(n.y, -n.x, 0.0f));
    return de_dnormalize_v3(de__v3(0.0f, n.z, -n.y));
}

static bool has_axis(const de__body_pool *b, u32 i) {
    const de_vec3 a = b->friction_axis[i];
    return a.x != 0.0f || a.y != 0.0f || a.z != 0.0f;
}

static void contact_friction(const de_physics_world *p, u32 a, u32 b, de_vec3 n, cs_contact *cc) {
    const de__body_pool *bp = &p->bodies;
    const f32 fa = de__maxf(bp->material[a].friction, 0.0f);
    const f32 fb = de__maxf(bp->material[b].friction, 0.0f);
    const bool ana = has_axis(bp, a), anb = has_axis(bp, b);
    f32 fa1 = fa, fb1 = fb;
    if (ana)
        fa1 = de__maxf(bp->friction_secondary[a], 0.0f);
    if (anb)
        fb1 = de__maxf(bp->friction_secondary[b], 0.0f);
    de_vec3 t0 = tangent_of(n);
    if (ana || anb) {
        const u32 src = ana ? a : b;
        const de_vec3 axis = de__quat_rotate(bp->rotation[src], bp->friction_axis[src]);
        const de_vec3 proj = de__v3_sub(axis, de__v3_scale(n, de__v3_dot(axis, n)));
        if (de__v3_dot(proj, proj) > 1.0e-6f)
            t0 = de_dnormalize_v3(proj);
        else {
            fa1 = fa; /* axis along the normal: isotropic for this contact */
            fb1 = fb;
        }
    }
    cc->tangent[0] = t0;
    cc->tangent[1] = de__v3_cross(n, t0);
    cc->friction[0] = de_dsqrt(fa * fb);
    cc->friction[1] = de_dsqrt(fa1 * fb1);
}

static void gather(de__solver *s, const de__solver_ctx *ctx, void *state) {
    (void)ctx;
    cs_state *st = (cs_state *)state;
    u32 count = 0;
    de__manifold *m = de__contacts_manifolds_mut(s->p, &count);
    st->manifolds = m;
    st->manifold_count = count;
    st->points = 0;
    for (u32 i = 0; i < count; ++i) {
        if ((m[i].flags & DE__MANIFOLD_TRIGGER) != 0u || m[i].point_count == 0u)
            continue;
        de__constraint *c =
            de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_CONTACT, i, m[i].key.a, m[i].key.b);
        if (c != NULL)
            st->points += m[i].point_count;
    }
}

static de_vec3 rel_velocity(const de__solver_body *A, const de__solver_body *B, de_vec3 ra,
                            de_vec3 rb) {
    const de_vec3 va = de__v3_add(A->v, de__v3_cross(A->w, ra));
    const de_vec3 vb = de__v3_add(B->v, de__v3_cross(B->w, rb));
    return de__v3_sub(vb, va);
}

/* Convex radius of the shape on one side of a manifold (a compound's child when `child` names
 * one). */
static f32 side_radius(const de_physics_world *p, u32 body, u32 child) {
    const de__shape *s = de__shape_get(p, p->bodies.shape[body]);
    if (s == NULL)
        return 0.0f;
    if (s->kind == DE_SHAPE_COMPOUND) {
        if (child >= s->u.compound.child_count)
            return 0.0f;
        s = s->u.compound.children[child].shape;
    }
    return de__shape_margin(s);
}

static void prepare(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    cs_state *st = (cs_state *)state;
    const de_physics_world *p = ctx->p;
    const de__body_pool *bp = &p->bodies;
    const de__manifold *m = &st->manifolds[c->id];
    cs_contact *cc = &st->contacts[c->data];
    const de__solver_body *A = &ctx->bodies[c->solver_a];
    const de__solver_body *B = &ctx->bodies[c->solver_b];
    const u32 a = c->body_a, b = c->body_b;

    cc->manifold = c->id;
    cc->point_count = m->point_count;
    cc->normal = m->normal;
    contact_friction(p, a, b, m->normal, cc);
    cc->restitution =
        de__maxf(de__maxf(bp->material[a].restitution, bp->material[b].restitution), 0.0f);
    const f32 ma = A->inv_mass, mb = B->inv_mass;
    cc->soft_static = (ma == 0.0f || mb == 0.0f) ? 1u : 0u;

    /* Anchor setup (DENG-222, physics/large_world.h). World points are formed in f64 relative
     * to the island's solve origin, so every f64 term is small, and only the offsets from the
     * centers of mass are rounded to f32, once (de__phys_relative). */
    const de_dvec3 o = de__phys_constraint_origin(ctx->solver, c);
    const de_dvec3 xa = de__d3_sub(bp->position[a], o);
    const de_dvec3 xb = de__d3_sub(bp->position[b], o);
    const de__dmat3 ra_m = de__dmat3_from_quat(bp->rotation[a]);
    const de__dmat3 rb_m = de__dmat3_from_quat(bp->rotation[b]);
    const de_dvec3 ca = de__d3_add(xa, de__dmat3_mul(&ra_m, de__d3_from(bp->local_com[a])));
    const de_dvec3 cb = de__d3_add(xb, de__dmat3_mul(&rb_m, de__d3_from(bp->local_com[b])));
    const de_vec3 n = cc->normal;
    const f32 rad_a = side_radius(p, a, m->key.child_a);
    const f32 rad_b = side_radius(p, b, m->key.child_b);
    for (u32 k = 0; k < m->point_count; ++k) {
        const de__contact *mp = &m->points[k];
        cs_point *cp = &cc->points[k];
        const de_dvec3 pa = de__d3_add(xa, de__dmat3_mul(&ra_m, de__d3_from(mp->local_a)));
        const de_dvec3 pb = de__d3_add(xb, de__dmat3_mul(&rb_m, de__d3_from(mp->local_b)));
        const de_dvec3 mid = de__d3_scale(de__d3_add(pa, pb), 0.5);
        cp->ra = de__phys_relative(mid, ca);
        cp->rb = de__phys_relative(mid, cb);
        cp->ca = de__v3_sub(cp->ra, de__v3_scale(n, rad_a));
        cp->cb = de__v3_add(cp->rb, de__v3_scale(n, rad_b));
        cp->base_separation = mp->separation - de__v3_dot(de__v3_sub(cp->cb, cp->ca), n);

        const f32 kn = ma + mb + de__angular_mass(&A->inv_inertia, cp->ra, n) +
                       de__angular_mass(&B->inv_inertia, cp->rb, n);
        cp->normal_mass = kn > 0.0f ? 1.0f / kn : 0.0f;
        for (u32 j = 0; j < 2; ++j) {
            const de_vec3 t = cc->tangent[j];
            const f32 kt = ma + mb + de__angular_mass(&A->inv_inertia, cp->ra, t) +
                           de__angular_mass(&B->inv_inertia, cp->rb, t);
            cp->tangent_mass[j] = kt > 0.0f ? 1.0f / kt : 0.0f;
            cp->tangent_impulse[j] = ctx->warm_start_scale * mp->tangent_impulse[j];
        }
        cp->normal_impulse = ctx->warm_start_scale * mp->normal_impulse;
        cp->max_normal_impulse = 0.0f;
        cp->approach = de__v3_dot(rel_velocity(A, B, cp->ra, cp->rb), n);
    }
}

/* ================================ Per substep ========================================== */

static void apply(de__solver_body *A, de__solver_body *B, de_vec3 ra, de_vec3 rb, de_vec3 P) {
    A->v = de__v3_sub(A->v, de__v3_scale(P, A->inv_mass));
    A->w = de__v3_sub(A->w, de__m3_mul(&A->inv_inertia, de__v3_cross(ra, P)));
    B->v = de__v3_add(B->v, de__v3_scale(P, B->inv_mass));
    B->w = de__v3_add(B->w, de__m3_mul(&B->inv_inertia, de__v3_cross(rb, P)));
}

/* Local copies of the two solver bodies; written back only for writable sides (a static,
 * kinematic or sleeping side is never written, so parallel batches never share it). */
static void load(const de__solver_ctx *ctx, const de__constraint *c, de__solver_body *A,
                 de__solver_body *B) {
    *A = ctx->bodies[c->solver_a];
    *B = ctx->bodies[c->solver_b];
}

static void store_bodies(const de__solver_ctx *ctx, const de__constraint *c,
                         const de__solver_body *A, const de__solver_body *B) {
    if (c->flags & DE__CONSTRAINT_WRITE_A) {
        ctx->bodies[c->solver_a].v = A->v;
        ctx->bodies[c->solver_a].w = A->w;
    }
    if (c->flags & DE__CONSTRAINT_WRITE_B) {
        ctx->bodies[c->solver_b].v = B->v;
        ctx->bodies[c->solver_b].w = B->w;
    }
}

static void warm_start(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    cs_state *st = (cs_state *)state;
    const cs_contact *cc = &st->contacts[c->data];
    de__solver_body A, B;
    load(ctx, c, &A, &B);
    for (u32 k = 0; k < cc->point_count; ++k) {
        const cs_point *cp = &cc->points[k];
        const de_vec3 P =
            de__v3_add(de__v3_scale(cc->normal, cp->normal_impulse),
                       de__v3_add(de__v3_scale(cc->tangent[0], cp->tangent_impulse[0]),
                                  de__v3_scale(cc->tangent[1], cp->tangent_impulse[1])));
        apply(&A, &B, cp->ra, cp->rb, P);
    }
    store_bodies(ctx, c, &A, &B);
}

/* Keeps the accumulated friction pair inside the friction ellipse (l0 / mu0)^2 + (l1 / mu1)^2 <=
 * ln^2. Outside, the pair goes to the ellipse point of maximum dissipation for the sticking
 * impulse (l0, l1): (mu0^2 l0, mu1^2 l1) * ln / sqrt(mu0^2 l0^2 + mu1^2 l1^2). Isotropic
 * (mu0 == mu1) that is the radial clamp to the circle; anisotropic, a sliding body is pushed
 * toward its slippery axis. A zero coefficient removes that direction. */
static void friction_clamp(f32 *l0, f32 *l1, f32 mu0, f32 mu1, f32 ln) {
    if (!(mu0 > 0.0f) || !(mu1 > 0.0f)) {
        *l0 = mu0 > 0.0f ? de__maxf(-mu0 * ln, de__minf(*l0, mu0 * ln)) : 0.0f;
        *l1 = mu1 > 0.0f ? de__maxf(-mu1 * ln, de__minf(*l1, mu1 * ln)) : 0.0f;
        return;
    }
    const f32 a = mu1 * *l0, b = mu0 * *l1, lim = mu0 * mu1 * ln;
    if (a * a + b * b <= lim * lim)
        return;
    const f32 x = mu0 * *l0, y = mu1 * *l1;
    const f32 len2 = x * x + y * y;
    if (!(len2 > 0.0f)) {
        *l0 = 0.0f;
        *l1 = 0.0f;
        return;
    }
    const f32 s = ln / de_dsqrt(len2);
    *l0 = mu0 * x * s;
    *l1 = mu1 * y * s;
}

static void solve(const de__solver_ctx *ctx, void *state, de__constraint *c, bool use_bias) {
    cs_state *st = (cs_state *)state;
    cs_contact *cc = &st->contacts[c->data];
    de__solver_body A, B;
    load(ctx, c, &A, &B);
    const de_vec3 n = cc->normal;
    const de__soft soft = cc->soft_static ? ctx->static_soft : ctx->contact_soft;
    const de_vec3 dp = de__v3_sub(B.dp, A.dp);

    /* Friction first (bounded by the accumulated normal impulse): non-penetration below has
     * the last word. Solving the normal first lets a 1:100 pair creep sideways. */
    const u32 pc = cc->point_count;
    for (u32 k = 0; k < pc; ++k) {
        cs_point *cp = &cc->points[k];
        const de_vec3 dv = rel_velocity(&A, &B, cp->ra, cp->rb);
        const f32 old0 = cp->tangent_impulse[0], old1 = cp->tangent_impulse[1];
        f32 l0 = old0 - cp->tangent_mass[0] * de__v3_dot(dv, cc->tangent[0]);
        f32 l1 = old1 - cp->tangent_mass[1] * de__v3_dot(dv, cc->tangent[1]);
        friction_clamp(&l0, &l1, cc->friction[0], cc->friction[1], cp->normal_impulse);
        cp->tangent_impulse[0] = l0;
        cp->tangent_impulse[1] = l1;
        const de_vec3 P = de__v3_add(de__v3_scale(cc->tangent[0], l0 - old0),
                                     de__v3_scale(cc->tangent[1], l1 - old1));
        apply(&A, &B, cp->ra, cp->rb, P);
    }

    for (u32 k = 0; k < pc; ++k) {
        cs_point *cp = &cc->points[k];
        const de_vec3 pra = de__quat_rotate_f(A.dq, cp->ca);
        const de_vec3 prb = de__quat_rotate_f(B.dq, cp->cb);
        const de_vec3 d = de__v3_add(dp, de__v3_sub(prb, pra));
        const f32 s = de__v3_dot(d, n) + cp->base_separation;
        f32 bias = 0.0f, mass_scale = 1.0f, impulse_scale = 0.0f;
        if (s > 0.0f) {
            bias = s * ctx->inv_h; /* speculative: the gap may close, no more */
        } else if (use_bias) {
            bias = de__maxf(soft.bias_rate * s, -ctx->max_bias_velocity);
            mass_scale = soft.mass_scale;
            impulse_scale = soft.impulse_scale;
        }
        const f32 vn = de__v3_dot(rel_velocity(&A, &B, cp->ra, cp->rb), n);
        f32 impulse =
            -cp->normal_mass * mass_scale * (vn + bias) - impulse_scale * cp->normal_impulse;
        const f32 next = de__maxf(cp->normal_impulse + impulse, 0.0f);
        impulse = next - cp->normal_impulse;
        cp->normal_impulse = next;
        cp->max_normal_impulse = de__maxf(cp->max_normal_impulse, impulse);
        if (next > 0.0f) {
            /* The speed at impact (file comment). A speculative point stopped in the relax pass
             * would have crossed in the next substep, after one more gravity step. */
            f32 impact = vn;
            if (s > 0.0f && !use_bias)
                impact += de__v3_dot(de__v3_sub(B.gravity_dv, A.gravity_dv), n);
            cp->approach = de__minf(cp->approach, impact);
        }
        apply(&A, &B, cp->ra, cp->rb, de__v3_scale(n, impulse));
    }
    store_bodies(ctx, c, &A, &B);
}

static void restitution(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    cs_state *st = (cs_state *)state;
    cs_contact *cc = &st->contacts[c->data];
    const f32 e = cc->restitution;
    if (!(e > 0.0f))
        return;
    de__solver_body A, B;
    load(ctx, c, &A, &B);
    const de_vec3 n = cc->normal;
    for (u32 k = 0; k < cc->point_count; ++k) {
        cs_point *cp = &cc->points[k];
        if (cp->approach > -ctx->restitution_threshold || cp->max_normal_impulse == 0.0f)
            continue;
        const f32 vn = de__v3_dot(rel_velocity(&A, &B, cp->ra, cp->rb), n);
        f32 impulse = -cp->normal_mass * (vn + e * cp->approach);
        const f32 next = de__maxf(cp->normal_impulse + impulse, 0.0f);
        impulse = next - cp->normal_impulse;
        cp->normal_impulse = next;
        cp->max_normal_impulse = de__maxf(cp->max_normal_impulse, impulse);
        apply(&A, &B, cp->ra, cp->rb, de__v3_scale(n, impulse));
    }
    store_bodies(ctx, c, &A, &B);
}

static void store(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    (void)ctx;
    cs_state *st = (cs_state *)state;
    const cs_contact *cc = &st->contacts[c->data];
    de__manifold *m = &st->manifolds[cc->manifold];
    for (u32 k = 0; k < cc->point_count; ++k) {
        de__contact *mp = &m->points[k];
        mp->normal_impulse = cc->points[k].normal_impulse;
        mp->tangent_impulse[0] = cc->points[k].tangent_impulse[0];
        mp->tangent_impulse[1] = cc->points[k].tangent_impulse[1];
    }
}

/* ================================ Registration ========================================= */

static void destroy(de_physics_world *p, void *state) {
    (void)p;
    cs_state *st = (cs_state *)state;
    if (st == NULL)
        return;
    const de_allocator *a = st->alloc;
    if (st->contacts != NULL)
        DE_FREE(a, st->contacts, sizeof *st->contacts * (size_t)st->capacity);
    DE_FREE(a, st, sizeof *st);
}

static const de__constraint_class k_contact_class = {
    "contact", gather, prepare, warm_start, solve, NULL, restitution, store, destroy};

de_result de__contact_solver_register(de_physics_world *p) {
    de__contacts *contacts = NULL;
    DE_TRY(de__contacts_ensure(p, &contacts));
    const u32 cap = contacts->max_manifolds;
    const de_allocator *a = p->alloc;
    cs_state *st = (cs_state *)DE_ALLOC(a, sizeof *st, DE_ALIGNOF(cs_state), DE_MEM_PHYSICS);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact solver: state");
    memset(st, 0, sizeof *st);
    st->alloc = a;
    st->capacity = cap;
    st->contacts = (cs_contact *)DE_ALLOC(a, sizeof *st->contacts * (size_t)cap,
                                          DE_ALIGNOF(cs_contact), DE_MEM_PHYSICS);
    if (st->contacts == NULL) {
        DE_FREE(a, st, sizeof *st);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "contact solver: %u contacts", cap);
    }
    const de_result r =
        de__solver_register_class(p, DE__CONSTRAINT_CLASS_CONTACT, &k_contact_class, st, cap);
    if (r != DE_OK)
        destroy(p, st);
    return r;
}

u32 de__contact_solver_points(const de_physics_world *p) {
    const de__solver *s = de__solver_get(p);
    if (s == NULL || s->classes[DE__CONSTRAINT_CLASS_CONTACT].cls == NULL)
        return 0;
    return ((const cs_state *)s->classes[DE__CONSTRAINT_CLASS_CONTACT].state)->points;
}
