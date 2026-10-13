/*
 * solver.c: the solver state, constraint class registration and the stage dispatchers of the
 * soft-step solver (DENG-217; see solver_internal.h). DENG-218 owns this file in wave 2 and
 * replaces the serial stage loops with graph-colored parallel batches.
 *
 * Every stage here visits the constraint records in the order they were added (classes in
 * ascending id, each in its own stable order) and the solver bodies in ascending slot order
 * (= ascending body index), on the calling thread (SPEC §13.2, §5.7 rule 5).
 */
#include "physics/solver/solver_internal.h"

#include "physics/broadphase/broadphase.h"
#include "physics/contact/manifold.h"

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

static f32 cvar_f32(de_cvar cv, f32 def, f32 lo) {
    if (!DE_HANDLE_VALID(cv))
        return def;
    const f32 v = (f32)de_cvar_float(cv);
    return de__f32_finite(v) && v >= lo ? v : def;
}

/* Registration takes the cvar registry lock: only at create. */
static void read_settings(de__solver_settings *out) {
    const de_cvar hz = float_cvar(DE__SOLVER_CONTACT_HERTZ_CVAR,
                                  "Physics contact stiffness in Hz (capped at a quarter of the "
                                  "substep rate; read when the solver is created)",
                                  (f64)DE__SOLVER_DEFAULT_CONTACT_HERTZ, 0.0, 1000.0);
    const de_cvar zeta = float_cvar(DE__SOLVER_CONTACT_DAMPING_RATIO_CVAR,
                                    "Physics contact damping ratio (read when the solver is "
                                    "created)",
                                    (f64)DE__SOLVER_DEFAULT_CONTACT_DAMPING_RATIO, 0.0, 1000.0);
    const de_cvar push = float_cvar(DE__SOLVER_CONTACT_PUSH_OUT_CVAR,
                                    "Physics: most speed (m/s) at which a penetrating contact is "
                                    "pushed apart (read when the solver is created)",
                                    (f64)DE__SOLVER_DEFAULT_CONTACT_PUSH_OUT, 0.0, 1000.0);
    const de_cvar rest = float_cvar(DE__SOLVER_RESTITUTION_THRESHOLD_CVAR,
                                    "Physics: approach speed (m/s) below which contacts do not "
                                    "bounce (read when the solver is created)",
                                    (f64)DE__SOLVER_DEFAULT_RESTITUTION_THRESHOLD, 0.0, 1000.0);
    const de_cvar speed = float_cvar(DE__SOLVER_MAX_LINEAR_SPEED_CVAR,
                                     "Physics: dynamic bodies are clamped to this speed (m/s; "
                                     "read when the solver is created)",
                                     (f64)DE__SOLVER_DEFAULT_MAX_LINEAR_SPEED, 0.0, 1.0e6);
    de_cvar_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.name = DE__SOLVER_WARM_START_CVAR;
    wd.help = "Physics: warm start constraints with last step's impulses (read when the solver "
              "is created)";
    wd.type = DE_CVAR_BOOL;
    wd.default_number = 1.0;
    const de_cvar warm = de_cvar_register(&wd);

    out->contact_hertz = cvar_f32(hz, DE__SOLVER_DEFAULT_CONTACT_HERTZ, 0.0f);
    out->contact_damping_ratio = cvar_f32(zeta, DE__SOLVER_DEFAULT_CONTACT_DAMPING_RATIO, 0.0f);
    out->contact_push_out = cvar_f32(push, DE__SOLVER_DEFAULT_CONTACT_PUSH_OUT, 0.0f);
    out->restitution_threshold = cvar_f32(rest, DE__SOLVER_DEFAULT_RESTITUTION_THRESHOLD, 0.0f);
    out->max_linear_speed = cvar_f32(speed, DE__SOLVER_DEFAULT_MAX_LINEAR_SPEED, 0.0f);
    out->warm_start = DE_HANDLE_VALID(warm) ? de_cvar_bool(warm) : true;
}

/* ================================ Lifetime ============================================= */

#define SV_ALIGN 16u

static size_t sv_align_up(size_t x) {
    return (x + (SV_ALIGN - 1u)) & ~(size_t)(SV_ALIGN - 1u);
}

