/*
 * test_render_loaders.c: the runtime `texture`, `mesh`, `material` and `model` types through the
 * asset system (DENG-105, SPEC §11.4). CTest names render_loaders.loaders.<test>.
 *
 * Most tests write small cooked blobs (loader_test_util.h) as source files of a manifest in a
 * scratch directory of the build tree (mounted as rl:/, one subdirectory per test) and register
 * the types without importers, so the bytes go to the loaders as they are. Frames are driven by
 * hand: de_render_uploader_flush, then de_asset_system_update.
 *
 *   - Null backend: publish after the fence (a held fence keeps the slot LOADING), uploads larger
 *     than the ring over several frames, materials waiting on their textures and PARTIAL on a
 *     failed texture, hot reload swapping the GPU objects, cancelling during an upload, failures.
 *   - Every GPU backend compiled in (Metal on the host; Vulkan where a driver exists, lavapipe in
 *     CI's golden job; D3D12 on the vendored WARP): a BC7 mip chain and a BC6H environment cube
 *     read back byte for byte, and the mesh buffers read back byte for byte. They SKIP without
 *     the device or the format.
 *   - With the cooker (DE_HAS_COOKER): a glTF model cooked and loaded end to end through the
 *     registered importers, and an environment cooked from an EXR, uploaded and read back.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/cook_stage_internal.h"
#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/slots_internal.h"
#include "loader_test_util.h"
#include "render/loaders/render_assets.h"
#include "render/loaders/upload_ring.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif

#if DE_HAS_COOKER
#include "asset/manifest_internal.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/lib/de_cooker.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct lfx {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_task_system *tasks;
    de_asset_system *s;
    de_gpu_device *d;
    de_render_uploader *u;
    de_type_registry *types;
    de_world *w;
    char dir[96];
    char why[1200];
} lfx;

static void lfx_setup(lfx *f) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("rl", DE_RL_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread(); /* the task system checks the main thread's FP mode */
}

/* Errors the device's validation layer reported (Vulkan, D3D12; 0 elsewhere). */
static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void lfx_teardown(lfx *f) {
    de_asset_system_destroy(f->s); /* first: its unloads use the uploader */
    de_render_uploader_destroy(f->u);
    if (f->d != NULL) {
        de_gpu_wait_idle(f->d);
        EXPECT_EQ(validation_errors(f->d), 0u); /* barriers, copies and transfers are valid */
        de_gpu_device_destroy(f->d);
    }
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).bytes_live, 0u);
}

typedef struct start_opts {
    de_gpu_backend backend; /* 0 = null */
    u64 ring_bytes;         /* 0 = 8 MiB */
    u64 frame_cap;
    bool hot_reload;
    bool import; /* register the importers (cooker builds) */
    bool types;  /* a type registry and a world, for the prefab importer */
} start_opts;

static void start(lfx *f, const char *name, start_opts o) {
    snprintf(f->dir, sizeof f->dir, "rl:/%s", name);
    if (!de_path_exists(f->dir))
        ASSERT_EQ(de_dir_create(f->dir), DE_OK);
    if (o.types) {
        ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
        ASSERT_EQ(
            de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
            DE_OK);
        ASSERT_TRUE(DE_HANDLE_VALID(de_builtin_components(f->w).transform));
    }
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    char cache[128];
    snprintf(cache, sizeof cache, "%s/decache", f->dir);
    de_asset_system_desc ad = {.allocator = &f->alloc,
                               .tasks = f->tasks,
                               .io_threads = 1,
                               .cache_dir = cache,
                               .hot_reload = o.hot_reload,
                               .reload_retire_frames = 2,
                               .types = f->types};
    ASSERT_EQ(de_asset_system_create(&ad, &f->s), DE_OK);
    const de_gpu_backend b = o.backend != DE_GPU_DEFAULT ? o.backend : DE_GPU_NULL;
    f->d = lt_device(b, &f->dev_alloc, f->why, sizeof f->why);
    if (f->d == NULL)
        SKIP(f->why);
    de__render_assets_desc rd = {.device = f->d,
                                 .allocator = &f->alloc,
                                 .ring_bytes = o.ring_bytes != 0 ? o.ring_bytes : (u64)8 << 20,
                                 .frame_cap_bytes = o.frame_cap,
                                 .no_import = !o.import};
    ASSERT_EQ(de__render_register_asset_types_ex(f->s, &rd, &f->u), DE_OK);
}

static void put(lfx *f, const char *file, const void *data, u64 size) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", f->dir, file);
    ASSERT_EQ(de_file_write_all_atomic(path, data, size), DE_OK);
}

static void put_blob(lfx *f, const char *file, const lt_blob *b) {
    put(f, file, b->data, b->size);
}

static void manifest(lfx *f, const char *text) {
    put(f, "assets.toml", text, strlen(text));
    char path[160];
    snprintf(path, sizeof path, "%s/assets.toml", f->dir);
    ASSERT_EQ(de_asset_manifest_load(f->s, path), DE_OK);
}

/* One frame: copies, then the asset update. */
static void frame(lfx *f) {
    de_render_uploader_flush(f->u);
    de__asset_wake_reset(f->s);
    de_asset_system_update(f->s, NULL);
}

static bool final_state(de_load_state st) {
    return st == DE_LOAD_READY || st == DE_LOAD_PARTIAL || st == DE_LOAD_FAILED ||
           st == DE_LOAD_CANCELLED;
}

/* Frames until request `h` is final (or LT_WAIT_NS). */
static de_load_state pump(lfx *f, de_load_handle h) {
    const u64 t0 = de_time_now_ns();
    for (;;) {
        frame(f);
        de_load_state st = de_asset_status(f->s, h, NULL);
        if (final_state(st) || de_time_now_ns() - t0 > LT_WAIT_NS)
            return st;
        (void)de__asset_wake_block(f->s, 2000000u);
    }
}

/* Frames until `cond(f)` holds (or LT_WAIT_NS); false on timeout. */
static bool pump_until(lfx *f, bool (*cond)(lfx *f)) {
    const u64 t0 = de_time_now_ns();
    while (!cond(f)) {
        if (de_time_now_ns() - t0 > LT_WAIT_NS)
            return false;
        frame(f);
        (void)de__asset_wake_block(f->s, 2000000u);
    }
    return true;
}

