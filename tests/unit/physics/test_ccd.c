/* test_ccd.c: continuous collision (DENG-219, SPEC §13.1, §13.2, §5.7): predictive broadphase
 * boxes, the per-pair speculative distance, and the swept time of impact of bullets
 * (DE_BODY_FLAG_CCD). See src/physics/ccd_internal.h. */
#include "solver_test_util.h"

#include "physics/ccd_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_task.h>

#include <stdio.h>
#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------------------------ */

/* A small deterministic generator (xorshift64*), uniform in [0, 1). */
static f64 rnd(u64 *s) {
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return (f64)((x * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
}

static de_body make_body(de_physics_world *p, de_body_type type, de_shape s, de_transform t,
                         f32 friction, f32 restitution, f32 gravity_scale, u32 flags) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.gravity_scale = gravity_scale;
    d.material.friction = friction;
    d.material.restitution = restitution;
    d.flags = flags;
    return de_body_create(p, &d);
}

static de__ccd *ccd_state(de_physics_world *p) {
    de__ccd *c = NULL;
    if (de__ccd_ensure(p, &c) != DE_OK)
        return NULL;
    return c;
}

/* ---- bullets ------------------------------------------------------------------------------ */

/* A 1 cm sphere (radius 5 mm) shot at 500 m/s at a 1 cm static wall (40 m x 40 m, the plane
 * x = 0) from 5 cm to 6 m away, at `angles` random angles of up to 70 degrees from the wall's
 * normal and random places. Every shot is stepped 4 times (it reaches the wall within 2.1 steps).
 * Returns how many shots passed through (center beyond the wall's back face at any step) and,
 * in *reached, how many ended at the wall. */
static u32 shoot_wall(u32 angles, bool ccd, u32 *reached, u32 *hits) {
    de_physics_world *p = st_world(8, NULL, NULL);
    if (p == NULL)
        return angles;
    de__solver *s = st_solver(p);
    s->settings.max_linear_speed = 1000.0f; /* 500 m/s is above the 400 m/s default clamp */
    const de_shape wall = st_box(p, 0.005f, 20.0f, 20.0f);
    const de_shape ball = st_sphere(p, 0.005f);
    (void)make_body(p, DE_BODY_STATIC, wall, st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.0f,
                    0.0f, 0.0f, 0u);
    const de_body b =
        make_body(p, DE_BODY_DYNAMIC, ball, st_xform(-1.0, 0.0, 0.0, st_quat_identity()), 0.0f,
                  0.0f, 1.0f, ccd ? (u32)DE_BODY_FLAG_CCD : 0u);
    u64 seed = 0x0CCD219ull;
    u32 through = 0;
    *reached = 0;
    *hits = 0;
    for (u32 k = 0; k < angles; ++k) {
        const f32 theta = (f32)(rnd(&seed) * 70.0 * 3.14159265358979 / 180.0);
        const f32 phi = (f32)(rnd(&seed) * 2.0 * 3.14159265358979);
        const f64 gap = 0.05 + rnd(&seed) * 5.95;
        const f64 y = rnd(&seed) * 4.0 - 2.0, z = rnd(&seed) * 4.0 - 2.0;
        f32 st, ct, sp, cp;
        de_dsincos(theta, &st, &ct);
        de_dsincos(phi, &sp, &cp);
        const de_vec3 v = {500.0f * ct, 500.0f * st * cp, 500.0f * st * sp};
        const de_transform at = st_xform(-0.005 - 0.005 - gap, y, z, st_quat_identity());
        ASSERT_TRUE(de_body_set_transform(p, b, &at));
        ASSERT_TRUE(de_body_set_velocity(p, b, v, (de_vec3){0.0f, 0.0f, 0.0f}));
        bool passed = false;
        for (u32 step = 0; step < 4; ++step) {
            de_physics_step(p, ST_DT);
            de__ccd_stats cs;
            de__ccd_stats_get(p, &cs);
            *hits += cs.hits;
            if (st_pos(p, b).x > 0.005)
                passed = true;
        }
        through += passed ? 1u : 0u;
        /* At the wall: the sphere's surface within 2 cm of the front face (x = -0.005). */
        if (!passed && st_pos(p, b).x > -0.005 - 0.005 - 0.02)
            ++*reached;
    }
    de_physics_destroy(p);
    return through;
}

TEST(physics_ccd, bullet_vs_thin_wall) {
    /* 1,000 shots x 4 steps of a two-body world: about 30 ms natively (Dev), well within the
     * TSan Debug budget (the step cost is the same per shot). */
    u32 reached = 0, hits = 0;
    const u32 through = shoot_wall(1000u, true, &reached, &hits);
    printf("  ccd: %u of 1000 shots passed through, %u stopped at the wall, %u TOI clamps\n",
           through, reached, hits);
    EXPECT_EQ(through, 0u);
    EXPECT_EQ(reached, 1000u);
    EXPECT_GT(hits, 0u); /* the far shots needed the time of impact, not only speculation */
}

TEST(physics_ccd, no_ccd_tunnels_documented) {
    /* Without DE_BODY_FLAG_CCD the 500 m/s sphere moves 8.3 m per step. Speculative contacts
     * reach at most physics.ccd.speculative_max (1 m), so the wall stops only the shots that
     * start a step within 1 m of it (and within the approach distance of the step); the rest
     * tunnel. This documents the limit that bullets remove. */
    u32 reached = 0, hits = 0;
    const u32 through = shoot_wall(200u, false, &reached, &hits);
    printf("  no ccd: %u of 200 shots passed through, %u stopped at the wall\n", through, reached);
    EXPECT_EQ(hits, 0u);
    EXPECT_GT(through, 100u);
    EXPECT_GT(reached, 0u); /* those within 1 m of the wall were stopped by speculation */
    EXPECT_EQ(through + reached, 200u);
}

TEST(physics_ccd, bullet_vs_mesh_heightfield_and_compound) {
    /* A spinning 2 cm box bullet at 300 m/s at a mesh plane, a heightfield and a compound of two
     * 1 cm plates: it stops at each and never gets through. */
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    st_solver(p)->settings.max_linear_speed = 1000.0f;
    /* A 20 m x 20 m two-triangle mesh in the plane x = 10 (normal -X). */
    const de_vec3 verts[4] = {{10.0f, -10.0f, -10.0f},
                              {10.0f, 10.0f, -10.0f},
                              {10.0f, 10.0f, 10.0f},
                              {10.0f, -10.0f, 10.0f}};
    const u32 idx[6] = {0, 2, 1, 0, 3, 2};
    de_shape_desc md;
    memset(&md, 0, sizeof md);
    md.kind = DE_SHAPE_MESH;
    md.vertices = verts;
    md.vertex_count = 4;
    md.indices = idx;
    md.index_count = 6;
    const de_shape mesh = de_shape_create(p, &md);
    ASSERT_TRUE(DE_HANDLE_VALID(mesh));
    (void)make_body(p, DE_BODY_STATIC, mesh, st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.0f,
                    0.0f, 0.0f, 0u);
    const de_shape cube = st_box(p, 0.01f, 0.01f, 0.01f);
    const de_body b =
        make_body(p, DE_BODY_DYNAMIC, cube, st_xform(0.0, 3.0, 2.0, st_quat_identity()), 0.0f, 0.0f,
                  0.0f, DE_BODY_FLAG_CCD);
    ASSERT_TRUE(
        de_body_set_velocity(p, b, (de_vec3){300.0f, 0.0f, 0.0f}, (de_vec3){40.0f, 25.0f, 10.0f}));
    f64 max_x = -1e9;
    for (u32 i = 0; i < 6; ++i) {
        de_physics_step(p, ST_DT);
        max_x = de__maxd(max_x, st_pos(p, b).x);
    }
    EXPECT_TRUE(max_x < 10.0); /* never through the plane */
    EXPECT_TRUE(max_x > 9.95); /* it got there */
    de_physics_destroy(p);

    /* Heightfield 8 x 8 cells of 1 m, flat at z = 0; the bullet falls at 300 m/s. */
    p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    st_solver(p)->settings.max_linear_speed = 1000.0f;
    f32 heights[81];
    for (u32 i = 0; i < 81; ++i)
        heights[i] = 0.0f;
    de_shape_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE_HEIGHTFIELD;
    hd.heights = heights;
    hd.cols = 9;
    hd.rows = 9;
    hd.scale = (de_vec3){1.0f, 1.0f, 1.0f};
    const de_shape hf = de_shape_create(p, &hd);
    ASSERT_TRUE(DE_HANDLE_VALID(hf));
    (void)make_body(p, DE_BODY_STATIC, hf, st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.0f, 0.0f,
                    0.0f, 0u);
    const de_shape cube2 = st_box(p, 0.01f, 0.01f, 0.01f);
    const de_body c =
        make_body(p, DE_BODY_DYNAMIC, cube2, st_xform(4.3, 4.6, 20.0, st_quat_identity()), 0.0f,
                  0.0f, 1.0f, DE_BODY_FLAG_CCD);
    ASSERT_TRUE(
        de_body_set_velocity(p, c, (de_vec3){5.0f, 0.0f, -300.0f}, (de_vec3){30.0f, 0.0f, 20.0f}));
    f64 min_z = 1e9;
    for (u32 i = 0; i < 8; ++i) {
        de_physics_step(p, ST_DT);
        min_z = de__mind(min_z, st_pos(p, c).z);
    }
    EXPECT_TRUE(min_z > 0.0);
    EXPECT_TRUE(min_z < 0.05);
    de_physics_destroy(p);

    /* A compound of two thin plates (1 cm) at x = 5 and x = 5.5; a compound bullet of two
     * spheres at 400 m/s. */
    p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    st_solver(p)->settings.max_linear_speed = 1000.0f;
    const de_shape plate = st_box(p, 0.005f, 5.0f, 5.0f);
    const de_shape kids[2] = {plate, plate};
    const de_transform kid_xf[2] = {st_xform(5.5, 0.0, 0.0, st_quat_identity()),
                                    st_xform(5.0, 0.0, 0.0, st_quat_identity())};
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kid_xf;
    cd.child_count = 2;
    const de_shape plates = de_shape_create(p, &cd);
    ASSERT_TRUE(DE_HANDLE_VALID(plates));
    (void)make_body(p, DE_BODY_STATIC, plates, st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.0f,
                    0.0f, 0.0f, 0u);
    const de_shape pellet = st_sphere(p, 0.004f);
    const de_shape two[2] = {pellet, pellet};
    const de_transform two_xf[2] = {st_xform(0.0, 0.01, 0.0, st_quat_identity()),
                                    st_xform(0.0, -0.01, 0.0, st_quat_identity())};
    cd.children = two;
    cd.child_xforms = two_xf;
    const de_shape dumbbell = de_shape_create(p, &cd);
    ASSERT_TRUE(DE_HANDLE_VALID(dumbbell));
    const de_body d =
        make_body(p, DE_BODY_DYNAMIC, dumbbell, st_xform(-3.0, 0.3, 0.2, st_quat_identity()), 0.0f,
                  0.0f, 0.0f, DE_BODY_FLAG_CCD);
    ASSERT_TRUE(
        de_body_set_velocity(p, d, (de_vec3){400.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 90.0f}));
    max_x = -1e9;
    for (u32 i = 0; i < 6; ++i) {
        de_physics_step(p, ST_DT);
        max_x = de__maxd(max_x, st_pos(p, d).x);
    }
    EXPECT_TRUE(max_x < 5.0 - 0.005); /* stopped by the first plate */
    EXPECT_TRUE(max_x > 4.95);
    de_physics_destroy(p);
}

TEST(physics_ccd, resting_bullet_does_not_pause) {
    /* A bullet ball rolling on the ground and a bullet box sliding and spinning on it: the ground
     * is within the slop at the start of every step, so the sweep falls back to the small sphere
     * at the center of mass, which never touches the ground; neither stops. */
    de_physics_world *p = st_world(8, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape ground = st_box(p, 50.0f, 50.0f, 0.5f);
    (void)make_body(p, DE_BODY_STATIC, ground, st_xform(0.0, 0.0, -0.5, st_quat_identity()), 0.5f,
                    0.0f, 0.0f, 0u);
    const de_shape ball_shape = st_sphere(p, 0.25f);
    const de_body ball =
        make_body(p, DE_BODY_DYNAMIC, ball_shape, st_xform(-20.0, 0.0, 0.25, st_quat_identity()),
                  0.5f, 0.0f, 1.0f, DE_BODY_FLAG_CCD);
    ASSERT_TRUE(de_body_set_velocity(p, ball, (de_vec3){6.0f, 0.0f, 0.0f},
                                     (de_vec3){0.0f, 6.0f / 0.25f, 0.0f}));
    const de_shape cube = st_box(p, 0.3f, 0.3f, 0.3f);
    const de_body box =
        make_body(p, DE_BODY_DYNAMIC, cube, st_xform(-20.0, 5.0, 0.3, st_quat_identity()), 0.0f,
                  0.0f, 1.0f, DE_BODY_FLAG_CCD);
    ASSERT_TRUE(
        de_body_set_velocity(p, box, (de_vec3){8.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 6.0f}));
    u32 hits = 0, fallbacks = 0;
    for (u32 i = 0; i < 60; ++i) {
        de_physics_step(p, ST_DT);
        de__ccd_stats cs;
        de__ccd_stats_get(p, &cs);
        hits += cs.hits;
        fallbacks += cs.fallbacks;
    }
    printf("  resting bullets: %u TOI clamps, %u fallbacks in 60 steps\n", hits, fallbacks);
    EXPECT_EQ(hits, 0u);
    EXPECT_GT(fallbacks, 0u);
    /* The ball rolled about 6 m, the frictionless box slid 8 m. */
    EXPECT_FLOAT_EQ((f32)st_pos(p, ball).x, -14.0f, 0.1f);
    EXPECT_FLOAT_EQ((f32)st_pos(p, box).x, -12.0f, 0.1f);
    EXPECT_FLOAT_EQ((f32)st_pos(p, box).z, 0.3f, 0.02f);
    de_physics_destroy(p);
}

TEST(physics_ccd, ccd_cvars_and_settings) {
    de_physics_world *p = st_world(8, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de__ccd *c = ccd_state(p);
    ASSERT_TRUE(c != NULL);
    EXPECT_TRUE(c->settings.enabled);
    EXPECT_FLOAT_EQ(c->settings.speculative_max, DE__CCD_DEFAULT_SPECULATIVE_MAX, 0.0f);
    EXPECT_FLOAT_EQ(c->settings.predict_max, DE__CCD_DEFAULT_PREDICT_MAX, 0.0f);
    EXPECT_TRUE(DE_HANDLE_VALID(de_cvar_find(DE__CCD_ENABLED_CVAR)));
    EXPECT_TRUE(DE_HANDLE_VALID(de_cvar_find(DE__CCD_SPECULATIVE_MAX_CVAR)));
    EXPECT_TRUE(DE_HANDLE_VALID(de_cvar_find(DE__CCD_PREDICT_MAX_CVAR)));
    /* Disabled: the bullet pass does nothing. */
    c->settings.enabled = false;
    st_solver(p)->settings.max_linear_speed = 1000.0f;
    const de_shape wall = st_box(p, 0.005f, 20.0f, 20.0f);
    const de_shape ball = st_sphere(p, 0.005f);
    (void)make_body(p, DE_BODY_STATIC, wall, st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.0f,
                    0.0f, 0.0f, 0u);
    const de_body b =
        make_body(p, DE_BODY_DYNAMIC, ball, st_xform(-5.0, 0.0, 0.0, st_quat_identity()), 0.0f,
                  0.0f, 0.0f, DE_BODY_FLAG_CCD);
    ASSERT_TRUE(
        de_body_set_velocity(p, b, (de_vec3){500.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f}));
    de_physics_step(p, ST_DT);
    EXPECT_TRUE(st_pos(p, b).x > 3.0); /* tunnelled */
    de__ccd_stats cs;
    de__ccd_stats_get(p, &cs);
    EXPECT_EQ(cs.bullets, 0u);
    de_physics_destroy(p);
    de__ccd_stats_get(NULL, &cs);
    EXPECT_EQ(cs.hits, 0u);
}

/* ---- speculative contacts ----------------------------------------------------------------- */

/* Energy per kg of a unit cube (isotropic inertia m/6): translation, rotation, height of the
 * center of mass. */
static f64 cube_energy(const de_physics_world *p, de_body b) {
    const de_vec3 v = st_vel(p, b), w = st_angvel(p, b);
    return 0.5 * (f64)de__v3_dot(v, v) + 0.5 / 6.0 * (f64)de__v3_dot(w, w) + 9.81 * st_pos(p, b).z;
}

typedef struct drop_result {
    f64 min_bottom;   /* lowest center height - 0.5 over every drop and tick */
    f64 max_ratio;    /* largest upward speed after the impact / impact speed */
    f64 max_gain;     /* largest energy gain over the energy just before the impact (J/kg) */
    f64 max_rest_err; /* largest |center height - 0.5| at the end */
} drop_result;

/* A unit box dropped at 50 m/s (0.83 m per step) onto a 20 cm static slab, without CCD, from
 * `drops` start heights spread over one step of travel, 90 steps each. */
static void fast_drop(f32 restitution, u32 drops, drop_result *out) {
    de_physics_world *p = st_world(8, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape slab = st_box(p, 10.0f, 10.0f, 0.1f);
    (void)make_body(p, DE_BODY_STATIC, slab, st_xform(0.0, 0.0, -0.1, st_quat_identity()), 0.5f,
                    0.0f, 0.0f, 0u);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_body b =
        make_body(p, DE_BODY_DYNAMIC, cube, st_xform(0.0, 0.0, 3.0, st_quat_identity()), 0.5f,
                  restitution, 1.0f, 0u);
    out->min_bottom = 1e9;
    out->max_ratio = 0.0;
    out->max_gain = -1e9;
    out->max_rest_err = 0.0;
    for (u32 k = 0; k < drops; ++k) {
        const f64 h0 = 1.5 + 0.8333 * (f64)k / (f64)drops;
        const de_transform at = st_xform(0.0, 0.0, 0.5 + h0, st_quat_identity());
        ASSERT_TRUE(de_body_set_transform(p, b, &at));
        ASSERT_TRUE(
            de_body_set_velocity(p, b, (de_vec3){0.0f, 0.0f, -50.0f}, (de_vec3){0.0f, 0.0f, 0.0f}));
        bool landed = false;
        f64 e_impact = 0.0, v_impact = 0.0;
        for (u32 i = 0; i < 90; ++i) {
            const f64 e0 = cube_energy(p, b);
            const f64 vz0 = (f64)st_vel(p, b).z;
            de_physics_step(p, ST_DT);
            const f64 bottom = st_pos(p, b).z - 0.5;
            out->min_bottom = de__mind(out->min_bottom, bottom);
            if (!landed && bottom < 0.05) {
                landed = true;
                e_impact = e0;
                v_impact = -vz0;
            }
            if (landed) {
                out->max_ratio = de__maxd(out->max_ratio, (f64)st_vel(p, b).z / v_impact);
                out->max_gain = de__maxd(out->max_gain, cube_energy(p, b) - e_impact);
            }
        }
        out->max_rest_err = de__maxd(out->max_rest_err, de__absd(st_pos(p, b).z - 0.5));
    }
    de_physics_destroy(p);
}

TEST(physics_ccd, fast_drop_speculative) {
    /* e = 0: the box stops on the slab. Four speculative points solved one after another give
     * the flat impact a small spin (the box rocks onto an edge and back, a few cm/s of upward
     * speed); it never bounces, and never gains energy. */
    drop_result r;
    fast_drop(0.0f, 12, &r);
    printf("  fast drop e=0: lowest bottom %.4f m, largest upward speed / impact speed %.3f, "
           "largest energy gain %.3f J/kg (of 1250), rest error %.4f m\n",
           r.min_bottom, r.max_ratio, r.max_gain, r.max_rest_err);
    EXPECT_TRUE(r.min_bottom > -0.02); /* no tunnelling: soft contacts sink a few mm at most */
    EXPECT_TRUE(r.max_ratio < 0.04);   /* no bounce */
    EXPECT_TRUE(r.max_gain < 0.5);
    EXPECT_TRUE(r.max_rest_err < 0.01);

    /* e = 0.5: the rebound speed is half the impact speed, and no energy is gained. */
    fast_drop(0.5f, 12, &r);
    printf("  fast drop e=0.5: lowest bottom %.4f m, largest rebound / impact speed %.3f, "
           "largest energy gain %.3f J/kg\n",
           r.min_bottom, r.max_ratio, r.max_gain);
    EXPECT_TRUE(r.min_bottom > -0.02);
    EXPECT_TRUE(r.max_ratio < 0.5 * 1.03);
    EXPECT_TRUE(r.max_ratio > 0.5 * 0.9);
    EXPECT_TRUE(r.max_gain < 0.5);
}

TEST(physics_ccd, no_ghost_collision) {
    /* A sphere rolls along +X past a static box whose side face is 2 cm from its path (the box's
     * two vertical edges and its side face come within the speculative distance). The
     * speculative contacts never push it: it keeps its line and speed. */
    const f32 speeds[4] = {2.0f, 5.0f, 10.0f, 20.0f};
    for (u32 k = 0; k < 4; ++k) {
        const f32 v0 = speeds[k];
        de_physics_world *p = st_world(8, NULL, NULL);
        ASSERT_TRUE(p != NULL);
        const de_shape ground = st_box(p, 60.0f, 5.0f, 0.5f);
        (void)make_body(p, DE_BODY_STATIC, ground, st_xform(0.0, 0.0, -0.5, st_quat_identity()),
                        0.5f, 0.0f, 0.0f, 0u);
        const de_shape block = st_box(p, 1.0f, 0.5f, 0.5f);
        const de_body wall = make_body(p, DE_BODY_STATIC, block,
                                       st_xform(0.0, 0.25 + 0.02 + 0.5, 0.5, st_quat_identity()),
                                       0.5f, 0.0f, 0.0f, 0u);
        const de_shape ball_shape = st_sphere(p, 0.25f);
        const de_body ball =
            make_body(p, DE_BODY_DYNAMIC, ball_shape, st_xform(-4.0, 0.0, 0.25, st_quat_identity()),
                      0.5f, 0.0f, 1.0f, 0u);
        ASSERT_TRUE(de_body_set_velocity(p, ball, (de_vec3){v0, 0.0f, 0.0f},
                                         (de_vec3){0.0f, v0 / 0.25f, 0.0f}));
        const u32 steps = (u32)(8.0f / (v0 * ST_DT)) + 1u;
        f64 max_dy = 0.0;
        f32 min_vx = v0, max_vy = 0.0f;
        u32 near_ticks = 0;
        const u32 bi = de__handle_index(ball.id), wi = de__handle_index(wall.id);
        for (u32 i = 0; i < steps; ++i) {
            de_physics_step(p, ST_DT);
            const de_vec3 v = st_vel(p, ball);
            max_dy = de__maxd(max_dy, de__absd(st_pos(p, ball).y));
            min_vx = de__minf(min_vx, v.x);
            max_vy = de__maxf(max_vy, de__absf(v.y));
            de__contact_key key;
            memset(&key, 0, sizeof key);
            key.a = bi < wi ? bi : wi;
            key.b = bi < wi ? wi : bi;
            key.child_a = key.sub_a = key.child_b = key.sub_b = DE__SUB_NONE;
            if (de__contacts_find(p, &key) != NULL)
                ++near_ticks;
        }
        printf("  rolling past at %.0f m/s: |dy| <= %.2e m, |vy| <= %.2e m/s, vx >= %.4f m/s, "
               "%u ticks with a speculative contact on the box\n",
               (double)v0, max_dy, (double)max_vy, (double)min_vx, near_ticks);
        EXPECT_GT(near_ticks, 0u); /* the speculative contact existed */
        EXPECT_TRUE(max_dy < 1e-3);
        EXPECT_TRUE(max_vy < 0.01f * v0);
        EXPECT_TRUE(min_vx > 0.97f * v0);
        EXPECT_TRUE(st_pos(p, ball).x > 3.0);
        de_physics_destroy(p);
    }
}

/* ---- predictive broadphase boxes ---------------------------------------------------------- */

TEST(physics_ccd, predictive_aabb_pairs) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_shape ball = st_sphere(p, 0.25f);
    const de_body box = make_body(
        p, DE_BODY_STATIC, cube, st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.5f, 0.0f, 0.0f, 0u);
    /* 0.9 m from the box, at 60 m/s (1 m per step). */
    const de_body fast =
        make_body(p, DE_BODY_DYNAMIC, ball, st_xform(-1.65, 0.0, 0.0, st_quat_identity()), 0.5f,
                  0.0f, 0.0f, 0u);
    ASSERT_TRUE(
        de_body_set_velocity(p, fast, (de_vec3){60.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f}));
    /* A field of movers with random velocities. */
    u64 seed = 0xB0A7ull;
    for (u32 i = 0; i < 100; ++i) {
        const de_body m =
            make_body(p, i % 4u == 0u ? DE_BODY_STATIC : DE_BODY_DYNAMIC, i % 2u ? ball : cube,
                      st_xform(5.0 + rnd(&seed) * 12.0, rnd(&seed) * 12.0 - 6.0, rnd(&seed) * 6.0,
                               st_quat_identity()),
                      0.5f, 0.0f, 0.0f, 0u);
        ASSERT_TRUE(DE_HANDLE_VALID(m));
        (void)de_body_set_velocity(
            p, m,
            (de_vec3){(f32)(rnd(&seed) * 80.0 - 40.0), (f32)(rnd(&seed) * 80.0 - 40.0),
                      (f32)(rnd(&seed) * 20.0 - 10.0)},
            (de_vec3){(f32)(rnd(&seed) * 20.0 - 10.0), 0.0f, (f32)(rnd(&seed) * 8.0)});
    }
    de__ccd *c = ccd_state(p);
    ASSERT_TRUE(c != NULL);
    const u32 bi = de__handle_index(box.id), fi = de__handle_index(fast.id);
    const f32 d = de__contacts_speculative(p);

    /* Pairs without growth, then with it. */
    for (u32 pass = 0; pass < 2; ++pass) {
        c->settings.predict_max = pass == 0 ? 0.0f : DE__CCD_DEFAULT_PREDICT_MAX;
        de__ccd_prepare(p, ST_DT);
        de__broadphase_set_pair_distance(p, d);
        ASSERT_EQ(de__broadphase_update(p), DE_OK);
        const de__bp_pair *pairs = NULL;
        u32 n = 0;
        de__broadphase_pairs(p, &pairs, &n);
        /* Sorted, unique, and exactly the brute-force set over the predictive boxes. */
        u32 brute = 0;
        bool has_fast = false;
        for (u32 a = 0; a < p->bodies.high_water; ++a)
            for (u32 b = a + 1; b < p->bodies.high_water; ++b) {
                if (!de__bp_should_collide(p, a, b))
                    continue;
                const de_aabb ba = de__broadphase_predict_box(p, a);
                const de_aabb bb = de__broadphase_predict_box(p, b);
                if (ba.min.x > bb.max.x + d || bb.min.x > ba.max.x + d || ba.min.y > bb.max.y + d ||
                    bb.min.y > ba.max.y + d || ba.min.z > bb.max.z + d || bb.min.z > ba.max.z + d)
                    continue;
                ASSERT_TRUE(brute < n);
                EXPECT_EQ(pairs[brute].a, a);
                EXPECT_EQ(pairs[brute].b, b);
                ++brute;
            }
        EXPECT_EQ(n, brute);
        for (u32 k = 0; k < n; ++k)
            if (pairs[k].a == (bi < fi ? bi : fi) && pairs[k].b == (bi < fi ? fi : bi))
                has_fast = true;
        de__ccd_stats cs;
        de__ccd_stats_get(p, &cs);
        printf("  predict_max %.1f: %u pairs, %u bodies grown\n", (double)c->settings.predict_max,
               n, cs.predicted);
        EXPECT_EQ(has_fast, pass == 1); /* found only with the predictive box */
        p->tick += 1;                   /* the next prepare is a new step */
    }

    /* Through the step: the speculative contact (0.9 m) stops the ball at the box. */
    c->settings.predict_max = DE__CCD_DEFAULT_PREDICT_MAX;
    de_physics_step(p, ST_DT);
    de_physics_step(p, ST_DT);
    EXPECT_TRUE(st_pos(p, fast).x < -0.75 + 0.005);
    EXPECT_TRUE(st_pos(p, fast).x > -0.75 - 0.01);
    de_physics_destroy(p);
}

TEST(physics_ccd, per_pair_speculative_distance) {
    de_physics_world *p = st_world(8, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_body a = make_body(p, DE_BODY_DYNAMIC, cube,
                                st_xform(0.0, 0.0, 0.0, st_quat_identity()), 0.5f, 0.0f, 0.0f, 0u);
    const de_body b = make_body(p, DE_BODY_DYNAMIC, cube,
                                st_xform(3.0, 0.0, 0.0, st_quat_identity()), 0.5f, 0.0f, 0.0f, 0u);
    const u32 ia = de__handle_index(a.id), ib = de__handle_index(b.id);
    de__contacts *ct = NULL;
    ASSERT_EQ(de__contacts_ensure(p, &ct), DE_OK);
    /* Not prepared this step: the base margin. */
    EXPECT_FLOAT_EQ(de__contacts_pair_speculative(p, ct, ia, ib), 0.02f, 0.0f);
    /* Approaching at 30 m/s: 0.5 m. */
    ASSERT_TRUE(de_body_set_velocity(p, a, (de_vec3){15.0f, 0.0f, 0.0f}, (de_vec3){0, 0, 0}));
    ASSERT_TRUE(de_body_set_velocity(p, b, (de_vec3){-15.0f, 0.0f, 0.0f}, (de_vec3){0, 0, 0}));
    de__ccd_prepare(p, ST_DT);
    EXPECT_FLOAT_EQ(de__contacts_pair_speculative(p, ct, ia, ib), 0.5f, 1e-5f);
    /* Spinning adds |w| * reach. */
    ASSERT_TRUE(de_body_set_velocity(p, a, (de_vec3){0, 0, 0}, (de_vec3){0.0f, 0.0f, 10.0f}));
    ASSERT_TRUE(de_body_set_velocity(p, b, (de_vec3){0, 0, 0}, (de_vec3){0, 0, 0}));
    p->tick += 1;
    de__ccd_prepare(p, ST_DT);
    const f32 reach = 10.0f * de_dsqrt(0.75f); /* |w| * half diagonal */
    EXPECT_FLOAT_EQ(de__contacts_pair_speculative(p, ct, ia, ib), reach * ST_DT, 1e-4f);
    /* Clamped to speculative_max. */
    ASSERT_TRUE(de_body_set_velocity(p, a, (de_vec3){300.0f, 0.0f, 0.0f}, (de_vec3){0, 0, 0}));
    p->tick += 1;
    de__ccd_prepare(p, ST_DT);
    EXPECT_FLOAT_EQ(de__contacts_pair_speculative(p, ct, ia, ib), 1.0f, 0.0f);
    /* The point filter: separating points beyond the base margin go, approaching ones stay. */
    de__contact_set set;
    memset(&set, 0, sizeof set);
    set.normal = (de_vec3){1.0f, 0.0f, 0.0f};
    set.point_count = 3;
    set.points[0].separation = 0.01f; /* within the base margin: always kept */
    set.points[1].separation = 2.0f;  /* farther than one step of approach (5 m): kept */
    set.points[1].local_a = (de_vec3){0.5f, 0.0f, 0.0f};
    set.points[1].local_b = (de_vec3){-0.5f, 0.0f, 0.0f};
    set.points[2].separation = 6.0f; /* beyond 300 m/s * dt: dropped */
    de__contacts_filter_speculative(p, ct, ia, ib, &set, 1u);
    EXPECT_EQ(set.point_count, 2u);
    EXPECT_FLOAT_EQ(set.points[1].separation, 2.0f, 0.0f);
    /* Moving apart: only the base margin. */
    set.normal = (de_vec3){-1.0f, 0.0f, 0.0f};
    de__contacts_filter_speculative(p, ct, ia, ib, &set, 1u);
    EXPECT_EQ(set.point_count, 1u);
    de_physics_destroy(p);
}

/* ---- allocation --------------------------------------------------------------------------- */

static de_task_system *tasks_new(u32 workers) {
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

/* Bullets, fast boxes and a resting pile; returns the per-tick checksum fold. */
static u64 ccd_scene_run(de_task_system *ts, const de_allocator *a, u32 ticks, bool check_alloc,
                         de_tracking_allocator *t) {
    de_physics_world *p = st_world(64, ts, a);
    if (p == NULL)
        return 0;
    st_solver(p)->settings.max_linear_speed = 1000.0f;
    const de_shape ground = st_box(p, 20.0f, 20.0f, 0.5f);
    const de_shape wall = st_box(p, 0.01f, 10.0f, 5.0f);
    const de_shape cube = st_box(p, 0.3f, 0.3f, 0.3f);
    const de_shape pellet = st_sphere(p, 0.02f);
    (void)make_body(p, DE_BODY_STATIC, ground, st_xform(0.0, 0.0, -0.5, st_quat_identity()), 0.5f,
                    0.0f, 0.0f, 0u);
    (void)make_body(p, DE_BODY_STATIC, wall, st_xform(8.0, 0.0, 5.0, st_quat_identity()), 0.5f,
                    0.0f, 0.0f, 0u);
    for (u32 i = 0; i < 6; ++i) {
        const de_body b =
            make_body(p, DE_BODY_DYNAMIC, cube,
                      st_xform(-4.0 + 1.5 * (f64)i, -3.0, 2.0 + (f64)i, st_quat_identity()), 0.5f,
                      0.2f, 1.0f, 0u);
        (void)de_body_set_velocity(p, b, (de_vec3){0.0f, 0.0f, -30.0f - 5.0f * (f32)i},
                                   (de_vec3){1.0f, 2.0f, 0.0f});
    }
    for (u32 i = 0; i < 8; ++i) {
        const de_body b =
            make_body(p, DE_BODY_DYNAMIC, pellet,
                      st_xform(-6.0, -2.0 + 0.5 * (f64)i, 1.0 + 0.3 * (f64)i, st_quat_identity()),
                      0.2f, 0.3f, 1.0f, DE_BODY_FLAG_CCD);
        (void)de_body_set_velocity(p, b, (de_vec3){250.0f + 20.0f * (f32)i, 3.0f, 0.0f},
                                   (de_vec3){0.0f, 0.0f, 0.0f});
    }
    de_physics_step(p, ST_DT); /* warm-up: broadphase, contacts, solver, CCD state */
    de_tracking_stats before;
    memset(&before, 0, sizeof before);
    if (check_alloc)
        before = de_tracking_allocator_stats(t);
    u64 sum = 0;
    for (u32 tick = 1; tick < ticks; ++tick) {
        de_physics_step(p, ST_DT);
        sum = sum * 31u + de_physics_checksum(p);
    }
    if (check_alloc) {
        const de_tracking_stats after = de_tracking_allocator_stats(t);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
        EXPECT_EQ(after.free_count, before.free_count);
    }
    de_physics_destroy(p);
    return sum;
}

TEST(physics_ccd, no_heap_allocation_per_step) {
    const u32 worker_counts[2] = {0, 4};
    for (u32 w = 0; w < 2; ++w) {
        de_task_system *ts = tasks_new(worker_counts[w]);
        de_tracking_allocator t;
        de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
        (void)ccd_scene_run(ts, &a, 90, true, &t);
        EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
        de_task_system_destroy(ts);
    }
}

TEST(physics_ccd, worker_count_invariance) {
    const u32 worker_counts[3] = {0, 1, 4};
    u64 sums[3];
    for (u32 w = 0; w < 3; ++w) {
        de_task_system *ts = tasks_new(worker_counts[w]);
        sums[w] = ccd_scene_run(ts, NULL, 90, false, NULL);
        de_task_system_destroy(ts);
    }
    EXPECT_EQ(sums[0], sums[1]);
    EXPECT_EQ(sums[0], sums[2]);
}
