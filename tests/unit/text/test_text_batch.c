#include "runtime_test_util.h"
#include "text/render/text_render_internal.h"
#include "gpu/gpu_backend.h"
#include "gpu/texture_lease_internal.h"
#include <stb_image_write.h>
#if DE_HAS_RENDER
#include "render/world/render_world.h"
#include "render/components_internal.h"
#include <stdlib.h>
#include <math.h>
#define TEXT_WAIT (30ULL * 1000000000)
typedef struct text_render_fx {
    runtime_fx fonts;
    de_gpu_device *d;
    de_renderer *r;
    de_world *w;
    de_type_registry *types;
    de_font bitmap;
    de_load_handle scene_loads[3];
    de_gpu_texture screen_target;
    de_atomic_u32 screen_errors;
    bool screen_sink;
    u32 basic_outputs;
    de_gpu_buffer world_depth;
    bool world_depth_found;
} text_render_fx;
static void screen_error_sink(const de_log_record *record, void *user) {
    text_render_fx *f = user;
    if (record->level >= DE_LOG_ERROR)
        de_atomic_fetch_add_u32(&f->screen_errors, 1, DE_ATOMIC_RELAXED);
}
static void text_render_fx_setup(text_render_fx *f) {
    (void)f;
}
static void text_render_fx_teardown(text_render_fx *f) {
    if (f->screen_sink) {
        de_log_flush();
        de_log_remove_sink(screen_error_sink, f);
    }
    if (f->fonts.t)
        de_text_attach(f->fonts.t, NULL, NULL);
    de_text_destroy(f->fonts.t);
    f->fonts.t = NULL;
    if (f->fonts.assets) {
        for (u32 i = 0; i < 3; ++i)
            if (f->scene_loads[i].id)
                de_asset_release(f->fonts.assets, f->scene_loads[i]);
        for (u32 i = 0; i < 3; ++i)
            if (f->fonts.loads[i].id)
                de_asset_release(f->fonts.assets, f->fonts.loads[i]);
        de_asset_system_destroy(f->fonts.assets);
        f->fonts.assets = NULL;
    }
    if (f->r)
        de_renderer_destroy(f->r);
    if (f->screen_target.id)
        de_gpu_texture_destroy(f->d, f->screen_target);
    if (f->world_depth.id)
        de_gpu_buffer_destroy(f->d, f->world_depth);
    if (f->w)
        de_world_destroy(f->w);
    if (f->types)
        de_type_registry_destroy(f->types);
    if (f->d)
        de_gpu_device_destroy(f->d);
    de_fs_unmount("fontall", DE_FONT_DATA "/..");
    de_fs_unmount("textscene", DE_FONT_DATA "/../../../..");
}
static void start_render(text_render_fx *f, de_gpu_backend backend) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    runtime_fx_setup_with_types(&f->fonts, f->types);
    de_result result = de_gpu_device_create(
        &(de_gpu_device_desc){
            .backend = backend, .debug_layer = backend != DE_GPU_NULL, .pipeline_cache_dir = ""},
        &f->d);
    if (result != DE_OK)
        SKIP("native device unavailable");
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->types}, &f->w), DE_OK);
    ASSERT_EQ(de_renderer_create(
                  &(de_renderer_desc){.device = f->d, .assets = f->fonts.assets, .types = f->types},
                  &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->w);
    de_cvar pipeline = de_cvar_register(
        &(de_cvar_desc){.name = "r.pipelined", .type = DE_CVAR_BOOL, .default_number = 0});
    ASSERT_EQ(de_cvar_set(pipeline, "false"), DE_OK);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.render_scale = 1;
    settings.ao = 0;
    settings.reflections = 0;
    settings.gi = 0;
    settings.sss = 0;
    settings.shadows = 0;
    de_renderer_set_settings(f->r, &settings);
    const DeRenderComponents *ids = de__render_components(f->w);
    de_builtin_component_ids bi = de_builtin_components(f->w);
    de_component cs[] = {bi.transform, ids->camera};
    de_entity entity;
    ASSERT_EQ(de_entity_spawn_with(f->w, cs, 2, &entity), DE_OK);
    DeTransform *transform = de_entity_get(f->w, entity, bi.transform);
    *transform = de_transform_identity();
    transform->position = (de_dvec3){0, 0, 2};
    ASSERT_EQ(de_text_attach(f->fonts.t, f->r, f->w), DE_OK);
    de_asset_id id = de_asset_id_from("font/bitmap");
    f->bitmap = de_asset_font(f->fonts.assets, id);
    de_load_handle h = de_asset_load(f->fonts.assets, id, 0);
    ASSERT_EQ(de_asset_wait(f->fonts.assets, h, TEXT_WAIT), (int)DE_LOAD_READY);
    de_asset_release(f->fonts.assets, h);
}
static void render_frame(text_render_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->w), DE_OK);
    de__render_world_wait_idle(f->r);
}
TEST_F(text_batch, reloaded_atlas_reuses_generation_slots_and_preserves_pending_packet_lease,
       text_render_fx) {
#if DE__ASSET_RELOAD_ENABLED
    start_render(f, DE_GPU_NULL);
    de_gpu_texture old = de_text_page_texture(f->fonts.t, f->fonts.latin, 0);
    ASSERT_TRUE(old.id);
    u32 textures = de__gpu_live_count(f->d, DE__GPU_TEXTURE);
    for (u32 n = 0; n < 32; ++n) {
        ASSERT_TRUE(de__gpu_texture_pin(f->d, old));
        u64 identity = de_text__font_get(f->fonts.t, f->fonts.latin)->identity;
        de__asset_reload_on_change(f->fonts.assets, "fontall:/latin/NotoSans.ttf",
                                   DE_WATCH_MODIFIED, false);
        u64 end = de_time_now_ns() + TEXT_WAIT;
        while (de_text__font_get(f->fonts.t, f->fonts.latin)->identity == identity &&
               de_time_now_ns() < end) {
            de_asset_system_update(f->fonts.assets, NULL);
            de_text_update(f->fonts.t);
            de_sleep_ns(1000000);
        }
        ASSERT_NE(de_text__font_get(f->fonts.t, f->fonts.latin)->identity, identity);
        de_text_update(f->fonts.t);
        de_gpu_texture next = de_text_page_texture(f->fonts.t, f->fonts.latin, 0);
        ASSERT_TRUE(next.id);
        EXPECT_NE(next.id, old.id);
        EXPECT_EQ(f->fonts.t->render->page_count, 1u);
        de_gpu_wait_idle(f->d);
        EXPECT_TRUE(de__gpu_get_texture(f->d, old) != NULL);
        de__gpu_texture_unpin(f->d, old);
        EXPECT_TRUE(de__gpu_get_texture(f->d, old) == NULL);
        de_gpu_wait_idle(f->d);
        EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), textures);
        old = next;
    }
