#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include <dengine/de_reflect.h>
#include "gpu/gpu_backend.h"
#include "render/renderer_internal.h"
#include "render/passes/ssr/ssr_internal.h"
#include "render/passes/ao/gtao_internal.h"
#include "render/probes/probes_internal.h"
#include "render/graph/rg_internal.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_material.h"
#include "render/materials/material_models.h"
#include "render/materials/brdf_luts.h"
#include "render/loaders/render_assets.h"
#include "render/passes/vis/gbuffer.h"
#include "render_d3d12_test.h"
#include "flip.h"
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
static de__gpu_backend_vtable counted_wait_vt;
static const de__gpu_backend_vtable *original_wait_vt;
static u32 counted_wait_calls;
static void count_wait_idle(de_gpu_device *device) {
    ++counted_wait_calls;
    original_wait_vt->wait_idle(device);
}
typedef struct sfx {
    de_tracking_allocator tracking;
    de_allocator alloc;
    de_gpu_device *dev;
    de_renderer *r;
    de_type_registry *types;
    de_world *ecs;
    de_arena *arena;
    world scene;
    bool scene_live;
    de_rg *graphs[16];
    u32 graph_count;
    de_gpu_texture textures[32];
    u32 texture_count;
    de_gpu_buffer buffers[64];
    u32 buffer_count;
    de__vis_kernel kernel;
    bool suppress_readback;
    de__screen_space_context *common;
    de_view view;
    de__rw_view camera;
    u32 width, height;
    de_render_settings settings;
    de_gpu_buffer pre_readback;
    de_rg_resource pre_source;
    de_gpu_buffer view_readback;
    de_rg_resource reads[5];
    u64 offsets[5];
    u32 pitches[5];
} sfx;
static void sfx_setup(sfx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_RENDER);
}
TEST(ssr, fixture_removal_capacity) {
    world scene;
    world_init(&scene, 8);
    for (u32 slot = 0; slot < scene.cap; ++slot)
        scene.f.removed_instances[scene.f.removed_instance_count++] = slot;
    EXPECT_EQ(scene.f.removed_instance_count, scene.cap);
    for (u32 slot = 0; slot < scene.cap; ++slot)
        EXPECT_EQ(scene.f.removed_instances[slot], slot);
    world_free(&scene);
}
static void sfx_teardown(sfx *f) {
    if (f->dev && f->dev->vt == &counted_wait_vt)
        f->dev->vt = original_wait_vt;
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    de__vis_kernel_destroy(f->r ? de__views_vis(f->r) : NULL, &f->kernel);
    for (u32 i = 0; i < f->graph_count; ++i)
        de__rg_destroy(f->graphs[i]);
    for (u32 i = 0; i < f->texture_count; ++i)
        de_gpu_texture_destroy(f->dev, f->textures[i]);
    for (u32 i = 0; i < f->buffer_count; ++i)
        de_gpu_buffer_destroy(f->dev, f->buffers[i]);
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
    if (f->arena)
        de_arena_destroy(f->arena);
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
static void open_gpu(sfx *f, de_gpu_backend backend) {
    de_fp_env_init_thread();
    if (!backend_built(backend) && backend != DE_GPU_NULL)
        SKIP("backend not built");
#if defined(__APPLE__)
    if (backend == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc d = {.backend = backend,
                            .allocator = &f->alloc,
                            .debug_layer = backend != DE_GPU_NULL,
                            .gpu_validation = backend == DE_GPU_VULKAN,
                            .pipeline_cache_dir = ""};
    de_result result = de_gpu_device_create(&d, &f->dev);
    if (result == DE_ERR_UNSUPPORTED && backend == DE_GPU_VULKAN) {
        d.adapter = 1;
        result = de_gpu_device_create(&d, &f->dev);
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
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = 32u << 20}, &f->arena), DE_OK);
    (void)de_cvar_set(de_cvar_find("r.pipelined"), "0");
    (void)de_cvar_set(de_cvar_find("r.ambient_fallback"), "0");
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
typedef struct test_ray {
    f32 origin[4], delta[4];
} test_ray;
typedef struct hiz_params {
    u32 hzb, depth, rays, result, width, height, hzb_width, hzb_height, mips, count, max_steps,
        reserved;
    f32 near_z, thickness, depth_scale, padding;
} hiz_params;
DE_STATIC_ASSERT(sizeof(hiz_params) == 64, "HiZTestParams is 64 bytes");
typedef struct hiz_case {
    sfx *f;
    de_gpu_texture depth;
    de_gpu_buffer upload, input, result, readback;
    u32 pitch;
    de_rg_resource d, h, o, rb;
    hiz_params pc;
} hiz_case;
static void upload_depth(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    hiz_case *c = user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, c->d), 0, 0, c->upload, 0, c->pitch);
}
static void trace_rays(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    hiz_case *c = user;
    hiz_params pc = c->pc;
    pc.depth = de_gpu_texture_bindless(c->f->dev, de_rg_texture(pass, c->d), 0, false);
    pc.hzb = de_gpu_texture_bindless(c->f->dev, de_rg_texture(pass, c->h), 0, false);
    pc.rays = de_gpu_buffer_bindless(c->f->dev, c->input);
    pc.result = de_gpu_buffer_bindless(c->f->dev, c->result);
    de_gpu_cmd_set_pipeline(cmd, c->f->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.count + 63) / 64, 1, 1);
}
static void read_rays(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    hiz_case *c = user;
    (void)pass;
    de_gpu_cmd_copy_buffer(cmd, c->readback, 0, c->result, 0, (u64)c->pc.count * 16);
}
static f32 random_float(u32 *state) {
    *state = *state * 1664525u + 1013904223u;
    return (f32)(*state >> 8) * (1.0f / 16777216.0f);
}
static bool linear_trace(const f32 *depth, u32 w, u32 h, const test_ray *ray, f32 out[2]) {
    f32 pixels = fmaxf(fabsf(ray->delta[0]) * w, fabsf(ray->delta[1]) * h);
    f64 start = 1.05 / (f64)pixels, step = 1.0 / ((f64)pixels * 4096);
    for (f64 td = start; td < 1; td += step) {
        f64 x = ray->origin[0] + td * ray->delta[0], y = ray->origin[1] + td * ray->delta[1];
        f64 z = ray->origin[2] + td * ray->delta[2];
        if (x < 0 || x >= 1 || y < 0 || y >= 1 || z < 0 || z > 1)
            break;
        f32 d = depth[(u32)(y * h) * w + (u32)(x * w)];
        if (!(d > 0))
            continue;
        f32 dz = d - 0.1f / (0.1f / d + 0.01f);
        if (z <= d && z >= d - dz) {
            out[0] = (f32)x;
            out[1] = (f32)y;
            return true;
        }
    }
    return false;
}
// Independent exhaustive leaf-slab oracle: no hierarchy, GPU boundary rules or traversal order.
// This also catches intervals narrower than a dense uniform linear march can sample.
static bool leaf_reference(const f32 *depth, u32 w, u32 h, const test_ray *ray, f32 out[2]) {
    f64 pixels = fmax(fabs(ray->delta[0]) * w, fabs(ray->delta[1]) * h), best = 2;
    if (!(pixels > 1e-5))
        return false;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            f64 d = depth[(size_t)y * w + x];
            if (!(d > 0))
                continue;
            f64 lo[3] = {(f64)x / w, (f64)y / h, 0.1 / (0.1 / d + 0.01)},
                hi[3] = {(f64)(x + 1) / w, (f64)(y + 1) / h, d};
            f64 enter = 1.05 / pixels, exit = 1;
            for (u32 a = 0; a < 3; ++a) {
                f64 delta = ray->delta[a], origin = ray->origin[a];
                if (fabs(delta) < 1e-10) {
                    if (origin < lo[a] || origin >= hi[a])
                        exit = -1;
                    continue;
                }
                f64 a0 = (lo[a] - origin) / delta, a1 = (hi[a] - origin) / delta;
                enter = fmax(enter, fmin(a0, a1));
                exit = fmin(exit, fmax(a0, a1));
            }
            if (enter <= exit && enter < best && enter < 1) {
                best = enter;
            }
        }
    if (!(best < 1))
        return false;
    out[0] = (f32)(ray->origin[0] + best * ray->delta[0]);
    out[1] = (f32)(ray->origin[1] + best * ray->delta[1]);
    return true;
}
static void hiz_matches(sfx *f, de_gpu_backend backend) {
    open_gpu(f, backend);
    de__vis_shared *vis = de__views_vis(f->r);
    const char *entry = "cs_hiz_test";
    const u32 groups[] = {64, 1, 1};
    de__engine_shader_code code;
    ASSERT_EQ(de__vis_shader_load(vis, "ssr_trace", &entry, 1, &code), DE_OK);
    ASSERT_EQ(de__vis_kernel_create(vis, &code, 0, entry, groups, sizeof(hiz_params), &f->kernel),
              DE_OK);
    de__engine_shader_code_free(&code);
    const u32 sizes[][2] = {{64, 64}, {65, 37}, {127, 83}, {4096, 37}};
    for (u32 k = 0; k < 4; ++k) {
        u32 w = sizes[k][0], h = sizes[k][1], count = 256, state = 100 + k;
        f32 *depth = malloc((size_t)w * h * 4);
        ASSERT_TRUE(depth);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                depth[y * w + x] =
                    (x % 17 == 0 && y % 7 < 3) ? 0 : 0.2f + 0.45f * random_float(&state);
        test_ray rays[256];
        for (u32 i = 0; i < count; ++i) {
            memset(&rays[i], 0, sizeof rays[i]);
            rays[i].origin[0] = 0.05f + 0.9f * random_float(&state);
            rays[i].origin[1] = 0.05f + 0.9f * random_float(&state);
            rays[i].origin[2] = 0.8f;
            rays[i].delta[0] = (random_float(&state) - 0.5f) * 1.6f;
            rays[i].delta[1] = (random_float(&state) - 0.5f) * 1.6f;
            rays[i].delta[2] = -0.75f;
            if (k == 3)
                rays[i].delta[0] *= 0.03125f;
        }
        // Exact negative cell entries and axis-aligned / almost-axis-aligned motion complement
        // random rays. A returned hit must identify the same leaf used for its depth.
        for (u32 i = 224; i < count; ++i) {
            u32 axis = i & 1;
            f32 direction = (i & 2) ? -0.8f : 0.8f;
            rays[i].origin[0] = (axis == 0 ? 32.0f : 32.5f) / w;
            rays[i].origin[1] = (axis == 1 ? 18.0f : 18.5f) / h;
            rays[i].delta[axis] = direction;
            rays[i].delta[1 - axis] = (i & 4) ? 1e-9f : 0;
            if (k == 3) {
                rays[i].origin[0] = 3072.0f / w;
                rays[i].delta[0] = (i & 2) ? -0.025f : 0.025f;
            }
        }
        for (u32 i = 248; i < count; ++i) {
            rays[i].origin[0] = 0;
            rays[i].origin[1] = 0;
            rays[i].delta[0] = -0.8f;
            rays[i].delta[1] = -0.8f;
        }
        hiz_case c = {.f = f,
                      .pitch = (w * 4 + 511u) & ~511u,
                      .pc = {.width = w,
                             .height = h,
                             .count = count,
                             .max_steps = 2048,
                             .near_z = 0.1f,
                             .thickness = 0.01f}};
        de__vis_hzb_size(w, h, &c.pc.hzb_width, &c.pc.hzb_height, &c.pc.mips);
        c.depth = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.width = w,
                                           .height = h,
                                           .format = DE_FMT_R32_FLOAT,
                                           .usage = DE_USAGE_COPY_DST | DE_USAGE_SAMPLED,
                                           .name = "ssr.synthetic_depth"});
        c.upload = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)c.pitch * h,
                                                                      .memory = DE_MEM_UPLOAD,
                                                                      .usage = DE_USAGE_COPY_SRC});
        c.input = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof rays,
                                                                     .stride = sizeof(test_ray),
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .usage = DE_USAGE_STORAGE});
        c.result = de_gpu_buffer_create(
            f->dev, &(de_gpu_buffer_desc){.size = count * 16u,
                                          .stride = 16,
                                          .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
        c.readback =
            de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = count * 16u,
                                                               .memory = DE_MEM_READBACK,
                                                               .usage = DE_USAGE_COPY_DST});
        ASSERT_TRUE(DE_HANDLE_VALID(c.depth) && DE_HANDLE_VALID(c.upload) &&
                    DE_HANDLE_VALID(c.input) && DE_HANDLE_VALID(c.result) &&
                    DE_HANDLE_VALID(c.readback));
        u8 *up = de_gpu_buffer_map(f->dev, c.upload);
        ASSERT_TRUE(up);
        for (u32 y = 0; y < h; ++y)
            memcpy(up + (size_t)y * c.pitch, depth + (size_t)y * w, w * 4);
        memcpy(de_gpu_buffer_map(f->dev, c.input), rays, sizeof rays);
        de_rg *g = NULL;
        ASSERT_EQ(
            de__rg_create(
                &(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "ssr.trace_test"},
                &g),
            DE_OK);
        f->graphs[f->graph_count++] = g;
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(g, f->arena), DE_OK);
        c.d = de_rg_import_texture(g, c.depth, DE_ACCESS_NONE);
        c.o = de_rg_import_buffer(g, c.result, DE_ACCESS_NONE);
        c.rb = de_rg_import_buffer(g, c.readback, DE_ACCESS_NONE);
        c.h = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.width = c.pc.hzb_width,
                                      .height = c.pc.hzb_height,
                                      .mip_count = c.pc.mips,
                                      .format = DE_FMT_R32_FLOAT,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                      .name = "ssr.test_max_hzb"});
        de_rg_pass *p = de_rg_add_pass(g, "test.depth_upload", DE_QUEUE_GRAPHICS, upload_depth, &c);
        de_rg_write(p, c.d, DE_ACCESS_COPY_DST);
        de__vis_hzb_target target = {c.h, c.pc.hzb_width, c.pc.hzb_height, c.pc.mips};
        de__vis_hzb_add_passes(vis, g, c.d, w, h, &target, DE__VIS_HZB_OP_MAX, false);
        p = de_rg_add_pass(g, "test.trace", DE_QUEUE_GRAPHICS, trace_rays, &c);
        de_rg_read(p, c.d, DE_ACCESS_SHADER_READ);
        de_rg_read(p, c.h, DE_ACCESS_SHADER_READ);
        de_rg_read(p, de_rg_import_buffer(g, c.input, DE_ACCESS_SHADER_READ),
                   DE_ACCESS_SHADER_READ);
        de_rg_write(p, c.o, DE_ACCESS_SHADER_WRITE);
        p = de_rg_add_pass(g, "test.readback", DE_QUEUE_GRAPHICS, read_rays, &c);
        de_rg_pass_side_effect(p);
        de_rg_read(p, c.o, DE_ACCESS_COPY_SRC);
        de_rg_write(p, c.rb, DE_ACCESS_COPY_DST);
        ASSERT_EQ(de__rg_finalize(g), DE_OK);
        ASSERT_EQ(de__rg_compile(g), DE_OK);
        ASSERT_EQ(de__rg_execute(g, &(de__rg_execute_desc){.frame_index = k}), DE_OK);
        de_gpu_wait_idle(f->dev);
        de__rg_reset(g);
        const f32 *gpu = de_gpu_buffer_map(f->dev, c.readback);
        ASSERT_TRUE(gpu);
        u32 mismatches = 0, hits = 0;
        for (u32 i = 0; i < count; ++i) {
            f32 xy[2] = {0};
            bool hit = k == 3 ? leaf_reference(depth, w, h, &rays[i], xy)
                              : linear_trace(depth, w, h, &rays[i], xy),
                 gh = gpu[i * 4 + 3] > 0;
            if (gh) {
                u32 hx = (u32)(gpu[i * 4] * w), hy = (u32)(gpu[i * 4 + 1] * h);
                ASSERT_TRUE(hx < w && hy < h);
                EXPECT_FLOAT_EQ(depth[hy * w + hx], gpu[i * 4 + 2], 1e-7f);
            }
            if (hit)
                ++hits;
            if (hit != gh || (hit && (fabsf(xy[0] - gpu[i * 4]) * w > 1 ||
                                      fabsf(xy[1] - gpu[i * 4 + 1]) * h > 1))) {

                if (mismatches++ < 4)
                    printf("size%ux%u ray%u cpu%d gpu%d cpu(%g,%g) gpu(%g,%g)\n", w, h, i, hit, gh,
                           (double)xy[0], (double)xy[1], (double)gpu[i * 4],
                           (double)gpu[i * 4 + 1]);
            }
        }
        printf("size%ux%u %u hits, %u mismatches\n", w, h, hits, mismatches);
        EXPECT_EQ(mismatches, 0u);
        EXPECT_TRUE(hits > 20);
        f->textures[f->texture_count++] = c.depth;
        f->buffers[f->buffer_count++] = c.upload;
        f->buffers[f->buffer_count++] = c.input;
        f->buffers[f->buffer_count++] = c.result;
        f->buffers[f->buffer_count++] = c.readback;
        free(depth);
    }
}
TEST_F(ssr, hiz_trace_matches_linear_march_metal, sfx) {
    hiz_matches(f, DE_GPU_METAL);
}
TEST_F(ssr, hiz_trace_matches_linear_march_vulkan, sfx) {
    hiz_matches(f, DE_GPU_VULKAN);
}
TEST_F(ssr, hiz_trace_matches_linear_march_d3d12, sfx) {
    hiz_matches(f, DE_GPU_D3D12);
}

