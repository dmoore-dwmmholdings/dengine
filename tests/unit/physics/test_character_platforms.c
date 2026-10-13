/*
 * test_character_platforms.c: the character controller on moving platforms and against dynamic
 * bodies (DENG-221, SPEC §13.1): translating, rotating and vertical platforms (no sliding),
 * pushing boxes (mass ratio, push force, wake-up), static walls, determinism of a mixed run.
 */
#include "character_test_util.h"

static const de_vec3 STILL = {0.0f, 0.0f, 0.0f};

/* The character's position in the platform's frame. */
static de_dvec3 in_frame(const de_physics_world *p, de_body platform, de_dvec3 x) {
    de_transform t;
    if (!de_body_get_transform(p, platform, &t))
        return x;
    const de__dmat3 r = de__dmat3_from_quat(t.rotation);
    return de__dmat3_tmul(&r, de__d3_sub(x, t.position));
}

/* Yaw of a rotation about +Z (radians). */
static f32 yaw_of(de_quat q) {
    const de_vec3 f = de__quat_rotate(q, (de_vec3){1.0f, 0.0f, 0.0f});
    return de_datan2(f.y, f.x);
}

static f32 wrap_pi(f32 a) {
    while (a > 3.14159265f)
        a -= 6.28318531f;
    while (a < -3.14159265f)
        a += 6.28318531f;
    return a;
}

typedef struct ride_result {
    f64 drift;          /* largest distance from the start, platform frame (m) */
    f64 yaw_error;      /* |character yaw change - platform yaw change| at the end (rad) */
    u32 airborne_ticks; /* ticks the character was not grounded */
    bool on_platform;   /* the last move was carried */
} ride_result;

/* A kinematic 5 m x 5 m platform at height 1 moving with (v, w) (v switches sign every
 * `flip` ticks when flip > 0); the character stands at (ox, oy) on it for `ticks` ticks. */
static ride_result ride(de_vec3 v, de_vec3 w, f64 ox, f64 oy, u32 ticks, u32 flip, de_vec3 walk) {
    ride_result r;
    memset(&r, 0, sizeof r);
    de_physics_world *p = st_world(64, NULL, NULL);
    if (p == NULL)
        return r;
    const de_body plat = st_body(p, DE_BODY_KINEMATIC, st_box(p, 2.5f, 2.5f, 0.25f),
                                 st_xform(0.0, 0.0, 1.0, st_quat_identity()), 0.5f, 0.0f);
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, ox, oy, ct_stand_z(1.25));
    ct_run(p, ch, &c, STILL, 2); /* settle onto the platform */
    (void)de_body_set_velocity(p, plat, v, w);
    const de_dvec3 start = in_frame(p, plat, st_pos(p, ch));
    const f32 yaw_c0 = yaw_of(st_rot(p, ch)), yaw_p0 = yaw_of(st_rot(p, plat));
    for (u32 i = 0; i < ticks; ++i) {
        if (flip > 0u && i > 0u && i % flip == 0u) {
            v = de__v3_scale(v, -1.0f);
            (void)de_body_set_velocity(p, plat, v, w);
        }
        if (!ct_tick(p, ch, &c, walk, NULL))
            break;
        if (!c.grounded)
            ++r.airborne_ticks;
        const de_dvec3 now = in_frame(p, plat, st_pos(p, ch));
        const de_dvec3 expected =
            de__d3_add(start, de__d3((f64)walk.x * (f64)(i + 1u) / 60.0,
                                     (f64)walk.y * (f64)(i + 1u) / 60.0, 0.0));
        const f64 d = st_len(de__d3_sub(now, expected));
        if (d > r.drift)
            r.drift = d;
    }
    r.on_platform = ct_stats(p).on_platform;
    const f32 dc = wrap_pi(yaw_of(st_rot(p, ch)) - yaw_c0);
    const f32 dp = wrap_pi(yaw_of(st_rot(p, plat)) - yaw_p0);
    r.yaw_error = de__absd((f64)wrap_pi(dc - dp));
    de_physics_destroy(p);
    return r;
}

