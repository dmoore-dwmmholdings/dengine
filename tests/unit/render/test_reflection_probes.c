#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include <dengine/de_reflect.h>
#include "gpu/gpu_backend.h"
#include "render/renderer_internal.h"
#include "render/probes/probes_internal.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_env.h"
#include "render/formats/de_cooked_le.h"
#include "render/materials/material_models.h"
#include "render/materials/brdf_luts.h"
#include "render/passes/vis/gbuffer.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/loaders/render_assets.h"
#include "render_d3d12_test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "lighting_fixture.h"
#if DE_HAS_COOKER
#include "cook/importers/texture/env_internal.h"
#endif

typedef struct pfx {
    de_tracking_allocator tracking;
    de_allocator alloc;
    de_gpu_device *dev;
    de_renderer *r;
    de_type_registry *types;
    de_world *ecs;
    world scene;
    bool scene_live;
    de_asset_system *assets;
    de_task_system *tasks;
    de_load_handle load;
    bool mounted;
} pfx;
static void pfx_setup(pfx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_RENDER);
}
static void pfx_teardown(pfx *f) {
    if (f->assets) {
        if (DE_HANDLE_VALID(f->load))
            de_asset_release(f->assets, f->load);
        de_asset_system_destroy(f->assets);
        if (f->r)
            f->r->assets = NULL;
    }
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    if (f->mounted)
        (void)de_fs_unmount("dev", DE_PROBE_TEST_TMP_DIR);
    if (f->r) {
        if (f->r->render_world)
            f->r->render_world->current = NULL;
        de_renderer_destroy(f->r);
    }
    if (f->scene_live)
        world_free(&f->scene);
    for (u32 i = 0; i < g_mesh_count; ++i)
        mesh_free(&g_mesh[i], f->dev);
    g_mesh_count = g_mat_count = 0;
    if (f->ecs)
        de_world_destroy(f->ecs);
    if (f->types)
        de_type_registry_destroy(f->types);
    if (f->dev) {
        EXPECT_EQ(validation_errors(f->dev), 0u);
        de_gpu_device_destroy(f->dev);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
}
static void open_renderer(pfx *f, de_gpu_backend backend) {
    de_fp_env_init_thread();
    if (!backend_built(backend) && backend != DE_GPU_NULL)
        SKIP("backend not built");
    de_gpu_device_desc desc = {.backend = backend,
                               .allocator = &f->alloc,
                               .debug_layer = backend != DE_GPU_NULL,
                               .gpu_validation = backend == DE_GPU_VULKAN,
                               .pipeline_cache_dir = ""};
    de_result result = de_gpu_device_create(&desc, &f->dev);
    if (result == DE_ERR_UNSUPPORTED && backend == DE_GPU_VULKAN) {
        desc.adapter = 1;
        result = de_gpu_device_create(&desc, &f->dev);
    }
    if (result != DE_OK)
        SKIP("no usable GPU");
    render_d3d12_report(f->dev);
    if (!de_gpu_get_caps(f->dev)->base_tier_ok)
        SKIP("GPU below Base tier");
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->ecs),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .allocator = &f->alloc,
                                                     .types = f->types,
                                                     .frames_in_flight = 2},
                                 &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->ecs);
    (void)de_cvar_set(de_cvar_find("r.pipelined"), "0");
    (void)de_cvar_set(de_cvar_find("r.probe_faces_per_frame"), "6");
    (void)de_cvar_set(de_cvar_find("r.probe_pool_size"), "16");
}
static const f32 room_colors[6][3] = {{2, .1f, .2f}, {.1f, 3, .3f}, {.2f, .3f, 4},
                                      {2, 3, .2f},   {.2f, 3, 4},   {2, .2f, 4}};
