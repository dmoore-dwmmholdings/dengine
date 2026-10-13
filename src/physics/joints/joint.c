/*
 * joint.c: the joint pool, de_joint_* API, the joint constraint class and the shared rows of the
 * joint solver (DENG-220; see joint_internal.h). The rows of each kind are in joint_<kind>.c.
 */
#include "physics/joints/joint_internal.h"

#include "physics/broadphase/broadphase.h"
#include "physics/islands.h"
#include "physics/solver/solver_internal.h"

#include <dengine/de_debug.h>

/* ================================ Kinds ================================================ */

static const de__joint_ops *ops_of(u32 kind) {
    switch (kind) {
    case DE_JOINT_FIXED:
        return &de__joint_fixed_ops;
    case DE_JOINT_HINGE:
        return &de__joint_hinge_ops;
    case DE_JOINT_BALL:
        return &de__joint_ball_ops;
    case DE_JOINT_SLIDER:
        return &de__joint_slider_ops;
    case DE_JOINT_DISTANCE:
        return &de__joint_distance_ops;
    case DE_JOINT_CONE_TWIST:
        return &de__joint_cone_twist_ops;
    default:
        return &de__joint_6dof_ops;
    }
}

/* ================================ Cvars ================================================ */

static de_cvar float_cvar(const char *name, const char *help, f64 def, f64 lo, f64 hi) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_FLOAT;
    d.default_number = def;
    d.min = lo;
    d.max = hi;
    return de_cvar_register(&d);
}

static f32 cvar_f32(de_cvar cv, f32 def) {
    if (!DE_HANDLE_VALID(cv))
        return def;
    const f32 v = (f32)de_cvar_float(cv);
    return de__f32_finite(v) && v > 0.0f ? v : def;
}

/* ================================ Solver class ========================================= */

static void sides(const de__solver_ctx *ctx, const de__constraint *c, de__solver_body **A,
                  de__solver_body **B) {
    *A = &ctx->bodies[c->solver_a];
    *B = &ctx->bodies[c->solver_b];
}

/* The current geometry and velocities of joint j (record c) for one stage call. */
static void jx_begin(de__jx *x, const de__solver_ctx *ctx, const de__joints *js, const de__joint *j,
                     const de__constraint *c, bool use_bias) {
    de__solver_body *A, *B;
    sides(ctx, c, &A, &B);
    x->ctx = ctx;
    x->js = js;
    x->va = A->v;
    x->wa = A->w;
    x->vb = B->v;
    x->wb = B->w;
    x->ma = A->inv_mass;
    x->mb = B->inv_mass;
    x->ia = &A->inv_inertia;
    x->ib = &B->inv_inertia;
    x->ra = de__quat_rotate_f(A->dq, j->step.ra0);
    x->rb = de__quat_rotate_f(B->dq, j->step.rb0);
    x->d = de__v3_add(de__v3_add(de__v3_sub(B->dp, A->dp), j->step.delta_center),
                      de__v3_sub(x->rb, x->ra));
    x->qa = de__jq_mul(A->dq, j->step.qa0);
    x->qb = de__jq_mul(B->dq, j->step.qb0);
    x->use_bias = use_bias;
    if (j->hertz > 0.0f) {
        x->soft = j->step.spring;
    } else if (use_bias) {
        x->soft = js->joint_soft;
    } else {
        x->soft.bias_rate = 0.0f;
        x->soft.mass_scale = 1.0f;
        x->soft.impulse_scale = 0.0f;
    }
}

/* Velocities back to the writable sides only (constraint.h). */
static void jx_end(const de__jx *x, const de__solver_ctx *ctx, const de__constraint *c) {
    de__solver_body *A, *B;
    sides(ctx, c, &A, &B);
    if ((c->flags & DE__CONSTRAINT_WRITE_A) != 0u) {
        A->v = x->va;
        A->w = x->wa;
    }
    if ((c->flags & DE__CONSTRAINT_WRITE_B) != 0u) {
        B->v = x->vb;
        B->w = x->wb;
    }
}

static void class_gather(de__solver *s, const de__solver_ctx *ctx, void *state) {
    de__joints *js = (de__joints *)state;
    /* From scratch on every call (the solver may gather again after a wake). */
    js->gathered = 0;
    const f32 auto_hertz = DE__JOINTS_HERTZ_SUBSTEP_RATIO * ctx->inv_h;
    const f32 hertz =
        js->settings.hertz > 0.0f ? de__minf(js->settings.hertz, auto_hertz) : auto_hertz;
    js->joint_soft = de__soft_make(hertz, js->settings.damping_ratio, ctx->h);

    for (u32 i = 0; i < js->high_water; ++i) {
        const de__joint *j = &js->joints[i];
        if (!j->alive || j->broken)
            continue;
        de__constraint *c =
            de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_JOINT, i, j->body_a, j->body_b);
        if (c != NULL) {
            c->data = i;
            ++js->gathered;
        }
    }
}

/* Center of mass (f64), anchor offset from it (f32, world axes) and frame rotation of one side at
 * the step start. */
