/*
 * test_character.c: the kinematic capsule character controller (DENG-221, SPEC §13.1):
 * create and argument checks, standing, falling, jumping, collide and slide (walls, creases),
 * depenetration, slopes, steps, stairs (ground snap), thin walls at speed, no allocation.
 * Platforms and pushing are in test_character_platforms.c.
 */
#include "character_test_util.h"

#include <math.h> /* INFINITY (test input only) */

static const de_vec3 STILL = {0.0f, 0.0f, 0.0f};

static de_physics_world *ct_world(void) {
    de_physics_world *p = st_world(256, NULL, NULL);
    if (p != NULL)
        (void)ct_ground(p);
    return p;
}

/* ---- Create, destroy, arguments ---------------------------------------------------------- */

TEST(physics_character, create_and_destroy) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    DeCharacter c = ct_params();
    const de_transform at = st_xform(1.0, 2.0, 3.0, ct_axis_angle(1.0f, 0.0f, 0.0f, 0.3f));
    const de_body ch = de_character_create(p, &c, &at, DE_ENTITY_NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(ch));
    EXPECT_EQ(de_body_count(p), 2u);
    de_transform t;
    ASSERT_TRUE(de_body_get_transform(p, ch, &t));
    EXPECT_FLOAT_EQ(t.position.z, 3.0, 1e-12);
    /* Tilt is dropped, yaw kept (none here). */
    EXPECT_FLOAT_EQ(t.rotation.w, 1.0f, 1e-6f);
    de_mass_props mp;
    ASSERT_TRUE(de_body_get_mass_props(p, ch, &mp));
    EXPECT_TRUE(mp.mass == 0.0f); /* kinematic */
    u32 i = 0;
    ASSERT_TRUE(de__body_resolve(p, ch, &i));
    const de__shape *s = de__shape_get(p, p->bodies.shape[i]);
    ASSERT_TRUE(s != NULL);
    EXPECT_EQ((u32)s->kind, (u32)DE_SHAPE_CAPSULE);
    EXPECT_FLOAT_EQ(s->u.capsule.radius, 0.3f, 1e-7f);
    EXPECT_FLOAT_EQ(s->u.capsule.half_height, 0.6f, 1e-6f);
    const de_shape shape = p->bodies.shape[i];
    de_character_destroy(p, ch);
    EXPECT_FALSE(de_body_valid(p, ch));
    EXPECT_FALSE(de_shape_valid(p, shape));
    de_character_destroy(p, ch); /* stale: ignored */

    /* Zero parameters take the defaults; yaw survives. */
    DeCharacter z;
    memset(&z, 0, sizeof z);
    const de_transform yawed = st_xform(0.0, 0.0, 2.0, ct_axis_angle(0.0f, 0.0f, 1.0f, 1.0f));
    const de_body d = de_character_create(p, &z, &yawed, DE_ENTITY_NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(d));
    ASSERT_TRUE(de__body_resolve(p, d, &i));
    s = de__shape_get(p, p->bodies.shape[i]);
    EXPECT_FLOAT_EQ(s->u.capsule.radius, DE_CHARACTER_DEFAULT_RADIUS, 1e-7f);
    EXPECT_FLOAT_EQ(s->u.capsule.half_height,
                    0.5f * DE_CHARACTER_DEFAULT_HEIGHT - DE_CHARACTER_DEFAULT_RADIUS, 1e-6f);
    ASSERT_TRUE(de_body_get_transform(p, d, &t));
    EXPECT_FLOAT_EQ(t.rotation.z, yawed.rotation.z, 1e-5f);
    EXPECT_FLOAT_EQ(t.rotation.w, yawed.rotation.w, 1e-5f);
    de_physics_destroy(p);
}

