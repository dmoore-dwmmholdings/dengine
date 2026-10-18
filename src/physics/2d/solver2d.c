/*
 * solver2d.c: the 2D solver state, constraint class registration, the stage dispatchers and the
 * per-body kernels of the soft-step solver (DENG-254; see solver2d_internal.h and constraint2d.h).
 * The 2D port of physics/solver/solver.c and integrate.c.
 *
 * Per-body kernels (fixed ranges of solver slots, ascending body index; each slot independent):
 *   - setup: velocities, inverse mass and inertia, damping factors 1 / (1 + h c) (negative c
 *     counts as 0), gravity increment h * gravity_scale * gravity;
 *   - velocities (dynamic only; kinematic velocities are inputs): v = v d + gravity_dv,
 *     w = w d_w (0 with fixed rotation), then |v| <= max_linear_speed and |w| <= a quarter turn
 *     per substep;
 *   - positions (dynamic and kinematic): dp += h v; dq = rot(h w) dq with de_dsincos, composed
 *     and normalized in f64; da += h w;
 *   - finalize, in f64: position += dp + R0 lcom - R1 lcom, rotation = normalize(dq q0), so the
 *     center of mass moves by exactly dp and the origin turns about it. No world position is
 *     formed in f32 (SPEC §5.5, DENG-2 Q10).
 *
 * Determinism (SPEC §5.7): + - * /, de_dsqrt, de_dsqrt64 and de_dsincos only; bodies in ascending
 * index, records in record order, fixed ranges.
 */
#include "physics/2d/broadphase2d.h"
#include "physics/2d/manifold2d.h"
#include "physics/2d/solver2d_internal.h"

#include <dengine/de_debug.h>

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

static de_cvar bool_cvar(const char *name, const char *help) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_BOOL;
    d.default_number = 1.0;
    return de_cvar_register(&d);
}

static f32 cvar_f32(de_cvar cv, f32 def) {
    if (!DE_HANDLE_VALID(cv))
        return def;
    const f32 v = (f32)de_cvar_float(cv);
    return de__f32_finite(v) && v >= 0.0f ? v : def;
}

/* Registration takes the cvar registry lock: only at create. */
static void read_settings(de__solver2d_settings *out) {
    const de_cvar hz = float_cvar(DE__SOLVER2D_CONTACT_HERTZ_CVAR,
                                  "2D physics contact stiffness in Hz (capped at a quarter of the "
                                  "substep rate; read when the solver is created)",
                                  (f64)DE__SOLVER2D_DEFAULT_CONTACT_HERTZ, 0.0, 1000.0);
    const de_cvar zeta = float_cvar(DE__SOLVER2D_CONTACT_DAMPING_RATIO_CVAR,
                                    "2D physics contact damping ratio (read when the solver is "
                                    "created)",
                                    (f64)DE__SOLVER2D_DEFAULT_CONTACT_DAMPING_RATIO, 0.0, 1000.0);
    const de_cvar push = float_cvar(DE__SOLVER2D_CONTACT_PUSH_OUT_CVAR,
                                    "2D physics: most speed (m/s) at which a penetrating contact "
                                    "is pushed apart (read when the solver is created)",
                                    (f64)DE__SOLVER2D_DEFAULT_CONTACT_PUSH_OUT, 0.0, 1000.0);
    const de_cvar rest = float_cvar(DE__SOLVER2D_RESTITUTION_THRESHOLD_CVAR,
                                    "2D physics: approach speed (m/s) below which contacts do not "
                                    "bounce (read when the solver is created)",
                                    (f64)DE__SOLVER2D_DEFAULT_RESTITUTION_THRESHOLD, 0.0, 1000.0);
    const de_cvar speed = float_cvar(DE__SOLVER2D_MAX_LINEAR_SPEED_CVAR,
                                     "2D physics: dynamic bodies are clamped to this speed (m/s; "
                                     "read when the solver is created)",
                                     (f64)DE__SOLVER2D_DEFAULT_MAX_LINEAR_SPEED, 0.0, 1.0e6);
    const de_cvar warm = bool_cvar(DE__SOLVER2D_WARM_START_CVAR,
                                   "2D physics: warm start constraints with last step's impulses "
                                   "(read when the solver is created)");
    const de_cvar bullets = bool_cvar(
        DE__SOLVER2D_BULLETS_CVAR, "2D physics: sweep bullet bodies against static and kinematic "
                                   "bodies (read when the solver is created)");

    out->contact_hertz = cvar_f32(hz, DE__SOLVER2D_DEFAULT_CONTACT_HERTZ);
    out->contact_damping_ratio = cvar_f32(zeta, DE__SOLVER2D_DEFAULT_CONTACT_DAMPING_RATIO);
    out->contact_push_out = cvar_f32(push, DE__SOLVER2D_DEFAULT_CONTACT_PUSH_OUT);
    out->restitution_threshold = cvar_f32(rest, DE__SOLVER2D_DEFAULT_RESTITUTION_THRESHOLD);
    out->max_linear_speed = cvar_f32(speed, DE__SOLVER2D_DEFAULT_MAX_LINEAR_SPEED);
    out->warm_start = DE_HANDLE_VALID(warm) ? de_cvar_bool(warm) : true;
    out->bullets = DE_HANDLE_VALID(bullets) ? de_cvar_bool(bullets) : true;
}