static bool uploads_waiting(lfx *f) {
    return de_asset_get_stats(f->s).uploads_in_flight != 0;
}

/* Both textures of material_depends_textures wait for the fence, their copies submitted: only
 * then does lifting the fence hold make all of them ready in one update (DENG-413). */
static bool two_uploads_submitted(lfx *f) {
    return de_asset_get_stats(f->s).uploads_in_flight == 2u &&
           de_render_uploader_stats(f->u).queued == 0u;
}

static de_asset_id id(const char *path) {
    return de_asset_id_from(path);
}

static const char *first_error(lfx *f, de_load_handle h) {
    static de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    return info.first_error_msg;
}

/* ---- loaders.texture_publish_after_fence ------------------------------------------------------
 *
 * The GPU is "late" (the uploader sees its fence at 0): the copies are submitted, the asset waits
 * in WAIT_UPLOAD, its slot stays LOADING, no record is visible and uploads_in_flight counts it,
 * for as many frames as the fence holds. The update after the fence passes publishes it READY,
 * with its record, its bindless index and its GPU bytes in the texture budget. */
TEST_F(loaders, texture_publish_after_fence, lfx) {
    start(f, "publish_fence", (start_opts){0});
    lt_blob t = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 32, 16, 1, 3, 0, 1, false);
    put_blob(f, "a.dtex", &t);
    manifest(f, "[assets]\n\"tex/a\" = { type = \"texture\", source = \"a.dtex\" }\n");
    de__uploader_test_hold_fence(f->u, 0);
    de_texture tex = de_asset_texture(f->s, id("tex/a"));
    ASSERT_TRUE(DE_HANDLE_VALID(tex));
    de_load_handle h = de_asset_load(f->s, id("tex/a"), 0);
    ASSERT_TRUE(pump_until(f, uploads_waiting));
    EXPECT_EQ(de__pipeline_debug_stage(f->s, tex.id), (u32)DE__PJOB_WAIT_UPLOAD);
    for (u32 i = 0; i < 10; ++i) {
        frame(f);
        EXPECT_EQ((int)de_asset_status_of(f->s, id("tex/a")), (int)DE_LOAD_LOADING);
        EXPECT_TRUE(de_render_texture_get(f->s, tex) == NULL);
    }
    de_render_upload_stats us = de_render_uploader_stats(f->u);
    EXPECT_EQ(us.submitted, 1u); /* the copies went in the first frame */
    EXPECT_EQ(us.completed, 0u);
    EXPECT_EQ(de_asset_get_stats(f->s).uploads_in_flight, 1u);

    de__uploader_test_hold_fence(f->u, UINT64_MAX);
    frame(f); /* the fence has passed: published in this update */
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_READY);
    const de_render_texture *r = de_render_texture_get(f->s, tex);
    ASSERT_TRUE(r != NULL);
    EXPECT_EQ(r->width, 32u);
    EXPECT_EQ(r->height, 16u);
    EXPECT_EQ(r->mip_count, 3u);
    EXPECT_EQ(r->layers, 1u);
    EXPECT_EQ(r->format, (u32)DE_FMT_RGBA8_UNORM);
    EXPECT_FALSE(r->has_env);
    EXPECT_NE(r->bindless, 0u);
    EXPECT_EQ(r->bindless, de_gpu_texture_bindless(f->d, r->texture, 0, false));
    EXPECT_EQ(r->gpu_bytes, (u64)(32 * 16 + 16 * 8 + 8 * 4) * 4u);
    de_asset_stats st = de_asset_get_stats(f->s);
    EXPECT_EQ(st.uploads_in_flight, 0u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_GPU_TEXTURE], r->gpu_bytes);
    EXPECT_EQ(de_render_uploader_stats(f->u).pending_acquires, 1u); /* for the renderer */
    /* Without a renderer: the uploader's own graphics list takes them. */
    EXPECT_EQ(de_render_uploader_acquire_submit(f->u), 1u);
    EXPECT_EQ(de_render_uploader_stats(f->u).pending_acquires, 0u);
    EXPECT_EQ(de_render_uploader_acquire_submit(f->u), 0u);
    de_asset_release(f->s, h);
    lt_free(&t);
}

/* ---- loaders.split_across_frames --------------------------------------------------------------
 *
 * With a 64 KiB ring and a 64 KiB cap, a 128x128 RGBA8 texture (mip 0 alone fills the ring) and a
 * 20 000-vertex mesh (a 160 KiB position stream, cut into ring-sized pieces) upload over many
 * frames, through heap spills, and both end READY. */
TEST_F(loaders, split_across_frames, lfx) {
    start(f, "split", (start_opts){.ring_bytes = 64u << 10, .frame_cap = 64u << 10});
    lt_blob t = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 128, 128, 1, 8, 0, 3, false);
    lt_blob m = lt_dmsh(20000, 0, 4);
    put_blob(f, "t.dtex", &t);
    put_blob(f, "m.dmsh", &m);
    manifest(f, "[assets]\n\"big/t\" = { type = \"texture\", source = \"t.dtex\" }\n"
                "\"big/m\" = { type = \"mesh\", source = \"m.dmsh\" }\n");
    de_asset_id ids[2] = {id("big/t"), id("big/m")};
    de_load_handle h = de_asset_load_many(f->s, ids, 2, 0);
    EXPECT_EQ((int)pump(f, h), (int)DE_LOAD_READY);
    if (de_asset_status(f->s, h, NULL) != DE_LOAD_READY)
        printf("  %s\n", first_error(f, h));
    de_render_upload_stats us = de_render_uploader_stats(f->u);
    EXPECT_GE(us.spilled, 1u);
    EXPECT_GE(us.frames, (t.size + m.size) / (64u << 10));
    const de_render_mesh *rm = de_render_mesh_get(f->s, de_asset_mesh(f->s, ids[1]));
    ASSERT_TRUE(rm != NULL);
    EXPECT_EQ(rm->vertex_count, 20000u);
    EXPECT_EQ(rm->buffer_bytes[DE_RENDER_MESH_VERTICES],
              lt_align(20000u * 8u, 256) + lt_align(20000u * 4u, 256) + 20000u * 4u);
    de_asset_release(f->s, h);
    lt_free(&t);
    lt_free(&m);
}

