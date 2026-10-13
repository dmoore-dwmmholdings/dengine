/* solver_test_util.h: small scene helpers shared by the solver tests (DENG-217). Test-only. */
#ifndef DE_SOLVER_TEST_UTIL_H
#define DE_SOLVER_TEST_UTIL_H

#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/broadphase/broadphase.h"
#include "physics/contact/manifold.h"
#include "physics/physics_internal.h"
#include "physics/solver/solver_internal.h"

#include <stdio.h>
#include <string.h>

#define ST_DT (1.0f / 60.0f)

/** Float comparisons that print both values on failure. */
#define ST_LT(a, b)                                                                                \
    do {                                                                                           \
        const double st_a_ = (double)(a), st_b_ = (double)(b);                                     \
        if (!(st_a_ < st_b_))                                                                      \
            fprintf(stderr, "  %s = %.9g, %s = %.9g\n", #a, st_a_, #b, st_b_);                     \
        EXPECT_TRUE(st_a_ < st_b_);                                                                \
    } while (0)
#define ST_GT(a, b) ST_LT(b, a)

static inline de_quat st_quat_identity(void) {
    return (de_quat){0.0f, 0.0f, 0.0f, 1.0f};
}

static inline de_quat st_quat_axis_angle(de_vec3 axis, f32 angle) {
    const de_vec3 a = de_dnormalize_v3(axis);
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    return (de_quat){a.x * s, a.y * s, a.z * s, c};
}

static inline de_transform st_xform(f64 x, f64 y, f64 z, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = (de_dvec3){x, y, z};
    t.rotation = q;
    t.scale = (de_vec3){1.0f, 1.0f, 1.0f};
    return t;
}

static inline de_physics_world *st_world(u32 max_bodies, de_task_system *tasks,
                                         const de_allocator *a) {
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = max_bodies;
    d.tasks = tasks;
    d.allocator = a;
    de_physics_world *p = NULL;
    if (de_physics_create(&d, &p) != DE_OK)
        return NULL;
    return p;
}

static inline de_shape st_box(de_physics_world *p, f32 hx, f32 hy, f32 hz) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){hx, hy, hz};
    return de_shape_create(p, &d);
}

static inline de_shape st_sphere(de_physics_world *p, f32 r) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = r;
    return de_shape_create(p, &d);
}

/** A body with gravity scale 1 and the given material. */
static inline de_body st_body(de_physics_world *p, de_body_type type, de_shape s, de_transform t,
                              f32 friction, f32 restitution) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.gravity_scale = 1.0f;
    d.material.friction = friction;
    d.material.restitution = restitution;
    return de_body_create(p, &d);
}

static inline de_dvec3 st_pos(const de_physics_world *p, de_body b) {
    de_transform t;
    if (!de_body_get_transform(p, b, &t))
        return (de_dvec3){0, 0, 0};
    return t.position;
}

static inline de_quat st_rot(const de_physics_world *p, de_body b) {
    de_transform t;
    if (!de_body_get_transform(p, b, &t))
        return st_quat_identity();
    return t.rotation;
}

static inline de_vec3 st_vel(const de_physics_world *p, de_body b) {
    de_vec3 v = {0, 0, 0};
    (void)de_body_get_velocity(p, b, &v, NULL);
    return v;
}

static inline de_vec3 st_angvel(const de_physics_world *p, de_body b) {
    de_vec3 w = {0, 0, 0};
    (void)de_body_get_velocity(p, b, NULL, &w);
    return w;
}

static inline f64 st_len(de_dvec3 a) {
    return de_dsqrt64(a.x * a.x + a.y * a.y + a.z * a.z);
}

static inline f32 st_lenf(de_vec3 a) {
    return de_dsqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

/** Speculative margin `spec` and a broadphase margin that allows it (fast impacts in tests;
 *  per-body margins come with CCD, DENG-219). Call before the first step. */
static inline void st_margins(de_physics_world *p, f32 spec) {
    de__broadphase_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.margin = 2.0f * spec;
    ASSERT_EQ(de__broadphase_create(p, &bd), DE_OK);
    de__contacts_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.speculative = spec;
    ASSERT_EQ(de__contacts_create(p, &cd), DE_OK);
}

/** The solver state (created if needed), for settings changes before the first step. */
static inline de__solver *st_solver(de_physics_world *p) {
    de__solver *s = NULL;
    if (de__solver_ensure(p, &s) != DE_OK)
        return NULL;
    return s;
}

#endif /* DE_SOLVER_TEST_UTIL_H */