static void solver_free(de__solver *s) {
    if (s == NULL)
        return;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k) {
        de__solver_class_slot *c = &s->classes[k];
        if (c->cls != NULL && c->cls->destroy != NULL)
            c->cls->destroy(s->p, c->state);
        memset(c, 0, sizeof *c);
    }
    const de_allocator *a = s->alloc;
    if (s->constraints != NULL)
        DE_FREE(a, s->constraints, sizeof *s->constraints * (size_t)s->constraint_capacity);
    if (s->block != NULL)
        DE_FREE(a, s->block, s->block_size);
    DE_FREE(a, s, sizeof *s);
}

static void ext_destroy(de_physics_world *p, void *state) {
    (void)p;
    solver_free((de__solver *)state);
}

static const de__phys_ext_vtbl k_solver_vtbl = {ext_destroy, NULL, NULL};

static void static_slot(de__solver_body *b) {
    memset(b, 0, sizeof *b);
    b->dq.w = 1.0f;
    b->body = DE__CONSTRAINT_NO_BODY;
    b->linear_damping = 1.0f;
    b->angular_damping = 1.0f;
}

static de_result solver_create(de_physics_world *p, de__solver **out) {
    const de_allocator *a = p->alloc;
    de__solver *s = (de__solver *)DE_ALLOC(a, sizeof *s, DE_ALIGNOF(de__solver), DE_MEM_PHYSICS);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver_ensure: state");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->p = p;
    read_settings(&s->settings);
    const u32 cap = p->bodies.capacity;
    s->body_capacity = cap;

    size_t off_bodies = 0;
    size_t off_slot = sv_align_up(off_bodies + sizeof(de__solver_body) * ((size_t)cap + 1u));
    size_t off_awake = sv_align_up(off_slot + sizeof(u32) * (size_t)cap);
    const size_t size = sv_align_up(off_awake + sizeof(u32) * (size_t)cap);
    u8 *block = (u8 *)DE_ALLOC(a, size, SV_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver_ensure: %u solver bodies", cap);
    }
    s->block = block;
    s->block_size = size;
    s->bodies = (de__solver_body *)(void *)(block + off_bodies);
    s->slot_of = (u32 *)(void *)(block + off_slot);
    s->awake = (u32 *)(void *)(block + off_awake);
    memset(s->slot_of, 0, sizeof(u32) * (size_t)cap);
    static_slot(&s->bodies[0]);
    s->body_count = 1;
    s->ctx.p = p;
    s->ctx.solver = s;
    s->ctx.bodies = s->bodies;
    s->ctx.settings = &s->settings;
    *out = s;
    return DE_OK;
}

de_result de__solver_ensure(de_physics_world *p, de__solver **out) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__solver_ensure: world is NULL");
    de__solver *s = de__solver_get(p);
    if (s == NULL) {
        de__contacts *c = NULL;
        DE_TRY(de__contacts_ensure(p, &c));
        DE_TRY(solver_create(p, &s));
        de__phys_ext_set(p, DE__PHYS_EXT_SOLVER, s, &k_solver_vtbl);
        const de_result r = de__contact_solver_register(p);
        if (r != DE_OK) {
            de__phys_ext_set(p, DE__PHYS_EXT_SOLVER, NULL, NULL);
            solver_free(s);
            return r;
        }
    }
    if (out != NULL)
        *out = s;
    return DE_OK;
}

de_result de__solver_register_class(de_physics_world *p, u32 class_id,
                                    const de__constraint_class *cls, void *state,
                                    u32 max_constraints) {
    if (p == NULL || cls == NULL || class_id >= DE__CONSTRAINT_CLASS_COUNT || cls->gather == NULL ||
        cls->solve == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__solver_register_class: bad class %u",
                       class_id);
    de__solver *s = NULL;
    DE_TRY(de__solver_ensure(p, &s));
    if (s->classes[class_id].cls != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__solver_register_class: class %u exists",
                       class_id);
    DE_ASSERT(!s->active);
    u64 total = max_constraints;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k)
        total += s->classes[k].capacity;
    if (total > 0x3FFFFFFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__solver_register_class: too many constraints");
    if (total > s->constraint_capacity) {
        de__constraint *nc = (de__constraint *)DE_ALLOC(s->alloc, sizeof *nc * (size_t)total,
                                                        DE_ALIGNOF(de__constraint), DE_MEM_PHYSICS);
        if (nc == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver_register_class: %u records",
                           (u32)total);
        if (s->constraints != NULL)
            DE_FREE(s->alloc, s->constraints,
                    sizeof *s->constraints * (size_t)s->constraint_capacity);
        s->constraints = nc;
        s->constraint_capacity = (u32)total;
        s->constraint_count = 0;
    }
    de__solver_class_slot *c = &s->classes[class_id];
    c->cls = cls;
    c->state = state;
    c->capacity = max_constraints;
    c->count = 0;
    return DE_OK;
}