static void view_read_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    sfx *f = user;
    for (u32 i = 0; i < 5; ++i)
        de_gpu_cmd_copy_texture_to_buffer(cmd, f->view_readback, f->offsets[i], f->pitches[i],
                                          de_rg_texture(pass, f->reads[i]), 0, 0);
}
static void view_read_hook(de_rg *g, de_view view, void *user) {
    sfx *f = user;
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
    de_rg_pass *p =
        de_rg_add_pass(g, "test.ssr_view_read", DE_QUEUE_GRAPHICS, view_read_execute, f);
    for (u32 i = 0; i < 5; ++i)
        de_rg_read(p, f->reads[i], DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(g, f->view_readback, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
}
static void camera_at(sfx *f, de_vec3 eye, de_vec3 target) {
    de_mat4 view = de_m4_look_at(eye, target, (de_vec3){0, 0, 1}),
            inv = de_m4_inverse_affine(&view);
    f->camera.transform = de_affine_from_m4(&inv);
    f->camera.offset = eye;
    de_mat3 rotation;
    memcpy(rotation.m, f->camera.transform.m, sizeof rotation.m);
    f->camera.rotation = de_quat_from_m3(&rotation);
}
static void mirror_scene(sfx *f, de_gpu_backend backend, f32 roughness, f32 brightness) {
    open_gpu(f, backend);
    f->width = f->height = 128;
    world_init(&f->scene, 8);
    f->scene_live = true;
    g_mesh_count = 2;
    mesh_plane(&g_mesh[0], f->dev, 1, 1, 12);
    mesh_plane(&g_mesh[1], f->dev, 1, 1, 2);
    u32 floor = material_add(1, roughness, 1, 1);
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
    f->settings.ao = f->settings.gi = f->settings.sss = DE_QUALITY_OFF;
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
    f->buffers[f->buffer_count++] = f->view_readback;
    de_render_add_hook(f->r, DE_HOOK_AFTER_TRANSPARENT, view_read_hook, f);
}
static void view_frame(sfx *f) {
    world_begin(&f->scene);
    f->r->render_world->last = &f->scene.f;
    f->r->render_world->current = &f->scene.f;
    de__renderer_kick_prepare(f->r);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    de_gpu_wait_idle(f->dev);
}
static void read_texture(sfx *f, de_gpu_texture texture, u32 before, u32 bytes_per_pixel,
                         void *out) {
    u32 pitch = (f->width * bytes_per_pixel + 511u) & ~511u;
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * f->height,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK});
    ASSERT_TRUE(DE_HANDLE_VALID(rb));
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "test.ssr_storage_read");
    ASSERT_TRUE(cmd);
    de_gpu_barrier b = {.texture = texture, .before = before, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, pitch, texture, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = before;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    const u8 *data = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(data);
    for (u32 y = 0; y < f->height; ++y)
        memcpy((u8 *)out + (size_t)y * f->width * bytes_per_pixel, data + (size_t)y * pitch,
               f->width * bytes_per_pixel);
    de_gpu_buffer_destroy(f->dev, rb);
}
static void mirror_reference(sfx *f, const u16 *first);
static void mirror_smoke(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.01f, 1);
    view_frame(f);
    de__ssr_info info;
    ASSERT_TRUE(de__ssr_view_info(f->r->ssr, f->view, &info));
    EXPECT_FALSE(info.history_valid);
    EXPECT_EQ(info.dispatches, 2u);
    u16 *first = malloc((size_t)f->width * f->height * 8);
    ASSERT_TRUE(first);
    const u8 *first_rb = de_gpu_buffer_map(f->dev, f->view_readback);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(first + (size_t)y * f->width * 4,
               first_rb + f->offsets[4] + (size_t)y * f->pitches[4], f->width * 8);
    view_frame(f);
    ASSERT_TRUE(de__ssr_view_info(f->r->ssr, f->view, &info));
    EXPECT_TRUE(info.history_valid);
    u16 *image = malloc((size_t)f->width * f->height * 8);
    ASSERT_TRUE(image);
    read_texture(f, info.temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8, image);
    u32 hits = 0;
    f32 maximum = 0;
    for (u32 i = 0; i < f->width * f->height; ++i) {
        f32 alpha = half_to_f32(image[i * 4 + 3]);
        if (alpha > 0.1f) {
            ++hits;
            maximum = fmaxf(maximum, half_to_f32(image[i * 4 + 2]));
        }
    }
    printf("mirror view %u confident pixels, maximum blue %g\n", hits, (double)maximum);
    EXPECT_TRUE(hits > 20);
    EXPECT_TRUE(maximum > 0.1f);
    mirror_reference(f, first);
    free(first);
    free(image);
}
TEST_F(ssr, ssr_mirror_floor_reference_metal, sfx) {
    mirror_smoke(f, DE_GPU_METAL);
}
TEST_F(ssr, ssr_mirror_floor_reference_vulkan, sfx) {
    mirror_smoke(f, DE_GPU_VULKAN);
}
TEST_F(ssr, ssr_mirror_floor_reference_d3d12, sfx) {
    mirror_smoke(f, DE_GPU_D3D12);
}
static u8 display_byte(f32 x) {
    f32 c = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(fmaxf(x, 0), 1 / 2.4f) - 0.055f;
    return (u8)lrintf(fminf(fmaxf(c, 0), 1) * 255);
}
static void cpu_response(const f32 *lut, f32 mu, f32 rough, const f32 base[3], f32 metal, f32 spec,
                         f32 out[3]) {
    const u32 n = DE__BRDF_LUT_SIZE;
    const f32 upper = (f32)(n - 1);
    f32 x = fminf(fmaxf(mu * (f32)n - 0.5f, 0), upper);
    f32 y = fminf(fmaxf(rough * upper, 0), upper);
    u32 x0 = (u32)x, y0 = (u32)y, x1 = x0 + 1 < n ? x0 + 1 : x0, y1 = y0 + 1 < n ? y0 + 1 : y0;
    f32 fx = x - x0, fy = y - y0, t[4];
    for (u32 c = 0; c < 4; ++c) {
        f32 a = lut[(y0 * n + x0) * 4 + c] * (1 - fx) + lut[(y0 * n + x1) * 4 + c] * fx;
        f32 b = lut[(y1 * n + x0) * 4 + c] * (1 - fx) + lut[(y1 * n + x1) * 4 + c] * fx;
        t[c] = a * (1 - fy) + b * fy;
    }
    for (u32 c = 0; c < 3; ++c) {
        f32 f0 = 0.04f * spec * (1 - metal) + base[c] * metal, f90 = spec * (1 - metal) + metal;
        f32 avg = f0 + (f90 - f0) / 21, ea = t[2] + t[3],
            ms = avg * avg * ea / (1 - avg * (1 - ea));
        out[c] = f0 * t[0] + f90 * t[1] + ms * (1 - t[0] - t[1]);
    }
}
static bool mirror_linear(const f32 *depth, u32 w, u32 h, const test_ray *ray, f32 uv[2],
                          f32 *hit_depth) {
    f64 pixels = fmax(fabs(ray->delta[0]) * w, fabs(ray->delta[1]) * h);
    if (!(pixels > 0))
        return false;
    for (f64 t = 1.05 / pixels; t < 1; t += 1 / (pixels * 64)) {
        f64 x = ray->origin[0] + t * ray->delta[0], y = ray->origin[1] + t * ray->delta[1],
            z = ray->origin[2] + t * ray->delta[2];
        if (x < 0 || x >= 1 || y < 0 || y >= 1 || z < 0 || z > 1)
            break;
        f32 d = depth[(u32)(y * h) * w + (u32)(x * w)];
        if (d > 0 && z <= d && z >= 0.1 / (0.1 / d + 0.1)) {
            uv[0] = (f32)x;
            uv[1] = (f32)y;
            *hit_depth = d;
            return true;
        }
    }
    return false;
}
static de_vec3 cpu_position(const de_mat4 *inv, f32 x, f32 y, f32 z) {
    de_vec4 p = de_m4_mul_v4(inv, (de_vec4){x * 2 - 1, 1 - y * 2, z, 1});
    return (de_vec3){p.x / p.w, p.y / p.w, p.z / p.w};
}
static f32 bilinear_half(const u16 *image, u32 w, u32 h, f32 x, f32 y, u32 channel) {
    f32 px = x * w - 0.5f, py = y * h - 0.5f;
    int ax = (int)floorf(px), ay = (int)floorf(py);
    f32 fx = px - floorf(px), fy = py - floorf(py), sum = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int sx = ax + i, sy = ay + j;
            if (sx < 0)
                sx = 0;
            if (sy < 0)
                sy = 0;
            if (sx >= (int)w)
                sx = (int)w - 1;
            if (sy >= (int)h)
                sy = (int)h - 1;
            sum += half_to_f32(image[((size_t)sy * w + (u32)sx) * 4 + channel]) *
                   (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        }
    return sum;
}
static void mirror_reference(sfx *f, const u16 *first) {
    u32 w = f->width, h = f->height;
    size_t count = (size_t)w * h;
    const u8 *rb = de_gpu_buffer_map(f->dev, f->view_readback);
    ASSERT_TRUE(rb);
    f32 *depth = malloc(count * 4), *lut = malloc(DE__BRDF_LUT_SIZE * DE__BRDF_LUT_ROWS * 16);
    u8 *ref = malloc(count * 3), *actual = malloc(count * 3), *mask = calloc(count, 1);
    f32 *error = malloc(count * 4), *selected = malloc(count * 4);
    ASSERT_TRUE(depth && lut && ref && actual && mask && error && selected);
    de__brdf_lut_reference(DE__BRDF_LUT_SAMPLES, lut);
    for (u32 y = 0; y < h; ++y)
        memcpy(depth + (size_t)y * w, rb + f->offsets[0] + (size_t)y * f->pitches[0], w * 4);
    de_mat4 camera = de_affine_to_m4(&f->camera.transform), view = de_m4_inverse_affine(&camera);
    de_mat4 proj = de_m4_perspective_rz(f->camera.camera.fov_y, (f32)w / h, 0.1f),
            vp = de_m4_mul(&proj, &view), inv = de_m4_inverse(&vp);
    u32 hits = 0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            const u32 *g0 = (const u32 *)(rb + f->offsets[1] + (size_t)y * f->pitches[1]);
            const u32 *g1 = (const u32 *)(rb + f->offsets[2] + (size_t)y * f->pitches[2]);
            const u32 *g2 = (const u32 *)(rb + f->offsets[3] + (size_t)y * f->pitches[3]);
            const u16 *lit = (const u16 *)(rb + f->offsets[4] + (size_t)y * f->pitches[4]);
            for (u32 c = 0; c < 3; ++c)
                ref[i * 3 + c] = actual[i * 3 + c] = display_byte(half_to_f32(lit[x * 4 + c]));
            f32 normal[3], rough;
            de__gb1_unpack(g1[x], normal, &rough);
            if (!(depth[i] > 0) || rough >= 0.05f)
                continue;
            de_vec3 n = {normal[0], normal[1], normal[2]},
                    p = cpu_position(&inv, ((f32)x + 0.5f) / w, ((f32)y + 0.5f) / h, depth[i]);
            de_vec3 v = de_v3_normalize(de_v3_sub(f->camera.offset, p));
            f32 mu = de_v3_dot(n, v);
            de_vec3 direction = de_v3_sub(de_v3_scale(n, 2 * mu), v),
                    start = de_v3_add(p, de_v3_scale(n, 0.005f));
            de_vec4 a = de_m4_mul_v4(&vp, (de_vec4){start.x, start.y, start.z, 1});
            de_vec3 endpoint = de_v3_add(start, de_v3_scale(direction, 100));
            de_vec4 b = de_m4_mul_v4(&vp, (de_vec4){endpoint.x, endpoint.y, endpoint.z, 1});
            if (!(a.w > 0) || !(b.w > 0))
                continue;
            test_ray ray = {{a.x / a.w * 0.5f + 0.5f, 0.5f - a.y / a.w * 0.5f, a.z / a.w, 0},
                            {(b.x / b.w - a.x / a.w) * 0.5f, (a.y / a.w - b.y / b.w) * 0.5f,
                             b.z / b.w - a.z / a.w, 0}};
            f32 uv[2], hd;
            if (!mirror_linear(depth, w, h, &ray, uv, &hd))
                continue;
            u32 hx = (u32)(uv[0] * w), hy = (u32)(uv[1] * h);
            if (hx >= w || hy >= h)
                continue;
            const u32 *hitgb = (const u32 *)(rb + f->offsets[2] + (size_t)hy * f->pitches[2]);
            f32 hn[3], hr;
            de__gb1_unpack(hitgb[hx], hn, &hr);
            f32 front = -(hn[0] * direction.x + hn[1] * direction.y + hn[2] * direction.z);
            f32 edge = fminf(fminf(uv[0], 1 - uv[0]), fminf(uv[1], 1 - uv[1]));
            f32 confidence = fminf(fmaxf(edge * 20, 0), 1) * fminf(fmaxf(front * 4, 0), 1);
            if (confidence <= 0.1f)
                continue;
            f32 base[3], response[3];
            for (u32 c = 0; c < 3; ++c)
                base[c] = de__gb_srgb_decode((f32)((g0[x] >> (c * 8)) & 255) / 255);
            cpu_response(lut, fmaxf(mu, 1e-4f), rough, base, (f32)(g2[x] & 255) / 255,
                         (f32)(g2[x] >> 24) / 255, response);
            for (u32 c = 0; c < 3; ++c)
                ref[i * 3 + c] = display_byte(bilinear_half(first, w, h, uv[0], uv[1], c) *
                                              response[c] * confidence);
            mask[i] = 1;
            ++hits;
        }
    de_flip_image ri = {ref, w, h, 3, 0}, ai = {actual, w, h, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&ri, &ai, 0, error));
    u32 n = 0;
    for (size_t i = 0; i < count; ++i)
        if (mask[i])
            selected[n++] = error[i];
    de_flip_stats stats;
    ASSERT_TRUE(n > 20);
    ASSERT_TRUE(de_flip_stats_compute(selected, n, &stats));
    printf("CPU/G-buffer mirror %u on-screen hits: FLIP mean%g p99%g\n", hits, stats.mean,
           (double)stats.p99);
    EXPECT_TRUE(stats.mean < 0.005);
    EXPECT_TRUE(stats.p99 < 0.05f);
    free(depth);
    free(lut);
    free(ref);
    free(actual);
    free(mask);
    free(error);
    free(selected);
}

