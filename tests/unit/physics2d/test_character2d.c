/*
 * test_character2d.c: the 2D kinematic character controller (DENG-258): create / destroy /
 * arguments, falling and landing, jumps, slopes (30° walked, 60° blocked), steps, stairs down,
 * one-way platforms, moving platforms (drift), box proxies, top-down movement, no allocation per
 * move, and the `platformer_character2d` course golden
 * (tests/unit/physics2d/vectors/platformer_character2d_golden.txt). Regenerate it (never by hand)
 * with
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics2d/test_physics2d_character
 * --filter=physics2d_character.platformer_character2d_golden
 *
 * The tests never call de_physics2d_step: each tick moves the character, then sets its proxy to
 * the predicted pose and moves kinematic platforms by de__p2d_predict_kinematic, which is what the
 * step's kinematic integration (DENG-254) does. So the results do not depend on the solver.
 */
#include "p2d_collide_test_util.h"
#include "platformer_character2d_course.h"

#include "physics/2d/character2d_internal.h"

#ifndef DE_PHYSICS2D_CHARACTER_GOLDEN
#error "DE_PHYSICS2D_CHARACTER_GOLDEN (path of platformer_character2d_golden.txt) must be defined"
#endif

#define DT   (1.0f / 60.0f)
#define SKIN ((f64)DE_CHARACTER2D_SKIN_WIDTH)

/* ---- Helpers -------------------------------------------------------------------------------- */

static de_physics2d_world *make_world(void) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 256;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    return p;
}

static de_body static_body(de_physics2d_world *p, de_shape s, f64 x, f64 y, f32 angle) {
    return tu_body(p, DE_BODY_STATIC, s, tu_xf(x, y, angle), 0);
}

/* Ground: a static box whose top is y = 0, from x = -50 to 50. */
static de_body ground(de_physics2d_world *p) {
    return static_body(p, tu_box(p, 50.0f, 1.0f, 0.0f), 0.0, -1.0, 0.0f);
}

/* A chain from left-to-right points, created reversed (collides from above). */
static de_body chain_lr(de_physics2d_world *p, const de_vec2 *pts, u32 n) {
    de_vec2 rev[64];
    for (u32 i = 0; i < n; ++i)
        rev[i] = pts[n - 1u - i];
    const de_shape s = de_shape2d_chain_create(p, rev, n, false);
    EXPECT_TRUE(DE_HANDLE_VALID(s));
    return static_body(p, s, 0.0, 0.0, 0.0f);
}

static DeCharacter2D character(void) {
    DeCharacter2D c;
    memset(&c, 0, sizeof c);
    c.size = de__v2(0.6f, 1.8f);
    return c;
}

static de_body spawn(de_physics2d_world *p, const DeCharacter2D *c, f64 x, f64 y,
                     de_character2d_proxy kind) {
    de_dpoint2d at;
    at.x = x;
    at.y = y;
    const de_body b = de_character2d_create(p, c, at, kind, DE_ENTITY_NULL);
    EXPECT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

/* One tick without a solver: move, then put the proxy where the step would. */
static void tick(de_physics2d_world *p, de_body ch, DeCharacter2D *io) {
    de_transform2d out;
    ASSERT_EQ(de_character2d_move(p, ch, io, DT, &out), DE_OK);
    ASSERT_TRUE(de_body2d_set_transform(p, ch, &out));
}

/* Advances kinematic body `b` by its velocity over one tick (the step's integration). */
static void advance_kinematic(de_physics2d_world *p, de_body b) {
    de_vec2 v;
    f32 w;
    ASSERT_TRUE(de_body2d_get_velocity(p, b, &v, &w));
    de_transform2d t;
    de__p2d_predict_kinematic(p, tu_index(b), v, w, DT, &t);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &t));
}

static de_transform2d pose(de_physics2d_world *p, de_body b) {
    de_transform2d t;
    EXPECT_TRUE(de_body2d_get_transform(p, b, &t));
    return t;
}

static const de__character2d_stats *last_stats(de_physics2d_world *p) {
    return &de__character2d_get(p)->last;
}

/* Lets a character settle on the ground (a few ticks of no input). */
static void settle(de_physics2d_world *p, de_body ch, DeCharacter2D *io, u32 ticks) {
    io->desired_velocity = de__v2(0.0f, 0.0f);
    for (u32 i = 0; i < ticks; ++i)
        tick(p, ch, io);
}

/* ---- Create, destroy, arguments ---------------------------------------------------------- */