/* ---- loaders.material_depends_textures -------------------------------------------------------
 *
 * A material is READY only when its textures are: while their uploads wait for the fence the
 * material waits too (its own load finished long before), and it is published in the same update
 * as its last texture. Its record then holds the texture handles in the slots and in the
 * standard parameters, and the cooked factors. */
TEST_F(loaders, material_depends_textures, lfx) {
    start(f, "material_deps", (start_opts){0});
    lt_blob a = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_SRGB, 16, 16, 1, 1, 0, 1, false);
    lt_blob n = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 16, 16, 1, 1, 1, 2, false);
    const u32 slots[2] = {DE_COOKED_TEX_BASE_COLOR, DE_COOKED_TEX_NORMAL};
    const u64 tids[2] = {id("mat/albedo").hash, id("mat/normal").hash};
    lt_blob m = lt_dmat(slots, tids, 2, 0.5f);
    put_blob(f, "albedo.dtex", &a);
    put_blob(f, "normal.dtex", &n);
    put_blob(f, "m.dmat", &m);
    manifest(f, "[assets]\n\"mat/albedo\" = { type = \"texture\", source = \"albedo.dtex\" }\n"
                "\"mat/normal\" = { type = \"texture\", source = \"normal.dtex\" }\n"
                "\"mat/m\" = { type = \"material\", source = \"m.dmat\" }\n");
    de__uploader_test_hold_fence(f->u, 0);
    de_load_handle h = de_asset_load(f->s, id("mat/m"), 0);
    /* Not "some upload waits": the second texture may still be loading on a slow worker (TSan),
     * and then it is rightly not READY one update after the hold is lifted (DENG-413). */
    ASSERT_TRUE(pump_until(f, two_uploads_submitted));
    for (u32 i = 0; i < 5; ++i) {
        frame(f);
        EXPECT_EQ((int)de_asset_status_of(f->s, id("mat/m")), (int)DE_LOAD_LOADING);
        EXPECT_EQ((int)de_asset_status_of(f->s, id("mat/albedo")), (int)DE_LOAD_LOADING);
        EXPECT_EQ((int)de_asset_status_of(f->s, id("mat/normal")), (int)DE_LOAD_LOADING);
    }
    EXPECT_EQ(de_asset_get_stats(f->s).uploads_in_flight, 2u);
    de__uploader_test_hold_fence(f->u, UINT64_MAX);
    frame(f);
    EXPECT_EQ((int)de_asset_status_of(f->s, id("mat/albedo")), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(f->s, id("mat/normal")), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(f->s, id("mat/m")), (int)DE_LOAD_READY); /* same update */
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_READY);
    const de_render_material *r =
        de_render_material_get(f->s, de_asset_material(f->s, id("mat/m")));
    ASSERT_TRUE(r != NULL);
    const de_texture ta = de_asset_texture(f->s, id("mat/albedo"));
    const de_texture tn = de_asset_texture(f->s, id("mat/normal"));
    EXPECT_EQ(r->textures[DE_COOKED_TEX_BASE_COLOR].id, ta.id);
    EXPECT_EQ(r->textures[DE_COOKED_TEX_NORMAL].id, tn.id);
    EXPECT_EQ(r->standard.base_color_tex.id, ta.id);
    EXPECT_EQ(r->standard.normal_tex.id, tn.id);
    EXPECT_EQ(r->standard.occlusion_tex.id, 0u);
    EXPECT_EQ(r->texture_ids[DE_COOKED_TEX_NORMAL].hash, tids[1]);
    EXPECT_FLOAT_EQ(r->standard.base_color.x, 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(r->standard.roughness, 0.25f, 0.0f);
    EXPECT_FLOAT_EQ(r->standard.ior, 1.5f, 0.0f);
    EXPECT_EQ(r->standard.alpha_mode, (u32)DE_COOKED_ALPHA_MASK);
    EXPECT_TRUE(r->standard.double_sided);
    EXPECT_TRUE(de_render_texture_get(f->s, ta) != NULL);
    de_asset_release(f->s, h);
    lt_free(&a);
    lt_free(&n);
    lt_free(&m);
}

/* ---- loaders.material_partial_on_texture_fail ------------------------------------------------
 *
 * A bundle of a material and its two textures, one of them damaged: the good texture is READY,
 * the bad one and the material FAILED, so the request is PARTIAL; de_load_info reports the first
 * failure in request order with the texture's parse error, and the material's message names the
 * texture. */
TEST_F(loaders, material_partial_on_texture_fail, lfx) {
    start(f, "material_partial", (start_opts){0});
    lt_blob a = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_SRGB, 16, 16, 1, 1, 0, 1, false);
    const u32 slots[2] = {DE_COOKED_TEX_BASE_COLOR, DE_COOKED_TEX_EMISSIVE};
    const u64 tids[2] = {id("bad/good").hash, id("bad/broken").hash};
    lt_blob m = lt_dmat(slots, tids, 2, 1.0f);
    put_blob(f, "good.dtex", &a);
    a.data[60] = 1; /* a reserved header field: no longer a valid DTEX */
    put_blob(f, "broken.dtex", &a);
    put_blob(f, "m.dmat", &m);
    manifest(f, "[assets]\n\"bad/m\" = { type = \"material\", source = \"m.dmat\" }\n"
                "\"bad/good\" = { type = \"texture\", source = \"good.dtex\" }\n"
                "\"bad/broken\" = { type = \"texture\", source = \"broken.dtex\" }\n"
                "[bundles.mat]\nassets = [\"bad/*\"]\n");
    de_load_handle h = de_asset_load_bundle(f->s, "mat", 0);
    EXPECT_EQ((int)pump(f, h), (int)DE_LOAD_PARTIAL);
    de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    EXPECT_EQ(info.assets_total, 3u);
    EXPECT_EQ(info.assets_ready, 1u);
    EXPECT_EQ(info.assets_failed, 2u);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_PARSE);
    printf("  first error: %s\n", info.first_error_msg);
    EXPECT_TRUE(strstr(info.first_error_msg, "not a cooked texture") != NULL);
    EXPECT_EQ((int)de_asset_status_of(f->s, id("bad/good")), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(f->s, id("bad/broken")), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)de_asset_status_of(f->s, id("bad/m")), (int)DE_LOAD_FAILED);
    char msg[512];
    EXPECT_EQ((int)de_asset_last_error(f->s, id("bad/m"), msg, sizeof msg), (int)DE_ERR_PARSE);
    EXPECT_TRUE(strstr(msg, "bad/broken") != NULL);
    de_asset_release(f->s, h);
    lt_free(&a);
    lt_free(&m);
}