TEST(physics_character_platforms, rides_translating_platform) {
    const ride_result r =
        ride((de_vec3){1.5f, -0.5f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f}, 0.5, 0.3, 300, 0, STILL);
    EXPECT_TRUE(r.on_platform);
    EXPECT_EQ(r.airborne_ticks, 0u);
    ST_LT(r.drift, 0.01);
}

TEST(physics_character_platforms, rides_rotating_platform) {
    /* Translating and spinning at 0.8 rad/s; the character 1.5 m from the axis. */
    const ride_result r =
        ride((de_vec3){1.0f, 0.5f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.8f}, 1.5, 0.0, 300, 0, STILL);
    EXPECT_TRUE(r.on_platform);
    EXPECT_EQ(r.airborne_ticks, 0u);
    ST_LT(r.drift, 0.01);
    ST_LT(r.yaw_error, 1e-3);
}

TEST(physics_character_platforms, rides_vertical_platform) {
    /* An elevator at 2 m/s, reversing every second. */
    const ride_result r =
        ride((de_vec3){0.0f, 0.0f, 2.0f}, (de_vec3){0.0f, 0.0f, 0.0f}, 0.0, 0.0, 300, 60, STILL);
    EXPECT_EQ(r.airborne_ticks, 0u);
    ST_LT(r.drift, 0.01);
}

TEST(physics_character_platforms, walks_on_moving_platform) {
    /* Walking +x at 1 m/s on a platform moving +y at 1.5 m/s: in the platform's frame the
     * character walks exactly 1 m/s along +x (no rotation, so world and platform axes agree). */
    const ride_result r = ride((de_vec3){0.0f, 1.5f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f}, -1.0, 0.0,
                               120, 0, (de_vec3){1.0f, 0.0f, 0.0f});
    EXPECT_TRUE(r.on_platform);
    EXPECT_EQ(r.airborne_ticks, 0u);
    ST_LT(r.drift, 0.01);
}

/* ---- Pushing ----------------------------------------------------------------------------------
 */

typedef struct push_result {
    f64 box_dx;     /* box displacement along +x after the run */
    f64 min_gap;    /* smallest x gap between the capsule and the box's near face */
    u32 push_ticks; /* moves that pushed */
    bool box_awake; /* the box was not asleep at the end */
} push_result;

/* The character walks +x at 2 m/s for `ticks` ticks into a 1 m cube of `mass` kg at x = 2. */
static push_result push_run(f32 mass, f32 push_force, bool sleepy, u32 ticks) {
    push_result r;
    memset(&r, 0, sizeof r);
    r.min_gap = 1e9;
    de_physics_world *p = st_world(64, NULL, NULL);
    if (p == NULL)
        return r;
    (void)ct_ground(p);
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_DYNAMIC;
    bd.shape = st_box(p, 0.5f, 0.5f, 0.5f);
    bd.transform = st_xform(2.5, 0.0, 0.5, st_quat_identity());
    bd.mass = mass;
    bd.material.friction = 0.5f;
    bd.gravity_scale = 1.0f;
    bd.flags = sleepy ? (u32)DE_BODY_FLAG_ALLOW_SLEEP : 0u;
    const de_body box = de_body_create(p, &bd);
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    de__character_state *st = de__character_get(p);
    if (st != NULL)
        st->settings.push_force = push_force;
    /* Let the box settle (and fall asleep when it may). */
    ct_run(p, ch, &c, STILL, 60);
    if (sleepy && !de_body_is_sleeping(p, box))
        r.box_awake = true; /* reported as a failure by the caller */
    const f64 x0 = st_pos(p, box).x;
    for (u32 i = 0; i < ticks; ++i) {
        if (!ct_tick(p, ch, &c, (de_vec3){2.0f, 0.0f, 0.0f}, NULL))
            break;
        if (ct_stats(p).pushes > 0u)
            ++r.push_ticks;
        const f64 gap = (st_pos(p, box).x - 0.5) - (st_pos(p, ch).x + 0.3);
        if (gap < r.min_gap)
            r.min_gap = gap;
    }
    r.box_dx = st_pos(p, box).x - x0;
    r.box_awake = !de_body_is_sleeping(p, box);
    de_physics_destroy(p);
    return r;
}

