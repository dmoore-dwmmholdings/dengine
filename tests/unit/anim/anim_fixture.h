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
typedef struct anim_blob {
    u8 *data;
    u32 bytes;
} anim_blob;
static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static u32 aligned(u32 value) {
    return (value + 7) & ~7u;
}
static anim_blob skeleton_blob(u32 joints) {
    u32 parents = 112, names = aligned(parents + joints * 2), rest = aligned(names + joints * 8),
        inverse = aligned(rest + joints * 40), remap = aligned(inverse + joints * 48),
        size = aligned(remap + joints * 2);
    anim_blob blob = {calloc(1, size), size};
    u8 *p = blob.data;
    memcpy(p, "DSKL", 4);
    de_cooked_put_u32(p + 4, 1);
    de_cooked_put_u32(p + 12, joints);
    de_cooked_put_u32(p + 16, joints);
    de_cooked_put_u32(p + 20, 0);
    de_cooked_put_u32(p + 24, parents);
    de_cooked_put_u32(p + 28, names);
    de_cooked_put_u32(p + 32, rest);
    de_cooked_put_u32(p + 36, inverse);
    de_cooked_put_u32(p + 40, remap);
    de_cooked_put_u32(p + 52, size);
    de_affine identity = de_affine_identity();
    for (u32 k = 0; k < 12; ++k)
        de_cooked_put_f32(p + 56 + k * 4, identity.m[k]);
    for (u32 j = 0; j < joints; ++j) {
        put_u16(p + parents + j * 2, j ? (u16)(j - 1) : DE_COOKED_SKEL_NO_PARENT);
        char name[24];
        snprintf(name, sizeof name, "joint%u", j);
        de_cooked_put_u64(p + names + j * 8, de_hash64_str(name));
        de_cooked_put_f32(p + rest + j * 40 + 4, j ? 1.f : 0.f);
        de_cooked_put_f32(p + rest + j * 40 + 24, 1);
        for (u32 c = 0; c < 3; ++c)
            de_cooked_put_f32(p + rest + j * 40 + 28 + c * 4, 1);
        de_affine bind = identity;
        bind.m[10] = -(f32)j;
        for (u32 k = 0; k < 12; ++k)
            de_cooked_put_f32(p + inverse + j * 48 + k * 4, bind.m[k]);
        put_u16(p + remap + j * 2, (u16)j);
    }
    return blob;
}
static anim_blob clip_blob(u32 joints) {
    /* Quantized translation, smallest-three quaternion, uniform scale. */
    u32 tracks = 80, times = 128, data = 136, size = 224;
    anim_blob blob = {calloc(1, size), size};
    u8 *p = blob.data;
    memcpy(p, "DCLP", 4);
    de_cooked_put_u32(p + 4, 1);
    de_cooked_put_u32(p + 12, joints);
    de_cooked_put_u32(p + 16, 3);
    de_cooked_put_u32(p + 20, 2);
    de_cooked_put_u32(p + 24, DE_COOKED_NONE);
    de_cooked_put_f32(p + 28, 1);
    de_cooked_put_f32(p + 32, 1);
    de_cooked_put_u32(p + 36, tracks);
    de_cooked_put_u32(p + 40, times);
    de_cooked_put_u32(p + 44, data);
    de_cooked_put_u32(p + 48, size - data);
    de_cooked_put_u32(p + 52, size);
    de_cooked_put_f32(p + times + 4, 1);
    u32 offsets[] = {0, 40, 56};
    for (u32 t = 0; t < 3; ++t) {
        u8 *track = p + tracks + t * 16;
        track[2] = (u8)t;
        track[3] = (u8)(t == 0   ? DE_COOKED_CLIP_ENC_QUANT16
                        : t == 1 ? DE_COOKED_CLIP_ENC_SMALLEST3
                                 : DE_COOKED_CLIP_ENC_RAW);
        track[4] = (u8)(t == 0 ? 3 : t == 1 ? 4 : 1);
        if (t == 2)
            track[5] = DE_COOKED_CLIP_TRACK_UNIFORM;
        de_cooked_put_u32(track + 8, 2);
        de_cooked_put_u32(track + 12, offsets[t]);
    }
    u8 *translation = p + data;
    de_cooked_put_f32(translation + 12, 2);
    put_u16(translation + 26, 1);
    put_u16(translation + 34, 65535);
    u8 *rotation = p + data + 40;
    put_u16(rotation + 2, 1);
    const f64 quats[2][4] = {{0, 0, 0, 1}, {0, 0, .7071067811865476, .7071067811865476}};
    for (u32 key = 0; key < 2; ++key) {
#if DE_ANIM_HAS_COOKER
        u16 words[3];
        f32 decoded[4];
        de_model_clip_s3_encode(quats[key], words, decoded);
        for (u32 w = 0; w < 3; ++w)
            put_u16(rotation + 4 + key * 6 + w * 2, words[w]);
#else
        u32 q = 16384;
        u64 bits = ((u64)3 << 45) | ((u64)q << 30) | ((u64)q << 15) | (key ? 32767 : q);
        for (u32 b = 0; b < 6; ++b)
            rotation[4 + key * 6 + b] = (u8)(bits >> (b * 8));
        (void)quats;
#endif
    }
    u8 *scale = p + data + 56;
    put_u16(scale + 2, 1);
    de_cooked_put_f32(scale + 4, 1);
    de_cooked_put_f32(scale + 8, 2);
    return blob;
}
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
