#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include <dengine/de_reflect.h>
#include "gpu/gpu_backend.h"
#include "render/renderer_internal.h"
#include "render/probes/probes_internal.h"
#include "render/probes/irradiance_internal.h"
#include "dpak_writer.h"
#include "render/passes/ssgi/ssgi_internal.h"
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

typedef struct gifx {
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
    char mount_root[1024];
    de_view view;
    de__rw_view camera;
    u32 width, height;
    de_render_settings settings;
    de_gpu_texture textures[8];
    u32 texture_count;
    de_gpu_buffer view_readback;
    de_rg_resource reads[5];
    u64 offsets[5];
    u32 pitches[5];
    bool suppress_readback;
    de__screen_space_context *common;
    de_gpu_buffer pre_readback;
    de_rg_resource pre_source;
    de_gpu_texture ao;
} gifx;
static void gifx_setup(gifx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_RENDER);
}
static void gifx_teardown(gifx *f) {
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, f->view_readback);
    de_gpu_buffer_destroy(f->dev, f->pre_readback);
    for (u32 i = 0; i < f->texture_count; ++i)
        de_gpu_texture_destroy(f->dev, f->textures[i]);
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
        (void)de_fs_unmount("dev", f->mount_root);
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
static void open_renderer(gifx *f, de_gpu_backend backend) {
    de_fp_env_init_thread();
    if (!backend_built(backend) && backend != DE_GPU_NULL)
        SKIP("backend not built");
#if defined(__APPLE__)
    if (backend == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
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
    (void)de_cvar_set(de_cvar_find("r.gi_faces_per_frame"), "6");
    (void)de_cvar_set(de_cvar_find("r.probe_pool_size"), "16");
    /* VVL 1.3.275 checks bindless image layouts at list end (DENG-235/DENG-427).
     * Match the shared lighting fixture: one graph command list, validation remains on. */
    if (backend == DE_GPU_VULKAN) {
        de_cvar cost = de_cvar_find("r.rg_record_cost");
        if (DE_HANDLE_VALID(cost))
            ASSERT_EQ(de_cvar_set(cost, "1048576"), DE_OK);
        else {
            char *args[] = {"test", "+r.rg_record_cost", "1048576"};
            ASSERT_EQ(de_cvar_apply_args(3, args), DE_OK);
        }
    }
}
static const f32 room_colors[6][3] = {{2, .1f, .2f}, {.1f, 3, .3f}, {.2f, .3f, 4},
                                      {2, 3, .2f},   {.2f, 3, 4},   {2, .2f, 4}};
static void colored_room(gifx *f) {
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
        /* Cube cameras mirror horizontal image coordinates. A mesh transform must instead be
         * right handed so the geometric winding and its +Z vertex normal both face inward. */
        for (u32 k = 0; k < 3; ++k)
            xf.m[k] = -xf.m[k];
        de_vec3 right = {xf.m[0], xf.m[1], xf.m[2]}, up = {xf.m[3], xf.m[4], xf.m[5]},
                normal = {xf.m[6], xf.m[7], xf.m[8]};
        EXPECT_TRUE(de_v3_dot(de_v3_cross(right, up), normal) > .999f);
        for (u32 k = 0; k < 3; ++k)
            xf.m[9 + k] = -xf.m[6 + k] * 5;
        world_set(&f->scene, face, 1, xf, mat);
    }
    f->scene.f.probe_count = 0;
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
static void room_frame(gifx *f) {
    world_begin(&f->scene);
    f->r->render_world->last = &f->scene.f;
    f->r->render_world->current = &f->scene.f;
    de__renderer_kick_prepare(f->r);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    de_gpu_wait_idle(f->dev);
}

static f32 unsigned_float(u32 value, u32 mantissa) {
    u32 exponent = value >> mantissa, fraction = value & ((1u << mantissa) - 1);
    return exponent ? ldexpf(1 + (f32)fraction / (f32)(1u << mantissa), (int)exponent - 15)
                    : ldexpf((f32)fraction, -14 - (int)mantissa);
}
static f32 quantize_unsigned_float(f32 value, u32 bits) {
    int exponent = 0;
    (void)frexpf(value, &exponent);
    f32 step = ldexpf(1, (exponent - 1 - (int)bits) > -14 - (int)bits ? (exponent - 1 - (int)bits)
                                                                      : -14 - (int)bits);
    return floorf(value / step) * step;
}
static void unpack_irradiance(u32 value, f32 rgb[3]) {
    rgb[0] = unsigned_float(value & 2047u, 6);
    rgb[1] = unsigned_float((value >> 11) & 2047u, 6);
    rgb[2] = unsigned_float(value >> 22, 5);
}
TEST(gi, layout_limits) {
    DeIrradianceVolume v = {.extent = {5, 5, 5}, .spacing = 2, .counts = {6, 6, 6}, .intensity = 1};
    de__gi_layout l;
    ASSERT_EQ(de__gi_layout_make(&v, &l), DE_OK);
    EXPECT_EQ(l.probes, 216u);
    EXPECT_EQ(l.columns, 64u);
    EXPECT_EQ(l.rows, 4u);
    EXPECT_EQ(l.iw, 640u);
    EXPECT_EQ(l.ih, 40u);
    EXPECT_EQ(l.dw, 1152u);
    EXPECT_EQ(l.dh, 72u);
    EXPECT_FLOAT_EQ(l.range, 32.f, 0);
    v.spacing = 3e38f;
    v.counts[0] = v.counts[1] = v.counts[2] = 2;
    ASSERT_EQ(de__gi_layout_make(&v, &l), DE_OK);
    EXPECT_FLOAT_EQ(l.range, 64.f, 0);
    v.counts[2] = 4096;
    EXPECT_NE(de__gi_layout_make(&v, &l), DE_OK);
    v.counts[2] = 2;
    v.spacing = NAN;
    EXPECT_NE(de__gi_layout_make(&v, &l), DE_OK);
    v.spacing = 1;
    v.extent.x = 0x1p-149f;
    v.counts[0] = 16;
    EXPECT_NE(de__gi_layout_make(&v, &l), DE_OK);
    v.extent.x = INFINITY;
    EXPECT_NE(de__gi_layout_make(&v, &l), DE_OK);
}
TEST(gi, blob_hostile_and_inplace) {
    DeIrradianceVolume v = {
        .extent = {5, 5, 5}, .spacing = 3e38f, .counts = {2, 2, 2}, .intensity = 1};
    de__gi_layout l, parsed;
    ASSERT_EQ(de__gi_layout_make(&v, &l), DE_OK);
    u64 bytes = DE__GI_BLOB_HEADER + l.payload_bytes;
    u8 *blob = calloc(1, (size_t)bytes), *payload = calloc(1, (size_t)l.payload_bytes);
    ASSERT_TRUE(blob && payload);
    const void *data = NULL;
    ASSERT_EQ(de__gi_blob_write(blob, bytes, 17, &l, payload), DE_OK);
    ASSERT_EQ(de__gi_blob_read(blob, bytes, 17, &parsed, &data), DE_OK);
    EXPECT_FLOAT_EQ(parsed.range, l.range, 0);
    EXPECT_EQ(parsed.probes, l.probes);
    EXPECT_EQ(memcmp(data, payload, (size_t)l.payload_bytes), 0);
    ASSERT_EQ(de__gi_blob_write(blob, bytes, 17, &l, data), DE_OK);
    ASSERT_EQ(de__gi_blob_read(blob, bytes, 17, &parsed, &data), DE_OK);
    for (u64 n = 0; n < bytes; n += n < 100 ? 1 : 113)
        EXPECT_NE(de__gi_blob_read(blob, n, 17, &parsed, &data), DE_OK);
    EXPECT_NE(de__gi_blob_read(blob, bytes, 18, &parsed, &data), DE_OK);
    put_u32(payload, 0x7c0u); // R11 exponent31: self-consistent hostile payload, not bad checksum
    EXPECT_NE(de__gi_blob_write(blob, bytes, 17, &l, payload), DE_OK);
    memset(payload, 0, (size_t)l.payload_bytes);
    payload[(size_t)l.iw * l.ih * 4 + 1] = 0x7c;
    EXPECT_NE(de__gi_blob_write(blob, bytes, 17, &l, payload), DE_OK);
    // A checksum-valid hostile body must be rejected by the READER too.
    for (u32 kind = 0; kind < 11; ++kind) {
        memset(payload, 0, (size_t)l.payload_bytes);
        ASSERT_EQ(de__gi_blob_write(blob, bytes, 17, &l, payload), DE_OK);
        if (kind == 0)
            put_u32(blob + 80, 0x7c0u);
        if (kind == 1)
            blob[80 + (size_t)l.iw * l.ih * 4 + 1] = 0x7c;
        if (kind == 2)
            blob[80 + (size_t)l.iw * l.ih * 4 + 1] = 0x40;
        if (kind == 3)
            put_u32(blob + 80 + ((size_t)l.iw * l.ih + (size_t)l.dw * l.dh) * 4, 0x7fc00000u);
        if (kind == 4)
            put_u32(blob + 80 + ((size_t)l.iw * l.ih + (size_t)l.dw * l.dh) * 4 + 12, 0x40000000u);
        if (kind == 5)
            put_u32(blob + 48, 0xffffffffu);
        if (kind == 6)
            put_u32(blob + 68, 0);
        if (kind == 7)
            put_u32(blob + 64, 0x3f000000u);
        if (kind >= 8)
            put_u32(blob + 48 + (kind - 8) * 4, 0); // persisted counts cannot use defaults
        ASSERT_EQ(de__probe_envelope_write(blob, bytes, "DEGI", 1, 17, blob + 48, bytes - 48),
                  DE_OK);
        EXPECT_NE(de__gi_blob_read(blob, bytes, 17, &parsed, &data), DE_OK);
    }
    for (u32 axis = 0; axis < 3; ++axis) {
        de__gi_layout invalid = l;
        invalid.counts[axis] = 0;
        EXPECT_NE(de__gi_blob_write(blob, bytes, 17, &invalid, payload), DE_OK);
    }
    free(blob);
    free(payload);
}
TEST(gi, reflection_envelope_compatible) {
    u64 n = de__probe_payload_size(64), bytes = n + DE__PROBE_BLOB_HEADER;
    u8 *payload = malloc((size_t)n), *a = malloc((size_t)bytes), *b = malloc((size_t)bytes);
    ASSERT_TRUE(payload && a && b);
    for (u64 i = 0; i < n; ++i)
        payload[i] = (u8)(i * 37);
    ASSERT_EQ(de__probe_blob_write(a, bytes, 42, 64, payload), DE_OK);
    // Independent construction of the old DEPR v1 envelope proves exact byte compatibility.
    memset(b, 0, 48);
    memcpy(b, "DEPR", 4);
    put_u32(b + 4, 1);
    put_u32(b + 8, 48);
    put_u32(b + 12, 64);
    de_cooked_put_u64(b + 16, 42);
    de_cooked_put_u64(b + 24, n);
    u64 hash = 14695981039346656037ull;
    for (u64 i = 0; i < n; ++i) {
        hash ^= payload[i];
        hash *= 1099511628211ull;
    }
    de_cooked_put_u64(b + 32, hash);
    memcpy(b + 48, payload, (size_t)n);
    EXPECT_EQ(memcmp(a, b, (size_t)bytes), 0);
    ASSERT_EQ(de__probe_blob_write(a, bytes, 42, 64, a + 48), DE_OK);
    EXPECT_EQ(memcmp(a, b, (size_t)bytes), 0);
    free(payload);
    free(a);
    free(b);
}
static void constant_room(gifx *f) {
    colored_room(f);
    // Pure emitters: finite-storage tests compare captured radiance with the analytic
    // constant field, independently of any specular response to emissive light proxies.
    for (u32 i = 0; i < g_mat_count; ++i)
        g_mat[i].standard.specular = 0;
    for (u32 i = 0; i < g_mat_count; ++i)
        g_mat[i].standard.emissive = (de_vec3){1, 2, 3};
    f->scene.f.irradiance_count = 1;
    f->scene.f.irradiance[0] = (de__irradiance_snapshot){.entity = 77,
                                                         .rotation = {0, 0, 0, 1},
                                                         .volume = {.extent = {1, 1, 1},
                                                                    .counts = {2, 2, 2},
                                                                    .intensity = 1,
                                                                    .bounces = 1,
                                                                    .blend_distance = .1f}};
}
static void read_texture_access(gifx *f, de_gpu_texture texture, u32 width, u32 height, u32 bytes,
                                void *data, u32 access) {
    u32 pitch = (width * bytes + 511u) & ~511u;
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * height,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(rb.id);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gi.test_readback");
    ASSERT_TRUE(cmd);
    de_gpu_barrier b = {.texture = texture, .before = access, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, pitch, texture, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = access;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    const u8 *map = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(map);
    for (u32 y = 0; y < height; ++y)
        memcpy((u8 *)data + (u64)y * width * bytes, map + (u64)y * pitch, width * bytes);
    de_gpu_buffer_destroy(f->dev, rb);
}
static void read_storage_texture(gifx *f, de_gpu_texture texture, u32 width, u32 height, u32 bytes,
                                 void *data) {
    read_texture_access(f, texture, width, height, bytes, data, DE__GI_ACCESS);
}
static void bake_constant(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    de__irradiance_info info;
    de__irradiance_debug_skip_depth(f->r, 1u);
    room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    EXPECT_EQ(info.probe, 0u);
    EXPECT_EQ(info.faces, 0u);
    EXPECT_FALSE(info.ready);
    de__irradiance_debug_skip_depth(f->r, 0);
    // One face per frame proves the raw GPU transfer preserves all prior faces.
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.gi_faces_per_frame"), "1"), DE_OK);
    for (u32 i = 0; i < 48; ++i)
        room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    ASSERT_TRUE(info.ready);
    u8 *payload = malloc((size_t)info.layout.payload_bytes);
    ASSERT_TRUE(payload);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, payload, info.layout.payload_bytes), DE_OK);
    u16 raw[32 * 192 * 4];
    f32 distances[32 * 192];
    read_storage_texture(f, info.raw_radiance, 32, 192, 8, raw);
    read_storage_texture(f, info.raw_distance, 32, 192, 4, distances);
    f32 minraw[3] = {INFINITY, INFINITY, INFINITY}, maxraw[3] = {0};
    for (u32 i = 0; i < 32 * 192; ++i)
        for (u32 c = 0; c < 3; ++c) {
            f32 value = half_to_f32(raw[i * 4 + c]);
            minraw[c] = fminf(minraw[c], value);
            maxraw[c] = fmaxf(maxraw[c], value);
        }
    for (u32 face = 0; face < 6; ++face)
        for (u32 y = 0; y < 32; ++y)
            for (u32 x = 0; x < 32; ++x) {
                f32 d[3];
                de_cooked_env_face_direction(face, ((f32)x + .5f) / 16 - 1, ((f32)y + .5f) / 16 - 1,
                                             d);
                f32 length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), hit = INFINITY;
                for (u32 k = 0; k < 3; ++k)
                    if (fabsf(d[k]) > 1e-6f)
                        hit = fminf(hit, ((d[k] > 0 ? 5.f : -5.f) - .5f) * length / d[k]);
                EXPECT_FLOAT_EQ(distances[face * 1024 + y * 32 + x],
                                fminf(hit / info.layout.range, 1), .002f);
            }
    printf("raw radiance ranges %g..%g %g..%g %g..%g\n", (double)minraw[0], (double)maxraw[0],
           (double)minraw[1], (double)maxraw[1], (double)minraw[2], (double)maxraw[2]);
    f32 mine[3] = {INFINITY, INFINITY, INFINITY}, maxe[3] = {0};
    for (u32 p = 0; p < info.layout.probes; ++p)
        for (u32 y = 0; y < 10; ++y)
            for (u32 x = 0; x < 10; ++x) {
                u32 pixel = (p / info.layout.columns * 10 + y) * info.layout.iw +
                            p % info.layout.columns * 10 + x;
                f32 rgb[3];
                unpack_irradiance(de_cooked_get_u32(payload + (u64)pixel * 4), rgb);
                for (u32 c = 0; c < 3; ++c) {
                    mine[c] = fminf(mine[c], rgb[c]);
                    maxe[c] = fmaxf(maxe[c], rgb[c]);
                }
            }
    printf("irradiance ranges %g..%g %g..%g %g..%g\n", (double)mine[0], (double)maxe[0],
           (double)mine[1], (double)maxe[1], (double)mine[2], (double)maxe[2]);
    for (u32 c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(mine[c],
                        quantize_unsigned_float((f32)(c + 1) * 3.141592653589793f, c == 2 ? 5 : 6),
                        .001f);
        EXPECT_FLOAT_EQ(maxe[c],
                        quantize_unsigned_float((f32)(c + 1) * 3.141592653589793f, c == 2 ? 5 : 6),
                        .001f);
    }
    u64 offset = (u64)info.layout.iw * info.layout.ih * 4;
    const u16 *moments = (const u16 *)(payload + offset);
    for (u32 i = 0; i < info.layout.dw * info.layout.dh * 2; ++i) {
        f32 value = half_to_f32(moments[i]);
        EXPECT_TRUE(isfinite(value) && value >= 0 && value <= 1);
    }
    u64 bytes = DE__GI_BLOB_HEADER + info.layout.payload_bytes;
    u8 *blob = malloc((size_t)bytes);
    ASSERT_TRUE(blob);
    ASSERT_EQ(de__gi_blob_write(blob, bytes, de__gi_settings_hash(&f->scene.f.irradiance[0]),
                                &info.layout, payload),
              DE_OK);
    de__gi_layout parsed;
    const void *data = NULL;
    ASSERT_EQ(de__gi_blob_read(blob, bytes, de__gi_settings_hash(&f->scene.f.irradiance[0]),
                               &parsed, &data),
              DE_OK);
    EXPECT_EQ(memcmp(data, payload, (size_t)parsed.payload_bytes), 0);
    free(blob);
    free(payload);
}
TEST_F(gi, bake_capture_depth_retry_metal, gifx) {
    bake_constant(f, DE_GPU_METAL);
}
TEST_F(gi, bake_capture_depth_retry_vulkan, gifx) {
    bake_constant(f, DE_GPU_VULKAN);
}
TEST_F(gi, bake_capture_depth_retry_d3d12, gifx) {
    bake_constant(f, DE_GPU_D3D12);
}
static const de__gpu_backend_vtable *gi_original_backend;
static u32 gi_failed_classifications;
static de_result fail_gi_classification(de_gpu_device *dev, de__gpu_buffer *buffer,
                                        const de_gpu_buffer_desc *desc) {
    if (desc->name && strcmp(desc->name, "gi.bake_classification") == 0) {
        ++gi_failed_classifications;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected GI classification upload failure");
    }
    return gi_original_backend->buffer_create(dev, buffer, desc);
}
static void nonaligned_bake_reload(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    for (u32 i = 0; i < g_mat_count; ++i)
        g_mat[i].standard.emissive = (de_vec3){10000, 20000, 30000};
    DeIrradianceVolume *volume = &f->scene.f.irradiance[0].volume;
    volume->counts[0] = volume->counts[1] = volume->counts[2] = 6;
    for (u32 i = 0; i < 216; ++i)
        room_frame(f);
    de__irradiance_info info;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    ASSERT_TRUE(info.ready);
    EXPECT_EQ(info.layout.probes, 216u);
    EXPECT_EQ(info.layout.rows, 4u);
    u64 bytes = info.layout.payload_bytes;
    u8 *before = malloc((size_t)bytes), *after = malloc((size_t)bytes);
    ASSERT_TRUE(before && after);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, before, bytes), DE_OK);
    for (u32 probe = 216; probe < 256; ++probe) {
        for (u32 y = 0; y < 10; ++y)
            for (u32 x = 0; x < 10; ++x) {
                u64 index = (u64)(probe / 64 * 10 + y) * info.layout.iw + probe % 64 * 10 + x;
                EXPECT_EQ(de_cooked_get_u32(before + index * 4), 0u);
            }
        for (u32 y = 0; y < 18; ++y)
            for (u32 x = 0; x < 18; ++x) {
                u64 index = (u64)(probe / 64 * 18 + y) * info.layout.dw + probe % 64 * 18 + x;
                EXPECT_EQ(de_cooked_get_u32(before + (u64)info.layout.iw * info.layout.ih * 4 +
                                            index * 4),
                          0x3c003c00u);
            }
    }
    f32 rgb[3];
    unpack_irradiance(de_cooked_get_u32(before), rgb);
    for (u32 c = 0; c < 3; ++c) {
        EXPECT_TRUE(isfinite(rgb[c]));
        EXPECT_FLOAT_EQ(
            rgb[c],
            quantize_unsigned_float(fminf((f32)(c + 1) * 10000 * 3.141592653589793f, 60000),
                                    c == 2 ? 5 : 6),
            1.f);
    }
    ++volume->priority; // Same radiance, new bake-affecting settings: deterministic recapture.
    for (u32 i = 0; i < 216; ++i)
        room_frame(f);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, bytes), DE_OK);
    EXPECT_EQ(memcmp(before, after, (size_t)bytes), 0);
    int root_length = snprintf(f->mount_root, sizeof f->mount_root, "%s/%s", DE_GI_TEST_TMP_DIR,
                               de_gpu_backend_name(backend));
    ASSERT_TRUE(root_length > 0 && (size_t)root_length < sizeof f->mount_root);
    ASSERT_EQ(de_fs_mount("dev", f->mount_root, DE_MOUNT_WRITABLE, 0), DE_OK);
    f->mounted = true;
    ASSERT_EQ(de_command_exec("r.gi_bake"), DE_OK);
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.allocator = &f->alloc, .worker_count = 2}, &f->tasks),
              DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .types = f->types,
                                                             .mode = DE_ASSET_MODE_PACKED,
                                                             .cache_dir = "dev:/decache",
                                                             .io_threads = 1},
                                     &f->assets),
              DE_OK);
    de_asset_id ids[] = {de_asset_id_from("gi/room"), de_asset_id_from("gi/wrong-counts"),
                         de_asset_id_from("gi/wrong-bounces")};
    de_asset_id id = ids[0];
    u64 blob_bytes = DE__GI_BLOB_HEADER + bytes;
    u8 *blob = malloc((size_t)blob_bytes);
    ASSERT_TRUE(blob);
    ASSERT_EQ(de__gi_blob_write(blob, blob_bytes, de__gi_settings_hash(&f->scene.f.irradiance[0]),
                                &info.layout, after),
              DE_OK);
    dpak_writer *writer = NULL;
    ASSERT_EQ(dpak_writer_create(&(dpak_writer_desc){.zstd_level = 1}, &writer), DE_OK);
    ASSERT_EQ(dpak_writer_add(writer, id.hash, "blob", blob, blob_bytes, NULL, 0, "gi/room"),
              DE_OK);
    de__gi_layout wrong = info.layout;
    wrong.counts[0] = 3;
    wrong.counts[1] = 6;
    wrong.counts[2] = 12;
    ASSERT_EQ(de__gi_blob_write(blob, blob_bytes, de__gi_settings_hash(&f->scene.f.irradiance[0]),
                                &wrong, after),
              DE_OK);
    ASSERT_EQ(
        dpak_writer_add(writer, ids[1].hash, "blob", blob, blob_bytes, NULL, 0, "gi/wrong-counts"),
        DE_OK);
    wrong = info.layout;
    wrong.bounces = 2;
    ASSERT_EQ(de__gi_blob_write(blob, blob_bytes, de__gi_settings_hash(&f->scene.f.irradiance[0]),
                                &wrong, after),
              DE_OK);
    ASSERT_EQ(
        dpak_writer_add(writer, ids[2].hash, "blob", blob, blob_bytes, NULL, 0, "gi/wrong-bounces"),
        DE_OK);
    ASSERT_EQ(dpak_writer_finish(writer, "dev:/gi.dpak"), DE_OK);
    dpak_writer_destroy(writer);
    free(blob);
    ASSERT_EQ(de_asset_pack_mount(f->assets, "dev:/gi.dpak", 0), DE_OK);
    f->load = de_asset_load_many(f->assets, ids, 3, 0);
    ASSERT_TRUE(f->load.id);
    ASSERT_EQ(de_asset_wait(f->assets, f->load, 10ull * 1000000000ull), DE_LOAD_READY);
    f->r->assets = f->assets;
    volume->bake = de_asset_blob(f->assets, id);
    ASSERT_TRUE(volume->bake.id);
    de__irradiance_destroy(f->r->irradiance);
    f->r->irradiance = NULL;
    ASSERT_EQ(de__irradiance_create(f->r, &f->r->irradiance), DE_OK);
    room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    EXPECT_TRUE(info.ready && info.baked);
    EXPECT_EQ(info.captures, 0u);
    if (backend == DE_GPU_METAL)
        EXPECT_TRUE(de__irradiance_resource_drains(f->r) > 0);
    EXPECT_EQ(de__views_last_order(f->r, NULL, 0), 0u);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, bytes), DE_OK);
    EXPECT_EQ(memcmp(before, after, (size_t)bytes), 0);
    room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    EXPECT_EQ(info.captures, 0u);
    de_gpu_buffer original_classification = info.classification;
    for (u32 i = 1; i < 3; ++i) {
        volume->bake = de_asset_blob(f->assets, ids[i]);
        ASSERT_TRUE(volume->bake.id);
        room_frame(f);
        ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
        EXPECT_TRUE(info.ready);
        EXPECT_EQ(info.classification.id, original_classification.id);
        EXPECT_EQ(info.captures, 0u);
    }
    volume->bake = de_asset_blob(f->assets, id);
    gi_original_backend = f->dev->vt;
    de__gpu_backend_vtable failing = *gi_original_backend;
    failing.buffer_create = fail_gi_classification;
    gi_failed_classifications = 0;
    f->dev->vt = &failing;
    de__irradiance_prepare(f->r, &f->scene.f);
    f->dev->vt = gi_original_backend;
    de_error_clear();
    EXPECT_EQ(gi_failed_classifications, 1u);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    EXPECT_TRUE(info.ready);
    EXPECT_EQ(info.classification.id, original_classification.id);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, bytes), DE_OK);
    EXPECT_EQ(memcmp(before, after, (size_t)bytes), 0);
    room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    EXPECT_TRUE(info.ready && info.baked);
    EXPECT_EQ(info.captures, 0u);
    EXPECT_NE(info.classification.id, original_classification.id);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, bytes), DE_OK);
    EXPECT_EQ(memcmp(before, after, (size_t)bytes), 0);
    u64 steady_drains = de__irradiance_resource_drains(f->r);
    room_frame(f);
    room_frame(f);
    EXPECT_EQ(de__irradiance_resource_drains(f->r), steady_drains);
    free(before);
    free(after);
}
TEST_F(gi, irradiance_bake_reload_identical_metal, gifx) {
    nonaligned_bake_reload(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_bake_reload_identical_vulkan, gifx) {
    nonaligned_bake_reload(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_bake_reload_identical_d3d12, gifx) {
    nonaligned_bake_reload(f, DE_GPU_D3D12);
}
static void gi_queries(gifx *f, const de__gi_gpu *volumes, u32 volume_count, const f32 *queries,
                       u32 count, f32 *result) {
    de__gi_gpu records[9];
    memset(records, 0, sizeof records);
    records[0].counts[0] = volume_count;
    memcpy(records + 1, volumes, (size_t)volume_count * sizeof *volumes);
    de_gpu_buffer metadata = upload_buf(f->dev, records, sizeof records);
    de_gpu_buffer input = upload_buf(f->dev, queries, (u64)count * 48);
    de_gpu_buffer output = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16,
                                      .stride = 16,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(metadata.id && input.id && output.id && rb.id);
    const char *entry = "cs_gi_sample_reference";
    const u32 group[] = {64, 1, 1};
    de__engine_shader_code code;
    de__vis_shared *vis = de__views_vis(f->r);
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(vis, "irradiance_bake", &entry, 1, &code), DE_OK);
    ASSERT_EQ(de__vis_kernel_create(vis, &code, 0, entry, group, 48, &kernel), DE_OK);
    de__engine_shader_code_free(&code);
    u32 pc[12] = {de_gpu_buffer_bindless(f->dev, metadata),
                  de_gpu_buffer_bindless(f->dev, input),
                  de_gpu_buffer_bindless(f->dev, output),
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  count};
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gi.query");
    ASSERT_TRUE(cmd);
    de_gpu_barrier barrier = {
        .buffer = output, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (count + 63) / 64, 1, 1);
    barrier.before = DE_ACCESS_SHADER_WRITE;
    barrier.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer(cmd, rb, 0, output, 0, (u64)count * 16);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    memcpy(result, de_gpu_buffer_map(f->dev, rb), (size_t)count * 16);
    de__vis_kernel_destroy(vis, &kernel);
    de_gpu_buffer_destroy(f->dev, metadata);
    de_gpu_buffer_destroy(f->dev, input);
    de_gpu_buffer_destroy(f->dev, output);
    de_gpu_buffer_destroy(f->dev, rb);
}
static u32 cpu_packed_component(f32 value, u32 mantissa) {
    if (!isfinite(value))
        value = value > 0.f ? 60000.f : 0.f;
    value = fminf(fmaxf(value, 0), 60000);
    if (value < ldexpf(1.f, -14))
        return 0;
    int e = 0;
    f64 significand = frexp((f64)value, &e) * 2;
    return (u32)(e - 1 + 15) << mantissa | (u32)floor((significand - 1) * (f64)(1u << mantissa));
}
static u32 cpu_normalized_half(f32 value) {
    if (!isfinite(value))
        value = 1.f;
    value = fminf(fmaxf(value, 0.f), 1.f);
    if (value < ldexpf(1.f, -14))
        return (u32)floor(ldexp((f64)value, 24));
    int exponent = 0;
    f64 significand = frexp((f64)value, &exponent) * 2;
    return (u32)(exponent - 1 + 15) << 10 | (u32)floor((significand - 1) * 1024);
}
static void quantizer_boundary(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    f32 values[96][4] = {{0}};
    for (u32 i = 0; i < 90; ++i) {
        f32 boundary = ldexpf(1, (int)(i / 3) - 24);
        u32 bits;
        memcpy(&bits, &boundary, 4);
        bits += (i % 3 == 0 ? 0 : i % 3 == 1 ? 0xffffffffu : 1u);
        f32 value;
        memcpy(&value, &bits, 4);
        values[i][0] = values[i][1] = values[i][2] = value;
    }
    values[90][0] = values[90][1] = values[90][2] = 60000;
    values[91][0] = INFINITY;
    values[91][1] = -INFINITY;
    values[91][2] = NAN;
    values[92][0] = 0x1p-149f;
    values[92][1] = -1;
    values[92][2] = 65000;
    values[93][0] = values[93][1] = values[93][2] = 100000;
    values[94][0] = values[94][1] = values[94][2] = 0.f;
    values[95][0] = values[95][1] = values[95][2] = 1.f;
    de_gpu_buffer input = upload_buf(f->dev, values, sizeof values);
    de_gpu_buffer out = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){.size = 192 * 4, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer readback = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = 192 * 4, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(readback.id);
    ASSERT_TRUE(out.id);
    de__vis_shared *vis = de__views_vis(f->r);
    const char *entry = "cs_gi_quantize";
    const u32 group[] = {64, 1, 1};
    de__engine_shader_code code;
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(vis, "irradiance_bake", &entry, 1, &code), DE_OK);
    ASSERT_EQ(de__vis_kernel_create(vis, &code, 0, entry, group, 48, &kernel), DE_OK);
    de__engine_shader_code_free(&code);
    u32 pc[12] = {de_gpu_buffer_bindless(f->dev, input),
                  0,
                  de_gpu_buffer_bindless(f->dev, out),
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  96};
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gi.quantizer_boundary");
    ASSERT_TRUE(cmd);
    de_gpu_barrier b = {.buffer = out, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, 2, 1, 1);
    b.before = DE_ACCESS_SHADER_WRITE;
    b.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, readback, 0, out, 0, 192 * 4);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    u32 actual[192];
    de_gpu_wait_idle(f->dev);
    memcpy(actual, de_gpu_buffer_map(f->dev, readback), sizeof actual);
    for (u32 i = 0; i < 96; ++i) {
        u32 expected = cpu_packed_component(values[i][0], 6) |
                       cpu_packed_component(values[i][1], 6) << 11 |
                       cpu_packed_component(values[i][2], 5) << 22;
        EXPECT_EQ(actual[i], expected);
        EXPECT_EQ(actual[i + 96],
                  cpu_normalized_half(values[i][0]) | cpu_normalized_half(values[i][1]) << 16);
    }
    de__vis_kernel_destroy(vis, &kernel);
    de_gpu_buffer_destroy(f->dev, input);
    de_gpu_buffer_destroy(f->dev, out);
    de_gpu_buffer_destroy(f->dev, readback);
}
TEST_F(gi, packed_quantizer_boundary_metal, gifx) {
    quantizer_boundary(f, DE_GPU_METAL);
}
TEST_F(gi, packed_quantizer_boundary_vulkan, gifx) {
    quantizer_boundary(f, DE_GPU_VULKAN);
}
TEST_F(gi, packed_quantizer_boundary_d3d12, gifx) {
    quantizer_boundary(f, DE_GPU_D3D12);
}
static const f64 cornell_albedo[6][3] = {{.6, .05, .05}, {.05, .6, .05}, {.2, .2, .2},
                                         {.2, .2, .2},   {0, 0, 0},      {.2, .2, .2}};