static void colored_room(pfx *f) {
    world_init(&f->scene, 8);
    f->scene_live = true;
    g_mesh_count = 1;
    mesh_plane(&g_mesh[0], f->dev, 1, 1, 10);
    for (u32 face = 0; face < 6; ++face) {
        u32 mat = material_add(0, .5f, 0, .5f);
        g_mat[mat - 1].standard.emissive =
            (de_vec3){room_colors[face][0], room_colors[face][1], room_colors[face][2]};
        g_mat[mat - 1].standard.double_sided = true;
        de__rw_view camera;
        de__probe_face_camera((de_dvec3){0}, (de_dvec3){0}, face, &camera);
        de_affine xf = camera.transform;
        for (u32 k = 0; k < 3; ++k)
            xf.m[9 + k] = -xf.m[6 + k] * 5;
        world_set(&f->scene, face, 1, xf, mat);
    }
    f->scene.f.probe_count = 1;
    f->scene.f.probes[0] = (de__probe_snapshot){.entity = 42,
                                                .rotation = {0, 0, 0, 1},
                                                .probe = {.extent = {5, 5, 5},
                                                          .blend_distance = 1,
                                                          .box_projection = true,
                                                          .resolution = 64,
                                                          .intensity = 1}};
    de__gpu_scene_destroy(f->r->gpu_scene);
    ASSERT_EQ(
        de__gpu_scene_create(&(de__gpu_scene_desc){.device = f->dev,
                                                   .allocator = &f->alloc,
                                                   .resolver = {.mesh = fake_mesh,
                                                                .material = fake_material,
                                                                .texture_bindless = fake_texture},
                                                   .staging_bytes = 16u << 20,
                                                   .views_in_view_graphs = true,
                                                   .initial_capacity[DE__GS_VIEWS] = DE__VIEW_MAX},
                             &f->r->gpu_scene),
        DE_OK);
}
static void room_frame(pfx *f) {
    world_begin(&f->scene);
    f->r->render_world->last = &f->scene.f;
    f->r->render_world->current = &f->scene.f;
    de__renderer_kick_prepare(f->r);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    de_gpu_wait_idle(f->dev);
}
TEST(probes, probe_capture_face_orientation) {
    for (u32 face = 0; face < 6; ++face) {
        de__rw_view c;
        de__probe_face_camera((de_dvec3){100000, 2, 3}, (de_dvec3){99328, 0, 0}, face, &c);
        de_mat4 view = de_affine_to_m4(&c.transform);
        f32 d[3];
        de_cooked_env_face_direction(face, 0, 0, d);
        for (u32 k = 0; k < 3; ++k)
            EXPECT_FLOAT_EQ(view.m[8 + k], -d[k], 1e-6);
        EXPECT_FLOAT_EQ(c.offset.x, 672, 1e-6);
        EXPECT_FLOAT_EQ(de__exposure_from_ev100(c.camera.exposure_ev100), 1, 1e-6);
    }
}
TEST(probes, probe_box_projection_golden) {
    DeReflectionProbe p = {.extent = {5, 4, 3}, .box_projection = true};
    de_vec3 hit = de__probe_parallax(&p, (de_vec3){1, 1, 1}, (de_vec3){4, 3, 2});
    EXPECT_FLOAT_EQ(hit.x, 5, 1e-6);
    EXPECT_FLOAT_EQ(hit.y, 4, 1e-6);
    EXPECT_FLOAT_EQ(hit.z, 3, 1e-6);
    p.shape = DE_PROBE_SPHERE;
    p.extent.x = 3;
    hit = de__probe_parallax(&p, (de_vec3){1, 0, 0}, (de_vec3){0, 1, 0});
    EXPECT_FLOAT_EQ(hit.x * hit.x + hit.y * hit.y + hit.z * hit.z, 9, 1e-5);
}
TEST(probes, probe_blend_continuous) {
    DeReflectionProbe p = {.extent = {5, 5, 5}, .blend_distance = 2};
    f32 previous = 0;
    for (u32 k = 0; k <= 10000; ++k) {
        f32 x = -5 + (f32)k * .001f;
        f32 a = de__probe_weight(&p, (de_vec3){x, 0, 0});
        f32 b = de__probe_weight(&p, (de_vec3){x - 2, 0, 0});
        f32 value = a + (1 - a) * b * 2;
        if (k)
            EXPECT_TRUE(fabsf(value - previous) < .002f);
        previous = value;
    }
}
TEST(probes, probe_fallback_sky) {
    DeReflectionProbe p = {.extent = {5, 5, 5}, .blend_distance = 1};
    EXPECT_FLOAT_EQ(de__probe_weight(&p, (de_vec3){5, 0, 0}), 0, 0);
    EXPECT_FLOAT_EQ(de__probe_weight(&p, (de_vec3){50, 50, 50}), 0, 0);
    p.shape = DE_PROBE_SPHERE;
    EXPECT_FLOAT_EQ(de__probe_weight(&p, (de_vec3){4, 4, 0}), 0, 0);
}
TEST_F(probes, probe_pool_lru, pfx) {
    open_renderer(f, DE_GPU_NULL);
    de__probe_store s;
    ASSERT_EQ(de__probe_store_init(&s, f->dev, &f->alloc, 64, 3), DE_OK);
    de__probe_handle a = de__probe_store_alloc(&s, 1, 1), b = de__probe_store_alloc(&s, 2, 2);
    de__probe_store_free(&s, a.slot);
    de__probe_handle b2 = de__probe_store_alloc(&s, 2, 3);
    EXPECT_EQ(b.slot, b2.slot);
    EXPECT_EQ(b.generation, b2.generation);
    a = de__probe_store_alloc(&s, 1, 4);
    de__probe_handle c = de__probe_store_alloc(&s, 3, 5);
    EXPECT_TRUE(de__probe_store_valid(&s, c));
    de__probe_handle d = de__probe_store_alloc(&s, 4, 6);
    EXPECT_EQ(d.slot, b.slot);
    EXPECT_FALSE(de__probe_store_valid(&s, b));
    EXPECT_TRUE(de__probe_store_valid(&s, a));
    de__probe_store_destroy(&s);
}
TEST(probes, probe_bake_reload_identical) {
    u64 size = de__probe_payload_size(64);
    u8 *payload = malloc((size_t)size), *blob = malloc((size_t)size + 48);
    ASSERT_TRUE(payload && blob);
    for (u64 i = 0; i < size; ++i)
        payload[i] = (u8)(i * 127u);
    ASSERT_EQ(de__probe_blob_write(blob, size + 48, 123, 64, payload), DE_OK);
    const void *data = NULL;
    u32 resolution = 0;
    ASSERT_EQ(de__probe_blob_read(blob, size + 48, 123, &resolution, &data), DE_OK);
    EXPECT_EQ(resolution, 64u);
    EXPECT_EQ(memcmp(data, payload, (size_t)size), 0);
    for (u64 n = 0; n < 48; ++n)
        EXPECT_NE(de__probe_blob_read(blob, n, 123, &resolution, &data), DE_OK);
    EXPECT_NE(de__probe_blob_read(blob, size + 47, 123, &resolution, &data), DE_OK);
    EXPECT_NE(de__probe_blob_read(blob, size + 48, 124, &resolution, &data), DE_OK);
    blob[48] ^= 1;
    EXPECT_NE(de__probe_blob_read(blob, size + 48, 123, &resolution, &data), DE_OK);
    blob[48] ^= 1;
    blob[4] = 2;
    EXPECT_NE(de__probe_blob_read(blob, size + 48, 123, &resolution, &data), DE_OK);
    free(payload);
    free(blob);
}
static void capture_colors(pfx *f, de_gpu_backend backend, bool time_slice) {
    open_renderer(f, backend);
    colored_room(f);
    if (time_slice)
        (void)de_cvar_set(de_cvar_find("r.probe_faces_per_frame"), "1");
    for (u32 i = 0; i < (time_slice ? 6u : 1u); ++i) {
        room_frame(f);
        de__probe_info info;
        ASSERT_TRUE(de__probes_info(f->r, 42, &info));
        EXPECT_EQ(info.capture_faces, time_slice ? i + 1 : 6u);
        EXPECT_EQ(info.ready, i == (time_slice ? 5u : 0u));
        de_view order[8];
        u32 count = de__views_last_order(f->r, order, 8);
        EXPECT_EQ(count,
                  time_slice ? 1u : 6u); /* No gameplay camera. Only scheduled faces render. */
    }
    u64 size = de__probe_payload_size(64);
    u16 *payload = malloc((size_t)size);
    ASSERT_TRUE(payload);
    ASSERT_EQ(de__probes_readback(f->r, 42, payload, size), DE_OK);
    for (u32 face = 0; face < 6; ++face)
        for (u32 c = 0; c < 3; ++c) {
            f32 value = half_to_f32(payload[((size_t)face * 64 * 64 + 32 * 64 + 32) * 4 + c]);
            printf("face %u channel %u: %.5f expected %.5f\n", face, c, value,
                   room_colors[face][c]);
            EXPECT_FLOAT_EQ(value, room_colors[face][c], .02f);
        }
    de__probe_info info;
    ASSERT_TRUE(de__probes_info(f->r, 42, &info));
    room_frame(f);
    de__probe_info once;
    ASSERT_TRUE(de__probes_info(f->r, 42, &once));
    EXPECT_EQ(once.capture_faces, info.capture_faces);
    EXPECT_EQ(de__views_last_order(f->r, NULL, 0), 0u);
    f->scene.f.probe_count = 0;
    room_frame(f);
    EXPECT_FALSE(de__probes_info(f->r, 42, &info));
    EXPECT_EQ(de__views_last_order(f->r, NULL, 0), 0u);
    free(payload);
}
TEST_F(probes, capture_colored_room_metal, pfx) {
    capture_colors(f, DE_GPU_METAL, false);
}
TEST_F(probes, capture_colored_room_vulkan, pfx) {
    capture_colors(f, DE_GPU_VULKAN, false);
}
TEST_F(probes, capture_colored_room_d3d12, pfx) {
    capture_colors(f, DE_GPU_D3D12, false);
}
TEST_F(probes, probe_time_sliced_order_metal, pfx) {
    capture_colors(f, DE_GPU_METAL, true);
}
TEST_F(probes, probe_time_sliced_order_vulkan, pfx) {
    capture_colors(f, DE_GPU_VULKAN, true);
}
TEST_F(probes, probe_time_sliced_order_d3d12, pfx) {
    capture_colors(f, DE_GPU_D3D12, true);
}

