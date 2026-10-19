#include "ui_test_util.h"
#include "ui/render/draw_list.h"
#include "ui/binding/binding_internal.h"
#include "asset/hot_reload_internal.h"
#include "gpu/gpu_backend.h"
#include "render/loaders/render_assets.h"
#include "de_alloc_guard.h"
#include <stdio.h>
static DeUiStyle *style(ui_fx *f, de_entity entity, de_vec4 color, f32 opacity) {
    DeUiStyle *s = de_entity_add(f->world, entity, f->ui->ids.style);
    *s = (DeUiStyle){.background = color, .opacity = opacity, .text_color = {1, 1, 1, 1}};
    return s;
}
static void dimensions(ui_fx *f, de_entity e, f32 w, f32 h) {
    DeUiNode *n = ui_node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, w};
    n->height = (de_ui_len){DE_UI_PX, h};
    n->shrink = 0;
}
static struct de_ui_render *render(ui_fx *f) {
    return f->ui->render;
}
TEST_F(ui_draw, command_generation_opacity_sort_popup, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    style(f, root, (de_vec4){.1f, .1f, .1f, 1}, .5f);
    de_entity a = ui_node(f, root), b = ui_node(f, root), popup = ui_node(f, root);
    dimensions(f, a, 20, 20);
    dimensions(f, b, 20, 20);
    dimensions(f, popup, 20, 20);
    style(f, a, (de_vec4){1, 0, 0, 1}, .5f);
    style(f, b, (de_vec4){0, 1, 0, 1}, 1);
    style(f, popup, (de_vec4){0, 0, 1, 1}, 1);
    ui_node_edit(f, a)->z_order = 10;
    ui_node_edit(f, b)->z_order = -1;
    ui_node_edit(f, popup)->z_order = -100;
    DeUiPopup *p = de_entity_add(f->world, popup, f->ui->ids.popup);
    *p = (DeUiPopup){.anchor = a, .placement = DE_UI_POPUP_OVERLAY};
    ui_frame(f);
    struct de_ui_render *r = render(f);
    ASSERT_EQ(r->build.count, 4u);
    EXPECT_FLOAT_EQ(r->build.commands[0].gpu.color.y, 1, 0); /* child z=-1 precedes root z=0 */
    EXPECT_FLOAT_EQ(r->build.commands[2].gpu.color.x, 1, 0);
    EXPECT_FLOAT_EQ(r->build.commands[2].gpu.color.w, .25f, 0);
    EXPECT_FLOAT_EQ(r->build.commands[3].gpu.color.z, 1, 0); /* popup wins despite low z */
}
TEST(ui_draw, nine_slice_three_sizes_and_small_destination) {
    de_ui_gpu_quad quads[9];
    for (u32 width = 100; width <= 300; width += 100) {
        ASSERT_EQ(de_ui__nine_slice((de_vec4){10, 20, (f32)width, 80}, (de_vec4){0, 0, 1, 1},
                                    (de_vec4){8, 6, 8, 6}, 32, 24, 2, quads),
                  9u);
        expect_rect(quads[0].rect, 10, 20, 16, 12);
        expect_rect(quads[8].rect, (f32)width - 6, 88, 16, 12);
        EXPECT_FLOAT_EQ(quads[4].uv.x, .25f, 1e-6f);
        EXPECT_FLOAT_EQ(quads[4].uv.y, .25f, 1e-6f);
        EXPECT_FLOAT_EQ(quads[4].uv.z, .75f, 1e-6f);
        EXPECT_FLOAT_EQ(quads[4].uv.w, .75f, 1e-6f);
    }
    u32 count = de_ui__nine_slice((de_vec4){0, 0, 8, 6}, (de_vec4){0, 0, 1, 1},
                                  (de_vec4){8, 6, 8, 6}, 32, 24, 1, quads);
    for (u32 i = 0; i < count; ++i) {
        EXPECT_TRUE(quads[i].rect.x + quads[i].rect.z <= 8);
        EXPECT_TRUE(quads[i].rect.y + quads[i].rect.w <= 6);
    }
    EXPECT_EQ(de_ui__nine_slice((de_vec4){0, 0, 10, 10}, (de_vec4){0, 0, 1, 1},
                                (de_vec4){1, 1, 1, 1}, 0, 24, 1, quads),
              0u);
}
TEST_F(ui_draw, nested_clip_intersection, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), parent = ui_node(f, root),
              child = ui_node(f, parent);
    dimensions(f, parent, 100, 50);
    ui_node_edit(f, parent)->clip = true;
    dimensions(f, child, 200, 80);
    style(f, child, (de_vec4){1, 1, 1, 1}, 1);
    ui_frame(f);
    struct de_ui_render *r = render(f);
    ASSERT_EQ(r->build.count, 1u);
    expect_rect(r->build.commands[0].clip, 0, 0, 100, 50);
    expect_rect(de_ui__clip_intersect((de_vec4){0, 0, 10, 10}, (de_vec4){20, 20, 10, 10}), 20, 20,
                0, 0);
}
TEST_F(ui_draw, overflow_truncates_without_overwrite, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 8; ++i) {
        de_entity e = ui_node(f, root);
        dimensions(f, e, 10, 10);
        style(f, e, (de_vec4){1, 1, 1, 1}, 1);
    }
    de_cvar max = render(f)->max_quads;
    ASSERT_EQ(de_cvar_set(max, "2"), DE_OK);
    ui_frame(f);
    EXPECT_EQ(render(f)->build.count, 2u);
    EXPECT_EQ(de_ui_get_render_stats(f->ui).truncated, 6u);
    ASSERT_EQ(de_cvar_set(max, "65536"), DE_OK);
}
TEST_F(ui_draw, extract_copy_preserves_previous_frame, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    style(f, root, (de_vec4){1, 0, 0, 1}, 1);
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    struct de_ui_render *r = render(f);
    u32 first = r->published;
    u64 token = r->snapshots[first].generation;
    style(f, root, (de_vec4){0, 1, 0, 1}, 1);
    ui_frame(f);
    EXPECT_FLOAT_EQ(r->snapshots[first].commands[0].gpu.color.x, 1, 0);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    EXPECT_TRUE(first != r->published);
    EXPECT_TRUE(token != r->snapshots[r->published].generation);
    EXPECT_FLOAT_EQ(r->snapshots[first].commands[0].gpu.color.x, 1, 0);
    EXPECT_FLOAT_EQ(r->snapshots[r->published].commands[0].gpu.color.y, 1, 0);
}
TEST_F(ui_draw, two_thousand_widgets_static_zero_alloc_build_extract, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 2000; ++i) {
        de_entity e = ui_node(f, root);
        dimensions(f, e, 4, 4);
        style(f, e, (de_vec4){1, 1, 1, 1}, 1);
    }
    for (u32 i = 0; i < 8; ++i) {
        ui_frame(f);
        ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    }
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    f64 total = 0;
    const u64 frame_start = de_time_now_ns();
    for (u32 i = 0; i < 1000; ++i) {
        ui_frame(f);
        ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
        total += de_ui_get_render_stats(f->ui).build_extract_ms;
    }
    const f64 whole_frame_mean_ms = (f64)(de_time_now_ns() - frame_start) / 1e9;
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    printf("2000-widget build+Extract mean %.4fms, quads%u\n", total / 1000,
           de_ui_get_render_stats(f->ui).quads);
    printf("2000-widget whole UI frame+Extract mean %.4fms (1000 frames)\n", whole_frame_mean_ms);
    EXPECT_TRUE(whole_frame_mean_ms <= 2.5);
    EXPECT_EQ(de_ui_get_render_stats(f->ui).quads, 2000u);
    EXPECT_TRUE(total / 1000 <= .5);
}
#if DE_HAS_RENDER
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
static void draw_renderer(ui_fx *f, de_gpu_backend backend) {
    de_fp_env_init_thread();
    de_ui_destroy(f->ui);
    f->ui = NULL;
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = backend, .pipeline_cache_dir = ""}, &f->device),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device,
                                                     .assets = f->assets,
                                                     .types = f->types,
                                                     .frames_in_flight = 2},
                                 &f->renderer),
              DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    ASSERT_EQ(
        de_ui_create(
            &(de_ui_desc){.renderer = f->renderer, .reference_height = 512, .max_nodes = 6000},
            &f->ui),
        DE_OK);
    de_ui_set_viewport(f->ui, 512, 512, (de_window_insets){0});
    de_ui_attach(f->ui, f->world);
}
static void draw_kick(ui_fx *f) {
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
}
TEST_F(ui_draw, no_camera_twelve_frames_retire_unsubmitted_packets, ui_fx) {
    draw_renderer(f, DE_GPU_NULL);
    de_entity root = ui_node(f, (de_entity){0});
    style(f, root, (de_vec4){1, 1, 1, 1}, 1);
    for (u32 i = 0; i < 12; ++i) {
        draw_kick(f);
        struct de_ui_render *r = render(f);
        EXPECT_EQ(r->snapshots[r->published].generation, (u64)i + 1);
        EXPECT_EQ(r->snapshots[r->published].last_gpu_fence, 0u);
        EXPECT_EQ(de_renderer_completed_extract_frame(f->renderer), (u64)i + 1);
        EXPECT_EQ(de_ui_get_render_stats(f->ui).truncated, 0u);
    }
}
#if DE_HAS_GPU_METAL
static de_gpu_texture floating_view(ui_fx *f, de_gpu_buffer *readback, de_view *view) {
    de_gpu_texture target = de_gpu_texture_create(
        f->device, &(de_gpu_texture_desc){.width = 512,
                                          .height = 512,
                                          .format = DE_FMT_RGBA32_FLOAT,
                                          .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                   DE_USAGE_COPY_SRC});
    ASSERT_TRUE(target.id);
    *readback = de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 512u * 512u * 16u,
                                                                      .usage = DE_USAGE_COPY_DST,
                                                                      .memory = DE_MEM_READBACK});
    ASSERT_TRUE(readback->id);
    *view = de_view_create(
        f->renderer, &(de_view_desc){.target = target, .name = "UI linear nits qualification"});
    ASSERT_TRUE(view->id);
    de_builtin_component_ids bi = de_builtin_components(f->world);
    de_component camera = de_component_find(f->world, "DeCamera");
    de_component components[] = {bi.transform, camera};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &e), DE_OK);
    *(DeTransform *)de_entity_get(f->world, e, bi.transform) = de_transform_identity();
    *(DeCamera *)de_entity_get(f->world, e, camera) = (DeCamera){
        .view = *view, .projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 2, .near_z = .1f};
    return target;
}
static de_vec4 read_float_pixel(ui_fx *f, de_gpu_texture target, de_gpu_buffer buffer, u32 x,
                                u32 y) {
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "UI float readback");
    ASSERT_TRUE(cmd);
    de_gpu_barrier b = {
        .texture = target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, 0, 512 * 16, target, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->device, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->device);
    const de_vec4 *p = de_gpu_buffer_map(f->device, buffer);
    ASSERT_TRUE(p);
    return p[y * 512 + x];
}
TEST_F(ui_draw, metal_linear_ui_nits_alpha_and_two_thousand_widget_batch, ui_fx) {
    draw_renderer(f, DE_GPU_METAL);
    de_gpu_buffer readback;
    de_view view;
    de_gpu_texture target = floating_view(f, &readback, &view);
    de_entity root = ui_node(f, (de_entity){0});
    DeUiStyle *s = style(f, root, (de_vec4){0, 0, 0, 0}, 1);
    draw_kick(f);
    de_vec4 background = read_float_pixel(f, target, readback, 256, 256);
    const f32 nits[] = {80, 203, 400};
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_EQ(
            de_renderer_set_ui_output(f->renderer, &(de_ui_output_calibration){true, nits[i], 203}),
            DE_OK);
        s = de_entity_get(f->world, root, f->ui->ids.style);
        s->background = (de_vec4){1, 1, 1, 1};
        draw_kick(f);
        de_vec4 p = read_float_pixel(f, target, readback, 256, 256);
        EXPECT_FLOAT_EQ(p.x * 203, nits[i], .5f);
        EXPECT_FLOAT_EQ(p.y, p.x, .002f);
        EXPECT_FLOAT_EQ(p.z, p.x, .002f);
        s = de_entity_get(f->world, root, f->ui->ids.style);
        s->background.w = .5f;
        draw_kick(f);
        de_vec4 alpha = read_float_pixel(f, target, readback, 256, 256);
        EXPECT_FLOAT_EQ(alpha.x, .5f * nits[i] / 203 + .5f * background.x, .003f);
        printf("UIlinearwhite %.0fnits: opaque%g halfalpha%g (paperwhite203); not PQ/scRGB display "
               "qualification\n",
               (double)nits[i], (double)p.x, (double)alpha.x);
    }
    ASSERT_EQ(de_renderer_set_ui_output(f->renderer,
                                        &(de_ui_output_calibration){.hdr = false,
                                                                    .ui_white_nits = 203,
                                                                    .paper_white_nits = 203}),
              DE_OK);
    DeUiGrid *grid = de_entity_add(f->world, root, f->ui->ids.grid);
    grid->column_count = 1;
    grid->columns[0] = (de_ui_len){DE_UI_FR, 1};
    ui_node_edit(f, root)->display =
        DE_UI_GRID; /* Visible fixed-position children use explicit anchors. */
    for (u32 i = 0; i < 2000; ++i) {
        de_entity e = ui_node(f, root);
        DeUiAnchor *a = de_entity_add(f->world, e, f->ui->ids.anchor);
        f32 x = (f32)(i % 50) * 8, y = (f32)(i / 50) * 8;
        *a = (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + 8, y + 8}};
        style(f, e, (de_vec4){.2f, .4f, .8f, 1}, 1);
    }
    draw_kick(f);
    de_gpu_wait_idle(f->device);
    de_ui_render_stats stats = de_ui_get_render_stats(f->ui);
    printf("actualMetal2000-widget HUD: quads%u draws%u build+Extract%.4fms\n", stats.quads,
           stats.draw_calls, (double)stats.build_extract_ms);
    EXPECT_EQ(stats.quads, 2001u);
    EXPECT_TRUE(stats.draw_calls > 0 && stats.draw_calls <= 4);
    de_gpu_buffer_destroy(f->device, readback);
    de_gpu_texture_destroy(f->device, target);
}
TEST_F(ui_draw, metal_exact_extract_token_ignores_future_published_packet, ui_fx) {
    draw_renderer(f, DE_GPU_METAL);
    de_gpu_buffer readback;
    de_view view;
    de_gpu_texture target = floating_view(f, &readback, &view);
    de_entity root = ui_node(f, (de_entity){0});
    style(f, root, (de_vec4){1, 0, 0, 1}, 1);
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    struct de_ui_render *r = render(f);
    u32 old = r->published;
    u64 token = r->snapshots[old].generation;
    style(f, root, (de_vec4){0, 1, 0, 1}, 1);
    ui_frame(f);
    /* Interleave a deliberately future published UI packet before actual worldN renders.
     * A latest-published implementation would draw green; the immutable world token selects red. */
    u32 future = (old + 1) % DE_UI_RENDER_SNAPSHOTS;
    de_ui_draw_list *p = &r->snapshots[future];
    memcpy(p->commands, r->build.commands, (size_t)r->build.count * sizeof *p->commands);
    memcpy(p->roots, r->build.roots, (size_t)r->build.root_count * sizeof *p->roots);
    p->count = r->build.count;
    p->root_count = r->build.root_count;
    p->generation = token + 1;
    p->valid = true;
    r->published = future;
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_vec4 color = read_float_pixel(f, target, readback, 256, 256);
    EXPECT_FLOAT_EQ(color.x, 1, .001f);
    EXPECT_FLOAT_EQ(color.y, 0, .001f);
    EXPECT_EQ(r->snapshots[old].last_gpu_fence, 1u);
    EXPECT_EQ(p->last_gpu_fence, 0u);
    de_gpu_buffer_destroy(f->device, readback);
    de_gpu_texture_destroy(f->device, target);
}
#if defined(DE_UI_DRAW_ASSET_ROOT)
TEST_F(ui_draw, metal_unsent_image_packet_keeps_same_slot_reload_generation_alive, ui_fx) {
#if DE__ASSET_RELOAD_ENABLED
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.types = f->types, .hot_reload = true},
                                     &f->assets),
              DE_OK);
    de__asset_reload_set_watching(f->assets, false);
    draw_renderer(f, DE_GPU_METAL);
    ASSERT_EQ(de_fs_mount("uireload", DE_UI_DRAW_ASSET_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "uireload:/tests/unit/ui/assets.toml"), DE_OK);
    de_asset_id id = de_asset_id_from("ui/checker");
    de_texture image = de_asset_texture(f->assets, id);
    f->load = de_asset_load(f->assets, id, 0);
    u64 end = de_time_now_ns() + 30000000000ULL;
    while (de_asset_status(f->assets, f->load, NULL) != DE_LOAD_READY && de_time_now_ns() < end) {
        de_asset_system_update(f->assets, NULL);
        de_render_uploader_flush(f->renderer->uploader);
        de_render_uploader_acquire_submit(f->renderer->uploader);
        de_sleep_ns(1000000);
    }
    ASSERT_EQ(de_asset_status(f->assets, f->load, NULL), DE_LOAD_READY);
    de_gpu_buffer readback;
    de_view view;
    de_gpu_texture target = floating_view(f, &readback, &view);
    de_entity e = ui_node(f, (de_entity){0});
    *(DeUiImage *)de_entity_add(f->world, e, f->ui->ids.image) =
        (DeUiImage){.texture = image, .uv_rect = {0, 0, 1, 1}, .tint = {1, 1, 1, 1}};
    de_gpu_texture old = de_render_texture_gpu(f->renderer, image);
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    struct de_ui_render *r = render(f);
    de_ui_draw_list *packet = &r->snapshots[r->published];
    ASSERT_EQ(packet->count, 1u);
    EXPECT_EQ(packet->last_gpu_fence, 0u);
    EXPECT_EQ(packet->commands[0].texture_lease.id, old.id);
    u32 descriptor = packet->commands[0].gpu.texture;
    de__asset_reload_on_change(f->assets, "uireload:/tests/data/fonts/latin/pixel.png",
                               DE_WATCH_MODIFIED, false);
    end = de_time_now_ns() + 30000000000ULL;
    while (de_render_texture_gpu(f->renderer, image).id == old.id && de_time_now_ns() < end) {
        de_asset_system_update(f->assets, NULL);
        de_render_uploader_flush(f->renderer->uploader);
        de_render_uploader_acquire_submit(f->renderer->uploader);
        de_sleep_ns(1000000);
    }
    ASSERT_NE(de_render_texture_gpu(f->renderer, image).id, old.id);
    for (u32 i = 0; i < 8; ++i) {
        de_asset_system_update(f->assets, NULL);
        de_render_uploader_flush(f->renderer->uploader);
    }
    de_gpu_wait_idle(f->device);
    ASSERT_TRUE(de__gpu_get_texture(f->device, old) != NULL);
    EXPECT_TRUE(de__gpu_get_texture(f->device, old)->destroy_requested);
    EXPECT_EQ(de_gpu_texture_bindless(f->device, old, 0, false), descriptor);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    (void)read_float_pixel(f, target, readback, 0, 0);
    const de_vec4 *pixels = de_gpu_buffer_map(f->device, readback);
    u32 visible = 0;
    for (u32 i = 0; i < 512 * 512; ++i)
        visible += pixels[i].x > .2f || pixels[i].y > .2f || pixels[i].z > .2f;
    EXPECT_GT(visible, 100u);
    printf("Unsent hot-reload packet rendered%u pixels from its leased generation\n", visible);
    de_ui__packet_unpin(r, packet);
    EXPECT_TRUE(de__gpu_get_texture(f->device, old) == NULL);
    de_gpu_wait_idle(f->device);
    de_gpu_buffer_destroy(f->device, readback);
    de_gpu_texture_destroy(f->device, target);
    ASSERT_EQ(de_fs_unmount("uireload", DE_UI_DRAW_ASSET_ROOT), DE_OK);
