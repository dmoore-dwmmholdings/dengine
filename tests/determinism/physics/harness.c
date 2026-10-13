/* harness.c: the physics per-tick checksum harness (DENG-136). See harness.h.
 *
 * Also the test executable's main(): it takes the harness's own option --update-golden and passes
 * everything else to the de_test runner (de_test.c is compiled into the executable with
 * DE_TEST_NO_MAIN). Built with DE_DETERMINISTIC_FLAGS. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "harness.h"

#include "physics/physics_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PD_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#ifndef DE_DET_OUT_DIR
#error "DE_DET_OUT_DIR must be defined by tests/determinism/physics/CMakeLists.txt"
#endif
#ifndef DE_PHYS_DET_GOLDEN_DIR
#error "DE_PHYS_DET_GOLDEN_DIR must be defined by tests/determinism/physics/CMakeLists.txt"
#endif

/* Golden files may only be rewritten by a Debug or Dev build (never by the Release CI jobs). */
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define PD_CAN_UPDATE 1
#else
#define PD_CAN_UPDATE 0
#endif

static bool g_update_flag; /* --update-golden */

/* ================================ Little-endian hashing ================================ */

typedef struct pd_buf {
    u8 b[512];
    u32 n;
} pd_buf;

static void pd_u32(pd_buf *c, u32 v) {
    DE_ASSERT(c->n + 4 <= sizeof c->b);
    for (u32 i = 0; i < 4; ++i)
        c->b[c->n++] = (u8)(v >> (8u * i));
}
static void pd_u64(pd_buf *c, u64 v) {
    DE_ASSERT(c->n + 8 <= sizeof c->b);
    for (u32 i = 0; i < 8; ++i)
        c->b[c->n++] = (u8)(v >> (8u * i));
}
static void pd_f32(pd_buf *c, f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    pd_u32(c, u);
}
static void pd_f64(pd_buf *c, f64 v) {
    u64 u;
    memcpy(&u, &v, sizeof u);
    pd_u64(c, u);
}
static void pd_v3(pd_buf *c, de_vec3 v) {
    pd_f32(c, v.x);
    pd_f32(c, v.y);
    pd_f32(c, v.z);
}
static void pd_m3(pd_buf *c, const de_mat3 *m) {
    for (u32 i = 0; i < 9; ++i)
        pd_f32(c, m->m[i]);
}
/* Hashes the buffer into *h (running seed) and empties it. */
static void pd_flush(pd_buf *c, u64 *h) {
    *h = de_hash64(c->b, c->n, *h);
    c->n = 0;
}

/* The field-group hashes of the state after a tick. */
static void pd_fields(const de_physics_world *p, u64 probe, u64 ecs, u64 *out) {
    const de__body_pool *b = &p->bodies;
    pd_buf c;
    c.n = 0;
    for (u32 g = 0; g < DE_PHYS_DET_FIELD_COUNT; ++g)
        out[g] = 0;
    pd_u64(&c, p->tick);
    pd_v3(&c, p->gravity);
    pd_u32(&c, p->substeps);
    pd_u32(&c, b->live_count);
    pd_u32(&c, b->high_water);
    pd_flush(&c, &out[DE_PHYS_DET_FIELD_WORLD]);
    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i])
            continue;
        pd_u32(&c, i);
        pd_f64(&c, b->position[i].x);
        pd_f64(&c, b->position[i].y);
        pd_f64(&c, b->position[i].z);
        pd_flush(&c, &out[DE_PHYS_DET_FIELD_POSITION]);
        pd_u32(&c, i);
        pd_f32(&c, b->rotation[i].x);
        pd_f32(&c, b->rotation[i].y);
        pd_f32(&c, b->rotation[i].z);
        pd_f32(&c, b->rotation[i].w);
        pd_flush(&c, &out[DE_PHYS_DET_FIELD_ROTATION]);
        pd_u32(&c, i);
        pd_v3(&c, b->linear_velocity[i]);
        pd_v3(&c, b->angular_velocity[i]);
        pd_flush(&c, &out[DE_PHYS_DET_FIELD_VELOCITY]);
        pd_u32(&c, i);
        pd_f32(&c, b->mass[i]);
        pd_f32(&c, b->inv_mass[i]);
        pd_v3(&c, b->local_com[i]);
        pd_m3(&c, &b->local_inertia[i]);
        pd_m3(&c, &b->local_inv_inertia[i]);
        pd_flush(&c, &out[DE_PHYS_DET_FIELD_MASS]);
        pd_u32(&c, i);
        pd_v3(&c, b->world_aabb[i].min);
        pd_v3(&c, b->world_aabb[i].max);
        pd_flush(&c, &out[DE_PHYS_DET_FIELD_AABB]);
        pd_u32(&c, i);
        pd_u32(&c, b->generation[i]);
        pd_u32(&c, b->type[i]);
        pd_u32(&c, b->flags[i]);
        pd_u32(&c, b->shape[i].id);
        pd_u32(&c, b->filter[i].layer);
        pd_u32(&c, b->filter[i].mask);
        pd_u32(&c, b->filter[i].group);
        pd_f32(&c, b->material[i].friction);
        pd_f32(&c, b->material[i].restitution);
        pd_f32(&c, b->material[i].density);
        pd_f32(&c, b->linear_damping[i]);
        pd_f32(&c, b->angular_damping[i]);
        pd_f32(&c, b->gravity_scale[i]);
        pd_u64(&c, b->owner[i].bits);
        pd_v3(&c, b->friction_axis[i]); /* DENG-217 */
        pd_f32(&c, b->friction_secondary[i]);
        pd_flush(&c, &out[DE_PHYS_DET_FIELD_BODY]);
    }
    out[DE_PHYS_DET_FIELD_PROBE] = probe;
    out[DE_PHYS_DET_FIELD_ECS] = ecs;
}