#if DE_HAS_COOKER
static void prefilter_reference(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    room_frame(f);
    u64 bytes = de__probe_payload_size(64);
    u16 *gpu = malloc((size_t)bytes);
    ASSERT_TRUE(gpu);
    ASSERT_EQ(de__probes_readback(f->r, 42, gpu, bytes), DE_OK);
    de_tex_image image = {.width = 64, .height = 64, .layers = 6, .cube = true};
    image.rgba = malloc(6u * 64u * 64u * 4u * sizeof(f32));
    ASSERT_TRUE(image.rgba);
    /* DENG-100 interprets cube sources as Y-up; rotate captured engine-space source into that
     * convention. The 90-degree permutation preserves texel centres exactly. */
    for (u32 face = 0; face < 6; ++face)
        for (u32 y = 0; y < 64; ++y)
            for (u32 x = 0; x < 64; ++x) {
                f32 source[3];
                de_cooked_env_face_direction(face, ((f32)x + .5f) / 32 - 1, ((f32)y + .5f) / 32 - 1,
                                             source);
                f32 engine[3] = {source[0], -source[2], source[1]}, sc, tc;
                u32 ef = de_cooked_env_direction_face(engine, &sc, &tc);
                u32 ex = (u32)fminf(63, floorf((sc + 1) * 32)),
                    ey = (u32)fminf(63, floorf((tc + 1) * 32));
                for (u32 c = 0; c < 4; ++c)
                    image.rgba[(((size_t)face * 64 + y) * 64 + x) * 4 + c] =
                        half_to_f32(gpu[(((size_t)ef * 64 + ey) * 64 + ex) * 4 + c]);
            }
    de_tex_chain reference;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &image, &(de_env_settings){.size = 64, .samples = 1024},
                           &reference),
              DE_OK);
    u64 off = 0;
    f32 worst = 0;
    for (u32 mip = 0; mip < reference.mip_count; ++mip) {
        u32 n = reference.width[mip];
        for (u32 i = 0; i < 6 * n * n; ++i)
            for (u32 c = 0; c < 3; ++c) {
                f32 actual = half_to_f32(gpu[off + (u64)i * 4 + c]),
                    expected = reference.rgba[mip][i * 4 + c];
                f32 error = fabsf(actual - expected) / fmaxf(expected, .05f);
                if (error > worst)
                    worst = error;
            }
        off += (u64)n * n * 6 * 4;
    }
    printf("GPU/DENG-100 worst relative prefilter error: %.6f\n", worst);
    EXPECT_TRUE(worst <= .02f);
    de_tex_chain_free(&f->alloc, &reference);
    free(image.rgba);
    free(gpu);
}
TEST_F(probes, probe_prefilter_matches_cpu_metal, pfx) {
    prefilter_reference(f, DE_GPU_METAL);
}
TEST_F(probes, probe_prefilter_matches_cpu_vulkan, pfx) {
    prefilter_reference(f, DE_GPU_VULKAN);
}
TEST_F(probes, probe_prefilter_matches_cpu_d3d12, pfx) {
    prefilter_reference(f, DE_GPU_D3D12);
}
#endif

