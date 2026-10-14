/* views_scenes.c — golden scenes of the renderer's views (DENG-176, SPEC §12.4, §12.9, §20.1).
 *
 *   views_split_screen  The renderer (offscreen target, 1280 x 720) with four views and no main
 *                       camera: "left" (left half, render_scale 1) and "right" (right half,
 *                       render_scale 0.5, composited back to its half), a picture-in-picture view
 *                       "pip" (top right, order 2, over "right"), and a render-to-texture view
 *                       "monitor" (256 x 256 target, order -1) that the "left" view samples in
 *                       the same frame (a DE_HOOK_UI pass draws it into its top-left quarter).
 *                       Until S13 each view clears to the debug colour of its name, so the image
 *                       is flat blocks: the viewports, the composite, the order and the
 *                       render-to-texture hand-over are what it checks.
 *
 * The scene runs the real frame loop: ECS cameras, the Extract schedule and
 * de__render_extract_and_kick (r.pipelined 0), three frames. It reads the renderer's offscreen
 * target back on its own submission, then uploads the pixels into the harness target, so the
 * renderer can be destroyed inside the scene. Test code: it includes renderer internals.
 */
#include "golden.h"

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>

#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <stdio.h>
#include <string.h>

typedef struct inset {
    de_renderer *r;
    de_view consumer;
    de_gpu_texture tex;
} inset;

/* DE_HOOK_UI: in the consumer view, the render-to-texture target drawn into the top-left quarter
 * of its colour (sampled through the view composite). */
static void inset_hook(de_rg *g, de_view v, void *user) {
    const inset *in = (const inset *)user;
    if (v.id != in->consumer.id)
        return;
    const de_rg_resource color = de_view_color(in->r, v);
    u32 w = 0, h = 0;
    de_view_size(in->r, v, &w, &h);
    if (!DE_HANDLE_VALID(color) || w == 0)
        return;
    const de_rg_resource src = de_rg_import_texture(g, in->tex, DE_ACCESS_SHADER_READ);
    de_rg_set_name(g, src, "monitor");
    const de__composite_item it = {.src = src, .rect = {0.0f, 0.0f, (f32)(w / 2u), (f32)(h / 2u)}};
    (void)de__composite_add(de__views_composite(in->r), g, g->arena, "golden.inset", color,
                            DE__VIEW_OUTPUT_FORMAT, NULL, &it, 1);
}

static void set_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
}

static de_entity spawn_camera(de_world *w, const DeRenderComponents *ids,
                              const de_builtin_component_ids *bi, de_view v, f64 x) {
    const de_component comps[2] = {bi->transform, ids->camera};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){x, 0.0, 2.0};
    *(DeTransform *)de_entity_get(w, e, bi->transform) = t;
    ((DeCamera *)de_entity_get(w, e, ids->camera))->view = v;
    return e;
}

/* Reads the offscreen target (left in DE__RENDER_OFFSCREEN_FINAL_ACCESS) into `px` (rows of
 * `pitch` bytes) on a submission of its own, and waits. */
static bool read_offscreen(de_golden_ctx *ctx, de_gpu_texture src, u32 pitch, u32 h, u8 *px) {
    de_gpu_buffer rb =
        de_gpu_buffer_create(ctx->device, &(de_gpu_buffer_desc){.size = (u64)pitch * h,
                                                                .usage = DE_USAGE_COPY_DST,
                                                                .memory = DE_MEM_READBACK,
                                                                .name = "views readback"});
    if (!DE_HANDLE_VALID(rb))
        return false;
    bool ok = false;
    de_gpu_cmd *c = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "views readback");
    const de_gpu_fence fence = de_gpu_fence_create(ctx->device, 0);
    if (c != NULL && DE_HANDLE_VALID(fence)) {
        de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, src, 0, 0);
        const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
        if (de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS, &s) == DE_OK &&
            de_gpu_fence_wait(ctx->device, fence, 1, 60ull * 1000000000ull) == DE_OK) {
            const void *m = de_gpu_buffer_map(ctx->device, rb);
            if (m != NULL) {
                memcpy(px, m, (size_t)pitch * h);
                ok = true;
            }
        }
    }
    if (DE_HANDLE_VALID(fence))
        de_gpu_fence_destroy(ctx->device, fence);
    de_gpu_buffer_destroy(ctx->device, rb);
    return ok;
}