void de__solver_unregister_class(de_physics_world *p, u32 class_id) {
    de__solver *s = p != NULL ? de__solver_get(p) : NULL;
    if (s == NULL || class_id >= DE__CONSTRAINT_CLASS_COUNT || s->classes[class_id].cls == NULL)
        return;
    DE_ASSERT(!s->active);
    de__solver_class_slot *c = &s->classes[class_id];
    if (c->cls->destroy != NULL)
        c->cls->destroy(p, c->state);
    memset(c, 0, sizeof *c);
}

/* ================================ Records ============================================== */

u32 de__solver_slot(const de__solver *s, u32 body_index) {
    return body_index < s->body_capacity ? s->slot_of[body_index] : DE__SOLVER_STATIC_SLOT;
}

static u32 side_slot(const de__solver *s, u32 body, bool *writable) {
    *writable = false;
    if (body == DE__CONSTRAINT_NO_BODY)
        return DE__SOLVER_STATIC_SLOT;
    const u32 slot = de__solver_slot(s, body);
    *writable =
        slot != DE__SOLVER_STATIC_SLOT && (s->bodies[slot].flags & DE__SOLVER_BODY_DYNAMIC) != 0u;
    return slot;
}

de__constraint *de__solver_add_constraint(de__solver *s, u32 class_id, u32 id, u32 body_a,
                                          u32 body_b) {
    DE_ASSERT(class_id < DE__CONSTRAINT_CLASS_COUNT);
    bool wa, wb;
    const u32 sa = side_slot(s, body_a, &wa);
    const u32 sb = side_slot(s, body_b, &wb);
    if (!wa && !wb)
        return NULL;
    de__solver_class_slot *c = &s->classes[class_id];
    if (c->count >= c->capacity || s->constraint_count >= s->constraint_capacity) {
        ++s->stats.dropped;
        return NULL;
    }
    de__constraint *r = &s->constraints[s->constraint_count++];
    r->class_id = class_id;
    r->id = id;
    r->body_a = body_a;
    r->body_b = body_b;
    r->solver_a = sa;
    r->solver_b = sb;
    r->flags = (wa ? DE__CONSTRAINT_WRITE_A : 0u) | (wb ? DE__CONSTRAINT_WRITE_B : 0u);
    r->data = c->count++;
    r->color = DE__CONSTRAINT_NO_COLOR;
    return r;
}

/* ================================ Stages =============================================== */

de__solver *de__solver_begin(de_physics_world *p, f32 dt) {
    de__solver *s = NULL;
    if (de__solver_ensure(p, &s) != DE_OK)
        return NULL;
    DE_ASSERT(!s->active);
    de__solver_ctx *x = &s->ctx;
    x->bodies = s->bodies;
    x->dt = dt;
    x->substeps = p->substeps > 0 ? p->substeps : 1u;
    x->h = dt / (f32)x->substeps;
    x->inv_h = 1.0f / x->h;
    x->substep = 0;
    /* Contact stiffness: at most a quarter of the substep rate (stable for any substep count),
     * twice that against infinite mass (static or kinematic side), as in Box2D v3. */
    const f32 hertz = de__minf(s->settings.contact_hertz, 0.25f * x->inv_h);
    x->contact_soft = de__soft_make(hertz, s->settings.contact_damping_ratio, x->h);
    x->static_soft = de__soft_make(2.0f * hertz, s->settings.contact_damping_ratio, x->h);
    x->max_bias_velocity = s->settings.contact_push_out;
    x->restitution_threshold = s->settings.restitution_threshold;
    x->warm_start_scale = s->settings.warm_start ? 1.0f : 0.0f;

    s->body_count = 1;
    s->awake_count = 0;
    s->constraint_count = 0;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k)
        s->classes[k].count = 0;
    memset(&s->stats, 0, sizeof s->stats);
    s->active = true;
    return s;
}

