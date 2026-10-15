/*
 * test_render_churn.c: asset unloads against the render thread (DENG-422).
 *
 * The rule under test (render/loaders/render_assets.h, "Deferred unloads", and
 * render/scene/gpu_scene.h, Threads):
 *   - the render thread never reads asset records: the GPU scene resolves meshes, materials and
 *     textures on the main thread at the kick (de__gpu_scene_prepare);
 *   - an unloaded record and its GPU objects stay alive until the renderer's frame fence passes
 *     the last frame prepared before the unload, because that frame's GPU-scene entries may still
 *     name its bindless indices.
 *
 *   render_churn.churn.unload_waits_for_frame_fence  unpipelined, step by step: an unloaded mesh
 *       waits on the retire list while the frame fence is held, its GPU-scene entry stops being
 *       READY at the next kick, and it is freed by the first kick after the fence passes.
 *   render_churn.churn.pipelined_churn_1000          r.pipelined = 1, at least 1000 frames of
 *       loads, releases, evictions (de_asset_flush_unused), spawns, despawns and moves with a fixed
 *       seed; every 50 frames (render thread idle) every READY mesh entry of the GPU scene names
 *       the bindless indices of a live record. Run under TSan and ASan (CI: ci-linux-tsan,
 *       ASan+UBSan; local: macOS) for the data-race and use-after-free part.
 *
 * Progress is driven by state with deadlines, never by a frame count alone (DENG-425): a frame of
 * a Release build on the null backend is well under a millisecond, while a load waits for the I/O
 * thread (a first read on a Windows runner can take longer than hundreds of frames). So the churn
 * starts after every asset has been READY once, mostly releases only loads that have finished,
 * and runs past 1000 frames until enough records went through the retire list (or a deadline).
 *
 * Null backend, the real asset system (cooked DMSH and DMAT blobs from loader_test_util.h), the
 * real ECS loop: PostUpdate, Extract, de__render_extract_and_kick, with the asset update first,
 * as the host loop does.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "asset/load_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/slots_internal.h"
#include "loader_test_util.h"
#include "render/components_internal.h"
#include "render/loaders/render_assets.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

#include <stdio.h>
#include <string.h>

#define MESHES       8u
#define MATERIALS    4u
#define RCH_WAIT_NS  (10ull * 1000000000ull) /* a load's deadline */
#define RCH_CHURN_NS (60ull * 1000000000ull) /* the churn's deadline past its 1000 frames */
#define RCH_FREED    20u                     /* records the churn must free through the list */
#define ENTITIES     256u

typedef struct cfx {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *w;
    de_asset_system *s;
    de_gpu_device *d;
    de_renderer *r;
    DeRenderComponents ids;
    de_builtin_component_ids bi;
    char dir[96];
    de_entity ents[ENTITIES];
    u32 ent_count;
    u64 rng;
} cfx;

static u32 rnd(cfx *f) {
    f->rng ^= f->rng << 13;
    f->rng ^= f->rng >> 7;
    f->rng ^= f->rng << 17;
    return (u32)(f->rng >> 11);
}

static void set_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
}

static void cfx_setup(cfx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    f->rng = 0xD422ull;
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("rch", DE_RCH_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
}

static void cfx_teardown(cfx *f) {
    if (f->r != NULL)
        de__render_world_wait_idle(f->r);
    /* The asset system first: its unloads use the renderer's uploader (render_assets.h). */
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    if (f->r != NULL)
        de_renderer_destroy(f->r);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->d != NULL)
        de_gpu_device_destroy(f->d);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    set_cvar("r.pipelined", "1");
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).blocks_live, 0u);
}

static void put(cfx *f, const char *file, const void *data, u64 size) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", f->dir, file);
    ASSERT_EQ(de_file_write_all_atomic(path, data, size), DE_OK);
}

static de_asset_id mesh_id(u32 i) {
    char name[32];
    snprintf(name, sizeof name, "churn/m%u", i);
    return de_asset_id_from(name);
}

