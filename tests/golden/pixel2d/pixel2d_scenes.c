/* DENG-202: actual manifest/scene/sprite/light/pixel-output reference, unchanged S10 harness.
 * Direction: quiet/cool/layered dusk ruins; warm character/torch focus; 8px rhythms.
 * All variation parameters are fixed; no animation or wall-clock dependent lighting. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "golden.h"
#include "render/components_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/rt/rt_shadows.h"
#include <dengine/de_asset.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if DE_GOLDEN_COOK && DE_HAS_COOKER
typedef struct px_scene {
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
} px_scene;
static void px_cvar(const char *name, const char *value) {
    de_cvar c = de_cvar_find(name);
    if (c.id)
        (void)de_cvar_set(c, value);
    de_error_clear();
}
static bool px_frame(px_scene *p) {
    de_asset_system_update(p->assets, NULL);
    de_result result = de_world_run_schedule(p->world, DE_SCHEDULE_POST_UPDATE, 0, 0);
    if (result == DE_OK)
        result = de_world_run_schedule(p->world, DE_SCHEDULE_EXTRACT, 0, 0);
    if (result == DE_OK)
        result = de__render_extract_and_kick(p->r, p->world);
    if (result != DE_OK)
        de_golden_fail(p->ctx, "pixel2d frame: %s", de_error_last()->message);
    return result == DE_OK;
}
static void px_close(px_scene *p) {
    de_gpu_wait_idle(p->ctx->device);
    if (p->r)
        de__render_world_wait_idle(p->r);
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
    px_cvar("r.pipelined", "1");
}
static void px_reverse_sort(const de_query_chunk *chunk, void *user) {
    (void)user;
    DeSprite *s = chunk->columns[0];
    for (u32 i = 0; i < chunk->count; ++i) {
        s[i].layer = -s[i].layer;
        s[i].order = -s[i].order;
    }
}
static void px_run(de_golden_ctx *ctx, u32 variant) {
    const bool rt = variant == 1;
    if (rt && (ctx->caps->raytracing != DE_RT_HARDWARE || !ctx->caps->ray_query)) {
        de_golden_skip(ctx, "pixel2d RT requires hardware ray query (needs-hw-verify)");
        return;
    }
    static bool mounted;
    if (!mounted) {
        if (de_fs_mount("pixel2d", DE_PIXEL2D_ASSETS, 0, 0) != DE_OK ||
            de_fs_mount("pixelcache", DE_PIXEL2D_CACHE, DE_MOUNT_WRITABLE, 0) != DE_OK) {
            de_golden_fail(ctx, "pixel2d mount");
            return;
        }
        mounted = true;
    }
    de_fp_env_init_thread();
    px_scene p = {.ctx = ctx};
    if (de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &p.tasks) != DE_OK ||
        de_type_registry_create(NULL, &p.types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = p.types, .tasks = p.tasks}, &p.world) != DE_OK ||
        de_asset_system_create(&(de_asset_system_desc){.tasks = p.tasks,
                                                       .types = p.types,
                                                       .io_threads = 1,
                                                       .cache_dir = "pixelcache:/",
                                                       .staging_ring_bytes = 8u << 20},
                               &p.assets) != DE_OK ||
        de_renderer_create(
            &(de_renderer_desc){.device = ctx->device, .assets = p.assets, .types = p.types},
            &p.r) != DE_OK) {
        de_golden_fail(ctx, "pixel2d engine setup");
        px_close(&p);
        return;
    }
    de_renderer_attach(p.r, p.world);
    px_cvar("r.pipelined", "0");
    px_cvar("r.ambient_fallback", "0.3");
    de_render_settings settings =
        de_render_settings_preset(rt ? DE_TIER_RT : DE_TIER_BASE, DE_QUALITY_HIGH);
    settings.render_scale = 1;
    settings.dynamic_res_target_ms = 0;
    settings.ao = settings.reflections = settings.gi = settings.volumetrics = settings.sss =
        DE_QUALITY_OFF;
    settings.rt_shadows = rt;
    settings.rt_ao = settings.rt_reflections = settings.rt_gi = settings.rt_refraction = false;
    settings.upscaler = DE_UPSCALE_NONE;
    de_renderer_set_settings(p.r, &settings);
    p.target = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.width = ctx->width,
                                            .height = ctx->height,
                                            .format = DE_FMT_RGBA8_UNORM,
                                            .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                     DE_USAGE_COPY_SRC,
                                            .name = "pixel2d actual output"});
    p.readback = de_gpu_buffer_create(
        ctx->device, &(de_gpu_buffer_desc){.size = (u64)ctx->width * ctx->height * 4,
                                           .usage = DE_USAGE_COPY_DST,
                                           .memory = DE_MEM_READBACK});
    p.view = de_view_create(p.r, &(de_view_desc){.target = p.target, .name = "pixel2d X-Z"});
    if (!p.target.id || !p.readback.id || !p.view.id ||
        de_asset_manifest_load(p.assets, "pixel2d:/assets.toml") != DE_OK) {
        de_golden_fail(ctx, "pixel2d target/manifest");
        px_close(&p);
        return;
    }
    p.bundle = de_asset_load_bundle(p.assets, "pixel2d", 0);
    u64 deadline = de_time_now_ns() + 120ull * 1000000000;
    de_load_state state = DE_LOAD_PENDING;
    while (de_time_now_ns() < deadline) {
        state = de_asset_wait(p.assets, p.bundle, 10000000);
        if (state == DE_LOAD_READY || state == DE_LOAD_FAILED || state == DE_LOAD_PARTIAL)
            break;
        if (!px_frame(&p))
            break;
    }
    if (state != DE_LOAD_READY) {
        de_load_info info = {0};
        (void)de_asset_status(p.assets, p.bundle, &info);
        de_golden_fail(ctx, "pixel2d bundle: %s", info.first_error_msg);
        px_close(&p);
        return;
    }
    de_entity camera = {0};
    if (de_prefab_instantiate(p.assets, p.world,
                              de_asset_scene(p.assets, de_asset_id_from("scene/pixel2d")),
                              DE_ENTITY_NULL, &camera) != DE_OK) {
        de_golden_fail(ctx, "pixel2d instantiate: %s", de_error_last()->message);
        px_close(&p);
        return;
    }
    const DeRenderComponents *ids = de__render_components(p.world);
    DeCamera *cam = de_entity_get(p.world, camera, ids->camera);
    if (!cam) {
        de_golden_fail(ctx, "pixel2d first root camera");
        px_close(&p);
        return;
    }
    printf("  pixel2d camera projection=%u ortho=%g\n", (u32)cam->projection,
           (double)cam->ortho_height);
    cam->view = p.view;
    de_pixel_art_settings pixel = {.internal_width = 480,
                                   .internal_height = 270,
                                   .integer_scale_only = variant != 2,
                                   .snap_camera = true,
                                   .snap_sprites = true,
                                   .smooth_subpixel_scroll = variant == 3,
                                   .letterbox_color = {.03f, .04f, .06f, 1}};
    if (variant == 4) {
        pixel.palette_lut = de_asset_texture(p.assets, de_asset_id_from("tex/palette"));
        pixel.dither_strength = 1.f;
    }
    const char *negative = getenv("DE_PIXEL2D_NEGATIVE");
    if (negative && strcmp(negative, "snap") == 0)
        pixel.snap_sprites = false;
    if (negative && strcmp(negative, "sort") == 0) {
        /* Authoring equivalent of reversing the layer/order key; no broken renderer committed. */
        de_query_term term = {.component = ids->sprite, .access = DE_WRITE};
        de_query q = de_query_create(p.world, &(de_query_desc){.terms = &term, .term_count = 1});
        de_query_each(p.world, q, px_reverse_sort, NULL);
        de_query_destroy(p.world, q);
    }
    de_view_set_pixel_art(p.r, p.view, &pixel);
    if (variant == 3) {
        DeTransform *t = de_entity_get(p.world, camera, de_builtin_components(p.world).transform);
        t->position.x = .021875;
    }
    de_cvar seed = de_cvar_register(&(de_cvar_desc){.name = "r.rt.golden_sample",
                                                    .type = DE_CVAR_INT,
                                                    .default_number = -1,
                                                    .min = -1,
                                                    .max = 2147483647});
    (void)seed;
    for (u32 frame = 0; frame < (rt ? 32u : 10u); ++frame) {
        char value[16];
        snprintf(value, sizeof value, "%u", frame);
        px_cvar("r.rt.golden_sample", value);
        if (!px_frame(&p)) {
            px_close(&p);
            return;
        }
    }
    de_gpu_wait_idle(ctx->device);
    if (rt) {
        de__rt_shadows_info info = {0};
        if (!de__rt_shadows_get(p.r->rt_shadows, p.view, &info) || !info.rays)
            de_golden_fail(ctx, "pixel2d RT shadow producer absent");
        else
            printf("  pixel2d actual RT shadow rays=%llu\n", (unsigned long long)info.rays);
    }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "pixel2d capture");
    de_gpu_barrier barrier = {
        .texture = p.target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, p.readback, 0, ctx->width * 4, p.target, 0, 0);
    de_result result = de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS,
                                           &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    de_gpu_wait_idle(ctx->device);
    const u8 *pixels = de_gpu_buffer_map(ctx->device, p.readback);
    if (result != DE_OK || !pixels)
        de_golden_fail(ctx, "pixel2d readback");
    else {
        u32 lit = 0, chromatic = 0, edges = 0;
        for (u64 i = 0; i < (u64)ctx->width * ctx->height * 4; i += 4) {
            if (i >= 4 && memcmp(pixels + i, pixels + i - 4, 3) != 0)
                ++edges;
            lit += (pixels[i] | pixels[i + 1] | pixels[i + 2]) > 16;
            chromatic += pixels[i] != pixels[i + 1] || pixels[i + 1] != pixels[i + 2];
        }
        printf("  pixel2d bundle READY: lit=%u chromatic=%u pixels=%u\n", lit, chromatic,
               ctx->width * ctx->height);
        if (edges < 1000 || lit < ctx->width * ctx->height / 4 ||
            chromatic < ctx->width * ctx->height / 4)
            de_golden_fail(ctx, "pixel2d image lacks scene coverage/edges (%u)", edges);
        de_gpu_buffer upload =
            de_golden_upload_buffer(ctx, pixels, (u64)ctx->width * ctx->height * 4,
                                    DE_USAGE_COPY_SRC, "pixel2d rendered pixels");
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, upload, 0, ctx->width * 4);
    }
    px_close(&p);
}
#else
static void px_run(de_golden_ctx *ctx, u32 variant) {
    (void)variant;
    de_golden_skip(ctx, "pixel2d SOURCE assets require cooker");
}
#endif
DE_GOLDEN_SCENE(pixel2d_base_int4x) {
    px_run(ctx, 0);
}
DE_GOLDEN_SCENE(pixel2d_rt_int4x) {
    px_run(ctx, 1);
}
DE_GOLDEN_SCENE(pixel2d_sharp_bilinear) {
    px_run(ctx, 2);
}
DE_GOLDEN_SCENE(pixel2d_subtexel_scroll) {
    px_run(ctx, 3);
}
DE_GOLDEN_SCENE(pixel2d_palette_dither) {
    px_run(ctx, 4);
}
DE_GOLDEN_SCENE(pixel2d_translucent_sort) {
    px_run(ctx, 5);
}