/* ---- loaders.cancel_during_upload -------------------------------------------------------------
 *
 * A request released while its texture waits for the fence: the job is discarded, the texture's
 * upload leaves the uploader with its GPU objects (no leak, nothing queued, no acquire left), and a
 * second load of the same asset works. */
TEST_F(loaders, cancel_during_upload, lfx) {
    start(f, "cancel", (start_opts){0});
    lt_blob t = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 64, 64, 1, 1, 0, 1, false);
    put_blob(f, "t.dtex", &t);
    manifest(f, "[assets]\n\"c/t\" = { type = \"texture\", source = \"t.dtex\" }\n");
    de__uploader_test_hold_fence(f->u, 0);
    de_load_handle h = de_asset_load(f->s, id("c/t"), 0);
    ASSERT_TRUE(pump_until(f, uploads_waiting));
    de_asset_release(f->s, h);
    frame(f);
    EXPECT_EQ(de_asset_get_stats(f->s).uploads_in_flight, 0u);
    de_render_upload_stats us = de_render_uploader_stats(f->u);
    EXPECT_EQ(us.queued, 0u);
    EXPECT_EQ(us.pending_acquires, 0u);
    de__uploader_test_hold_fence(f->u, UINT64_MAX);
    h = de_asset_load(f->s, id("c/t"), 0);
    EXPECT_EQ((int)pump(f, h), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);
    lt_free(&t);
}

/* ---- loaders.load_failures --------------------------------------------------------------------
 *
 * Damaged blobs fail with DE_ERR_PARSE and the parser's reason; a texture whose mip 0 is larger
 * than the ring fails with DE_ERR_UNSUPPORTED; registering the types twice is refused. */
TEST_F(loaders, load_failures, lfx) {
    start(f, "failures", (start_opts){.ring_bytes = 16u << 10});
    lt_blob t = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 128, 128, 1, 1, 0, 1, false); /* 64 KiB */
    lt_blob m = lt_dmsh(10, 0, 1);
    m.data[4] = 9; /* version */
    put_blob(f, "big.dtex", &t);
    put_blob(f, "bad.dmsh", &m);
    put(f, "bad.dmat", "DMAT", 4);
    put(f, "bad.dmdl", "not a model", 11);
    manifest(f, "[assets]\n\"f/big\" = { type = \"texture\", source = \"big.dtex\" }\n"
                "\"f/mesh\" = { type = \"mesh\", source = \"bad.dmsh\" }\n"
                "\"f/mat\" = { type = \"material\", source = \"bad.dmat\" }\n"
                "\"f/model\" = { type = \"model\", source = \"bad.dmdl\" }\n");
    static const char *const want[4][2] = {{"f/big", "does not fit in the 16384-byte upload ring"},
                                           {"f/mesh", "not a cooked mesh"},
                                           {"f/mat", "not a cooked material"},
                                           {"f/model", "not a cooked model"}};
    for (u32 i = 0; i < 4; ++i) {
        de_load_handle h = de_asset_load(f->s, id(want[i][0]), 0);
        EXPECT_EQ((int)pump(f, h), (int)DE_LOAD_FAILED);
        const char *msg = first_error(f, h);
        if (strstr(msg, want[i][1]) == NULL)
            printf("  %s: \"%s\"\n", want[i][0], msg);
        EXPECT_TRUE(strstr(msg, want[i][1]) != NULL);
        de_load_info info;
        (void)de_asset_status(f->s, h, &info);
        EXPECT_EQ((int)info.first_error, i == 0 ? (int)DE_ERR_UNSUPPORTED : (int)DE_ERR_PARSE);
        de_asset_release(f->s, h);
    }
    de_render_uploader *again = NULL;
    EXPECT_EQ(de_render_register_asset_types(f->s, f->d, &f->alloc, &again), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(again == NULL);
    EXPECT_EQ(de_render_register_asset_types(NULL, f->d, NULL, &again), DE_ERR_INVALID_ARGUMENT);
    lt_free(&t);
    lt_free(&m);
}

/* ---- loaders.hot_reload_swap ------------------------------------------------------------------
 *
 * A texture edited on disk reloads behind the same handle: while the new copies wait for the
 * fence the old object stays published (no stall: the slot stays READY and serves the old data),
 * then one update swaps the new object in (new GPU texture, new size, same handle, one RELOADED),
 * and reload_retire_frames updates later the old GPU texture is destroyed (deferred by the RHI). */
TEST_F(loaders, hot_reload_swap, lfx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled out of this build (Release, Shipping)");
#else
    start(f, "hot_reload", (start_opts){.hot_reload = true});
    de__asset_reload_set_watching(f->s, false);
    lt_blob t1 = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 64, 64, 1, 1, 0, 1, false);
    lt_blob t2 = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 32, 32, 1, 2, 0, 2, false);
    put_blob(f, "t.dtex", &t1);
    manifest(f, "[assets]\n\"hr/t\" = { type = \"texture\", source = \"t.dtex\" }\n");
    de_texture tex = de_asset_texture(f->s, id("hr/t"));
    de_load_handle h = de_asset_load(f->s, id("hr/t"), 0);
    ASSERT_EQ((int)pump(f, h), (int)DE_LOAD_READY);
    const de_render_texture *r1 = de_render_texture_get(f->s, tex);
    ASSERT_TRUE(r1 != NULL);
    const de_gpu_texture old = r1->texture;

    put_blob(f, "t.dtex", &t2);
    char path[160];
    snprintf(path, sizeof path, "%s/t.dtex", f->dir);
    de__uploader_test_hold_fence(f->u, 0);
    de__asset_reload_on_change(f->s, path, DE_WATCH_MODIFIED, false);
    ASSERT_TRUE(pump_until(f, uploads_waiting));
    for (u32 i = 0; i < 5; ++i) {
        frame(f);
        EXPECT_EQ((int)de_asset_status_of(f->s, id("hr/t")), (int)DE_LOAD_READY);
        const de_render_texture *cur = de_render_texture_get(f->s, tex);
        ASSERT_TRUE(cur != NULL);
        EXPECT_EQ(cur->texture.id, old.id); /* still the old one */
    }
    de__asset_reload_stats rs;
    de__asset_reload_get_stats(f->s, &rs);
    EXPECT_EQ(rs.swapped, 0u);
    de__uploader_test_hold_fence(f->u, UINT64_MAX);
    frame(f);
    de__asset_reload_get_stats(f->s, &rs);
    EXPECT_EQ(rs.swapped, 1u);
    const de_render_texture *r2 = de_render_texture_get(f->s, tex);
    ASSERT_TRUE(r2 != NULL);
    EXPECT_NE(r2->texture.id, old.id);
    EXPECT_EQ(r2->width, 32u);
    EXPECT_EQ(r2->mip_count, 2u);
    EXPECT_NE(de_gpu_texture_bindless(f->d, old, 0, false), 0u); /* retired, not destroyed yet */
    for (u32 i = 0; i < 4; ++i)
        frame(f);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, old, 0, false), 0u); /* unloaded: handle stale */
    EXPECT_NE(de_gpu_texture_bindless(f->d, r2->texture, 0, false), 0u);
    de_asset_release(f->s, h);
    lt_free(&t1);
    lt_free(&t2);