static void storage_reload(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    room_frame(f);
    u64 bytes = de__probe_payload_size(64);
    u8 *original = malloc((size_t)bytes), *copy = malloc((size_t)bytes),
       *blob = malloc((size_t)bytes + 48);
    ASSERT_TRUE(original && copy && blob);
    ASSERT_EQ(de__probes_readback(f->r, 42, original, bytes), DE_OK);
    de__probe_store store;
    ASSERT_EQ(de__probe_store_init(&store, f->dev, &f->alloc, 256, 2), DE_OK);
    de__probe_handle h = de__probe_store_alloc_resolution(&store, 77, 1, 64);
    ASSERT_TRUE(de__probe_store_valid(&store, h));
    u64 hash = de__probe_settings_hash(&f->scene.f.probes[0]);
    ASSERT_EQ(de__probe_blob_write(blob, bytes + 48, hash, 64, original), DE_OK);
    const void *data = NULL;
    u32 resolution;
    ASSERT_EQ(de__probe_blob_read(blob, bytes + 48, hash, &resolution, &data), DE_OK);
    /* Upload does not need a capture: the generic API's renderer is the only dependency. */
    de__probe_capture capture = {.renderer = f->r};
    ASSERT_EQ(de__probe_store_upload(&capture, &store, h, data), DE_OK);
    ASSERT_EQ(de__probe_store_readback(&capture, &store, h, copy), DE_OK);
    EXPECT_EQ(memcmp(copy, original, (size_t)bytes), 0);
    /* A replacement upload into a ready slot also preserves bytes. */
    ASSERT_EQ(de__probe_store_upload(&capture, &store, h, data), DE_OK);
    ASSERT_EQ(de__probe_store_readback(&capture, &store, h, copy), DE_OK);
    EXPECT_EQ(memcmp(copy, original, (size_t)bytes), 0);
    de__probe_store_destroy(&store);
    free(original);
    free(copy);
    free(blob);
}
TEST_F(probes, store_bake_reload_metal, pfx) {
    storage_reload(f, DE_GPU_METAL);
}
TEST_F(probes, store_bake_reload_vulkan, pfx) {
    storage_reload(f, DE_GPU_VULKAN);
}
TEST_F(probes, store_bake_reload_d3d12, pfx) {
    storage_reload(f, DE_GPU_D3D12);
}