#else
    SKIP("hot reload is a Debug/Dev feature");
#endif
}
static de_glyph_quad quad(text_render_fx *f, de_font font, u32 cp, f32 x, f32 y, f32 size) {
    de_text_font *raw = de_text__font_get(f->fonts.t, font);
    u32 glyph = de_text__codepoint(raw, cp);
    de_text_glyph_entry e;
    ASSERT_TRUE(de_text__glyph(f->fonts.t, font, glyph, &e));
    f32 scale = raw->mode == DE_FONT_MSDF ? size : size / raw->em;
    return (de_glyph_quad){
        .pos = {x, y, (e.plane.z - e.plane.x) * scale, (e.plane.w - e.plane.y) * scale},
        .uv = e.uv,
        .atlas_page = e.page,
        .color = {1, 1, 1, 1}};
}
TEST_F(text_batch, submission_clipping_and_upload_regions, text_render_fx) {
    start_render(f, DE_GPU_NULL);
    de_text_update(f->fonts.t);
    de_glyph_quad q = quad(f, f->bitmap, 'A', 10, 20, 8);
    de_text_draw_opts options = {.font = f->bitmap, .clip_px = {10, 20, 4, 8}, .pixel_snap = true};
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &q, 1, (de_vec2){.2f, .2f}, &options), DE_OK);
    de__text_queue *queue = &f->fonts.t->render->queues[0];
    EXPECT_EQ(queue->screen_count, 1u);
    EXPECT_FLOAT_EQ(queue->screen[0].gpu.rect.x, 10, .001);
    EXPECT_FLOAT_EQ(queue->screen[0].gpu.clip.z, 4, .001);
    render_frame(f);
    de_text_render_stats stats = de_text_get_render_stats(f->fonts.t);
    EXPECT_EQ(stats.draw_calls, 1u);
    EXPECT_EQ(stats.glyphs, 1u);
    EXPECT_EQ(stats.uploads, 1u);
    de_text_font *font = de_text__font_get(f->fonts.t, f->bitmap);
    de_text_update(f->fonts.t);
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &q, 1, (de_vec2){0}, &options), DE_OK);
    render_frame(f);
    EXPECT_EQ(de_text_get_render_stats(f->fonts.t).uploads, 0u);
    font->pixels[0][4 * (3 * font->width + 2)] = 20;
    font->dirty[font->dirty_count++] = (de_text_dirty_rect){0, 2, 3, 1, 1};
    de_text_update(f->fonts.t);
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &q, 1, (de_vec2){0}, &options), DE_OK);
    render_frame(f);
    stats = de_text_get_render_stats(f->fonts.t);
    EXPECT_EQ(stats.uploads, 1u);
    EXPECT_EQ(stats.uploaded_bytes, 4u);
}
TEST_F(text_batch, mixed_fonts_same_page_zero, text_render_fx) {
    start_render(f, DE_GPU_NULL);
    de_text_update(f->fonts.t);
    de_glyph_quad glyphs[] = {quad(f, f->fonts.latin, 'A', 10, 10, 16),
                              quad(f, f->bitmap, 'A', 40, 10, 8)};
    glyphs[0].font = f->fonts.latin;
    glyphs[1].font = f->bitmap;
    ASSERT_EQ(glyphs[0].atlas_page, 0u);
    ASSERT_EQ(glyphs[1].atlas_page, 0u);
    de_text_draw_opts opts = {.font = f->fonts.latin};
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, glyphs, 2, (de_vec2){0}, &opts), DE_OK);
    de__text_queue *queue = &f->fonts.t->render->queues[0];
    EXPECT_EQ(queue->screen[0].font.id, f->fonts.latin.id);
    EXPECT_EQ(queue->screen[1].font.id, f->bitmap.id);
    render_frame(f);
    EXPECT_EQ(de_text_get_render_stats(f->fonts.t).draw_calls, 1u);
    EXPECT_EQ(de_text_get_render_stats(f->fonts.t).glyphs, 2u);
    de_gpu_texture latin = de_text_page_texture(f->fonts.t, f->fonts.latin, 0);
    de_gpu_texture bitmap = de_text_page_texture(f->fonts.t, f->bitmap, 0);
    EXPECT_NE(latin.id, bitmap.id);
}
TEST_F(text_batch, ten_thousand_one_draw, text_render_fx) {
    start_render(f, DE_GPU_NULL);
    de_text_update(f->fonts.t);
    de_glyph_quad *quads =
        DE_ALLOC(de_heap_allocator(), 10000 * sizeof *quads, _Alignof(de_glyph_quad), DE_MEM_UI);
    ASSERT_TRUE(quads);
    de_glyph_quad a = quad(f, f->fonts.latin, 'A', 0, 0, 16);
    for (u32 i = 0; i < 10000; ++i) {
        quads[i] = a;
        quads[i].pos.x = (f32)(i % 100) * 10;
        quads[i].pos.y = (f32)(i / 100) * 10;
    }
    de_text_draw_opts options = {.font = f->fonts.latin};
    u64 start = de_time_now_ns();
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, quads, 10000, (de_vec2){0}, &options), DE_OK);
    u64 duration = de_time_now_ns() - start;
    printf("10000 screen glyph submit %.3fms\n", (double)duration / 1e6);
    render_frame(f);
    de_text_render_stats stats = de_text_get_render_stats(f->fonts.t);
    EXPECT_EQ(stats.glyphs, 10000u);
    EXPECT_LE(stats.draw_calls, 2u);
