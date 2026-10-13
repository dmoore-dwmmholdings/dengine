/*
 * solver.c: the solver state, constraint class registration and the stage dispatchers of the
 * soft-step solver (DENG-217; DENG-218: islands, sleeping, graph-colored parallel batches; see
 * solver_internal.h, islands.h and graph_color.h).
 *
 * Records are added in a stable order (classes in ascending id, each in its own stable order) and
 * colored in that order once per step (graph_color.c); every constraint stage runs the colors in
 * order and splits each color into fixed chunks on p->tasks (solver_parallel.c). The per-body
 * kernels run in fixed ranges of slots (ascending body index). Results do not depend on the worker
 * count or on tasks == NULL (SPEC §13.2, §5.7 rule 5).
 */
#include "physics/solver/solver_internal.h"

#include "physics/broadphase/broadphase.h"
#include "physics/contact/manifold.h"
#include "physics/islands.h"
#include "physics/solver/graph_color.h"

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

/* The record array and the coloring's record order share one allocation. */
static size_t records_bytes(u32 capacity) {
    return sv_align_up(sizeof(de__constraint) * (size_t)capacity) + sizeof(u32) * (size_t)capacity;
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
        DE_FREE(a, s->constraints, records_bytes(s->constraint_capacity));
    if (s->block != NULL)
        DE_FREE(a, s->block, s->block_size);
    DE_FREE(a, s, sizeof *s);
}

static void ext_destroy(de_physics_world *p, void *state) {
    (void)p;
    solver_free((de__solver *)state);
}

/* Sleep state follows the body pool (index reuse, destroyed sleeping bodies). */
static void ext_body_added(de_physics_world *p, void *state, u32 index) {
    (void)p;
    de__islands_body_added(((de__solver *)state)->islands, index);
}

static void ext_body_removed(de_physics_world *p, void *state, u32 index) {
    (void)p;
    de__islands_body_removed(((de__solver *)state)->islands, index);
}

static const de__phys_ext_vtbl k_solver_vtbl = {ext_destroy, ext_body_added, ext_body_removed};

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
    size_t off_islands = sv_align_up(off_awake + sizeof(u32) * (size_t)cap);
    size_t off_coloring = sv_align_up(off_islands + de__islands_bytes(cap));
    const size_t size = sv_align_up(off_coloring + de__coloring_bytes(cap + 1u));
    u8 *block = (u8 *)DE_ALLOC(a, size, SV_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver_ensure: %u solver bodies", cap);
    }
    memset(block, 0, size);
    s->block = block;
    s->block_size = size;
    s->bodies = (de__solver_body *)(void *)(block + off_bodies);
    s->slot_of = (u32 *)(void *)(block + off_slot);
    s->awake = (u32 *)(void *)(block + off_awake);
    s->islands = de__islands_init(block + off_islands, cap);
    s->coloring = de__coloring_init(block + off_coloring, cap + 1u);
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
        /* Records, then the coloring's record order (graph_color.h). */
        u8 *mem = (u8 *)DE_ALLOC(s->alloc, records_bytes((u32)total), SV_ALIGN, DE_MEM_PHYSICS);
        if (mem == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__solver_register_class: %u records",
                           (u32)total);
        if (s->constraints != NULL)
            DE_FREE(s->alloc, s->constraints, records_bytes(s->constraint_capacity));
        s->constraints = (de__constraint *)(void *)mem;
        s->constraint_capacity = (u32)total;
        s->constraint_count = 0;
        de__coloring_bind_order(
            s->coloring, (u32 *)(void *)(mem + sv_align_up(sizeof(de__constraint) * (size_t)total)),
            (u32)total);
    }
    de__solver_class_slot *c = &s->classes[class_id];
    c->cls = cls;
    c->state = state;
    c->capacity = max_constraints;
    c->count = 0;
    c->iterations = 1;
    return DE_OK;
}

