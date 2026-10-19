/*
 * platformer_character2d_course.h: the `platformer_character2d` course (DENG-258), defined once
 * for the unit golden of test_character2d.c and the ECS determinism scenario of DENG-260.
 *
 * A side-view level (gravity (0, -9.81), 60 Hz, PC2D_TICKS ticks) for one capsule character
 * (PC2D_SIZE, default slope and step height):
 *
 *   x:  -10        -8.5 -5.5         1    3       6      8.6     11.6   14.6       20  21.15   25
 *        |  [moving platform]         [one-way]        /‾‾‾‾‾‾‾‾‾‾‾\_             /‾‾‾‾‾‾‾|
 *        |____________________________________________/    30°      stairs \_____/ 60°
 *
 * - Terrain: one static open chain (PC2D_TERRAIN, listed left to right and created right to left
 *   so every segment collides from above / from the open side): a wall at x = -10, flat ground
 *   at y = 0, a 30° ramp up to y = 1.5, a plateau, six 0.25 m stairs down to y = 0, flat ground,
 *   a 60° slope up to y = 2 and a wall at x = 25.
 * - One-way platform: a static two-point chain from (3, 1.2) to (1, 1.2) (normal up).
 * - Moving platform: a kinematic 3 x 0.2 m box centered at PC2D_PLATFORM_START, moving along x
 *   at pc2d_platform_velocity(tick) (+-0.5 m/s, reversing every 120 ticks).
 * - Character: starts standing on the moving platform (PC2D_START), idles for 2.5 s (ticks
 *   0..149: the platform carries it, reversing at tick 120), then follows pc2d_input(tick): walks
 *   right off the platform onto the ground and under the one-way platform, stops, jumps straight
 *   up through it and lands on it, walks off it, up the ramp, over the plateau, down the stairs,
 *   into the 60° slope (blocked from about tick 785), then back left up the stairs onto the
 *   plateau.
 *
 * Tick order (both users): set the platform's velocity for the tick, move the character
 * (de_character2d_move, grounded starts true), then step. Without a solver step (the unit test),
 * the proxy is set to the move's predicted pose and the platform to de__p2d_predict_kinematic of
 * its velocity, which is what the step's kinematic integration does.
 *
 * Everything here is plain data and static inline functions of the public API.
 */
#ifndef DE_TEST_PLATFORMER_CHARACTER2D_COURSE_H
#define DE_TEST_PLATFORMER_CHARACTER2D_COURSE_H

#include <dengine/de_physics.h>

#include <string.h>

/** Ticks of the course and the fixed step. */
#define PC2D_TICKS 1000u
#define PC2D_DT    (1.0f / 60.0f)
/** Idle ticks on the moving platform (2.5 s; the platform reverses at tick 120). */
#define PC2D_IDLE_TICKS 150u

/** Character size (capsule, width x height) and walking speed. */
#define PC2D_WIDTH      0.6f
#define PC2D_HEIGHT     1.8f
#define PC2D_WALK_SPEED 3.0f
#define PC2D_JUMP_SPEED 6.0f

/** Terrain points, left to right (created reversed). */
static const de_vec2 PC2D_TERRAIN[] = {
    {-10.0f, 6.0f},     {-10.0f, 0.0f},      {6.0f, 0.0f},        {8.598076f, 1.5f},
    {11.598076f, 1.5f}, {11.598076f, 1.25f}, {12.098076f, 1.25f}, {12.098076f, 1.0f},
    {12.598076f, 1.0f}, {12.598076f, 0.75f}, {13.098076f, 0.75f}, {13.098076f, 0.5f},
    {13.598076f, 0.5f}, {13.598076f, 0.25f}, {14.098076f, 0.25f}, {14.098076f, 0.0f},
    {20.0f, 0.0f},      {21.154701f, 2.0f},  {25.0f, 2.0f},       {25.0f, 6.0f},
};
#define PC2D_TERRAIN_COUNT ((u32)(sizeof PC2D_TERRAIN / sizeof PC2D_TERRAIN[0]))

/** One-way platform, left to right (created reversed: normal up). */
static const de_vec2 PC2D_ONE_WAY[2] = {{1.0f, 1.2f}, {3.0f, 1.2f}};

/** Moving platform: half extents and start center. */
#define PC2D_PLATFORM_HX 1.5f
#define PC2D_PLATFORM_HY 0.1f
#define PC2D_PLATFORM_X  (-7.0)
#define PC2D_PLATFORM_Y  0.9

/** The character's start: on the platform, a skin above its top. */
static inline de_dpoint2d pc2d_start(void) {
    de_dpoint2d s;
    s.x = PC2D_PLATFORM_X;
    s.y = PC2D_PLATFORM_Y + (f64)PC2D_PLATFORM_HY + 0.5 * (f64)PC2D_HEIGHT +
          (f64)DE_CHARACTER2D_SKIN_WIDTH;
    return s;
}