static de__ssr_info current_info(sfx *f) {
    de__ssr_info info;
    ASSERT_TRUE(de__ssr_view_info(f->r->ssr, f->view, &info));
    return info;
}
static void rough_converges(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.25f, 2000);
    size_t count = (size_t)f->width * f->height;
    u16 *image = malloc(count * 8), *previous = calloc(count, 8);
    f32 *moments = malloc(count * 16);
    ASSERT_TRUE(image && previous && moments);
    f64 early = 0, late = 0;
    u32 early_samples = 0, late_samples = 0, accumulated = 0;
    f32 brightest_moment = 0;
    for (u32 frame = 0; frame < 17; ++frame) {
        view_frame(f);
        de__ssr_info info = current_info(f);
        read_texture(f, info.temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8, image);
        read_texture(f, info.moments, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 16, moments);
        f64 delta = 0;
        u32 stable = 0;
        for (size_t i = 0; i < count; ++i) {
            for (u32 c = 0; c < 4; ++c) {
                EXPECT_TRUE(isfinite(half_to_f32(image[i * 4 + c])));
                EXPECT_TRUE(isfinite(moments[i * 4 + c]));
            }
            if (moments[i * 4 + 2] >= 8)
                ++accumulated;
            brightest_moment = fmaxf(brightest_moment, moments[i * 4 + 1]);
            if (frame > 1 && half_to_f32(image[i * 4 + 3]) > 0.9f &&
                half_to_f32(previous[i * 4 + 3]) > 0.9f) {
                f64 d = half_to_f32(image[i * 4 + 2]) - half_to_f32(previous[i * 4 + 2]);
                delta += d * d;
                ++stable;
            }
        }
        if (stable) {
            delta /= stable;
            if (frame >= 2 && frame <= 5) {
                early += delta;
                early_samples += stable;
            }
            if (frame >= 13) {
                late += delta;
                late_samples += stable;
            }
        }
        memcpy(previous, image, count * 8);
    }
    printf("rough HDR temporal: early delta %g late %g, accumulated %u, second moment %g\n", early,
           late, accumulated, (double)brightest_moment);
    EXPECT_TRUE(early_samples > 100);
    EXPECT_TRUE(late_samples > 100);
    EXPECT_TRUE(accumulated > 100);
    EXPECT_TRUE(brightest_moment > 65504);
    /* Exact half-float stability is already converged; require strict improvement otherwise.
     * The receiver/sample-count and HDR moment checks above prevent empty/black success. */
    EXPECT_TRUE(late < early || (early == 0 && late == 0));
    free(image);
    free(previous);
    free(moments);
}
static void confidence_converges(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.25f, 2000);
    const size_t count = (size_t)f->width * f->height;
    u16 *image = malloc(count * 8);
    f64 *mean = calloc(count * 4, sizeof *mean), *m2 = calloc(count * 4, sizeof *m2);
    ASSERT_TRUE(image && mean && m2);
    for (u32 frame = 0; frame < 96; ++frame) {
        view_frame(f);
        read_texture(f, current_info(f).temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8,
                     image);
        if (frame < 64)
            continue;
        for (size_t i = 0; i < count; ++i) {
            const f64 confidence = half_to_f32(image[i * 4 + 3]);
            EXPECT_TRUE(isfinite(confidence) && confidence >= 0 && confidence <= 1);
            for (u32 c = 0; c < 4; ++c) {
                /* Lighting uses RGB * confidence; stable RGB alone is insufficient. */
                const f64 value = c == 3 ? confidence : half_to_f32(image[i * 4 + c]) * confidence;
                EXPECT_TRUE(isfinite(value));
                const f64 delta = value - mean[i * 4 + c];
                mean[i * 4 + c] += delta / (frame - 63);
                m2[i * 4 + c] += delta * (value - mean[i * 4 + c]);
            }
        }
    }
    f64 confidence_variance = 0, contribution_variance = 0;
    u32 reflective = 0;
    for (size_t i = 0; i < count; ++i) {
        confidence_variance += m2[i * 4 + 3] / 32;
        for (u32 c = 0; c < 3; ++c)
            contribution_variance += m2[i * 4 + c] / 32;
        reflective += mean[i * 4 + 3] > 0.1;
    }
    confidence_variance /= count;
    contribution_variance /= count * 3;
    printf("stationary SSR: confidence variance %g, HDR contribution variance %g, %u receivers\n",
           confidence_variance, contribution_variance, reflective);
    EXPECT_GT(reflective, 1000u); /* Reject a vacuous result from disabling SSR. */
    /* Original instantaneous confidence measured .001704 / 1705.86 on native Metal.
     * Leave substantial headroom over the corrected .000420 / 412.79. */
    EXPECT_TRUE(confidence_variance < .001);
    EXPECT_TRUE(contribution_variance < 1000);

    /* Remove reflected geometry while the receiver/history remain valid: all misses must
     * immediately restore fallback, even after confidence has accumulated for 96 frames. */
    for (u32 slot = 1; slot < f->scene.f.instances.count; ++slot) {
        f->scene.f.removed_instances[f->scene.f.removed_instance_count++] = slot;
        f->scene.f.instances.entity[slot] = 0;
    }
    f->scene.f.instances.count = 1;
    view_frame(f);
    EXPECT_TRUE(current_info(f).history_valid);
    read_texture(f, current_info(f).temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8,
                 image);
    f32 max_missing_confidence = 0;
    for (size_t i = 0; i < count; ++i)
        max_missing_confidence = fmaxf(max_missing_confidence, half_to_f32(image[i * 4 + 3]));
    EXPECT_FLOAT_EQ(max_missing_confidence, 0, 0);
    free(image);
    free(mean);
    free(m2);
}
static void exposure_correct(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.01f, 10);
    g_mat[0].standard.metallic = 0; // nonwhite dielectric, response must be applied once
    view_frame(f);
    size_t count = (size_t)f->width * f->height;
    u16 *first = malloc(count * 8);
    ASSERT_TRUE(first);
    const u8 *first_rb = de_gpu_buffer_map(f->dev, f->view_readback);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(first + (size_t)y * f->width * 4,
               first_rb + f->offsets[4] + (size_t)y * f->pitches[4], f->width * 8);
    view_frame(f);
    mirror_reference(f, first);
    free(first);
    de__ssr_info before = current_info(f);
    u16 *a = malloc(count * 8), *b = malloc(count * 8);
    ASSERT_TRUE(a && b);
    read_texture(f, before.temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8, a);
    f->camera.camera.exposure_ev100 += 2;
    view_frame(f);
    de__ssr_info after = current_info(f);
    EXPECT_TRUE(after.history_valid);
    EXPECT_FLOAT_EQ(after.current_exposure / before.current_exposure, 0.25f, 1e-5f);
    EXPECT_FLOAT_EQ(after.previous_exposure, before.current_exposure, 1e-5f);
    read_texture(f, after.temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8, b);
    const u8 *rb = de_gpu_buffer_map(f->dev, f->view_readback);
    u32 selected = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            size_t i = (size_t)y * f->width + x;
            if (half_to_f32(a[i * 4 + 3]) < 0.99f || half_to_f32(b[i * 4 + 3]) < 0.99f)
                continue;
            f32 expected = half_to_f32(a[i * 4 + 2]);
            if (expected < 0.01f)
                continue;
            ++selected;
            EXPECT_FLOAT_EQ(half_to_f32(b[i * 4 + 2]), expected, expected * 0.01f + 0.001f);
            const u16 *lit = (const u16 *)(rb + f->offsets[4] + (size_t)y * f->pitches[4]);
            EXPECT_FLOAT_EQ(half_to_f32(lit[x * 4 + 2]), expected * after.current_exposure,
                            expected * 0.01f + 0.001f);
        }
    EXPECT_TRUE(selected > 100);
    free(a);
    free(b);
}
static void off_no_dispatch(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.01f, 1);
    f->settings.reflections = DE_QUALITY_OFF;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    EXPECT_EQ(current_info(f).dispatches, 0u);
    EXPECT_TRUE(f->common == NULL);
    f->settings.gi = DE_QUALITY_HIGH;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    EXPECT_EQ(current_info(f).dispatches, 0u);
    ASSERT_TRUE(f->common);
    EXPECT_TRUE(f->common->capture_added);
    EXPECT_FALSE(f->common->valid);
    view_frame(f);
    EXPECT_EQ(current_info(f).dispatches, 0u);
    ASSERT_TRUE(f->common);
    EXPECT_TRUE(f->common->valid);
    f->settings.reflections = DE_QUALITY_MEDIUM;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    de__ssr_info info = current_info(f);
    EXPECT_TRUE(info.history_valid);
    EXPECT_EQ(info.dispatches, 2u);
    EXPECT_EQ(info.trace_width, 64u);
    EXPECT_EQ(info.trace_height, 64u);
    f->settings.reflections = f->settings.gi = DE_QUALITY_OFF;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    EXPECT_EQ(current_info(f).dispatches, 0u);
    EXPECT_TRUE(f->common == NULL);
    f->settings.reflections = DE_QUALITY_HIGH;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    EXPECT_FALSE(current_info(f).history_valid);
    view_frame(f);
    EXPECT_TRUE(current_info(f).history_valid);
    for (u32 cycle = 0; cycle < 3; ++cycle) {
        f->settings.reflections = DE_QUALITY_OFF;
        de_view_set_settings(f->r, f->view, &f->settings);
        view_frame(f);
        EXPECT_EQ(current_info(f).dispatches, 0u);
        f->settings.reflections = DE_QUALITY_HIGH;
        de_view_set_settings(f->r, f->view, &f->settings);
        view_frame(f);
        EXPECT_FALSE(current_info(f).history_valid);
        view_frame(f);
        EXPECT_TRUE(current_info(f).history_valid);
    }
    u64 drains = de__views_resource_drains(f->r);
    for (u32 frame = 0; frame < 4; ++frame)
        view_frame(f);
    EXPECT_EQ(de__views_resource_drains(f->r), drains);
    camera_at(f, (de_vec3){0, 20, 3}, (de_vec3){0, 0, 0.8f});
    view_frame(f);
    EXPECT_FALSE(current_info(f).history_valid);
}
#define SSR_BACKEND_TESTS(suffix, backend)                                                         \
    TEST_F(ssr, ssr_confidence_converges_##suffix, sfx) {                                          \
        confidence_converges(f, backend);                                                          \
    }                                                                                              \
    TEST_F(ssr, ssr_rough_converges_no_nan_##suffix, sfx) {                                        \
        rough_converges(f, backend);                                                               \
    }                                                                                              \
    TEST_F(ssr, ssr_history_exposure_correct_##suffix, sfx) {                                      \
        exposure_correct(f, backend);                                                              \
    }                                                                                              \
    TEST_F(ssr, ssr_off_no_dispatch_##suffix, sfx) {                                               \
        off_no_dispatch(f, backend);                                                               \
    }