TEST(physics2d_character, create_destroy_defaults) {
    de_physics2d_world *p = make_world();
    DeCharacter2D c;
    memset(&c, 0, sizeof c);
    const de_body b = spawn(p, &c, 1.0, 2.0, DE_CHARACTER2D_PROXY_CAPSULE);
    de_aabb2d box;
    de_shape2d_kind kind;
    u32 i = tu_index(b);
    ASSERT_TRUE(de_shape2d_get_local_aabb(p, p->bodies.shape[i], &box));
    ASSERT_TRUE(de_shape2d_get_kind(p, p->bodies.shape[i], &kind));
    EXPECT_EQ((int)kind, (int)DE_SHAPE2D_CAPSULE);
    EXPECT_FLOAT_EQ(box.max.x - box.min.x, DE_CHARACTER2D_DEFAULT_WIDTH, 1e-5);
    EXPECT_FLOAT_EQ(box.max.y - box.min.y, DE_CHARACTER2D_DEFAULT_HEIGHT, 1e-5);
    EXPECT_EQ((int)p->bodies.type[i], (int)DE_BODY_KINEMATIC);
    EXPECT_TRUE((p->bodies.flags[i] & DE_BODY2D_FLAG_FIXED_ROTATION) != 0u);
    const de_transform2d t = pose(p, b);
    TU_EXPECT_FEQ(t.x, 1.0);
    TU_EXPECT_FEQ(t.y, 2.0);
    EXPECT_EQ(de__character2d_get(p)->live, 1u);

    DeCharacter2D bc = character();
    bc.size = de__v2(1.0f, 0.5f);
    const de_body bb = spawn(p, &bc, 5.0, 2.0, DE_CHARACTER2D_PROXY_BOX);
    ASSERT_TRUE(de_shape2d_get_kind(p, p->bodies.shape[tu_index(bb)], &kind));
    EXPECT_EQ((int)kind, (int)DE_SHAPE2D_BOX);
    EXPECT_EQ(de__character2d_get(p)->live, 2u);

    const de_shape s = p->bodies.shape[i];
    de_character2d_destroy(p, b);
    EXPECT_FALSE(de_body2d_valid(p, b));
    EXPECT_FALSE(de_shape2d_valid(p, s));
    EXPECT_EQ(de__character2d_get(p)->live, 1u);
    de_character2d_destroy(p, b); /* stale: ignored */
    /* A body that is not a character is not destroyed. */
    const de_body g = ground(p);
    de_character2d_destroy(p, g);
    EXPECT_TRUE(de_body2d_valid(p, g));
    /* Destroying the proxy body directly forgets the character. */
    de_body2d_destroy(p, bb);
    EXPECT_EQ(de__character2d_get(p)->live, 0u);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, invalid_arguments) {
    de_physics2d_world *p = make_world();
    DeCharacter2D c = character();
    de_dpoint2d at = {0.0, 0.0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_character2d_create(NULL, &c, at, 0, DE_ENTITY_NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_character2d_create(p, NULL, at, DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL)));
    EXPECT_FALSE(
        DE_HANDLE_VALID(de_character2d_create(p, &c, at, (de_character2d_proxy)7, DE_ENTITY_NULL)));
    DeCharacter2D bad = c;
    bad.size = de__v2(1.0f, 0.5f); /* capsule lower than wide */
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_character2d_create(p, &bad, at, DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL)));
    bad = c;
    bad.size.x = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_character2d_create(p, &bad, at, DE_CHARACTER2D_PROXY_BOX, DE_ENTITY_NULL)));
    bad = c;
    bad.max_slope_rad = 1.6f;
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_character2d_create(p, &bad, at, DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL)));
    bad = c;
    bad.step_height = -0.1f;
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_character2d_create(p, &bad, at, DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL)));
    at.x = (f64)INFINITY;
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_character2d_create(p, &c, at, DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL)));
    EXPECT_EQ(de_body2d_count(p), 0u);

    at.x = 0.0;
    const de_body ch = spawn(p, &c, 0.0, 5.0, DE_CHARACTER2D_PROXY_CAPSULE);
    const de_body g = ground(p);
    DeCharacter2D io = c;
    EXPECT_EQ(de_character2d_move(NULL, ch, &io, DT, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_character2d_move(p, ch, NULL, DT, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_character2d_move(p, g, &io, DT, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_character2d_move(p, ch, &io, 0.0f, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_character2d_move(p, ch, &io, NAN, NULL), DE_ERR_INVALID_ARGUMENT);
    io.desired_velocity.x = NAN;
    EXPECT_EQ(de_character2d_move(p, ch, &io, DT, NULL), DE_ERR_INVALID_ARGUMENT);
    io = c;
    io.max_slope_rad = 2.0f;
    EXPECT_EQ(de_character2d_move(p, ch, &io, DT, NULL), DE_ERR_INVALID_ARGUMENT);
    /* Nothing changed. */
    de_vec2 v;
    ASSERT_TRUE(de_body2d_get_velocity(p, ch, &v, NULL));
    TU_EXPECT_FEQ(v.x, 0.0f);
    TU_EXPECT_FEQ(v.y, 0.0f);
    io = c;
    EXPECT_EQ(de_character2d_move(p, ch, &io, DT, NULL), DE_OK);
    de_physics2d_destroy(p);
}

/* ---- Falling, landing, jumping ------------------------------------------------------------ */

TEST(physics2d_character, falls_lands_and_stands) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 3.0, DE_CHARACTER2D_PROXY_CAPSULE);
    u32 landed = 0;
    for (u32 i = 0; i < 120 && landed == 0u; ++i) {
        tick(p, ch, &io);
        if (io.grounded)
            landed = i + 1u;
    }
    ASSERT_GT(landed, 0u);
    /* Fell 3 - 0.9 - skin = 2.09 m: about sqrt(2 * 2.09 / 9.81) = 0.65 s. */
    EXPECT_GT(landed, 30u);
    EXPECT_LT(landed, 50u);
    const f64 rest = 0.9 + SKIN;
    EXPECT_FLOAT_EQ(pose(p, ch).y, rest, 1e-4);
    /* Standing: no drift, grounded, zero velocity. */
    for (u32 i = 0; i < 120; ++i) {
        tick(p, ch, &io);
        ASSERT_TRUE(io.grounded);
    }
    EXPECT_FLOAT_EQ(pose(p, ch).y, rest, 1e-5);
    EXPECT_FLOAT_EQ(pose(p, ch).x, 0.0, 1e-9);
    EXPECT_FLOAT_EQ(io.velocity.y, 0.0, 1e-4);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, jump_peak_and_air_time) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    settle(p, ch, &io, 5);
    const f64 y0 = pose(p, ch).y;
    io.desired_velocity = de__v2(0.0f, 5.0f);
    tick(p, ch, &io);
    EXPECT_FALSE(io.grounded);
    io.desired_velocity = de__v2(0.0f, 0.0f);
    f64 peak = y0;
    u32 air = 1;
    while (!io.grounded && air < 200u) {
        tick(p, ch, &io);
        const f64 y = pose(p, ch).y;
        if (y > peak)
            peak = y;
        ++air;
    }
    /* v^2 / 2g = 1.274 m; explicit integration loses up to about v dt / 2. */
    EXPECT_FLOAT_EQ(peak - y0, 25.0 / (2.0 * 9.81), 0.05);
    /* 2 v / g = 1.02 s = 61 ticks. */
    EXPECT_GE(air, 58u);
    EXPECT_LE(air, 64u);
    EXPECT_FLOAT_EQ(pose(p, ch).y, y0, 1e-4);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, ceiling_stops_jump) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    (void)static_body(p, tu_box(p, 5.0f, 0.1f, 0.0f), 0.0, 2.5, 0.0f); /* bottom at 2.4 */
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    settle(p, ch, &io, 2);
    io.desired_velocity = de__v2(0.0f, 8.0f);
    tick(p, ch, &io);
    io.desired_velocity = de__v2(0.0f, 0.0f);
    f64 top = 0.0;
    for (u32 i = 0; i < 90; ++i) {
        tick(p, ch, &io);
        const f64 y = pose(p, ch).y + 0.9;
        if (y > top)
            top = y;
    }
    EXPECT_TRUE(top <= 2.4 - SKIN + 1e-4);
    EXPECT_TRUE(top > 2.4 - 3.0 * SKIN);
    EXPECT_TRUE(io.grounded);
    de_physics2d_destroy(p);
}

