/*
 * DENG-830: world partition goldens (assets in world/assets, cooked by `de-cook world`).
 *
 *   world_proxy_far        no streaming source: cell (0, 0) is not loaded, its far proxy draws
 *   world_proxy_crossfade  the cell is loaded and half way through its fade (alpha 0.5): the
 *                          proxy and the cell's meshes each draw a complementary dither half
 *   world_near_origin      the cell is loaded and its fade is done: the cell's meshes only
 *   world_100km            world_near_origin's content and camera 99840 m away on x and y
 *                          (cell (390, 390)); compared with world_near_origin's references, so it
 *                          proves the camera-relative render origin (SPEC 5.5) has no jitter
 *
 * The fade is driven by de_world_partition_update's `dt`: 0 while the cell loads (alpha stays 1),
 * then one step that sets the wanted alpha, then 0 again, so the image does not depend on how
 * many frames the loads take. The cell and proxy bundles are loaded (and cooked) before the
 * partition opens, so the partition finds them READY.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "golden.h"
#include "cooked_assets.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "render/components_internal.h"
#include "render/renderer_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WG_LOAD_WAIT_NS  (300ull * 1000000000ull) /* the first run cooks every asset */
#define WG_FRAME_WAIT_NS (120ull * 1000000000ull)

typedef enum wg_mode {
    WG_PROXY,     /* no source: the proxy only */
    WG_CROSSFADE, /* loaded, alpha 0.5 */
    WG_LOADED     /* loaded, the fade done */
} wg_mode;

typedef struct wg_opts {
    const char *world; /**< "near" or "far" */
    i32 cell;          /**< the world's one cell is (cell, cell) */
    wg_mode mode;
} wg_opts;

#if DE_GOLDEN_COOK

#define WG_MODEL_COUNT 2u
static const char *const k_models[WG_MODEL_COUNT] = {"mesh/box", "mesh/sphere"};

typedef struct wg {
    de_golden_ctx *ctx;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_renderer *r;
    de_world_partition *part;
    de_gpu_texture target;
    de_gpu_buffer rb;
    de_gpu_fence fence;
    u64 fence_value;
    de_view view;
    de_load_handle scene_load, cell_load, proxy_load;
    de_load_handle model_load[WG_MODEL_COUNT];
    char asset_pack[256];
    u32 pitch;
    u8 *now;
} wg;

static void wg_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
    de_error_clear();
}

static const char *wg_err(void) {
    const de_error *e = de_error_last();
    return e != NULL ? e->message : "?";
}

static const char *wg_backend_dir(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_METAL:
        return "metal";
    case DE_GPU_VULKAN:
        return "vulkan";
    case DE_GPU_D3D12:
        return "d3d12";
    default:
        return "other";
    }
}

/* The assets read-only as "worldgold", the cook cache writable as "worldcache" (CMake makes the
 * directory). */