#endif
}

/* ---- Readback on GPU backends ------------------------------------------------------------------
 */

/* Compares `read` (from lt_readback) with subresource `idx` of `blob`, row data only. */
static bool rows_match(const u8 *rb, const lt_tex_read *read, const lt_blob *blob, u32 idx,
                       u32 format) {
    de_cooked_texture_subresource s = lt_dtex_sub(blob, idx);
    de_cooked_texture_block blk;
    if (!de_cooked_texture_format_block(format, &blk))
        return false;
    const u64 row = de_cooked_texture_row_bytes(&blk, s.width);
    if (s.row_pitch != read->row_pitch || s.row_count != read->rows)
        return false;
    for (u32 r = 0; r < s.row_count; ++r)
        if (memcmp(rb + read->dst_off + (u64)r * s.row_pitch,
                   blob->data + s.offset + (u64)r * s.row_pitch, (size_t)row) != 0) {
            printf("  subresource %u: row %u differs\n", idx, r);
            return false;
        }
    return true;
}

/* Loads `n` textures; SKIPs when the device has no such format. */
static void load_textures(lfx *f, const de_asset_id *ids, u32 n) {
    de_load_handle h = de_asset_load_many(f->s, ids, n, 0);
    de_load_state st = pump(f, h);
    if (st != DE_LOAD_READY) {
        const char *msg = first_error(f, h);
        if (strstr(msg, "cannot create the GPU texture") != NULL) {
            snprintf(f->why, sizeof f->why, "this device cannot create the texture: %s", msg);
            de_asset_release(f->s, h);
            SKIP(f->why);
        }
        printf("  %s\n", msg);
    }
    ASSERT_EQ((int)st, (int)DE_LOAD_READY);
    /* The request stays: the assets are not evicted (no budgets) and stay READY. */
}

/* A BC7 mip chain and a BC6H environment cube read back as their cooked bytes: every mip of the
 * BC7 texture and mip 0 of every face, after the acquire on the graphics queue. */
static void run_texture_readback(lfx *f, de_gpu_backend b, const char *name) {
    start(f, name, (start_opts){.backend = b});
    lt_blob bc7 = lt_dtex(DE_TEX_2D, DE_FMT_BC7_UNORM, 64, 64, 1, 7, 0, 5, false);
    lt_blob env = lt_env(DE_FMT_BC6H_UFLOAT, 16, 6);
    put_blob(f, "bc7.dtex", &bc7);
    put_blob(f, "env.dtex", &env);
    manifest(f, "[assets]\n\"rb/bc7\" = { type = \"texture\", source = \"bc7.dtex\" }\n"
                "\"rb/env\" = { type = \"texture\", source = \"env.dtex\" }\n");
    const de_asset_id ids[2] = {id("rb/bc7"), id("rb/env")};
    load_textures(f, ids, 2);
    const de_render_texture *t7 = de_render_texture_get(f->s, de_asset_texture(f->s, ids[0]));
    const de_render_texture *te = de_render_texture_get(f->s, de_asset_texture(f->s, ids[1]));
    ASSERT_TRUE(t7 != NULL && te != NULL);
    EXPECT_EQ(t7->mip_count, 7u);
    EXPECT_EQ(te->dim, (u32)DE_TEX_CUBE);
    EXPECT_EQ(te->layers, 6u);
    EXPECT_EQ(te->format, (u32)DE_FMT_BC6H_UFLOAT);
    EXPECT_TRUE(te->has_env);
    EXPECT_EQ(te->env.face_size, 16u);
    EXPECT_EQ(te->env.mip_count, 5u);
    EXPECT_FLOAT_EQ(te->env.mip_per_roughness, 4.0f, 0.0f);
    EXPECT_FLOAT_EQ(te->env.rotation_deg, 30.0f, 0.0f);
    EXPECT_FLOAT_EQ(te->env.sh9[8][2], 8.2f, 1e-6f);

    lt_tex_read reads[7];
    for (u32 m = 0; m < 7; ++m) {
        de_cooked_texture_subresource s = lt_dtex_sub(&bc7, m);
        reads[m] =
            (lt_tex_read){.mip = m, .layer = 0, .row_pitch = s.row_pitch, .rows = s.row_count};
    }
    u8 *rb = NULL;
    u64 rb_size = 0;
    ASSERT_TRUE(lt_readback(f->d, f->u, t7->texture, reads, 7, NULL, NULL, NULL, 0, &rb, &rb_size));
    for (u32 m = 0; m < 7; ++m)
        EXPECT_TRUE(rows_match(rb, &reads[m], &bc7, m, DE_FMT_BC7_UNORM));
    free(rb);

    lt_tex_read faces[6];
    for (u32 l = 0; l < 6; ++l) {
        de_cooked_texture_subresource s = lt_dtex_sub(&env, l * 5u);
        faces[l] =
            (lt_tex_read){.mip = 0, .layer = l, .row_pitch = s.row_pitch, .rows = s.row_count};
    }
    ASSERT_TRUE(lt_readback(f->d, f->u, te->texture, faces, 6, NULL, NULL, NULL, 0, &rb, &rb_size));
    for (u32 l = 0; l < 6; ++l)
        EXPECT_TRUE(rows_match(rb, &faces[l], &env, l * 5u, DE_FMT_BC6H_UFLOAT));
    free(rb);
    lt_free(&bc7);
    lt_free(&env);
}