TEST(physics_character_platforms, pushes_50kg_box) {
    const push_result r = push_run(50.0f, DE__CHAR_DEFAULT_PUSH_FORCE, false, 180);
    ST_GT(r.push_ticks, 10u);
    /* 3 s at 2 m/s from 1.7 m away: about 4.3 m of contact walking. */
    ST_GT(r.box_dx, 2.5);
    ST_GT(r.min_gap, -0.01); /* the box never ends up inside the capsule */
}

TEST(physics_character_platforms, heavier_box_moves_less) {
    const push_result light = push_run(50.0f, 400.0f, false, 180);
    const push_result heavy = push_run(150.0f, 400.0f, false, 180);
    ST_GT(light.box_dx, heavy.box_dx + 0.1);
    /* 400 N against 0.5 * 150 * 9.81 = 736 N of friction: the heavy box does not slide. */
    ST_LT(heavy.box_dx, 0.05);
}

TEST(physics_character_platforms, no_push_force_no_push) {
    const push_result r = push_run(50.0f, 0.0f, false, 120);
    EXPECT_EQ(r.push_ticks, 0u);
    ST_LT(de__absd(r.box_dx), 0.02);
}

TEST(physics_character_platforms, push_wakes_sleeping_box) {
    const push_result r = push_run(50.0f, DE__CHAR_DEFAULT_PUSH_FORCE, true, 120);
    ST_GT(r.box_dx, 1.0);
    EXPECT_TRUE(r.box_awake);
}

TEST(physics_character_platforms, cannot_push_static_wall) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    (void)ct_ground(p);
    const de_body wall = ct_static_box(p, 0.5f, 3.0f, 1.5f, 2.5, 0.0, 1.5, st_quat_identity());
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    ct_run(p, ch, &c, (de_vec3){4.0f, 0.0f, 0.0f}, 120);
    EXPECT_EQ(ct_stats(p).pushes, 0u);
    EXPECT_TRUE(st_pos(p, wall).x == 2.5);
    EXPECT_FLOAT_EQ(st_pos(p, ch).x, 2.0 - 0.3 - 0.02, 2e-3);
    de_physics_destroy(p);
}

/* ---- Determinism ------------------------------------------------------------------------------
 */

/* A short mixed run: ground, ramp, step, a kinematic platform and a box; the checksum after it. */
static u64 mixed_run(void) {
    de_physics_world *p = st_world(64, NULL, NULL);
    if (p == NULL)
        return 0;
    (void)ct_ground(p);
    (void)ct_ramp(p, 4.0, 25.0f, 3.0f);
    (void)ct_static_box(p, 1.0f, 1.0f, 0.15f, -3.0, 0.0, 0.15, st_quat_identity());
    const de_body plat = st_body(p, DE_BODY_KINEMATIC, st_box(p, 1.5f, 1.5f, 0.2f),
                                 st_xform(0.0, 4.0, 0.2, st_quat_identity()), 0.5f, 0.0f);
    (void)de_body_set_velocity(p, plat, (de_vec3){0.5f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.3f});
    (void)st_body(p, DE_BODY_DYNAMIC, st_box(p, 0.4f, 0.4f, 0.4f),
                  st_xform(2.0, -2.0, 0.4, st_quat_identity()), 0.5f, 0.0f);
    DeCharacter c = ct_params();
    const de_body ch = ct_create(p, &c, 0.0, 0.0, ct_stand_z(0.0));
    const de_vec3 inputs[6] = {{4.0f, 0.0f, 0.0f}, {0.0f, -3.0f, 0.0f}, {-4.0f, 0.0f, 5.0f},
                               {0.0f, 4.0f, 0.0f}, {3.0f, 3.0f, 0.0f},  {-2.0f, -1.0f, 4.0f}};
    for (u32 i = 0; i < 240; ++i)
        (void)ct_tick(p, ch, &c, inputs[(i / 40u) % 6u], NULL);
    const u64 sum = de_physics_checksum(p);
    de_physics_destroy(p);
    return sum;
}

TEST(physics_character_platforms, identical_runs_identical_checksums) {
    const u64 a = mixed_run();
    const u64 b = mixed_run();
    EXPECT_NE(a, 0ull);
    EXPECT_EQ(a, b);
}
