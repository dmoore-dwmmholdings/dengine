/* DENG-80: tilemap_lit_two_layers. The manifest of examples/scenes/tilemap/ is loaded in SOURCE
 * mode (the Tiled map, its tileset texture and the scene are cooked in-process), the scene is
 * instantiated, and the tilemap runtime (`tilemap.update`, PostUpdate) builds the map's chunk
 * sprites, which the renderer draws through the Sprite material model with the scene's two
 * lights. The animation clock stays at 0 (every frame runs with dt 0), so the picture is fixed.
 * The flow is the pixel2d scenes' (tests/golden/pixel2d/). */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "golden.h"
#include "render/components_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "gameplay/tilemap/tilemap_internal.h"
#include <dengine/de_asset.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>
#include <stdio.h>
#include <string.h>

#if DE_GOLDEN_COOK && DE_HAS_COOKER
typedef struct tg_scene {
    de_golden_ctx *ctx;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_renderer *r;
    de_view view;
    de_gpu_texture target;
    de_gpu_buffer readback;
    de_load_handle bundle;
} tg_scene;

static void tg_cvar(const char *name, const char *value) {
    de_cvar c = de_cvar_find(name);
    if (c.id)
        (void)de_cvar_set(c, value);
    de_error_clear();
}

static bool tg_frame(tg_scene *p) {
    de_asset_system_update(p->assets, NULL);
    de_result result = de_world_run_schedule(p->world, DE_SCHEDULE_POST_UPDATE, 0, 0);
    if (result == DE_OK)
        result = de_world_run_schedule(p->world, DE_SCHEDULE_EXTRACT, 0, 0);
    if (result == DE_OK)
        result = de__render_extract_and_kick(p->r, p->world);
    if (result != DE_OK)
        de_golden_fail(p->ctx, "tilemap frame: %s", de_error_last()->message);
    return result == DE_OK;
}

static void tg_close(tg_scene *p) {
    de_gpu_wait_idle(p->ctx->device);
    if (p->r)
        de__render_world_wait_idle(p->r);
    if (p->world)
        de_tilemap_detach(p->world);
    if (p->assets) {
        if (p->bundle.id)
            de_asset_release(p->assets, p->bundle);
        de_asset_system_destroy(p->assets);
    }
    if (p->r)
        de_renderer_destroy(p->r);
    if (p->world)
        de_world_destroy(p->world);
    if (p->types)
        de_type_registry_destroy(p->types);
    if (p->tasks)
        de_task_system_destroy(p->tasks);
    de_gpu_texture_destroy(p->ctx->device, p->target);
    de_gpu_buffer_destroy(p->ctx->device, p->readback);
    tg_cvar("r.pipelined", "1");
}