static de_asset_id mat_id(u32 i) {
    char name[32];
    snprintf(name, sizeof name, "churn/mat%u", i);
    return de_asset_id_from(name);
}

/* A world, an asset system with MESHES meshes and MATERIALS materials, a null renderer. */
static void start(cfx *f, const char *name, bool pipelined) {
    de_fp_env_init_thread();
    snprintf(f->dir, sizeof f->dir, "rch:/%s", name);
    if (!de_path_exists(f->dir))
        ASSERT_EQ(de_dir_create(f->dir), DE_OK);
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(
        de_world_create(
            &(de_world_desc){.allocator = &f->alloc, .types = f->types, .tasks = f->tasks}, &f->w),
        DE_OK);
    char cache[128];
    snprintf(cache, sizeof cache, "%s/decache", f->dir);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .cache_dir = cache,
                                                             .staging_ring_bytes = 8u << 20,
                                                             .types = f->types},
                                     &f->s),
              DE_OK);
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &f->dev_alloc}, &f->d),
              DE_OK);
    ASSERT_EQ(de_renderer_create(
                  &(de_renderer_desc){
                      .device = f->d, .assets = f->s, .types = f->types, .allocator = &f->alloc},
                  &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->w);
    f->ids = *de__render_components(f->w);
    f->bi = de_builtin_components(f->w);
    set_cvar("r.pipelined", pipelined ? "1" : "0");

    char text[4096] = "[assets]\n";
    size_t at = strlen(text);
    for (u32 i = 0; i < MESHES; ++i) {
        lt_blob b = lt_dmsh(64u + 16u * i, 0, i);
        char file[32];
        snprintf(file, sizeof file, "m%u.dmsh", i);
        put(f, file, b.data, b.size);
        lt_free(&b);
        at +=
            (size_t)snprintf(text + at, sizeof text - at,
                             "\"churn/m%u\" = { type = \"mesh\", source = \"m%u.dmsh\" }\n", i, i);
    }
    for (u32 i = 0; i < MATERIALS; ++i) {
        lt_blob b = lt_dmat(NULL, NULL, 0, 0.1f * (f32)(i + 1u));
        char file[32];
        snprintf(file, sizeof file, "mat%u.dmat", i);
        put(f, file, b.data, b.size);
        lt_free(&b);
        at += (size_t)snprintf(
            text + at, sizeof text - at,
            "\"churn/mat%u\" = { type = \"material\", source = \"mat%u.dmat\" }\n", i, i);
    }
    put(f, "assets.toml", text, strlen(text));
    char path[160];
    snprintf(path, sizeof path, "%s/assets.toml", f->dir);
    ASSERT_EQ(de_asset_manifest_load(f->s, path), DE_OK);
}

/* One host frame: the asset update, PostUpdate, Extract, the kick. */
static void frame(cfx *f) {
    de_asset_system_update(f->s, NULL);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->w), DE_OK);
}