TEST_F(loaders, texture_readback_metal, lfx) {
    run_texture_readback(f, DE_GPU_METAL, "readback_metal");
}
TEST_F(loaders, texture_readback_vulkan, lfx) {
    run_texture_readback(f, DE_GPU_VULKAN, "readback_vulkan");
}
TEST_F(loaders, texture_readback_d3d12, lfx) {
    run_texture_readback(f, DE_GPU_D3D12, "readback_d3d12");
}

/* A mesh's five buffers hold its sections byte for byte (each stream at its offset of the vertex
 * buffer), their bindless indices are non-zero, distinct, the RHI's, and the same many frames
 * later. On the null backend (copies are no-ops) only the record and the indices are checked. */
static void run_mesh_roundtrip(lfx *f, de_gpu_backend b, const char *name) {
    start(f, name, (start_opts){.backend = b});
    lt_blob m = lt_dmsh(300, 0x1234, 7);
    put_blob(f, "m.dmsh", &m);
    manifest(f, "[assets]\n\"rt/m\" = { type = \"mesh\", source = \"m.dmsh\" }\n");
    de_mesh mh = de_asset_mesh(f->s, id("rt/m"));
    de_load_handle h = de_asset_load(f->s, id("rt/m"), 0);
    ASSERT_EQ((int)pump(f, h), (int)DE_LOAD_READY);
    const de_render_mesh *r = de_render_mesh_get(f->s, mh);
    ASSERT_TRUE(r != NULL);
    de_cooked_mesh_view v;
    ASSERT_TRUE(de_cooked_mesh_parse(m.data, m.size, &v, NULL));
    EXPECT_EQ(r->vertex_count, 300u);
    EXPECT_EQ(r->index_count, v.index_count);
    EXPECT_EQ(r->meshlet_count, v.meshlet_count);
    EXPECT_EQ(r->lod_count, 1u);
    EXPECT_EQ(r->lods[0].triangle_count, 298u);
    EXPECT_EQ(r->submesh_count, 1u);
    EXPECT_EQ(r->submeshes[0].material.hash, 0x1234u);
    EXPECT_EQ(r->lod_submeshes[0].meshlet_count, v.meshlet_count);
    EXPECT_EQ(r->stream_offset[DE_COOKED_STREAM_POSITION], 0u);
    EXPECT_EQ(r->stream_offset[DE_COOKED_STREAM_TANGENT], UINT32_MAX);
    EXPECT_EQ(r->stream_format[DE_COOKED_STREAM_UV0], (u32)DE_COOKED_VF_UNORM16X2);
    u32 bindless[DE_RENDER_MESH_BUFFER_COUNT];
    for (u32 i = 0; i < DE_RENDER_MESH_BUFFER_COUNT; ++i) {
        bindless[i] = r->bindless[i];
        EXPECT_NE(r->bindless[i], 0u);
        EXPECT_EQ(r->bindless[i], de_gpu_buffer_bindless(f->d, r->buffers[i]));
        for (u32 k = 0; k < i; ++k)
            EXPECT_NE(r->bindless[i], r->bindless[k]);
    }
    const de_asset_stats ast = de_asset_get_stats(f->s);
    EXPECT_EQ(ast.bytes_resident[DE_ASSET_BYTES_GPU_MESH], r->gpu_bytes);
    for (u32 i = 0; i < 10; ++i)
        frame(f);
    ASSERT_TRUE(de_render_mesh_get(f->s, mh) == r);
    for (u32 i = 0; i < DE_RENDER_MESH_BUFFER_COUNT; ++i)
        EXPECT_EQ(r->bindless[i], bindless[i]);

    if (b != DE_GPU_NULL) {
        u64 offs[DE_RENDER_MESH_BUFFER_COUNT];
        u8 *rb = NULL;
        u64 rb_size = 0;
        ASSERT_TRUE(lt_readback(f->d, f->u, (de_gpu_texture){0}, NULL, 0, r->buffers,
                                r->buffer_bytes, offs, DE_RENDER_MESH_BUFFER_COUNT, &rb, &rb_size));
        const u8 *vb = rb + offs[DE_RENDER_MESH_VERTICES];
        for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s) {
            if (v.streams[s] == NULL)
                continue;
            const u64 len = (u64)v.vertex_count * de_cooked_vertex_format_size(v.stream_format[s]);
            EXPECT_MEMEQ(vb + r->stream_offset[s], v.streams[s], (size_t)len);
        }
        EXPECT_MEMEQ(rb + offs[DE_RENDER_MESH_MESHLETS], v.meshlets,
                     (size_t)r->buffer_bytes[DE_RENDER_MESH_MESHLETS]);
        EXPECT_MEMEQ(rb + offs[DE_RENDER_MESH_MESHLET_VERTICES], v.meshlet_vertices,
                     (size_t)r->buffer_bytes[DE_RENDER_MESH_MESHLET_VERTICES]);
        EXPECT_MEMEQ(rb + offs[DE_RENDER_MESH_MESHLET_TRIANGLES], v.meshlet_triangles,
                     (size_t)r->buffer_bytes[DE_RENDER_MESH_MESHLET_TRIANGLES]);
        EXPECT_MEMEQ(rb + offs[DE_RENDER_MESH_INDICES], v.indices,
                     (size_t)r->buffer_bytes[DE_RENDER_MESH_INDICES]);
        free(rb);
    }
    de_asset_release(f->s, h);
    lt_free(&m);
}

TEST_F(loaders, mesh_roundtrip_null, lfx) {
    run_mesh_roundtrip(f, DE_GPU_NULL, "mesh_null");
}

/* With de__render_keep_cpu_meshes on (the editor service, DENG-650), a mesh keeps LOD 0's
 * positions and triangles on the CPU, equal to the cooked ones; off, it keeps none. */