#if defined(DE_DEV)
    EXPECT_LT(duration, 300000u);
#endif
    DE_FREE(de_heap_allocator(), quads, 10000 * sizeof *quads);
}
static u8 *readback_target(text_render_fx *f, de_gpu_texture texture) {
    u32 w = 1280, h = 720, pitch = w * 4;
    de_gpu_wait_idle(f->d);
    de_gpu_buffer buffer =
        de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = (u64)pitch * h,
                                                         .usage = DE_USAGE_COPY_DST,
                                                         .memory = DE_MEM_READBACK,
                                                         .name = "text readback"});
    ASSERT_TRUE(buffer.id);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "text readback");
    de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, 0, pitch, texture, 0, 0);
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    de_gpu_submit submit = {.cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &submit), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->d, fence, 1, TEXT_WAIT), DE_OK);
    u8 *out = malloc((size_t)pitch * h);
    ASSERT_TRUE(out);
    memcpy(out, de_gpu_buffer_map(f->d, buffer), (size_t)pitch * h);
    de_gpu_fence_destroy(f->d, fence);
    de_gpu_buffer_destroy(f->d, buffer);
    return out;
}
static u8 *readback(text_render_fx *f) {
    return readback_target(f, de__renderer_offscreen(f->r));
}
#if defined(__APPLE__)
static void coverage_clear_stage(de__view_frame *vf, void *user);
typedef struct world_depth_copy {
    de_rg_resource depth, buffer;
} world_depth_copy;
static void world_depth_copy_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    world_depth_copy *copy = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, copy->buffer), 0, 1280 * 4,
                                      de_rg_texture(pass, copy->depth), 0, 0);
}
static void world_depth_copy_stage(de__view_frame *vf, void *user) {
    text_render_fx *f = user;
    for (u32 i = 0; i < vf->graph->resource_count; ++i) {
        const de__rg_resource *resource = &vf->graph->resources[i];
        if (strcmp(resource->name, "text.depth") != 0)
            continue;
        world_depth_copy *copy = de_arena_push_zero(vf->graph->arena, sizeof *copy, 16);
        if (!copy)
            return;
        copy->depth = de__rg_resource_handle(vf->graph, i);
        copy->buffer = de_rg_import_buffer(vf->graph, f->world_depth, DE_ACCESS_COPY_DST);
        de_rg_pass *pass = de_rg_add_pass(vf->graph, "text.depth.readback", DE_QUEUE_GRAPHICS,
                                          world_depth_copy_execute, copy);
        if (pass) {
            de_rg_read(pass, copy->depth, DE_ACCESS_COPY_SRC);
            de_rg_write(pass, copy->buffer, DE_ACCESS_COPY_DST);
            f->world_depth_found = true;
        }
        return;
    }
}
#endif
TEST_F(text_batch, metal_world_text_only_has_valid_cleared_depth_and_visible_glyphs,
       text_render_fx) {
#if defined(__APPLE__)
    start_render(f, DE_GPU_METAL);
    de_asset_id font_id = de_asset_id_from("font/tt-msdf");
    f->scene_loads[0] = de_asset_load(f->fonts.assets, font_id, 0);
    ASSERT_EQ(de_asset_wait(f->fonts.assets, f->scene_loads[0], TEXT_WAIT), (int)DE_LOAD_READY);
    de_log_add_sink(screen_error_sink, f);
    f->screen_sink = true;
    f->world_depth = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = 1280 * 720 * 4,
                                                                      .usage = DE_USAGE_COPY_DST,
                                                                      .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->world_depth.id);
    de_entity entity = de_entity_spawn(f->w);
    de_builtin_component_ids bi = de_builtin_components(f->w);
    DeTransform *transform = de_entity_add(f->w, entity, bi.transform);
    *transform = de_transform_identity();
    transform->position.x = -.5;
    DeWorldText *text = de_entity_add(f->w, entity, de_component_find(f->w, "DeWorldText"));
    ASSERT_TRUE(text);
    *text = (DeWorldText){
        .style = {.font = de_asset_font(f->fonts.assets, font_id), .color = {0, 1, 0, 1}},
        .size_m = .5f,
        .depth_test = true};
    strcpy(text->literal, "BOag");
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_VISIBILITY, 0, "world.background",
                                      coverage_clear_stage, NULL),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_TRANSPARENT, 101, "text.depth.readback",
                                      world_depth_copy_stage, f),
              DE_OK);
    de_text_update(f->fonts.t);
    render_frame(f);
    EXPECT_TRUE(f->world_depth_found);
    u8 *pixels = readback(f);
    u32 green = 0;
    for (u32 i = 0; i < 1280 * 720; ++i)
        green += pixels[i * 4 + 1] > 128 && pixels[i * 4] < 16 && pixels[i * 4 + 2] < 16;
    printf("text-only world green glyph pixels=%u\n", green);
    EXPECT_GT(green, 500u);
    ASSERT_TRUE(
        stbi_write_png(DE_TEXT_RENDER_CAPTURE ".world.png", 1280, 720, 4, pixels, 1280 * 4));
    free(pixels);
    const f32 *depth = de_gpu_buffer_map(f->d, f->world_depth);
    ASSERT_TRUE(depth);
    for (u32 i = 0; i < 1280 * 720; ++i)
        ASSERT_FLOAT_EQ(depth[i], 0, 0);
    de_log_flush();
    EXPECT_EQ(de_atomic_load_u32(&f->screen_errors, DE_ATOMIC_RELAXED), 0u);
    de__view_unregister_stage(f->r, world_depth_copy_stage, f);
    de__view_unregister_stage(f->r, coverage_clear_stage, NULL);