static void side_setup(const de_physics_world *p, u32 body, de_vec3 local, de_dvec3 world,
                       de_quat frame, de_dvec3 *com, de_vec3 *r0, de_quat *q0) {
    if (body == DE__CONSTRAINT_NO_BODY) {
        *com = world;
        *r0 = de__v3(0.0f, 0.0f, 0.0f);
        *q0 = frame;
        return;
    }
    const de__body_pool *b = &p->bodies;
    const de__dmat3 m = de__dmat3_from_quat(b->rotation[body]);
    *com = de__d3_add(b->position[body], de__dmat3_mul(&m, de__d3_from(b->local_com[body])));
    const de_dvec3 r =
        de__dmat3_mul(&m, de__d3_sub(de__d3_from(local), de__d3_from(b->local_com[body])));
    *r0 = de__v3((f32)r.x, (f32)r.y, (f32)r.z);
    *q0 = de_dnormalize_quat(de__jq_mul(b->rotation[body], frame));
}

static void class_prepare(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    de__joints *js = (de__joints *)state;
    de__joint *j = &js->joints[c->data];
    const de_physics_world *p = ctx->p;
    de_dvec3 ca, cb;
    side_setup(p, j->body_a, j->local_a, j->world_a, j->frame_qa, &ca, &j->step.ra0, &j->step.qa0);
    side_setup(p, j->body_b, j->local_b, j->world_b, j->frame_qb, &cb, &j->step.rb0, &j->step.qb0);
    const de_dvec3 dc = de__d3_sub(cb, ca);
    j->step.delta_center = de__v3((f32)dc.x, (f32)dc.y, (f32)dc.z);
    j->step.spring = de__soft_make(j->hertz, j->damping_ratio, ctx->h);
    j->step.motor = de__soft_make(j->motor_hertz, j->motor_damping_ratio, ctx->h);
    for (u32 k = 0; k < 6; ++k)
        j->step.max_motor[k] = j->motor_max_force[k] * ctx->h;
    /* Warm start scale (0 when warm starting is off). */
    const f32 w = ctx->warm_start_scale;
    j->lin = de__v3_scale(j->lin, w);
    j->ang = de__v3_scale(j->ang, w);
    for (u32 k = 0; k < 6; ++k) {
        j->eq[k] *= w;
        j->lower[k] *= w;
        j->upper[k] *= w;
        j->motor[k] *= w;
    }
}

static void class_warm_start(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    de__joints *js = (de__joints *)state;
    de__joint *j = &js->joints[c->data];
    de__jx x;
    jx_begin(&x, ctx, js, j, c, true);
    ops_of(j->kind)->warm_start(&x, j);
    jx_end(&x, ctx, c);
}

static void class_solve(const de__solver_ctx *ctx, void *state, de__constraint *c, bool use_bias) {
    de__joints *js = (de__joints *)state;
    de__joint *j = &js->joints[c->data];
    de__jx x;
    jx_begin(&x, ctx, js, j, c, use_bias);
    ops_of(j->kind)->solve(&x, j);
    jx_end(&x, ctx, c);
}

/* Break check: the linear impulse on the anchors per substep, as a force. Writes only joint j. */
static void class_store(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    de__joints *js = (de__joints *)state;
    de__joint *j = &js->joints[c->data];
    de__jx x;
    jx_begin(&x, ctx, js, j, c, false);
    const de_vec3 l = ops_of(j->kind)->linear_impulse(&x, j);
    j->force = de_dsqrt(de__v3_dot(l, l)) * ctx->inv_h;
    if (j->break_force > 0.0f && j->force > j->break_force) {
        j->broken = 1;
        j->break_tick = ctx->p->tick;
        j->lin = de__v3(0.0f, 0.0f, 0.0f);
        j->ang = j->lin;
        for (u32 k = 0; k < 6; ++k) {
            j->eq[k] = 0.0f;
            j->lower[k] = 0.0f;
            j->upper[k] = 0.0f;
            j->motor[k] = 0.0f;
        }
    }
}

static void joints_free(de__joints *js) {
    if (js->block != NULL)
        DE_FREE(js->alloc, js->block, js->block_size);
    DE_FREE(js->alloc, js, sizeof *js);
}

/* The solver owns the class; the pool lives in the extension slot. Unregistering (or the solver
 * going away first at world destroy) leaves the pool alone. */
static const de__constraint_class k_joint_class = {
    "joints", class_gather, class_prepare, class_warm_start, class_solve, NULL,
    NULL,     class_store,  NULL};

/* ================================ Body lists =========================================== */

static u32 edge_joint(u32 e) {
    return e >> 1;
}
static u32 edge_side(u32 e) {
    return e & 1u;
}

static void list_push(de__joints *js, u32 body, u32 joint, u32 side) {
    if (body == DE__CONSTRAINT_NO_BODY)
        return;
    js->joints[joint].next[side] = js->heads[body];
    js->heads[body] = joint * 2u + side;
}

static void list_remove(de__joints *js, u32 body, u32 joint, u32 side) {
    if (body == DE__CONSTRAINT_NO_BODY)
        return;
    const u32 target = joint * 2u + side;
    u32 *link = &js->heads[body];
    while (*link != DE__JOINT_NONE) {
        const u32 e = *link;
        if (e == target) {
            *link = js->joints[edge_joint(e)].next[edge_side(e)];
            return;
        }
        link = &js->joints[edge_joint(e)].next[edge_side(e)];
    }
    DE_ASSERT(false);
}

bool de__joints_exclude_pair(const de_physics_world *p, u32 a, u32 b) {
    const de__joints *js = de__joints_get(p);
    if (js == NULL || js->exclusions == 0u || a >= js->body_capacity)
        return false;
    for (u32 e = js->heads[a]; e != DE__JOINT_NONE;) {
        const de__joint *j = &js->joints[edge_joint(e)];
        const u32 other = edge_side(e) == 0u ? j->body_b : j->body_a;
        if (other == b && !j->collide)
            return true;
        e = j->next[edge_side(e)];
    }
    return false;
}