/* ================================ Lifetime ============================================= */

#define SV2_ALIGN 16u

static size_t sv2_align(size_t x) {
    return (x + (SV2_ALIGN - 1u)) & ~(size_t)(SV2_ALIGN - 1u);
}

/* The record array and the coloring's record order share one allocation. */
static size_t records_bytes(u32 capacity) {
    return sv2_align(sizeof(de__constraint2d) * (size_t)capacity) + sizeof(u32) * (size_t)capacity;
}

static void solver_free(de__solver2d *s) {
    if (s == NULL)
        return;
    for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k) {
        de__solver2d_class_slot *c = &s->classes[k];
        if (c->cls != NULL && c->cls->destroy != NULL)
            c->cls->destroy(s->p, c->state);
        memset(c, 0, sizeof *c);
    }
    const de_allocator *a = s->alloc;
    if (s->constraints != NULL)
        DE_FREE(a, s->constraints, records_bytes(s->constraint_capacity));
    if (s->block != NULL)
        DE_FREE(a, s->block, s->block_size);
    DE_FREE(a, s, sizeof *s);
}

static void ext_destroy(de_physics2d_world *p, void *state) {
    (void)p;
    solver_free((de__solver2d *)state);
}

/* Sleep state follows the body pool (index reuse, destroyed sleeping bodies). */
static void ext_body_added(de_physics2d_world *p, void *state, u32 index) {
    (void)p;
    de__islands2d_body_added(((de__solver2d *)state)->islands, index);
}

static void ext_body_removed(de_physics2d_world *p, void *state, u32 index) {
    (void)p;
    de__islands2d_body_removed(((de__solver2d *)state)->islands, index);
}

static const de__p2d_ext_vtbl k_solver2d_vtbl = {ext_destroy, ext_body_added, ext_body_removed};

static void static_slot(de__solver_body2d *b) {
    memset(b, 0, sizeof *b);
    b->dq = de__rot2d_identity();
    b->body = DE__CONSTRAINT2D_NO_BODY;
    b->linear_damping = 1.0f;
    b->angular_damping = 1.0f;
}

