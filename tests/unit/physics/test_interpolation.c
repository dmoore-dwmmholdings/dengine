/* test_interpolation.c: physics.interpolate (DENG-223): alpha blending at a 30 Hz fixed rate and
 * 144 Hz frames, pass-through in variable timing, an unchanged simulation, children of
 * interpolated bodies, the opt-out, and teleports. */
#include "ecs_test_util.h"

static const de_transform *global_of(const pe_env *e, de_entity ent) {
    return (const de_transform *)de_entity_get_const(e->w, ent, e->gx);
}

/* A sphere in zero gravity at constant velocity, never sleeping. */
static de_entity moving_sphere(pe_env *e, f64 x, f32 vx) {
    const de_entity s = pe_spawn_body(e, DE_BODY_DYNAMIC, st_sphere(e->p, 0.25f), x, 0.0, 1.0);
    DeRigidBody *rb = (DeRigidBody *)de_entity_get(e->w, s, e->rb);
    rb->linear_velocity = de__v3(vx, 0.0f, 0.0f);
    rb->allow_sleep = false;
    return s;
}

/* One frame of `frame_dt` at a fixed rate: FixedUpdate as often as the accumulator allows, then
 * PostUpdate with the leftover alpha. Returns the number of ticks. */
static u32 frame(pe_env *e, f64 *acc, f64 frame_dt, f64 fixed_dt) {
    *acc += frame_dt;
    u32 ticks = 0;
    while (*acc >= fixed_dt) {
        EXPECT_EQ(pe_tick_dt(e, (f32)fixed_dt), DE_OK);
        *acc -= fixed_dt;
        ++ticks;
    }
    const f32 alpha = (f32)(*acc / fixed_dt);
    EXPECT_EQ(de_world_run_schedule(e->w, DE_SCHEDULE_POST_UPDATE, (f32)frame_dt, alpha), DE_OK);
    return ticks;
}

TEST(physics_interpolation, smooth_at_30hz_with_144hz_frames) {
    pe_env e;
    pe_init(&e, 0);
    de_physics_set_gravity(e.p, de__v3(0.0f, 0.0f, 0.0f));
    const de_entity s = moving_sphere(&e, 0.0, 3.0f);
    const f64 fixed = 1.0 / 30.0, fdt = 1.0 / 144.0;
    f64 acc = fixed; /* the first frame runs one tick */
    (void)frame(&e, &acc, 0.0, fixed);
    f64 last = global_of(&e, s)->position.x;
    f64 max_step = 0.0;
    u32 ticks = 0;
    for (u32 f = 0; f < 288; ++f) { /* 2 s */
        ticks += frame(&e, &acc, fdt, fixed);
        const de_transform *g = global_of(&e, s);
        const f64 x = g->position.x;
        EXPECT_TRUE(x >= last); /* monotonic for a constant velocity */
        if (x - last > max_step)
            max_step = x - last;
        last = x;
        /* The rendered pose is the blend of the last two ticks; the sim pose is untouched. */
        const de__pecs *st = de__pecs_get(e.p);
        const de__pecs_pose_pair *pp = &st->poses[pe_index(&e, s)];
        const f64 a = acc / fixed;
        EXPECT_FLOAT_EQ(x, pp->prev.position.x + (pp->curr.position.x - pp->prev.position.x) * a,
                        1e-6);
        EXPECT_MEMEQ(&pe_xf(&e, s)->position, &pp->curr.position, sizeof pp->curr.position);
    }
    EXPECT_GE(ticks, 59u);
    EXPECT_LE(ticks, 61u);
    /* Smooth: every frame moves about 3 / 144 m (a whole tick would be 0.1 m). */
    ST_LT(max_step, 3.0 / 144.0 * 1.05);
    pe_free(&e);
}

TEST(physics_interpolation, variable_timing_passes_through) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_entity a =
        pe_spawn_body(&e, DE_BODY_DYNAMIC, st_box(e.p, 0.3f, 0.3f, 0.3f), 0.0, 0.0, 2.0);
    for (u32 f = 0; f < 90; ++f) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, ST_DT, 1.0f), DE_OK);
        de_transform bt;
        ASSERT_TRUE(de_body_get_transform(e.p, de_physics_entity_body(e.p, a), &bt));
        EXPECT_MEMEQ(&global_of(&e, a)->position, &bt.position, sizeof bt.position);
        EXPECT_MEMEQ(&global_of(&e, a)->rotation, &bt.rotation, sizeof bt.rotation);
    }
    pe_free(&e);
}