/* ---- Walls, slopes ------------------------------------------------------------------------ */

TEST(physics2d_character, wall_stops_at_skin) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    (void)static_body(p, tu_box(p, 0.5f, 3.0f, 0.0f), 3.5, 3.0, 0.0f); /* face at x = 3 */
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(4.0f, 0.0f);
    for (u32 i = 0; i < 90; ++i) {
        tick(p, ch, &io);
        ASSERT_TRUE(io.grounded);
    }
    EXPECT_FLOAT_EQ(pose(p, ch).x, 3.0 - 0.3 - SKIN, 1e-4);
    EXPECT_FLOAT_EQ(io.velocity.x, 0.0, 1e-3);
    de_physics2d_destroy(p);
}

/* A ramp from (x0, 0) rising at `deg` for `rise` m, then flat. */
static void ramp(de_physics2d_world *p, f64 x0, f64 deg, f64 rise) {
    const f64 run = rise / tan(deg * 3.14159265358979 / 180.0);
    const de_vec2 pts[4] = {{-20.0f, 0.0f},
                            {(f32)x0, 0.0f},
                            {(f32)(x0 + run), (f32)rise},
                            {(f32)(x0 + run + 20.0), (f32)rise}};
    (void)chain_lr(p, pts, 4);
}

TEST(physics2d_character, walks_up_30_degree_slope) {
    de_physics2d_world *p = make_world();
    ramp(p, 2.0, 30.0, 2.0);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(3.0f, 0.0f);
    u32 ungrounded = 0;
    for (u32 i = 0; i < 150; ++i) {
        tick(p, ch, &io);
        if (!io.grounded)
            ++ungrounded;
    }
    EXPECT_EQ(ungrounded, 0u);
    /* Over the top: on the plateau at y = 2. */
    const de_transform2d t = pose(p, ch);
    EXPECT_TRUE(t.x > 2.0 + 3.4641 + 0.5);
    EXPECT_FLOAT_EQ(t.y, 2.0 + 0.9 + SKIN, 2e-3);
    /* On the slope the capsule rests at skin along the normal: check one point mid-ramp. */
    de_physics2d_destroy(p);
    p = make_world();
    ramp(p, 2.0, 30.0, 4.0);
    io = character();
    const de_body c2 = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(3.0f, 0.0f);
    for (u32 i = 0; i < 80; ++i)
        tick(p, c2, &io);
    ASSERT_TRUE(io.grounded);
    const de_transform2d m = pose(p, c2);
    /* The lower cap center (x, y - 0.6) is r + skin from the slope line. */
    const f64 nx = -0.5, ny = 0.8660254037844386;
    const f64 d = (m.x - 2.0) * nx + (m.y - 0.6) * ny;
    EXPECT_FLOAT_EQ(d, 0.3 + SKIN, 2e-4);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, blocked_by_60_degree_slope) {
    de_physics2d_world *p = make_world();
    ramp(p, 2.0, 60.0, 3.0);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(3.0f, 0.0f);
    f64 max_y = 0.0;
    for (u32 i = 0; i < 120; ++i) {
        tick(p, ch, &io);
        const de_transform2d t = pose(p, ch);
        if (t.y > max_y)
            max_y = t.y;
    }
    const de_transform2d t = pose(p, ch);
    /* Stopped at the foot: it may rest against the slope but never climbs it. */
    EXPECT_TRUE(t.x < 2.0);
    EXPECT_TRUE(max_y < 0.9 + SKIN + 0.05);
    EXPECT_TRUE(io.grounded);
    /* Dropped onto the slope from above, it slides down and does not rise. */
    de_body2d_destroy(p, ch);
    io = character();
    const de_body c2 = spawn(p, &io, 2.0 + 0.5, 3.0, DE_CHARACTER2D_PROXY_CAPSULE);
    f64 prev_y = pose(p, c2).y;
    bool rose = false;
    /* The slide reaches a steady speed (the carried vertical speed is the projected one), so it
     * takes a while: about 0.5 m/s down along the slope. */
    for (u32 i = 0; i < 360; ++i) {
        tick(p, c2, &io);
        const f64 y = pose(p, c2).y;
        if (y > prev_y + 1e-9)
            rose = true;
        prev_y = y;
    }
    EXPECT_FALSE(rose);
    EXPECT_TRUE(pose(p, c2).x < 2.0);
    EXPECT_TRUE(io.grounded);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, stands_on_40_degree_slope) {
    de_physics2d_world *p = make_world();
    ramp(p, 0.0, 40.0, 5.0);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 3.0, 4.5, DE_CHARACTER2D_PROXY_CAPSULE);
    settle(p, ch, &io, 60);
    ASSERT_TRUE(io.grounded);
    const de_transform2d a = pose(p, ch);
    settle(p, ch, &io, 120);
    const de_transform2d b = pose(p, ch);
    EXPECT_TRUE(fabs(b.x - a.x) + fabs(b.y - a.y) < 2e-3);
    de_physics2d_destroy(p);
}