/* Frees joint slot i: unlinks it, bumps the generation, pushes the index. */
static void joint_free(de__joints *js, u32 i) {
    de__joint *j = &js->joints[i];
    list_remove(js, j->body_a, i, 0u);
    list_remove(js, j->body_b, i, 1u);
    if (!j->collide && j->body_a != DE__CONSTRAINT_NO_BODY && j->body_b != DE__CONSTRAINT_NO_BODY)
        --js->exclusions;
    j->alive = 0;
    j->generation = de__gen_next(j->generation);
    js->free_list[js->free_count++] = i;
    --js->live_count;
}

/* The pair (a, b) was excluded and may collide again: reinsert a's broadphase proxy so the next
 * update queries it (the broadphase keeps no candidate for an excluded pair). The pair output
 * does not depend on the tree shape, so this changes nothing else. */
static void requery_pair(de_physics_world *p, u32 a, u32 b) {
    const de__phys_ext *x = &p->ext[DE__PHYS_EXT_BROADPHASE];
    if (x->state == NULL || x->vtbl == NULL || x->vtbl->body_removed == NULL ||
        x->vtbl->body_added == NULL || !de__body_alive(p, a) || !de__body_alive(p, b) ||
        de__joints_exclude_pair(p, a, b))
        return;
    x->vtbl->body_removed(p, x->state, a);
    x->vtbl->body_added(p, x->state, a);
}

/* ================================ Extension slot ======================================= */

static void ext_destroy(de_physics_world *p, void *state) {
    de__solver *s = de__solver_get(p);
    if (s != NULL && s->classes[DE__CONSTRAINT_CLASS_JOINT].state == state)
        de__solver_unregister_class(p, DE__CONSTRAINT_CLASS_JOINT);
    joints_free((de__joints *)state);
}

/* A body is destroyed: its joints go with it (their handles become stale); the other sides wake. */
static void ext_body_removed(de_physics_world *p, void *state, u32 index) {
    de__joints *js = (de__joints *)state;
    if (index >= js->body_capacity)
        return;
    while (js->heads[index] != DE__JOINT_NONE) {
        const u32 e = js->heads[index];
        const de__joint *j = &js->joints[edge_joint(e)];
        const u32 other = edge_side(e) == 0u ? j->body_b : j->body_a;
        joint_free(js, edge_joint(e));
        if (other != DE__CONSTRAINT_NO_BODY && other != index)
            de__islands_wake_body(p, other);
    }
}

static const de__phys_ext_vtbl k_joints_vtbl = {ext_destroy, NULL, ext_body_removed};

#define JS_ALIGN 16u

static size_t js_align_up(size_t x) {
    return (x + (JS_ALIGN - 1u)) & ~(size_t)(JS_ALIGN - 1u);
}

de_result de__joints_create(de_physics_world *p, u32 capacity) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__joints_create: world is NULL");
    if (de__joints_get(p) != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__joints_create: joints exist");
    de_cvar_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.name = DE__JOINTS_MAX_CVAR;
    cd.help = "Physics joint pool size (read when the first joint is created; 0 = max(64, "
              "min(max_bodies, 4096)))";
    cd.type = DE_CVAR_INT;
    cd.min = 0.0;
    cd.max = 1048574.0;
    const de_cvar cap_cv = de_cvar_register(&cd);
    const de_cvar hz_cv = float_cvar(DE__JOINTS_HERTZ_CVAR,
                                     "Physics: stiffness of rigid joints in Hz (0 = a quarter of "
                                     "the substep rate, also the most; read when the joint pool "
                                     "is created)",
                                     0.0, 0.0, 100000.0);
    const de_cvar zeta_cv = float_cvar(DE__JOINTS_DAMPING_RATIO_CVAR,
                                       "Physics: damping ratio of rigid joints (read when the "
                                       "joint pool is created)",
                                       (f64)DE__JOINTS_DEFAULT_DAMPING_RATIO, 0.0, 1000.0);
    if (capacity == 0u && DE_HANDLE_VALID(cap_cv) && de_cvar_int(cap_cv) > 0)
        capacity = (u32)de_cvar_int(cap_cv);
    if (capacity == 0u) {
        capacity = p->bodies.capacity < DE__JOINTS_MAX_DEFAULT ? p->bodies.capacity
                                                               : DE__JOINTS_MAX_DEFAULT;
        if (capacity < DE__JOINTS_MIN_CAPACITY)
            capacity = DE__JOINTS_MIN_CAPACITY;
    }
    if (capacity > DE__HANDLE_INDEX_MASK - 1u)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__joints_create: %u joints is too many",
                       capacity);

    const de_allocator *a = p->alloc;
    de__joints *js = (de__joints *)DE_ALLOC(a, sizeof *js, DE_ALIGNOF(de__joints), DE_MEM_PHYSICS);
    if (js == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__joints_create: state");
    memset(js, 0, sizeof *js);
    js->alloc = a;
    js->p = p;
    js->settings.hertz = cvar_f32(hz_cv, 0.0f);
    js->settings.damping_ratio =
        DE_HANDLE_VALID(zeta_cv) ? (f32)de_cvar_float(zeta_cv) : DE__JOINTS_DEFAULT_DAMPING_RATIO;
    if (!de__f32_finite(js->settings.damping_ratio) || js->settings.damping_ratio < 0.0f)
        js->settings.damping_ratio = DE__JOINTS_DEFAULT_DAMPING_RATIO;
    js->capacity = capacity;
    js->body_capacity = p->bodies.capacity;
    const size_t off_joints = 0;
    const size_t off_free = js_align_up(off_joints + sizeof(de__joint) * (size_t)capacity);
    const size_t off_heads = js_align_up(off_free + sizeof(u32) * (size_t)capacity);
    const size_t size = js_align_up(off_heads + sizeof(u32) * (size_t)js->body_capacity);
    u8 *block = (u8 *)DE_ALLOC(a, size, JS_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, js, sizeof *js);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__joints_create: %u joints", capacity);
    }
    memset(block, 0, size);
    js->block = block;
    js->block_size = size;
    js->joints = (de__joint *)(void *)(block + off_joints);
    js->free_list = (u32 *)(void *)(block + off_free);
    js->heads = (u32 *)(void *)(block + off_heads);
    for (u32 i = 0; i < capacity; ++i)
        js->joints[i].generation = 1u;
    for (u32 i = 0; i < js->body_capacity; ++i)
        js->heads[i] = DE__JOINT_NONE;

    const de_result r =
        de__solver_register_class(p, DE__CONSTRAINT_CLASS_JOINT, &k_joint_class, js, capacity);
    if (r != DE_OK) {
        joints_free(js);
        return r;
    }
    de__phys_ext_set(p, DE__PHYS_EXT_JOINTS, js, &k_joints_vtbl);
    return DE_OK;
}