static const char *const g_field_names[DE_PHYS_DET_FIELD_COUNT] = {
    "world", "position", "rotation", "velocity", "mass", "aabb", "body", "probe", "ecs"};

const char *de_phys_det_field_name(u32 field) {
    return field < (u32)DE_PHYS_DET_FIELD_COUNT ? g_field_names[field] : "?";
}

/* ================================ Session ============================================== */

/* de_det callbacks. The de_det state block holds one pointer: the physics session. */
static de_result pd_det_setup(de_world *w, u64 seed, void *state) {
    (void)w;
    (void)seed;
    (void)state; /* the physics world is created after de_det_session_begin (it needs the
                    session's task system) */
    return DE_OK;
}

static void pd_det_tick(de_world *w, u64 tick_index, void *state) {
    (void)w;
    (void)tick_index;
    de_phys_det_session *s = *(de_phys_det_session **)state;
    if (s->scenario->script != NULL)
        s->scenario->script(&s->ctx, s->tick);
    if (s->options.inject != NULL)
        s->options.inject(&s->ctx, s->tick, s->options.inject_user);
}

static void pd_step_system(de_system_ctx *ctx) {
    de_phys_det_session *s = (de_phys_det_session *)ctx->user;
    de_physics_step(s->ctx.physics, s->ctx.dt);
}