/* ---- Steps and stairs ---------------------------------------------------------------------- */

static void step_scene(de_physics2d_world *p, f32 h) {
    (void)ground(p);
    (void)static_body(p, tu_box(p, 5.0f, 0.5f * h, 0.0f), 5.0, 0.5 * (f64)h, 0.0f); /* x 0..10 */
}

TEST(physics2d_character, climbs_step_up_to_step_height) {
    de_physics2d_world *p = make_world();
    step_scene(p, 0.25f);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, -2.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(3.0f, 0.0f);
    bool stepped = false;
    for (u32 i = 0; i < 90; ++i) {
        tick(p, ch, &io);
        ASSERT_TRUE(io.grounded);
        stepped = stepped || last_stats(p)->stepped;
    }
    EXPECT_TRUE(stepped);
    EXPECT_TRUE(pose(p, ch).x > 1.0);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 0.25 + 0.9 + SKIN, 2e-3);
    de_physics2d_destroy(p);

    /* 0.4 m is above the default 0.3 m step height: blocked. */
    p = make_world();
    step_scene(p, 0.4f);
    io = character();
    const de_body c2 = spawn(p, &io, -2.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(3.0f, 0.0f);
    for (u32 i = 0; i < 90; ++i)
        tick(p, c2, &io);
    EXPECT_TRUE(pose(p, c2).x < 0.0);
    EXPECT_FLOAT_EQ(pose(p, c2).y, 0.9 + SKIN, 1e-3);
    /* With step_height 0.45 it climbs. */
    io.step_height = 0.45f;
    for (u32 i = 0; i < 60; ++i)
        tick(p, c2, &io);
    EXPECT_TRUE(pose(p, c2).x > 0.5);
    EXPECT_FLOAT_EQ(pose(p, c2).y, 0.4 + 0.9 + SKIN, 2e-3);
    de_physics2d_destroy(p);
}

/* Stairs down to the right: n steps of `h` high and `d` deep from (x0, n h) to y = 0. */
static void stairs(de_physics2d_world *p, f64 x0, u32 n, f64 h, f64 d) {
    de_vec2 pts[64];
    u32 k = 0;
    pts[k++] = de__v2(-20.0f, (f32)(n * h));
    for (u32 i = 0; i < n; ++i) {
        const f64 x = x0 + d * i;
        pts[k++] = de__v2((f32)x, (f32)(h * (n - i)));
        pts[k++] = de__v2((f32)x, (f32)(h * (n - i - 1u)));
    }
    pts[k++] = de__v2(40.0f, 0.0f);
    (void)chain_lr(p, pts, k);
}