/* ================================ Public API ============================================ */

static de_joint joint_handle(const de__joints *js, u32 i) {
    de_joint h;
    h.id = de__handle_make(i, js->joints[i].generation);
    return h;
}

de__joint *de__joint_resolve(const de_physics_world *p, de_joint h) {
    const de__joints *js = p != NULL ? de__joints_get(p) : NULL;
    if (js == NULL || h.id == 0)
        return NULL;
    const u32 i = de__handle_index(h.id);
    if (i >= js->high_water || !js->joints[i].alive ||
        js->joints[i].generation != de__handle_gen(h.id))
        return NULL;
    return &js->joints[i];
}

static bool quat_finite(de_quat q) {
    return de__f32_finite(q.x) && de__f32_finite(q.y) && de__f32_finite(q.z) && de__f32_finite(q.w);
}

static de_quat frame_rotation(de_quat q) {
    if (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f)
        return de__jq(0.0f, 0.0f, 0.0f, 1.0f);
    return de_dnormalize_quat(q);
}

/* A side: body index (or NO_BODY), with the error message. */
static bool resolve_side(const de_physics_world *p, de_body b, de_entity e, u32 *out,
                         const char *name) {
    if (b.id != 0) {
        if (!de__body_resolve(p, b, out)) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_joint_create: %s is stale", name);
            return false;
        }
        return true;
    }
    if (e.bits != 0u) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_joint_create: entity side %s has no body (the ECS systems resolve entity "
                "joints, DENG-223)",
                name);
        return false;
    }
    *out = DE__CONSTRAINT_NO_BODY;
    return true;
}

static bool f32_nonneg(f32 v) {
    return de__f32_finite(v) && v >= 0.0f;
}

/* Validates the numbers of a desc for its kind. */
static bool validate(const de_joint_desc *d) {
    if ((u32)d->kind > (u32)DE_JOINT_6DOF)
        return false;
    if (!de__d3_finite(d->frame_a.position) || !de__d3_finite(d->frame_b.position) ||
        !quat_finite(d->frame_a.rotation) || !quat_finite(d->frame_b.rotation))
        return false;
    for (u32 k = 0; k < 6; ++k) {
        if (!de__f32_finite(d->limits_min[k]) || !de__f32_finite(d->limits_max[k]) ||
            !de__f32_finite(d->motor_target[k]) || !f32_nonneg(d->motor_max_force[k]) ||
            (u32)d->motor_mode[k] > (u32)DE_JOINT_MOTOR_POSITION ||
            (u32)d->axis_mode[k] > (u32)DE_JOINT_AXIS_FREE)
            return false;
        if (d->kind == DE_JOINT_6DOF && d->axis_mode[k] == DE_JOINT_AXIS_LIMITED &&
            d->limits_min[k] > d->limits_max[k])
            return false;
    }
    if (!f32_nonneg(d->break_force) || !f32_nonneg(d->hertz) || !f32_nonneg(d->damping_ratio) ||
        !f32_nonneg(d->motor_hertz) || !f32_nonneg(d->motor_damping_ratio))
        return false;
    if ((d->kind == DE_JOINT_HINGE || d->kind == DE_JOINT_SLIDER) && d->enable_limit &&
        d->limits_min[0] > d->limits_max[0])
        return false;
    if (d->kind == DE_JOINT_DISTANCE &&
        (d->limits_min[0] < 0.0f || d->limits_min[0] > d->limits_max[0]))
        return false;
    if (d->kind == DE_JOINT_CONE_TWIST &&
        (d->limits_min[3] > d->limits_max[3] || d->limits_max[4] < 0.0f || d->limits_max[5] < 0.0f))
        return false;
    return true;
}

static f32 clamp_angle(f32 a) {
    const f32 pi = 3.14159265359f;
    return a < -pi ? -pi : (a > pi ? pi : a);
}