TEST(physics_character, invalid_arguments) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    const de_transform at = st_xform(0.0, 0.0, 1.0, st_quat_identity());
    DeCharacter c = ct_params();
    EXPECT_FALSE(DE_HANDLE_VALID(de_character_create(NULL, &c, &at, DE_ENTITY_NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_character_create(p, NULL, &at, DE_ENTITY_NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_character_create(p, &c, NULL, DE_ENTITY_NULL)));
    DeCharacter bad = c;
    bad.height = 0.5f; /* < 2 * radius */
    EXPECT_FALSE(DE_HANDLE_VALID(de_character_create(p, &bad, &at, DE_ENTITY_NULL)));
    bad = c;
    bad.max_slope_rad = 1.6f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_character_create(p, &bad, &at, DE_ENTITY_NULL)));
    bad = c;
    bad.skin_width = -0.1f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_character_create(p, &bad, &at, DE_ENTITY_NULL)));
    EXPECT_EQ(de_body_count(p), 1u);

    const de_body ch = de_character_create(p, &c, &at, DE_ENTITY_NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(ch));
    EXPECT_EQ(de_character_move(p, ch, NULL, ST_DT, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_character_move(p, ch, &c, 0.0f, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_character_move(p, ch, &c, -1.0f, NULL), DE_ERR_INVALID_ARGUMENT);
    c.desired_velocity.x = INFINITY;
    EXPECT_EQ(de_character_move(p, ch, &c, ST_DT, NULL), DE_ERR_INVALID_ARGUMENT);
    c.desired_velocity.x = 0.0f;
    /* A body that is not a character. */
    const de_body box = st_body(p, DE_BODY_DYNAMIC, st_box(p, 0.5f, 0.5f, 0.5f),
                                st_xform(5.0, 0.0, 2.0, st_quat_identity()), 0.5f, 0.0f);
    EXPECT_EQ(de_character_move(p, box, &c, ST_DT, NULL), DE_ERR_INVALID_ARGUMENT);
    de_character_destroy(p, box); /* not a character: ignored */
    EXPECT_TRUE(de_body_valid(p, box));
    EXPECT_EQ(de_character_move(p, ch, &c, ST_DT, NULL), DE_OK);
    de_physics_destroy(p);
}

/* ---- Standing, falling, jumping ------------------------------------------------------------ */

TEST(physics_character, falls_lands_and_stands) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    DeCharacter c = ct_params();
    const f64 z0 = 3.0;
    const de_body ch = ct_create(p, &c, 0.0, 0.0, z0);
    /* Free fall: the controller integrates gravity like the substep-free Euler v += g dt. */
    f64 v = 0.0, z = z0;
    for (u32 i = 0; i < 20; ++i) {
        de_transform pred;
        ASSERT_TRUE(ct_tick(p, ch, &c, STILL, &pred));
        v += -9.81 * (f64)ST_DT;
        z += v * (f64)ST_DT;
        const de_dvec3 pos = st_pos(p, ch);
        /* The predicted pose is exactly where the step put the proxy. */
        EXPECT_TRUE(pos.z == pred.position.z);
        EXPECT_TRUE(pos.x == pred.position.x);
        EXPECT_FALSE(c.grounded);
    }
    EXPECT_FLOAT_EQ(st_pos(p, ch).z, z, 1e-4);
    ct_run(p, ch, &c, STILL, 100);
    EXPECT_TRUE(c.grounded);
    EXPECT_FLOAT_EQ(st_pos(p, ch).z, ct_stand_z(0.0), 2e-3);
    EXPECT_FLOAT_EQ(c.ground_normal.z, 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(c.velocity.z, 0.0f, 1e-3f);
    /* Standing still does not drift. */
    const f64 rest = st_pos(p, ch).z;
    ct_run(p, ch, &c, STILL, 120);
    EXPECT_TRUE(c.grounded);
    EXPECT_FLOAT_EQ(st_pos(p, ch).z, rest, 1e-5);
    EXPECT_TRUE(st_pos(p, ch).x == 0.0);
    de_physics_destroy(p);
}

TEST(physics_character, jump) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    ct_run(p, ch, &c, STILL, 5);
    ASSERT_TRUE(c.grounded);
    const f64 base = st_pos(p, ch).z;
    /* Hold jump: only the grounded tick jumps. */
    f64 peak = base;
    u32 airborne = 0, landed_tick = 0;
    for (u32 i = 0; i < 120; ++i) {
        ASSERT_TRUE(ct_tick(p, ch, &c, (de_vec3){1.0f, 0.0f, 5.0f}, NULL));
        const f64 z = st_pos(p, ch).z;
        if (z > peak)
            peak = z;
        if (!c.grounded)
            ++airborne;
        else if (airborne > 0 && landed_tick == 0)
            landed_tick = i;
        if (landed_tick != 0)
            break;
    }
    /* v^2 / 2g = 1.274 m (explicit Euler: a little less). */
    ST_GT(peak - base, 1.15);
    ST_LT(peak - base, 1.35); /* + v dt / 2: the jump tick has no gravity yet */
    /* About 2 v / g = 1.02 s in the air. */
    ST_GT(airborne, 55u);
    ST_LT(airborne, 66u);
    EXPECT_FLOAT_EQ(st_pos(p, ch).x, (f64)(airborne + 1u) / 60.0, 0.05);
    de_physics_destroy(p);
}