#else
    SKIP("Metal native host required");
#endif
}
#if defined(__APPLE__)
static de_load_state wait_screen_asset(text_render_fx *f, de_load_handle load) {
    u64 deadline = de_time_now_ns() + TEXT_WAIT;
    de_load_state state;
    do {
        de_render_uploader_flush(f->r->uploader);
        de_render_uploader_acquire_submit(f->r->uploader);
        de_asset_system_update(f->fonts.assets, NULL);
        state = de_asset_wait(f->fonts.assets, load, 1000000);
    } while ((state == DE_LOAD_PENDING || state == DE_LOAD_LOADING) && de_time_now_ns() < deadline);
    return state;
}
static void observe_basic_output(de__view_frame *vf, void *user) {
    text_render_fx *f = user;
    u32 index = de__rg_resource_index(vf->graph, vf->color);
    if (index < vf->graph->resource_count) {
        const de__rg_resource *resource = &vf->graph->resources[index];
        if (strcmp(resource->name, "output.basic") == 0 &&
            (resource->tex.usage & DE_USAGE_RENDER_TARGET) &&
            resource->tex.format == DE_FMT_RGBA16_FLOAT)
            ++f->basic_outputs;
    }
}
#endif
TEST_F(text_batch, metal_screen_text_over_lit_basic_output_main_rtt_and_hdr, text_render_fx) {
#if defined(__APPLE__)
    start_render(f, DE_GPU_METAL);
    de_log_add_sink(screen_error_sink, f);
    f->screen_sink = true;
    ASSERT_EQ(de_fs_mount("textscene", DE_FONT_DATA "/../../../..", DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(
        de_asset_manifest_load(f->fonts.assets, "textscene:/tests/unit/text/screen-assets.toml"),
        DE_OK);
    const char *assets[] = {"mesh/card", "mat/white", "ui/pixel_font"};
    for (u32 i = 0; i < 3; ++i) {
        f->scene_loads[i] = de_asset_load(f->fonts.assets, de_asset_id_from(assets[i]), 0);
        de_load_state state = wait_screen_asset(f, f->scene_loads[i]);
        de_load_info info = {0};
        de_asset_status(f->fonts.assets, f->scene_loads[i], &info);
        printf("loading %s: %u %s\n", assets[i], state, info.first_error_msg);
        ASSERT_EQ(state, DE_LOAD_READY);
    }
    de_asset_id mesh_id = de_asset_id_from("mesh/card#card");
    de_mesh mesh = de_asset_mesh(f->fonts.assets, mesh_id);
    de_load_handle mesh_load = de_asset_load(f->fonts.assets, mesh_id, 0);
    ASSERT_EQ(wait_screen_asset(f, mesh_load), (int)DE_LOAD_READY);
    de_asset_release(f->fonts.assets, mesh_load);
    const DeRenderComponents *ids = de__render_components(f->w);
    de_builtin_component_ids bi = de_builtin_components(f->w);
    de_entity card = de_entity_spawn(f->w);
    DeTransform *card_transform = de_entity_add(f->w, card, bi.transform);
    *card_transform = de_transform_identity();
    /* Model import converts the glTF card normal to -Y; face this camera on +Z. */
    card_transform->rotation = (de_quat){-.70710678f, 0, 0, .70710678f};
    DeMeshRenderer *drawable = de_entity_add(f->w, card, ids->mesh_renderer);
    drawable->mesh = mesh;
    drawable->materials[0] = de_asset_material(f->fonts.assets, de_asset_id_from("mat/white"));
    de_entity world_label = de_entity_spawn(f->w);
    DeTransform *world_transform = de_entity_add(f->w, world_label, bi.transform);
    *world_transform = de_transform_identity();
    world_transform->position = (de_dvec3){-.15, -.1, -.25};
    DeWorldText *world_text =
        de_entity_add(f->w, world_label, de_component_find(f->w, "DeWorldText"));
    *world_text = (DeWorldText){
        .style = {.font = de_asset_font(f->fonts.assets, de_asset_id_from("ui/pixel_font")),
                  .color = {0, 1, 0, 1}},
        .size_m = .3f,
        .depth_test = true};
    strcpy(world_text->literal, "A");
    f->screen_target = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 1280,
                                     .height = 720,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                              DE_USAGE_COPY_SRC});
    ASSERT_TRUE(f->screen_target.id);
    de_view rtt =
        de_view_create(f->r, &(de_view_desc){.target = f->screen_target, .name = "text.rtt"});
    ASSERT_TRUE(rtt.id);
    de_entity camera = de_entity_spawn(f->w);
    DeTransform *transform = de_entity_add(f->w, camera, bi.transform);
    *transform = de_transform_identity();
    transform->position.z = 2;
    DeCamera *rtt_camera = de_entity_add(f->w, camera, ids->camera);
    *rtt_camera = (DeCamera){
        .view = rtt, .projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 2, .near_z = .1f};
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_OUTPUT, -100, "text.observe_basic",
                                      observe_basic_output, f),
              DE_OK);
    de_font font = de_asset_font(f->fonts.assets, de_asset_id_from("ui/pixel_font"));
    for (u32 medium = 0; medium < 2; ++medium)
        for (u32 hdr = 0; hdr < 2; ++hdr) {
            de_render_settings settings = de_render_settings_preset(
                DE_TIER_BASE, medium ? DE_QUALITY_MEDIUM : DE_QUALITY_LOW);
            settings.render_scale = 1;
            settings.ao = settings.reflections = settings.gi = settings.sss = settings.shadows = 0;
            de_renderer_set_settings(f->r, &settings);
            /* Offscreen exercises the HDR text multiplier without requiring an HDR display. */
            f->r->hdr_output = hdr != 0;
            de_text_update(f->fonts.t);
            de_glyph_quad q = quad(f, font, 'A', 32, 32, 64);
            q.color = (de_vec4){0, 1, 0, 1};
            de_text_draw_opts options = {.font = font, .pixel_snap = true};
            ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &q, 1, (de_vec2){0}, &options), DE_OK);
            ASSERT_EQ(de_text_draw(f->fonts.t, rtt, &q, 1, (de_vec2){0}, &options), DE_OK);
            u32 before = f->basic_outputs;
            render_frame(f);
            EXPECT_EQ(f->basic_outputs - before, 2u);
            for (u32 target = 0; target < 2; ++target) {
                u8 *pixels =
                    readback_target(f, target ? f->screen_target : de__renderer_offscreen(f->r));
                u32 green = 0;
                for (u32 y = 32; y < 96; ++y)
                    for (u32 x = 32; x < 96; ++x) {
                        const u8 *p = pixels + 4 * (y * 1280 + x);
                        green += p[1] > 128 && p[0] < 16 && p[2] < 16;
                    }
                printf("screen text medium=%u hdr=%u rtt=%u green=%u\n", medium, hdr, target,
                       green);
                EXPECT_GT(green, 500u);
                u32 occluded = 0;
                for (u32 y = 100; y < 620; ++y)
                    for (u32 x = 100; x < 1180; ++x) {
                        const u8 *p = pixels + 4 * (y * 1280 + x);
                        occluded += p[1] > 128 && p[0] < 16 && p[2] < 16;
                    }
                EXPECT_EQ(occluded, 0u);
                if (!medium && !hdr && !target)
                    ASSERT_TRUE(stbi_write_png(DE_TEXT_RENDER_CAPTURE ".screen.png", 1280, 720, 4,
                                               pixels, 1280 * 4));
                free(pixels);
            }
            de_log_flush();
            EXPECT_EQ(de_atomic_load_u32(&f->screen_errors, DE_ATOMIC_RELAXED), 0u);
        }
    /* Existing opaque depth must be reused, while the explicit overlay flag bypasses it. */
    world_text = de_entity_get(f->w, world_label, de_component_find(f->w, "DeWorldText"));
    world_text->depth_test = false;
    de_text_update(f->fonts.t);
    render_frame(f);
    for (u32 target = 0; target < 2; ++target) {
        u8 *pixels = readback_target(f, target ? f->screen_target : de__renderer_offscreen(f->r));
        u32 overlay = 0;
        for (u32 i = 0; i < 1280 * 720; ++i)
            overlay += pixels[i * 4 + 1] > 128 && pixels[i * 4] < 16 && pixels[i * 4 + 2] < 16;
        printf("world overlay rtt=%u green=%u\n", target, overlay);
        EXPECT_GT(overlay, 100u);
        free(pixels);
    }
    de_log_flush();
    EXPECT_EQ(de_atomic_load_u32(&f->screen_errors, DE_ATOMIC_RELAXED), 0u);
    de__view_unregister_stage(f->r, observe_basic_output, f);
    de_view_destroy(f->r, rtt);