#else
    SKIP("hot reload is a Debug/Dev feature");
#endif
}
TEST_F(ui_draw, metal_bound_text_tint_changes_actual_glyph_pixels_with_default_sheet, ui_fx) {
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.types = f->types}, &f->assets),
              DE_OK);
    de_text_system *text = NULL;
    ASSERT_EQ(de_text_create(NULL, f->assets, &text), DE_OK);
    draw_renderer(f, DE_GPU_METAL);
    ASSERT_EQ(de_text_attach(text, f->renderer, f->world), DE_OK);
    f->ui->desc.text = text;
    ASSERT_EQ(de_fs_mount("uitint", DE_UI_DRAW_ASSET_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "uitint:/tests/golden/ui/assets.toml"), DE_OK);
    de_asset_id id = de_asset_id_from("ui/font");
    de_font font = de_asset_font(f->assets, id);
    f->load = de_asset_load(f->assets, id, 0);
    u64 deadline = de_time_now_ns() + 30000000000ULL;
    while (de_asset_status(f->assets, f->load, NULL) != DE_LOAD_READY &&
           de_time_now_ns() < deadline) {
        de_asset_system_update(f->assets, NULL);
        de_text_update(text);
        de__renderer_kick_prepare(f->renderer);
        de_sleep_ns(1000000);
    }
    ASSERT_EQ(de_asset_status(f->assets, f->load, NULL), DE_LOAD_READY);
    typedef struct UiTintSource {
        de_vec4 color;
    } UiTintSource;
    const de_field_info field = DE_FIELD(UiTintSource, color, DE_FK_COLOR, DE_FIELD_DEFAULT);
    de_type_id type;
    ASSERT_EQ(de_type_register_ex(f->types,
                                  &(de_type_desc){.name = "UiTintSource",
                                                  .size = sizeof(UiTintSource),
                                                  .align = _Alignof(UiTintSource),
                                                  .version = 1,
                                                  .is_pod = true,
                                                  .fields = &field,
                                                  .field_count = 1},
                                  &type, NULL),
              DE_OK);
    de_component source_component = de_component_register(f->world, type, 0);
    de_entity source = de_entity_spawn(f->world);
    ASSERT_TRUE(de_entity_add(f->world, source, source_component));
    de_gpu_buffer readback;
    de_view view;
    de_gpu_texture target = floating_view(f, &readback, &view);
    de_entity e = ui_node(f, (de_entity){0});
    DeUiText *label = de_entity_add(f->world, e, f->ui->ids.text);
    label->style = (de_text_style){.font = font, .size_px = 64, .color = {1, 1, 1, 1}};
    strcpy(label->text, "MMMM");
    DeUiBinding *binding = de_entity_add(f->world, e, f->ui->binding->component);
    binding->source = source;
    strcpy(binding->path, "UiTintSource.color");
    strcpy(binding->property, "tint");
    for (u32 phase = 0; phase < 2; ++phase) {
        ((UiTintSource *)de_entity_get(f->world, source, source_component))->color =
            phase ? (de_vec4){0, 1, 0, 1} : (de_vec4){1, 0, 0, 1};
        for (u32 frame = 0; frame < 3; ++frame) {
            de_asset_system_update(f->assets, NULL);
            de_text_update(text);
            draw_kick(f);
        }
        (void)read_float_pixel(f, target, readback, 0, 0);
        const de_vec4 *pixels = de_gpu_buffer_map(f->device, readback);
        ASSERT_TRUE(pixels);
        u32 red = 0, green = 0;
        for (u32 y = 0; y < 128; ++y)
            for (u32 x = 0; x < 256; ++x) {
                de_vec4 p = pixels[y * 512 + x];
                red += p.x > .2f && p.y < .05f && p.z < .05f;
                green += p.y > .2f && p.x < .05f && p.z < .05f;
            }
        printf("Metal bound text phase%u: red%u green%u glyph pixels\n", phase, red, green);
        EXPECT_GT(phase ? green : red, 100u);
        EXPECT_EQ(phase ? red : green, 0u);
        EXPECT_GT(de_ui_get_render_stats(f->ui).draw_calls, 0u);
    }
    de_gpu_buffer_destroy(f->device, readback);
    de_gpu_texture_destroy(f->device, target);
    de_ui_destroy(f->ui);
    f->ui = NULL;
    de_text_destroy(text);
    ASSERT_EQ(de_fs_unmount("uitint", DE_UI_DRAW_ASSET_ROOT), DE_OK);
}
#endif
#endif
#endif
#if DE_HAS_RENDER && defined(DE_UI_DRAW_ASSET_ROOT)
#include "asset/slots_internal.h"
TEST_F(ui_draw, image_pin_survives_load_release_and_despawn_until_packet_retirement, ui_fx) {
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.types = f->types}, &f->assets),
              DE_OK);
    draw_renderer(f, DE_GPU_NULL);
    ASSERT_EQ(de_fs_mount("uidrawassets", DE_UI_DRAW_ASSET_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "uidrawassets:/tests/unit/ui/assets.toml"), DE_OK);
    de_asset_id id = de_asset_id_from("ui/checker");
    de_texture texture = de_asset_texture(f->assets, id);
    f->load = de_asset_load(f->assets, id, 0);
    u64 until = de_time_now_ns() + 30000000000ULL;
    while (de_asset_status(f->assets, f->load, NULL) != DE_LOAD_READY && de_time_now_ns() < until) {
        de_asset_system_update(f->assets, NULL);
        de__renderer_kick_prepare(f->renderer);
        de_sleep_ns(1000000);
    }
    ASSERT_EQ(de_asset_status(f->assets, f->load, NULL), DE_LOAD_READY);
    de_entity root = ui_node(f, (de_entity){0});
    DeUiImage *image = de_entity_add(f->world, root, f->ui->ids.image);
    *image = (DeUiImage){.texture = texture, .uv_rect = {0, 0, 1, 1}, .tint = {1, 1, 1, 1}};
    de_component camera = de_component_find(f->world, "DeCamera");
    de_builtin_component_ids bi = de_builtin_components(f->world);
    de_component components[] = {camera, bi.transform};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &e), DE_OK);
    *(DeTransform *)de_entity_get(f->world, e, bi.transform) = de_transform_identity();
    *(DeCamera *)de_entity_get(f->world, e, camera) =
        (DeCamera){.projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 2, .near_z = .1f};
    draw_kick(f);
    struct de_ui_render *r = render(f);
    u32 packet = r->published;
    ASSERT_TRUE(r->snapshots[packet].commands[0].pins & 1u);
    ASSERT_TRUE(r->snapshots[packet].last_gpu_fence > 0);
    de_asset_release(f->assets, f->load);
    f->load = (de_load_handle){0};
    de_asset_flush_unused(f->assets);
    EXPECT_TRUE(de_render_texture_gpu(f->renderer, texture).id);
    de_entity_despawn_recursive(f->world, root);
    /* The authored entity is gone, but the submitted packet still owns its generation. */
    u32 handles = 0;
    de__slot_refs(de__slot_get(f->assets, texture.id), &handles, NULL);
    EXPECT_TRUE(handles > 0);
    de_asset_flush_unused(f->assets);
    EXPECT_TRUE(de_render_texture_gpu(f->renderer, texture).id);
    for (u32 i = 0; i < 6; ++i) {
        draw_kick(f);
        de_gpu_wait_idle(f->device);
    }
    de__slot_refs(de__slot_get(f->assets, texture.id), &handles, NULL);
    EXPECT_EQ(handles, 0u);
    de_asset_flush_unused(f->assets);
    EXPECT_EQ(de_render_texture_gpu(f->renderer, texture).id, 0u);
    ASSERT_EQ(de_fs_unmount("uidrawassets", DE_UI_DRAW_ASSET_ROOT), DE_OK);
}
#endif

