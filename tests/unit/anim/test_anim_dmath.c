#include "anim_fixture.h"
#include "anim/anim_math.h"

static u32 float_bits(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
TEST(anim_fixed, integer_clock_rate_pause_seek_reverse_and_long_running_time) {
    de_fp_env_init_thread();
    de__anim_clock clock = {0};
    DeAnimator a = {.speed = 1, .playing = true, .loop = true};
    bool ended;
    const f32 dt = 1.f / 60.f;
    for (u32 tick = 1; tick <= 10000; ++tick) {
        de__anim_clock_step(&clock, &a, dt, 3, 1, false, &ended);
        EXPECT_EQ(clock.ticks, tick);
        EXPECT_FALSE(ended);
        EXPECT_EQ(float_bits(a.time), float_bits((f32)fmod((f64)tick * (f64)dt, 3)));
    }
    f32 paused = a.time;
    a.playing = false;
    for (u32 i = 0; i < 200; ++i)
        de__anim_clock_step(&clock, &a, dt, 3, 1, false, &ended);
    EXPECT_EQ(float_bits(a.time), float_bits(paused));
    a.speed = -2;
    a.playing = true;
    f64 previous = clock.exact_time;
    de__anim_clock_step(&clock, &a, dt, 3, 1, false, &ended);
    EXPECT_EQ(float_bits(a.time), float_bits((f32)(previous - 2 * (f64)dt)));
    a.time = .75f;
    de__anim_clock_step(&clock, &a, .125f, 3, 1, true, &ended);
    EXPECT_EQ(float_bits(a.time), 0x3f000000u);
    clock = (de__anim_clock){.initialized = true,
                             .duration = 1,
                             .clip_key = 1,
                             .dt = .125f,
                             .speed = 1,
                             .segment_ticks = UINT64_C(1) << 36};
    a = (DeAnimator){.speed = 1, .playing = true, .loop = true};
    de__anim_clock_step(&clock, &a, .125f, 1, 1, false, &ended);
    EXPECT_EQ(float_bits(a.time), 0x3e000000u); /* f32 tick arithmetic would lose this. */
    a.loop = false;
    a.time = .875f;
    de__anim_clock_step(&clock, &a, .25f, 1, 1, true, &ended);
    EXPECT_TRUE(ended);
    EXPECT_FALSE(a.playing);
    EXPECT_TRUE(a.time == 1);
    a.playing = true;
    a.speed = -1;
    de__anim_clock_step(&clock, &a, .25f, 1, 1, false, &ended);
    EXPECT_TRUE(a.time == .75f);
}

TEST(anim_fixed, deterministic_quaternion_ieee_vectors) {
    de_fp_env_init_thread();
    de_quat n = de__anim_normalize((de_quat){0, 0, 3, 4}, true);
    EXPECT_EQ(float_bits(n.z), 0x3f19999au);
    EXPECT_EQ(float_bits(n.w), 0x3f4ccccdu);
    n = de__anim_nlerp((de_quat){0, 0, 0, 1}, (de_quat){0, 0, 1, 0}, .5f, true);
    EXPECT_EQ(float_bits(n.z), 0x3f3504f3u);
    EXPECT_EQ(float_bits(n.w), 0x3f3504f3u);
}

TEST(anim_fixed, binary_clock_wrap_matches_independent_libm_oracle) {
    de_fp_env_init_thread();
    de_rng rng = de_rng_seed(192, 91);
    for (u32 i = 0; i < 4096; ++i) {
        u32 time_bits = de_rng_u32(&rng), period_bits = de_rng_u32(&rng) & 0x7fffffffu;
        /* Finite normal inputs include huge exponent gaps and both time signs. */
        time_bits = (time_bits & 0x807fffffu) | (((time_bits >> 23) % 254 + 1) << 23);
        period_bits = (period_bits & 0x007fffffu) | (((period_bits >> 23) % 254 + 1) << 23);
        f32 time, period;
        memcpy(&time, &time_bits, sizeof time);
        memcpy(&period, &period_bits, sizeof period);
        DeAnimator a = {.time = time, .speed = 1, .loop = true};
        de__anim_clock clock = {0};
        bool ended;
        de__anim_clock_step(&clock, &a, 0, period, 1, false, &ended);
        f64 reference = fmod((f64)time, (f64)period);
        if (reference < 0)
            reference += period;
        if (reference >= period)
            reference = 0;
        if (reference == 0)
            reference = 0; /* Normalize negative zero, matching the runtime contract. */
        EXPECT_EQ(float_bits(a.time), float_bits((f32)reference));
        EXPECT_FALSE(ended);
    }
}

static void fixed_tick(anim_fixture *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, dt, 0), DE_OK);
}
TEST_F(anim_fixed, fixed_only_mixed_cadence_mode_handoff_and_snapshot_replay, anim_fixture) {
    load_assets(f, 3);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->deterministic = true;
    anim_frame(f, .4f);
    EXPECT_TRUE(a->time == 0);
    fixed_tick(f, .125f);
    a = de_entity_get(f->world, entity, f->anim->component); /* Clock add moves the row. */
    EXPECT_TRUE(a->time == .125f);
    const de__anim_clock *clock = de_entity_get_const(f->world, entity, f->anim->clock_component);
    ASSERT_TRUE(clock);
    EXPECT_EQ(clock->ticks, 1u);
    u64 checksum = de_anim_checksum(f->world);
    for (u32 i = 0; i < 5; ++i)
        anim_frame(f, .37f);
    EXPECT_EQ(de_anim_checksum(f->world), checksum);
    EXPECT_TRUE(a->time == .125f);
    de_world_snapshot snap = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snap), DE_OK);
    u64 expected[12], worlds[12];
    for (u32 i = 0; i < 12; ++i) {
        fixed_tick(f, .125f);
        expected[i] = de_anim_checksum(f->world);
        worlds[i] = de_world_checksum(f->world, DE_COMP_SNAPSHOT);
    }
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snap), DE_OK);
    EXPECT_EQ(de_anim_checksum(f->world), checksum);
    de_transform bone;
    EXPECT_FALSE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    for (u32 i = 0; i < 12; ++i) {
        a = de_entity_get(f->world, entity, f->anim->component);
        a->deterministic = false;
        anim_frame(f, .031f);
        a = de_entity_get(f->world, entity, f->anim->component);
        a->deterministic = true;
        fixed_tick(f, .125f);
        EXPECT_EQ(de_anim_checksum(f->world), expected[i]);
        EXPECT_EQ(de_world_checksum(f->world, DE_COMP_SNAPSHOT), worlds[i]);
        anim_frame(f, .017f);
    }
    de_world_snapshot_free(&f->allocator, &snap);
    EXPECT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
}