typedef struct reference_probe {
    f32 position[4], extent[4], rotation[4], blend[4];
    u32 info[4];
} reference_probe;
static void probe_reference_run(pfx *f, reference_probe *records, u32 probes, const f32 *queries,
                                u32 count, f32 *output) {
    de__vis_shared *vis = de__views_vis(f->r);
    const char *entries[] = {"cs_probe_reference"};
    de__engine_shader_code code;
    ASSERT_EQ(de__vis_shader_load(vis, "reflection_probe_sample", entries, 1, &code), DE_OK);
    de__vis_kernel kernel;
    const u32 group[] = {64, 1, 1};
    ASSERT_EQ(de__vis_kernel_create(vis, &code, 0, "probe.reference", group, 32, &kernel), DE_OK);
    de__engine_shader_code_free(&code);
    de_gpu_buffer rec =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)probes * sizeof *records,
                                                           .stride = sizeof *records,
                                                           .usage = DE_USAGE_STORAGE,
                                                           .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(f->dev, rec), records, (size_t)probes * sizeof *records);
    u32 list[65] = {0};
    list[0] = probes;
    for (u32 i = 0; i < probes; ++i)
        list[1 + i] = i;
    de_gpu_buffer lists = upload_buf(f->dev, list, sizeof list);
    de_gpu_buffer input =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 32,
                                                           .stride = 16,
                                                           .usage = DE_USAGE_STORAGE,
                                                           .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(f->dev, input), queries, (size_t)count * 32);
    de_gpu_buffer result = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16,
                                      .stride = 16,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK});
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "probe.reference");
    ASSERT_TRUE(cmd);
    const de_gpu_barrier before = {
        .buffer = result, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &before, 1);
    const u32 pc[] = {de_gpu_buffer_bindless(f->dev, rec),
                      de_gpu_buffer_bindless(f->dev, lists),
                      0,
                      records[0].info[0] /* overwritten below */,
                      de_gpu_buffer_bindless(f->dev, input),
                      de_gpu_buffer_bindless(f->dev, result),
                      count,
                      0};
    u32 params[8];
    memcpy(params, pc, sizeof params);
    de_gpu_sampler sampler =
        de_gpu_sampler_create(f->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                             .mag = DE_FILTER_LINEAR,
                                                             .mip = DE_FILTER_LINEAR,
                                                             .u = DE_ADDR_CLAMP,
                                                             .v = DE_ADDR_CLAMP,
                                                             .w = DE_ADDR_CLAMP});
    params[3] = de_gpu_sampler_bindless(f->dev, sampler);
    de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, params, sizeof params);
    de_gpu_cmd_dispatch(cmd, (count + 63) / 64, 1, 1);
    const de_gpu_barrier after = {
        .buffer = result, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &after, 1);
    de_gpu_cmd_copy_buffer(cmd, rb, 0, result, 0, (u64)count * 16);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    memcpy(output, de_gpu_buffer_map(f->dev, rb), (size_t)count * 16);
    de_gpu_buffer_destroy(f->dev, rec);
    de_gpu_buffer_destroy(f->dev, lists);
    de_gpu_buffer_destroy(f->dev, input);
    de_gpu_buffer_destroy(f->dev, result);
    de_gpu_buffer_destroy(f->dev, rb);
    de_gpu_sampler_destroy(f->dev, sampler);
    de__vis_kernel_destroy(vis, &kernel);
}
static reference_probe reference_record(de_gpu_device *dev, const de__probe_store *store,
                                        de__probe_handle h, f32 center) {
    reference_probe p = {.position = {center, 0, 0, 8.660254f},
                         .extent = {5, 5, 5, 1},
                         .rotation = {0, 0, 0, 1},
                         .blend = {1, 64, 0, 0},
                         .info = {0, 1, 0, 7}};
    p.info[2] = de_gpu_texture_bindless(dev, store->slots[h.slot].strip, 0, false);
    return p;
}
static void gpu_projection_blend(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    room_frame(f);
    u64 size = de__probe_payload_size(64);
    u16 *payload = malloc((size_t)size);
    ASSERT_TRUE(payload);
    ASSERT_EQ(de__probes_readback(f->r, 42, payload, size), DE_OK);
    de__probe_store store;
    ASSERT_EQ(de__probe_store_init(&store, f->dev, &f->alloc, 256, 2), DE_OK);
    de__probe_handle a = de__probe_store_alloc_resolution(&store, 1, 1, 64),
                     b = de__probe_store_alloc_resolution(&store, 2, 2, 64);
    de__probe_capture capture = {.renderer = f->r};
    ASSERT_EQ(de__probe_store_upload(&capture, &store, a, payload), DE_OK);
    /* Distinguishable overlapping probe: double radiance in every mip. */
    for (u64 i = 0; i < size / 2; ++i)
        if (i % 4 != 3) {
            if ((payload[i] & 0x7c00u) != 0 && (payload[i] & 0x7c00u) < 0x7800u)
                payload[i] = (u16)(payload[i] + 1024u);
        }
    ASSERT_EQ(de__probe_store_upload(&capture, &store, b, payload), DE_OK);
    reference_probe records[2] = {reference_record(f->dev, &store, a, 0),
                                  reference_record(f->dev, &store, b, 2)};
    enum { SIDE = 32, COUNT = SIDE * SIDE };
    f32 queries[COUNT * 8], out[COUNT * 4];
    /* Reference camera inside the colored box, displaced from its probe. Mirror directions
     * cover the upper room, including side/top corner boundaries. */
    for (u32 y = 0; y < SIDE; ++y)
        for (u32 x = 0; x < SIDE; ++x) {
            u32 i = y * SIDE + x;
            f32 *q = queries + i * 8;
            memset(q, 0, 8 * sizeof *q);
            q[0] = 2;
            q[1] = 1;
            q[2] = -1;
            q[4] = ((f32)x + .5f) / 8 - 2;
            q[5] = ((f32)y + .5f) / 8 - 2;
            q[6] = 1;
        }
    probe_reference_run(f, records, 1, queries, COUNT, out);
    for (u32 i = 0; i < COUNT; ++i) {
        const f32 *q = queries + i * 8;
        f32 t = 1e30;
        for (u32 k = 0; k < 3; ++k)
            if (fabsf(q[4 + k]) > 1e-6f)
                t = fminf(t, ((q[4 + k] > 0 ? 5.f : -5.f) - q[k]) / q[4 + k]);
        f32 hit[3] = {q[0] + t * q[4], q[1] + t * q[5], q[2] + t * q[6]}, sc, tc;
        u32 face = de_cooked_env_direction_face(hit, &sc, &tc);
        for (u32 c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(out[i * 4 + c], room_colors[face][c], .02f);
        EXPECT_FLOAT_EQ(out[i * 4 + 3], 1, 1e-6);
    }
    /* Sweep across both influence borders. Actual GPU radiance/alpha must stay continuous and
     * become alpha=0 outside, which is the sky-IBL fallback contract. */
    for (u32 i = 0; i < COUNT; ++i) {
        f32 *q = queries + i * 8;
        memset(q, 0, 8 * sizeof *q);
        q[0] = -6 + (f32)i * 14 / (COUNT - 1);
        q[6] = 1;
    }
    probe_reference_run(f, records, 2, queries, COUNT, out);
    f32 previous = 0;
    for (u32 i = 0; i < COUNT; ++i) {
        f32 sky = 7;
        f32 composed = out[i * 4] * out[i * 4 + 3] + sky * (1 - out[i * 4 + 3]);
        if (i)
            EXPECT_TRUE(fabsf(composed - previous) < .15f);
        previous = composed;
        if (queries[i * 8] <= -5 || queries[i * 8] >= 7)
            EXPECT_FLOAT_EQ(out[i * 4 + 3], 0, 1e-6);
    }
    free(payload);
    de__probe_store_destroy(&store);
}
TEST_F(probes, probe_projection_blend_sky_gpu_metal, pfx) {
    gpu_projection_blend(f, DE_GPU_METAL);
}
TEST_F(probes, probe_projection_blend_sky_gpu_vulkan, pfx) {
    gpu_projection_blend(f, DE_GPU_VULKAN);
}
TEST_F(probes, probe_projection_blend_sky_gpu_d3d12, pfx) {
    gpu_projection_blend(f, DE_GPU_D3D12);
}

typedef struct probe_view_read {
    de_renderer *renderer;
    de_view view;
    de_gpu_buffer readback;
    de_rg_resource source;
} probe_view_read;
static void probe_view_read_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const probe_view_read *r = (const probe_view_read *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, r->readback, 0, 512, de_rg_texture(pass, r->source), 0,
                                      0);
}
static void probe_view_read_hook(de_rg *g, de_view view, void *user) {
    probe_view_read *read = (probe_view_read *)user;
    if (view.id != read->view.id)
        return;
    de__lighting_context *lc = de__lighting_frame_context(de__views_lighting(read->renderer), view,
                                                          g, read->renderer->frame_index);
    ASSERT_TRUE(lc && lc->lit);
    read->source = lc->target;
    de_rg_resource rb = de_rg_import_buffer(g, read->readback, DE_ACCESS_COPY_DST);
    de_rg_pass *pass = de_rg_add_pass(g, "test.probe_view_readback", DE_QUEUE_GRAPHICS,
                                      probe_view_read_execute, read);
    de_rg_read(pass, read->source, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, rb, DE_ACCESS_COPY_DST);
}
static void read_center(pfx *f, de_gpu_buffer rb, f32 out[3]) {
    const u16 *pixels = (const u16 *)de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(pixels);
    for (u32 c = 0; c < 3; ++c)
        out[c] = half_to_f32(pixels[(32u * 64u + 32u) * 4u + c]);
}
static void render_lighting_parity(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    room_frame(f);
    u32 mirror = material_add(1, 0, 1, .5f);
    g_mat[mirror - 1].standard.double_sided = true;
    world_set(&f->scene, 6, 1,
              de_affine_from_trs((de_vec3){0, 0, -1}, de_quat_identity(), (de_vec3){1, 1, 1}),
              mirror);
    de_gpu_texture target = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = 64,
                                       .height = 64,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET});
    de_view view =
        de_view_create(f->r, &(de_view_desc){.target = target, .name = "probe.reference_camera"});
    ASSERT_TRUE(DE_HANDLE_VALID(view));
    de__rw_view camera = {
        .entity = 700,
        .camera = {.view = view, .near_z = .1f, .fov_y = 1.5707963f, .exposure_ev100 = -.2630344f},
        .offset = {0, 0, 2},
        .rotation = {0, 0, 0, 1},
        .transform = de_affine_identity()};
    camera.transform.m[11] = 2;
    f->scene.f.views = &camera;
    f->scene.f.view_count = 1;
    probe_view_read read = {
        .renderer = f->r,
        .view = view,
        .readback = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = 64u * 512u,
                                                                       .usage = DE_USAGE_COPY_DST,
                                                                       .memory = DE_MEM_READBACK})};
    de_render_add_hook(f->r, DE_HOOK_AFTER_TRANSPARENT, probe_view_read_hook, &read);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    settings.render_scale = 1;
    settings.ao = DE_QUALITY_OFF;
    settings.gi = DE_QUALITY_OFF;
    settings.sss = DE_QUALITY_OFF;
    de_view_set_settings(f->r, view, &settings);
    f32 opaque[3] = {0}, off[3], forward[3], forward_off[3];
    for (u32 frame = 0; frame < 6; ++frame) {
        camera.offset.x = (f32)(frame % 3) * .1f;
        camera.transform.m[9] = camera.offset.x;
        room_frame(f);
        read_center(f, read.readback, opaque);
        for (u32 c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(opaque[c], room_colors[4][c], .04f);
    }
    settings.reflections = DE_QUALITY_OFF;
    de_view_set_settings(f->r, view, &settings);
    room_frame(f);
    read_center(f, read.readback, off);
    for (u32 c = 0; c < 3; ++c)
        EXPECT_FLOAT_EQ(off[c], 0, .001f);
    g_mat[mirror - 1].alpha_mode = 2;
    g_mat[mirror - 1].standard.alpha_mode = 2;
    g_mat[mirror - 1].standard.base_color.w = .5f;
    f->scene.touch_all = true;
    room_frame(f);
    read_center(f, read.readback, forward_off);
    settings.reflections = DE_QUALITY_HIGH;
    de_view_set_settings(f->r, view, &settings);
    room_frame(f);
    read_center(f, read.readback, forward);
    for (u32 c = 0; c < 3; ++c) {
        printf("channel %u deferred %.5f forward_delta %.5f\n", c, opaque[c],
               (forward[c] - forward_off[c]) * 2);
        EXPECT_FLOAT_EQ((forward[c] - forward_off[c]) * 2, opaque[c], .06f);
    }
    f->scene.f.views = NULL;
    f->scene.f.view_count = 0;
    de_view_destroy(f->r, view);
    de_gpu_texture_destroy(f->dev, target);
    de_gpu_buffer_destroy(f->dev, read.readback);
}
TEST_F(probes, clustered_view_deferred_forward_metal, pfx) {
    render_lighting_parity(f, DE_GPU_METAL);
}
TEST_F(probes, clustered_view_deferred_forward_vulkan, pfx) {
    render_lighting_parity(f, DE_GPU_VULKAN);
}
TEST_F(probes, clustered_view_deferred_forward_d3d12, pfx) {
    render_lighting_parity(f, DE_GPU_D3D12);
}