TEST(physics2d_character, grounded_down_and_up_stairs) {
    de_physics2d_world *p = make_world();
    stairs(p, 0.0, 8, 0.25, 0.4);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, -1.0, 2.0 + 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(4.0f, 0.0f);
    u32 airborne = 0;
    for (u32 i = 0; i < 75; ++i) {
        tick(p, ch, &io);
        if (!io.grounded)
            ++airborne;
    }
    EXPECT_EQ(airborne, 0u);
    EXPECT_TRUE(pose(p, ch).x > 3.5);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 0.9 + SKIN, 2e-3);
    /* And back up. */
    io.desired_velocity = de__v2(-3.0f, 0.0f);
    for (u32 i = 0; i < 120; ++i) {
        tick(p, ch, &io);
        if (!io.grounded)
            ++airborne;
    }
    EXPECT_EQ(airborne, 0u);
    EXPECT_TRUE(pose(p, ch).x < -0.5);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 2.0 + 0.9 + SKIN, 2e-3);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, fast_wall_not_tunneled) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    (void)static_body(p, tu_box(p, 0.025f, 3.0f, 0.0f), 3.0, 3.0, 0.0f); /* 5 cm wall */
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    io.desired_velocity = de__v2(40.0f, 0.0f);
    for (u32 i = 0; i < 30; ++i)
        tick(p, ch, &io);
    EXPECT_FLOAT_EQ(pose(p, ch).x, 3.0 - 0.025 - 0.3 - SKIN, 1e-4);
    de_physics2d_destroy(p);
}

/* ---- One-way platforms --------------------------------------------------------------------- */