TEST_F(anim_fixed, strict_model_palette_matches_analytic_parent_chain, anim_fixture) {
    load_assets(f, 3);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->deterministic = true;
    a->playing = false;
    fixed_tick(f, 0);
    de__anim_entry *entry = &f->anim->entries[de_entity_index(entity)];
    for (u32 j = 0; j < 3; ++j) {
        entry->local.rotations[j] = (de_quat){0, 0, 0, 1};
        entry->local.translations[j] = (de_vec3){1, 2, 3};
        entry->local.scales[j] = (de_vec3){1, 1, 1};
    }
    de__anim_pose_deterministic(&entry->skeleton, &entry->local, entry->model, entry->palette);
    for (u32 j = 0; j < 3; ++j) {
        EXPECT_TRUE(entry->model[j].m[9] == (f32)(j + 1));
        EXPECT_TRUE(entry->model[j].m[10] == (f32)(2 * (j + 1)));
        EXPECT_TRUE(entry->model[j].m[11] == (f32)(3 * (j + 1)));
        EXPECT_TRUE(entry->palette[j].m[10] == (f32)(j + 2));
    }
    entry->local.rotations[0] = (de_quat){0, 0, 1, 0};
    de__anim_pose_deterministic(&entry->skeleton, &entry->local, entry->model, entry->palette);
    for (u32 j = 0; j < 3; ++j) {
        EXPECT_TRUE(entry->model[j].m[9] == 1.f - (f32)j);
        EXPECT_TRUE(entry->model[j].m[10] == 2.f - 2.f * (f32)j);
        EXPECT_TRUE(entry->model[j].m[11] == 3.f * (f32)(j + 1));
        EXPECT_TRUE(entry->palette[j].m[10] == 2.f - (f32)j);
    }
}

TEST_F(anim_fixed, deferred_clocks_parallel_ranges_and_restore_before_first_tick, anim_fixture) {
    load_assets(f, 3);
    de_entity actors[130]; /* Multiple 32-row ranges and entry-table growth. */
    for (u32 i = 0; i < 130; ++i) {
        actors[i] = spawn_animator(f);
        DeAnimator *a = de_entity_get(f->world, actors[i], f->anim->component);
        a->deterministic = true;
        a->speed = (f32)(i % 7 + 1) * .125f;
    }
    de_world_snapshot before = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &before), DE_OK);
    fixed_tick(f, .125f);
    u64 checksum = de_anim_checksum(f->world);
    for (u32 i = 0; i < 130; ++i) {
        const DeAnimator *a = de_entity_get_const(f->world, actors[i], f->anim->component);
        EXPECT_TRUE(a->time == (f32)(i % 7 + 1) * .015625f);
        const de__anim_clock *clock =
            de_entity_get_const(f->world, actors[i], f->anim->clock_component);
        ASSERT_TRUE(clock);
        EXPECT_EQ(clock->ticks, 1u);
    }
    /* Alter archetype and row placement without altering animation state. */
    for (u32 i = 0; i < 130; i += 2)
        ASSERT_TRUE(de_entity_add(f->world, actors[i], f->anim->ik_component));
    EXPECT_EQ(de_anim_checksum(f->world), checksum);
    ASSERT_EQ(de_world_snapshot_restore(f->world, &before), DE_OK);
    fixed_tick(f, .125f);
    EXPECT_EQ(de_anim_checksum(f->world), checksum);
    de_world_snapshot_free(&f->allocator, &before);
}
