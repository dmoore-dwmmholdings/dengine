#include "de_test.h"
#include "de_det_harness.h"
#include "anim_test_assets.h"
#include "anim/anim_math.h"
#include <dengine/de_platform.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define test_pid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_pid getpid
#endif
#include <errno.h>

#define TICKS  1000u
#define ACTORS 20u
#define SEED   UINT64_C(0xd1925eed)
typedef struct scenario_state {
    de_asset_system *assets;
    de_anim_system *anim;
    de_load_handle loads[4];
    de_entity actors[ACTORS];
    de_anim_clip clips[3];
    f32 weights[ACTORS];
    u64 seed;
    u32 perturb_tick;
} scenario_state;

static void blend(de_anim_system *anim, void *user) {
    scenario_state *state = user;
    for (u32 i = 0; i < anim->job_count; ++i) {
        de__anim_entry *e = &anim->entries[anim->jobs[i]];
        u32 actor = 0;
        while (actor < ACTORS && state->actors[actor].bits != e->entity.bits)
            ++actor;
        ASSERT_TRUE(actor < ACTORS);
        /* Blend two sampled authored clips through the same strict pose helper
         * that blend trees will consume. Fixed scalar rest buffers, no allocation. */
        de_quat rotations[3];
        de_vec3 translations[3], scales[3];
        de__anim_local_pose other = {
            .count = 3, .rotations = rotations, .translations = translations, .scales = scales};
        const de__anim_asset *asset =
            de__anim_asset_get(anim, state->clips[(actor + 1) % 3].id, "anim_clip");
        ASSERT_TRUE(asset);
        de__anim_clip_cursor cursor = {0};
        u32 keys[3] = {0};
        cursor.keys = keys;
        cursor.capacity = 3;
        ASSERT_TRUE(de__anim_sample_pose_cached(&asset->clip, &e->skeleton, e->animator->time,
                                                &other, &cursor));
        ASSERT_TRUE(de__anim_pose_blend(&e->local, &e->local, &other, state->weights[actor], true));
        if (actor == 3 && de_world_tick(anim->world) == state->perturb_tick) {
            u32 bits;
            memcpy(&bits, &e->local.translations[0].x, sizeof bits);
            bits ^= 1u;
            memcpy(&e->local.translations[0].x, &bits, sizeof bits);
        }
    }
}
static de_result setup(de_world *world, u64 seed, void *user) {
    scenario_state *s = user;
    s->seed = seed;
    s->perturb_tick = UINT32_MAX;
    static bool mounted;
    if (!mounted) {
        char path[sizeof(DE_ANIM_DET_ASSETS) + 64];
        snprintf(path, sizeof path, "%s/%lu", DE_ANIM_DET_ASSETS, (unsigned long)test_pid());
#if defined(_WIN32)
        int rc = _mkdir(path);
#else
        int rc = mkdir(path, 0700);
#endif
        if (rc != 0 && errno != EEXIST)
            return DE_ERR_IO;
        DE_TRY(de_fs_mount("animdet", path, DE_MOUNT_WRITABLE, 0));
        mounted = true;
    }
    DE_TRY(de_asset_system_create(&(de_asset_system_desc){.tasks = de_world_task_system(world),
                                                          .types = de_world_types(world),
                                                          .io_threads = 1,
                                                          .cache_dir = "animdet:/cache"},
                                  &s->assets));
    DE_TRY(de_anim_create(NULL, s->assets, &s->anim));
    de_anim_set_task_system(s->anim, de_world_task_system(world));
    de_anim_attach(s->anim, world);
    if (!de_anim_component(s->anim).id)
        return DE_ERR_INTERNAL;
    anim_blob skeleton = skeleton_blob(3);
    de_result result =
        de_file_write_all_atomic("animdet:/skeleton.dskl", skeleton.data, skeleton.bytes);
    free(skeleton.data);
    DE_TRY(result);
    for (u32 i = 0; i < 3; ++i) {
        anim_blob clip = clip_blob(3);
        /* Distinct translation ranges and clip durations; original quantized
         * key-time and smallest-three rotations exercise the production decoder. */
        de_cooked_put_f32(clip.data + 28, 1.f + (f32)i * .5f);
        de_cooked_put_f32(clip.data + 136 + 12, 2.f + (f32)i);
        char path[64];
        snprintf(path, sizeof path, "animdet:/clip%u.dclp", i);
        result = de_file_write_all_atomic(path, clip.data, clip.bytes);
        free(clip.data);
        DE_TRY(result);
    }
    const char *manifest = "[assets.\"det/skel\"]\ntype=\"skeleton\"\nsource=\"skeleton.dskl\"\n"
                           "[assets.\"det/clip0\"]\ntype=\"anim_clip\"\nsource=\"clip0.dclp\"\n"
                           "[assets.\"det/clip1\"]\ntype=\"anim_clip\"\nsource=\"clip1.dclp\"\n"
                           "[assets.\"det/clip2\"]\ntype=\"anim_clip\"\nsource=\"clip2.dclp\"\n";
    DE_TRY(de_file_write_all_atomic("animdet:/assets.toml", manifest, strlen(manifest)));
    DE_TRY(de_asset_manifest_load(s->assets, "animdet:/assets.toml"));
    const char *names[] = {"det/skel", "det/clip0", "det/clip1", "det/clip2"};
    for (u32 i = 0; i < 4; ++i) {
        s->loads[i] = de_asset_load(s->assets, de_asset_id_from(names[i]), DE_LOAD_NO_EVENTS);
        if (de_asset_wait(s->assets, s->loads[i], UINT64_C(10000000000)) != DE_LOAD_READY)
            return DE_ERR_IO;
    }
    for (u32 i = 0; i < 3; ++i)
        s->clips[i] = de_anim_clip_asset(s->anim, de_asset_id_from(names[i + 1]));
    de_skeleton skel = de_anim_skeleton(s->anim, de_asset_id_from(names[0]));
    for (u32 i = 0; i < ACTORS; ++i) {
        de_component component = de_anim_component(s->anim);
        DE_TRY(de_entity_spawn_with(world, &component, 1, &s->actors[i]));
        DeAnimator *a = de_entity_get(world, s->actors[i], component);
        a->skeleton = skel;
        a->clip = s->clips[i % 3];
        a->deterministic = true;
        a->time = (f32)i * .03125f;
    }
    return de_anim__register_stage(s->anim, DE_ANIM_STAGE_BLEND, blend, s);
}
static void script(de_world *world, u64 tick, void *user) {
    scenario_state *s = user;
    de_rng rng = de_rng_seed(s->seed, tick);
    for (u32 i = 0; i < ACTORS; ++i) {
        DeAnimator *a = de_entity_get(world, s->actors[i], de_anim_component(s->anim));
        f32 speed = de_rng_f32(&rng);
        s->weights[i] = de_rng_f32(&rng);
        a->speed = (speed - .5f) * 6.f;
        a->playing = (tick + i) % 23 != 0;
        if (tick % 37 == 0)
            a->clip = s->clips[(i + (u32)(tick / 37)) % 3];
    }
}
static void teardown(de_world *world, void *user) {
    (void)world;
    scenario_state *s = user;
    de_anim_destroy(s->anim);
    if (s->assets) {
        for (u32 i = 0; i < 4; ++i)
            if (s->loads[i].id)
                de_asset_release(s->assets, s->loads[i]);
        de_asset_system_destroy(s->assets);
    }
}
static const de_det_scenario scenario = {.name = "anim",
                                         .setup = setup,
                                         .tick = script,
                                         .teardown = teardown,
                                         .ticks = TICKS,
                                         .state_size = sizeof(scenario_state)};
