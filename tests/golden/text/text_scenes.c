/* DENG-262: real renderer, font cooker and atlas uploader, fixed deterministic scenes. */
#include "golden.h"
#include "cooked_assets.h"
#include <dengine/de_text.h>
#include <dengine/de_task.h>
#include <dengine/de_platform.h>
#include "text/render/text_render_internal.h"
#include "render/components_internal.h"
#include "render/world/render_world.h"
#include <string.h>

typedef struct text_scene {
    de_asset_system *assets;
    char asset_pack[256];
    de_task_system *tasks;
    de_text_system *text;
    de_type_registry *types;
    de_world *world;
    de_renderer *renderer;
    de_font font, bitmap;
    de_load_handle loads[2];
} text_scene;
static de_result setup(text_scene *s, de_golden_ctx *ctx) {
    de_fp_env_init_thread();
    DE_TRY(de_fs_mount("textgolden", DE_GOLDEN_TEXT_ROOT, DE_MOUNT_READ_ONLY, 0));
    DE_TRY(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &s->tasks));
    DE_TRY(de_asset_system_create(
        &(de_asset_system_desc){.mode = DE_GOLDEN_ASSET_MODE, .tasks = s->tasks}, &s->assets));
    DE_TRY(de_text_create(NULL, s->assets, &s->text));
    DE_TRY(de_asset_manifest_load(s->assets, "textgolden:/tests/golden/text/assets.toml"));
    DE_TRY(de_golden_pack_assets(s->assets, "textgolden:/tests/data/fonts", s->asset_pack));
    const char *names[] = {"font/latin", "font/bitmap"};
    de_font *fonts[] = {&s->font, &s->bitmap};
    for (u32 i = 0; i < 2; ++i) {
        de_asset_id id = de_asset_id_from(names[i]);
        *fonts[i] = de_asset_font(s->assets, id);
        s->loads[i] = de_asset_load(s->assets, id, 0);
        if (de_asset_wait(s->assets, s->loads[i], 120ULL * 1000000000) != DE_LOAD_READY)
            return DE_ERR_IO;
    }
    DE_TRY(de_type_registry_create(NULL, &s->types));
    DE_TRY(de_world_create(&(de_world_desc){.types = s->types}, &s->world));
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){.device = ctx->device, .assets = s->assets, .types = s->types},
        &s->renderer));
    de_renderer_attach(s->renderer, s->world);
    de_cvar pipelined = de_cvar_find("r.pipelined");
    DE_TRY(de_cvar_set(pipelined, "false"));
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.render_scale = 1;
    settings.ao = settings.reflections = settings.gi = settings.sss = settings.shadows =
        DE_QUALITY_OFF;
    de_renderer_set_settings(s->renderer, &settings);
    const DeRenderComponents *ids = de__render_components(s->world);
    de_builtin_component_ids bi = de_builtin_components(s->world);
    de_component components[] = {bi.transform, ids->camera};
    de_entity camera;
    DE_TRY(de_entity_spawn_with(s->world, components, 2, &camera));
    DeTransform *transform = de_entity_get(s->world, camera, bi.transform);
    *transform = de_transform_identity();
    transform->position = (de_dvec3){0, 0, 2};
    ((DeCamera *)de_entity_get(s->world, camera, ids->camera))->exposure_ev100 = 0;
    DE_TRY(de_text_attach(s->text, s->renderer, s->world));
    return DE_OK;
}
static void cleanup(text_scene *s, de_golden_ctx *ctx) {
    de_text_attach(s->text, NULL, NULL);
    if (s->assets) {
        for (u32 i = 0; i < 2; ++i)
            if (s->loads[i].id)
                de_asset_release(s->assets, s->loads[i]);
        de_asset_system_destroy(s->assets);
    }
    de_text_destroy(s->text);
    if (s->renderer)
        de_renderer_destroy(s->renderer);
    if (s->world)
        de_world_destroy(s->world);
    if (s->types)
        de_type_registry_destroy(s->types);
    if (s->tasks)
        de_task_system_destroy(s->tasks);
    de_result packed_cleanup = de_golden_pack_cleanup(s->asset_pack);
    if (packed_cleanup != DE_OK)
        de_golden_fail(ctx, "golden pack cleanup: %s", de_result_str(packed_cleanup));
    de_fs_unmount("textgolden", DE_GOLDEN_TEXT_ROOT);
}
static de_result string_draw(text_scene *s, const char *string, f32 size, f32 x, f32 y, bool bitmap,
                             bool effects) {
    de_font handle = bitmap ? s->bitmap : s->font;
    de_text_font *font = de_text__font_get(s->text, handle);
    de_glyph_quad quads[128];
    u32 count = 0;
    for (const char *p = string; *p && count < 128; ++p) {
        de_text_glyph_entry e;
        if (!de_text__glyph(s->text, handle, de_text__codepoint(font, (u8)*p), &e))
            return DE_ERR_NOT_FOUND;
        f32 scale = bitmap ? size / font->em : size;
        if (e.w && e.h)
            quads[count++] = (de_glyph_quad){.pos = {x + e.plane.x * scale, y + e.plane.y * scale,
                                                     (e.plane.z - e.plane.x) * scale,
                                                     (e.plane.w - e.plane.y) * scale},
                                             .uv = e.uv,
                                             .atlas_page = e.page,
                                             .color = {1, 1, 1, 1}};
        x += e.advance * scale;
    }
    de_text_draw_opts opts = {.font = handle, .pixel_snap = bitmap};
    if (effects) {
        opts.outline_width = 2;
        opts.outline_color = (de_vec4){.08f, .1f, .6f, 1};
        opts.shadow_offset = (de_vec2){7, 9};
        opts.shadow_color = (de_vec4){0, 0, 0, .8f};
    }
    return de_text_draw(s->text, (de_view){0}, quads, count, (de_vec2){0}, &opts);
}
static de_result spawn_world(text_scene *s, f64 z, f32 x, bool lit) {
    de_builtin_component_ids bi = de_builtin_components(s->world);
    de_component c[] = {bi.transform, de_component_find(s->world, "DeWorldText")};
    de_entity entity;
    DE_TRY(de_entity_spawn_with(s->world, c, 2, &entity));
    DeTransform *tr = de_entity_get(s->world, entity, bi.transform);
    *tr = de_transform_identity();
    tr->position = (de_dvec3){x, 0, z};
    DeWorldText *text = de_entity_get(s->world, entity, c[1]);
    *text = (DeWorldText){.style = {.font = s->font, .color = {1, 1, 1, 1}},
                          .size_m = z < -10 ? 8 : .35f,
                          .billboard = true,
                          .lit = lit,
                          .depth_test = true,
                          .align = DE_ALIGN_CENTER};
    memcpy(text->literal, lit ? "50 m lit" : "2 m", lit ? 9 : 4);
    return DE_OK;
}
static void scene(de_golden_ctx *ctx, u32 kind) {
    text_scene s = {0};
    de_result result = setup(&s, ctx);
    if (result != DE_OK) {
        de_golden_fail(ctx, "text scene setup: %d", result);
        cleanup(&s, ctx);
        return;
    }
    if (kind == 2) {
        de_text_font *font = de_text__font_get(s.text, s.bitmap);
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x) {
                u8 *pixel = font->pixels[0] + 4 * (y * font->width + x);
                pixel[0] = pixel[1] = pixel[2] = 255;
                pixel[3] = ((x + y) & 1) ? 0 : 255;
            }
    }
    if (kind == 3) {
        de_builtin_component_ids bi = de_builtin_components(s.world);
        const DeRenderComponents *ids = de__render_components(s.world);
        de_component sprite_components[] = {bi.transform, ids->sprite};
        de_entity backdrop;
        result = de_entity_spawn_with(s.world, sprite_components, 2, &backdrop);
        if (result == DE_OK) {
            DeTransform transform = de_transform_identity();
            transform.position = (de_dvec3){0, -1, -55};
            *(DeTransform *)de_entity_get(s.world, backdrop, bi.transform) = transform;
            *(DeSprite *)de_entity_get(s.world, backdrop, ids->sprite) =
                (DeSprite){.size = {80, 35},
                           .pivot = {.5f, .5f},
                           .uv_rect = {0, 0, 1, 1},
                           .color = {.08f, .1f, .15f, 1}};
        }
        de_component components[] = {bi.transform, ids->light};
        de_entity sun;
        result = de_entity_spawn_with(s.world, components, 2, &sun);
        if (result == DE_OK) {
            *(DeTransform *)de_entity_get(s.world, sun, bi.transform) = de_transform_identity();
            *(DeLight *)de_entity_get(s.world, sun, ids->light) =
                (DeLight){.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 3};
        }
        if (result == DE_OK)
            result = spawn_world(&s, 0, -.5f, false);
        if (result == DE_OK)
            result = spawn_world(&s, -48, 10, true);
    }
    for (u32 frame = 0; frame < 3 && result == DE_OK; ++frame) {
        de_text_update(s.text);
        if (kind == 0) {
            const f32 sizes[] = {8, 16, 64, 512};
            const f32 ys[] = {25, 65, 125, 260};
            for (u32 i = 0; i < 4 && result == DE_OK; ++i)
                result = string_draw(&s, "Sdf", sizes[i], 25, ys[i], false, false);
        }
        if (kind == 1)
            result = string_draw(&s, "Outline + shadow", 84, 40, 220, false, true);
        if (kind == 2) {
            result = string_draw(&s, "AAAA", 8, 32, 40, true, false);
            if (result == DE_OK)
                result = string_draw(&s, "AAAA", 24, 32, 110, true, false);
        }
        if (result == DE_OK)
            result = de_world_run_schedule(s.world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0);
        if (result == DE_OK)
            result = de_world_run_schedule(s.world, DE_SCHEDULE_EXTRACT, 1.f / 60, 0);
        if (result == DE_OK)
            result = de__render_extract_and_kick(s.renderer, s.world);
        de__render_world_wait_idle(s.renderer);
    }
    if (result != DE_OK)
        de_golden_fail(ctx, "text scene frame: %d", result);
    u32 pitch = 1280 * 4;
    u64 bytes = (u64)pitch * 720;
    de_gpu_buffer rb =
        de_gpu_buffer_create(ctx->device, &(de_gpu_buffer_desc){.size = bytes,
                                                                .usage = DE_USAGE_COPY_DST,
                                                                .memory = DE_MEM_READBACK,
                                                                .name = "text golden readback"});
    de_gpu_cmd *cmd = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "text golden copy");
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, pitch, de__renderer_offscreen(s.renderer), 0, 0);
    de_gpu_fence fence = de_gpu_fence_create(ctx->device, 0);
    de_gpu_submit submit = {.cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = 1};
    result = de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS, &submit);
    if (result == DE_OK)
        result = de_gpu_fence_wait(ctx->device, fence, 1, 120ULL * 1000000000);
    if (result == DE_OK) {
        const u8 *pixels = de_gpu_buffer_map(ctx->device, rb);
        if (kind == 3) {
            u32 white = 0, lit = 0;
            for (u64 pixel = 0; pixel < bytes; pixel += 4) {
                if (pixels[pixel] > 240 && pixels[pixel + 1] > 240 && pixels[pixel + 2] > 240)
                    ++white;
                if (pixels[pixel] > 120 && pixels[pixel] < 220 &&
                    pixels[pixel] == pixels[pixel + 1] && pixels[pixel] == pixels[pixel + 2])
                    ++lit;
            }
            if (white < 500 || lit < 500)
                de_golden_fail(ctx, "world text content missing: white=%u lit=%u", white, lit);
        }
        de_gpu_buffer up =
            de_golden_upload_buffer(ctx, pixels, bytes, DE_USAGE_COPY_SRC, "text golden output");
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, pitch);
    } else
        de_golden_fail(ctx, "text readback: %d", result);
    de_gpu_fence_destroy(ctx->device, fence);
    de_gpu_buffer_destroy(ctx->device, rb);
    cleanup(&s, ctx);
}
DE_GOLDEN_SCENE(text_msdf_scales) {
    scene(ctx, 0);
}
DE_GOLDEN_SCENE(text_outline_shadow) {
    scene(ctx, 1);
}
DE_GOLDEN_SCENE(text_bitmap_pixel) {
    scene(ctx, 2);
}
DE_GOLDEN_SCENE(text_world_text) {
    scene(ctx, 3);
}
