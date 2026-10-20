#ifndef DE_TEST_SCENE_AS_FIXTURE_H
#define DE_TEST_SCENE_AS_FIXTURE_H
/* DENG-44: production scene AS lifecycle and native alpha readback. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv in loader_test_util.h; precedes all system headers. */
#endif
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "gpu/gpu_backend.h"
#include "render/components_internal.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/loaders/render_assets.h"
#include "render/materials/brdf_luts.h"
#include "render/materials/material_models.h"
#include "render/passes/sss/sss.h"
#include "render/passes/vis/gbuffer.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lighting_fixture.h"

#include "render/rt/rt_internal.h"
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>
#include "loader_test_util.h"
#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "render/loaders/upload_ring.h"

typedef struct rt_fixture {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_gpu_device *d;
    de_renderer *r;
    de_type_registry *types;
    de_asset_system *assets;
    de_task_system *tasks;
    world w;
    de__render_world handoff;
    de_view view;
    de_gpu_texture target, texture;
    de_gpu_buffer indices, rays, hits, readback;
    de_cooked_mesh_range ranges[2];
    de_render_submesh submesh;
    u32 flags, stages;
} rt_fixture;
static inline u32 texture_index(void *user, de_texture t) {
    rt_fixture *f = user;
    return t.id ? de_gpu_texture_bindless(f->d, f->texture, 0, false) : 0;
}
static inline void trace_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    rt_fixture *f = user;
    de__rt_context *c = &f->r->rt_scene->views[(f->view.id & 255u) - 1u].context;
    de__rt_trace(f->r, cmd, c, f->rays, f->hits, 2, f->flags);
    de_gpu_barrier b = {
        .buffer = f->hits, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, f->readback, 0, f->hits, 0, 2 * sizeof(de__rt_hit));
}
static inline void trace_stage(de__view_frame *vf, void *user) {
    rt_fixture *f = user;
    if (vf->view.id != f->view.id)
        return;
    de__rt_context *c = de__rt_context_get(f->r, vf->view, vf->graph, vf->frame_index);
    ASSERT_TRUE(c != NULL);
    ++f->stages;
    de_rg_pass *p =
        de_rg_add_pass(vf->graph, "RT acceptance trace", DE_QUEUE_GRAPHICS, trace_execute, f);
    de_rg_read(p, c->dependency, DE_ACCESS_SHADER_READ);
    de_rg_read(p, de_rg_import_buffer(vf->graph, f->rays, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_write(p, de_rg_import_buffer(vf->graph, f->hits, DE_ACCESS_COPY_SRC),
                DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, de_rg_import_buffer(vf->graph, f->readback, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
}
static inline void rt_fixture_setup(rt_fixture *f) {
    if (!de__gpu_find_backend(DE_GPU_METAL))
        SKIP("native Metal scene AS acceptance requires the Metal backend");
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
#if defined(__APPLE__)
    setenv("MTL_DEBUG_LAYER", "1", 1);
#endif
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                         .debug_layer = true,
                                                         .allocator = &f->dev_alloc,
                                                         .pipeline_cache_dir = ""},
                                   &f->d),
              DE_OK);
    ASSERT_TRUE(de_gpu_get_caps(f->d)->ray_query);
    printf("scene AS acceptance: %s\n", de_gpu_get_caps(f->d)->device_name);
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    ASSERT_EQ(
        de_renderer_create(
            &(de_renderer_desc){.device = f->d, .types = f->types, .allocator = &f->alloc}, &f->r),
        DE_OK);
    de_render_settings settings = {.tier = DE_TIER_RT, .render_scale = 1};
    de_renderer_set_settings(f->r, &settings);
    de__gpu_scene_destroy(f->r->gpu_scene);
    de__gpu_scene_desc desc = {.device = f->d,
                               .allocator = &f->alloc,
                               .views_in_view_graphs = true,
                               .resolver = {.user = f,
                                            .mesh = fake_mesh,
                                            .material = fake_material,
                                            .texture_bindless = texture_index}};
    desc.initial_capacity[DE__GS_INSTANCES] = 32;
    desc.initial_capacity[DE__GS_VIEWS] = 32;
    ASSERT_EQ(de__gpu_scene_create(&desc, &f->r->gpu_scene), DE_OK);
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 1;
    memset(g_mat, 0, sizeof g_mat);
    g_mat_count = 0;
    mesh_plane(&g_mesh[0], f->d, 1, 1, 2);
    de_render_mesh *m = &g_mesh[0].rm;
    u32 ix[2 * PATCH_TRIS * 3];
    u32 n = 0;
    for (u32 y = 0; y + 1 < PATCH; ++y)
        for (u32 x = 0; x + 1 < PATCH; ++x) {
            u32 a = y * PATCH + x, b = a + 1, c = b + PATCH, d = a + PATCH;
            ix[n++] = a;
            ix[n++] = b;
            ix[n++] = c;
            ix[n++] = a;
            ix[n++] = c;
            ix[n++] = d;
        }
    memcpy(ix + n, ix, n * 4);
    f->indices = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = sizeof ix,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT,
                                    .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(f->d, f->indices), ix, sizeof ix);
    m->buffers[DE_RENDER_MESH_INDICES] = f->indices;
    m->bindless[DE_RENDER_MESH_INDICES] = de_gpu_buffer_bindless(f->d, f->indices);
    m->index_count = n * 2;
    m->lod_count = 2;
    m->lods[0].index_count = n;
    m->lods[1] = m->lods[0];
    m->lods[1].index_first = n;
    m->lods[1].error = 1000;
    f->ranges[0] = (de_cooked_mesh_range){.index_count = n};
    f->ranges[1] = (de_cooked_mesh_range){.index_first = n, .index_count = n};
    m->lod_submeshes = f->ranges;
    m->submeshes = &f->submesh;
    material_add(1, 0.5f, 0, 0.5f);
    g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[0].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[0].standard.alpha_cutoff = 0.5f;
    g_mat[0].standard.base_color_tex = (de_texture){1};
    g_mat[0].textures[0] = (de_texture){1};
    f->texture = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 2,
                                     .height = 1,
                                     .depth_or_layers = 1,
                                     .mip_count = 1,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    de_gpu_buffer tex = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *pixels = de_gpu_buffer_map(f->d, tex);
    memset(pixels, 255, 256);
    pixels[3] = 0;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "alpha fixture upload");
    de_gpu_barrier b = {.texture = f->texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, f->texture, 0, 0, tex, 0, 256);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_buffer_destroy(f->d, tex);
    world_init(&f->w, 32);
    world_set(&f->w, 0, 1, at(3, 0, 0), 1);
    f->w.f.instances.flags[0] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    f->w.f.instances.layer_mask[0] = 1u << 24;
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->r->gpu_scene, &f->w.f), DE_OK);
    ASSERT_EQ(de__rt_prepare(f->r), DE_OK);
    ASSERT_TRUE(f->r->rt_scene != NULL);
    ASSERT_EQ(de__rt_mesh_publish(f->r->rt_scene, m), DE_OK);
    f->target = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 64,
                                     .height = 64,
                                     .depth_or_layers = 1,
                                     .mip_count = 1,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    f->view = de_view_create(
        f->r, &(de_view_desc){.target = f->target, .viewport = {0, 0, 1, 1}, .name = "RT fixture"});
    ASSERT_TRUE(f->view.id != 0);
    de_view_set_layer_mask(f->r, f->view, 1u << 24);
    de__rw_view camera = {
        .camera = {.fov_y = 0.9f, .near_z = 0.1f}, .offset = {3, 0, 4}, .rotation = {0, 0, 0, 1}};
    camera.transform = at(3, 0, 4);
    de__view_capture_camera(f->r, f->view, &camera);
    const de__rt_ray rays[2] = {{{2.5f, 0, 2, 0.001f}, {0, 0, -1, 20}},
                                {{3.5f, 0, 2, 0.001f}, {0, 0, -1, 20}}};
    f->rays = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = sizeof rays,
                                                               .usage = DE_USAGE_STORAGE,
                                                               .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(f->d, f->rays), rays, sizeof rays);
    f->hits = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 2 * sizeof(de__rt_hit),
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    f->readback = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = 2 * sizeof(de__rt_hit),
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK});
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_LIGHTING, INT32_MIN + 1, "RT acceptance",
                                      trace_stage, f),
              DE_OK);
}
static inline de__rt_hit *frame_rt(rt_fixture *f) {
    f->w.touch_all = true;
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->r->gpu_scene, &f->w.f), DE_OK);
    /* Same prepared/current snapshot ordering as render_thread.c, on this test thread. */
    f->handoff.last = &f->w.f;
    f->handoff.current = &f->w.f;
    f->r->render_world = &f->handoff;
    de__renderer_kick_prepare(f->r);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    de_gpu_wait_idle(f->d);
    f->handoff.current = NULL;
    ASSERT_TRUE(f->stages != 0);
    ASSERT_FALSE(de_gpu_device_is_lost(f->d));
    return de_gpu_buffer_map(f->d, f->readback);
}
static inline void rt_fixture_teardown(rt_fixture *f) {
    if (!f->d)
        return;
    de_gpu_wait_idle(f->d);
    f->r->render_world = NULL;
    de_asset_system_destroy(f->assets);
    f->r->assets = NULL;
    de_renderer_destroy(f->r);
    de_task_system_destroy(f->tasks);
    mesh_free(&g_mesh[0], f->d);
    de_gpu_buffer_destroy(f->d, f->indices);
    de_gpu_buffer_destroy(f->d, f->rays);
    de_gpu_buffer_destroy(f->d, f->hits);
    de_gpu_buffer_destroy(f->d, f->readback);
    de_gpu_texture_destroy(f->d, f->target);
    de_gpu_texture_destroy(f->d, f->texture);
    de_gpu_wait_idle(f->d);
    de_gpu_device_destroy(f->d);
    de_type_registry_destroy(f->types);
    world_free(&f->w);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).blocks_live, 0u);
}

#endif