TEST(physics2d_character, one_way_platform_up_through_and_land) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    const de_vec2 ow[2] = {{-2.0f, 1.5f}, {2.0f, 1.5f}};
    (void)chain_lr(p, ow, 2);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    settle(p, ch, &io, 2);
    io.desired_velocity = de__v2(0.0f, 7.0f); /* rise 2.5 m: the feet pass 1.5 */
    tick(p, ch, &io);
    io.desired_velocity = de__v2(0.0f, 0.0f);
    f64 top = 0.0;
    for (u32 i = 0; i < 120; ++i) {
        tick(p, ch, &io);
        const f64 y = pose(p, ch).y;
        if (y > top)
            top = y;
    }
    EXPECT_TRUE(top - 0.9 > 1.5); /* went up through */
    EXPECT_TRUE(io.grounded);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 1.5 + 0.9 + SKIN, 1e-4); /* landed on top */
    /* Walks off its end and falls to the ground. */
    io.desired_velocity = de__v2(3.0f, 0.0f);
    for (u32 i = 0; i < 90; ++i)
        tick(p, ch, &io);
    EXPECT_TRUE(pose(p, ch).x > 2.3);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 0.9 + SKIN, 1e-4);
    /* A short jump from below (feet do not clear it) passes up and falls back down. */
    io.desired_velocity = de__v2(-3.0f, 0.0f);
    for (u32 i = 0; i < 50; ++i)
        tick(p, ch, &io);
    io.desired_velocity = de__v2(0.0f, 3.0f); /* rise 0.46 m: the head passes 1.5, not the feet */
    tick(p, ch, &io);
    io.desired_velocity = de__v2(0.0f, 0.0f);
    for (u32 i = 0; i < 60; ++i)
        tick(p, ch, &io);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 0.9 + SKIN, 1e-4);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, one_way_rays_and_casts) {
    de_physics2d_world *p = make_world();
    const de_vec2 ow[2] = {{-2.0f, 1.0f}, {2.0f, 1.0f}};
    const de_body plat = chain_lr(p, ow, 2);
    de_ray_hit2d h;
    de_dpoint2d o = {0.0, 0.0};
    EXPECT_FALSE(de_physics2d_raycast_ex(p, o, de__v2(0.0f, 1.0f), 5.0f, NULL, &h));
    o.y = 3.0;
    ASSERT_TRUE(de_physics2d_raycast_ex(p, o, de__v2(0.0f, -1.0f), 5.0f, NULL, &h));
    EXPECT_EQ(h.body.id, plat.id);
    EXPECT_FLOAT_EQ(h.fraction, 0.4f, 1e-6);
    TU_EXPECT_FEQ(h.normal.y, 1.0f);
    const de_shape circle = tu_circle(p, 0.5f, de__v2(0.0f, 0.0f));
    de_transform2d from = tu_xf(0.0, 0.0, 0.0f);
    EXPECT_FALSE(de_physics2d_shape_cast(p, circle, &from, de__v2(0.0f, 1.0f), 5.0f, NULL, &h));
    from.y = 3.0;
    ASSERT_TRUE(de_physics2d_shape_cast(p, circle, &from, de__v2(0.0f, -1.0f), 5.0f, NULL, &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0f, 1.5f, 2e-5);
    /* Straddling with the center below the line: passes down (and up). */
    from.y = 0.8;
    EXPECT_FALSE(de_physics2d_shape_cast(p, circle, &from, de__v2(0.0f, -1.0f), 2.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_shape_cast(p, circle, &from, de__v2(0.0f, 1.0f), 2.0f, NULL, &h));
    /* Center above the line, rounding below it: in front, so it is stopped at once (it starts
     * overlapping): a body whose core cleared a one-way platform lands on it. */
    from.y = 1.3;
    ASSERT_TRUE(de_physics2d_shape_cast(p, circle, &from, de__v2(0.0f, -1.0f), 2.0f, NULL, &h));
    TU_EXPECT_FEQ(h.fraction, 0.0f);
    /* Within 5 mm of the line is still in front. */
    from.y = 1.0 + 0.5 - 0.004;
    ASSERT_TRUE(de_physics2d_shape_cast(p, circle, &from, de__v2(0.0f, -1.0f), 2.0f, NULL, &h));
    de_physics2d_destroy(p);
}

/* ---- Moving platforms ---------------------------------------------------------------------- */

/* Rides a kinematic platform moving with velocity v(t); returns the worst relative drift. */
static f64 ride(de_physics2d_world *p, de_body plat, de_body ch, DeCharacter2D *io,
                de_vec2 (*vel)(u32), f32 (*spin)(u32), u32 ticks) {
    const de_transform2d c0 = pose(p, ch), p0 = pose(p, plat);
    /* The character's offset in the platform's frame. */
    const f64 rx = c0.x - p0.x, ry = c0.y - p0.y;
    const f64 lx = p0.rotation.c * rx + p0.rotation.s * ry;
    const f64 ly = -p0.rotation.s * rx + p0.rotation.c * ry;
    f64 worst = 0.0;
    for (u32 i = 0; i < ticks; ++i) {
        ASSERT_TRUE(de_body2d_set_velocity(p, plat, vel(i), spin != NULL ? spin(i) : 0.0f));
        tick(p, ch, io);
        advance_kinematic(p, plat);
        EXPECT_TRUE(io->grounded);
        const de_transform2d c = pose(p, ch), q = pose(p, plat);
        const f64 ex = q.x + q.rotation.c * lx - q.rotation.s * ly;
        const f64 ey = q.y + q.rotation.s * lx + q.rotation.c * ly;
        const f64 d = sqrt((c.x - ex) * (c.x - ex) + (c.y - ey) * (c.y - ey));
        if (d > worst)
            worst = d;
    }
    return worst;
}

static de_vec2 vel_sideways(u32 t) {
    return de__v2(((t / 90u) % 2u) == 0u ? 1.5f : -1.5f, 0.0f);
}
static de_vec2 vel_elevator(u32 t) {
    return de__v2(0.0f, ((t / 100u) % 2u) == 0u ? 2.0f : -2.0f);
}
static de_vec2 vel_diag(u32 t) {
    (void)t;
    return de__v2(0.7f, 0.3f);
}
static f32 spin_slow(u32 t) {
    (void)t;
    return 0.2f; /* 0.4 rad over 2 s: the platform stays walkable */
}

static de_body platform(de_physics2d_world *p, f64 x, f64 y) {
    return tu_body(p, DE_BODY_KINEMATIC, tu_box(p, 3.0f, 0.2f, 0.0f), tu_xf(x, y, 0.0f), 0);
}

TEST(physics2d_character, rides_moving_platforms) {
    de_physics2d_world *p = make_world();
    const de_body plat = platform(p, 0.0, 1.0);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.5, 1.2 + 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    settle(p, ch, &io, 3);
    f64 drift = ride(p, plat, ch, &io, vel_sideways, NULL, 300);
    EXPECT_TRUE(drift < 0.01);
    EXPECT_TRUE(last_stats(p)->on_platform);
    drift = ride(p, plat, ch, &io, vel_elevator, NULL, 300);
    EXPECT_TRUE(drift < 0.01);
    drift = ride(p, plat, ch, &io, vel_diag, NULL, 300);
    EXPECT_TRUE(drift < 0.01);
    /* Translating and turning (0.4 rad over 2 s): the upright capsule rolls on its rounded bottom
     * as the surface tilts under it (r sin 0.4 = 0.12 m), but stays grounded on the platform. */
    drift = ride(p, plat, ch, &io, vel_diag, spin_slow, 120);
    EXPECT_TRUE(drift < 0.15);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, walks_on_moving_platform) {
    de_physics2d_world *p = make_world();
    const de_body plat = platform(p, 0.0, 1.0);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, -2.0, 1.2 + 0.9 + SKIN, DE_CHARACTER2D_PROXY_CAPSULE);
    io.grounded = true;
    settle(p, ch, &io, 3);
    const f64 x0 = pose(p, ch).x - pose(p, plat).x;
    io.desired_velocity = de__v2(1.0f, 0.0f);
    for (u32 i = 0; i < 120; ++i) {
        ASSERT_TRUE(de_body2d_set_velocity(p, plat, de__v2(-2.0f, 0.0f), 0.0f));
        tick(p, ch, &io);
        advance_kinematic(p, plat);
        ASSERT_TRUE(io.grounded);
    }
    /* 2 s at 1 m/s relative to the platform. */
    const f64 rel = pose(p, ch).x - pose(p, plat).x - x0;
    EXPECT_FLOAT_EQ(rel, 2.0, 0.01);
    EXPECT_FLOAT_EQ(io.velocity.x, 1.0, 1e-4); /* own velocity, without the carry */
    de_physics2d_destroy(p);
}

/* ---- Box proxies, top-down, dynamic bodies -------------------------------------------------- */

TEST(physics2d_character, box_proxy_walks_slopes_and_steps) {
    de_physics2d_world *p = make_world();
    const de_vec2 pts[6] = {{-20.0f, 0.0f}, {2.0f, 0.0f},   {2.0f, 0.2f},
                            {4.0f, 0.2f},   {6.0f, 1.355f}, {30.0f, 1.355f}}; /* step, 30° */
    (void)chain_lr(p, pts, 6);
    DeCharacter2D io = character();
    io.size = de__v2(0.8f, 1.0f);
    const de_body ch = spawn(p, &io, 0.0, 0.5 + SKIN, DE_CHARACTER2D_PROXY_BOX);
    io.grounded = true;
    io.desired_velocity = de__v2(3.0f, 0.0f);
    u32 airborne = 0;
    for (u32 i = 0; i < 150; ++i) {
        tick(p, ch, &io);
        if (!io.grounded)
            ++airborne;
    }
    EXPECT_EQ(airborne, 0u);
    EXPECT_TRUE(pose(p, ch).x > 6.5);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 1.355 + 0.5 + SKIN, 2e-3);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, top_down_moves_freely) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.plane = DE_PHYSICS2D_PLANE_TOP_DOWN;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    (void)static_body(p, tu_box(p, 0.5f, 5.0f, 0.0f), 3.0, 0.0, 0.0f); /* face at x = 2.5 */
    DeCharacter2D io = character();
    io.size = de__v2(0.6f, 0.6f);
    const de_body ch = spawn(p, &io, 0.0, 0.0, DE_CHARACTER2D_PROXY_CAPSULE);
    io.desired_velocity = de__v2(0.0f, 2.0f);
    for (u32 i = 0; i < 30; ++i)
        tick(p, ch, &io);
    EXPECT_FALSE(io.grounded);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 1.0, 1e-4);
    EXPECT_FLOAT_EQ(pose(p, ch).x, 0.0, 1e-9);
    /* Diagonal into the wall: slides along it. */
    io.desired_velocity = de__v2(3.0f, 3.0f);
    for (u32 i = 0; i < 60; ++i)
        tick(p, ch, &io);
    EXPECT_FLOAT_EQ(pose(p, ch).x, 2.5 - 0.3 - SKIN, 1e-4);
    EXPECT_TRUE(pose(p, ch).y > 3.5);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, stands_on_dynamic_body) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    (void)tu_body(p, DE_BODY_DYNAMIC, tu_box(p, 1.0f, 0.5f, 0.0f), tu_xf(0.0, 0.5, 0.0f), 0);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 3.0, DE_CHARACTER2D_PROXY_CAPSULE);
    for (u32 i = 0; i < 90; ++i)
        tick(p, ch, &io);
    EXPECT_TRUE(io.grounded);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 1.0 + 0.9 + SKIN, 1e-4);
    de_physics2d_destroy(p);
}