/* World position of a side's anchor (f64). */
static de_dvec3 anchor_world(const de_physics_world *p, u32 body, de_dvec3 local) {
    if (body == DE__CONSTRAINT_NO_BODY)
        return local;
    const de__dmat3 m = de__dmat3_from_quat(p->bodies.rotation[body]);
    return de__d3_add(p->bodies.position[body], de__dmat3_mul(&m, local));
}

de_joint de_joint_create(de_physics_world *p, const de_joint_desc *desc) {
    de_joint none = {0};
    if (p == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_joint_create: NULL world or desc");
        return none;
    }
    if (!validate(desc)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_joint_create: bad kind, frame, limit or force");
        return none;
    }
    u32 a, b;
    if (!resolve_side(p, desc->body_a, desc->a, &a, "body_a") ||
        !resolve_side(p, desc->body_b, desc->b, &b, "body_b"))
        return none;
    if (a == DE__CONSTRAINT_NO_BODY && b == DE__CONSTRAINT_NO_BODY) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_joint_create: both sides are the world");
        return none;
    }
    if (a == b) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_joint_create: both sides are the same body");
        return none;
    }
    de__joints *js = de__joints_get(p);
    if (js == NULL) {
        if (de__joints_create(p, 0u) != DE_OK)
            return none;
        js = de__joints_get(p);
    }
    u32 i;
    if (js->free_count > 0u) {
        i = js->free_list[--js->free_count];
    } else if (js->high_water < js->capacity) {
        i = js->high_water++;
    } else {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                "de_joint_create: joint pool full (%u, cvar " DE__JOINTS_MAX_CVAR ")",
                js->capacity);
        return none;
    }
    de__joint *j = &js->joints[i];
    const u32 gen = j->generation;
    memset(j, 0, sizeof *j);
    j->generation = gen;
    j->alive = 1;
    j->kind = (u8)desc->kind;
    j->collide = desc->collide_connected ? 1u : 0u;
    j->enable_limit = desc->enable_limit ? 1u : 0u;
    j->body_a = a;
    j->body_b = b;
    j->entity_a = desc->a;
    j->entity_b = desc->b;
    j->frame_qa = frame_rotation(desc->frame_a.rotation);
    j->frame_qb = frame_rotation(desc->frame_b.rotation);
    const de_dvec3 pa = desc->frame_a.position, pb = desc->frame_b.position;
    j->local_a = de__v3((f32)pa.x, (f32)pa.y, (f32)pa.z);
    j->local_b = de__v3((f32)pb.x, (f32)pb.y, (f32)pb.z);
    j->world_a = pa;
    j->world_b = pb;
    for (u32 k = 0; k < 6; ++k) {
        j->lo[k] = desc->limits_min[k];
        j->hi[k] = desc->limits_max[k];
        j->motor_target[k] = desc->motor_target[k];
        j->motor_max_force[k] = desc->motor_max_force[k];
        j->motor_mode[k] = (u8)desc->motor_mode[k];
        j->axis_mode[k] = (u8)(desc->kind == DE_JOINT_6DOF ? desc->axis_mode[k] : 0);
    }
    j->hertz = desc->hertz;
    j->damping_ratio = desc->damping_ratio;
    j->motor_hertz = desc->motor_hertz;
    j->motor_damping_ratio = desc->motor_damping_ratio;
    j->break_force = desc->break_force;
    switch (desc->kind) {
    case DE_JOINT_HINGE:
        j->lo[0] = clamp_angle(j->lo[0]);
        j->hi[0] = clamp_angle(j->hi[0]);
        break;
    case DE_JOINT_DISTANCE:
        if (j->hi[0] == 0.0f) {
            /* Both 0: a rod of the current length. */
            const de_dvec3 wa = anchor_world(p, a, pa);
            const de_dvec3 wb = anchor_world(p, b, pb);
            const de_dvec3 dd = de__d3_sub(wb, wa);
            const f32 len = (f32)de_dsqrt64(de__d3_dot(dd, dd));
            j->lo[0] = len;
            j->hi[0] = len;
        }
        break;
    case DE_JOINT_CONE_TWIST:
        j->lo[3] = clamp_angle(j->lo[3]);
        j->hi[3] = clamp_angle(j->hi[3]);
        j->hi[4] = clamp_angle(j->hi[4]);
        j->hi[5] = clamp_angle(j->hi[5]);
        break;
    case DE_JOINT_6DOF: {
        bool lin = true, ang = true;
        for (u32 k = 0; k < 3; ++k) {
            lin = lin && j->axis_mode[k] == (u8)DE_JOINT_AXIS_LOCKED;
            ang = ang && j->axis_mode[k + 3u] == (u8)DE_JOINT_AXIS_LOCKED;
        }
        j->linear_block = lin ? 1u : 0u;
        j->angular_block = ang ? 1u : 0u;
        for (u32 k = 3; k < 6; ++k) {
            j->lo[k] = clamp_angle(j->lo[k]);
            j->hi[k] = clamp_angle(j->hi[k]);
        }
        break;
    }
    default:
        break;
    }
    list_push(js, a, i, 0u);
    list_push(js, b, i, 1u);
    if (!j->collide && a != DE__CONSTRAINT_NO_BODY && b != DE__CONSTRAINT_NO_BODY)
        ++js->exclusions;
    ++js->live_count;
    if (a != DE__CONSTRAINT_NO_BODY)
        de__islands_wake_body(p, a);
    if (b != DE__CONSTRAINT_NO_BODY)
        de__islands_wake_body(p, b);
    return joint_handle(js, i);
}