static u32 cpu_random(u32 *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}
static de_dvec3 cpu_cosine(de_dvec3 n, f64 u, f64 v) {
    de_dvec3 tangent = fabs(n.z) < .5 ? (de_dvec3){0, 0, 1} : (de_dvec3){1, 0, 0};
    de_dvec3 bitangent = {n.y * tangent.z - n.z * tangent.y, n.z * tangent.x - n.x * tangent.z,
                          n.x * tangent.y - n.y * tangent.x};
    f64 r = sqrt(u), phi = 2 * PI_D * v, z = sqrt(1 - u);
    return (de_dvec3){n.x * z + tangent.x * r * cos(phi) + bitangent.x * r * sin(phi),
                      n.y * z + tangent.y * r * cos(phi) + bitangent.y * r * sin(phi),
                      n.z * z + tangent.z * r * cos(phi) + bitangent.z * r * sin(phi)};
}
static u32 cpu_box_hit(de_dvec3 p, de_dvec3 d, de_dvec3 *hit, de_dvec3 *normal) {
    f64 position[] = {p.x, p.y, p.z}, direction[] = {d.x, d.y, d.z}, t = 1e30;
    u32 face = 0;
    for (u32 axis = 0; axis < 3; ++axis)
        if (fabs(direction[axis]) > 1e-12) {
            f64 candidate = ((direction[axis] > 0 ? 5 : -5) - position[axis]) / direction[axis];
            if (candidate > 1e-6 && candidate < t) {
                t = candidate;
                face = axis * 2 + (direction[axis] < 0 ? 1u : 0u);
            }
        }
    *hit = (de_dvec3){p.x + t * d.x, p.y + t * d.y, p.z + t * d.z};
    *normal = (de_dvec3){0};
    if (face / 2 == 0)
        normal->x = face % 2 ? 1 : -1;
    if (face / 2 == 1)
        normal->y = face % 2 ? 1 : -1;
    if (face / 2 == 2)
        normal->z = face % 2 ? 1 : -1;
    return face;
}
static void cpu_cornell(de_dvec3 p, de_dvec3 normal, u32 bounces, f64 out[3]) {
    enum { SAMPLES = 131072 };
    out[0] = out[1] = out[2] = 0;
    u32 state = 12345;
    for (u32 i = 0; i < SAMPLES; ++i) {
        f64 u = ((f64)cpu_random(&state) + .5) / 4294967296.,
            v = ((f64)cpu_random(&state) + .5) / 4294967296.;
        de_dvec3 direction = cpu_cosine(normal, u, v), hit, n;
        u32 face = cpu_box_hit(p, direction, &hit, &n);
        f64 value[3] = {face == 4 ? 2 : 0, face == 4 ? 2 : 0, face == 4 ? 2 : 0};
        if (bounces > 1 && face != 4) {
            u = ((f64)cpu_random(&state) + .5) / 4294967296.;
            v = ((f64)cpu_random(&state) + .5) / 4294967296.;
            de_dvec3 second_hit, second_normal;
            u32 next = cpu_box_hit(hit, cpu_cosine(n, u, v), &second_hit, &second_normal);
            if (next == 4)
                for (u32 c = 0; c < 3; ++c)
                    value[c] += cornell_albedo[face][c] * 2;
        }
        for (u32 c = 0; c < 3; ++c)
            out[c] += value[c] * PI_D / SAMPLES;
    }
}
static void cornell_reference(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    for (u32 face = 0; face < 6; ++face) {
        g_mat[face].standard.base_color =
            (de_vec4){(f32)cornell_albedo[face][0], (f32)cornell_albedo[face][1],
                      (f32)cornell_albedo[face][2], 1};
        g_mat[face].standard.emissive = face == 4 ? (de_vec3){2, 2, 2} : (de_vec3){0};
        g_mat[face].standard.roughness = 1;
        g_mat[face].standard.specular = 0;
    }
    DeIrradianceVolume *volume = &f->scene.f.irradiance[0].volume;
    volume->extent = (de_vec3){5.2f, 5.2f, 5.2f};
    volume->counts[0] = volume->counts[1] = volume->counts[2] = 7;
    volume->bounces = 2;
    volume->blend_distance = .1f;
    const f32 queries[6][12] = {
        {4.95f, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0}, {-4.95f, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
        {0, 4.95f, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0}, {0, -4.95f, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
        {0, 0, -4.95f, 0, 0, 0, 1, 0, 0, 0, 1, 0},  {3, 0, -4.95f, 0, 0, 0, 1, 0, 0, 0, 1, 0}};
    f32 first[24] = {0};
    for (u32 bounce = 1; bounce <= 2; ++bounce) {
        for (u32 i = 0; i < 343; ++i)
            room_frame(f);
        de__irradiance_info info;
        ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
        EXPECT_EQ(info.bounce, bounce);
        f32 actual[24];
        gi_queries(f, &info.gpu, 1, &queries[0][0], 6, actual);
        f64 worst = 0;
        for (u32 i = 0; i < 6; ++i) {
            f64 reference[3];
            cpu_cornell((de_dvec3){queries[i][0], queries[i][1], queries[i][2]},
                        (de_dvec3){queries[i][4], queries[i][5], queries[i][6]}, bounce, reference);
            EXPECT_FLOAT_EQ(actual[i * 4 + 3], 1, 1e-6);
            printf("Cornell bounce%u wall%u CPU %.6f %.6f %.6f GPU %.6f %.6f %.6f\n", bounce, i,
                   reference[0], reference[1], reference[2], (double)actual[i * 4],
                   (double)actual[i * 4 + 1], (double)actual[i * 4 + 2]);
            for (u32 c = 0; c < 3; ++c) {
                f64 error = fabs(actual[i * 4 + c] - reference[c]) / reference[c];
                worst = fmax(worst, error);
                EXPECT_TRUE(error <= .1);
            }
        }
        printf("Cornell bounce%u worst relative error %.6f\n", bounce, worst);
        if (bounce == 1)
            memcpy(first, actual, sizeof first);
        else {
            EXPECT_TRUE(actual[20] > actual[21]);
            EXPECT_TRUE(actual[20] > first[20]);
        }
    }
}
TEST_F(gi, irradiance_bake_cornell_reference_metal, gifx) {
    cornell_reference(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_bake_cornell_reference_vulkan, gifx) {
    cornell_reference(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_bake_cornell_reference_d3d12, gifx) {
    cornell_reference(f, DE_GPU_D3D12);
}

static void view_read_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    gifx *f = user;
    for (u32 i = 0; i < 5; ++i)
        de_gpu_cmd_copy_texture_to_buffer(cmd, f->view_readback, f->offsets[i], f->pitches[i],
                                          de_rg_texture(pass, f->reads[i]), 0, 0);
}
static void view_read_hook(de_rg *g, de_view view, void *user) {
    gifx *f = user;
    if (f->suppress_readback || view.id != f->view.id)
        return;
    de__vis_context *vc = de__vis_frame_context(de__views_vis(f->r), view, g, f->r->frame_index);
    de__lighting_context *lc =
        de__lighting_frame_context(de__views_lighting(f->r), view, g, f->r->frame_index);
    ASSERT_TRUE(vc && lc && lc->lit);
    f->reads[0] = vc->depth;
    f->common =
        de__scene_history_context(de__ssr_scene_history(f->r->ssr), view, g, f->r->frame_index);
    for (u32 i = 0; i < 3; ++i)
        f->reads[i + 1] = vc->gbuffer[i];
    f->reads[4] = lc->target;
    de_rg_pass *p = de_rg_add_pass(g, "test.gi_view_read", DE_QUEUE_GRAPHICS, view_read_execute, f);
    for (u32 i = 0; i < 5; ++i)
        de_rg_read(p, f->reads[i], DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(g, f->view_readback, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
    // Restore the sampled depth/G-buffer and storage lighting states after the
    // copy, as in lighting_fixture: legacy VVL rechecks bindless descriptors at
    // the end of the recorded list. The readback must not leave TRANSFER_SRC.
    p = de_rg_add_pass(g, "test.gi_view_restore", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_pass_side_effect(p);
    for (u32 i = 0; i < 4; ++i)
        de_rg_read(p, f->reads[i], DE_ACCESS_SHADER_READ);
    de_rg_write(p, f->reads[4], DE_ACCESS_SHADER_WRITE);
}
static void camera_at(gifx *f, de_vec3 eye, de_vec3 target) {
    de_mat4 view = de_m4_look_at(eye, target, (de_vec3){0, 0, 1}),
            inv = de_m4_inverse_affine(&view);
    f->camera.transform = de_affine_from_m4(&inv);
    f->camera.offset = eye;
    de_mat3 rotation;
    memcpy(rotation.m, f->camera.transform.m, sizeof rotation.m);
    f->camera.rotation = de_quat_from_m3(&rotation);
}
static void attach_reference_view(gifx *f) {
    de_gpu_texture target = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = f->width,
                                       .height = f->height,
                                       .format = DE_FMT_RGBA16_FLOAT,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                       .name = "ssr.reference_view"});
    ASSERT_TRUE(DE_HANDLE_VALID(target));
    f->textures[f->texture_count++] = target;
    f->view =
        de_view_create(f->r, &(de_view_desc){.target = target, .name = "ssr.reference_camera"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->view));
    f->camera = (de__rw_view){
        .entity = 100,
        .camera = {
            .view = f->view, .near_z = 0.1f, .fov_y = 1.04719755f, .exposure_ev100 = -0.2630344f}};
    camera_at(f, (de_vec3){0, -6, 3}, (de_vec3){0, 0, 0.8f});
    f->scene.f.views = &f->camera;
    f->scene.f.view_count = 1;
    f->settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    f->settings.render_scale = 1;
    f->settings.ao = f->settings.reflections = f->settings.sss = DE_QUALITY_OFF;
    f->settings.gi_mode = DE_GI_SCREEN_SPACE;
    de_view_set_settings(f->r, f->view, &f->settings);
    u64 total = 0;
    for (u32 i = 0; i < 5; ++i) {
        f->pitches[i] = ((f->width * (i == 4 ? 8 : 4)) + 511u) & ~511u;
        f->offsets[i] = total;
        total += (u64)f->pitches[i] * f->height;
    }
    f->view_readback = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = total, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(DE_HANDLE_VALID(f->view_readback));

    de_render_add_hook(f->r, DE_HOOK_AFTER_TRANSPARENT, view_read_hook, f);
}

static void screen_scene(gifx *f, de_gpu_backend backend, f32 roughness, f32 brightness) {
    open_renderer(f, backend);
    f->width = f->height = 128;
    world_init(&f->scene, 8);
    f->scene_live = true;
    g_mesh_count = 2;
    mesh_plane(&g_mesh[0], f->dev, 1, 1, 12);
    mesh_plane(&g_mesh[1], f->dev, 1, 1, 2);
    u32 floor = material_add(1, roughness, 0, 0);
    g_mat[floor - 1].standard.base_color = (de_vec4){0.2f, 0.5f, 0.8f, 1};
    world_set(&f->scene, 0, 1, de_affine_identity(), floor);
    for (u32 face = 0; face < 6; ++face) {
        u32 mat = material_add(0, 1, 0, 1);
        g_mat[mat - 1].standard.emissive =
            (de_vec3){brightness * 0.3f, brightness * 0.7f, brightness};
        g_mat[mat - 1].standard.double_sided = true;
        de__rw_view face_camera;
        de__probe_face_camera((de_dvec3){0}, (de_dvec3){0}, face, &face_camera);
        de_affine xf = face_camera.transform;
        for (u32 j = 0; j < 3; ++j) {
            xf.m[9 + j] = -xf.m[6 + j];
            xf.m[6 + j] = -xf.m[6 + j];
        }
        xf.m[11] += 1;
        world_set(&f->scene, face + 1, 2, xf, mat);
    }
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
    attach_reference_view(f);
}

static de__ssgi_info ssgi_info(gifx *f) {
    de__ssgi_info info;
    ASSERT_TRUE(de__ssgi_view_info(f->r->ssgi, f->view, &info));
    return info;
}
static void ssgi_emissive(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 1, 4);
    f->settings.gi_mode = DE_GI_STATIC_PROBES;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    room_frame(f);
    de__ssgi_info initial = ssgi_info(f);
    EXPECT_EQ(initial.dispatches, 0u);
    const u8 *raw = de_gpu_buffer_map(f->dev, f->view_readback);
    ASSERT_TRUE(raw);
    u16 baseline[128 * 128 * 4];
    for (u32 y = 0; y < 128; ++y)
        memcpy(baseline + y * 128 * 4, raw + f->offsets[4] + y * f->pitches[4], 128 * 8);
    EXPECT_TRUE(!f->common);
    f->settings.gi_mode = DE_GI_SCREEN_SPACE;
    de_view_set_settings(f->r, f->view, &f->settings);
    u32 coverage = 0;
    f64 energy = 0;
    u16 output[128 * 128 * 4];
    f32 moments[128 * 128 * 4];
    for (u32 frame = 0; frame < 20; ++frame) {
        room_frame(f);
        de__ssgi_info info = ssgi_info(f);
        EXPECT_EQ(info.dispatches, 2u);
        EXPECT_EQ(info.width, 128u);
        EXPECT_EQ(info.trace_width, 64u);
        read_storage_texture(f, info.temporal, 128, 128, 8, output);
        read_storage_texture(f, info.moments, 128, 128, 16, moments);
        u32 count = 0;
        f64 sum = 0;
        for (u32 i = 0; i < 128 * 128; ++i) {
            for (u32 c = 0; c < 4; ++c) {
                EXPECT_TRUE(isfinite(half_to_f32(output[i * 4 + c])));
                EXPECT_TRUE(isfinite(moments[i * 4 + c]));
            }
            if (half_to_f32(output[i * 4 + 3]) > .02f && moments[i * 4 + 2] > 0) {
                ++count;
                sum += half_to_f32(output[i * 4]) + half_to_f32(output[i * 4 + 1]) +
                       half_to_f32(output[i * 4 + 2]);
            }
        }
        printf("SSGI frame%u coverage%u energy%.6f\n", frame, count, sum);
        if (frame == 19) {
            coverage = count;
            energy = sum;
        }
    }
    EXPECT_TRUE(coverage > 100);
    EXPECT_TRUE(energy > 10);
    raw = de_gpu_buffer_map(f->dev, f->view_readback);
    u32 bounced = 0;
    for (u32 y = 0; y < 128; ++y)
        for (u32 x = 0; x < 128; ++x) {
            u32 i = y * 128 + x;
            const u16 *lit = (const u16 *)(raw + f->offsets[4] + y * f->pitches[4]) + x * 4;
            if (half_to_f32(baseline[i * 4 + 2]) < .01f && half_to_f32(lit[2]) > .01f)
                ++bounced;
        }
    EXPECT_TRUE(bounced > 100);
}
TEST_F(gi, ssgi_emissive_on_screen_metal, gifx) {
    ssgi_emissive(f, DE_GPU_METAL);
}
TEST_F(gi, ssgi_emissive_on_screen_vulkan, gifx) {
    ssgi_emissive(f, DE_GPU_VULKAN);
}
TEST_F(gi, ssgi_emissive_on_screen_d3d12, gifx) {
    ssgi_emissive(f, DE_GPU_D3D12);
}
static void thin_wall(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    for (u32 face = 0; face < 6; ++face) {
        g_mat[face].standard.base_color = (de_vec4){0, 0, 0, 1};
        g_mat[face].standard.emissive = face == 0 ? (de_vec3){4, 0, 0} : (de_vec3){0};
    }
    de_affine separator = de_affine_identity();
    separator.m[0] = 0;
    separator.m[2] = -1;
    separator.m[6] = 1;
    separator.m[8] = 0;
    u32 black = material_add(0, 1, 0, 0);
    g_mat[black - 1].standard.double_sided = true;
    world_set(&f->scene, 6, 1, separator, black);
    DeIrradianceVolume *volume = &f->scene.f.irradiance[0].volume;
    volume->extent = (de_vec3){4, 4, 4};
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    de__irradiance_info info;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    ASSERT_TRUE(info.ready);
    const f32 queries[4][12] = {{-.25f, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
                                {.25f, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
                                {-2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
                                {2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
    f32 actual[16];
    gi_queries(f, &info.gpu, 1, &queries[0][0], 4, actual);
    printf("grid centers %.6f %.6f\n", (double)actual[8], (double)actual[12]);
    printf("thin wall dark %.8f lit %.8f\n", (double)actual[0], (double)actual[4]);
    EXPECT_TRUE(actual[4] > 8.f);
    EXPECT_TRUE(actual[0] < .02f);
    EXPECT_TRUE(actual[0] < actual[4] * .01f);
    de_quat rotation = de_quat_axis_angle((de_vec3){0, 0, 1}, .63f);
    de_affine rotate = de_affine_from_trs((de_vec3){0}, rotation, (de_vec3){1, 1, 1});
    for (u32 i = 0; i < 7; ++i) {
        de_affine xf = de_affine_mul(&rotate, &f->scene.f.instances.transform[i]);
        world_set(&f->scene, i, 1, xf, i + 1);
    }
    f->scene.f.irradiance[0].rotation = rotation;
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    ASSERT_TRUE(info.ready);
    f32 rotated[4][12];
    memcpy(rotated, queries, sizeof rotated);
    for (u32 i = 0; i < 4; ++i)
        for (u32 j = 0; j < 3; ++j) {
            de_vec3 v = de_quat_rotate(rotation, (de_vec3){queries[i][j * 4], queries[i][j * 4 + 1],
                                                           queries[i][j * 4 + 2]});
            rotated[i][j * 4] = v.x;
            rotated[i][j * 4 + 1] = v.y;
            rotated[i][j * 4 + 2] = v.z;
        }
    gi_queries(f, &info.gpu, 1, &rotated[0][0], 4, actual);
    printf("rotated thin wall dark %.8f lit %.8f\n", (double)actual[0], (double)actual[4]);
    EXPECT_TRUE(actual[4] > 8.f);
    EXPECT_TRUE(actual[0] < .02f);
    EXPECT_TRUE(actual[0] < actual[4] * .01f);
}
TEST_F(gi, irradiance_thin_wall_visibility_metal, gifx) {
    thin_wall(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_thin_wall_visibility_vulkan, gifx) {
    thin_wall(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_thin_wall_visibility_d3d12, gifx) {
    thin_wall(f, DE_GPU_D3D12);
}
static void ssgi_convergence(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 1, 2);
    enum { PIXELS = 128 * 128 };
    f32 *early = calloc(PIXELS * 4 * 4, sizeof(f32)), *late = calloc(PIXELS * 4 * 4, sizeof(f32)),
        *reference = calloc(PIXELS * 4, sizeof(f32));
    ASSERT_TRUE(early && late && reference);
    u16 image[PIXELS * 4];
    f32 moments[PIXELS * 4];
    for (u32 frame = 0; frame < 48; ++frame) {
        room_frame(f);
        de__ssgi_info info = ssgi_info(f);
        read_storage_texture(f, info.temporal, 128, 128, 8, image);
        for (u32 i = 0; i < PIXELS * 4; ++i) {
            f32 value = half_to_f32(image[i]);
            EXPECT_TRUE(isfinite(value));
            if (frame >= 2 && frame < 6)
                early[(frame - 2) * PIXELS * 4 + i] = value;
            if (frame >= 28 && frame < 32)
                late[(frame - 28) * PIXELS * 4 + i] = value;
            if (frame >= 32)
                reference[i] += value / 16;
        }
    }
    de__ssgi_info info = ssgi_info(f);
    read_storage_texture(f, info.moments, 128, 128, 16, moments);
    u32 samples = 0, history = 0;
    f64 early_error = 0, late_error = 0;
    for (u32 i = 0; i < PIXELS; ++i) {
        if (moments[i * 4 + 2] > 8)
            ++history;
        for (u32 frame = 0; frame < 4; ++frame) {
            u32 at = frame * PIXELS * 4 + i * 4;
            if (early[at + 3] < .02f || late[at + 3] < .02f || reference[i * 4 + 3] < .02f)
                continue;
            ++samples;
            for (u32 c = 0; c < 3; ++c) {
                f64 e = early[at + c] - reference[i * 4 + c],
                    l = late[at + c] - reference[i * 4 + c];
                early_error += e * e;
                late_error += l * l;
            }
        }
    }
    printf("SSGI convergence samples%u history%u early%.8f late%.8f\n", samples, history,
           early_error, late_error);
    EXPECT_TRUE(samples > 1000);
    EXPECT_TRUE(history > 100);
    EXPECT_TRUE(late_error < early_error);
    free(early);
    free(late);
    free(reference);
}
TEST_F(gi, ssgi_temporal_convergence_metal, gifx) {
    ssgi_convergence(f, DE_GPU_METAL);
}
TEST_F(gi, ssgi_temporal_convergence_vulkan, gifx) {
    ssgi_convergence(f, DE_GPU_VULKAN);
}
TEST_F(gi, ssgi_temporal_convergence_d3d12, gifx) {
    ssgi_convergence(f, DE_GPU_D3D12);
}

static void resize_generation(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 0.01f, 1);
    room_frame(f);
    room_frame(f);
    EXPECT_TRUE(ssgi_info(f).history_valid);
    f->settings.gi = DE_QUALITY_MEDIUM;
    f->settings.render_scale = 97.0f / 128.0f;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    de__ssgi_info info = ssgi_info(f);
    EXPECT_EQ(info.width, 97u);
    EXPECT_EQ(info.height, 97u);
    EXPECT_EQ(info.trace_width, 49u);
    EXPECT_FALSE(info.history_valid);
    room_frame(f);
    EXPECT_TRUE(ssgi_info(f).history_valid);
    f->settings.render_scale = 1;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    EXPECT_FALSE(ssgi_info(f).history_valid);
    room_frame(f);
    EXPECT_TRUE(ssgi_info(f).history_valid);
    u64 before_transitions = de__views_resource_drains(f->r);
    for (u32 cycle = 0; cycle < 3; ++cycle) {
        f->settings.render_scale = 97.0f / 128.0f;
        de_view_set_settings(f->r, f->view, &f->settings);
        room_frame(f);
        EXPECT_FALSE(ssgi_info(f).history_valid);
        room_frame(f);
        EXPECT_TRUE(ssgi_info(f).history_valid);
        f->settings.render_scale = 1;
        de_view_set_settings(f->r, f->view, &f->settings);
        room_frame(f);
        EXPECT_FALSE(ssgi_info(f).history_valid);
        room_frame(f);
        EXPECT_TRUE(ssgi_info(f).history_valid);
    }
    if (backend == DE_GPU_METAL)
        EXPECT_TRUE(de__views_resource_drains(f->r) > before_transitions);
    // Warm both persistent-history access variants and their graph placements after resize.
    room_frame(f);
    room_frame(f);
    u64 drains = de__views_resource_drains(f->r);
    for (u32 frame = 0; frame < 4; ++frame)
        room_frame(f);
    EXPECT_EQ(de__views_resource_drains(f->r), drains);
    de_view old = f->view;
    de_view_destroy(f->r, old);
    f->view = de_view_create(
        f->r, &(de_view_desc){.target = f->textures[0], .name = "ssr.recycled_view"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->view));
    EXPECT_NE(old.id, f->view.id);
    f->camera.camera.view = f->view;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    EXPECT_FALSE(ssgi_info(f).history_valid);
    room_frame(f);
    EXPECT_TRUE(ssgi_info(f).history_valid);
}
static void pipeline_history(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 0.25f, 20000);
    f->suppress_readback = true;
    const DeRenderComponents *ids = de__render_components(f->ecs);
    de_builtin_component_ids bi = de_builtin_components(f->ecs);
    for (u32 i = 0; i < f->scene.f.instances.count; ++i) {
        de_entity e;
        de_component components[] = {bi.transform, ids->mesh_renderer};
        ASSERT_EQ(de_entity_spawn_with(f->ecs, components, 2, &e), DE_OK);
        de_transform xf;
        ASSERT_TRUE(
            de_transform_from_affine(&f->scene.f.instances.transform[i], (de_dvec3){0}, &xf));
        *(DeTransform *)de_entity_get(f->ecs, e, bi.transform) = xf;
        DeMeshRenderer *mesh = de_entity_get(f->ecs, e, ids->mesh_renderer);
        mesh->mesh = f->scene.f.instances.mesh[i];
        mesh->layer_mask = 1;
        memcpy(mesh->materials, &f->scene.f.instances.materials[i * DE__RW_MATERIALS],
               sizeof mesh->materials);
    }
    de_entity camera;
    de_component components[] = {bi.transform, ids->camera};
    ASSERT_EQ(de_entity_spawn_with(f->ecs, components, 2, &camera), DE_OK);
    de_transform xf;
    ASSERT_TRUE(de_transform_from_affine(&f->camera.transform, (de_dvec3){0}, &xf));
    *(DeTransform *)de_entity_get(f->ecs, camera, bi.transform) = xf;
    *(DeCamera *)de_entity_get(f->ecs, camera, ids->camera) = f->camera.camera;
    f->r->render_world->last = NULL;
    f->r->render_world->current = NULL;

    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "1"), DE_OK);
    for (u32 frame = 0; frame < 12; ++frame) {
        ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
        ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
        // Fake asset handles bypass the asset loader; provide the loaded mesh bounds that
        // production Extract normally obtains from de_render_mesh_get.
        de__rw_instances *instances = &f->r->render_world->filling->instances;
        for (u32 i = 0; i < instances->count; ++i) {
            const de_render_mesh *mesh = fake_mesh(NULL, instances->mesh[i]);
            if (!mesh)
                continue;
            de_aabb bounds = {{mesh->bounds_min[0], mesh->bounds_min[1], mesh->bounds_min[2]},
                              {mesh->bounds_max[0], mesh->bounds_max[1], mesh->bounds_max[2]}};
            instances->bounds[i] = de_aabb_transform(bounds, &instances->transform[i]);
            instances->flags[i] |= DE__RW_INST_HAS_BOUNDS;
        }
        ASSERT_EQ(de__render_extract_and_kick(f->r, f->ecs), DE_OK);
    }
    de__render_world_wait_idle(f->r);
    de_gpu_wait_idle(f->dev);
    EXPECT_EQ(de__render_world_rendered(f->r), 12u);
    de__ssgi_info info = ssgi_info(f);
    EXPECT_TRUE(info.history_valid);
    EXPECT_EQ(info.dispatches, 2u);
    size_t count = (size_t)f->width * f->height;
    f32 *moments = malloc(count * 16);
    ASSERT_TRUE(moments);
    read_storage_texture(f, info.moments, f->width, f->height, 16, moments);
    u32 accumulated = 0, bright = 0;
    for (size_t i = 0; i < count; ++i) {
        for (u32 c = 0; c < 4; ++c)
            EXPECT_TRUE(isfinite(moments[i * 4 + c]));
        if (moments[i * 4 + 1] > 65504)
            ++bright;
        if (moments[i * 4 + 2] > 4)
            ++accumulated;
    }
    EXPECT_TRUE(accumulated > 100);
    EXPECT_TRUE(bright > 100);
    free(moments);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
}

#define GI_LIFECYCLE_TESTS(suffix, backend)                                                        \
    TEST_F(gi, ssgi_resize_view_generation_##suffix, gifx) {                                       \
        resize_generation(f, backend);                                                             \
    }                                                                                              \
    TEST_F(gi, ssgi_pipelined_history_##suffix, gifx) {                                            \
        pipeline_history(f, backend);                                                              \
    }
GI_LIFECYCLE_TESTS(metal, DE_GPU_METAL)
GI_LIFECYCLE_TESTS(vulkan, DE_GPU_VULKAN)
GI_LIFECYCLE_TESTS(d3d12, DE_GPU_D3D12)

TEST_F(gi, settings_component_extract, gifx) {
    open_renderer(f, DE_GPU_NULL);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    EXPECT_EQ(settings.gi_mode, DE_GI_SCREEN_SPACE);
    de_render_settings other = settings;
    other.gi_mode = DE_GI_STATIC_PROBES;
    EXPECT_FALSE(de__render_settings_equal(&settings, &other));
    settings.gi_mode = DE_GI_RT;
    de__render_settings_clamp(&f->r->caps, &f->r->report, &settings);
    EXPECT_EQ(settings.gi_mode, DE_GI_SCREEN_SPACE);
    settings.gi_mode = (de_gi_mode)99;
    de__render_settings_clamp(&f->r->caps, &f->r->report, &settings);
    EXPECT_EQ(settings.gi_mode, DE_GI_STATIC_PROBES);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.gi_mode"), "static_probes"), DE_OK);
    EXPECT_TRUE(de__render_cvars_sync(f->r));
    EXPECT_EQ(f->r->settings.gi_mode, DE_GI_STATIC_PROBES);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.gi_mode"), "screen_space"), DE_OK);
    EXPECT_TRUE(de__render_cvars_sync(f->r));
    EXPECT_EQ(f->r->settings.gi_mode, DE_GI_SCREEN_SPACE);
    const DeRenderComponents *ids = de__render_components(f->ecs);
    de_builtin_component_ids bi = de_builtin_components(f->ecs);
    const de_type_desc *type = de_type_get(f->types, de_type_find(f->types, "DeIrradianceVolume"));
    ASSERT_TRUE(type);
    EXPECT_EQ(type->size, sizeof(DeIrradianceVolume));
    EXPECT_EQ(type->field_count, 8u);
    de_entity highest = {0};
    for (u32 i = 0; i < 10; ++i) {
        de_entity entity;
        de_component components[] = {bi.transform, ids->irradiance_volume};
        ASSERT_EQ(de_entity_spawn_with(f->ecs, components, 2, &entity), DE_OK);
        DeIrradianceVolume *volume = de_entity_get(f->ecs, entity, ids->irradiance_volume);
        EXPECT_FLOAT_EQ(volume->spacing, 2, 0);
        EXPECT_FLOAT_EQ(volume->intensity, 1, 0);
        EXPECT_EQ(volume->bounces, 2u);
        if (i == 9) {
            volume->priority = 100;
            highest = entity;
        }
    }
    ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->ecs, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    const de__render_world_frame *frame = f->r->render_world->filling;
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->irradiance_count, 8u);
    EXPECT_EQ(frame->irradiance[0].entity, highest.bits);
    for (u32 i = 2; i < 8; ++i)
        EXPECT_TRUE(frame->irradiance[i - 1].entity < frame->irradiance[i].entity);
}
static de_result fail_gi_texture(de_gpu_device *dev, de__gpu_texture *texture,
                                 const de_gpu_texture_desc *desc) {
    (void)dev;
    (void)texture;
    (void)desc;
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected GI atlas failure");
}
static const char *gi_failed_buffer_name;
static u32 gi_failed_buffers;
static de_result fail_gi_named_buffer(de_gpu_device *dev, de__gpu_buffer *buffer,
                                      const de_gpu_buffer_desc *desc) {
    if (desc->name && strcmp(desc->name, gi_failed_buffer_name) == 0) {
        ++gi_failed_buffers;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected GI named buffer failure");
    }
    return gi_original_backend->buffer_create(dev, buffer, desc);
}
static u32 gi_reported_submit_failures;
static de_result fail_gi_reported_submit(de_gpu_device *dev, de_gpu_queue queue,
                                         const de_gpu_submit *submit, u64 value) {
    de_result result = gi_original_backend->queue_submit(dev, queue, submit, value);
    if (result == DE_OK && submit->cmd_count == 1 &&
        strcmp(submit->cmds[0]->name, "gi.integrate") == 0) {
        // Execute normally so the backend's internal timeline still completes,
        // but report failure: publication must be conservative even if GPU work ran.
        ++gi_reported_submit_failures;
        return DE_FAIL(DE_ERR_INTERNAL, "injected GI reported submission failure");
    }
    return result;
}
static void allocation_retry(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    de__irradiance_info old;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &old));
    ASSERT_TRUE(old.ready);
    void *payload = malloc((size_t)old.layout.payload_bytes);
    ASSERT_TRUE(payload);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, payload, old.layout.payload_bytes), DE_OK);
    f->scene.f.irradiance[0].volume.extent.x = 2;
    const de__gpu_backend_vtable *original = f->dev->vt;
    de__gpu_backend_vtable failing = *original;
    failing.texture_create = fail_gi_texture;
    f->dev->vt = &failing;
    de__irradiance_prepare(f->r, &f->scene.f);
    f->dev->vt = original;
    de_error_clear();
    de__irradiance_info preserved;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &preserved));
    EXPECT_TRUE(preserved.ready);
    EXPECT_EQ(preserved.irradiance.id, old.irradiance.id);
    void *after = malloc((size_t)old.layout.payload_bytes);
    ASSERT_TRUE(after);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, old.layout.payload_bytes), DE_OK);
    EXPECT_EQ(memcmp(payload, after, (size_t)old.layout.payload_bytes), 0);
    // Failure after atlas/classification creation must preserve the ready record.
    gi_original_backend = original;
    failing = *original;
    failing.buffer_create = fail_gi_named_buffer;
    gi_failed_buffer_name = "gi.initialize_upload";
    gi_failed_buffers = 0;
    f->dev->vt = &failing;
    de__irradiance_prepare(f->r, &f->scene.f);
    f->dev->vt = original;
    de_error_clear();
    EXPECT_EQ(gi_failed_buffers, 1u);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &preserved));
    EXPECT_TRUE(preserved.ready);
    EXPECT_EQ(preserved.irradiance.id, old.irradiance.id);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, old.layout.payload_bytes), DE_OK);
    EXPECT_EQ(memcmp(payload, after, (size_t)old.layout.payload_bytes), 0);
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &preserved));
    EXPECT_TRUE(preserved.ready);
    EXPECT_NE(preserved.irradiance.id, old.irradiance.id);
    old = preserved;
    ASSERT_EQ(de__irradiance_readback(f->r, 77, payload, old.layout.payload_bytes), DE_OK);
    // A second, larger volume requires scratch growth, but may not damage the
    // first ready volume or publish a partially integrated probe on OOM.
    f->scene.f.irradiance_count = 2;
    f->scene.f.irradiance[1] = f->scene.f.irradiance[0];
    f->scene.f.irradiance[1].entity = 88;
    for (u32 axis = 0; axis < 3; ++axis)
        f->scene.f.irradiance[1].volume.counts[axis] = 3;
    gi_failed_buffer_name = "gi.convolution";
    gi_failed_buffers = 0;
    f->dev->vt = &failing;
    room_frame(f);
    f->dev->vt = original;
    de_error_clear();
    EXPECT_EQ(gi_failed_buffers, 1u);
    de__irradiance_info pending;
    ASSERT_TRUE(de__irradiance_get(f->r, 88, &pending));
    EXPECT_FALSE(pending.ready);
    EXPECT_EQ(pending.probe, 0u);
    EXPECT_EQ(pending.bounce, 0u);
    EXPECT_EQ(pending.faces, 63u);
    EXPECT_EQ(pending.captures, 1u);
    EXPECT_EQ(pending.convolution.id, old.convolution.id);
    EXPECT_EQ(pending.convolution_capacity, old.convolution_capacity);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, old.layout.payload_bytes), DE_OK);
    EXPECT_EQ(memcmp(payload, after, (size_t)old.layout.payload_bytes), 0);
    room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 88, &pending));
    EXPECT_EQ(pending.probe, 1u);
    EXPECT_EQ(pending.captures, 1u); // integration retry skips recapture
    EXPECT_EQ(pending.faces, 0u);
    EXPECT_TRUE(pending.convolution_capacity > old.convolution_capacity);
    EXPECT_TRUE(pending.convolution_capacity <= 8ull * 1024 * 1024);
    const de_gpu_texture unpublished = pending.irradiance;
    failing = *original;
    failing.queue_submit = fail_gi_reported_submit;
    gi_reported_submit_failures = 0;
    f->dev->vt = &failing;
    room_frame(f);
    f->dev->vt = original;
    de_error_clear();
    EXPECT_EQ(gi_reported_submit_failures, 1u);
    ASSERT_TRUE(de__irradiance_get(f->r, 88, &pending));
    EXPECT_FALSE(pending.ready);
    EXPECT_EQ(pending.irradiance.id, unpublished.id);
    EXPECT_EQ(pending.probe, 1u);
    EXPECT_EQ(pending.bounce, 0u);
    EXPECT_EQ(pending.faces, 63u);
    EXPECT_EQ(pending.captures, 2u);
    room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 88, &pending));
    EXPECT_EQ(pending.probe, 2u);
    EXPECT_EQ(pending.captures, 2u);
    EXPECT_EQ(pending.faces, 0u);
    for (u32 i = 0; i < 25; ++i)
        room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 88, &pending));
    EXPECT_TRUE(pending.ready);
    EXPECT_EQ(pending.captures, 27u);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, after, old.layout.payload_bytes), DE_OK);
    EXPECT_EQ(memcmp(payload, after, (size_t)old.layout.payload_bytes), 0);
    free(payload);
    free(after);
}
TEST_F(gi, irradiance_allocation_failure_retry_metal, gifx) {
    allocation_retry(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_allocation_failure_retry_vulkan, gifx) {
    allocation_retry(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_allocation_failure_retry_d3d12, gifx) {
    allocation_retry(f, DE_GPU_D3D12);
}

static void ao_stage(de__view_frame *vf, void *user) {
    gifx *f = user;
    if (vf->view.id != f->view.id)
        return;
    de__lighting_inputs *in =
        de__lighting_view_inputs(de__views_lighting(f->r), (vf->view.id & 255u) - 1);
    in->ao = de_rg_import_texture(vf->graph, f->ao, DE_ACCESS_SHADER_READ);
}
static void static_material_exposure(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    for (u32 i = 0; i < 6; ++i) {
        g_mat[i].standard.base_color = (de_vec4){.2f, .5f, .8f, 1};
        g_mat[i].standard.specular = 0;
        g_mat[i].standard.roughness = 1;
    }
    f->scene.f.irradiance[0].volume.extent = (de_vec3){5.2f, 5.2f, 5.2f};
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    de__irradiance_info info;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    ASSERT_TRUE(info.ready);
    f->width = f->height = 128;
    attach_reference_view(f);
    camera_at(f, (de_vec3){0, 0, 0}, (de_vec3){5, 0, 0});
    f->settings.gi_mode = DE_GI_STATIC_PROBES;
    de_view_set_settings(f->r, f->view, &f->settings);
    f32 e[] = {quantize_unsigned_float((f32)PI_D, 6), quantize_unsigned_float(2 * (f32)PI_D, 6),
               quantize_unsigned_float(3 * (f32)PI_D, 5)};
    for (u32 ev = 0; ev < 2; ++ev) {
        room_frame(f);
        const u8 *data = de_gpu_buffer_map(f->dev, f->view_readback);
        ASSERT_TRUE(data);
        u32 checked = 0;
        for (u32 y = 16; y < 112; ++y)
            for (u32 x = 16; x < 112; ++x) {
                u32 gb;
                memcpy(&gb, data + f->offsets[1] + y * f->pitches[1] + x * 4, 4);
                const u16 *lit = (const u16 *)(data + f->offsets[4] + y * f->pitches[4]) + x * 4;
                for (u32 c = 0; c < 3; ++c) {
                    f32 base = de__gb_srgb_decode((f32)((gb >> (c * 8)) & 255u) / 255);
                    f32 expected = ((f32)(c + 1) + e[c] * base / (f32)PI_D) * (ev ? .25f : 1);
                    EXPECT_FLOAT_EQ(half_to_f32(lit[c]), expected, .006f);
                }
                ++checked;
            }
        EXPECT_TRUE(checked > 1000);
        EXPECT_EQ(ssgi_info(f).dispatches, 0u);
        EXPECT_TRUE(!f->common);
        f->camera.camera.exposure_ev100 += 2;
    }
    f->camera.camera.exposure_ev100 = -.2630344f;
    u32 transparent = material_add(1, 1, 0, 0);
    g_mat[transparent - 1].alpha_mode = DE_COOKED_ALPHA_BLEND;
    g_mat[transparent - 1].standard.alpha_mode = DE_COOKED_ALPHA_BLEND;
    g_mat[transparent - 1].standard.base_color = (de_vec4){.7f, .1f, .05f, .5f};
    de_affine xf = f->scene.f.instances.transform[0];
    xf.m[9] = 4.9f;
    world_set(&f->scene, 6, 1, xf, transparent);
    room_frame(f);
    const u8 *data = de_gpu_buffer_map(f->dev, f->view_readback);
    const f32 own[] = {.7f, .1f, .05f};
    for (u32 y = 32; y < 96; ++y)
        for (u32 x = 32; x < 96; ++x) {
            u32 gb;
            memcpy(&gb, data + f->offsets[1] + y * f->pitches[1] + x * 4, 4);
            const u16 *lit = (const u16 *)(data + f->offsets[4] + y * f->pitches[4]) + x * 4;
            for (u32 c = 0; c < 3; ++c) {
                f32 base = de__gb_srgb_decode((f32)((gb >> (c * 8)) & 255u) / 255);
                f32 expected = .5f * ((f32)(c + 1) + e[c] * (base + own[c]) / (f32)PI_D);
                EXPECT_FLOAT_EQ(half_to_f32(lit[c]), expected, .006f);
            }
        }
    f32 ao[128 * 128];
    for (u32 i = 0; i < 128 * 128; ++i)
        ao[i] = .5f;
    de_gpu_buffer staging = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = sizeof ao, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    ASSERT_TRUE(staging.id);
    void *ao_map = de_gpu_buffer_map(f->dev, staging);
    ASSERT_TRUE(ao_map);
    memcpy(ao_map, ao, sizeof ao);
    f->ao = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = 128,
                                       .height = 128,
                                       .format = DE_FMT_R32_FLOAT,
                                       .usage = DE_USAGE_COPY_DST | DE_USAGE_SAMPLED});
    ASSERT_TRUE(f->ao.id);
    f->textures[f->texture_count++] = f->ao;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gi.test_ao");
    ASSERT_TRUE(cmd);
    de_gpu_barrier barrier = {
        .texture = f->ao, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, f->ao, 0, 0, staging, 0, 128 * 4);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_buffer_destroy(f->dev, staging);
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_LIGHTING, -1, "test.gi_ao", ao_stage, f),
              DE_OK);
    room_frame(f);
    data = de_gpu_buffer_map(f->dev, f->view_readback);
    for (u32 y = 32; y < 96; ++y)
        for (u32 x = 32; x < 96; ++x) {
            u32 gb;
            memcpy(&gb, data + f->offsets[1] + y * f->pitches[1] + x * 4, 4);
            const u16 *lit = (const u16 *)(data + f->offsets[4] + y * f->pitches[4]) + x * 4;
            for (u32 c = 0; c < 3; ++c) {
                f32 base = de__gb_srgb_decode((f32)((gb >> (c * 8)) & 255u) / 255);
                f32 expected = .5f * (f32)(c + 1) + .25f * e[c] * (base + own[c]) / (f32)PI_D;
                EXPECT_FLOAT_EQ(half_to_f32(lit[c]), expected, .006f);
            }
        }
}
TEST_F(gi, irradiance_static_material_exposure_metal, gifx) {
    static_material_exposure(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_static_material_exposure_vulkan, gifx) {
    static_material_exposure(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_static_material_exposure_d3d12, gifx) {
    static_material_exposure(f, DE_GPU_D3D12);
}

static void large_world(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    de__irradiance_info info;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    u64 bytes = info.layout.payload_bytes;
    void *a = malloc((size_t)bytes), *b = malloc((size_t)bytes);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, a, bytes), DE_OK);
    f->scene.f.origin = (de_dvec3){1e6, -2e6, 3e6};
    f->scene.f.irradiance[0].position = f->scene.f.origin;
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    EXPECT_TRUE(info.ready);
    ASSERT_EQ(de__irradiance_readback(f->r, 77, b, bytes), DE_OK);
    EXPECT_EQ(memcmp(a, b, (size_t)bytes), 0);
    const f32 query[12] = {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    f32 result[4];
    gi_queries(f, &info.gpu, 1, query, 1, result);
    EXPECT_FLOAT_EQ(result[0], quantize_unsigned_float((f32)PI_D, 6), 1e-5f);
    EXPECT_FLOAT_EQ(result[3], 1, 0);
    free(a);
    free(b);
}
TEST_F(gi, irradiance_large_world_origin_metal, gifx) {
    large_world(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_large_world_origin_vulkan, gifx) {
    large_world(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_large_world_origin_d3d12, gifx) {
    large_world(f, DE_GPU_D3D12);
}
static void continuity_blend(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    constant_room(f);
    f->scene.f.irradiance_count = 2;
    f->scene.f.irradiance[0].volume.blend_distance = .5f;
    f->scene.f.irradiance[1] = f->scene.f.irradiance[0];
    f->scene.f.irradiance[1].entity = 78;
    f->scene.f.irradiance[1].volume.intensity = 2;
    f->scene.f.irradiance[1].volume.priority = 10;
    for (u32 i = 0; i < 16; ++i)
        room_frame(f);
    de__irradiance_info low, high;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &low));
    ASSERT_TRUE(de__irradiance_get(f->r, 78, &high));
    ASSERT_TRUE(low.ready && high.ready);
    de__gi_gpu volumes[] = {high.gpu, low.gpu};
    volumes[0].local[0][3] = -.5f;
    enum { N = 401 };
    f32 queries[N][12] = {{0}}, result[N * 4];
    for (u32 i = 0; i < N; ++i) {
        queries[i][0] = -2 + (f32)i * .01f;
        queries[i][6] = 1;
        queries[i][10] = 1;
    }
    gi_queries(f, volumes, 2, &queries[0][0], N, result);
    f32 e = quantize_unsigned_float((f32)PI_D, 6);
    for (u32 i = 0; i < N; ++i) {
        f32 x = queries[i][0], a = fminf(fmaxf((1 - fabsf(x - .5f)) / .5f, 0), 1),
            b = fminf(fmaxf((1 - fabsf(x)) / .5f, 0), 1);
        EXPECT_FLOAT_EQ(result[i * 4], e * (2 * a + b * (1 - a)), 2e-5f);
        EXPECT_FLOAT_EQ(result[i * 4 + 3], a + b * (1 - a), 2e-6f);
        if (i)
            EXPECT_TRUE(fabsf(result[i * 4] - result[(i - 1) * 4]) < .13f);
    }
    // Every folded-oct seam and grid corner sees the same constant environment.
    f32 seam[20][12] = {{0}}, out[80];
    for (u32 i = 0; i < 20; ++i) {
        f32 angle = (f32)i * .314159265f;
        seam[i][0] = (i & 1) ? -.49999f : .49999f;
        seam[i][4] = cosf(angle);
        seam[i][5] = sinf(angle);
        seam[i][6] = (i & 2) ? -.001f : .001f;
        seam[i][10] = 1;
    }
    gi_queries(f, &low.gpu, 1, &seam[0][0], 20, out);
    for (u32 i = 0; i < 20; ++i)
        EXPECT_FLOAT_EQ(out[i * 4], e, 2e-5f);
}
TEST_F(gi, irradiance_cell_volume_oct_continuity_metal, gifx) {
    continuity_blend(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_cell_volume_oct_continuity_vulkan, gifx) {
    continuity_blend(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_cell_volume_oct_continuity_d3d12, gifx) {
    continuity_blend(f, DE_GPU_D3D12);
}

static void before_forward_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    gifx *f = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->pre_readback, 0, f->pitches[4],
                                      de_rg_texture(pass, f->pre_source), 0, 0);
}
static void before_forward_hook(de_rg *g, de_view view, void *user) {
    gifx *f = user;
    if (f->suppress_readback || view.id != f->view.id)
        return;
    de__lighting_context *lc =
        de__lighting_frame_context(de__views_lighting(f->r), view, g, f->r->frame_index);
    ASSERT_TRUE(lc && lc->lit);
    f->pre_source = lc->target;
    de_rg_pass *p =
        de_rg_add_pass(g, "test.ssr_before_forward", DE_QUEUE_GRAPHICS, before_forward_execute, f);
    de_rg_read(p, f->pre_source, DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(g, f->pre_readback, DE_ACCESS_COPY_DST), DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
}
static void forward_preserved(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 0.01f, 1);
    u32 mat = material_add(1, 0.01f, 0, 1);
    g_mat[mat - 1].alpha_mode = DE_COOKED_ALPHA_BLEND;
    g_mat[mat - 1].standard.alpha_mode = DE_COOKED_ALPHA_BLEND;
    g_mat[mat - 1].standard.base_color = (de_vec4){0.7f, 0.1f, 0.05f, 0.5f};
    world_set(&f->scene, 7, 1, at(0, 0, 0.02f), mat);
    f->pre_readback =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)f->pitches[4] * f->height,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(DE_HANDLE_VALID(f->pre_readback));

    de_render_add_hook(f->r, DE_HOOK_AFTER_LIGHTING, before_forward_hook, f);
    room_frame(f);
    room_frame(f);
    size_t count = (size_t)f->width * f->height;
    u16 *opaque = malloc(count * 8), *transparent = malloc(count * 8), *ssr = malloc(count * 8);
    ASSERT_TRUE(opaque && transparent && ssr);
    const u8 *pre = de_gpu_buffer_map(f->dev, f->pre_readback),
             *post = de_gpu_buffer_map(f->dev, f->view_readback);
    for (u32 y = 0; y < f->height; ++y) {
        memcpy(opaque + (size_t)y * f->width * 4, pre + (size_t)y * f->pitches[4], f->width * 8);
        memcpy(transparent + (size_t)y * f->width * 4,
               post + f->offsets[4] + (size_t)y * f->pitches[4], f->width * 8);
    }
    read_storage_texture(f, ssgi_info(f).temporal, f->width, f->height, 8, ssr);
    f->settings.gi_mode = DE_GI_STATIC_PROBES;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    pre = de_gpu_buffer_map(f->dev, f->pre_readback);
    post = de_gpu_buffer_map(f->dev, f->view_readback);
    u32 selected = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            size_t i = (size_t)y * f->width + x;
            if (half_to_f32(ssr[i * 4 + 3]) < 0.02f)
                continue;
            const u16 *a = (const u16 *)(pre + (size_t)y * f->pitches[4]),
                      *b = (const u16 *)(post + f->offsets[4] + (size_t)y * f->pitches[4]);
            f32 opaque_delta = half_to_f32(opaque[i * 4 + 2]) - half_to_f32(a[x * 4 + 2]);
            if (opaque_delta < 0.1f)
                continue;
            ++selected;
            f32 transparent_delta = half_to_f32(transparent[i * 4 + 2]) - half_to_f32(b[x * 4 + 2]);
            EXPECT_FLOAT_EQ(transparent_delta, opaque_delta * 0.5f, 0.004f);
        }
    EXPECT_TRUE(selected > 100);
    free(opaque);
    free(transparent);
    free(ssr);
}