#else
    SKIP("Metal native host required");
#endif
}
#if defined(__APPLE__)
typedef struct coverage_clear {
    de_rg_resource target;
} coverage_clear;
static void coverage_clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    coverage_clear *p = user;
    de_gpu_attachment attachment = {.texture = de_rg_texture(pass, p->target),
                                    .clear = true,
                                    .store = true,
                                    .clear_color = {.01f, .015f, .02f, 1}};
    de_gpu_cmd_begin_render(cmd, &attachment, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}
static void coverage_clear_stage(de__view_frame *vf, void *user) {
    (void)user;
    coverage_clear *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    if (!p)
        return;
    p->target = vf->color;
    de_rg_pass *pass = de_rg_add_pass(vf->graph, "coverage.background", DE_QUEUE_GRAPHICS,
                                      coverage_clear_execute, p);
    if (pass)
        de_rg_write(pass, p->target, DE_ACCESS_RENDER_TARGET);
}
#endif
TEST_F(text_batch, metal_msdf_effects_ignore_neighboring_atlas_glyphs, text_render_fx) {
#if defined(__APPLE__)
    start_render(f, DE_GPU_METAL);
    de_asset_id id = de_asset_id_from("font/tt-msdf");
    f->scene_loads[0] = de_asset_load(f->fonts.assets, id, 0);
    ASSERT_EQ(de_asset_wait(f->fonts.assets, f->scene_loads[0], TEXT_WAIT), (int)DE_LOAD_READY);
    de_font handle = de_asset_font(f->fonts.assets, id);
    de_text_font *font = de_text__font_get(f->fonts.t, handle);
    de_text_glyph_entry glyph;
    ASSERT_TRUE(de_text__glyph(f->fonts.t, handle, de_text__codepoint(font, 'A'), &glyph));
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_OUTPUT, 0, "effects.background",
                                      coverage_clear_stage, NULL),
              DE_OK);
    de_glyph_quad q = quad(f, handle, 'A', 96, 96, 84);
    de_text_draw_opts options = {.font = handle,
                                 .outline_width = 2,
                                 .outline_color = {0, 0, 1, 1},
                                 .shadow_offset = {7, 9},
                                 .shadow_color = {1, 0, 0, 1}};
    /* Preserve the real-font native image before constructing adversarial neighbors. */
    de_text_update(f->fonts.t);
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &q, 1, (de_vec2){0}, &options), DE_OK);
    render_frame(f);
    u8 *original = readback(f);
    ASSERT_TRUE(stbi_write_png(DE_TEXT_RENDER_CAPTURE ".effects-original.png", 1280, 720, 4,
                               original, 1280 * 4));
    free(original);
    for (u32 direction = 0; direction < 2; ++direction) {
        options.shadow_offset = direction ? (de_vec2){-7, -9} : (de_vec2){7, 9};
        u8 *reference = NULL;
        for (u32 poison = 0; poison < 2; ++poison) {
            /* Keep every texel of this glyph, including MSDF padding, identical. Only
             * neighboring allocations change. The resulting image must be invariant. */
            u8 *atlas = font->pixels[glyph.page];
            for (u32 y = 0; y < font->height; ++y)
                for (u32 x = 0; x < font->width; ++x)
                    if (x < glyph.x || x >= glyph.x + glyph.w || y < glyph.y ||
                        y >= glyph.y + glyph.h)
                        memset(atlas + 4 * ((u64)y * font->width + x), poison ? 255 : 0, 4);
            de_text__font_dirty(font,
                                (de_text_dirty_rect){glyph.page, 0, 0, font->width, font->height});
            de_text_update(f->fonts.t);
            ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &q, 1, (de_vec2){0}, &options), DE_OK);
            render_frame(f);
            u8 *pixels = readback(f);
            if (!poison) {
                reference = pixels;
                continue;
            }
            u32 changed = 0;
            for (u32 i = 0; i < 1280 * 720; ++i)
                changed += memcmp(reference + i * 4, pixels + i * 4, 4) != 0;
            printf("MSDF shadow direction=%u changed neighboring-atlas pixels=%u\n", direction,
                   changed);
            if (!direction)
                ASSERT_TRUE(stbi_write_png(DE_TEXT_RENDER_CAPTURE ".effects-poison.png", 1280, 720,
                                           4, pixels, 1280 * 4));
            free(reference);
            free(pixels);
            EXPECT_EQ(changed, 0u);
        }
    }
    de__view_unregister_stage(f->r, coverage_clear_stage, NULL);