/* ---- Collide and slide --------------------------------------------------------------------- */

TEST(physics_character, slides_along_wall) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    /* Wall: its face at x = 2. */
    (void)ct_static_box(p, 0.5f, 20.0f, 2.0f, 2.5, 0.0, 2.0, st_quat_identity());
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    ct_run(p, ch, &c, STILL, 2);
    /* 45 degrees into the wall at 4 m/s for 2 s. */
    const de_vec3 want = {2.8284271f, 2.8284271f, 0.0f};
    f64 max_x = 0.0;
    for (u32 i = 0; i < 120; ++i) {
        ASSERT_TRUE(ct_tick(p, ch, &c, want, NULL));
        const de_dvec3 pos = st_pos(p, ch);
        if (pos.x > max_x)
            max_x = pos.x;
        EXPECT_TRUE(c.grounded);
    }
    const de_dvec3 pos = st_pos(p, ch);
    /* Never closer than the skin, and resting at about the skin. */
    ST_LT(max_x, 2.0 - 0.3 - 0.02 + 1e-4);
    EXPECT_FLOAT_EQ(pos.x, 2.0 - 0.3 - 0.02, 2e-3);
    /* Along the wall at the projected speed (2.83 m/s) after reaching it. */
    ST_GT(pos.y, 2.8284271 * 2.0 - 0.5);
    ST_LT(pos.y, 2.8284271 * 2.0 + 1e-3);
    EXPECT_FLOAT_EQ(pos.z, ct_stand_z(0.0), 2e-3);
    de_physics_destroy(p);
}

TEST(physics_character, crease_stops_in_corner) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    /* Two walls at 60 degrees forming a V that opens toward -x: walking +x ends in its tip. */
    const f32 a = 0.5235988f; /* 30 degrees each side */
    (void)ct_static_box(p, 3.0f, 0.25f, 2.0f, 3.0, 1.5, 2.0, ct_axis_angle(0.0f, 0.0f, 1.0f, -a));
    (void)ct_static_box(p, 3.0f, 0.25f, 2.0f, 3.0, -1.5, 2.0, ct_axis_angle(0.0f, 0.0f, 1.0f, a));
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    de_dvec3 last = st_pos(p, ch);
    f64 moved_late = 0.0;
    for (u32 i = 0; i < 240; ++i) {
        ASSERT_TRUE(ct_tick(p, ch, &c, (de_vec3){4.0f, 0.3f, 0.0f}, NULL));
        const de_dvec3 pos = st_pos(p, ch);
        if (i >= 200)
            moved_late += st_len(de__d3_sub(pos, last));
        last = pos;
    }
    /* At rest in the corner: no jitter, symmetric within the input's bias. */
    ST_LT(moved_late, 1e-3);
    ST_GT(last.x, 1.0);
    ST_LT(st_lenf(c.velocity), 0.05f);
    /* Nothing overlaps the capsule. */
    u32 i = 0;
    ASSERT_TRUE(de__body_resolve(p, ch, &i));
    de_transform t;
    ASSERT_TRUE(de_body_get_transform(p, ch, &t));
    de_physics_query_filter f;
    memset(&f, 0, sizeof f);
    f.ignore_body = ch;
    de_body hits[4];
    const de_shape cap = p->bodies.shape[i];
    t.position.z += 0.01; /* off the ground */
    EXPECT_EQ(de_physics_overlap_ex(p, cap, &t, &f, hits, NULL, 4), 0u);
    de_physics_destroy(p);
}