static void geometry_override(de_ui *ui, de_entity entity, de_vec2 *offset, f32 *width,
                              void *user) {
    (void)ui;
    EXPECT_EQ(entity.bits, 123u);
    EXPECT_FLOAT_EQ(*width, 80, 0);
    ++*(u32 *)user;
    *offset = (de_vec2){-17, 4};
    *width = 0; /* A single line does not wrap to its visual clip. */
}
TEST_F(ui_draw, optional_text_geometry_preserves_defaults_and_clip, ui_fx) {
    de_vec2 offset;
    f32 width;
    de_ui__text_geometry(f->ui, (de_entity){123}, 80, &offset, &width);
    EXPECT_FLOAT_EQ(offset.x, 0, 0);
    EXPECT_FLOAT_EQ(width, 80, 0);
    u32 calls = 0;
    de_ui__set_text_geometry(f->ui, geometry_override, &calls);
    de_entity root = ui_node(f, (de_entity){0});
    dimensions(f, root, 80, 20);
    ui_node_edit(f, root)->clip = true;
    style(f, root, (de_vec4){1, 1, 1, 1}, 1);
    ui_frame(f);
    de_vec4 clip = render(f)->build.commands[0].clip;
    de_ui__text_geometry(f->ui, (de_entity){123}, 80, &offset, &width);
    EXPECT_EQ(calls, 1u);
    EXPECT_FLOAT_EQ(offset.x, -17, 0);
    EXPECT_FLOAT_EQ(offset.y, 4, 0);
    EXPECT_FLOAT_EQ(width, 0, 0);
    EXPECT_TRUE(memcmp(&clip, &render(f)->build.commands[0].clip, sizeof clip) == 0);
    de_ui__set_text_geometry(f->ui, NULL, NULL);
    de_ui__text_geometry(f->ui, (de_entity){123}, 80, &offset, &width);
    EXPECT_EQ(calls, 1u);
    EXPECT_FLOAT_EQ(offset.x, 0, 0);
    EXPECT_FLOAT_EQ(width, 80, 0);
}