static void renderer_lru(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    (void)de_cvar_set(de_cvar_find("r.probe_pool_size"), "1");
    f->scene.f.probe_count = 2;
    f->scene.f.probes[1] = f->scene.f.probes[0];
    f->scene.f.probes[0].probe.priority = 10;
    f->scene.f.probes[1].entity = 43;
    f->scene.f.probes[1].probe.resolution = 128;
    room_frame(f);
    de__probe_info a, b;
    ASSERT_TRUE(de__probes_info(f->r, 42, &a));
    EXPECT_TRUE(a.ready);
    room_frame(f);
    ASSERT_TRUE(de__probes_info(f->r, 42, &a));
    ASSERT_TRUE(de__probes_info(f->r, 43, &b));
    EXPECT_FALSE(a.ready);
    EXPECT_TRUE(b.ready);
    EXPECT_EQ(b.resolution, 128u);
    ASSERT_EQ(de_render_probe_request_update(f->r, (de_entity){42}), DE_OK);
    room_frame(f);
    ASSERT_TRUE(de__probes_info(f->r, 42, &a));
    ASSERT_TRUE(de__probes_info(f->r, 43, &b));
    EXPECT_TRUE(a.ready);
    EXPECT_FALSE(b.ready);
    EXPECT_EQ(a.capture_faces, 12u);
}
TEST_F(probes, renderer_pool_lru_restore_metal, pfx) {
    renderer_lru(f, DE_GPU_METAL);
}
TEST_F(probes, renderer_pool_lru_restore_vulkan, pfx) {
    renderer_lru(f, DE_GPU_VULKAN);
}
TEST_F(probes, renderer_pool_lru_restore_d3d12, pfx) {
    renderer_lru(f, DE_GPU_D3D12);
}