TEST(physics2d_character, depenetrates) {
    de_physics2d_world *p = make_world();
    (void)ground(p);
    DeCharacter2D io = character();
    const de_body ch = spawn(p, &io, 0.0, 0.9 - 0.2, DE_CHARACTER2D_PROXY_CAPSULE); /* 20 cm in */
    io.grounded = true;
    settle(p, ch, &io, 10);
    EXPECT_TRUE(io.grounded);
    EXPECT_FLOAT_EQ(pose(p, ch).y, 0.9 + SKIN, 1e-3);
    de_physics2d_destroy(p);
}

/* ---- Allocation ------------------------------------------------------------------------------ */

TEST(physics2d_character, no_allocation_per_move) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.max_bodies = 256;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    pc2d_course course;
    ASSERT_TRUE(pc2d_build(p, &course));
    DeCharacter2D io = pc2d_character();
    const de_body ch =
        de_character2d_create(p, &io, pc2d_start(), DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(ch));
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    for (u32 i = 0; i < 600; ++i) {
        ASSERT_TRUE(de_body2d_set_velocity(p, course.platform, pc2d_platform_velocity(i), 0.0f));
        io.desired_velocity = pc2d_input(i);
        tick(p, ch, &io);
        advance_kinematic(p, course.platform);
        de_ray_hit2d h;
        de_dpoint2d o = {0.0, 5.0};
        (void)de_physics2d_raycast_ex(p, o, de__v2(0.0f, -1.0f), 10.0f, NULL, &h);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    de_physics2d_destroy(p);
    const de_tracking_stats end = de_tracking_allocator_stats(&t);
    EXPECT_EQ(end.bytes_live, 0u);
}

/* ---- The platformer_character2d course ----------------------------------------------------- */

typedef struct course_run {
    f64 x[PC2D_TICKS], y[PC2D_TICKS];
    bool grounded[PC2D_TICKS];
    f64 worst_platform_drift; /* during the idle ticks */
    tu_golden lines[TU_GOLDEN_MAX];
    u32 n;
} course_run;