static de_result solver_create(de_physics2d_world *p, de__solver2d **out) {
    const de_allocator *a = p->alloc;
    de__solver2d *s =
        (de__solver2d *)DE_ALLOC(a, sizeof *s, DE_ALIGNOF(de__solver2d), DE_MEM_PHYSICS);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver2d_ensure: state");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->p = p;
    read_settings(&s->settings);
    const u32 cap = p->bodies.capacity;
    s->body_capacity = cap;

    const size_t off_bodies = 0;
    const size_t off_slot = sv2_align(off_bodies + sizeof(de__solver_body2d) * ((size_t)cap + 1u));
    const size_t off_awake = sv2_align(off_slot + sizeof(u32) * (size_t)cap);
    const size_t off_islands = sv2_align(off_awake + sizeof(u32) * (size_t)cap);
    const size_t off_coloring = sv2_align(off_islands + de__islands2d_bytes(cap));
    const size_t size = sv2_align(off_coloring + de__coloring2d_bytes(cap + 1u));
    u8 *block = (u8 *)DE_ALLOC(a, size, SV2_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver2d_ensure: %u solver bodies", cap);
    }
    memset(block, 0, size);
    s->block = block;
    s->block_size = size;
    s->bodies = (de__solver_body2d *)(void *)(block + off_bodies);
    s->slot_of = (u32 *)(void *)(block + off_slot);
    s->awake = (u32 *)(void *)(block + off_awake);
    s->islands = de__islands2d_init(block + off_islands, cap);
    s->coloring = de__coloring2d_init(block + off_coloring, cap + 1u);
    static_slot(&s->bodies[0]);
    s->body_count = 1;
    s->ctx.p = p;
    s->ctx.solver = s;
    s->ctx.bodies = s->bodies;
    s->ctx.settings = &s->settings;
    *out = s;
    return DE_OK;
}

de_result de__solver2d_ensure(de_physics2d_world *p, de__solver2d **out) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__solver2d_ensure: world is NULL");
    de__solver2d *s = de__solver2d_get(p);
    if (s == NULL) {
        de__contacts2d *c = NULL;
        DE_TRY(de__contacts2d_ensure(p, &c));
        DE_TRY(solver_create(p, &s));
        de__p2d_ext_set(p, DE__P2D_EXT_SOLVER, s, &k_solver2d_vtbl);
        const de_result r = de__contact_solver2d_register(p);
        if (r != DE_OK) {
            de__p2d_ext_set(p, DE__P2D_EXT_SOLVER, NULL, NULL);
            solver_free(s);
            return r;
        }
    }
    if (out != NULL)
        *out = s;
    return DE_OK;
}

de_result de__solver2d_register_class(de_physics2d_world *p, u32 class_id,
                                      const de__constraint2d_class *cls, void *state,
                                      u32 max_constraints) {
    if (p == NULL || cls == NULL || class_id >= DE__CONSTRAINT2D_CLASS_COUNT ||
        cls->gather == NULL || cls->solve == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__solver2d_register_class: bad class %u",
                       class_id);
    de__solver2d *s = NULL;
    DE_TRY(de__solver2d_ensure(p, &s));
    if (s->classes[class_id].cls != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__solver2d_register_class: class %u exists",
                       class_id);
    DE_ASSERT(!s->active);
    u64 total = max_constraints;
    for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k)
        total += s->classes[k].capacity;
    if (total > 0x3FFFFFFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__solver2d_register_class: too many constraints");
    if (total > s->constraint_capacity) {
        u8 *mem = (u8 *)DE_ALLOC(s->alloc, records_bytes((u32)total), SV2_ALIGN, DE_MEM_PHYSICS);
        if (mem == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver2d_register_class: %u records",
                           (u32)total);
        if (s->constraints != NULL)
            DE_FREE(s->alloc, s->constraints, records_bytes(s->constraint_capacity));
        s->constraints = (de__constraint2d *)(void *)mem;
        s->constraint_capacity = (u32)total;
        s->constraint_count = 0;
        de__coloring2d_bind_order(
            s->coloring, (u32 *)(void *)(mem + sv2_align(sizeof(de__constraint2d) * (size_t)total)),
            (u32)total);
    }
    de__solver2d_class_slot *c = &s->classes[class_id];
    c->cls = cls;
    c->state = state;
    c->capacity = max_constraints;
    c->count = 0;
    c->iterations = 1;
    return DE_OK;
}