/** The platform's velocity during `tick`: +0.5 m/s along x for 120 ticks, then -0.5, ... */
static inline de_vec2 pc2d_platform_velocity(u32 tick) {
    de_vec2 v;
    v.x = ((tick / 120u) % 2u) == 0u ? 0.5f : -0.5f;
    v.y = 0.0f;
    return v;
}

/** One stretch of the input script: from `first` (inclusive) until the next entry. */
typedef struct pc2d_input_span {
    u32 first;
    f32 vx;
    f32 jump; /* desired_velocity.y */
} pc2d_input_span;

static const pc2d_input_span PC2D_SCRIPT[] = {
    {0u, 0.0f, 0.0f},                         /* idle on the moving platform */
    {PC2D_IDLE_TICKS, PC2D_WALK_SPEED, 0.0f}, /* walk right, off the platform, under the one-way */
    {312u, 0.0f, 0.0f},                       /* stop under the one-way platform (x = 1.6) */
    {320u, 0.0f, PC2D_JUMP_SPEED},            /* jump straight up through it */
    {326u, 0.0f, 0.0f},                       /* land on it */
    {400u, PC2D_WALK_SPEED, 0.0f},            /* walk off it, up the ramp, down the stairs */
    {800u, -PC2D_WALK_SPEED, 0.0f},           /* blocked by the 60° slope since ~785; back left,
                                                 up the stairs onto the plateau */
};
#define PC2D_SCRIPT_COUNT ((u32)(sizeof PC2D_SCRIPT / sizeof PC2D_SCRIPT[0]))

/** desired_velocity for `tick`. */
static inline de_vec2 pc2d_input(u32 tick) {
    u32 k = 0;
    for (u32 i = 0; i < PC2D_SCRIPT_COUNT; ++i)
        if (PC2D_SCRIPT[i].first <= tick)
            k = i;
    de_vec2 v;
    v.x = PC2D_SCRIPT[k].vx;
    v.y = PC2D_SCRIPT[k].jump;
    return v;
}

/** The DeCharacter2D the course starts with. */
static inline DeCharacter2D pc2d_character(void) {
    DeCharacter2D c;
    memset(&c, 0, sizeof c);
    c.size.x = PC2D_WIDTH;
    c.size.y = PC2D_HEIGHT;
    c.grounded = true;
    return c;
}

/** Bodies of a built course. */
typedef struct pc2d_course {
    de_shape terrain_shape, one_way_shape, platform_shape;
    de_body terrain, one_way, platform;
} pc2d_course;

/** Builds the static terrain, the one-way platform and the moving platform (body API). Returns
 *  false when a shape or body could not be made. */
static inline bool pc2d_build(de_physics2d_world *p, pc2d_course *c) {
    memset(c, 0, sizeof *c);
    de_vec2 rev[PC2D_TERRAIN_COUNT];
    for (u32 i = 0; i < PC2D_TERRAIN_COUNT; ++i)
        rev[i] = PC2D_TERRAIN[PC2D_TERRAIN_COUNT - 1u - i];
    c->terrain_shape = de_shape2d_chain_create(p, rev, PC2D_TERRAIN_COUNT, false);
    const de_vec2 ow[2] = {PC2D_ONE_WAY[1], PC2D_ONE_WAY[0]};
    c->one_way_shape = de_shape2d_chain_create(p, ow, 2, false);
    de_shape2d_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.kind = DE_SHAPE2D_BOX;
    bd.half_extents.x = PC2D_PLATFORM_HX;
    bd.half_extents.y = PC2D_PLATFORM_HY;
    c->platform_shape = de_shape2d_create(p, &bd);
    if (!DE_HANDLE_VALID(c->terrain_shape) || !DE_HANDLE_VALID(c->one_way_shape) ||
        !DE_HANDLE_VALID(c->platform_shape))
        return false;
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_STATIC;
    d.transform.rotation.c = 1.0f;
    d.shape = c->terrain_shape;
    c->terrain = de_body2d_create(p, &d);
    d.shape = c->one_way_shape;
    c->one_way = de_body2d_create(p, &d);
    d.type = DE_BODY_KINEMATIC;
    d.shape = c->platform_shape;
    d.transform.x = PC2D_PLATFORM_X;
    d.transform.y = PC2D_PLATFORM_Y;
    d.linear_velocity = pc2d_platform_velocity(0);
    c->platform = de_body2d_create(p, &d);
    return DE_HANDLE_VALID(c->terrain) && DE_HANDLE_VALID(c->one_way) &&
           DE_HANDLE_VALID(c->platform);
}

#endif /* DE_TEST_PLATFORMER_CHARACTER2D_COURSE_H */
