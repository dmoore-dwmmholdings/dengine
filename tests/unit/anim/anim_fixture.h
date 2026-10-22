#ifndef DE_TEST_ANIM_FIXTURE_H
#define DE_TEST_ANIM_FIXTURE_H
#include "de_test.h"
#include "anim/anim_internal.h"
#include "render/formats/de_cooked_le.h"
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define anim_test_pid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define anim_test_pid getpid
#endif
#if DE_ANIM_HAS_COOKER
#include "cook/importers/model/anim_cook.h"
#endif
#include "anim_test_assets.h"
typedef struct anim_fixture {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_anim_system *anim;
    de_skeleton skeleton;
    de_anim_clip clip;
    de_load_handle skeleton_load, clip_load;
    anim_blob skel_blob, clip_blob;
#if DE_HAS_RENDER
    de_renderer *renderer;
    de_gpu_device *device;
    de_gpu_texture target;
    de_gpu_buffer readback;
#endif
} anim_fixture;
static void anim_fixture_setup_options(anim_fixture *f, bool hot_reload) {
    static bool mounted;
    if (!mounted) {
        /* CTest runs separate cases concurrently; their clips have different joint counts. */
        char native[sizeof(DE_ANIM_TEST_TMP) + 64];
        int n = snprintf(native, sizeof native, "%s/process-%lu", DE_ANIM_TEST_TMP,
                         (unsigned long)anim_test_pid());
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof native);
#if defined(_WIN32)
        int created = _mkdir(native);
#else
        int created = mkdir(native, 0700);
#endif
        ASSERT_TRUE(created == 0 || errno == EEXIST);
        ASSERT_EQ(de_fs_mount("animtest", native, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_GENERAL);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.allocator = &f->allocator,
                                                           .worker_count = 3,
                                                           .background_worker_count = 1},
                                    &f->tasks),
              DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->allocator, &f->types), DE_OK);
    ASSERT_EQ(
        de_world_create(
            &(de_world_desc){.allocator = &f->allocator, .types = f->types, .tasks = f->tasks},
            &f->world),
        DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->allocator,
                                                             .tasks = f->tasks,
                                                             .types = f->types,
                                                             .io_threads = 1,
                                                             .hot_reload = hot_reload,
                                                             .cache_dir = "animtest:/cache"},
                                     &f->assets),
              DE_OK);
    ASSERT_EQ(de_anim_create(&f->allocator, f->assets, &f->anim), DE_OK);
    de_anim_set_task_system(f->anim, f->tasks);
    de_anim_attach(f->anim, f->world);
    ASSERT_TRUE(de_anim_component(f->anim).id);
}
static void anim_fixture_setup(anim_fixture *f) {
    anim_fixture_setup_options(f, false);
}
static void anim_fixture_teardown(anim_fixture *f) {
    if (f->anim)
        de_anim_destroy(f->anim);
    if (f->assets) {
        if (f->skeleton_load.id)
            de_asset_release(f->assets, f->skeleton_load);
        if (f->clip_load.id)
            de_asset_release(f->assets, f->clip_load);
        de_asset_system_destroy(f->assets);
    }
#if DE_HAS_RENDER
    if (f->renderer)
        de_renderer_destroy(f->renderer);
    if (f->device) {
        de_gpu_buffer_destroy(f->device, f->readback);
        de_gpu_texture_destroy(f->device, f->target);
        de_gpu_device_destroy(f->device);
    }
#endif
    if (f->world)
        de_world_destroy(f->world);
    if (f->types)
        de_type_registry_destroy(f->types);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    free(f->skel_blob.data);
    free(f->clip_blob.data);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
}
static void load_assets(anim_fixture *f, u32 joints) {
    f->skel_blob = skeleton_blob(joints);
    f->clip_blob = clip_blob(joints);
    ASSERT_EQ(
        de_file_write_all_atomic("animtest:/skeleton.dskl", f->skel_blob.data, f->skel_blob.bytes),
        DE_OK);
    ASSERT_EQ(
        de_file_write_all_atomic("animtest:/clip.dclp", f->clip_blob.data, f->clip_blob.bytes),
        DE_OK);
    const char *manifest =
        "[assets.\"test/skeleton\"]\ntype=\"skeleton\"\nsource=\"skeleton.dskl\"\n[assets.\"test/"
        "clip\"]\ntype=\"anim_clip\"\nsource=\"clip.dclp\"\n";
    ASSERT_EQ(de_file_write_all_atomic("animtest:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "animtest:/assets.toml"), DE_OK);
    f->skeleton_load =
        de_asset_load(f->assets, de_asset_id_from("test/skeleton"), DE_LOAD_NO_EVENTS);
    f->clip_load = de_asset_load(f->assets, de_asset_id_from("test/clip"), DE_LOAD_NO_EVENTS);
    ASSERT_EQ(de_asset_wait(f->assets, f->skeleton_load, 10000000000ull), DE_LOAD_READY);
    ASSERT_EQ(de_asset_wait(f->assets, f->clip_load, 10000000000ull), DE_LOAD_READY);
    f->skeleton = de_anim_skeleton(f->anim, de_asset_id_from("test/skeleton"));
    f->clip = de_anim_clip_asset(f->anim, de_asset_id_from("test/clip"));
    ASSERT_TRUE(f->skeleton.id && f->clip.id);
}
static de_entity spawn_animator(anim_fixture *f) {
    de_component components[] = {de_anim_component(f->anim),
                                 de_builtin_components(f->world).transform};
    de_entity entity;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &entity), DE_OK);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->skeleton = f->skeleton;
    a->clip = f->clip;
    *(DeTransform *)de_entity_get(f->world, entity, de_builtin_components(f->world).transform) =
        de_transform_identity();
    return entity;
}
static void anim_frame(anim_fixture *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_UPDATE, dt, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, dt, 0), DE_OK);
}
#endif