void de_joint_destroy(de_physics_world *p, de_joint h) {
    de__joint *j = de__joint_resolve(p, h);
    if (j == NULL)
        return;
    de__joints *js = de__joints_get(p);
    const u32 a = j->body_a, b = j->body_b;
    const bool excluded = !j->collide && a != DE__CONSTRAINT_NO_BODY && b != DE__CONSTRAINT_NO_BODY;
    joint_free(js, (u32)(j - js->joints));
    if (a != DE__CONSTRAINT_NO_BODY)
        de__islands_wake_body(p, a);
    if (b != DE__CONSTRAINT_NO_BODY)
        de__islands_wake_body(p, b);
    if (excluded)
        requery_pair(p, a, b);
}

bool de_joint_valid(const de_physics_world *p, de_joint h) {
    return de__joint_resolve(p, h) != NULL;
}

bool de_joint_is_broken(const de_physics_world *p, de_joint h) {
    const de__joint *j = de__joint_resolve(p, h);
    return j != NULL && j->broken != 0u;
}

bool de_joint_set_motor_target(de_physics_world *p, de_joint h, u32 axis, f32 target) {
    de__joint *j = de__joint_resolve(p, h);
    if (j == NULL || axis >= 6u || !de__f32_finite(target))
        return false;
    if (j->motor_target[axis] != target) {
        j->motor_target[axis] = target;
        if (j->body_a != DE__CONSTRAINT_NO_BODY)
            de__islands_wake_body(p, j->body_a);
        if (j->body_b != DE__CONSTRAINT_NO_BODY)
            de__islands_wake_body(p, j->body_b);
    }
    return true;
}

u32 de_physics_joint_breaks(const de_physics_world *p, de_joint_break_event *out, u32 max) {
    const de__joints *js = p != NULL ? de__joints_get(p) : NULL;
    if (js == NULL || p->tick == 0u)
        return 0u;
    u32 n = 0;
    for (u32 i = 0; i < js->high_water; ++i) {
        const de__joint *j = &js->joints[i];
        if (!j->alive || !j->broken || j->break_tick + 1u != p->tick)
            continue;
        if (out != NULL && n < max) {
            de_joint_break_event *e = &out[n];
            memset(e, 0, sizeof *e);
            e->joint = joint_handle(js, i);
            if (j->body_a != DE__CONSTRAINT_NO_BODY)
                e->body_a = de__body_handle(p, j->body_a);
            if (j->body_b != DE__CONSTRAINT_NO_BODY)
                e->body_b = de__body_handle(p, j->body_b);
            e->a = j->entity_a;
            e->b = j->entity_b;
            e->force = j->force;
        }
        ++n;
    }
    return n;
}

/* ================================ Test hash ============================================= */

typedef struct jh_buf {
    u8 b[256];
    u32 n;
} jh_buf;

static void jh_u32(jh_buf *c, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        c->b[c->n++] = (u8)(v >> (8u * i));
}
static void jh_f32(jh_buf *c, f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    jh_u32(c, u);
}
static void jh_v3(jh_buf *c, de_vec3 v) {
    jh_f32(c, v.x);
    jh_f32(c, v.y);
    jh_f32(c, v.z);
}

u64 de__joints_hash(const de_physics_world *p) {
    const de__joints *js = p != NULL ? de__joints_get(p) : NULL;
    if (js == NULL)
        return 0u;
    u64 h = 0x6A6F696E74ull;
    for (u32 i = 0; i < js->high_water; ++i) {
        const de__joint *j = &js->joints[i];
        if (!j->alive)
            continue;
        jh_buf c;
        c.n = 0;
        jh_u32(&c, i);
        jh_u32(&c, j->generation);
        jh_u32(&c, j->broken);
        jh_v3(&c, j->lin);
        jh_v3(&c, j->ang);
        for (u32 k = 0; k < 6; ++k) {
            jh_f32(&c, j->eq[k]);
            jh_f32(&c, j->lower[k]);
            jh_f32(&c, j->upper[k]);
            jh_f32(&c, j->motor[k]);
        }
        jh_f32(&c, j->force);
        h = de_hash64(c.b, c.n, h);
    }
    return h;
}

/* ================================ Geometry ============================================== */

de_quat de__joint_relative(const de__jx *x) {
    de_quat q = de__jq_mul(de__jq_conj(x->qa), x->qb);
    if (q.w < 0.0f)
        q = de__jq(-q.x, -q.y, -q.z, -q.w);
    return q;
}

void de__joint_swing_twist(de_quat q, f32 *twist, f32 *sy, f32 *sz, f32 *swing) {
    const f32 tn2 = q.x * q.x + q.w * q.w;
    de_quat t = de__jq(0.0f, 0.0f, 0.0f, 1.0f);
    *twist = 0.0f;
    if (tn2 > 1.0e-12f) {
        const f32 inv = 1.0f / de_dsqrt(tn2);
        t = de__jq(q.x * inv, 0.0f, 0.0f, q.w * inv);
        *twist = 2.0f * de_datan2(q.x, q.w);
    }
    const de_quat s = de__jq_mul(q, de__jq_conj(t));
    const f32 r = de_dsqrt(s.y * s.y + s.z * s.z);
    const f32 sw = s.w < 0.0f ? -s.w : s.w;
    const f32 sign = s.w < 0.0f ? -1.0f : 1.0f;
    if (r > 1.0e-6f) {
        const f32 angle = 2.0f * de_datan2(r, sw);
        *sy = sign * angle * s.y / r;
        *sz = sign * angle * s.z / r;
        *swing = angle;
    } else {
        *sy = sign * 2.0f * s.y;
        *sz = sign * 2.0f * s.z;
        *swing = 2.0f * r;
    }
}