de_result de__solver_set_class_iterations(de_physics_world *p, u32 class_id, u32 iterations) {
    de__solver *s = p != NULL ? de__solver_get(p) : NULL;
    if (s == NULL || class_id >= DE__CONSTRAINT_CLASS_COUNT || s->classes[class_id].cls == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__solver_set_class_iterations: class %u is not registered", class_id);
    DE_ASSERT(!s->active);
    s->classes[class_id].iterations =
        iterations < 1u
            ? 1u
            : (iterations > DE__SOLVER_MAX_CLASS_ITERATIONS ? DE__SOLVER_MAX_CLASS_ITERATIONS
                                                            : iterations);
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

/* An awake side that may wake a sleeping one: a writable (awake dynamic) body, or a kinematic body
 * that moves. */
static bool side_active(const de__solver *s, u32 slot, bool writable) {
    if (writable)
        return true;
    if (slot == DE__SOLVER_STATIC_SLOT)
        return false;
    const de__solver_body *b = &s->bodies[slot];
    return (b->flags & DE__SOLVER_BODY_KINEMATIC) != 0u &&
           (b->v.x != 0.0f || b->v.y != 0.0f || b->v.z != 0.0f || b->w.x != 0.0f ||
            b->w.y != 0.0f || b->w.z != 0.0f);
}

static bool side_sleeping(const de__islands *is, u32 body) {
    return body != DE__CONSTRAINT_NO_BODY && body < is->capacity && is->sleeping[body] != 0u;
}

de__constraint *de__solver_add_constraint(de__solver *s, u32 class_id, u32 id, u32 body_a,
                                          u32 body_b) {
    DE_ASSERT(class_id < DE__CONSTRAINT_CLASS_COUNT);
    bool wa, wb;
    const u32 sa = side_slot(s, body_a, &wa);
    const u32 sb = side_slot(s, body_b, &wb);
    de__islands *is = s->islands;
    if (is->stats.sleeping_bodies != 0u) {
        /* A constraint between a sleeping body and an active one wakes the sleeper's island; the
         * solver gathers again (de__solver_prepare). */
        const bool za = side_sleeping(is, body_a), zb = side_sleeping(is, body_b);
        if (za && !zb && side_active(s, sb, wb))
            de__islands_request_wake(is, body_a);
        else if (zb && !za && side_active(s, sa, wa))
            de__islands_request_wake(is, body_b);
    }
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

/* Back to an empty awake set (a constraint woke an island: collect and gather again). */
static void reset_awake(de__solver *s) {
    for (u32 k = 0; k < s->awake_count; ++k)
        s->slot_of[s->awake[k]] = DE__SOLVER_STATIC_SLOT;
    s->body_count = 1;
    s->awake_count = 0;
    s->constraint_count = 0;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k)
        s->classes[k].count = 0;
    s->stats.dropped = 0;
}

static void init_bodies(de__solver *s, u32 begin, u32 end) {
    for (u32 k = begin; k < end; ++k)
        de__integrate_init_body(s, s->awake[k - 1u], &s->bodies[k]);
}

void de__solver_prepare(de__solver *s) {
    for (;;) {
        de__solver_for_bodies(s, init_bodies);
        for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k) {
            const de__solver_class_slot *c = &s->classes[k];
            if (c->cls != NULL)
                c->cls->gather(s, &s->ctx, c->state);
        }
        if (!de__islands_wake_pending(s->islands))
            break;
        /* Each pass wakes at least one island, so this ends. */
        reset_awake(s);
        ++s->islands->stats.regathers;
        de__islands_collect_awake(s->p, s);
    }
    de__islands_build(s);
    de__graph_color(s);
    de__solver_run_stage(s, DE__STAGE_PREPARE);
    s->stats.awake_bodies = s->awake_count;
    s->stats.constraints = s->constraint_count;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k)
        s->stats.class_count[k] = s->classes[k].count;
    s->stats.contact_points = de__contact_solver_points(s->p);
}

void de__solver_integrate_velocities(de__solver *s) {
    de__solver_for_bodies(s, de__integrate_velocities);
}

void de__solver_warm_start(de__solver *s) {
    de__solver_run_stage(s, DE__STAGE_WARM_START);
}

/* Most iterations of a class with records this step (1 when none asks for more). */
static u32 max_class_iterations(const de__solver *s) {
    u32 n = 1;
    for (u32 k = 0; k < DE__CONSTRAINT_CLASS_COUNT; ++k) {
        const de__solver_class_slot *c = &s->classes[k];
        if (c->cls != NULL && c->count > 0u && c->iterations > n)
            n = c->iterations;
    }
    return n;
}

/* DENG-398: the extra passes of classes with more than one iteration, then the full pass over
 * every record (so later classes, contacts, keep the last word). */
static void run_iterated(de__solver *s, de__solver_stage stage) {
    const u32 n = max_class_iterations(s);
    for (u32 pass = 1; pass < n; ++pass) {
        s->ctx.pass = pass; /* set before the workers start; read-only for them */
        de__solver_run_stage_extra(s, stage, pass);
    }
    s->ctx.pass = 0;
    de__solver_run_stage(s, stage);
}

void de__solver_solve(de__solver *s) {
    run_iterated(s, DE__STAGE_SOLVE);
}

void de__solver_integrate_positions(de__solver *s) {
    de__solver_for_bodies(s, de__integrate_positions);
}

void de__solver_relax(de__solver *s) {
    run_iterated(s, DE__STAGE_RELAX);
    ++s->ctx.substep;
}

void de__solver_restitution(de__solver *s) {
    de__solver_run_stage(s, DE__STAGE_RESTITUTION);
}

void de__solver_store(de__solver *s) {
    de__solver_run_stage(s, DE__STAGE_STORE);
}

void de__solver_finalize(de__solver *s) {
    de__solver_for_bodies(s, de__integrate_finalize);
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