TEST(physics_character, depenetrates) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    /* A pillar overlapping the capsule by 10 cm on the +x side. */
    (void)ct_static_box(p, 0.5f, 0.5f, 2.0f, 0.7, 0.0, 2.0, st_quat_identity());
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    ASSERT_TRUE(ct_tick(p, ch, &c, STILL, NULL));
    EXPECT_GT(ct_stats(p).depen_iterations, 0u);
    const de_dvec3 pos = st_pos(p, ch);
    /* Out to the skin: face at x = 0.2. */
    EXPECT_FLOAT_EQ(pos.x, 0.2 - 0.3 - 0.02, 1e-3);
    EXPECT_FLOAT_EQ(pos.y, 0.0, 1e-6);
    EXPECT_FLOAT_EQ(pos.z, ct_stand_z(0.0), 2e-3);
    /* Deeply inside the floor too (feet 0.25 m under it). */
    const de_body ch2 = ct_create(p, &c, -5.0, 0.0, ct_stand_z(-0.25));
    DeCharacter c2 = ct_params();
    ct_run(p, ch2, &c2, STILL, 3);
    EXPECT_FLOAT_EQ(st_pos(p, ch2).z, ct_stand_z(0.0), 2e-3);
    EXPECT_TRUE(c2.grounded);
    de_physics_destroy(p);
}

/* ---- Slopes --------------------------------------------------------------------------------- */

/* Walks +x at 3 m/s from x = 0 toward a ramp whose foot is at x = 2; returns the final pose. */
static de_dvec3 walk_ramp(f32 degrees, u32 ticks, bool *grounded) {
    de_physics_world *p = ct_world();
    if (p == NULL)
        return (de_dvec3){0, 0, 0};
    (void)ct_ramp(p, 2.0, degrees, 6.0f);
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    ct_run(p, ch, &c, (de_vec3){3.0f, 0.0f, 0.0f}, ticks);
    const de_dvec3 pos = st_pos(p, ch);
    *grounded = c.grounded;
    de_physics_destroy(p);
    return pos;
}

TEST(physics_character, walks_up_30_degree_slope) {
    bool grounded = false;
    const de_dvec3 pos = walk_ramp(30.0f, 150, &grounded);
    /* 2 m to the foot, then up the slope: well above the ground and grounded. */
    ST_GT(pos.x, 4.0);
    const f64 surface = (pos.x - 2.0) * 0.57735027; /* tan 30 */
    EXPECT_TRUE(grounded);
    /* The capsule's lowest point is over x on the slope: its center sits 0.3 / cos 30 + skin
     * gap above the surface below the center, minus the hemisphere offset. Allow a band. */
    ST_GT(pos.z, surface + 0.9);
    ST_LT(pos.z, surface + 1.1);
}

TEST(physics_character, blocked_by_60_degree_slope) {
    bool grounded = false;
    const de_dvec3 pos = walk_ramp(60.0f, 150, &grounded);
    /* Stopped at the foot (the bottom hemisphere touches the slope near x = 2). */
    ST_LT(pos.x, 2.0);
    ST_LT(pos.z, ct_stand_z(0.0) + 0.05);
    EXPECT_TRUE(grounded);
}