static void h_f64(tu_hash *s, f64 v) {
    u64 u;
    memcpy(&u, &v, sizeof u);
    tu_h_u32(s, (u32)u);
    tu_h_u32(s, (u32)(u >> 32));
}

static void add_line(course_run *r, const char *name, u64 v) {
    ASSERT_LT(r->n, (u32)TU_GOLDEN_MAX);
    snprintf(r->lines[r->n].name, sizeof r->lines[r->n].name, "%s", name);
    r->lines[r->n].value = v;
    ++r->n;
}

static void run_course(course_run *r) {
    memset(r, 0, sizeof *r);
    de_physics2d_world *p = make_world();
    pc2d_course course;
    ASSERT_TRUE(pc2d_build(p, &course));
    DeCharacter2D io = pc2d_character();
    const de_body ch =
        de_character2d_create(p, &io, pc2d_start(), DE_CHARACTER2D_PROXY_CAPSULE, DE_ENTITY_NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(ch));
    const f64 off_x = pc2d_start().x - PC2D_PLATFORM_X;
    tu_hash block = {0};
    tu_hash all = {0};
    for (u32 i = 0; i < PC2D_TICKS; ++i) {
        ASSERT_TRUE(de_body2d_set_velocity(p, course.platform, pc2d_platform_velocity(i), 0.0f));
        io.desired_velocity = pc2d_input(i);
        tick(p, ch, &io);
        advance_kinematic(p, course.platform);
        const de_transform2d t = pose(p, ch);
        r->x[i] = t.x;
        r->y[i] = t.y;
        r->grounded[i] = io.grounded;
        if (i < PC2D_IDLE_TICKS) {
            const f64 d = fabs(t.x - pose(p, course.platform).x - off_x);
            if (d > r->worst_platform_drift)
                r->worst_platform_drift = d;
        }
        tu_hash *hs[2] = {&block, &all};
        for (u32 k = 0; k < 2; ++k) {
            h_f64(hs[k], t.x);
            h_f64(hs[k], t.y);
            tu_h_v2(hs[k], io.velocity);
            tu_h_u32(hs[k], io.grounded ? 1u : 0u);
        }
        if ((i + 1u) % 100u == 0u) {
            char name[32];
            snprintf(name, sizeof name, "ticks%03u", i - 99u);
            add_line(r, name, block.h);
            block.h = 0;
        }
    }
    add_line(r, "all_ticks", all.h);
    de_physics2d_destroy(p);
}

static const char k_course_header[] =
    "# platformer_character2d_golden.txt: the platformer_character2d course\n"
    "# (tests/unit/physics2d/platformer_character2d_course.h) for 1,000 ticks at 60 Hz without a\n"
    "# solver step: XXH3-64 of the character's position (f64), velocity and grounded flag, per\n"
    "# block of 100 ticks and over every tick (DENG-258).\n"
    "# Generated by test_physics2d_character with DE_REGEN_GOLDEN=1; never edit by hand.\n"
    "# x86-64 and arm64 must reproduce every line.\n";

TEST(physics2d_character, platformer_character2d_golden) {
    static course_run r;
    run_course(&r);
    /* The course does what its header says. */
    EXPECT_TRUE(r.worst_platform_drift < 0.01); /* rides the moving platform */
    for (u32 i = 1; i < PC2D_IDLE_TICKS; ++i)
        EXPECT_TRUE(r.grounded[i]);
    bool on_one_way = false, on_plateau = false, at_slope = false;
    for (u32 i = PC2D_IDLE_TICKS; i < PC2D_TICKS; ++i) {
        if (r.grounded[i] && r.x[i] > 1.3 && r.x[i] < 2.7 &&
            fabs(r.y[i] - (1.2 + 0.9 + SKIN)) < 1e-3)
            on_one_way = true;
        if (r.grounded[i] && r.x[i] > 9.0 && r.x[i] < 11.0 &&
            fabs(r.y[i] - (1.5 + 0.9 + SKIN)) < 2e-3)
            on_plateau = true;
        if (r.x[i] > 20.0 - 0.3 - 0.05)
            at_slope = true;
        EXPECT_TRUE(r.x[i] < 20.0); /* never climbs the 60° slope */
    }
    EXPECT_TRUE(on_one_way);
    EXPECT_TRUE(on_plateau);
    EXPECT_TRUE(at_slope);
    /* Back left up the stairs: on the plateau at the end. */
    EXPECT_TRUE(r.x[PC2D_TICKS - 1u] < 11.5);
    EXPECT_FLOAT_EQ(r.y[PC2D_TICKS - 1u], 1.5 + 0.9 + SKIN, 2e-3);
    EXPECT_TRUE(r.grounded[PC2D_TICKS - 1u]);
    if (tu_golden_check(DE_PHYSICS2D_CHARACTER_GOLDEN, k_course_header, r.lines, r.n))
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS2D_CHARACTER_GOLDEN);
}

TEST(physics2d_character, course_runs_identically) {
    static course_run a, b;
    run_course(&a);
    run_course(&b);
    ASSERT_EQ(a.n, b.n);
    for (u32 i = 0; i < a.n; ++i)
        EXPECT_EQ(a.lines[i].value, b.lines[i].value);
}
