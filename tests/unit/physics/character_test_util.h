/* character_test_util.h: scene helpers shared by the character controller tests (DENG-221).
 * Test-only. */
#ifndef DE_CHARACTER_TEST_UTIL_H
#define DE_CHARACTER_TEST_UTIL_H

#include "solver_test_util.h"

#include "physics/character/character_internal.h"

/** Character used by most tests: radius 0.3, height 1.8 (center 0.9 above its feet). */
#define CT_RADIUS 0.3f
#define CT_HEIGHT 1.8f
#define CT_HALF   0.9 /* center height above the feet */
#define CT_SKIN   0.02f

static inline DeCharacter ct_params(void) {
    DeCharacter c;
    memset(&c, 0, sizeof c);
    c.radius = CT_RADIUS;
    c.height = CT_HEIGHT;
    c.step_height = 0.35f;
    c.max_slope_rad = 0.785398163f; /* 45 degrees */
    c.skin_width = CT_SKIN;
    c.ground_entity = DE_ENTITY_NULL;
    return c;
}

static inline de_quat ct_axis_angle(f32 ax, f32 ay, f32 az, f32 angle) {
    return st_quat_axis_angle((de_vec3){ax, ay, az}, angle);
}

/** A static box with half extents (hx, hy, hz) at (x, y, z), rotated by q. */
static inline de_body ct_static_box(de_physics_world *p, f32 hx, f32 hy, f32 hz, f64 x, f64 y,
                                    f64 z, de_quat q) {
    return st_body(p, DE_BODY_STATIC, st_box(p, hx, hy, hz), st_xform(x, y, z, q), 0.5f, 0.0f);
}

/** Flat ground: a static box whose top is z = 0, 100 m x 100 m around the origin. */
static inline de_body ct_ground(de_physics_world *p) {
    return ct_static_box(p, 50.0f, 50.0f, 0.5f, 0.0, 0.0, -0.5, st_quat_identity());
}

/**
 * A ramp rising toward +x at `degrees`: a static slab 2 * half_len long along the slope and 6 m
 * wide whose top face starts on the ground plane at x = foot_x (the box reaches below the
 * ground there).
 */
static inline de_body ct_ramp(de_physics_world *p, f64 foot_x, f32 degrees, f32 half_len) {
    const f32 a = degrees * 0.0174532925f;
    f32 s, c;
    de_dsincos(a, &s, &c);
    const f32 t = 0.25f; /* half thickness */
    /* Rotation about +Y by -a tilts +Z toward -X: normal (-sin a, 0, cos a). */
    const de_quat q = ct_axis_angle(0.0f, 1.0f, 0.0f, -a);
    const f64 sx = foot_x + (f64)c * (f64)half_len, sz = (f64)s * (f64)half_len;
    return ct_static_box(p, half_len, 3.0f, t, sx - (f64)(-s) * (f64)t, 0.0, sz - (f64)c * (f64)t,
                         q);
}

/** Center of a character standing with its feet at height z (skin gap included). */
static inline f64 ct_stand_z(f64 z) {
    return z + CT_HALF + (f64)CT_SKIN;
}

static inline de_body ct_create(de_physics_world *p, const DeCharacter *c, f64 x, f64 y, f64 z) {
    const de_transform t = st_xform(x, y, z, st_quat_identity());
    return de_character_create(p, c, &t, DE_ENTITY_NULL);
}

/** One tick: move with `desired`, then step. Returns false when the move failed. */
static inline bool ct_tick(de_physics_world *p, de_body ch, DeCharacter *c, de_vec3 desired,
                           de_transform *predicted) {
    c->desired_velocity = desired;
    if (de_character_move(p, ch, c, ST_DT, predicted) != DE_OK)
        return false;
    de_physics_step(p, ST_DT);
    return true;
}

/** Runs `ticks` ticks with a constant input. */
static inline void ct_run(de_physics_world *p, de_body ch, DeCharacter *c, de_vec3 desired,
                          u32 ticks) {
    for (u32 i = 0; i < ticks; ++i)
        if (!ct_tick(p, ch, c, desired, NULL))
            return;
}

static inline de__character_stats ct_stats(const de_physics_world *p) {
    const de__character_state *s = de__character_get(p);
    de__character_stats z;
    memset(&z, 0, sizeof z);
    return s != NULL ? s->last : z;
}

#endif /* DE_CHARACTER_TEST_UTIL_H */