SSR_BACKEND_TESTS(metal, DE_GPU_METAL)
SSR_BACKEND_TESTS(vulkan, DE_GPU_VULKAN)
SSR_BACKEND_TESTS(d3d12, DE_GPU_D3D12)

static void offscreen_zero(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.01f, 1);
    f->scene.f.instances.count = 1; // only floor: reflected rays leave the view
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.ambient_fallback"), "2"), DE_OK);
    f->settings.reflections = DE_QUALITY_OFF;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    size_t count = (size_t)f->width * f->height;
    u16 *baseline = malloc(count * 8), *image = malloc(count * 8);
    ASSERT_TRUE(baseline && image);
    const u8 *rb = de_gpu_buffer_map(f->dev, f->view_readback);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(baseline + (size_t)y * f->width * 4, rb + f->offsets[4] + (size_t)y * f->pitches[4],
               f->width * 8);
    f->settings.reflections = DE_QUALITY_HIGH;
    de_view_set_settings(f->r, f->view, &f->settings);
    for (u32 frame = 0; frame < 4; ++frame)
        view_frame(f);
    read_texture(f, current_info(f).temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8,
                 image);
    rb = de_gpu_buffer_map(f->dev, f->view_readback);
    u32 fallback = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            size_t i = (size_t)y * f->width + x;
            EXPECT_FLOAT_EQ(half_to_f32(image[i * 4 + 3]), 0, 0);
            const u16 *lit = (const u16 *)(rb + f->offsets[4] + (size_t)y * f->pitches[4]);
            for (u32 c = 0; c < 3; ++c)
                EXPECT_FLOAT_EQ(half_to_f32(lit[x * 4 + c]), half_to_f32(baseline[i * 4 + c]),
                                0.001f);
            if (half_to_f32(lit[x * 4 + 2]) > 0.1f)
                ++fallback;
        }
    EXPECT_TRUE(fallback > 1000);
    free(baseline);
    free(image);
}
static void before_forward_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    sfx *f = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->pre_readback, 0, f->pitches[4],
                                      de_rg_texture(pass, f->pre_source), 0, 0);
}
static void before_forward_hook(de_rg *g, de_view view, void *user) {
    sfx *f = user;
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
static void forward_preserved(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.01f, 1);
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
    f->buffers[f->buffer_count++] = f->pre_readback;
    de_render_add_hook(f->r, DE_HOOK_AFTER_LIGHTING, before_forward_hook, f);
    view_frame(f);
    view_frame(f);
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
    read_texture(f, current_info(f).temporal, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 8,
                 ssr);
    f->settings.reflections = DE_QUALITY_OFF;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    pre = de_gpu_buffer_map(f->dev, f->pre_readback);
    post = de_gpu_buffer_map(f->dev, f->view_readback);
    u32 selected = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            size_t i = (size_t)y * f->width + x;
            if (half_to_f32(ssr[i * 4 + 3]) < 0.99f)
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
#define SSR_EDGE_TESTS(suffix, backend)                                                            \
    TEST_F(ssr, ssr_offscreen_confidence_zero_##suffix, sfx) {                                     \
        offscreen_zero(f, backend);                                                                \
    }                                                                                              \
    TEST_F(ssr, ssr_forward_preserves_fallback_##suffix, sfx) {                                    \
        forward_preserved(f, backend);                                                             \
    }
SSR_EDGE_TESTS(metal, DE_GPU_METAL)
SSR_EDGE_TESTS(vulkan, DE_GPU_VULKAN)
SSR_EDGE_TESTS(d3d12, DE_GPU_D3D12)

static void resize_generation(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.01f, 1);
    view_frame(f);
    view_frame(f);
    EXPECT_TRUE(current_info(f).history_valid);
    f->settings.reflections = DE_QUALITY_MEDIUM;
    f->settings.render_scale = 97.0f / 128.0f;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    de__ssr_info info = current_info(f);
    EXPECT_EQ(info.width, 97u);
    EXPECT_EQ(info.height, 97u);
    EXPECT_EQ(info.trace_width, 49u);
    EXPECT_FALSE(info.history_valid);
    view_frame(f);
    EXPECT_TRUE(current_info(f).history_valid);
    f->settings.render_scale = 1;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    EXPECT_FALSE(current_info(f).history_valid);
    view_frame(f);
    EXPECT_TRUE(current_info(f).history_valid);
    u64 before_transitions = de__views_resource_drains(f->r);
    if (backend == DE_GPU_METAL) {
        original_wait_vt = f->dev->vt;
        counted_wait_vt = *original_wait_vt;
        counted_wait_vt.wait_idle = count_wait_idle;
        counted_wait_calls = 0;
        f->dev->vt = &counted_wait_vt;
    }
    for (u32 cycle = 0; cycle < 3; ++cycle) {
        f->settings.render_scale = 97.0f / 128.0f;
        de_view_set_settings(f->r, f->view, &f->settings);
        view_frame(f);
        EXPECT_FALSE(current_info(f).history_valid);
        view_frame(f);
        EXPECT_TRUE(current_info(f).history_valid);
        f->settings.render_scale = 1;
        de_view_set_settings(f->r, f->view, &f->settings);
        view_frame(f);
        EXPECT_FALSE(current_info(f).history_valid);
        view_frame(f);
        EXPECT_TRUE(current_info(f).history_valid);
    }
    if (backend == DE_GPU_METAL) {
        f->dev->vt = original_wait_vt;
        /* view_frame explicitly waits once. No global drain may be added by resize. */
        EXPECT_EQ(counted_wait_calls, 12u);
        printf("resize: %u explicit waits across 12 frames, zero extra retirement drains\n",
               counted_wait_calls);
        EXPECT_EQ(de__views_resource_drains(f->r), before_transitions);
    }
    // Warm both persistent-history access variants and their graph placements after resize.
    view_frame(f);
    view_frame(f);
    u64 drains = de__views_resource_drains(f->r);
    for (u32 frame = 0; frame < 4; ++frame)
        view_frame(f);
    EXPECT_EQ(de__views_resource_drains(f->r), drains);
    de_view old = f->view;
    de_view_destroy(f->r, old);
    f->view = de_view_create(
        f->r, &(de_view_desc){.target = f->textures[0], .name = "ssr.recycled_view"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->view));
    EXPECT_NE(old.id, f->view.id);
    f->camera.camera.view = f->view;
    de_view_set_settings(f->r, f->view, &f->settings);
    view_frame(f);
    EXPECT_FALSE(current_info(f).history_valid);
    view_frame(f);
    EXPECT_TRUE(current_info(f).history_valid);
}
static void pipeline_history(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, 0.25f, 1000);
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
    de__ssr_info info = current_info(f);
    EXPECT_TRUE(info.history_valid);
    EXPECT_EQ(info.dispatches, 2u);
    size_t count = (size_t)f->width * f->height;
    f32 *moments = malloc(count * 16);
    ASSERT_TRUE(moments);
    read_texture(f, info.moments, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE, 16, moments);
    u32 accumulated = 0;
    for (size_t i = 0; i < count; ++i) {
        for (u32 c = 0; c < 4; ++c)
            EXPECT_TRUE(isfinite(moments[i * 4 + c]));
        if (moments[i * 4 + 2] > 4)
            ++accumulated;
    }
    EXPECT_TRUE(accumulated > 100);
    free(moments);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
}
#define SSR_LIFECYCLE_TESTS(suffix, backend)                                                       \
    TEST_F(ssr, ssr_resize_view_generation_##suffix, sfx) {                                        \
        resize_generation(f, backend);                                                             \
    }                                                                                              \
    TEST_F(ssr, ssr_pipelined_history_##suffix, sfx) {                                             \
        pipeline_history(f, backend);                                                              \
    }