TEST_F(loaders, mesh_keeps_cpu_lod0, lfx) {
    start(f, "mesh_cpu", (start_opts){.backend = DE_GPU_NULL});
    lt_blob m = lt_dmsh(300, 0x1234, 7);
    put_blob(f, "m.dmsh", &m);
    put_blob(f, "n.dmsh", &m);
    manifest(f, "[assets]\n\"rt/m\" = { type = \"mesh\", source = \"m.dmsh\" }\n"
                "\"rt/n\" = { type = \"mesh\", source = \"n.dmsh\" }\n");
    de__render_keep_cpu_meshes(true);
    de_load_handle h = de_asset_load(f->s, id("rt/m"), 0);
    ASSERT_EQ((int)pump(f, h), (int)DE_LOAD_READY);
    de__render_keep_cpu_meshes(false);
    de_load_handle h2 = de_asset_load(f->s, id("rt/n"), 0);
    ASSERT_EQ((int)pump(f, h2), (int)DE_LOAD_READY);
    const de_render_mesh *r = de_render_mesh_get(f->s, de_asset_mesh(f->s, id("rt/m")));
    ASSERT_TRUE(r != NULL);
    de_cooked_mesh_view v;
    ASSERT_TRUE(de_cooked_mesh_parse(m.data, m.size, &v, NULL));
    const de_cooked_mesh_lod l0 = de_cooked_mesh_get_lod(&v, 0);
    ASSERT_TRUE(r->cpu_positions != NULL && r->cpu_triangles != NULL);
    EXPECT_EQ(r->cpu_vertex_count, l0.vertex_count);
    EXPECT_EQ(r->cpu_triangle_count, l0.index_count / 3u);
    for (u32 i = 0; i < r->cpu_vertex_count; ++i) {
        f32 p[3];
        de_cooked_mesh_position(&v, i, p);
        EXPECT_MEMEQ(r->cpu_positions + 3u * i, p, sizeof p);
    }
    for (u32 i = 0; i < r->cpu_triangle_count * 3u; ++i)
        EXPECT_EQ(r->cpu_triangles[i], de_cooked_mesh_index(&v, l0.index_first + i));
    const de_render_mesh *r2 = de_render_mesh_get(f->s, de_asset_mesh(f->s, id("rt/n")));
    ASSERT_TRUE(r2 != NULL);
    EXPECT_TRUE(r2->cpu_positions == NULL && r2->cpu_triangles == NULL);
    EXPECT_EQ(r2->cpu_triangle_count, 0u);
    de_asset_release(f->s, h);
    de_asset_release(f->s, h2);
    lt_free(&m);
}
TEST_F(loaders, mesh_roundtrip_metal, lfx) {
    run_mesh_roundtrip(f, DE_GPU_METAL, "mesh_metal");
}
TEST_F(loaders, mesh_roundtrip_vulkan, lfx) {
    run_mesh_roundtrip(f, DE_GPU_VULKAN, "mesh_vulkan");
}
TEST_F(loaders, mesh_roundtrip_d3d12, lfx) {
    run_mesh_roundtrip(f, DE_GPU_D3D12, "mesh_d3d12");
}

/* ---- With the cooker
 * ----------------------------------------------------------------------------- */

#if DE_HAS_COOKER
static void copy_file(lfx *f, const char *from, const char *to) {
    void *p = NULL;
    u64 n = 0;
    ASSERT_EQ(de_file_read_all(from, &f->alloc, &p, &n), DE_OK);
    put(f, to, p, n);
    DE_FREE(&f->alloc, p, (size_t)n + 1u);
}

static void mount_import_data(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("rlimp", DE_RL_TEST_IMPORT_DATA_DIR, 0, 0), DE_OK);
        mounted = true;
    }
}
#endif

/* ---- loaders.model_cook_end_to_end ------------------------------------------------------------
 *
 * The registered importers in a cooker build: BoxTextured.gltf cooks in-process into a model and
 * its sub-assets (texture through de_texture_import, material, mesh, prefab), and the model is
 * READY with all of them. Its record names the mesh and material handles, the mesh has its GPU
 * buffers, the material its base-colour texture (a BC7 sRGB texture), and de_model_prefab gives
 * the `#prefab` sub-asset, which de_asset_prefab refuses for the model's own ID. */
TEST_F(loaders, model_cook_end_to_end, lfx) {
#if !DE_HAS_COOKER
    SKIP("this build does not link the cooker into the runtime (DE_HAS_COOKER)");
#else
    mount_import_data();
    start(f, "model_e2e", (start_opts){.import = true, .types = true});
    copy_file(f, "rlimp:/gltf/BoxTextured.gltf", "BoxTextured.gltf");
    copy_file(f, "rlimp:/gltf/BoxTextured0.bin", "BoxTextured0.bin");
    copy_file(f, "rlimp:/gltf/CesiumLogoFlat.png", "CesiumLogoFlat.png");
    manifest(f, "[assets]\n\"props/box\" = { type = \"model\", source = \"BoxTextured.gltf\" }\n");
    de_model mh = de_asset_model(f->s, id("props/box"));
    ASSERT_TRUE(DE_HANDLE_VALID(mh));
    de_load_handle h = de_asset_load(f->s, id("props/box"), 0);
    de_load_state st = pump(f, h);
    if (st != DE_LOAD_READY)
        printf("  %s\n", first_error(f, h));
    ASSERT_EQ((int)st, (int)DE_LOAD_READY);
    const de_render_model *m = de_render_model_get(f->s, mh);
    ASSERT_TRUE(m != NULL);
    EXPECT_GE(m->node_count, 1u);
    EXPECT_EQ(m->mesh_count, 1u);
    EXPECT_EQ(m->material_count, 1u);
    u32 with_mesh = 0;
    for (u32 i = 0; i < m->node_count; ++i)
        if (DE_HANDLE_VALID(m->nodes[i].mesh)) {
            with_mesh++;
            EXPECT_EQ(m->nodes[i].mesh.id, m->meshes[0].id);
            const de__slot *slot = de__slot_get(f->s, m->nodes[i].mesh.id);
            ASSERT_TRUE(slot != NULL);
            size_t needed = 0;
            ASSERT_EQ(de__cook_stage_subasset_path(f->s, slot->id, NULL, 0, &needed), DE_OK);
            char path[512], expected[512];
            memset(path, 0xb3, sizeof path);
            size_t sentinel = 7654;
            EXPECT_EQ(de__cook_stage_subasset_path(f->s, slot->id, path, needed - 1, &sentinel),
                      DE_ERR_INVALID_ARGUMENT);
            EXPECT_EQ(sentinel, 7654u);
            for (size_t j = 0; j < sizeof path; ++j)
                EXPECT_EQ((u8)path[j], 0xb3u);
            ASSERT_EQ(de__cook_stage_subasset_path(f->s, slot->id, path, sizeof path, &needed),
                      DE_OK);
            (void)snprintf(expected, sizeof expected, "props/box#%s", m->nodes[i].name);
            EXPECT_STREQ(path, expected);
            EXPECT_EQ(de_asset_id_from(path).hash, slot->id.hash);
            EXPECT_EQ(needed, strlen(path) + 1);
        }
    EXPECT_EQ(with_mesh, 1u);
    const de_render_mesh *rm = de_render_mesh_get(f->s, m->meshes[0]);
    ASSERT_TRUE(rm != NULL);
    EXPECT_GE(rm->vertex_count, 8u);
    EXPECT_NE(rm->bindless[DE_RENDER_MESH_INDICES], 0u);
    /* The submesh's default material is the model's material. */
    const de__slot *ms = de__slot_get(f->s, m->materials[0].id);
    ASSERT_TRUE(ms != NULL);
    EXPECT_EQ(rm->submeshes[0].material.hash, ms->id.hash);
    const de_render_material *mat = de_render_material_get(f->s, m->materials[0]);
    ASSERT_TRUE(mat != NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(mat->standard.base_color_tex));
    const de_render_texture *tex = de_render_texture_get(f->s, mat->standard.base_color_tex);
    ASSERT_TRUE(tex != NULL);
    EXPECT_TRUE(de_cooked_texture_format_is_srgb(tex->format)); /* a colour texture */
    EXPECT_GT(tex->mip_count, 1u);
    /* The prefab: by the model's handle, or by the sub-asset's ID; not by the model's ID. */
    de_prefab p = de_model_prefab(f->s, mh);
    EXPECT_TRUE(DE_HANDLE_VALID(p));
    EXPECT_EQ(p.id, m->prefab.id);
    EXPECT_EQ(p.id, de_asset_prefab(f->s, id("props/box#prefab")).id);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_prefab(f->s, id("props/box"))));
    EXPECT_FALSE(DE_HANDLE_VALID(de_model_prefab(f->s, (de_model){0})));
    de_asset_release(f->s, h);