de_result de__solver2d_set_class_iterations(de_physics2d_world *p, u32 class_id, u32 iterations) {
    de__solver2d *s = p != NULL ? de__solver2d_get(p) : NULL;
    if (s == NULL || class_id >= DE__CONSTRAINT2D_CLASS_COUNT || s->classes[class_id].cls == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__solver2d_set_class_iterations: class %u is not registered", class_id);
    DE_ASSERT(!s->active);
    s->classes[class_id].iterations =
        iterations < 1u
            ? 1u
            : (iterations > DE__SOLVER2D_MAX_CLASS_ITERATIONS ? DE__SOLVER2D_MAX_CLASS_ITERATIONS
                                                              : iterations);
    return DE_OK;
}

void de__solver2d_unregister_class(de_physics2d_world *p, u32 class_id) {
    de__solver2d *s = p != NULL ? de__solver2d_get(p) : NULL;
    if (s == NULL || class_id >= DE__CONSTRAINT2D_CLASS_COUNT || s->classes[class_id].cls == NULL)
        return;
    DE_ASSERT(!s->active);
    de__solver2d_class_slot *c = &s->classes[class_id];
    if (c->cls->destroy != NULL)
        c->cls->destroy(p, c->state);
    memset(c, 0, sizeof *c);
}

/* ================================ Records ============================================== */

u32 de__solver2d_slot(const de__solver2d *s, u32 body_index) {
    return body_index < s->body_capacity ? s->slot_of[body_index] : DE__SOLVER2D_STATIC_SLOT;
}

static u32 side_slot(const de__solver2d *s, u32 body, bool *writable) {
    *writable = false;
    if (body == DE__CONSTRAINT2D_NO_BODY)
        return DE__SOLVER2D_STATIC_SLOT;
    const u32 slot = de__solver2d_slot(s, body);
    *writable = slot != DE__SOLVER2D_STATIC_SLOT &&
                (s->bodies[slot].flags & DE__SOLVER2D_BODY_DYNAMIC) != 0u;
    return slot;
}

/* An awake side that may wake a sleeping one: a writable (awake dynamic) body, or a kinematic
 * body that moves. */
static bool side_active(const de__solver2d *s, u32 slot, bool writable) {
    if (writable)
        return true;
    if (slot == DE__SOLVER2D_STATIC_SLOT)
        return false;
    const de__solver_body2d *b = &s->bodies[slot];
    return (b->flags & DE__SOLVER2D_BODY_KINEMATIC) != 0u &&
           (b->v.x != 0.0f || b->v.y != 0.0f || b->w != 0.0f);
}

static bool side_sleeping(const de__islands2d *is, u32 body) {
    return body != DE__CONSTRAINT2D_NO_BODY && body < is->capacity && is->sleeping[body] != 0u;
}

de__constraint2d *de__solver2d_add_constraint(de__solver2d *s, u32 class_id, u32 id, u32 body_a,
                                              u32 body_b) {
    DE_ASSERT(class_id < DE__CONSTRAINT2D_CLASS_COUNT);
    bool wa, wb;
    const u32 sa = side_slot(s, body_a, &wa);
    const u32 sb = side_slot(s, body_b, &wb);
    de__islands2d *is = s->islands;
    if (is->stats.sleeping_bodies != 0u) {
        /* A constraint between a sleeping body and an active one wakes the sleeper's island; the
         * solver gathers again (de__solver2d_prepare). */
        const bool za = side_sleeping(is, body_a), zb = side_sleeping(is, body_b);
        if (za && !zb && side_active(s, sb, wb))
            de__islands2d_request_wake(is, body_a);
        else if (zb && !za && side_active(s, sa, wa))
            de__islands2d_request_wake(is, body_b);
    }
    if (!wa && !wb)
        return NULL;
    de__solver2d_class_slot *c = &s->classes[class_id];
    if (c->count >= c->capacity || s->constraint_count >= s->constraint_capacity) {
        ++s->stats.dropped;
        return NULL;
    }
    de__constraint2d *r = &s->constraints[s->constraint_count++];
    r->class_id = class_id;
    r->id = id;
    r->body_a = body_a;
    r->body_b = body_b;
    r->solver_a = sa;
    r->solver_b = sb;
    r->flags = (wa ? DE__CONSTRAINT2D_WRITE_A : 0u) | (wb ? DE__CONSTRAINT2D_WRITE_B : 0u);
    r->data = c->count++;
    r->color = DE__CONSTRAINT2D_NO_COLOR;
    return r;
}

de__d2 de__solver2d_constraint_origin(const de__solver2d *s, const de__constraint2d *c) {
    const de__islands2d *is = s->islands;
    const u32 slot = (c->flags & DE__CONSTRAINT2D_WRITE_A) != 0u   ? c->solver_a
                     : (c->flags & DE__CONSTRAINT2D_WRITE_B) != 0u ? c->solver_b
                                                                   : DE__SOLVER2D_STATIC_SLOT;
    if (slot == DE__SOLVER2D_STATIC_SLOT || slot >= s->body_count)
        return de__d2_make(0.0, 0.0);
    const u32 isl = is->island_of_slot[slot];
    if (isl == DE__ISLAND2D_NONE || isl >= is->island_count)
        return de__d2_make(0.0, 0.0);
    return is->islands[isl].origin;
}

/* ================================ Per-body kernels ===================================== */

static void init_body(const de__solver2d *s, u32 i, de__solver_body2d *out) {
    const de__body2d_pool *b = &s->p->bodies;
    const de__solver2d_ctx *x = &s->ctx;
    memset(out, 0, sizeof *out);
    out->v = b->linear_velocity[i];
    out->w = b->angular_velocity[i];
    out->dq = de__rot2d_identity();
    out->body = i;
    out->linear_damping = 1.0f;
    out->angular_damping = 1.0f;
    if (b->type[i] != (u8)DE_BODY_DYNAMIC) {
        out->flags = DE__SOLVER2D_BODY_KINEMATIC;
        return;
    }
    u32 flags = DE__SOLVER2D_BODY_DYNAMIC;
    out->inv_mass = b->inv_mass[i];
    out->inv_inertia = b->inv_inertia[i];
    if ((b->flags[i] & DE_BODY2D_FLAG_FIXED_ROTATION) != 0u) {
        flags |= DE__SOLVER2D_BODY_FIXED_ROTATION;
        out->inv_inertia = 0.0f;
    }
    out->flags = flags;
    /* Damping as in Box2D v3: v *= 1 / (1 + h c) per substep (negative c counts as 0). */
    out->linear_damping = 1.0f / (1.0f + x->h * de__maxf(b->linear_damping[i], 0.0f));
    out->angular_damping = 1.0f / (1.0f + x->h * de__maxf(b->angular_damping[i], 0.0f));
    out->gravity_dv = de__v2_scale(s->p->gravity, x->h * b->gravity_scale[i]);
}

static void init_bodies(de__solver2d *s, u32 begin, u32 end) {
    for (u32 k = begin; k < end; ++k)
        init_body(s, s->awake[k - 1u], &s->bodies[k]);
}

static de_vec2 clamp_length(de_vec2 v, f32 max) {
    const f32 l2 = de__v2_dot(v, v);
    if (!(l2 > max * max))
        return v;
    return de__v2_scale(v, max / de_dsqrt(l2));
}

static void integrate_velocities(de__solver2d *s, u32 begin, u32 end) {
    const f32 max_v = s->settings.max_linear_speed;
    const f32 max_w = DE__SOLVER2D_MAX_ROTATION_PER_SUBSTEP * s->ctx.inv_h;
    for (u32 k = begin; k < end; ++k) {
        de__solver_body2d *sb = &s->bodies[k];
        if ((sb->flags & DE__SOLVER2D_BODY_DYNAMIC) == 0u)
            continue; /* kinematic: velocity is an input */
        const de_vec2 v = de__v2_add(de__v2_scale(sb->v, sb->linear_damping), sb->gravity_dv);
        f32 w = sb->w * sb->angular_damping;
        if (sb->flags & DE__SOLVER2D_BODY_FIXED_ROTATION)
            w = 0.0f;
        sb->v = clamp_length(v, max_v);
        sb->w = de__maxf(-max_w, de__minf(w, max_w));
    }
}

/* a * b (apply b, then a) for unit rotations, in f64, normalized and rounded once. */
static de_rot2d rot_mul_normalize(f32 ac, f32 as, de_rot2d b) {
    const f64 c = (f64)ac * (f64)b.c - (f64)as * (f64)b.s;
    const f64 s = (f64)as * (f64)b.c + (f64)ac * (f64)b.s;
    const f64 l2 = c * c + s * s;
    de_rot2d r;
    if (!(l2 > 0.0))
        return de__rot2d_identity();
    const f64 inv = 1.0 / de_dsqrt64(l2);
    r.c = (f32)(c * inv);
    r.s = (f32)(s * inv);
    return r;
}

static void integrate_positions(de__solver2d *s, u32 begin, u32 end) {
    const f32 h = s->ctx.h;
    for (u32 k = begin; k < end; ++k) {
        de__solver_body2d *sb = &s->bodies[k];
        sb->dp = de__v2_add(sb->dp, de__v2_scale(sb->v, h));
        if (sb->w != 0.0f) {
            const f32 angle = sb->w * h;
            f32 sn, cs;
            de_dsincos(angle, &sn, &cs);
            sb->dq = rot_mul_normalize(cs, sn, sb->dq);
            sb->da += angle;
        }
    }
}

static void finalize_bodies(de__solver2d *s, u32 begin, u32 end) {
    de_physics2d_world *p = s->p;
    de__body2d_pool *b = &p->bodies;
    for (u32 k = begin; k < end; ++k) {
        const de__solver_body2d *sb = &s->bodies[k];
        const u32 i = sb->body;
        b->linear_velocity[i] = sb->v;
        b->angular_velocity[i] = sb->w;
        de__d2 pos = de__d2_add(b->position[i], de__d2_from(sb->dp));
        if (sb->dq.c != 1.0f || sb->dq.s != 0.0f) {
            const de_rot2d q0 = b->rotation[i];
            const de_rot2d q1 = rot_mul_normalize(sb->dq.c, sb->dq.s, q0);
            const de__d2 lcom = de__d2_from(b->local_com[i]);
            const de__d2 shift =
                de__d2_sub(de__rot2d_apply_d(q0, lcom), de__rot2d_apply_d(q1, lcom));
            pos = de__d2_add(pos, shift);
            b->rotation[i] = q1;
        }
        b->position[i] = pos;
        de__body2d_refresh_aabb(p, i);
    }
}

/* ================================ Stages =============================================== */

de__solver2d *de__solver2d_begin(de_physics2d_world *p, f32 dt) {
    de__solver2d *s = NULL;
    if (de__solver2d_ensure(p, &s) != DE_OK)
        return NULL;
    DE_ASSERT(!s->active);
    de__solver2d_ctx *x = &s->ctx;
    x->bodies = s->bodies;
    x->dt = dt;
    x->substeps = p->substeps > 0 ? p->substeps : 1u;
    x->h = dt / (f32)x->substeps;
    x->inv_h = 1.0f / x->h;
    x->substep = 0;
    x->pass = 0;
    /* Contact stiffness: at most a quarter of the substep rate, twice that against infinite mass
     * (static or kinematic side), as in Box2D v3. */
    const f32 hertz = de__minf(s->settings.contact_hertz, 0.25f * x->inv_h);
    x->contact_soft = de__soft_make(hertz, s->settings.contact_damping_ratio, x->h);
    x->static_soft = de__soft_make(2.0f * hertz, s->settings.contact_damping_ratio, x->h);
    x->max_bias_velocity = s->settings.contact_push_out;
    x->restitution_threshold = s->settings.restitution_threshold;
    x->warm_start_scale = s->settings.warm_start ? 1.0f : 0.0f;

    s->body_count = 1;
    s->awake_count = 0;
    s->constraint_count = 0;
    for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k)
        s->classes[k].count = 0;
    memset(&s->stats, 0, sizeof s->stats);
    memset(&s->bullet_stats, 0, sizeof s->bullet_stats);
    s->active = true;
    return s;
}

