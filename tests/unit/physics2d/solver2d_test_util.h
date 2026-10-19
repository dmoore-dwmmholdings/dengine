/*
 * solver2d_test_util.h: helpers of the 2D solver tests (DENG-254): worlds, bodies with materials,
 * task systems, the pyramid builder, a fake joint class that goes through constraint2d.h only, and
 * the four golden scenarios (pyramid2d_20, bounce2d, sleep_wake2d, bullet2d). Everything is
 * static: each test translation unit gets its own copy.
 */
#ifndef DE_TEST_SOLVER2D_TEST_UTIL_H
#define DE_TEST_SOLVER2D_TEST_UTIL_H

#include "p2d_collide_test_util.h"

#include <dengine/de_task.h>

#include "physics/2d/solver2d_internal.h"

#define SV_DT (1.0f / 60.0f)

/* Float comparisons (EXPECT_LT and EXPECT_GT take integers and pointers only). */
#define SV_EXPECT_LT(a, b) EXPECT_TRUE((a) < (b))
#define SV_EXPECT_GT(a, b) EXPECT_TRUE((a) > (b))

/* ---- Worlds, bodies, task systems ---------------------------------------------------------- */

static inline de_physics2d_world *sv_world(u32 max_bodies, de_task_system *ts,
                                           const de_allocator *a) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = max_bodies;
    d.tasks = ts;
    d.allocator = a;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    return p;
}

static inline de_task_system *sv_tasks(u32 workers) {
    if (workers == 0)
        return NULL;
    de_fp_env_init_thread();
    de_task_system_desc td;
    memset(&td, 0, sizeof td);
    td.worker_count = workers;
    td.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    return ts;
}

/** A body with gravity scale 1 (dynamic), friction and restitution. */
static inline de_body sv_body(de_physics2d_world *p, de_body_type type, de_shape s,
                              de_transform2d xf, f32 friction, f32 restitution, u32 flags) {
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = xf;
    d.flags = flags;
    d.gravity_scale = 1.0f;
    d.material.friction = friction;
    d.material.restitution = restitution;
    return de_body2d_create(p, &d);
}

static inline de_vec2 sv_vel(const de_physics2d_world *p, de_body b) {
    de_vec2 v = {0, 0};
    (void)de_body2d_get_velocity(p, b, &v, NULL);
    return v;
}

static inline f32 sv_angvel(const de_physics2d_world *p, de_body b) {
    f32 w = 0.0f;
    (void)de_body2d_get_velocity(p, b, NULL, &w);
    return w;
}

static inline de_transform2d sv_xf_of(const de_physics2d_world *p, de_body b) {
    de_transform2d t;
    memset(&t, 0, sizeof t);
    (void)de_body2d_get_transform(p, b, &t);
    return t;
}

/** Room for dense piles: broadphase pairs and manifolds. Call before the first step. */
static inline void sv_capacity(de_physics2d_world *p, u32 max_pairs, u32 max_manifolds) {
    de__bp2d_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.max_pairs = max_pairs;
    ASSERT_EQ(de__bp2d_create(p, &bd), DE_OK);
    de__contacts2d_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.max_manifolds = max_manifolds;
    ASSERT_EQ(de__contacts2d_create(p, &cd), DE_OK);
}

/* ---- Pyramid ------------------------------------------------------------------------------ */

#define SV_PYRAMID_MAX 256u

typedef struct sv_pyramid {
    de_body ground;
    de_body boxes[SV_PYRAMID_MAX];
    u32 count;
} sv_pyramid;

/**
 * A static ground (top at y = 0, 80 m wide) and a pyramid of `rows` rows of unit boxes centered at
 * x0: row r holds rows - r boxes 1.05 m apart (no side contacts), each on two below it, with 1 mm
 * gaps. Bottom row first, left to right. `flags` go on every box (DE_BODY_FLAG_ALLOW_SLEEP).
 */