static bool wg_mount(de_golden_ctx *ctx) {
    static bool mounted;
    if (mounted)
        return true;
    if (de_fs_mount("worldgold", DE_GOLDEN_WORLD_ASSETS, 0, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting %s: %s", DE_GOLDEN_WORLD_ASSETS, wg_err());
        return false;
    }
    if (de_fs_mount("worldcache", DE_GOLDEN_WORLD_CACHE, DE_MOUNT_WRITABLE, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting %s: %s", DE_GOLDEN_WORLD_CACHE, wg_err());
        return false;
    }
    mounted = true;
    return true;
}

/* One frame of the game loop: the asset update, the partition (when open) with `dt`, PostUpdate,
 * Extract and the kick (r.pipelined 0). */
static bool wg_frame(wg *m, f32 dt) {
    de_asset_system_update(m->assets, NULL);
    de_result r = m->part != NULL ? de_world_partition_update(m->part, dt) : DE_OK;
    if (r == DE_OK)
        r = de_world_run_schedule(m->world, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de_world_run_schedule(m->world, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de__render_extract_and_kick(m->r, m->world);
    if (r != DE_OK) {
        de_golden_fail(m->ctx, "frame: %s", wg_err());
        return false;
    }
    return true;
}

/* Reads the view's target into m->now on a submission of its own, and waits. */
static bool wg_read(wg *m) {
    de_golden_ctx *ctx = m->ctx;
    de_gpu_cmd *c = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "world readback");
    if (c == NULL) {
        de_golden_fail(ctx, "readback command list: %s", wg_err());
        return false;
    }
    de_gpu_barrier b = {
        .texture = m->target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, m->rb, 0, m->pitch, m->target, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    const u64 v = ++m->fence_value;
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = m->fence, .signal_value = v};
    if (de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS, &s) != DE_OK ||
        de_gpu_fence_wait(ctx->device, m->fence, v, WG_FRAME_WAIT_NS) != DE_OK) {
        de_golden_fail(ctx, "readback: %s", wg_err());
        return false;
    }
    const u8 *src = (const u8 *)de_gpu_buffer_map(ctx->device, m->rb);
    if (src == NULL) {
        de_golden_fail(ctx, "readback map: %s", wg_err());
        return false;
    }
    memcpy(m->now, src, (size_t)m->pitch * ctx->height);
    return true;
}

/* Waits for load `h` (asset or bundle `what`), running frames; fails the scene on an error or
 * when WG_LOAD_WAIT_NS have passed since `t0`. */
static bool wg_wait(wg *m, de_load_handle h, const char *what, u64 t0) {
    de_load_state st = DE_LOAD_PENDING;
    while (de_time_now_ns() - t0 < WG_LOAD_WAIT_NS) {
        st = de_asset_wait(m->assets, h, 200ull * 1000000ull);
        if (st == DE_LOAD_READY || st == DE_LOAD_FAILED || st == DE_LOAD_PARTIAL ||
            st == DE_LOAD_CANCELLED)
            break;
        if (!wg_frame(m, 0.0f))
            return false;
    }
    if (st == DE_LOAD_READY)
        return true;
    de_load_info info;
    memset(&info, 0, sizeof info);
    (void)de_asset_status(m->assets, h, &info);
    de_golden_fail(m->ctx, "loading %s: %s", what,
                   info.first_error != DE_OK ? info.first_error_msg : "timed out");
    return false;
}

static void wg_close(wg *m) {
    if (m->r != NULL)
        de__render_world_wait_idle(m->r);
    de_world_partition_close(m->part);
    if (m->assets != NULL) {
        (void)de_asset_set_streaming_sources(m->assets, NULL, 0);
        if (DE_HANDLE_VALID(m->scene_load))
            de_asset_release(m->assets, m->scene_load);
        if (DE_HANDLE_VALID(m->cell_load))
            de_asset_release(m->assets, m->cell_load);
        if (DE_HANDLE_VALID(m->proxy_load))
            de_asset_release(m->assets, m->proxy_load);
        for (u32 i = 0; i < WG_MODEL_COUNT; ++i)
            if (DE_HANDLE_VALID(m->model_load[i]))
                de_asset_release(m->assets, m->model_load[i]);
        de_asset_system_destroy(m->assets);
    }
    if (de_golden_pack_cleanup(m->asset_pack) != DE_OK)
        de_golden_fail(m->ctx, "cleaning temporary asset pack: %s", wg_err());
    if (m->r != NULL)
        de_renderer_destroy(m->r);
    if (m->world != NULL)
        de_world_destroy(m->world);
    if (m->types != NULL)
        de_type_registry_destroy(m->types);
    if (m->tasks != NULL)
        de_task_system_destroy(m->tasks);
    de_gpu_device *d = m->ctx->device;
    if (DE_HANDLE_VALID(m->fence))
        de_gpu_fence_destroy(d, m->fence);
    if (DE_HANDLE_VALID(m->rb))
        de_gpu_buffer_destroy(d, m->rb);
    if (DE_HANDLE_VALID(m->target))
        de_gpu_texture_destroy(d, m->target);
    free(m->now);
    wg_cvar("r.pipelined", "1");
}

/* Sets up the engine and the view; false (scene failed or skipped, `m` closed) on a problem. */
static bool wg_setup(de_golden_ctx *ctx, wg *m) {
    m->ctx = ctx;
    (void)de_cvar_register(
        &(de_cvar_desc){.name = "r.rg_gpu_timers", .type = DE_CVAR_BOOL, .default_number = 0});
    m->pitch = ctx->width * 4u;
    const size_t bytes = (size_t)m->pitch * ctx->height;
    m->now = (u8 *)calloc(bytes, 1);
    de_fp_env_init_thread();
    char cache[96];
    snprintf(cache, sizeof cache, "worldcache:/%s", wg_backend_dir(ctx->backend));
    if (!de_path_exists(cache))
        (void)de_dir_create(cache);
    if (m->now == NULL ||
        de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &m->tasks) != DE_OK ||
        de_type_registry_create(NULL, &m->types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = m->types, .tasks = m->tasks}, &m->world) !=
            DE_OK ||
        de_asset_system_create(&(de_asset_system_desc){.mode = DE_GOLDEN_ASSET_MODE,
                                                       .tasks = m->tasks,
                                                       .io_threads = 1,
                                                       .cache_dir = cache,
                                                       .staging_ring_bytes = 32u << 20,
                                                       .types = m->types},
                               &m->assets) != DE_OK ||
        de_renderer_create(
            &(de_renderer_desc){.device = ctx->device, .assets = m->assets, .types = m->types},
            &m->r) != DE_OK) {
        de_golden_fail(ctx, "setting up the engine: %s", wg_err());
        wg_close(m);
        return false;
    }
    const de__vis_shared *vis = de__views_vis(m->r);
    if (vis == NULL || !vis->enabled) {
        de_golden_skip(ctx, "the vis frame is off in this build: %s",
                       vis != NULL ? vis->why : "no vis frame");
        wg_close(m);
        return false;
    }
    de_renderer_attach(m->r, m->world);
    wg_cvar("r.pipelined", "0");
    wg_cvar("r.rg_gpu_timers", "0");
    if (ctx->backend == DE_GPU_VULKAN) /* one command list per submission (DENG-235) */
        wg_cvar("r.rg_record_cost", "1048576");
    de_render_settings rs = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_MEDIUM);
    rs.upscaler = DE_UPSCALE_NONE; /* no temporal AA: the dither pattern stays fixed */
    rs.shadows = rs.ao = rs.reflections = rs.gi = DE_QUALITY_OFF;
    de_renderer_set_settings(m->r, &rs);

    m->target = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_SRGB,
                                            .width = ctx->width,
                                            .height = ctx->height,
                                            .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                     DE_USAGE_COPY_SRC,
                                            .name = "world golden"});
    m->rb = de_gpu_buffer_create(ctx->device, &(de_gpu_buffer_desc){.size = (u64)bytes,
                                                                    .usage = DE_USAGE_COPY_DST,
                                                                    .memory = DE_MEM_READBACK,
                                                                    .name = "world readback"});
    m->fence = de_gpu_fence_create(ctx->device, 0);
    if (DE_HANDLE_VALID(m->target))
        m->view = de_view_create(m->r, &(de_view_desc){.target = m->target, .name = "world"});
    if (!DE_HANDLE_VALID(m->target) || !DE_HANDLE_VALID(m->rb) || !DE_HANDLE_VALID(m->fence) ||
        !DE_HANDLE_VALID(m->view)) {
        de_golden_fail(ctx, "the world view: %s", wg_err());
        wg_close(m);
        return false;
    }
    return true;
}