u32 de__solver2d_add_awake(de__solver2d *s, u32 body_index) {
    DE_ASSERT(body_index < s->body_capacity);
    DE_ASSERT(s->awake_count == 0 || s->awake[s->awake_count - 1u] < body_index);
    DE_ASSERT(s->body_count <= s->body_capacity);
    const u32 slot = s->body_count++;
    s->awake[s->awake_count++] = body_index;
    s->slot_of[body_index] = slot;
    return slot;
}

/* Back to an empty awake set (a constraint woke an island: collect and gather again). */
static void reset_awake(de__solver2d *s) {
    for (u32 k = 0; k < s->awake_count; ++k)
        s->slot_of[s->awake[k]] = DE__SOLVER2D_STATIC_SLOT;
    s->body_count = 1;
    s->awake_count = 0;
    s->constraint_count = 0;
    for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k)
        s->classes[k].count = 0;
    s->stats.dropped = 0;
}

void de__solver2d_prepare(de__solver2d *s) {
    for (;;) {
        de__solver2d_for_bodies(s, init_bodies);
        for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k) {
            const de__solver2d_class_slot *c = &s->classes[k];
            if (c->cls != NULL)
                c->cls->gather(s, &s->ctx, c->state);
        }
        if (!de__islands2d_wake_pending(s->islands))
            break;
        /* Each pass wakes at least one island, so this ends. */
        reset_awake(s);
        ++s->islands->stats.regathers;
        de__islands2d_collect_awake(s->p, s);
    }
    de__islands2d_build(s);
    de__graph2d_color(s);
    de__solver2d_run_stage(s, DE__STAGE2D_PREPARE);
    s->stats.awake_bodies = s->awake_count;
    s->stats.constraints = s->constraint_count;
    for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k)
        s->stats.class_count[k] = s->classes[k].count;
    s->stats.contact_points = de__contact_solver2d_points(s->p);
}