/* ================================ Rows ================================================== */

static f32 row_mass(const de__jx *x, de_vec3 n, de_vec3 a1, de_vec3 a2) {
    const f32 k = (x->ma + x->mb) * de__v3_dot(n, n) + de__v3_dot(a1, de__m3_mul(x->ia, a1)) +
                  de__v3_dot(a2, de__m3_mul(x->ib, a2));
    return k > 0.0f ? 1.0f / k : 0.0f;
}

de__jrow de__jrow_linear(const de__jx *x, de_vec3 n) {
    de__jrow r;
    r.n = n;
    r.a1 = de__v3_cross(de__v3_add(x->ra, x->d), n);
    r.a2 = de__v3_cross(x->rb, n);
    r.mass = row_mass(x, r.n, r.a1, r.a2);
    return r;
}

de__jrow de__jrow_point(const de__jx *x, de_vec3 n) {
    de__jrow r;
    r.n = n;
    r.a1 = de__v3_cross(x->ra, n);
    r.a2 = de__v3_cross(x->rb, n);
    r.mass = row_mass(x, r.n, r.a1, r.a2);
    return r;
}

de__jrow de__jrow_angular(const de__jx *x, de_vec3 axis) {
    de__jrow r;
    r.n = de__v3(0.0f, 0.0f, 0.0f);
    r.a1 = axis;
    r.a2 = axis;
    r.mass = row_mass(x, r.n, r.a1, r.a2);
    return r;
}

de__jrow de__jrow_neg(de__jrow r) {
    r.n = de__v3_scale(r.n, -1.0f);
    r.a1 = de__v3_scale(r.a1, -1.0f);
    r.a2 = de__v3_scale(r.a2, -1.0f);
    return r;
}

f32 de__jrow_cdot(const de__jx *x, const de__jrow *r) {
    return de__v3_dot(r->n, de__v3_sub(x->vb, x->va)) + de__v3_dot(r->a2, x->wb) -
           de__v3_dot(r->a1, x->wa);
}

void de__jrow_apply(de__jx *x, const de__jrow *r, f32 l) {
    x->va = de__v3_sub(x->va, de__v3_scale(r->n, x->ma * l));
    x->wa = de__v3_sub(x->wa, de__m3_mul(x->ia, de__v3_scale(r->a1, l)));
    x->vb = de__v3_add(x->vb, de__v3_scale(r->n, x->mb * l));
    x->wb = de__v3_add(x->wb, de__m3_mul(x->ib, de__v3_scale(r->a2, l)));
}

void de__jrow_equality(de__jx *x, const de__jrow *r, f32 C, f32 *acc) {
    const f32 cdot = de__jrow_cdot(x, r);
    const de__soft s = x->soft;
    const f32 l = -r->mass * s.mass_scale * (cdot + s.bias_rate * C) - s.impulse_scale * *acc;
    *acc += l;
    de__jrow_apply(x, r, l);
}

void de__jrow_limit(de__jx *x, const de__jrow *r, f32 C, f32 *acc) {
    f32 bias, ms = 1.0f, is = 0.0f;
    if (C > 0.0f) {
        bias = C * x->ctx->inv_h; /* speculative: may close the gap, no more */
    } else {
        bias = x->soft.bias_rate * C;
        ms = x->soft.mass_scale;
        is = x->soft.impulse_scale;
    }
    const f32 cdot = de__jrow_cdot(x, r);
    const f32 l = -r->mass * ms * (cdot + bias) - is * *acc;
    const f32 next = de__maxf(*acc + l, 0.0f);
    const f32 applied = next - *acc;
    *acc = next;
    de__jrow_apply(x, r, applied);
}

void de__jrow_limits(de__jx *x, const de__jrow *r, f32 coord, f32 lo, f32 hi, f32 *acc_lo,
                     f32 *acc_hi) {
    de__jrow_limit(x, r, coord - lo, acc_lo);
    const de__jrow neg = de__jrow_neg(*r);
    de__jrow_limit(x, &neg, hi - coord, acc_hi);
}

void de__jrow_motor(de__jx *x, const de__joint *j, u32 axis, const de__jrow *r, f32 coord,
                    f32 *acc) {
    const f32 max = j->step.max_motor[axis];
    if (j->motor_mode[axis] == (u8)DE_JOINT_MOTOR_OFF || !(max > 0.0f))
        return;
    const f32 cdot = de__jrow_cdot(x, r);
    f32 l;
    if (j->motor_mode[axis] == (u8)DE_JOINT_MOTOR_VELOCITY) {
        l = -r->mass * (cdot - j->motor_target[axis]);
    } else {
        /* A spring in both passes: a rigid relax pass would stop the motion that a force-limited
         * biased pass starts, and the motor would only creep. */
        const de__soft s = j->motor_hertz > 0.0f ? j->step.motor : x->js->joint_soft;
        const f32 C = coord - j->motor_target[axis];
        l = -r->mass * s.mass_scale * (cdot + s.bias_rate * C) - s.impulse_scale * *acc;
    }
    f32 next = *acc + l;
    next = next < -max ? -max : (next > max ? max : next);
    const f32 applied = next - *acc;
    *acc = next;
    de__jrow_apply(x, r, applied);
}