static void tg_run(de_golden_ctx *ctx) {
    static bool mounted;
    if (!mounted) {
        if (de_fs_mount("tmgold", DE_TILEMAP_GOLDEN_ASSETS, 0, 0) != DE_OK ||
            de_fs_mount("tmgoldcache", DE_TILEMAP_GOLDEN_CACHE, DE_MOUNT_WRITABLE, 0) != DE_OK) {
            de_golden_fail(ctx, "tilemap mount");
            return;
        }
        mounted = true;
    }
    de_fp_env_init_thread();
    tg_scene p = {.ctx = ctx};
    if (de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &p.tasks) != DE_OK ||
        de_type_registry_create(NULL, &p.types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = p.types, .tasks = p.tasks}, &p.world) != DE_OK ||
        de_asset_system_create(&(de_asset_system_desc){.tasks = p.tasks,
                                                       .types = p.types,
                                                       .io_threads = 1,
                                                       .cache_dir = "tmgoldcache:/",
                                                       .staging_ring_bytes = 8u << 20},
                               &p.assets) != DE_OK ||
        de_renderer_create(
            &(de_renderer_desc){.device = ctx->device, .assets = p.assets, .types = p.types},
            &p.r) != DE_OK) {
        de_golden_fail(ctx, "tilemap engine setup");
        tg_close(&p);
        return;
    }
    de_renderer_attach(p.r, p.world);
    /* Before the manifest: the scene importer reads DeTilemap from the type registry. */
    if (de_tilemap_attach(p.world, p.assets, NULL) != DE_OK) {
        de_golden_fail(ctx, "tilemap attach: %s", de_error_last()->message);
        tg_close(&p);
        return;
    }
    tg_cvar("r.pipelined", "0");
    tg_cvar("r.ambient_fallback", "0.25");
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    settings.render_scale = 1;
    settings.dynamic_res_target_ms = 0;
    settings.ao = settings.reflections = settings.gi = settings.volumetrics = settings.sss =
        DE_QUALITY_OFF;
    settings.rt_shadows = false;
    settings.rt_ao = settings.rt_reflections = settings.rt_gi = settings.rt_refraction = false;
    settings.upscaler = DE_UPSCALE_NONE;
    de_renderer_set_settings(p.r, &settings);
    p.target = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.width = ctx->width,
                                            .height = ctx->height,
                                            .format = DE_FMT_RGBA8_UNORM,
                                            .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                     DE_USAGE_COPY_SRC,
                                            .name = "tilemap output"});
    p.readback = de_gpu_buffer_create(
        ctx->device, &(de_gpu_buffer_desc){.size = (u64)ctx->width * ctx->height * 4,
                                           .usage = DE_USAGE_COPY_DST,
                                           .memory = DE_MEM_READBACK});
    p.view = de_view_create(p.r, &(de_view_desc){.target = p.target, .name = "tilemap X-Z"});
    if (!p.target.id || !p.readback.id || !p.view.id ||
        de_asset_manifest_load(p.assets, "tmgold:/assets.toml") != DE_OK) {
        de_golden_fail(ctx, "tilemap target/manifest");
        tg_close(&p);
        return;
    }
    p.bundle = de_asset_load_bundle(p.assets, "tilemap", 0);
    u64 deadline = de_time_now_ns() + 120ull * 1000000000;
    de_load_state state = DE_LOAD_PENDING;
    while (de_time_now_ns() < deadline) {
        state = de_asset_wait(p.assets, p.bundle, 10000000);
        if (state == DE_LOAD_READY || state == DE_LOAD_FAILED || state == DE_LOAD_PARTIAL)
            break;
        if (!tg_frame(&p))
            break;
    }
    if (state != DE_LOAD_READY) {
        de_load_info info = {0};
        (void)de_asset_status(p.assets, p.bundle, &info);
        de_golden_fail(ctx, "tilemap bundle: %s", info.first_error_msg);
        tg_close(&p);
        return;
    }
    de_entity camera = {0};
    if (de_prefab_instantiate(p.assets, p.world,
                              de_asset_scene(p.assets, de_asset_id_from("scene/tilemap")),
                              DE_ENTITY_NULL, &camera) != DE_OK) {
        de_golden_fail(ctx, "tilemap instantiate: %s", de_error_last()->message);
        tg_close(&p);
        return;
    }
    const DeRenderComponents *ids = de__render_components(p.world);
    DeCamera *cam = de_entity_get(p.world, camera, ids->camera);
    if (!cam) {
        de_golden_fail(ctx, "tilemap: the first root is not the camera");
        tg_close(&p);
        return;
    }
    cam->view = p.view;
    for (u32 frame = 0; frame < 10u; ++frame)
        if (!tg_frame(&p)) {
            tg_close(&p);
            return;
        }
    /* The runtime built both layers: count the tile sprites. */
    de__tilemap_sys *ts = de__tilemap_sys_of(p.world);
    u32 sprites = 0, layers = 0;
    for (u32 i = 0; ts != NULL && i < ts->inst_count; ++i) {
        const de__tm_instance *in = &ts->inst[i];
        layers += in->asset->view.h.layer_count;
        for (u32 c = 0; c < in->asset->view.h.chunk_count; ++c)
            for (u32 k = 0; in->chunks[c].sprites != NULL && k < DE__TM_CHUNK_CELLS; ++k)
                sprites += in->chunks[c].sprites[k].bits != 0;
    }
    printf("  tilemap layers=%u tile sprites=%u\n", layers, sprites);
    if (layers != 2u || sprites < 40u)
        de_golden_fail(ctx, "tilemap: expected 2 layers of tile sprites (%u, %u)", layers, sprites);
    de_gpu_wait_idle(ctx->device);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "tilemap capture");
    de_gpu_barrier barrier = {
        .texture = p.target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, p.readback, 0, ctx->width * 4, p.target, 0, 0);
    de_result result = de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS,
                                           &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    de_gpu_wait_idle(ctx->device);
    const u8 *pixels = de_gpu_buffer_map(ctx->device, p.readback);
    if (result != DE_OK || !pixels) {
        de_golden_fail(ctx, "tilemap readback");
    } else {
        u32 lit = 0, chromatic = 0;
        for (u64 i = 0; i < (u64)ctx->width * ctx->height * 4; i += 4) {
            lit += (pixels[i] | pixels[i + 1] | pixels[i + 2]) > 16;
            chromatic += pixels[i] != pixels[i + 1] || pixels[i + 1] != pixels[i + 2];
        }
        printf("  tilemap lit=%u chromatic=%u pixels=%u\n", lit, chromatic,
               ctx->width * ctx->height);
        if (lit < ctx->width * ctx->height / 8 || chromatic < ctx->width * ctx->height / 8)
            de_golden_fail(ctx, "tilemap image lacks map coverage (lit %u)", lit);
        de_gpu_buffer upload =
            de_golden_upload_buffer(ctx, pixels, (u64)ctx->width * ctx->height * 4,
                                    DE_USAGE_COPY_SRC, "tilemap rendered pixels");
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, upload, 0, ctx->width * 4);
    }
    tg_close(&p);
}
#else
static void tg_run(de_golden_ctx *ctx) {
    de_golden_skip(ctx, "tilemap SOURCE assets require the cooker");
}
#endif

DE_GOLDEN_SCENE(tilemap_lit_two_layers) {
    tg_run(ctx);
}