de_result de_phys_det_begin(de_phys_det_session *s, const de_phys_det_scenario *scenario,
                            const de_phys_det_options *options) {
    if (s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_phys_det_begin: NULL session");
    memset(s, 0, sizeof *s);
    if (scenario == NULL || scenario->name == NULL || scenario->setup == NULL ||
        scenario->ticks == 0 || scenario->ticks > DE_PHYS_DET_MAX_TICKS ||
        !(scenario->dt >= 0.0f && scenario->dt < 1.0f))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_phys_det_begin: incomplete scenario");

    s->scenario = scenario;
    if (options != NULL)
        s->options = *options;
    s->det_scenario.name = scenario->name;
    s->det_scenario.setup = pd_det_setup;
    s->det_scenario.tick = pd_det_tick;
    s->det_scenario.ticks = scenario->ticks;
    s->det_scenario.state_size = (u32)sizeof(de_phys_det_session *);
    s->det_scenario.dt = scenario->dt;
    const u64 seed = scenario->seed != 0 ? scenario->seed : DE_PHYS_DET_DEFAULT_SEED;

    de_result r = de_det_session_begin(&s->det, &s->det_scenario, seed, s->options.worker_count);
    if (r != DE_OK) {
        memset(s, 0, sizeof *s);
        return r;
    }
    *(de_phys_det_session **)s->det.state = s;

    s->ctx.world = s->det.world;
    s->ctx.seed = seed;
    s->ctx.worker_count = s->options.worker_count;
    s->ctx.dt = s->det.dt;

    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.tasks = s->det.tasks;
    pd.max_bodies =
        scenario->max_bodies != 0 ? scenario->max_bodies : DE_PHYS_DET_DEFAULT_MAX_BODIES;
    pd.deterministic = true;
    r = de_physics_create(&pd, &s->ctx.physics);
    if (r != DE_OK)
        goto fail;
    r = de_physics_attach(s->ctx.physics, s->det.world, s->det.types);
    if (r != DE_OK)
        goto fail;

    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "physics.det_step";
    sd.schedule = DE_SCHEDULE_FIXED_UPDATE;
    sd.fn = pd_step_system;
    sd.user = s;
    sd.flags = DE_SYSTEM_EXCLUSIVE | DE_SYSTEM_DETERMINISTIC;
    if (!DE_HANDLE_VALID(de_system_add(s->det.world, &sd))) {
        r = DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "de_phys_det_begin: step system");
        goto fail;
    }

    if (scenario->state_size > 0) {
        s->state = DE_ALLOC(de_heap_allocator(), scenario->state_size, 16, DE_MEM_GENERAL);
        if (s->state == NULL) {
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_phys_det_begin: scenario state");
            goto fail;
        }
        memset(s->state, 0, scenario->state_size);
    }
    s->ctx.state = s->state;
    r = scenario->setup(&s->ctx);
    if (r != DE_OK)
        goto fail;
    return DE_OK;

fail:
    de_phys_det_end(s);
    return r;
}