static void asset_bake_reload(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    room_frame(f);
    u64 bytes = de__probe_payload_size(64);
    u8 *before = malloc((size_t)bytes), *after = malloc((size_t)bytes);
    ASSERT_TRUE(before && after);
    ASSERT_EQ(de__probes_readback(f->r, 42, before, bytes), DE_OK);
    ASSERT_EQ(de_fs_mount("dev", DE_PROBE_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    f->mounted = true;
    ASSERT_EQ(de_command_exec("r.probes_bake"), DE_OK);
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.allocator = &f->alloc, .worker_count = 2}, &f->tasks),
              DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .types = f->types,
                                                             .cache_dir = "dev:/decache",
                                                             .io_threads = 1},
                                     &f->assets),
              DE_OK);
    const char manifest[] = "[assets]\n\"probes/room\" = { type = \"blob\", source = "
                            "\"probe-000000000000002a.deprobe\" }\n";
    ASSERT_EQ(de_file_write_all_atomic("dev:/assets.toml", manifest, sizeof manifest - 1), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "dev:/assets.toml"), DE_OK);
    de_asset_id id = de_asset_id_from("probes/room");
    f->load = de_asset_load_many(f->assets, &id, 1, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->load));
    ASSERT_EQ(de_asset_wait(f->assets, f->load, 10ull * 1000000000ull), DE_LOAD_READY);
    f->r->assets = f->assets;
    f->scene.f.probes[0].probe.bake = de_asset_blob(f->assets, id);
    ASSERT_TRUE(DE_HANDLE_VALID(f->scene.f.probes[0].probe.bake));
    /* Fresh probe subsystem: no captures, no warm slot. A shipping blob must supply all mips. */
    de__probes_destroy(f->r, f->r->probes);
    f->r->probes = NULL;
    ASSERT_EQ(de__probes_create(f->r, &f->r->probes), DE_OK);
    room_frame(f);
    de__probe_info info;
    ASSERT_TRUE(de__probes_info(f->r, 42, &info));
    EXPECT_TRUE(info.ready);
    EXPECT_TRUE(info.baked);
    EXPECT_EQ(info.capture_faces, 0u);
    EXPECT_EQ(de__views_last_order(f->r, NULL, 0), 0u);
    ASSERT_EQ(de__probes_readback(f->r, 42, after, bytes), DE_OK);
    EXPECT_EQ(memcmp(before, after, (size_t)bytes), 0);
    room_frame(f);
    ASSERT_TRUE(de__probes_info(f->r, 42, &info));
    EXPECT_EQ(info.capture_faces, 0u);
    free(before);
    free(after);
}
TEST_F(probes, asset_bake_reload_skips_capture_metal, pfx) {
    asset_bake_reload(f, DE_GPU_METAL);
}
TEST_F(probes, asset_bake_reload_skips_capture_vulkan, pfx) {
    asset_bake_reload(f, DE_GPU_VULKAN);
}
TEST_F(probes, asset_bake_reload_skips_capture_d3d12, pfx) {
    asset_bake_reload(f, DE_GPU_D3D12);
}

static de_result fail_probe_texture(de_gpu_device *dev, de__gpu_texture *texture,
                                    const de_gpu_texture_desc *desc) {
    (void)dev;
    (void)texture;
    (void)desc;
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected probe cube creation failure");
}