SSR_LIFECYCLE_TESTS(metal, DE_GPU_METAL)
SSR_LIFECYCLE_TESTS(vulkan, DE_GPU_VULKAN)
SSR_LIFECYCLE_TESTS(d3d12, DE_GPU_D3D12)

static void history_footprint(sfx *f, de_gpu_backend backend) {
    open_gpu(f, backend);
    f->width = f->height = 4;
    const char *entry = "cs_ssr_history_queries";
    const u32 group[] = {8, 8, 1};
    de__engine_shader_code code;
    de__vis_shared *vis = de__views_vis(f->r);
    ASSERT_EQ(de__vis_shader_load(vis, "ssr_resolve", &entry, 1, &code), DE_OK);
    ASSERT_EQ(de__vis_kernel_create(vis, &code, 0, entry, group, 16, &f->kernel), DE_OK);
    de__engine_shader_code_free(&code);
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){.size = 2048, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_COPY_SRC});
    ASSERT_TRUE(upload.id);
    f->buffers[f->buffer_count++] = upload;
    de_gpu_texture images[3];
    for (u32 i = 0; i < 3; ++i) {
        images[i] = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.width = 4,
                                           .height = 4,
                                           .format = DE_FMT_RGBA32_FLOAT,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST |
                                                    DE_USAGE_COPY_SRC});
        ASSERT_TRUE(images[i].id);
        f->textures[f->texture_count++] = images[i];
    }
    for (u32 test = 0; test < 20; ++test) {
        /* Stationary cases: 10 depth, 11 normal, 12 distant depth, 13 nearest-invalid,
         * 14 NaN normal, 15 NaN colour, 16/17 tilted near/far, 18 all-invalid distant,
         * 19 orthographic tilted plane. Valid neighbours must survive partial rejection. */
        const bool distant = (test >= 6 && test <= 8) || test == 12 || test == 17 || test == 18;
        const bool tilted = test == 16 || test == 17 || test == 19;
        u8 *data = de_gpu_buffer_map(f->dev, upload);
        memset(data, 0, 2048);
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) {
                float *color = (float *)(data + y * 256 + x * 16);
                float *surface = (float *)(data + 1024 + y * 256 + x * 16);
                bool poison = y < 2 && test != 0 && test != 7 && !tilted;
                color[0] = color[1] = color[2] = poison ? 100.0f : 1.0f;
                color[3] = 1;
                surface[0] = distant ? .001f : .5f;
                surface[3] = 1;
                if ((test == 1 && poison) || test == 3)
                    surface[0] = .25f;
                if (test == 2 && poison) {
                    surface[1] = 1;
                    surface[3] = 0;
                }
                if (test == 5 && poison)
                    color[0] = NAN;
                if ((test == 6 && poison) || test == 8)
                    surface[0] = .00101f; /* A ~1m separation at 100m passes a raw depth epsilon. */
                if ((test == 10 && poison) || (test == 13 && x == 2 && y == 2)) {
                    surface[0] = .25f;
                    color[0] = color[1] = color[2] = 100.0f;
                }
                if (test == 13 && !(x == 2 && y == 2))
                    color[0] = color[1] = color[2] = 1.0f;
                if (test == 11 && poison) {
                    surface[1] = 1;
                    surface[3] = 0;
                }
                if ((test == 12 && poison) || test == 18)
                    surface[0] = .00101f;
                if (test == 14 && poison)
                    surface[1] = NAN;
                if (test == 15 && poison)
                    color[0] = NAN;
                if (tilted)
                    surface[0] += (distant ? .0002f : .2f) * (((f32)x + .5f) * .5f - 1.0f);
                if (test == 9) {
                    /* Equal-time bright/low-confidence and dim/high-confidence samples:
                     * mean contribution .9, mean confidence .5, conditional radiance 1.8.
                     * Independent RGB/alpha averages would incorrectly contribute 2.5. */
                    color[0] = color[1] = color[2] = 1;
                    color[3] = .9f;
                    surface[0] = surface[1] = surface[2] = 9;
                    surface[3] = .1f;
                }
            }
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "ssr.history.footprint");
        ASSERT_TRUE(cmd);
        for (u32 i = 0; i < 2; ++i) {
            de_gpu_barrier b = {.texture = images[i], .after = DE_ACCESS_COPY_DST};
            de_gpu_cmd_barriers(cmd, &b, 1);
            de_gpu_cmd_copy_buffer_to_texture(cmd, images[i], 0, 0, upload, i * 1024, 256);
            b.before = DE_ACCESS_COPY_DST;
            b.after = DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE;
            de_gpu_cmd_barriers(cmd, &b, 1);
        }
        de_gpu_barrier b = {.texture = images[2], .after = DE_ACCESS_SHADER_WRITE};
        de_gpu_cmd_barriers(cmd, &b, 1);
        u32 pc[4] = {de_gpu_texture_bindless(f->dev, images[0], 0, true),
                     de_gpu_texture_bindless(f->dev, images[1], 0, true),
                     de_gpu_texture_bindless(f->dev, images[2], 0, true),
                     test == 9 ? 4u
                               : (test == 4 ? 0u
                                            : (1u | (distant ? 2u : 0u) | (test >= 10 ? 8u : 0u) |
                                               (tilted ? 16u : 0u) | (test == 19 ? 32u : 0u)))};
        de_gpu_cmd_set_pipeline(cmd, f->kernel.pipeline);
        de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, 1, 1, 1);
        ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                      &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
                  DE_OK);
        de_gpu_wait_idle(f->dev);
        float result[4 * 4 * 4];
        read_texture(f, images[2], DE_ACCESS_SHADER_WRITE, 16, result);
        printf("history footprint case %u: radiance=%g coverage=%g\n", test, (double)result[0],
               (double)result[3]);
        for (u32 i = 0; i < 16; ++i) {
            const bool moving = test == 9 && i % 4 == 1;
            const f32 expected_color =
                test == 9 ? (moving ? 5.0f : 1.8f)
                          : (test == 3 || test == 4 || test == 8 || test == 18 ? 0.0f : 1.0f);
            EXPECT_FLOAT_EQ(result[i * 4], expected_color, 1e-6);
            EXPECT_TRUE(isfinite(result[i * 4]));
            const f32 expected_coverage =
                test == 3 || test == 4 || test == 8 || test == 18 ? 0.0f
                : test == 13                                      ? .75f
                : test == 9                                       ? (moving ? .9f : .5f)
                : test == 1 || test == 2 || test == 5 || test == 6 || (test >= 10 && test <= 15)
                    ? .5f
                    : 1.0f;
            EXPECT_FLOAT_EQ(result[i * 4 + 3], expected_coverage, 1e-6);
        }
    }
}
#define HISTORY_FOOTPRINT_TEST(tag, backend)                                                       \
    TEST_F(ssr, history_footprint_##tag, sfx) {                                                    \
        history_footprint(f, backend);                                                             \
    }