static void wg_run(de_golden_ctx *ctx, const wg_opts *o) {
    if (!ctx->caps->base_tier_ok) {
        de_golden_skip(ctx, "below the Base tier (the renderer needs it): %s",
                       ctx->caps->base_tier_reason);
        return;
    }
    if (ctx->format != DE_FMT_RGBA8_UNORM || (ctx->width * 4u) % 256u != 0u) {
        de_golden_fail(ctx, "scenes.toml: the world scenes need format rgba8 and a width that "
                            "is a multiple of 64");
        return;
    }
    if (!wg_mount(ctx))
        return;
    wg m;
    memset(&m, 0, sizeof m);
    if (!wg_setup(ctx, &m))
        return;
    if (de_asset_manifest_load(m.assets, "worldgold:/assets.toml") != DE_OK) {
        de_golden_fail(ctx, "assets.toml: %s", wg_err());
        wg_close(&m);
        return;
    }
    if (de_golden_pack_assets(m.assets, "worldgold:/", m.asset_pack) != DE_OK) {
        de_golden_fail(ctx, "preparing packed world assets: %s", wg_err());
        wg_close(&m);
        return;
    }
    /* The models, then the view scene, the cell and the proxy. */
    const u64 t0 = de_time_now_ns();
    for (u32 i = 0; i < WG_MODEL_COUNT; ++i) {
        m.model_load[i] = de_asset_load(m.assets, de_asset_id_from(k_models[i]), 0);
        if (!wg_wait(&m, m.model_load[i], k_models[i], t0)) {
            wg_close(&m);
            return;
        }
    }
    char view_name[64], cell_name[64], proxy_name[64];
    snprintf(view_name, sizeof view_name, "scenes/%s_view", o->world);
    snprintf(cell_name, sizeof cell_name, "world/%s/cell_%d_%d", o->world, (int)o->cell,
             (int)o->cell);
    snprintf(proxy_name, sizeof proxy_name, "world/%s/proxy_%d_%d", o->world, (int)o->cell,
             (int)o->cell);
    const de_asset_id view_id = de_asset_id_from(view_name);
    m.scene_load = de_asset_load(m.assets, view_id, 0);
    m.cell_load = de_asset_load_bundle(m.assets, cell_name, DE_LOAD_NO_EVENTS);
    m.proxy_load = de_asset_load_bundle(m.assets, proxy_name, DE_LOAD_NO_EVENTS);
    if (!wg_wait(&m, m.scene_load, view_name, t0) || !wg_wait(&m, m.cell_load, cell_name, t0) ||
        !wg_wait(&m, m.proxy_load, proxy_name, t0)) {
        wg_close(&m);
        return;
    }
    printf("  %s ready after %.1f s\n", o->world, (f64)(de_time_now_ns() - t0) / 1e9);
    de_entity cam = DE_ENTITY_NULL;
    if (de_prefab_instantiate(m.assets, m.world, de_asset_scene(m.assets, view_id),
                              DE_ENTITY_NULL, &cam) != DE_OK) {
        de_golden_fail(ctx, "instantiating %s: %s", view_name, wg_err());
        wg_close(&m);
        return;
    }
    const DeRenderComponents *ids = de__render_components(m.world);
    DeCamera *camera = ids != NULL && de_entity_alive(m.world, cam)
                           ? (DeCamera *)de_entity_get(m.world, cam, ids->camera)
                           : NULL;
    if (camera == NULL) {
        de_golden_fail(ctx, "%s: entity 0x1 (the first root) has no DeCamera", view_name);
        wg_close(&m);
        return;
    }
    camera->view = m.view;
    camera->tonemapper = de_tonemapper_builtin(m.r, DE_TM_NONE);

    if (de_world_partition_open(m.assets, m.world, o->world, NULL, &m.part) != DE_OK) {
        de_golden_fail(ctx, "opening world %s: %s", o->world, wg_err());
        wg_close(&m);
        return;
    }
    /* The source sits at the camera (view scene: (128, 10, 50) in the cell's frame). */
    const f64 origin = 256.0 * (f64)o->cell;
    const de_streaming_source src = {origin + 128.0, origin + 10.0, 50.0, 0.0f};
    if (o->mode != WG_PROXY)
        (void)de_asset_set_streaming_sources(m.assets, &src, 1);

    if (m.r->frame_index > 48u) {
        de_golden_fail(ctx, "asset preparation exceeded the fixed pre-roll (%llu)",
                       (unsigned long long)m.r->frame_index);
        wg_close(&m);
        return;
    }
    /* Fixed frame count; the fade step runs once, at the first update after the cell loaded. */
    bool stepped = o->mode == WG_PROXY;
    while (m.r->frame_index < 96u) {
        f32 dt = 0.0f;
        if (!stepped &&
            de_world_partition_cell_state(m.part, o->cell, o->cell) == DE_WORLD_CELL_LOADED) {
            dt = o->mode == WG_CROSSFADE ? 0.5f : 2.0f; /* fade_seconds is 1 */
            stepped = true;
        }
        if (!wg_frame(&m, dt)) {
            wg_close(&m);
            return;
        }
    }
    /* The state the image must show. */
    bool visible = false;
    f32 alpha = -1.0f;
    const de_world_cell_state st = de_world_partition_cell_state(m.part, o->cell, o->cell);
    if (!de_world_partition_proxy(m.part, o->cell, o->cell, &visible, &alpha)) {
        de_golden_fail(ctx, "world %s has no proxy for cell (%d, %d)", o->world, (int)o->cell,
                       (int)o->cell);
        wg_close(&m);
        return;
    }
    const bool want_loaded = o->mode != WG_PROXY;
    const bool want_visible = o->mode != WG_LOADED;
    const f32 want_alpha = o->mode == WG_CROSSFADE ? 0.5f : (o->mode == WG_PROXY ? 1.0f : 0.0f);
    if ((st == DE_WORLD_CELL_LOADED) != want_loaded || visible != want_visible ||
        (want_visible && fabsf(alpha - want_alpha) > 1e-5f) || !stepped) {
        de_golden_fail(ctx, "cell state %d, proxy visible %d alpha %.3f (want %s)", (int)st,
                       (int)visible, (f64)alpha,
                       o->mode == WG_PROXY ? "proxy only"
                                           : (o->mode == WG_CROSSFADE ? "alpha 0.5" : "loaded"));
        wg_close(&m);
        return;
    }
    if (!wg_read(&m)) {
        wg_close(&m);
        return;
    }
    const size_t bytes = (size_t)m.pitch * ctx->height;
    const de_gpu_buffer up =
        de_golden_upload_buffer(ctx, m.now, (u64)bytes, DE_USAGE_COPY_SRC, "world pixels");
    if (DE_HANDLE_VALID(up)) {
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, m.pitch);
    }
    wg_close(&m);
}

#else /* !DE_GOLDEN_COOK */

static void wg_run(de_golden_ctx *ctx, const wg_opts *o) {
    (void)o;
    de_golden_skip(ctx, "world test-side asset preparation is unavailable (DE_GOLDEN_COOK)");
}

#endif

DE_GOLDEN_SCENE(world_proxy_far) {
    wg_run(ctx, &(wg_opts){.world = "near", .cell = 0, .mode = WG_PROXY});
}
DE_GOLDEN_SCENE(world_proxy_crossfade) {
    wg_run(ctx, &(wg_opts){.world = "near", .cell = 0, .mode = WG_CROSSFADE});
}
DE_GOLDEN_SCENE(world_near_origin) {
    wg_run(ctx, &(wg_opts){.world = "near", .cell = 0, .mode = WG_LOADED});
}
DE_GOLDEN_SCENE(world_100km) {
    wg_run(ctx, &(wg_opts){.world = "far", .cell = 390, .mode = WG_LOADED});
}