void de__solver2d_integrate_velocities(de__solver2d *s) {
    de__solver2d_for_bodies(s, integrate_velocities);
}

void de__solver2d_warm_start(de__solver2d *s) {
    de__solver2d_run_stage(s, DE__STAGE2D_WARM_START);
}

/* Most iterations of a class with records this step (1 when none asks for more). */
static u32 max_class_iterations(const de__solver2d *s) {
    u32 n = 1;
    for (u32 k = 0; k < DE__CONSTRAINT2D_CLASS_COUNT; ++k) {
        const de__solver2d_class_slot *c = &s->classes[k];
        if (c->cls != NULL && c->count > 0u && c->iterations > n)
            n = c->iterations;
    }
    return n;
}

/* The extra passes of classes with more than one iteration, then the pass over every record (so
 * later classes, contacts, keep the last word). */
static void run_iterated(de__solver2d *s, de__solver2d_stage stage) {
    const u32 n = max_class_iterations(s);
    for (u32 pass = 1; pass < n; ++pass) {
        s->ctx.pass = pass; /* set before the workers start; read-only for them */
        de__solver2d_run_stage_extra(s, stage, pass);
    }
    s->ctx.pass = 0;
    de__solver2d_run_stage(s, stage);
}

void de__solver2d_solve(de__solver2d *s) {
    run_iterated(s, DE__STAGE2D_SOLVE);
}

void de__solver2d_integrate_positions(de__solver2d *s) {
    de__solver2d_for_bodies(s, integrate_positions);
}

void de__solver2d_relax(de__solver2d *s) {
    run_iterated(s, DE__STAGE2D_RELAX);
    ++s->ctx.substep;
}

void de__solver2d_restitution(de__solver2d *s) {
    de__solver2d_run_stage(s, DE__STAGE2D_RESTITUTION);
}

void de__solver2d_store(de__solver2d *s) {
    de__solver2d_run_stage(s, DE__STAGE2D_STORE);
}

void de__solver2d_finalize(de__solver2d *s) {
    de__solver2d_for_bodies(s, finalize_bodies);
    for (u32 k = 0; k < s->awake_count; ++k)
        s->slot_of[s->awake[k]] = DE__SOLVER2D_STATIC_SLOT;
    /* Proxies follow the moved bodies now, so queries between steps find them. */
    de__bp2d_sync(s->p);
    s->active = false;
}

void de__solver2d_stats_get(const de_physics2d_world *p, de__solver2d_stats *out) {
    const de__solver2d *s = p != NULL ? de__solver2d_get(p) : NULL;
    if (s == NULL)
        memset(out, 0, sizeof *out);
    else
        *out = s->stats;
}