HISTORY_FOOTPRINT_TEST(metal, DE_GPU_METAL)
HISTORY_FOOTPRINT_TEST(vulkan, DE_GPU_VULKAN)
HISTORY_FOOTPRINT_TEST(d3d12, DE_GPU_D3D12)

static void simultaneous_history(sfx *f, de_gpu_backend backend) {
    mirror_scene(f, backend, .25f, 10);
    f->settings.ao = DE_QUALITY_LOW;
    f->settings.reflections = DE_QUALITY_LOW;
    de_view_set_settings(f->r, f->view, &f->settings);
    de_gpu_texture target = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = 128,
                                       .height = 128,
                                       .format = DE_FMT_RGBA8_UNORM,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    ASSERT_TRUE(target.id);
    f->textures[f->texture_count++] = target;
    de_view second =
        de_view_create(f->r, &(de_view_desc){.target = target, .name = "ssr.independent"});
    ASSERT_TRUE(second.id);
    de_view_set_settings(f->r, second, &f->settings);
    de__rw_view cameras[2] = {f->camera, f->camera};
    cameras[1].entity = 101;
    cameras[1].camera.view = second;
    cameras[1].camera.exposure_ev100 += 4;
    f->scene.f.views = cameras;
    f->scene.f.view_count = 2;
    for (u32 frame = 0; frame < 10; ++frame) {
        if (frame == 2) {
            cameras[0].camera.projection = DE_PROJ_ORTHOGRAPHIC;
            cameras[0].camera.ortho_height = 8;
        }
        if (frame == 4)
            cameras[0].camera.projection = DE_PROJ_PERSPECTIVE;
        if (frame == 6) {
            de_render_settings scaled = f->settings;
            scaled.render_scale = .75f;
            de_view_set_settings(f->r, second, &scaled);
        }
        if (frame == 8) {
            camera_at(f, (de_vec3){0, 20, 3}, (de_vec3){0, 0, .8f});
            cameras[0] = f->camera;
        }
        view_frame(f);
        de__ssr_info first, other;
        ASSERT_TRUE(de__ssr_view_info(f->r->ssr, f->view, &first));
        ASSERT_TRUE(de__ssr_view_info(f->r->ssr, second, &other));
        EXPECT_EQ(first.history_valid, frame != 0 && frame != 2 && frame != 4 && frame != 8);
        EXPECT_EQ(other.history_valid, frame != 0 && frame != 6);
        de__gtao_info ao_first, ao_other;
        ASSERT_TRUE(de__gtao_get(f->r->gtao, f->view, &ao_first));
        ASSERT_TRUE(de__gtao_get(f->r->gtao, second, &ao_other));
        EXPECT_EQ(ao_first.history_valid, frame != 0 && frame != 2 && frame != 4 && frame != 8);
        EXPECT_EQ(ao_other.history_valid, frame != 0 && frame != 6);
        EXPECT_NE(ao_first.history.id, ao_other.history.id);
        EXPECT_NE(first.temporal.id, other.temporal.id);
        EXPECT_NE(first.moments.id, other.moments.id);
        EXPECT_FLOAT_EQ(first.current_exposure / other.current_exposure, 16, .001f);
    }
    for (u32 frame = 0; frame < 4; ++frame)
        view_frame(f);
    const u64 warmed_bytes = de_tracking_allocator_stats(&f->tracking).bytes_live;
    for (u32 frame = 0; frame < 32; ++frame) {
        view_frame(f);
        EXPECT_TRUE(de_tracking_allocator_stats(&f->tracking).bytes_live <= warmed_bytes);
    }
    printf("two-view warmed allocation high-water: %llu bytes across 32 frames\n",
           (unsigned long long)warmed_bytes);
    // Avoid retaining a pointer to this stack fixture during teardown.
    f->scene.f.views = &f->camera;
    f->scene.f.view_count = 1;
}
#define SIMULTANEOUS_HISTORY_TEST(tag, backend)                                                    \
    TEST_F(ssr, simultaneous_projection_cut_resize_##tag, sfx) {                                   \
        simultaneous_history(f, backend);                                                          \
    }
SIMULTANEOUS_HISTORY_TEST(metal, DE_GPU_METAL)
SIMULTANEOUS_HISTORY_TEST(vulkan, DE_GPU_VULKAN)
SIMULTANEOUS_HISTORY_TEST(d3d12, DE_GPU_D3D12)