static de_entity spawn(cfx *f, u32 mesh, u32 mat, f64 x) {
    const de_component comps[2] = {f->bi.transform, f->ids.mesh_renderer};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(f->w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    DeTransform *t = (DeTransform *)de_entity_get(f->w, e, f->bi.transform);
    t->position = (de_dvec3){x, 0, 0};
    t->rotation = (de_quat){0, 0, 0, 1};
    t->scale = (de_vec3){1, 1, 1};
    DeMeshRenderer *m = (DeMeshRenderer *)de_entity_get(f->w, e, f->ids.mesh_renderer);
    m->mesh = de_asset_mesh(f->s, mesh_id(mesh));
    m->materials[0] = de_asset_material(f->s, mat_id(mat));
    return e;
}

/* The GPU-scene entry of mesh `i` (false when the scene has no slot for it). Render thread idle. */
static bool mesh_entry(cfx *f, u32 i, de__gs_mesh *out) {
    const u32 slot = de__gpu_scene_mesh_slot(f->r->gpu_scene, de_asset_mesh(f->s, mesh_id(i)));
    if (slot == DE__GS_NONE)
        return false;
    de__gpu_scene_debug_pack(f->r->gpu_scene, DE__GS_MESHES, slot, out);
    return true;
}

/* Every READY mesh entry names the buffers of a live record (render thread idle, right after a
 * kick: no asset update since). Returns the number of READY entries. */
static u32 check_entries(cfx *f) {
    u32 ready = 0;
    for (u32 i = 0; i < MESHES; ++i) {
        de__gs_mesh e = {0};
        if (!mesh_entry(f, i, &e) || !(e.flags & DE__GS_MESH_READY))
            continue;
        ++ready;
        const de_render_mesh *m = de_render_mesh_get(f->s, de_asset_mesh(f->s, mesh_id(i)));
        EXPECT_TRUE(m != NULL);
        if (m != NULL) {
            EXPECT_EQ(e.vertices, m->bindless[DE_RENDER_MESH_VERTICES]);
            EXPECT_EQ(e.indices, m->bindless[DE_RENDER_MESH_INDICES]);
        }
    }
    return ready;
}

/* Frames until mesh `i`'s GPU-scene entry is READY, by deadline (the I/O thread reads the file:
 * slow on some CI hosts, and a frame can be much shorter than the read). */
static bool pump_until_ready(cfx *f, u32 i, de__gs_mesh *entry) {
    const u64 t0 = de_time_now_ns();
    for (;;) {
        frame(f);
        if (mesh_entry(f, i, entry) && (entry->flags & DE__GS_MESH_READY))
            return true;
        if (de_time_now_ns() - t0 > RCH_WAIT_NS)
            return false;
        (void)de__asset_wake_block(f->s, 2000000u);
    }
}

/* ---- churn.unload_waits_for_frame_fence ------------------------------------------------------ */

TEST_F(churn, unload_waits_for_frame_fence, cfx) {
    start(f, "wait", false);
    de_render_uploader *u = de__renderer_uploader(f->r);
    ASSERT_TRUE(u != NULL);
    de_load_handle h = de_asset_load(f->s, mesh_id(0), 0);
    const de_entity e = spawn(f, 0, 0, 1.0);
    ASSERT_NE(e.bits, 0u);
    de__gs_mesh entry = {0};
    bool ready = false;
    ready = pump_until_ready(f, 0, &entry);
    ASSERT_TRUE(ready);
    const de_render_mesh *rec = de_render_mesh_get(f->s, de_asset_mesh(f->s, mesh_id(0)));
    ASSERT_TRUE(rec != NULL);
    EXPECT_EQ(entry.vertices, rec->bindless[DE_RENDER_MESH_VERTICES]);

    /* The GPU is "late": the last frame prepared (it named the mesh) has not completed, and the
     * frame fence stays one value short of it. */
    u64 frames = 0;
    const de_gpu_fence fence = de__renderer_frame_fence(f->r, &frames);
    const u64 done = de_gpu_fence_value(f->d, fence);
    ASSERT_GE(done, 1u);
    de__render_uploader_test_hold_frames(u, done - 1u);
    u32 pending = 0;
    u64 freed0 = 0, freed = 0;
    de__render_uploader_retire_stats(u, &pending, &freed0);
    EXPECT_EQ(pending, 0u);

    /* Unload it: the instance still names the handle, but the record goes. */
    de_asset_release(f->s, h);
    de_asset_flush_unused(f->s);
    EXPECT_TRUE(de_render_mesh_get(f->s, de_asset_mesh(f->s, mesh_id(0))) == NULL);
    de__render_uploader_retire_stats(u, &pending, &freed);
    EXPECT_EQ(pending, 1u); /* frames up to `frames` may name it: it waits */
    EXPECT_EQ(freed, freed0);
    for (u32 i = 0; i < 3; ++i) {
        frame(f);
        de__render_uploader_retire_stats(u, &pending, &freed);
        EXPECT_EQ(pending, 1u);
        EXPECT_EQ(freed, freed0);
        /* The next kick repacked the entry: no frame from now on names the old buffers. */
        ASSERT_TRUE(mesh_entry(f, 0, &entry));
        EXPECT_EQ(entry.flags & DE__GS_MESH_READY, 0u);
        EXPECT_EQ(entry.vertices, 0u);
    }
    /* The fence passes: the next kick frees it. */
    de__render_uploader_test_hold_frames(u, UINT64_MAX);
    frame(f);
    de__render_uploader_retire_stats(u, &pending, &freed);
    EXPECT_EQ(pending, 0u);
    EXPECT_EQ(freed, freed0 + 1u);

    /* Loaded again: a new record, READY again in the scene. */
    h = de_asset_load(f->s, mesh_id(0), 0);
    ready = false;
    ready = pump_until_ready(f, 0, &entry);
    if (!ready) {
        de_load_info info;
        const de_load_state st = de_asset_status(f->s, h, &info);
        const bool have = mesh_entry(f, 0, &entry);
        const de_render_upload_stats us = de_render_uploader_stats(u);
        printf("reload: state %d, ready %u/%u, failed %u, error %d \"%s\"; stage %u, uploads in "
               "flight %u, uploader queued %u submitted %llu completed %llu; scene slot %s, "
               "flags 0x%x\n",
               (int)st, info.assets_ready, info.assets_total, info.assets_failed,
               (int)info.first_error, info.first_error_msg,
               de__pipeline_debug_stage(f->s, de__slot_find(f->s, mesh_id(0))),
               de_asset_get_stats(f->s).uploads_in_flight, us.queued,
               (unsigned long long)us.submitted, (unsigned long long)us.completed,
               have ? "yes" : "no", have ? entry.flags : 0u);
    }
    EXPECT_TRUE(ready);
    (void)frames;
    de_asset_release(f->s, h);
}

/* ---- churn.pipelined_churn_1000 -------------------------------------------------------------- */

/* A load request that is no longer waiting for the pipeline. */
static bool load_finished(cfx *f, de_load_handle h) {
    const de_load_state st = de_asset_status(f->s, h, NULL);
    return st != DE_LOAD_PENDING && st != DE_LOAD_LOADING;
}

TEST_F(churn, pipelined_churn_1000, cfx) {
    start(f, "pipelined", true);
    de_render_uploader *u = de__renderer_uploader(f->r);
    ASSERT_TRUE(u != NULL);

    /* Warm-up: every mesh and material READY once (the first reads of the files are the slow
     * ones), then released: the churn's first flush_unused unloads them. */
    de_asset_id all[MESHES + MATERIALS];
    for (u32 i = 0; i < MESHES; ++i)
        all[i] = mesh_id(i);
    for (u32 i = 0; i < MATERIALS; ++i)
        all[MESHES + i] = mat_id(i);
    const de_load_handle warm = de_asset_load_many(f->s, all, MESHES + MATERIALS, 0);
    const u64 w0 = de_time_now_ns();
    while (de_asset_status(f->s, warm, NULL) != DE_LOAD_READY) {
        if (de_time_now_ns() - w0 > RCH_WAIT_NS) {
            de_load_info info;
            const de_load_state st = de_asset_status(f->s, warm, &info);
            printf("warm-up: state %d, ready %u/%u, failed %u, error %d \"%s\"\n", (int)st,
                   info.assets_ready, info.assets_total, info.assets_failed, (int)info.first_error,
                   info.first_error_msg);
            de_asset_release(f->s, warm);
            ASSERT_TRUE(false); /* the assets never loaded */
        }
        frame(f);
        (void)de__asset_wake_block(f->s, 2000000u);
    }
    de_asset_release(f->s, warm);

    de_load_handle held[32];
    u32 held_count = 0;
    u32 max_ready = 0, unload_calls = 0, cancels = 0;
    u32 pending = 0;
    u64 freed0 = 0, freed = 0;
    de__render_uploader_retire_stats(u, &pending, &freed0);
    const u64 t0 = de_time_now_ns();
    u32 n = 0;
    for (;; ++n) {
        if (n >= 1000u && (n % 50u) == 0u) {
            /* At least 1000 frames; then until enough records went through the retire list and
             * the GPU scene used loaded meshes, or the deadline. */
            de__render_uploader_retire_stats(u, &pending, &freed);
            if ((freed - freed0 > RCH_FREED && max_ready > 0) ||
                de_time_now_ns() - t0 > RCH_CHURN_NS)
                break;
        }
        const u32 ops = 1u + rnd(f) % 6u;
        for (u32 k = 0; k < ops; ++k) {
            const u32 op = rnd(f) % 100u;
            if (op < 20u && held_count < 32u) {
                const de_asset_id ids[2] = {mesh_id(rnd(f) % MESHES), mat_id(rnd(f) % MATERIALS)};
                held[held_count++] = de_asset_load_many(f->s, ids, 2, 0);
            } else if (op < 38u && held_count != 0) {
                /* Release a finished load; one release in four also cancels a load in flight. */
                const u32 i = rnd(f) % held_count;
                const bool cancel = rnd(f) % 4u == 0u;
                if (cancel || load_finished(f, held[i])) {
                    cancels += load_finished(f, held[i]) ? 0u : 1u;
                    de_asset_release(f->s, held[i]);
                    held[i] = held[--held_count];
                }
            } else if (op < 60u && f->ent_count < ENTITIES) {
                const de_entity e =
                    spawn(f, rnd(f) % MESHES, rnd(f) % MATERIALS, (f64)(rnd(f) % 1000u));
                if (e.bits != 0)
                    f->ents[f->ent_count++] = e;
            } else if (op < 75u && f->ent_count != 0) {
                const u32 i = rnd(f) % f->ent_count;
                (void)de_entity_despawn(f->w, f->ents[i]);
                f->ents[i] = f->ents[--f->ent_count];
            } else if (op < 90u && f->ent_count != 0) {
                const de_entity e = f->ents[rnd(f) % f->ent_count];
                DeTransform *t = (DeTransform *)de_entity_get(f->w, e, f->bi.transform);
                if (t != NULL)
                    t->position.y += 0.5;
            } else if (f->ent_count != 0) {
                /* Point an instance at another mesh: the old one may lose its last user. */
                const de_entity e = f->ents[rnd(f) % f->ent_count];
                DeMeshRenderer *m = (DeMeshRenderer *)de_entity_get(f->w, e, f->ids.mesh_renderer);
                if (m != NULL)
                    m->mesh = de_asset_mesh(f->s, mesh_id(rnd(f) % MESHES));
            }
        }
        if (n % 7u == 6u) {
            de_asset_flush_unused(f->s); /* evict every unreferenced asset: unloads */
            ++unload_calls;
        }
        frame(f);
        if (n % 50u == 49u) {
            /* Render thread idle, no asset update since the kick: the entries are checkable. */
            de__render_world_wait_idle(f->r);
            const u32 ready = check_entries(f);
            if (ready > max_ready)
                max_ready = ready;
        }
    }
    de__render_world_wait_idle(f->r);
    de__render_uploader_retire_stats(u, &pending, &freed);
    printf("churn: %u frames in %.1f ms, %u flushes, %u loads cancelled, %llu records freed after "
           "their frames, %u still waiting, up to %u READY meshes in the GPU scene\n",
           n, (double)(de_time_now_ns() - t0) / 1e6, unload_calls, cancels,
           (unsigned long long)(freed - freed0), pending, max_ready);
    EXPECT_GE(n, 1000u);
    EXPECT_GT(freed - freed0, (u64)RCH_FREED); /* the churn unloaded records through the list */
    EXPECT_GT(max_ready, 0u);                  /* and the GPU scene used loaded meshes */
    for (u32 i = 0; i < held_count; ++i)
        de_asset_release(f->s, held[i]);
}