static inline void sv_pyramid_build(sv_pyramid *py, de_physics2d_world *p, u32 rows, f64 x0,
                                    u32 flags, bool with_ground) {
    memset(py, 0, sizeof *py);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    if (with_ground) {
        const de_shape g = tu_box(p, 40.0f, 0.5f, 0.0f);
        py->ground = sv_body(p, DE_BODY_STATIC, g, tu_xf(x0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    }
    for (u32 r = 0; r < rows; ++r) {
        const u32 n = rows - r;
        for (u32 k = 0; k < n && py->count < SV_PYRAMID_MAX; ++k) {
            const f64 x = x0 + 1.05 * ((f64)k - 0.5 * (f64)(n - 1u));
            py->boxes[py->count++] = sv_body(
                p, DE_BODY_DYNAMIC, box, tu_xf(x, 0.5 + 1.001 * (f64)r, 0.0f), 0.6f, 0.0f, flags);
        }
    }
}

static inline u32 sv_pyramid_sleeping(const de_physics2d_world *p, const sv_pyramid *py) {
    u32 n = 0;
    for (u32 i = 0; i < py->count; ++i)
        n += de_body2d_is_sleeping(p, py->boxes[i]) ? 1u : 0u;
    return n;
}

/* ---- Fake joint class ---------------------------------------------------------------------- */

#define SV_JOINT_MAX 64u

/**
 * A fake joint class (DE__CONSTRAINT2D_CLASS_JOINT) through constraint2d.h only: `count` links,
 * gathered in index order with id = index. Its solve pulls the two velocities together a little
 * (so it writes both sides like a real joint) and counts the calls per link.
 */
typedef struct sv_joints {
    u32 count;
    u32 a[SV_JOINT_MAX], b[SV_JOINT_MAX];
    bool active[SV_JOINT_MAX];
    u32 solves[SV_JOINT_MAX];
    u32 passes[SV_JOINT_MAX]; /* solve calls with ctx->pass > 0 */
    u32 gathers;
} sv_joints;

static inline void sv_joints_gather(de__solver2d *s, const de__solver2d_ctx *ctx, void *state) {
    (void)ctx;
    sv_joints *j = (sv_joints *)state;
    ++j->gathers;
    for (u32 k = 0; k < j->count; ++k) {
        j->solves[k] = 0;
        j->passes[k] = 0;
        if (!j->active[k])
            continue;
        de__constraint2d *c =
            de__solver2d_add_constraint(s, DE__CONSTRAINT2D_CLASS_JOINT, k, j->a[k], j->b[k]);
        if (c != NULL)
            c->data = k;
    }
}

static inline void sv_joints_solve(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c,
                                   bool bias) {
    (void)bias;
    sv_joints *j = (sv_joints *)state;
    ++j->solves[c->data];
    if (ctx->pass > 0u)
        ++j->passes[c->data];
    de__solver_body2d *A = &ctx->bodies[c->solver_a];
    de__solver_body2d *B = &ctx->bodies[c->solver_b];
    const de_vec2 d = de__v2_scale(de__v2_sub(B->v, A->v), 0.01f);
    if (c->flags & DE__CONSTRAINT2D_WRITE_A)
        A->v = de__v2_add(A->v, d);
    if (c->flags & DE__CONSTRAINT2D_WRITE_B)
        B->v = de__v2_sub(B->v, d);
}

static const de__constraint2d_class k_sv_joints = {
    "sv_joints", sv_joints_gather, NULL, NULL, sv_joints_solve, NULL, NULL, NULL, NULL};

static inline void sv_joints_register(de_physics2d_world *p, sv_joints *j) {
    ASSERT_EQ(
        de__solver2d_register_class(p, DE__CONSTRAINT2D_CLASS_JOINT, &k_sv_joints, j, SV_JOINT_MAX),
        DE_OK);
}

static inline u32 sv_joints_add(sv_joints *j, u32 a, u32 b) {
    const u32 k = j->count++;
    j->a[k] = a;
    j->b[k] = b;
    j->active[k] = true;
    return k;
}

/* ---- Golden scenarios ---------------------------------------------------------------------- */

/*
 * Each scenario builds its world (on the given task system), then runs `ticks` steps with a
 * script. The probe hashes the checksum of every tick and the sleep state of every live body, in a
 * running XXH3; the vector records it every 60 ticks and at the end.
 */
typedef struct sv_scene {
    de_physics2d_world *p;
    sv_pyramid py;
    de_shape ball, small_box, bullet_circle, bullet_box;
    de_body bodies[64];
    u32 count;
    de_body platform, plate;
} sv_scene;

typedef enum sv_scenario {
    SV_PYRAMID2D_20,
    SV_BOUNCE2D,
    SV_SLEEP_WAKE2D,
    SV_BULLET2D,
    SV_SCENARIO_COUNT
} sv_scenario;

static const char *const k_sv_scenario_names[SV_SCENARIO_COUNT] = {"pyramid2d_20", "bounce2d",
                                                                   "sleep_wake2d", "bullet2d"};
static const u32 k_sv_scenario_ticks[SV_SCENARIO_COUNT] = {600u, 480u, 600u, 240u};

static inline void sv_scene_build(sv_scene *sc, sv_scenario which, de_task_system *ts,
                                  const de_allocator *a) {
    memset(sc, 0, sizeof *sc);
    de_physics2d_world *p = sv_world(which == SV_PYRAMID2D_20 ? 512u : 256u, ts, a);
    sc->p = p;
    sc->ball = tu_circle(p, 0.4f, (de_vec2){0, 0});
    sc->small_box = tu_box(p, 0.3f, 0.3f, 0.0f);
    sc->bullet_circle = tu_circle(p, 0.05f, (de_vec2){0, 0});
    sc->bullet_box = tu_box(p, 0.1f, 0.04f, 0.0f);
    switch (which) {
    case SV_PYRAMID2D_20:
        sv_capacity(p, 4096u, 2048u);
        sv_pyramid_build(&sc->py, p, 20, 0.0, DE_BODY_FLAG_ALLOW_SLEEP, true);
        break;
    case SV_BOUNCE2D: {
        const de_shape g = tu_box(p, 30.0f, 0.5f, 0.0f);
        (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.5f, 0.0f, 0);
        const de_shape plate = tu_box(p, 3.0f, 0.1f, 0.0f);
        (void)sv_body(p, DE_BODY_STATIC, plate, tu_xf(14.0, 2.0, 0.3f), 0.4f, 0.2f, 0);
        const de_shape cap = tu_capsule(p, (de_vec2){-0.4f, 0}, (de_vec2){0.4f, 0}, 0.2f);
        for (u32 i = 0; i < 16; ++i) {
            const de_shape s = (i % 4u) == 3u ? cap : ((i % 4u) == 2u ? sc->small_box : sc->ball);
            const f32 e = 0.0625f * (f32)i;
            de_body b = sv_body(p, DE_BODY_DYNAMIC, s,
                                tu_xf(-12.0 + 1.7 * (f64)i, 1.0 + 0.4 * (f64)(i % 5u), 0.2f * i),
                                0.3f + 0.04f * (f32)(i % 7u), e, 0);
            (void)de_body2d_set_velocity(p, b, (de_vec2){0.5f * (f32)(i % 3u) - 0.5f, 2.0f},
                                         0.7f * (f32)((i % 5u)) - 1.4f);
            sc->bodies[sc->count++] = b;
        }
        break;
    }
    case SV_SLEEP_WAKE2D: {
        sv_pyramid_build(&sc->py, p, 6, 0.0, DE_BODY_FLAG_ALLOW_SLEEP, true);
        const de_shape plate = tu_box(p, 1.5f, 0.1f, 0.0f);
        sc->plate = sv_body(p, DE_BODY_STATIC, plate, tu_xf(12.0, 3.0, 0.0f), 0.6f, 0.0f, 0);
        for (u32 i = 0; i < 3; ++i)
            sc->bodies[sc->count++] =
                sv_body(p, DE_BODY_DYNAMIC, sc->small_box, tu_xf(12.0, 3.4 + 0.601 * i, 0.0f), 0.6f,
                        0.0f, DE_BODY_FLAG_ALLOW_SLEEP);
        const de_shape pl = tu_box(p, 1.0f, 0.1f, 0.0f);
        sc->platform = sv_body(p, DE_BODY_KINEMATIC, pl, tu_xf(-12.0, 2.0, 0.0f), 0.8f, 0.0f, 0);
        sc->bodies[sc->count++] =
            sv_body(p, DE_BODY_DYNAMIC, sc->small_box, tu_xf(-12.0, 2.401, 0.0f), 0.8f, 0.0f,
                    DE_BODY_FLAG_ALLOW_SLEEP);
        break;
    }
    case SV_BULLET2D: {
        de_physics2d_set_gravity(p, (de_vec2){0.0f, 0.0f});
        /* Thin static walls: a segment, a 1 cm box, a chain, a kinematic slab. */
        const de_shape seg = tu_segment(p, (de_vec2){0, -2}, (de_vec2){0, 2});
        (void)sv_body(p, DE_BODY_STATIC, seg, tu_xf(10.0, 0.0, 0.0f), 0.2f, 0.0f, 0);
        const de_shape thin = tu_box(p, 0.005f, 2.0f, 0.0f);
        (void)sv_body(p, DE_BODY_STATIC, thin, tu_xf(10.0, 6.0, 0.0f), 0.2f, 0.0f, 0);
        static const de_vec2 pts[4] = {{0, 2}, {0, 0.5f}, {0, -0.5f}, {0, -2}};
        (void)sv_body(p, DE_BODY_STATIC, de_shape2d_chain_create(p, pts, 4, false),
                      tu_xf(10.0, 12.0, 0.0f), 0.2f, 0.0f, 0);
        const de_shape slab = tu_box(p, 0.01f, 2.0f, 0.0f);
        sc->platform = sv_body(p, DE_BODY_KINEMATIC, slab, tu_xf(10.0, 18.0, 0.0f), 0.2f, 0.0f, 0);
        (void)de_body2d_set_velocity(p, sc->platform, (de_vec2){-1.0f, 0.0f}, 0.0f);
        for (u32 i = 0; i < 4; ++i) {
            const u32 flags = DE_BODY2D_FLAG_BULLET;
            de_body b = sv_body(p, DE_BODY_DYNAMIC, (i & 1u) ? sc->bullet_box : sc->bullet_circle,
                                tu_xf(0.0, 6.0 * (f64)i + 0.3 * (f64)i, 0.1f * (f32)i), 0.2f,
                                0.2f * (f32)i, flags);
            (void)de_body2d_set_velocity(p, b, (de_vec2){300.0f, -4.0f * (f32)i},
                                         (i & 1u) ? 40.0f : 0.0f);
            sc->bodies[sc->count++] = b;
        }
        break;
    }
    default:
        break;
    }
}

static inline void sv_scene_script(sv_scene *sc, sv_scenario which, u32 tick) {
    de_physics2d_world *p = sc->p;
    switch (which) {
    case SV_PYRAMID2D_20:
        if (tick == 360u) {
            de_body b = sv_body(p, DE_BODY_DYNAMIC, sc->ball, tu_xf(-16.0, 1.0, 0.0f), 0.5f, 0.1f,
                                DE_BODY_FLAG_ALLOW_SLEEP);
            (void)de_body2d_set_velocity(p, b, (de_vec2){14.0f, 3.0f}, 0.0f);
        }
        break;
    case SV_SLEEP_WAKE2D:
        if (tick == 120u) { /* thrown ball at the pyramid */
            de_body b = sv_body(p, DE_BODY_DYNAMIC, sc->ball, tu_xf(-6.0, 1.0, 0.0f), 0.5f, 0.0f,
                                DE_BODY_FLAG_ALLOW_SLEEP);
            (void)de_body2d_set_velocity(p, b, (de_vec2){12.0f, 2.0f}, 0.0f);
        }
        if (tick == 200u) /* user velocity on the stack */
            (void)de_body2d_set_velocity(p, sc->bodies[2], (de_vec2){0.5f, 0.0f}, 0.0f);
        if (tick == 300u) /* explicit wake of the platform passenger */
            de_body2d_wake(p, sc->bodies[3]);
        if (tick == 330u)
            (void)de_body2d_set_velocity(p, sc->platform, (de_vec2){0.5f, 0.0f}, 0.0f);
        if (tick == 400u)
            (void)de_body2d_set_velocity(p, sc->platform, (de_vec2){0.0f, 0.0f}, 0.0f);
        if (tick == 460u) { /* destroy a sleeping box, then reuse its index */
            de_body2d_destroy(p, sc->bodies[1]);
            sc->bodies[1] = sv_body(p, DE_BODY_DYNAMIC, sc->small_box, tu_xf(12.0, 6.0, 0.3f), 0.6f,
                                    0.0f, DE_BODY_FLAG_ALLOW_SLEEP);
        }
        if (tick == 540u) /* the plate under the stack goes */
            de_body2d_destroy(p, sc->plate);
        break;
    default:
        break;
    }
}

/** Running hash of the checksum and the sleep state of every live body. */
static inline u64 sv_probe(const sv_scene *sc, u64 h) {
    const de_physics2d_world *p = sc->p;
    u8 buf[16];
    const u64 ck = de_physics2d_checksum(p);
    for (u32 i = 0; i < 8; ++i)
        buf[i] = (u8)(ck >> (8u * i));
    h = de_hash64(buf, 8, h);
    u32 bits = 0, n = 0;
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!p->bodies.alive[i])
            continue;
        if (de__islands2d_body_sleeping(p, i))
            bits ^= (1u << (n & 31u)) * (i + 1u);
        ++n;
    }
    for (u32 i = 0; i < 4; ++i)
        buf[i] = (u8)(bits >> (8u * i));
    return de_hash64(buf, 4, h);
}