DE_GOLDEN_SCENE(views_split_screen) {
    if (ctx->width != DE__RENDER_OFFSCREEN_WIDTH || ctx->height != DE__RENDER_OFFSCREEN_HEIGHT ||
        ctx->format != DE__RENDER_OFFSCREEN_FORMAT) {
        de_golden_fail(ctx, "scenes.toml: views_split_screen needs size [%u, %u], rgba8",
                       DE__RENDER_OFFSCREEN_WIDTH, DE__RENDER_OFFSCREEN_HEIGHT);
        return;
    }
    const u32 w = ctx->width, h = ctx->height, pitch = w * 4u; /* 5120: a multiple of 256 */
    /* The pixels go through an UPLOAD buffer the harness owns (it outlives the scene). */
    const de_gpu_buffer up =
        de_golden_upload_buffer(ctx, NULL, (u64)pitch * h, DE_USAGE_COPY_SRC, "views pixels");
    if (!DE_HANDLE_VALID(up))
        return;
    u8 *px = (u8 *)de_gpu_buffer_map(ctx->device, up);

    de_type_registry *types = NULL;
    de_world *world = NULL;
    de_renderer *r = NULL;
    de_gpu_texture monitor = {0};
    if (de_type_registry_create(NULL, &types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = types}, &world) != DE_OK ||
        de_renderer_create(&(de_renderer_desc){.device = ctx->device, .types = types}, &r) !=
            DE_OK) {
        const de_error *e = de_error_last();
        de_golden_fail(ctx, "setting up the renderer: %s", e != NULL ? e->message : "?");
        goto done;
    }
    if (!de__composite_ready(de__views_composite(r))) {
        de_golden_skip(ctx, "this build cannot cook the renderer's composite shader");
        goto done;
    }
    de_renderer_attach(r, world);
    const DeRenderComponents *ids = de__render_components(world);
    const de_builtin_component_ids bi = de_builtin_components(world);
    set_cvar("r.pipelined", "0");
    const de_render_settings s = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    de_renderer_set_settings(r, &s); /* render_scale 1 */

    monitor = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                            .width = 256,
                                            .height = 256,
                                            .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                            .name = "monitor"});
    const de_view left = de_view_create(
        r, &(de_view_desc){.name = "left", .viewport = {0.0f, 0.0f, 0.5f, 1.0f}, .order = 0});
    const de_view right = de_view_create(
        r, &(de_view_desc){.name = "right", .viewport = {0.5f, 0.0f, 0.5f, 1.0f}, .order = 1});
    const de_view pip = de_view_create(
        r, &(de_view_desc){.name = "pip", .viewport = {0.75f, 0.05f, 0.2f, 0.25f}, .order = 2});
    const de_view mon =
        de_view_create(r, &(de_view_desc){.name = "monitor", .target = monitor, .order = -1});
    if (!DE_HANDLE_VALID(monitor) || !DE_HANDLE_VALID(left) || !DE_HANDLE_VALID(right) ||
        !DE_HANDLE_VALID(pip) || !DE_HANDLE_VALID(mon) || ids == NULL) {
        de_golden_fail(ctx, "creating the views");
        goto done;
    }
    de_view_set_render_scale(r, right, 0.5f);
    (void)spawn_camera(world, ids, &bi, left, 0.0);
    (void)spawn_camera(world, ids, &bi, right, 1.0);
    (void)spawn_camera(world, ids, &bi, pip, 2.0);
    (void)spawn_camera(world, ids, &bi, mon, 3.0);
    inset in = {.r = r, .consumer = left, .tex = monitor};
    de_render_add_hook(r, DE_HOOK_UI, inset_hook, &in);
    for (u32 k = 0; k < 3; ++k) {
        de_result res = de_world_run_schedule(world, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f);
        if (res == DE_OK)
            res = de_world_run_schedule(world, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f);
        if (res == DE_OK)
            res = de__render_extract_and_kick(r, world);
        if (res != DE_OK) {
            const de_error *e = de_error_last();
            de_golden_fail(ctx, "frame %u: %s", k, e != NULL ? e->message : "?");
            goto done;
        }
    }
    if (px == NULL || !read_offscreen(ctx, de__renderer_offscreen(r), pitch, h, px)) {
        de_golden_fail(ctx, "reading the renderer's target back");
        goto done;
    }
    de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
    de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, pitch);

done:
    if (r != NULL)
        de_renderer_destroy(r); /* waits for the GPU */
    if (DE_HANDLE_VALID(monitor))
        de_gpu_texture_destroy(ctx->device, monitor);
    if (world != NULL)
        de_world_destroy(world);
    if (types != NULL)
        de_type_registry_destroy(types);
    set_cvar("r.pipelined", "1");
}