TEST(physics_character, slides_down_60_degree_slope) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    (void)ct_ramp(p, 0.0, 60.0f, 6.0f);
    DeCharacter c = ct_params();
    /* Start just off the slope, 4 m up (the surface point at z = 4 is x = 4 / tan 60 = 2.31). */
    const de_body ch = ct_create(p, &c, 2.0, 0.0, 4.8);
    f64 last_z = st_pos(p, ch).z;
    u32 rising = 0;
    for (u32 i = 0; i < 90; ++i) {
        ASSERT_TRUE(ct_tick(p, ch, &c, (de_vec3){-0.0f, 0.0f, 0.0f}, NULL));
        const f64 z = st_pos(p, ch).z;
        if (z > last_z + 1e-9)
            ++rising;
        last_z = z;
        EXPECT_FALSE(c.grounded && st_pos(p, ch).z > 2.0); /* never grounded on the slope */
    }
    EXPECT_EQ(rising, 0u);
    /* Down the slope steadily (at the projection's steady speed: the vertical speed carried into
     * the next tick is the projected one, about 3 g dt on 60 degrees). */
    ST_LT(last_z, 4.8 - 0.5);
    /* Walking into it does not climb it either. */
    const f64 z_before = st_pos(p, ch).z;
    ct_run(p, ch, &c, (de_vec3){-3.0f, 0.0f, 0.0f}, 30);
    ST_LT(st_pos(p, ch).z, z_before + 1e-3);
    de_physics_destroy(p);
}

TEST(physics_character, stands_on_40_degree_slope_without_drift) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    (void)ct_ramp(p, 0.0, 40.0f, 6.0f);
    DeCharacter c = ct_params();
    /* Drop onto the slope at x = 3 (surface z = 2.52). */
    const de_body ch = ct_create(p, &c, 3.0, 0.0, 4.0);
    ct_run(p, ch, &c, STILL, 60);
    ASSERT_TRUE(c.grounded);
    EXPECT_FLOAT_EQ(c.ground_normal.z, 0.76604444f, 1e-4f);
    const de_dvec3 a = st_pos(p, ch);
    ct_run(p, ch, &c, STILL, 120);
    const de_dvec3 b = st_pos(p, ch);
    /* Vertical drift < 1 mm/s over 2 s (and no sideways creep). */
    ST_LT(de__absd(b.z - a.z), 2e-3);
    ST_LT(de__absd(b.x - a.x), 2e-3);
    EXPECT_TRUE(c.grounded);
    de_physics_destroy(p);
}

/* ---- Steps and stairs ------------------------------------------------------------------------ */

/* Walks +x at 2 m/s toward a step of `height` whose riser is at x = 1; returns the final pose. */
static de_dvec3 walk_step(f32 height, f32 step_height, bool *stepped) {
    de_physics_world *p = ct_world();
    if (p == NULL)
        return (de_dvec3){0, 0, 0};
    (void)ct_static_box(p, 3.0f, 3.0f, 0.5f * height, 4.0, 0.0, 0.5 * (f64)height,
                        st_quat_identity());
    DeCharacter c = ct_params();
    c.step_height = step_height;
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    *stepped = false;
    for (u32 i = 0; i < 90; ++i) {
        (void)ct_tick(p, ch, &c, (de_vec3){2.0f, 0.0f, 0.0f}, NULL);
        if (ct_stats(p).stepped)
            *stepped = true;
    }
    const de_dvec3 pos = st_pos(p, ch);
    de_physics_destroy(p);
    return pos;
}

TEST(physics_character, climbs_step_below_step_height) {
    bool stepped = false;
    const de_dvec3 pos = walk_step(0.3f, 0.35f, &stepped);
    EXPECT_TRUE(stepped);
    ST_GT(pos.x, 1.5);
    /* On the step top, between half the skin and the skin above it. */
    ST_GT(pos.z, ct_stand_z(0.3) - 0.5 * (f64)CT_SKIN - 1e-4);
    ST_LT(pos.z, ct_stand_z(0.3) + 1e-4);
}