/* ================================ Blocks ================================================ */

de_vec3 de__jm3_solve(const de_mat3 *k, de_vec3 b) {
    const f32 *m = k->m; /* column-major: (r, c) = m[c * 3 + r] */
    const f32 a00 = m[0], a10 = m[1], a20 = m[2];
    const f32 a01 = m[3], a11 = m[4], a21 = m[5];
    const f32 a02 = m[6], a12 = m[7], a22 = m[8];
    const f32 c00 = a11 * a22 - a12 * a21;
    const f32 c01 = a12 * a20 - a10 * a22;
    const f32 c02 = a10 * a21 - a11 * a20;
    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    const f32 scale = a00 * a11 * a22;
    if (a00 > 0.0f && a11 > 0.0f && a22 > 0.0f && de__absf(det) > 1.0e-6f * scale) {
        const f32 inv = 1.0f / det;
        const f32 i00 = c00, i01 = a02 * a21 - a01 * a22, i02 = a01 * a12 - a02 * a11;
        const f32 i10 = c01, i11 = a00 * a22 - a02 * a20, i12 = a02 * a10 - a00 * a12;
        const f32 i20 = c02, i21 = a01 * a20 - a00 * a21, i22 = a00 * a11 - a01 * a10;
        return de__v3((i00 * b.x + i01 * b.y + i02 * b.z) * inv,
                      (i10 * b.x + i11 * b.y + i12 * b.z) * inv,
                      (i20 * b.x + i21 * b.y + i22 * b.z) * inv);
    }
    /* Singular (locked rotation axes on both sides): the diagonal alone. */
    return de__v3(a00 > 0.0f ? b.x / a00 : 0.0f, a11 > 0.0f ? b.y / a11 : 0.0f,
                  a22 > 0.0f ? b.z / a22 : 0.0f);
}

/* Column i of (I r x) x r for the point block: (I (r x e_i)) x r. */
static de_vec3 point_col(const de_mat3 *inv_i, de_vec3 r, de_vec3 e) {
    return de__v3_cross(de__m3_mul(inv_i, de__v3_cross(r, e)), r);
}

void de__jpoint_apply(de__jx *x, de_vec3 l) {
    x->va = de__v3_sub(x->va, de__v3_scale(l, x->ma));
    x->wa = de__v3_sub(x->wa, de__m3_mul(x->ia, de__v3_cross(x->ra, l)));
    x->vb = de__v3_add(x->vb, de__v3_scale(l, x->mb));
    x->wb = de__v3_add(x->wb, de__m3_mul(x->ib, de__v3_cross(x->rb, l)));
}

void de__jpoint_solve(de__jx *x, de_vec3 *acc) {
    const de_vec3 cdot = de__v3_sub(de__v3_add(x->vb, de__v3_cross(x->wb, x->rb)),
                                    de__v3_add(x->va, de__v3_cross(x->wa, x->ra)));
    const f32 msum = x->ma + x->mb;
    de_mat3 k;
    for (u32 c = 0; c < 3; ++c) {
        const de_vec3 e = de__v3(c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f);
        const de_vec3 col =
            de__v3_add(de__v3_add(de__v3_scale(e, msum), point_col(x->ia, x->ra, e)),
                       point_col(x->ib, x->rb, e));
        k.m[c * 3 + 0] = col.x;
        k.m[c * 3 + 1] = col.y;
        k.m[c * 3 + 2] = col.z;
    }
    const de__soft s = x->soft;
    const de_vec3 rhs = de__v3_add(cdot, de__v3_scale(x->d, s.bias_rate));
    const de_vec3 sol = de__jm3_solve(&k, rhs);
    const de_vec3 l =
        de__v3_sub(de__v3_scale(sol, -s.mass_scale), de__v3_scale(*acc, s.impulse_scale));
    *acc = de__v3_add(*acc, l);
    de__jpoint_apply(x, l);
}

void de__jlock_apply(de__jx *x, de_vec3 l) {
    x->wa = de__v3_sub(x->wa, de__m3_mul(x->ia, l));
    x->wb = de__v3_add(x->wb, de__m3_mul(x->ib, l));
}

void de__jlock_solve(de__jx *x, de_vec3 *acc) {
    const de_quat q = de__joint_relative(x);
    const de_vec3 C = de__quat_rotate_f(x->qa, de__v3(2.0f * q.x, 2.0f * q.y, 2.0f * q.z));
    const de_vec3 cdot = de__v3_sub(x->wb, x->wa);
    de_mat3 k;
    for (u32 i = 0; i < 9; ++i)
        k.m[i] = x->ia->m[i] + x->ib->m[i];
    const de__soft s = x->soft;
    const de_vec3 sol = de__jm3_solve(&k, de__v3_add(cdot, de__v3_scale(C, s.bias_rate)));
    const de_vec3 l =
        de__v3_sub(de__v3_scale(sol, -s.mass_scale), de__v3_scale(*acc, s.impulse_scale));
    *acc = de__v3_add(*acc, l);
    de__jlock_apply(x, l);
}

de_vec3 de__joint_point_impulse(const de__jx *x, const de__joint *j) {
    (void)x;
    return j->lin;
}