TEST_F(probes, allocation_failure_retry, pfx) {
    open_renderer(f, DE_GPU_NULL);
    de__probe_store store;
    const de__gpu_backend_vtable *original_vtable = f->dev->vt;
    de__gpu_backend_vtable failing_vtable = *original_vtable;
    failing_vtable.texture_create = fail_probe_texture;
    f->dev->vt = &failing_vtable;
    EXPECT_NE(de__probe_store_init(&store, f->dev, &f->alloc, 64, 2), DE_OK);
    EXPECT_EQ(store.capacity, 0u);
    EXPECT_FALSE(DE_HANDLE_VALID(store.cube));
    f->dev->vt = original_vtable;
    ASSERT_EQ(de__probe_store_init(&store, f->dev, &f->alloc, 64, 2), DE_OK);
    de__probe_handle a = de__probe_store_alloc(&store, 1, 1);
    ASSERT_TRUE(de__probe_store_valid(&store, a));
    de__probe_store_destroy(&store);
    de__probe_bounds_ring ring = {0};
    de_tracking_allocator_fail_after(&f->tracking, 0);
    EXPECT_NE(de__probe_bounds_reserve(f->dev, &ring, 32), DE_OK);
    EXPECT_EQ(ring.capacity, 0u);
    EXPECT_TRUE(ring.map == NULL);
    de_tracking_allocator_fail_after(&f->tracking, -1);
    ASSERT_EQ(de__probe_bounds_reserve(f->dev, &ring, 32), DE_OK);
    de_gpu_buffer previous = ring.bounds;
    f32 *previous_map = ring.map;
    de_tracking_allocator_fail_after(&f->tracking, 0);
    EXPECT_NE(de__probe_bounds_reserve(f->dev, &ring, 64), DE_OK);
    EXPECT_EQ(ring.capacity, 32u);
    EXPECT_EQ(ring.bounds.id, previous.id);
    EXPECT_TRUE(ring.map == previous_map);
    de_tracking_allocator_fail_after(&f->tracking, -1);
    ASSERT_EQ(de__probe_bounds_reserve(f->dev, &ring, 64), DE_OK);
    EXPECT_EQ(ring.capacity, 64u);
    de_gpu_buffer_destroy(f->dev, ring.bounds);
}

TEST_F(probes, reflected_component_extract_priority, pfx) {
    open_renderer(f, DE_GPU_NULL);
    const DeRenderComponents *ids = de__render_components(f->ecs);
    de_builtin_component_ids bi = de_builtin_components(f->ecs);
    de_type_id type = de_type_find(f->types, "DeReflectionProbe");
    const de_type_desc *desc = de_type_get(f->types, type);
    ASSERT_TRUE(desc);
    EXPECT_EQ(desc->field_count, 10u);
    EXPECT_EQ(desc->size, sizeof(DeReflectionProbe));
    de_entity highest = {0};
    for (u32 i = 0; i < 65; ++i) {
        de_entity entity;
        const de_component components[] = {bi.transform, ids->reflection_probe};
        ASSERT_EQ(de_entity_spawn_with(f->ecs, components, 2, &entity), DE_OK);
        DeReflectionProbe *probe =
            (DeReflectionProbe *)de_entity_get(f->ecs, entity, ids->reflection_probe);
        EXPECT_EQ(probe->resolution, 128u);
        EXPECT_FLOAT_EQ(probe->intensity, 1, 0);
        if (i == 64) {
            probe->priority = 100;
            probe->extent = (de_vec3){1, 2, 3};
            highest = entity;
        }
    }
    ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    const de__render_world_frame *frame = f->r->render_world->filling;
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->probe_count, 64u);
    EXPECT_EQ(frame->probes[0].entity, highest.bits);
    EXPECT_FLOAT_EQ(frame->probes[0].probe.extent.y, 2, 0);
    for (u32 i = 2; i < frame->probe_count; ++i)
        EXPECT_TRUE(frame->probes[i - 1].entity < frame->probes[i].entity);
}
static void pipelined_request(pfx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    const DeRenderComponents *ids = de__render_components(f->ecs);
    de_builtin_component_ids bi = de_builtin_components(f->ecs);
    de_entity entity;
    const de_component components[] = {bi.transform, ids->reflection_probe};
    ASSERT_EQ(de_entity_spawn_with(f->ecs, components, 2, &entity), DE_OK);
    ((DeReflectionProbe *)de_entity_get(f->ecs, entity, ids->reflection_probe))->resolution = 64;
    (void)de_cvar_set(de_cvar_find("r.pipelined"), "1");
    for (u32 k = 0; k < 3; ++k) {
        ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
        ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
        ASSERT_EQ(de__render_extract_and_kick(f->r, f->ecs), DE_OK);
        /* Request comes immediately after an asynchronous kick; it must wait rather than race
         * de__probes_after_views' publication of faces/queue state. */
        ASSERT_EQ(de_render_probe_request_update(f->r, entity), DE_OK);
        EXPECT_EQ(de__render_world_rendered(f->r), (u64)k + 1);
        de__probe_info info;
        ASSERT_TRUE(de__probes_info(f->r, entity.bits, &info));
        EXPECT_TRUE(info.ready);
        EXPECT_EQ(info.capture_faces, (k + 1) * 6);
    }
    (void)de_cvar_set(de_cvar_find("r.pipelined"), "0");
}
TEST_F(probes, pipelined_request_update_metal, pfx) {
    pipelined_request(f, DE_GPU_METAL);
}
TEST_F(probes, pipelined_request_update_vulkan, pfx) {
    pipelined_request(f, DE_GPU_VULKAN);
}
TEST_F(probes, pipelined_request_update_d3d12, pfx) {
    pipelined_request(f, DE_GPU_D3D12);
}
