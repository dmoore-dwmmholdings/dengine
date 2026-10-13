/* island_test_util.h: scenes and a fake joint class shared by the islands, sleep, coloring and
 * parallel solver tests (DENG-218). Test-only. */
#ifndef DE_ISLAND_TEST_UTIL_H
#define DE_ISLAND_TEST_UTIL_H

#include "solver_test_util.h"

#include <dengine/de_task.h>

#include "physics/islands.h"
#include "physics/solver/graph_color.h"

/* ---- bodies -------------------------------------------------------------------------------- */

/** A dynamic body that may sleep (DE_BODY_FLAG_ALLOW_SLEEP), gravity scale 1. */
static inline de_body it_sleeper(de_physics_world *p, de_shape s, de_transform t, f32 friction) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = s;
    d.transform = t;
    d.gravity_scale = 1.0f;
    d.material.friction = friction;
    d.flags = DE_BODY_FLAG_ALLOW_SLEEP;
    return de_body_create(p, &d);
}

static inline u32 it_index(de_body b) {
    return de__handle_index(b.id);
}

/** Room for dense piles: broadphase pairs and manifolds. Call before the first step. */
static inline void it_capacity(de_physics_world *p, u32 max_pairs, u32 max_manifolds) {
    de__broadphase_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.max_pairs = max_pairs;
    ASSERT_EQ(de__broadphase_create(p, &bd), DE_OK);
    de__contacts_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.max_manifolds = max_manifolds;
    ASSERT_EQ(de__contacts_create(p, &cd), DE_OK);
}

/* ---- pyramid -------------------------------------------------------------------------------- */

#define IT_PYRAMID_MAX 64u

typedef struct it_pyramid {
    de_body ground;
    de_body boxes[IT_PYRAMID_MAX];
    u32 count;
} it_pyramid;

/**
 * A static ground (top at z = 0) and a pyramid of `rows` rows of unit boxes centered at (x0, 0):
 * row r holds rows - r boxes, 1.05 m apart (no side contacts), each box on two below it, 1 mm
 * gaps. Bodies are created bottom row first, left to right. `allow_sleep` sets the flag on all.
 */
static inline void it_pyramid_build(it_pyramid *py, de_physics_world *p, u32 rows, f64 x0,
                                    bool allow_sleep, bool with_ground) {
    memset(py, 0, sizeof *py);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    if (with_ground) {
        const de_shape g = st_box(p, 30.0f, 30.0f, 0.5f);
        py->ground =
            st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.6f, 0.0f);
    }
    for (u32 r = 0; r < rows; ++r) {
        const u32 n = rows - r;
        for (u32 k = 0; k < n && py->count < IT_PYRAMID_MAX; ++k) {
            const f64 x = x0 + 1.05 * ((f64)k - 0.5 * (f64)(n - 1u));
            const de_transform t = st_xform(x, 0.0, 0.5 + 1.001 * (f64)r, st_quat_identity());
            py->boxes[py->count++] = allow_sleep ? it_sleeper(p, cube, t, 0.6f)
                                                 : st_body(p, DE_BODY_DYNAMIC, cube, t, 0.6f, 0.0f);
        }
    }
}

static inline u32 it_pyramid_sleeping(const de_physics_world *p, const it_pyramid *py) {
    u32 n = 0;
    for (u32 i = 0; i < py->count; ++i)
        n += de_body_is_sleeping(p, py->boxes[i]) ? 1u : 0u;
    return n;
}

/* ---- task systems ---------------------------------------------------------------------------- */

static inline de_task_system *it_tasks_new(u32 workers) {
    if (workers == 0)
        return NULL;
    de_fp_env_init_thread();
    de_task_system_desc d;
    memset(&d, 0, sizeof d);
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    if (de_task_system_create(&d, &ts) != DE_OK)
        return NULL;
    return ts;
}

/* ---- fake joint class ------------------------------------------------------------------------ */

#define IT_JOINT_MAX 64u

/**
 * A fake joint class (DE__CONSTRAINT_CLASS_JOINT) through constraint.h only: `count` links
 * (body_a, body_b), gathered in index order with id = index. Its solve pulls the two bodies'
 * velocities together a little (so it writes both sides like a real joint) and counts the calls
 * per link; each record writes only its own counter, so parallel colors stay race-free.
 */
typedef struct it_joints {
    u32 count;
    u32 a[IT_JOINT_MAX], b[IT_JOINT_MAX];
    bool active[IT_JOINT_MAX];
    u32 solves[IT_JOINT_MAX]; /* solve calls this step, per link */
    u32 gathers;
} it_joints;

static inline void it_joints_gather(de__solver *s, const de__solver_ctx *ctx, void *state) {
    (void)ctx;
    it_joints *j = (it_joints *)state;
    ++j->gathers;
    for (u32 k = 0; k < j->count; ++k) {
        j->solves[k] = 0;
        if (!j->active[k])
            continue;
        de__constraint *c =
            de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_JOINT, k, j->a[k], j->b[k]);
        if (c != NULL)
            c->data = k;
    }
}

static inline void it_joints_solve(const de__solver_ctx *ctx, void *state, de__constraint *c,
                                   bool bias) {
    (void)bias;
    it_joints *j = (it_joints *)state;
    ++j->solves[c->data];
    de__solver_body *A = &ctx->bodies[c->solver_a];
    de__solver_body *B = &ctx->bodies[c->solver_b];
    const de_vec3 d = de__v3_scale(de__v3_sub(B->v, A->v), 0.01f);
    if (c->flags & DE__CONSTRAINT_WRITE_A)
        A->v = de__v3_add(A->v, d);
    if (c->flags & DE__CONSTRAINT_WRITE_B)
        B->v = de__v3_sub(B->v, d);
}

static const de__constraint_class k_it_joints = {
    "it_joints", it_joints_gather, NULL, NULL, it_joints_solve, NULL, NULL, NULL, NULL};

/** Registers `j` (zeroed by the caller) as the joint class with room for IT_JOINT_MAX links. */
static inline void it_joints_register(de_physics_world *p, it_joints *j) {
    ASSERT_EQ(
        de__solver_register_class(p, DE__CONSTRAINT_CLASS_JOINT, &k_it_joints, j, IT_JOINT_MAX),
        DE_OK);
}

static inline u32 it_joints_add(it_joints *j, u32 a, u32 b) {
    const u32 k = j->count++;
    j->a[k] = a;
    j->b[k] = b;
    j->active[k] = true;
    return k;
}

#endif /* DE_ISLAND_TEST_UTIL_H */