de_result de_phys_det_step(de_phys_det_session *s, u64 *out_checksum, u64 *out_fields) {
    if (s == NULL || s->ctx.physics == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_phys_det_step: no session");
    u64 ecs = 0;
    const de_result r = de_det_session_step(&s->det, &ecs);
    if (r != DE_OK)
        return r;
    const u64 probe = s->scenario->probe != NULL ? s->scenario->probe(&s->ctx, s->tick) : 0u;
    const u64 phys = de_physics_checksum(s->ctx.physics);
    if (out_checksum != NULL) {
        pd_buf c;
        c.n = 0;
        pd_u64(&c, phys);
        pd_u64(&c, probe);
        pd_u64(&c, ecs);
        *out_checksum = de_hash64(c.b, c.n, 0);
    }
    if (out_fields != NULL)
        pd_fields(s->ctx.physics, probe, ecs, out_fields);
    s->last_probe = probe;
    s->last_ecs = ecs;
    ++s->tick;
    return DE_OK;
}

void de_phys_det_end(de_phys_det_session *s) {
    if (s == NULL || s->scenario == NULL)
        return;
    if (s->ctx.physics != NULL && s->scenario->teardown != NULL)
        s->scenario->teardown(&s->ctx);
    /* The ECS world (and its DePhysicsWorld resource) goes first: it points at the physics
     * world. */
    de_physics_world *p = s->ctx.physics;
    de_det_session_end(&s->det);
    de_physics_destroy(p);
    if (s->state != NULL)
        DE_FREE(de_heap_allocator(), s->state, s->scenario->state_size);
    memset(s, 0, sizeof *s);
}

de_result de_phys_det_run(const de_phys_det_scenario *scenario, const de_phys_det_options *options,
                          u64 *out_checksums, u64 *out_fields) {
    if (scenario == NULL || out_checksums == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_phys_det_run: NULL argument");
    de_phys_det_session s;
    de_result r = de_phys_det_begin(&s, scenario, options);
    for (u32 t = 0; t < scenario->ticks && r == DE_OK; ++t) {
        r = de_phys_det_step(&s, &out_checksums[t],
                             out_fields != NULL ? &out_fields[(size_t)t * DE_PHYS_DET_FIELD_COUNT]
                                                : NULL);
    }
    de_phys_det_end(&s);
    return r;
}

/* ================================ Field-level diff ===================================== */

/* One body as a flat record, every pool field (the order of de_physics_checksum). */
typedef struct pd_body {
    u32 alive, generation, type, flags;
    f64 position[3];
    f32 rotation[4];
    f32 linear_velocity[3];
    f32 angular_velocity[3];
    f32 mass, inv_mass;
    f32 local_com[3];
    f32 local_inertia[9];
    f32 local_inv_inertia[9];
    f32 linear_damping, angular_damping, gravity_scale;
    u32 shape;
    u32 filter[3];   /* layer, mask, group */
    f32 material[3]; /* friction, restitution, density */
    u64 owner;
    f32 aabb_min[3], aabb_max[3];
    f32 friction_aniso[4]; /* DENG-217: friction axis x, y, z, friction_secondary */
} pd_body;

/* The world-level values of a capture. */
typedef struct pd_world {
    u64 tick;
    f32 gravity[3];
    u32 substeps, live_count, high_water;
    u64 probe, ecs, physics_checksum;
} pd_world;

typedef enum pd_kind { PD_U32, PD_U64, PD_F32, PD_F64 } pd_kind;
typedef struct pd_field_desc {
    const char *name;
    size_t offset;
    pd_kind kind;
    u32 count;
} pd_field_desc;

#define PD_BF(name, kind, count) {#name, offsetof(pd_body, name), kind, count}
static const pd_field_desc g_body_fields[] = {
    PD_BF(alive, PD_U32, 1),
    PD_BF(generation, PD_U32, 1),
    PD_BF(type, PD_U32, 1),
    PD_BF(flags, PD_U32, 1),
    PD_BF(position, PD_F64, 3),
    PD_BF(rotation, PD_F32, 4),
    PD_BF(linear_velocity, PD_F32, 3),
    PD_BF(angular_velocity, PD_F32, 3),
    PD_BF(mass, PD_F32, 1),
    PD_BF(inv_mass, PD_F32, 1),
    PD_BF(local_com, PD_F32, 3),
    PD_BF(local_inertia, PD_F32, 9),
    PD_BF(local_inv_inertia, PD_F32, 9),
    PD_BF(linear_damping, PD_F32, 1),
    PD_BF(angular_damping, PD_F32, 1),
    PD_BF(gravity_scale, PD_F32, 1),
    PD_BF(shape, PD_U32, 1),
    PD_BF(filter, PD_U32, 3),
    PD_BF(material, PD_F32, 3),
    PD_BF(owner, PD_U64, 1),
    PD_BF(aabb_min, PD_F32, 3),
    PD_BF(aabb_max, PD_F32, 3),
    PD_BF(friction_aniso, PD_F32, 4),
};
#undef PD_BF
#define PD_WF(name, kind, count) {#name, offsetof(pd_world, name), kind, count}
static const pd_field_desc g_world_fields[] = {
    PD_WF(tick, PD_U64, 1),       PD_WF(gravity, PD_F32, 3),          PD_WF(substeps, PD_U32, 1),
    PD_WF(live_count, PD_U32, 1), PD_WF(high_water, PD_U32, 1),       PD_WF(probe, PD_U64, 1),
    PD_WF(ecs, PD_U64, 1),        PD_WF(physics_checksum, PD_U64, 1),
};
#undef PD_WF

typedef struct pd_capture {
    pd_world world;
    pd_body *bodies; /* [count], indexed by body index */
    u32 count;
} pd_capture;

static void pd_copy3(f32 *d, de_vec3 v) {
    d[0] = v.x;
    d[1] = v.y;
    d[2] = v.z;
}

/* Runs to the end of tick `tick` and captures the state. */
static de_result pd_capture_at(const de_phys_det_scenario *sc, const de_phys_det_options *o,
                               u64 tick, pd_capture *out) {
    memset(out, 0, sizeof *out);
    de_phys_det_session s;
    de_result r = de_phys_det_begin(&s, sc, o);
    u64 ck = 0;
    for (u64 t = 0; t <= tick && r == DE_OK; ++t)
        r = de_phys_det_step(&s, &ck, NULL);
    if (r == DE_OK) {
        const de_physics_world *p = s.ctx.physics;
        const de__body_pool *b = &p->bodies;
        out->world.tick = p->tick;
        pd_copy3(out->world.gravity, p->gravity);
        out->world.substeps = p->substeps;
        out->world.live_count = b->live_count;
        out->world.high_water = b->high_water;
        out->world.probe = s.last_probe;
        out->world.ecs = s.last_ecs;
        out->world.physics_checksum = de_physics_checksum(p);
        out->count = b->high_water;
        if (out->count > 0) {
            out->bodies = (pd_body *)DE_ALLOC(de_heap_allocator(), sizeof(pd_body) * out->count, 16,
                                              DE_MEM_GENERAL);
            if (out->bodies == NULL) {
                r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "field diff: capture");
                out->count = 0;
            }
        }
        for (u32 i = 0; i < out->count; ++i) {
            pd_body *d = &out->bodies[i];
            memset(d, 0, sizeof *d);
            d->alive = b->alive[i];
            if (!d->alive)
                continue;
            d->generation = b->generation[i];
            d->type = b->type[i];
            d->flags = b->flags[i];
            d->position[0] = b->position[i].x;
            d->position[1] = b->position[i].y;
            d->position[2] = b->position[i].z;
            d->rotation[0] = b->rotation[i].x;
            d->rotation[1] = b->rotation[i].y;
            d->rotation[2] = b->rotation[i].z;
            d->rotation[3] = b->rotation[i].w;
            pd_copy3(d->linear_velocity, b->linear_velocity[i]);
            pd_copy3(d->angular_velocity, b->angular_velocity[i]);
            d->mass = b->mass[i];
            d->inv_mass = b->inv_mass[i];
            pd_copy3(d->local_com, b->local_com[i]);
            memcpy(d->local_inertia, b->local_inertia[i].m, sizeof d->local_inertia);
            memcpy(d->local_inv_inertia, b->local_inv_inertia[i].m, sizeof d->local_inv_inertia);
            d->linear_damping = b->linear_damping[i];
            d->angular_damping = b->angular_damping[i];
            d->gravity_scale = b->gravity_scale[i];
            d->shape = b->shape[i].id;
            d->filter[0] = b->filter[i].layer;
            d->filter[1] = b->filter[i].mask;
            d->filter[2] = b->filter[i].group;
            d->material[0] = b->material[i].friction;
            d->material[1] = b->material[i].restitution;
            d->material[2] = b->material[i].density;
            d->owner = b->owner[i].bits;
            pd_copy3(d->aabb_min, b->world_aabb[i].min);
            pd_copy3(d->aabb_max, b->world_aabb[i].max);
            pd_copy3(d->friction_aniso, b->friction_axis[i]);
            d->friction_aniso[3] = b->friction_secondary[i];
        }
    }
    de_phys_det_end(&s);
    return r;
}

static void pd_capture_free(pd_capture *c) {
    if (c->bodies != NULL)
        DE_FREE(de_heap_allocator(), c->bodies, sizeof(pd_body) * c->count);
    memset(c, 0, sizeof *c);
}

static void pd_component_name(char *buf, size_t n, const char *name, u32 count, u32 k) {
    static const char xyzw[4] = {'x', 'y', 'z', 'w'};
    if (count == 1)
        snprintf(buf, n, "%s", name);
    else if (count <= 4)
        snprintf(buf, n, "%s.%c", name, xyzw[k]);
    else
        snprintf(buf, n, "%s[%u]", name, k);
}

/* Prints one value (hex bits and decimal). */
static void pd_value(char *buf, size_t n, pd_kind kind, const u8 *p) {
    switch (kind) {
    case PD_U32: {
        u32 v;
        memcpy(&v, p, sizeof v);
        snprintf(buf, n, "%u (0x%08x)", v, v);
        break;
    }
    case PD_U64: {
        u64 v;
        memcpy(&v, p, sizeof v);
        snprintf(buf, n, "0x%016llx", (unsigned long long)v);
        break;
    }
    case PD_F32: {
        f32 v;
        u32 u;
        memcpy(&v, p, sizeof v);
        memcpy(&u, p, sizeof u);
        snprintf(buf, n, "%.9g (0x%08x)", (double)v, u);
        break;
    }
    case PD_F64: {
        f64 v;
        u64 u;
        memcpy(&v, p, sizeof v);
        memcpy(&u, p, sizeof u);
        snprintf(buf, n, "%.17g (0x%016llx)", v, (unsigned long long)u);
        break;
    }
    }
}

static size_t pd_kind_size(pd_kind k) {
    return (k == PD_U64 || k == PD_F64) ? 8u : 4u;
}

#define PD_DIFF_PRINT_MAX 16u

/* Compares `count` fields of two records; prints and counts differences. */
static void pd_diff_record(const pd_field_desc *fields, u32 nfields, const u8 *a, const u8 *b,
                           u32 body, de_phys_det_field_diff *d) {
    for (u32 f = 0; f < nfields; ++f) {
        const size_t sz = pd_kind_size(fields[f].kind);
        for (u32 k = 0; k < fields[f].count; ++k) {
            const size_t off = fields[f].offset + sz * k;
            if (memcmp(a + off, b + off, sz) == 0)
                continue;
            char name[48];
            pd_component_name(name, sizeof name, fields[f].name, fields[f].count, k);
            if (d->differing == 0) {
                d->body = body;
                snprintf(d->field, sizeof d->field, "%s", name);
            }
            if (d->differing < PD_DIFF_PRINT_MAX) {
                char va[64], vb[64];
                pd_value(va, sizeof va, fields[f].kind, a + off);
                pd_value(vb, sizeof vb, fields[f].kind, b + off);
                if (body == 0xFFFFFFFFu)
                    fprintf(stderr, "  world %-20s %s  vs  %s\n", name, va, vb);
                else
                    fprintf(stderr, "  body %4u %-20s %s  vs  %s\n", body, name, va, vb);
            }
            ++d->differing;
        }
    }
}

u32 de_phys_det_diff_at(const char *what, const de_phys_det_scenario *scenario,
                        const de_phys_det_options *a, const de_phys_det_options *b, u64 tick,
                        de_phys_det_field_diff *out) {
    de_phys_det_field_diff d;
    memset(&d, 0, sizeof d);
    d.body = 0xFFFFFFFFu;
    pd_capture ca, cb;
    const de_result ra = pd_capture_at(scenario, a, tick, &ca);
    const de_result rb = pd_capture_at(scenario, b, tick, &cb);
    fprintf(stderr, "determinism: %s: field-level diff of %s after tick %llu:\n",
            what != NULL ? what : "runs", scenario->name, (unsigned long long)tick);
    if (ra != DE_OK || rb != DE_OK) {
        fprintf(stderr, "  (a run failed: %d / %d)\n", (int)ra, (int)rb);
        d.differing = 1;
        snprintf(d.field, sizeof d.field, "run failed");
    } else {
        /* Bodies first: a body field is the cause; the world checksum and counts follow. */
        const u32 n = ca.count > cb.count ? ca.count : cb.count;
        static const pd_body dead = {0}; /* a slot one side never used */
        for (u32 i = 0; i < n; ++i) {
            const pd_body *x = i < ca.count ? &ca.bodies[i] : &dead;
            const pd_body *y = i < cb.count ? &cb.bodies[i] : &dead;
            pd_diff_record(g_body_fields, (u32)PD_COUNT(g_body_fields), (const u8 *)x,
                           (const u8 *)y, i, &d);
        }
        pd_diff_record(g_world_fields, (u32)PD_COUNT(g_world_fields), (const u8 *)&ca.world,
                       (const u8 *)&cb.world, 0xFFFFFFFFu, &d);
        if (d.differing > PD_DIFF_PRINT_MAX)
            fprintf(stderr, "  ... %u differing fields in total\n", d.differing);
        else if (d.differing == 0)
            fprintf(stderr, "  (no difference in the physics state after this tick)\n");
    }
    pd_capture_free(&ca);
    pd_capture_free(&cb);
    if (out != NULL)
        *out = d;
    return d.differing;
}

/* ================================ Files ================================================ */

static const char *pd_out_dir(void) {
    const char *env = getenv("DE_DET_OUT_DIR");
    return (env != NULL && env[0] != '\0') ? env : DE_DET_OUT_DIR;
}

bool de_phys_det_update_mode(void) {
    const char *env = getenv("DE_DET_UPDATE_GOLDEN");
    return g_update_flag || (env != NULL && env[0] == '1');
}

static u64 pd_seed(const de_phys_det_scenario *sc) {
    return sc->seed != 0 ? sc->seed : DE_PHYS_DET_DEFAULT_SEED;
}

/* `<scenario>.checksums`: the de_det format (de_det_read_checksums reads it), with a header
 * that says what the values are. */
static de_result pd_write_checksums(const char *path, const de_phys_det_scenario *sc, const u64 *v,
                                    u32 count) {
    FILE *f = fopen(path, "wb"); /* binary: LF line ends on every platform */
    if (f == NULL)
        return DE_FAIL(DE_ERR_IO, "cannot open '%s'", path);
    bool ok = fprintf(f,
                      "# de_phys_det v1 scenario=%s seed=0x%llx ticks=%u: per-tick checksum "
                      "(de_physics_checksum, probe, de_world_checksum), one per line\n",
                      sc->name, (unsigned long long)pd_seed(sc), count) > 0;
    for (u32 t = 0; ok && t < count; ++t)
        ok = fprintf(f, "%016llx\n", (unsigned long long)v[t]) > 0;
    ok = (fclose(f) == 0) && ok;
    return ok ? DE_OK : DE_FAIL(DE_ERR_IO, "cannot write '%s'", path);
}

/* `<scenario>.fields`: one line per tick, "<tick> <group hash>...". */
static de_result pd_write_fields(const char *path, const de_phys_det_scenario *sc,
                                 const u64 *fields, u32 count) {
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return DE_FAIL(DE_ERR_IO, "cannot open '%s'", path);
    bool ok = fprintf(f, "# de_phys_det fields v1 scenario=%s seed=0x%llx ticks=%u groups=tick",
                      sc->name, (unsigned long long)pd_seed(sc), count) > 0;
    for (u32 g = 0; ok && g < (u32)DE_PHYS_DET_FIELD_COUNT; ++g)
        ok = fprintf(f, ",%s", g_field_names[g]) > 0;
    ok = ok && fputc('\n', f) != EOF;
    for (u32 t = 0; ok && t < count; ++t) {
        ok = fprintf(f, "%u", t) > 0;
        for (u32 g = 0; ok && g < (u32)DE_PHYS_DET_FIELD_COUNT; ++g) {
            ok = fprintf(f, " %016llx",
                         (unsigned long long)fields[(size_t)t * DE_PHYS_DET_FIELD_COUNT + g]) > 0;
        }
        ok = ok && fputc('\n', f) != EOF;
    }
    ok = (fclose(f) == 0) && ok;
    return ok ? DE_OK : DE_FAIL(DE_ERR_IO, "cannot write '%s'", path);
}

/* ================================ The scenario check =================================== */

static u64 *pd_alloc_u64(size_t n) {
    return (u64 *)DE_ALLOC(de_heap_allocator(), sizeof(u64) * n, 16, DE_MEM_GENERAL);
}
static void pd_free_u64(u64 *p, size_t n) {
    if (p != NULL)
        DE_FREE(de_heap_allocator(), p, sizeof(u64) * n);
}

static void pd_print_error(const char *what, de_result r) {
    const de_error *e = de_error_last();
    fprintf(stderr, "%s failed (%d): %s\n", what, (int)r, e != NULL ? e->message : "(no message)");
}

void de_phys_det_check(const de_phys_det_scenario *sc, u32 workers) {
    ASSERT_TRUE(sc != NULL);
    const u32 ticks = sc->ticks;
    ASSERT_TRUE(ticks > 0 && ticks <= DE_PHYS_DET_MAX_TICKS);
    const size_t nf = (size_t)ticks * DE_PHYS_DET_FIELD_COUNT;
    u64 *run = pd_alloc_u64(ticks);
    u64 *fields = pd_alloc_u64(nf);
    u64 *other = pd_alloc_u64((size_t)ticks + 8u);
    if (run == NULL || fields == NULL || other == NULL) {
        fprintf(stderr, "de_phys_det_check: out of memory\n");
        EXPECT_TRUE(false);
        goto done;
    }

    de_phys_det_options opt;
    memset(&opt, 0, sizeof opt);
    opt.worker_count = workers;
    de_result r = de_phys_det_run(sc, &opt, run, fields);
    if (r != DE_OK) {
        pd_print_error("de_phys_det_run", r);
        EXPECT_EQ(r, DE_OK);
        goto done;
    }

    /* A scenario whose state barely changes would make every comparison meaningless. */
    u32 changes = 0;
    for (u32 t = 1; t < ticks; ++t)
        changes += run[t] != run[t - 1];
    EXPECT_GE(changes * 2u, ticks - 1u);

    char path[1024];
    if (workers == 0) {
        snprintf(path, sizeof path, "%s/physics_%s.checksums", pd_out_dir(), sc->name);
        EXPECT_EQ(pd_write_checksums(path, sc, run, ticks), DE_OK);
        snprintf(path, sizeof path, "%s/physics_%s.fields", pd_out_dir(), sc->name);
        EXPECT_EQ(pd_write_fields(path, sc, fields, ticks), DE_OK);
    }

    char golden[1024];
    snprintf(golden, sizeof golden, "%s/%s.checksums", DE_PHYS_DET_GOLDEN_DIR, sc->name);
    if (de_phys_det_update_mode()) {
#if PD_CAN_UPDATE
        if (workers == 0) {
            EXPECT_EQ(pd_write_checksums(golden, sc, run, ticks), DE_OK);
            fprintf(stderr, "determinism: rewrote %s\n", golden);
        }
#else
        fprintf(stderr, "golden files can only be updated by a Debug or Dev build\n");
        EXPECT_TRUE(false);
        goto done;
#endif
    }

    if (workers > 0) {
        /* Same scenario without a task system: must be equal tick for tick. */
        r = de_phys_det_run(sc, NULL, other, NULL);
        EXPECT_EQ(r, DE_OK);
        char what[64];
        snprintf(what, sizeof what, "%s: workers 0 vs %u", sc->name, workers);
        const u32 first = de_det_report(what, other, run, ticks);
        if (first != DE_DET_NO_DIVERGENCE) {
            de_phys_det_options base;
            memset(&base, 0, sizeof base);
            de_phys_det_diff_at(what, sc, &base, &opt, first, NULL);
        }
        EXPECT_EQ(first, DE_DET_NO_DIVERGENCE);
    }

    /* In update mode, the workers > 0 cases above compared against workers 0 already, and the
     * golden file may be being rewritten by a parallel workers_0 process. */
    if (de_phys_det_update_mode() && workers > 0)
        goto done;
    u32 n = 0;
    r = de_det_read_checksums(golden, other, ticks + 8u, &n);
    if (r != DE_OK) {
        pd_print_error(golden, r);
        fprintf(stderr, "(generate it with --update-golden in a Debug or Dev build)\n");
        EXPECT_EQ(r, DE_OK);
        goto done;
    }
    EXPECT_EQ(n, ticks);
    char what[96];
    snprintf(what, sizeof what, "%s (workers %u): golden vs this build", sc->name, workers);
    const u32 first = de_det_report(what, other, run, n < ticks ? n : ticks);
    if (first != DE_DET_NO_DIVERGENCE) {
        fprintf(stderr, "  field groups of this build at tick %u:", first);
        for (u32 g = 0; g < (u32)DE_PHYS_DET_FIELD_COUNT; ++g) {
            fprintf(stderr, " %s=%016llx", g_field_names[g],
                    (unsigned long long)fields[(size_t)first * DE_PHYS_DET_FIELD_COUNT + g]);
        }
        fprintf(stderr,
                "\n  (compare with physics_%s.fields of a platform that matches the golden "
                "file; the Determinism CI diff job does this)\n",
                sc->name);
    }
    EXPECT_EQ(first, DE_DET_NO_DIVERGENCE);

done:
    pd_free_u64(run, ticks);
    pd_free_u64(fields, nf);
    pd_free_u64(other, (size_t)ticks + 8u);
}

/* ================================ main ================================================= */

int main(int argc, char **argv) {
    /* Take --update-golden; pass everything else to the de_test runner. */
    static char *kept[256];
    int n = 0;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && strcmp(argv[i], "--update-golden") == 0) {
            g_update_flag = true;
            continue;
        }
        if (n >= (int)PD_COUNT(kept)) {
            fprintf(stderr, "error: too many arguments\n");
            return 2;
        }
        kept[n++] = argv[i];
    }
#if !PD_CAN_UPDATE
    if (de_phys_det_update_mode()) {
        fprintf(stderr, "error: --update-golden / DE_DET_UPDATE_GOLDEN needs a Debug or Dev build "
                        "(golden files are never rewritten by Release builds)\n");
        return 2;
    }
#endif
    if (g_update_flag)
        fprintf(stderr, "determinism: update mode: the workers_0 cases rewrite %s/*.checksums\n",
                DE_PHYS_DET_GOLDEN_DIR);
    return de_test_main(n, kept);
}