/** Runs a scenario: fills `out` with "<name>.tNNN" entries every 60 ticks and "<name>.final". */
static inline u32 sv_scenario_run(sv_scenario which, de_task_system *ts, u32 grain,
                                  tu_golden *out) {
    static sv_scene sc;
    sv_scene_build(&sc, which, ts, NULL);
    if (grain != 0u) {
        de__solver2d *s = NULL;
        ASSERT_EQ(de__solver2d_ensure(sc.p, &s), DE_OK);
        s->coloring->grain = grain;
        s->coloring->body_grain = grain;
    }
    u64 h = 0;
    u32 n = 0;
    const u32 ticks = k_sv_scenario_ticks[which];
    for (u32 t = 1; t <= ticks; ++t) {
        sv_scene_script(&sc, which, t);
        de_physics2d_step(sc.p, SV_DT);
        h = sv_probe(&sc, h);
        if (t % 60u == 0u) {
            snprintf(out[n].name, sizeof out[n].name, "%s.t%03u", k_sv_scenario_names[which], t);
            out[n++].value = h;
        }
    }
    snprintf(out[n].name, sizeof out[n].name, "%s.final", k_sv_scenario_names[which]);
    out[n++].value = de_physics2d_checksum(sc.p);
    de_physics2d_destroy(sc.p);
    return n;
}

#endif /* DE_TEST_SOLVER2D_TEST_UTIL_H */