#endif
}

/* ---- loaders.env_cook_readback_<backend>
 * ---------------------------------------------------------
 *
 * An HDR environment cooked by the texture importer (env.exr, a 16-texel BC6H cube) loads as a
 * cube with its trailer, and mip 0 of every face reads back as the cooked bytes. */
static void run_env_cook_readback(lfx *f, de_gpu_backend b, const char *name) {
#if !DE_HAS_COOKER
    (void)f;
    (void)b;
    (void)name;
    SKIP("this build does not link the cooker into the runtime (DE_HAS_COOKER)");
#else
    mount_import_data();
    start(f, name, (start_opts){.backend = b});
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc}, &c), DE_OK);
    de_asset_type_desc tt = de_texture_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(c, &tt), DE_OK);
    de__manifest_kv kv[3];
    memset(kv, 0, sizeof kv);
    kv[0].key = "usage";
    kv[0].value.type = DE__MANIFEST_STRING;
    kv[0].value.u.s = "environment";
    kv[1].key = "env_size";
    kv[1].value.type = DE__MANIFEST_INT;
    kv[1].value.u.i = 16;
    kv[2].key = "env_samples";
    kv[2].value.type = DE__MANIFEST_INT;
    kv[2].value.u.i = 16;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "env/sky";
    a.type = "texture";
    a.source = "env.exr";
    a.file = "rlimp:/texture_bc/assets.toml";
    a.settings = kv;
    a.setting_count = 3;
    de_cook_result cr;
    de_result r =
        de_cooker_cook(c, &(de_cook_input){.asset = &a, .asset_root = "rlimp:/texture_bc"}, &cr);
    if (r != DE_OK)
        printf("  cook: %s\n", de_error_last() ? de_error_last()->message : "?");
    ASSERT_EQ(r, DE_OK);
    lt_blob env = {(u8 *)malloc((size_t)cr.size), cr.size};
    memcpy(env.data, cr.data, (size_t)cr.size);
    de_cook_result_free(&cr);
    de_cooker_destroy(c);
    de_cooked_env_view ev;
    ASSERT_TRUE(de_cooked_env_parse(env.data, env.size, &ev, NULL));
    put_blob(f, "sky.dtex", &env);
    manifest(f, "[assets]\n\"env/sky\" = { type = \"texture\", source = \"sky.dtex\" }\n");
    const de_asset_id ids[1] = {id("env/sky")};
    load_textures(f, ids, 1);
    const de_render_texture *t = de_render_texture_get(f->s, de_asset_texture(f->s, ids[0]));
    ASSERT_TRUE(t != NULL);
    EXPECT_TRUE(t->has_env);
    EXPECT_EQ(t->dim, (u32)DE_TEX_CUBE);
    EXPECT_EQ(t->width, 16u);
    EXPECT_EQ(t->format, ev.texture.format);
    EXPECT_FLOAT_EQ(t->env.sh9[0][0], ev.sh9[0][0], 0.0f);
    lt_tex_read faces[6];
    for (u32 l = 0; l < 6; ++l) {
        de_cooked_texture_subresource s = lt_dtex_sub(&env, l * ev.texture.mip_count);
        faces[l] =
            (lt_tex_read){.mip = 0, .layer = l, .row_pitch = s.row_pitch, .rows = s.row_count};
    }
    u8 *rb = NULL;
    u64 rb_size = 0;
    ASSERT_TRUE(lt_readback(f->d, f->u, t->texture, faces, 6, NULL, NULL, NULL, 0, &rb, &rb_size));
    for (u32 l = 0; l < 6; ++l)
        EXPECT_TRUE(rows_match(rb, &faces[l], &env, l * ev.texture.mip_count, ev.texture.format));
    free(rb);
    lt_free(&env);
#endif
}

TEST_F(loaders, env_cook_readback_metal, lfx) {
    run_env_cook_readback(f, DE_GPU_METAL, "env_metal");
}
TEST_F(loaders, env_cook_readback_vulkan, lfx) {
    run_env_cook_readback(f, DE_GPU_VULKAN, "env_vulkan");
}
TEST_F(loaders, env_cook_readback_d3d12, lfx) {
    run_env_cook_readback(f, DE_GPU_D3D12, "env_d3d12");
}