static void step(de_det_session *s, u64 *out) {
    u64 ecs;
    ASSERT_EQ(de_det_session_step(s, &ecs), DE_OK);
    u64 parts[] = {de_anim_checksum(s->world), ecs};
    *out = de_hash64(parts, sizeof parts, 0);
}
static void verify(u32 workers) {
    de_det_session session = {0};
    ASSERT_EQ(de_det_session_begin(&session, &scenario, SEED, workers), DE_OK);
    u64 actual[TICKS], golden[TICKS];
    for (u32 i = 0; i < TICKS; ++i)
        step(&session, &actual[i]);
    de_det_session_end(&session);
#if defined(DE_DEBUG) || defined(DE_DEV)
    const char *update = getenv("DE_DET_UPDATE_GOLDEN");
    if (update && !strcmp(update, "1") && workers == 1) {
        ASSERT_EQ(de_det_write_checksums(DE_ANIM_DET_GOLDEN, "anim", SEED, actual, TICKS), DE_OK);
    }
#endif
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(DE_ANIM_DET_GOLDEN, golden, TICKS, &count), DE_OK);
    ASSERT_EQ(count, TICKS);
    EXPECT_EQ(de_det_report("animation vs arm64 vector", golden, actual, TICKS),
              DE_DET_NO_DIVERGENCE);
    if (workers == 1)
        ASSERT_EQ(
            de_det_write_checksums(DE_DET_OUT_DIR "/anim.checksums", "anim", SEED, actual, TICKS),
            DE_OK);
}
TEST(anim_determinism, workers_1) {
    verify(1);
}
TEST(anim_determinism, workers_4) {
    verify(4);
}
TEST(anim_determinism, workers_8) {
    verify(8);
}
TEST(anim_determinism, timing_mode_toggle) {
    de_det_session session = {0};
    ASSERT_EQ(de_det_session_begin(&session, &scenario, SEED, 4), DE_OK);
    scenario_state *s = session.state;
    u64 actual[TICKS], golden[TICKS];
    for (u32 tick = 0; tick < TICKS; ++tick) {
        if (tick) {
            for (u32 i = 0; i < ACTORS; ++i) {
                DeAnimator *a =
                    de_entity_get(session.world, s->actors[i], de_anim_component(s->anim));
                a->deterministic = false;
            }
            ASSERT_EQ(de_world_run_schedule(session.world, DE_SCHEDULE_POST_UPDATE, .017f, 0),
                      DE_OK);
            for (u32 i = 0; i < ACTORS; ++i) {
                DeAnimator *a =
                    de_entity_get(session.world, s->actors[i], de_anim_component(s->anim));
                a->deterministic = true;
            }
        }
        step(&session, &actual[tick]);
    }
    de_det_session_end(&session);
    u32 count;
    ASSERT_EQ(de_det_read_checksums(DE_ANIM_DET_GOLDEN, golden, TICKS, &count), DE_OK);
    ASSERT_EQ(count, TICKS);
    EXPECT_EQ(de_det_report("timing-mode handoff vs original vector", golden, actual, TICKS),
              DE_DET_NO_DIVERGENCE);
}
TEST(anim_determinism, rollback_and_cadence) {
    de_det_session session = {0};
    ASSERT_EQ(de_det_session_begin(&session, &scenario, SEED, 4), DE_OK);
    u64 unused, expected[200];
    for (u32 i = 0; i < 400; ++i)
        step(&session, &unused);
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(session.world, NULL, &snapshot), DE_OK);
    for (u32 i = 0; i < 200; ++i)
        step(&session, &expected[i]);
    ASSERT_EQ(de_world_snapshot_restore(session.world, &snapshot), DE_OK);
    for (u32 i = 0; i < 200; ++i) {
        for (u32 frame = 0; frame < i % 4; ++frame) {
            ASSERT_EQ(de_world_run_schedule(session.world, DE_SCHEDULE_POST_UPDATE, .017f, 0),
                      DE_OK);
        }
        u64 actual;
        step(&session, &actual);
        EXPECT_EQ(actual, expected[i]);
    }
    de_world_snapshot_free(NULL, &snapshot);
    de_det_session_end(&session);
}
TEST(anim_determinism, detects_pose_bit_change) {
    de_det_session session = {0};
    ASSERT_EQ(de_det_session_begin(&session, &scenario, SEED, 1), DE_OK);
    u64 expected[TICKS], actual[TICKS];
    for (u32 tick = 0; tick < TICKS; ++tick)
        step(&session, &expected[tick]);
    de_det_session_end(&session);
    ASSERT_EQ(de_det_session_begin(&session, &scenario, SEED, 1), DE_OK);
    scenario_state *s = session.state;
    s->perturb_tick = 400;
    /* Normal finalization must capture the changed sampled pose. Never patch
     * the stored hash here: that would bypass the integration being verified. */
    for (u32 tick = 0; tick < TICKS; ++tick)
        step(&session, &actual[tick]);
    EXPECT_EQ(de_det_report("one-bit sampled pose perturbation", expected, actual, TICKS), 400u);
    de_det_session_end(&session);
}
