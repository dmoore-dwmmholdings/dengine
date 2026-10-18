#include "de_test.h"
#include "render/pixel/pixel_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include <dengine/de_reflect.h>
#include <dengine/de_ecs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
TEST(pixel, integer_scale_math) {
    de__pixel_layout a = de__pixel_layout_make(480, 270, 1920, 1080, true);
    ASSERT_TRUE(a.scale == 4 && a.x == 0 && a.y == 0);
    a = de__pixel_layout_make(480, 270, 2560, 1440, true);
    ASSERT_TRUE(a.scale == 5 && a.x == 80 && a.y == 45 && a.width == 2400 && a.height == 1350);
    a = de__pixel_layout_make(480, 270, 2560, 1440, false);
    ASSERT_TRUE(fabsf(a.width - 2560) < .001f && fabsf(a.height - 1440) < .001f);
}
TEST(pixel, camera_snap_subtexel_double) {
    const f64 texel = 1.0 / 16;
    de_vec2 residual;
    for (u32 frame = 0; frame < 20; ++frame) {
        f64 x = 1000000000.0 + frame * .3 * texel;
        de_dvec3 p = de__pixel_snap((de_dvec3){x, 0, 3}, DE_PLANE_TOP_XY, texel, &residual);
        ASSERT_TRUE(fabs(p.x / texel - round(p.x / texel)) < 1e-6);
        ASSERT_TRUE(fabs((x - p.x) / texel - residual.x) < 1e-6);
        ASSERT_TRUE(fabs((p.x + residual.x * texel) - x) < 1e-6);
    }
    de_dvec3 p = de__pixel_snap((de_dvec3){.02, 9, .08}, DE_PLANE_SIDE_XZ, .1, &residual);
    ASSERT_TRUE(p.y == 9 && fabs(p.z - .1) < 1e-12);
}
typedef struct pixel_fixture {
    de_gpu_device *d;
    de_renderer *r;
    de_type_registry *types;
    de_world *world;
    de_entity camera;
    de_view view;
    de_gpu_texture target, pattern;
    de_gpu_buffer readback;
    bool native;
    u32 iw, ih, ow, oh, internal_seen, ui_seen;
    de_vec2 residual;
    de_mat4 view_matrix;
} pixel_fixture;
static void pixel_fixture_setup(pixel_fixture *f) {
    (void)f;
}
static void pixel_fixture_teardown(pixel_fixture *f) {
    if (f->d)
        de_gpu_wait_idle(f->d);
    if (f->r)
        de_renderer_destroy(f->r);
    if (f->world)
        de_world_destroy(f->world);
    if (f->types)
        de_type_registry_destroy(f->types);
    if (f->d) {
        de_gpu_texture_destroy(f->d, f->pattern);
        de_gpu_texture_destroy(f->d, f->target);
        de_gpu_buffer_destroy(f->d, f->readback);
        de_gpu_device_destroy(f->d);
    }
    de_cvar cv = de_cvar_find("r.pipelined");
    if (cv.id)
        (void)de_cvar_set(cv, "1");
}
static void observe_internal(de__view_frame *vf, void *user) {
    pixel_fixture *f = user;
    if (vf->view.id != f->view.id)
        return;
    ASSERT_EQ(vf->width, f->iw);
    ASSERT_EQ(vf->height, f->ih);
    ++f->internal_seen;
    const de__lighting_context *lc =
        de__lighting_frame_context(de__views_lighting(f->r), vf->view, vf->graph, vf->frame_index);
    if (f->native) {
        ASSERT_TRUE(lc);
        ASSERT_EQ(lc->width, f->iw);
        ASSERT_EQ(lc->height, f->ih);
    }
    bool found = false;
    for (u32 i = 0; i < vf->graph->resource_count; ++i) {
        const de__rg_resource *res = &vf->graph->resources[i];
        if (strcmp(res->name, "pixel.internal") == 0) {
            ASSERT_EQ(res->tex.width, f->iw);
            ASSERT_EQ(res->tex.height, f->ih);
            found = true;
        }
    }
    ASSERT_TRUE(found);
    f->residual = vf->pixel_residual;
    f->view_matrix = vf->view_matrix;
}
static void pattern_stage(de__view_frame *vf, void *user) {
    pixel_fixture *f = user;
    if (vf->view.id != f->view.id || !f->pattern.id)
        return;
    vf->color = de_rg_import_texture(vf->graph, f->pattern, DE_ACCESS_SHADER_READ);
    vf->color_format = DE_FMT_RGBA8_UNORM;
}
static void observe_ui(de__view_frame *vf, void *user) {
    pixel_fixture *f = user;
    if (vf->view.id != f->view.id)
        return;
    ASSERT_EQ(vf->width, f->ow);
    ASSERT_EQ(vf->height, f->oh);
    ++f->ui_seen;
}
static void start_pixel(pixel_fixture *f, de_gpu_backend backend, u32 iw, u32 ih, u32 ow, u32 oh,
                        bool smooth) {
    f->native = backend != DE_GPU_NULL;
    if (de_gpu_device_create(&(de_gpu_device_desc){.backend = backend,
                                                   .debug_layer = backend != DE_GPU_NULL,
                                                   .pipeline_cache_dir = ""},
                             &f->d) != DE_OK)
        SKIP("native backend unavailable");
    ASSERT_EQ(de_type_registry_create(de_heap_allocator(), &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = de_heap_allocator(), .types = f->types},
                              &f->world),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->d,
                                                     .types = f->types,
                                                     .allocator = de_heap_allocator(),
                                                     .frames_in_flight = 2},
                                 &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->world);
    de_cvar cv = de_cvar_find("r.pipelined");
    if (cv.id)
        (void)de_cvar_set(cv, "0");
    f->iw = iw + (smooth ? 2 : 0);
    f->ih = ih + (smooth ? 2 : 0);
    f->ow = ow;
    f->oh = oh;
    f->target = de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.width = ow,
                                                                   .height = oh,
                                                                   .format = DE_FMT_RGBA8_UNORM,
                                                                   .usage = DE_USAGE_RENDER_TARGET |
                                                                            DE_USAGE_SAMPLED |
                                                                            DE_USAGE_COPY_SRC});
    f->view = de_view_create(f->r, &(de_view_desc){.target = f->target, .name = "pixel-test"});
    de_pixel_art_settings settings = {.internal_width = iw,
                                      .internal_height = ih,
                                      .integer_scale_only = true,
                                      .snap_camera = true,
                                      .smooth_subpixel_scroll = smooth,
                                      .letterbox_color = {.125f, .25f, .5f, 1}};
    de_view_set_pixel_art(f->r, f->view, &settings);
    de_view_set_render_scale(f->r, f->view, .33f);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    const DeRenderComponents *ids = de__render_components(f->world);
    de_component sprite_components[] = {bi.transform, ids->sprite};
    de_entity sprite;
    ASSERT_EQ(de_entity_spawn_with(f->world, sprite_components, 2, &sprite), DE_OK);
    DeTransform transform = de_transform_identity();
    transform.position.z = -5;
    *(DeTransform *)de_entity_get(f->world, sprite, bi.transform) = transform;
    *(DeSprite *)de_entity_get(f->world, sprite, ids->sprite) = (DeSprite){
        .size = {2, 2}, .pivot = {.5f, .5f}, .uv_rect = {0, 0, 1, 1}, .color = {1, 1, 1, 1}};
    de_component components[] = {bi.transform, ids->camera};
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &f->camera), DE_OK);
    *(DeTransform *)de_entity_get(f->world, f->camera, bi.transform) = de_transform_identity();
    *(DeCamera *)de_entity_get(f->world, f->camera, ids->camera) =
        (DeCamera){.view = f->view,
                   .projection = DE_PROJ_ORTHOGRAPHIC,
                   .ortho_height = (f32)ih,
                   .near_z = .1f};
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, INT32_MAX - 2,
                                      "pixel.test.internal", observe_internal, f),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, INT32_MAX - 1, "pixel.test.pattern",
                                      pattern_stage, f),
              DE_OK);
    ASSERT_EQ(
        de__view_register_stage(f->r, DE__VIEW_SLOT_OUTPUT, 1, "pixel.test.ui", observe_ui, f),
        DE_OK);
}
static void pixel_frame(pixel_fixture *f) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->world), DE_OK);
    de_gpu_wait_idle(f->d);
}
TEST_F(pixel, graph_internal_and_output_res, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 480, 270, 2560, 1440, true);
    pixel_frame(f);
    ASSERT_EQ(f->internal_seen, 1u);
    ASSERT_EQ(f->ui_seen, 1u);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = .3;
    pixel_frame(f);
    ASSERT_TRUE(fabsf(f->residual.x - .3f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12]) < 1e-6f);
    t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = .6;
    pixel_frame(f);
    ASSERT_TRUE(fabsf(f->residual.x + .4f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12] + 1) < 1e-6f);
    t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = 1000000000.3;
    pixel_frame(f);
    ASSERT_TRUE(fabsf(f->residual.x - .3f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12] - roundf(f->view_matrix.m[12])) < 1e-6f);
}
static void make_pattern(pixel_fixture *f) {
    u32 pitch = ((f->iw * 4 + 255) / 256) * 256;
    f->pattern = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = f->iw,
                                     .height = f->ih,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){
                  .size = (u64)pitch * f->ih, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *data = de_gpu_buffer_map(f->d, upload);
    for (u32 y = 0; y < f->ih; ++y)
        for (u32 x = 0; x < f->iw; ++x) {
            u8 *p = data + (size_t)y * pitch + x * 4;
            p[0] = (u8)x;
            p[1] = (u8)y;
            p[2] = ((x + y) & 1) ? 255 : 0;
            p[3] = 255;
        }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "pixel test pattern");
    de_gpu_barrier b = {.texture = f->pattern, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, f->pattern, 0, 0, upload, 0, pitch);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_buffer_destroy(f->d, upload);
}
static const u8 *read_output(pixel_fixture *f) {
    u32 pitch = f->ow * 4;
    if (!f->readback.id)
        f->readback = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = (u64)pitch * f->oh,
                                                                       .usage = DE_USAGE_COPY_DST,
                                                                       .memory = DE_MEM_READBACK});
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "pixel output readback");
    de_gpu_barrier b = {
        .texture = f->target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, 0, pitch, f->target, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->d);
    return de_gpu_buffer_map(f->d, f->readback);
}
static void check_pattern(pixel_fixture *f, u32 scale, u32 left, u32 top) {
    const u8 *data = read_output(f);
    u32 mismatches = 0;
    for (u32 y = 0; y < f->oh; ++y)
        for (u32 x = 0; x < f->ow; ++x) {
            const u8 *p = data + ((size_t)y * f->ow + x) * 4;
            if (x < left || y < top || x >= left + 480 * scale || y >= top + 270 * scale) {
                mismatches +=
                    abs((i32)p[0] - 32) > 1 || abs((i32)p[1] - 64) > 1 || abs((i32)p[2] - 128) > 1;
            } else {
                u32 ix = (x - left) / scale, iy = (y - top) / scale;
                mismatches +=
                    p[0] != (u8)ix || p[1] != (u8)iy || p[2] != (((ix + iy) & 1) ? 255 : 0);
            }
        }
    printf("Pixel upscale %ux%u: %u mismatches\n", f->ow, f->oh, mismatches);
    ASSERT_EQ(mismatches, 0u);
}
TEST_F(pixel, integer_four_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 1920, 1080, false);
    make_pattern(f);
    pixel_frame(f);
    check_pattern(f, 4, 0, 0);
}
TEST_F(pixel, integer_five_letterbox_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 2560, 1440, false);
    make_pattern(f);
    pixel_frame(f);
    check_pattern(f, 5, 80, 45);
    de_pixel_art_settings settings = {
        .internal_width = 480, .internal_height = 270, .integer_scale_only = false};
    de_view_set_pixel_art(f->r, f->view, &settings);
    pixel_frame(f);
    const u8 *data = read_output(f);
    // Fractional mode fills the viewport: an outer edge pixel comes from the pattern, not
    // letterbox.
    ASSERT_TRUE(data[0] != 32 || data[1] != 64 || data[2] != 128);
}
TEST_F(pixel, smooth_border_residual_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 1920, 1080, true);
    make_pattern(f);
    pixel_frame(f);
    const u8 *before = read_output(f);
    u8 old = before[3 * 4];
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = .3;
    pixel_frame(f);
    const u8 *after = read_output(f);
    ASSERT_TRUE(fabsf(f->residual.x - .3f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12]) < 1e-6f);
    printf("Residual .3 texel at 4x moves edge by one output pixel: %u -> %u\n", old, after[3 * 4]);
    ASSERT_EQ(old, 1u);
    ASSERT_EQ(after[3 * 4], 2u);
}

/* Retained source-review regression: fractional magnification must hold texel centers. */
TEST_F(pixel, fractional_sharp_interior_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 2560, 1440, false);
    de_pixel_art_settings settings = {
        .internal_width = 480, .internal_height = 270, .integer_scale_only = false};
    de_view_set_pixel_art(f->r, f->view, &settings);
    make_pattern(f);
    pixel_frame(f);
    const u8 *data = read_output(f);
    /* (7.5,2.5)/5.333 = (1.40625,.46875): well inside white texel (1,0). */
    u8 blue = data[((size_t)2 * f->ow + 7) * 4 + 2];
    printf("Fractional sharp checker interior: blue=%u (expected255)\n", blue);
    EXPECT_TRUE(blue >= 254);
}
