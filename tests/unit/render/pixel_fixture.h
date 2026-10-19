#ifndef DE_TEST_PIXEL_FIXTURE_H
#define DE_TEST_PIXEL_FIXTURE_H
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
typedef struct pixel_fixture {
    de_gpu_device *d;
    de_renderer *r;
    de_type_registry *types;
    de_world *world;
    de_entity camera, sprite;
    de_view view;
    de_gpu_texture target, pattern;
    de_gpu_buffer readback;
    bool native;
    u8 *packed_readback;
    u32 iw, ih, ow, oh, internal_seen, ui_seen;
    de_vec2 residual;
    de_mat4 view_matrix;
} pixel_fixture;
static void pixel_fixture_setup(pixel_fixture *f) {
    (void)f;
}
static void pixel_fixture_teardown(pixel_fixture *f) {
    free(f->packed_readback);
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
    f->sprite = sprite;
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
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, INT32_MAX - 2, "pixel.test.pattern",
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
    u32 pitch = (f->ow * 4 + 255u) & ~255u;
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
    const u8 *mapped = de_gpu_buffer_map(f->d, f->readback);
    if (pitch == f->ow * 4)
        return mapped;
    if (!f->packed_readback)
        f->packed_readback = malloc((size_t)f->ow * f->oh * 4);
    ASSERT_TRUE(mapped != NULL && f->packed_readback != NULL);
    for (u32 y = 0; y < f->oh; ++y)
        memcpy(f->packed_readback + (size_t)y * f->ow * 4, mapped + (size_t)y * pitch,
               (size_t)f->ow * 4);
    return f->packed_readback;
}

#endif