u32 de__solver_add_awake(de__solver *s, u32 body_index) {
    DE_ASSERT(body_index < s->body_capacity);
    DE_ASSERT(s->awake_count == 0 || s->awake[s->awake_count - 1u] < body_index);
    DE_ASSERT(s->body_count <= s->body_capacity);
    const u32 slot = s->body_count++;
    s->awake[s->awake_count++] = body_index;
    s->slot_of[body_index] = slot;
    return slot;
}

/* Calls stage `fn` of every record's class, in record order. */
typedef enum sv_stage {
    SV_PREPARE,
    SV_WARM_START,
    SV_SOLVE,
    SV_RELAX,
    SV_RESTITUTION,
    SV_STORE
} sv_stage;

static void run_stage(de__solver *s, sv_stage stage) {
    const de__solver_ctx *ctx = &s->ctx;
    for (u32 i = 0; i < s->constraint_count; ++i) {
        de__constraint *c = &s->constraints[i];
        const de__solver_class_slot *k = &s->classes[c->class_id];
        const de__constraint_class *cls = k->cls;
        switch (stage) {
        case SV_PREPARE:
            if (cls->prepare != NULL)
                cls->prepare(ctx, k->state, c);
            break;
        case SV_WARM_START:
            if (cls->warm_start != NULL)
                cls->warm_start(ctx, k->state, c);
            break;
        case SV_SOLVE:
            cls->solve(ctx, k->state, c, true);
            break;
        case SV_RELAX:
            if (cls->relax != NULL)
                cls->relax(ctx, k->state, c);
            else
                cls->solve(ctx, k->state, c, false);
            break;
        case SV_RESTITUTION:
            if (cls->restitution != NULL)
                cls->restitution(ctx, k->state, c);
            break;
        case SV_STORE:
            if (cls->store != NULL)
                cls->store(ctx, k->state, c);
            break;
        }
    }
}

void de__solver_prepare(de__solver *s) {
    for (u32 k = 0; k < s->awake_count; ++k)
        de__integrate_init_body(s, s->awake[k], &s->bodies[k + 1u]);
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k) {
        const de__solver_class_slot *c = &s->classes[k];
        if (c->cls != NULL)
            c->cls->gather(s, &s->ctx, c->state);
    }
    run_stage(s, SV_PREPARE);
    s->stats.awake_bodies = s->awake_count;
    s->stats.constraints = s->constraint_count;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k)
        s->stats.class_count[k] = s->classes[k].count;
    s->stats.contact_points = de__contact_solver_points(s->p);
}

void de__solver_integrate_velocities(de__solver *s) {
    de__integrate_velocities(s, 1u, s->body_count);
}

void de__solver_warm_start(de__solver *s) {
    run_stage(s, SV_WARM_START);
}

void de__solver_solve(de__solver *s) {
    run_stage(s, SV_SOLVE);
}

void de__solver_integrate_positions(de__solver *s) {
    de__integrate_positions(s, 1u, s->body_count);
}

void de__solver_relax(de__solver *s) {
    run_stage(s, SV_RELAX);
    ++s->ctx.substep;
}

void de__solver_restitution(de__solver *s) {
    run_stage(s, SV_RESTITUTION);
}

void de__solver_store(de__solver *s) {
    run_stage(s, SV_STORE);
}

void de__solver_finalize(de__solver *s) {
    de__integrate_finalize(s, 1u, s->body_count);
    for (u32 k = 0; k < s->awake_count; ++k)
        s->slot_of[s->awake[k]] = DE__SOLVER_STATIC_SLOT;
    /* Proxies follow the moved bodies now, so queries between steps find them (the pair list of
     * the next update does not depend on when proxies move). */
    de__broadphase_sync(s->p);
    s->active = false;
}

void de__solver_stats_get(const de_physics_world *p, de__solver_stats *out) {
    const de__solver *s = p != NULL ? de__solver_get(p) : NULL;
    if (s == NULL)
        memset(out, 0, sizeof *out);
    else
        *out = s->stats;
}