#else
    SKIP("Metal native host required");
#endif
}
TEST_F(text_batch, metal_cff_and_truetype_msdf_coverage_small_and_large, text_render_fx) {
#if defined(__APPLE__)
    start_render(f, DE_GPU_METAL);
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_OUTPUT, DE__VIEW_ORDER_UI,
                                      "coverage.background", coverage_clear_stage, NULL),
              DE_OK);
    const char *ids[] = {"font/cff-msdf", "font/tt-msdf"};
    de_font fonts[2];
    de_load_handle loads[2];
    for (u32 i = 0; i < 2; ++i) {
        de_asset_id id = de_asset_id_from(ids[i]);
        fonts[i] = de_asset_font(f->fonts.assets, id);
        loads[i] = de_asset_load(f->fonts.assets, id, 0);
        ASSERT_EQ(de_asset_wait(f->fonts.assets, loads[i], TEXT_WAIT), (int)DE_LOAD_READY);
    }
    const char *letters = "BOag";
    const u32 sizes[] = {24, 192};
    de_glyph_quad quads[16];
    de_text_update(f->fonts.t);
    for (u32 font = 0; font < 2; ++font)
        for (u32 size = 0; size < 2; ++size)
            for (u32 c = 0; c < 4; ++c) {
                de_glyph_quad q = quad(f, fonts[font], letters[c], 32 + c * (size ? 160.f : 96.f),
                                       32 + font * 320.f + size * 80.f, (f32)sizes[size]);
                q.font = fonts[font];
                q.color = (de_vec4){0, 1, 0, 1};
                quads[font * 8 + size * 4 + c] = q;
            }
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, quads, 16, (de_vec2){0},
                           &(de_text_draw_opts){.font = fonts[0]}),
              DE_OK);
    render_frame(f);
    u8 *image = readback(f);
    for (u32 i = 0; i < 16; ++i) {
        u32 font_index = i / 8, size = sizes[(i / 4) % 2];
        de_text_font *font = de_text__font_get(f->fonts.t, fonts[font_index]);
        stbtt_fontinfo reference_font;
        ASSERT_TRUE(stbtt_InitFont(&reference_font, font->source, 0));
        int glyph = stbtt_FindGlyphIndex(&reference_font, letters[i % 4]);
        de_text_glyph_entry entry;
        ASSERT_TRUE(de_text__glyph(f->fonts.t, fonts[font_index], (u32)glyph, &entry));
        de_vec4 rect = quads[i].pos;
        f32 baseline_x = rect.x - entry.plane.x * size, baseline_y = rect.y + entry.plane.w * size,
            scale = stbtt_ScaleForMappingEmToPixels(&reference_font, (f32)size);
        int width, height, xoff, yoff;
        u8 *reference = stbtt_GetGlyphBitmapSubpixel(
            &reference_font, scale, scale, baseline_x - floorf(baseline_x),
            baseline_y - floorf(baseline_y), glyph, &width, &height, &xoff, &yoff);
        ASSERT_TRUE(reference);
        int rx = (int)floorf(baseline_x) + xoff, ry = (int)floorf(baseline_y) + yoff;
        u32 intersection = 0, combined = 0;
        for (int y = (int)floorf(rect.y); y < (int)ceilf(rect.y + rect.w); ++y)
            for (int x = (int)floorf(rect.x); x < (int)ceilf(rect.x + rect.z); ++x) {
                const u8 *pixel = image + 4 * ((u32)y * 1280 + (u32)x);
                bool expected = x >= rx && x < rx + width && y >= ry && y < ry + height &&
                                reference[(y - ry) * width + x - rx] >= 128;
                bool actual = pixel[1] >= 128 && pixel[0] < 32 && pixel[2] < 32;
                intersection += expected && actual;
                combined += expected || actual;
            }
        ASSERT_GT(combined, 0u);
        f32 iou = (f32)intersection / combined;
        printf("Metal %s %c at%upx coverage IoU %.5f\n", ids[font_index], letters[i % 4], size,
               (double)iou);
        EXPECT_TRUE(iou >= (size == 24 ? .80f : .95f));
        stbtt_FreeBitmap(reference, NULL);
    }
    ASSERT_TRUE(stbi_write_png(DE_TEXT_RENDER_CAPTURE, 1280, 720, 4, image, 1280 * 4));
    free(image);
    de__view_unregister_stage(f->r, coverage_clear_stage, NULL);
    for (u32 i = 0; i < 2; ++i)
        de_asset_release(f->fonts.assets, loads[i]);