TEST_F(gi, ssgi_forward_preserves_fallback_metal, gifx) {
    forward_preserved(f, DE_GPU_METAL);
}
TEST_F(gi, ssgi_forward_preserves_fallback_vulkan, gifx) {
    forward_preserved(f, DE_GPU_VULKAN);
}
TEST_F(gi, ssgi_forward_preserves_fallback_d3d12, gifx) {
    forward_preserved(f, DE_GPU_D3D12);
}

static void ssgi_material_exposure(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 1, 10);
    room_frame(f);
    // First frame has no scene history: zero-coverage history must remain zero energy when
    // the next frame first gains ray hits (the temporal clipping regression).
    u16 zero[128 * 128 * 4];
    read_storage_texture(f, ssgi_info(f).temporal, 128, 128, 8, zero);
    for (u32 i = 0; i < 128 * 128 * 4; ++i)
        EXPECT_EQ(zero[i], 0u);
    u16 result[128 * 128 * 4];
    const f32 incident[] = {3, 7, 10};
    for (u32 ev = 0; ev < 2; ++ev) {
        room_frame(f);
        de__ssgi_info info = ssgi_info(f);
        EXPECT_TRUE(info.history_valid);
        read_storage_texture(f, info.temporal, 128, 128, 8, result);
        f32 rays[64 * 64 * 4];
        read_texture_access(f, info.rays, 64, 64, 16, rays, DE_ACCESS_SHADER_READ);
        f32 minray = INFINITY, maxray = 0;
        for (u32 i = 0; i < 4096; ++i)
            if (rays[i * 4 + 3] > 0) {
                minray = fminf(minray, rays[i * 4 + 2]);
                maxray = fmaxf(maxray, rays[i * 4 + 2]);
            }
        printf("incident ev%u min%.6f max%.6f exposure%.6f prev%.6f\n", ev, (double)minray,
               (double)maxray, (double)info.current_exposure, (double)info.previous_exposure);
        const u8 *data = de_gpu_buffer_map(f->dev, f->view_readback);
        u32 checked = 0;
        for (u32 y = 0; y < 128; ++y)
            for (u32 x = 0; x < 128; ++x) {
                u32 i = y * 128 + x;
                if (half_to_f32(result[i * 4 + 3]) < .02f || half_to_f32(result[i * 4 + 2]) < .1f)
                    continue;
                u32 gb;
                memcpy(&gb, data + f->offsets[1] + y * f->pitches[1] + x * 4, 4);
                const u16 *lit = (const u16 *)(data + f->offsets[4] + y * f->pitches[4]) + x * 4;
                f32 base[3], normal[3], roughness;
                u32 g1;
                memcpy(&g1, data + f->offsets[2] + y * f->pitches[2] + x * 4, 4);
                de__gb1_unpack(g1, normal, &roughness);
                f32 z;
                memcpy(&z, data + f->offsets[0] + y * f->pitches[0] + x * 4, 4);
                f64 sum[3] = {0}, weight = 0;
                for (i32 dy = -1; dy <= 1; ++dy)
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        i32 rx = (i32)(x / 2) + dx, ry = (i32)(y / 2) + dy;
                        if (rx < 0 || ry < 0 || rx >= 64 || ry >= 64)
                            continue;
                        u32 sx = (u32)rx * 2 + 1, sy = (u32)ry * 2 + 1, sg;
                        f32 sz, sn[3], sr;
                        memcpy(&sz, data + f->offsets[0] + sy * f->pitches[0] + sx * 4, 4);
                        memcpy(&sg, data + f->offsets[2] + sy * f->pitches[2] + sx * 4, 4);
                        de__gb1_unpack(sg, sn, &sr);
                        f32 dot = normal[0] * sn[0] + normal[1] * sn[1] + normal[2] * sn[2];
                        if (dot < .85f || fabsf(sz - z) > fmaxf(z * .05f, 1e-4f))
                            continue;
                        const f32 *ray = rays + ((u32)ry * 64 + (u32)rx) * 4;
                        f64 w = pow(fminf(fmaxf(dot, 0), 1), 16) * exp(-(dx * dx + dy * dy) * .5) *
                                ray[3];
                        weight += w;
                        for (u32 c = 0; c < 3; ++c)
                            sum[c] += ray[c] * w;
                    }
                for (u32 c = 0; c < 3; ++c) {
                    base[c] = de__gb_srgb_decode((f32)((gb >> (c * 8)) & 255u) / 255);
                    if (ev == 0) {
                        f32 expected = weight > 1e-8 ? (f32)(sum[c] / weight) * base[c] : 0;
                        EXPECT_FLOAT_EQ(half_to_f32(result[i * 4 + c]), expected,
                                        expected * .003f + .003f);
                    }
                    EXPECT_FLOAT_EQ(half_to_f32(lit[c]),
                                    half_to_f32(result[i * 4 + c]) *
                                        half_to_f32(result[i * 4 + 3]) * (ev ? .25f : 1),
                                    half_to_f32(result[i * 4 + c]) * .003f + .003f);
                }
                f32 scale = half_to_f32(result[i * 4 + 2]) / (incident[2] * base[2]);
                for (u32 c = 0; c < 2; ++c)
                    EXPECT_FLOAT_EQ(half_to_f32(result[i * 4 + c]), incident[c] * base[c] * scale,
                                    .02f);
                ++checked;
            }
        printf("SSGI independent material/exposure ev%u pixels%u\n", ev, checked);
        EXPECT_TRUE(checked > 100);
        if (ev)
            EXPECT_FLOAT_EQ(info.current_exposure / info.previous_exposure, .25f, 1e-5f);
        f->camera.camera.exposure_ev100 += 2;
    }
}
TEST_F(gi, ssgi_material_exposure_once_metal, gifx) {
    ssgi_material_exposure(f, DE_GPU_METAL);
}
TEST_F(gi, ssgi_material_exposure_once_vulkan, gifx) {
    ssgi_material_exposure(f, DE_GPU_VULKAN);
}
TEST_F(gi, ssgi_material_exposure_once_d3d12, gifx) {
    ssgi_material_exposure(f, DE_GPU_D3D12);
}
static void static_mode_restore(gifx *f, de_gpu_backend backend) {
    screen_scene(f, backend, 1, 4);
    f->settings.gi_mode = DE_GI_STATIC_PROBES;
    de_view_set_settings(f->r, f->view, &f->settings);
    f->scene.f.irradiance_count = 1;
    f->scene.f.irradiance[0] = (de__irradiance_snapshot){.entity = 77,
                                                         .rotation = {0, 0, 0, 1},
                                                         .volume = {.extent = {8, 8, 8},
                                                                    .counts = {2, 2, 2},
                                                                    .intensity = 1,
                                                                    .blend_distance = 1,
                                                                    .bounces = 1}};
    for (u32 i = 0; i < 10; ++i)
        room_frame(f);
    de__irradiance_info volume;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &volume));
    ASSERT_TRUE(volume.ready);
    u16 baseline[128 * 128 * 4];
    const u8 *data = de_gpu_buffer_map(f->dev, f->view_readback);
    u32 nonzero = 0;
    for (u32 y = 0; y < 128; ++y) {
        memcpy(baseline + y * 128 * 4, data + f->offsets[4] + y * f->pitches[4], 128 * 8);
        for (u32 x = 0; x < 128; ++x)
            if (half_to_f32(baseline[(y * 128 + x) * 4 + 2]) > .001f)
                ++nonzero;
    }
    EXPECT_TRUE(nonzero > 100);
    EXPECT_EQ(ssgi_info(f).dispatches, 0u);
    EXPECT_TRUE(!f->common);
    f->settings.gi_mode = DE_GI_SCREEN_SPACE;
    de_view_set_settings(f->r, f->view, &f->settings);
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    EXPECT_EQ(ssgi_info(f).dispatches, 2u);
    f->settings.gi_mode = DE_GI_STATIC_PROBES;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    room_frame(f);
    data = de_gpu_buffer_map(f->dev, f->view_readback);
    for (u32 y = 0; y < 128; ++y)
        EXPECT_EQ(memcmp(baseline + y * 128 * 4, data + f->offsets[4] + y * f->pitches[4], 128 * 8),
                  0);
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &volume));
    EXPECT_EQ(volume.captures, 8u);
    EXPECT_EQ(ssgi_info(f).dispatches, 0u);
    EXPECT_TRUE(!f->common);
    f->settings.gi = DE_QUALITY_OFF;
    de_view_set_settings(f->r, f->view, &f->settings);
    room_frame(f);
    EXPECT_EQ(ssgi_info(f).dispatches, 0u);
    EXPECT_TRUE(!f->common);
}
TEST_F(gi, ssgi_static_mode_restores_probe_baseline_metal, gifx) {
    static_mode_restore(f, DE_GPU_METAL);
}
TEST_F(gi, ssgi_static_mode_restores_probe_baseline_vulkan, gifx) {
    static_mode_restore(f, DE_GPU_VULKAN);
}
TEST_F(gi, ssgi_static_mode_restores_probe_baseline_d3d12, gifx) {
    static_mode_restore(f, DE_GPU_D3D12);
}
static void directional_seams(gifx *f, de_gpu_backend backend) {
    open_renderer(f, backend);
    colored_room(f);
    f->scene.f.irradiance_count = 1;
    f->scene.f.irradiance[0] = (de__irradiance_snapshot){.entity = 77,
                                                         .rotation = {0, 0, 0, 1},
                                                         .volume = {.extent = {1, 1, 1},
                                                                    .counts = {2, 2, 2},
                                                                    .intensity = 1,
                                                                    .blend_distance = .1f,
                                                                    .bounces = 1}};
    for (u32 i = 0; i < 8; ++i)
        room_frame(f);
    de__irradiance_info info;
    ASSERT_TRUE(de__irradiance_get(f->r, 77, &info));
    ASSERT_TRUE(info.ready);
    f32 queries[64][12] = {{0}}, result[256];
    for (u32 i = 0; i < 32; ++i) {
        de_vec3 n = {(i & 1) ? .7f : 1e-5f, (i & 1) ? 1e-5f : .7f, (i & 2) ? -.7f : .7f};
        n = de_v3_normalize(n);
        for (u32 side = 0; side < 2; ++side) {
            u32 j = i * 2 + side;
            queries[j][0] = (i & 4) ? .5f : -.5f;
            queries[j][0] += side ? 1e-5f : -1e-5f;
            queries[j][4] = n.x;
            queries[j][5] = n.y;
            queries[j][6] = n.z;
            if (side)
                queries[j][(i & 1) ? 5 : 4] *= -1;
            queries[j][10] = 1;
        }
    }
    gi_queries(f, &info.gpu, 1, &queries[0][0], 64, result);
    f32 min = INFINITY, max = 0;
    for (u32 i = 0; i < 32; ++i) {
        for (u32 c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(result[(i * 2) * 4 + c], result[(i * 2 + 1) * 4 + c], .01f);
        min = fminf(min, result[i * 8]);
        max = fmaxf(max, result[i * 8]);
    }
    EXPECT_TRUE(max - min > 1);
}
TEST_F(gi, irradiance_directional_oct_grid_seams_metal, gifx) {
    directional_seams(f, DE_GPU_METAL);
}
TEST_F(gi, irradiance_directional_oct_grid_seams_vulkan, gifx) {
    directional_seams(f, DE_GPU_VULKAN);
}
TEST_F(gi, irradiance_directional_oct_grid_seams_d3d12, gifx) {
    directional_seams(f, DE_GPU_D3D12);
}