TEST(physics_character, blocked_by_step_above_step_height) {
    bool stepped = false;
    const de_dvec3 pos = walk_step(0.5f, 0.35f, &stepped);
    EXPECT_FALSE(stepped);
    ST_LT(pos.x, 1.0 - 0.3 + 1e-3);
    EXPECT_FLOAT_EQ(pos.z, ct_stand_z(0.0), 3e-3);
}

TEST(physics_character, stays_grounded_walking_down_stairs) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    /* A landing 2 m high up to x = 0, then 10 steps down (rise 0.2 m, run 0.3 m) toward +x. */
    (void)ct_static_box(p, 2.0f, 3.0f, 1.0f, -2.0, 0.0, 1.0, st_quat_identity());
    for (u32 k = 0; k < 10; ++k) {
        const f32 top = 2.0f - 0.2f * (f32)(k + 1u);
        if (top <= 0.0f)
            break;
        (void)ct_static_box(p, 0.15f, 3.0f, 0.5f * top, 0.15 + 0.3 * (f64)k, 0.0, 0.5 * (f64)top,
                            st_quat_identity());
    }
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, -1.0, 0.0, ct_stand_z(2.0));
    ct_run(p, ch, &c, STILL, 3);
    ASSERT_TRUE(c.grounded);
    u32 airborne = 0;
    for (u32 i = 0; i < 70; ++i) {
        ASSERT_TRUE(ct_tick(p, ch, &c, (de_vec3){4.0f, 0.0f, 0.0f}, NULL));
        if (!c.grounded)
            ++airborne;
    }
    EXPECT_EQ(airborne, 0u);
    /* At the bottom: past the stairs on the ground. */
    const de_dvec3 pos = st_pos(p, ch);
    ST_GT(pos.x, 3.0);
    EXPECT_FLOAT_EQ(pos.z, ct_stand_z(0.0), 3e-3);
    de_physics_destroy(p);
}

/* ---- Speed --------------------------------------------------------------------------------- */

TEST(physics_character, no_tunneling_through_thin_wall) {
    de_physics_world *p = ct_world();
    ASSERT_TRUE(p != NULL);
    /* A 5 cm wall with its near face at x = 3. */
    (void)ct_static_box(p, 0.025f, 5.0f, 2.0f, 3.025, 0.0, 2.0, st_quat_identity());
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    for (u32 i = 0; i < 90; ++i) {
        /* 20 m/s, slightly off the wall's normal. */
        ASSERT_TRUE(ct_tick(p, ch, &c, (de_vec3){20.0f, 0.5f, 0.0f}, NULL));
        ST_LT(st_pos(p, ch).x, 3.0 - 0.3 + 1e-4);
    }
    EXPECT_FLOAT_EQ(st_pos(p, ch).x, 3.0 - 0.3 - 0.02, 3e-3);
    de_physics_destroy(p);
}

/* ---- Allocation ----------------------------------------------------------------------------- */

TEST(physics_character, no_allocation_per_move) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = st_world(64, NULL, &a);
    ASSERT_TRUE(p != NULL);
    (void)ct_ground(p);
    (void)ct_ramp(p, 3.0, 30.0f, 4.0f);
    (void)ct_static_box(p, 0.5f, 0.5f, 0.15f, -2.0, 0.0, 0.15, st_quat_identity());
    (void)st_body(p, DE_BODY_DYNAMIC, st_box(p, 0.4f, 0.4f, 0.4f),
                  st_xform(0.0, 2.0, 0.4, st_quat_identity()), 0.5f, 0.0f);
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    ASSERT_TRUE(ct_tick(p, ch, &c, STILL, NULL)); /* warm-up: physics state */
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    const de_vec3 inputs[4] = {
        {4.0f, 0.0f, 0.0f}, {0.0f, 4.0f, 0.0f}, {-4.0f, 0.0f, 5.0f}, {0.0f, -4.0f, 0.0f}};
    for (u32 i = 0; i < 120; ++i)
        ASSERT_TRUE(ct_tick(p, ch, &c, inputs[(i / 30u) % 4u], NULL));
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}