#else
    SKIP("Metal native host required");
#endif
}
TEST_F(text_batch, metal_bitmap_pixel_exact_1x_3x, text_render_fx) {
#if defined(__APPLE__)
    start_render(f, DE_GPU_METAL);
    de_text_font *font = de_text__font_get(f->fonts.t, f->bitmap);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            u8 *p = font->pixels[0] + 4 * (y * font->width + x);
            p[0] = p[1] = p[2] = 255;
            p[3] = ((x + y) & 1) ? 0 : 255;
        }
    de_text_update(f->fonts.t);
    de_glyph_quad a = quad(f, f->bitmap, 'A', 16, 16, 8), b = quad(f, f->bitmap, 'A', 48, 16, 24);
    de_text_draw_opts opts = {.font = f->bitmap, .pixel_snap = true};
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &a, 1, (de_vec2){0}, &opts), DE_OK);
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &b, 1, (de_vec2){0}, &opts), DE_OK);
    de_glyph_quad clipped = quad(f, f->bitmap, 'A', 90, 16, 8);
    opts.clip_px = (de_vec4){90, 16, 4, 8};
    ASSERT_EQ(de_text_draw(f->fonts.t, (de_view){0}, &clipped, 1, (de_vec2){0}, &opts), DE_OK);
    render_frame(f);
    u8 *image = readback(f);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            bool filled = x < 4 && ((x + y) & 1) == 0;
            const u8 *p = image + 4 * ((y + 16) * 1280 + x + 90);
            for (u32 channel = 0; channel < 3; ++channel)
                EXPECT_EQ(p[channel], filled ? 255 : image[channel]);
        }
    for (u32 size = 1; size <= 3; size += 2) {
        u32 ox = size == 1 ? 16 : 48;
        for (u32 y = 0; y < 8 * size; ++y)
            for (u32 x = 0; x < 8 * size; ++x) {
                bool filled = ((x / size + y / size) & 1) == 0;
                const u8 *p = image + 4 * ((y + 16) * 1280 + x + ox);
                for (u32 channel = 0; channel < 3; ++channel)
                    EXPECT_EQ(p[channel], filled ? 255 : image[channel]);
            }
    }
    free(image);
#else
    SKIP("Metal native host required");
#endif
}
#endif