/* A scene with every kind of interpolated body; returns per-tick checksums. */
static void sim_run(bool interpolate, u64 *out, u32 ticks) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_shape box = st_box(e.p, 0.3f, 0.3f, 0.3f);
    for (u32 i = 0; i < 6; ++i)
        pe_spawn_body(&e, DE_BODY_DYNAMIC, box, (f64)(i % 3u) * 0.5, 0.0, 0.5 + 0.7 * (f64)i);
    const de_entity k = pe_spawn_body(&e, DE_BODY_KINEMATIC, box, 4.0, 0.0, 0.3);
    const de_entity c = pe_spawn_xf(&e, -3.0, 0.0, 1.0);
    (void)de_entity_add(e.w, c, e.ch);
    const de_entity child = pe_spawn_xf(&e, 0.0, 0.0, 0.5);
    ASSERT_EQ(de_entity_set_parent(e.w, child, k), DE_OK);
    if (!interpolate) {
        /* Disabled: PostUpdate still runs (transform propagation), physics.interpolate not. */
        de_system s = de__pecs_get(e.p)->sys_interp;
        de_system_set_enabled(e.w, s, false);
    }
    for (u32 t = 0; t < ticks; ++t) {
        de_transform *xf = (de_transform *)de_entity_get(e.w, k, e.xf);
        xf->position.x = 4.0 + 0.01 * (f64)t;
        ((DeCharacter *)de_entity_get(e.w, c, e.ch))->desired_velocity = de__v3(1.0f, 0.0f, 0.0f);
        ASSERT_EQ(pe_tick(&e), DE_OK);
        for (u32 f = 0; f < 3; ++f)
            ASSERT_EQ(
                de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, ST_DT / 3.0f, (f32)f / 3.0f),
                DE_OK);
        out[2u * t] = de_physics_checksum(e.p);
        out[2u * t + 1u] = de_world_checksum(e.w, DE_COMP_SNAPSHOT);
    }
    pe_free(&e);
}

TEST(physics_interpolation, simulation_identical_without_it) {
    u64 with[240], without[240];
    sim_run(true, with, 120);
    sim_run(false, without, 120);
    for (u32 i = 0; i < 240; ++i)
        if (with[i] != without[i]) {
            fprintf(stderr, "  tick %u (%s) differs\n", i / 2u, (i & 1u) ? "ecs" : "physics");
            EXPECT_EQ(with[i], without[i]);
            break;
        }
}

TEST(physics_interpolation, children_opt_out_and_teleport) {
    pe_env e;
    pe_init(&e, 0);
    de_physics_set_gravity(e.p, de__v3(0.0f, 0.0f, 0.0f));
    const de_entity s = moving_sphere(&e, 0.0, 6.0f);
    const de_entity child = pe_spawn_xf(&e, 1.0, 0.0, 0.0); /* 1 m beside its parent */
    ASSERT_EQ(de_entity_set_parent(e.w, child, s), DE_OK);
    const de_entity off = moving_sphere(&e, 0.0, 6.0f);
    ((DeRigidBody *)de_entity_get(e.w, off, e.rb))->no_interpolate = true;
    for (u32 t = 0; t < 3; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, ST_DT, 0.5f), DE_OK);

    /* Halfway between the last two ticks: 0.05 m behind the sim pose. */
    const f64 sim_x = pe_xf(&e, s)->position.x;
    EXPECT_FLOAT_EQ(global_of(&e, s)->position.x, sim_x - 0.05, 1e-6);
    /* The child follows the interpolated parent. */
    EXPECT_FLOAT_EQ(global_of(&e, child)->position.x, sim_x - 0.05 + 1.0, 1e-6);
    /* The opt-out body shows its sim pose. */
    EXPECT_MEMEQ(&global_of(&e, off)->position, &pe_xf(&e, off)->position, sizeof(de_dvec3));

    /* A teleport does not smear: the blend starts at the new place. */
    de_transform *xf = (de_transform *)de_entity_get(e.w, s, e.xf);
    xf->position = de__d3(100.0, 0.0, 1.0);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, ST_DT, 0.0f), DE_OK);
    EXPECT_FLOAT_EQ(global_of(&e, s)->position.x, 100.0, 1e-9);
    pe_free(&e);
}
