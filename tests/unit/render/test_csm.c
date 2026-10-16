#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_test.h"
#include "flip.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>
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
#include "render/views_internal.h"
#include "render/probes/probes_internal.h"
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
#include "render/shadows/shadows_internal.h"

TEST(csm, practical_splits) {
    f32 s[5];
    de__csm_splits(.1f, 200.f, 4, s);
    EXPECT_FLOAT_EQ(s[0], .1f, 1e-6);
    EXPECT_FLOAT_EQ(s[4], 200, 1e-5);
    for (u32 i = 1; i < 4; ++i) {
        const double t = (double)i / 4.;
        EXPECT_FLOAT_EQ(s[i], .75 * .1 * pow(2000., t) + .25 * (.1 + 199.9 * t), 2e-5);
        EXPECT_TRUE(s[i] > s[i - 1]);
    }
}
/* Compare every fit payload byte exactly, excluding aligned aggregate tail padding. */
static void expect_same_fit(const de__csm_cascade *a, const de__csm_cascade *b) {
    EXPECT_EQ(memcmp(&a->view, &b->view, sizeof a->view), 0);
    EXPECT_EQ(memcmp(&a->proj, &b->proj, sizeof a->proj), 0);
    EXPECT_EQ(memcmp(&a->center, &b->center, sizeof a->center), 0);
    EXPECT_EQ(memcmp(&a->radius, &b->radius, sizeof a->radius), 0);
    EXPECT_EQ(memcmp(&a->texel, &b->texel, sizeof a->texel), 0);
    EXPECT_EQ(memcmp(&a->near_distance, &b->near_distance, sizeof a->near_distance), 0);
    EXPECT_EQ(memcmp(&a->far_distance, &b->far_distance, sizeof a->far_distance), 0);
}
typedef struct depth_copy {
    de_rg_resource depth, mirror, scratch, readback;
    u32 layer, pitch;
} depth_copy;
static void read_depth(de_gpu_cmd *cmd, const de_rg_pass *p, void *u) {
    depth_copy *d = u;
    de_gpu_buffer rb = de_rg_buffer(p, d->readback);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, d->pitch, de_rg_texture(p, d->depth), 0,
                                      d->layer);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 16384, d->pitch, de_rg_texture(p, d->mirror), 0, 0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 32768, d->pitch, de_rg_texture(p, d->depth), 0, 6);
}
static const de__gpu_backend_vtable *shadow_original_vt;
static bool shadow_reported_failure;
static de_result fail_shadow_cull_upload(de_gpu_device *dev, de__gpu_buffer *buffer,
                                         const de_gpu_buffer_desc *desc) {
    if (desc->name && strcmp(desc->name, "vis cull view 1") == 0)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected shadow cull upload failure");
    return shadow_original_vt->buffer_create(dev, buffer, desc);
}
static de_result report_shadow_submit_failure(de_gpu_device *dev, de_gpu_queue queue,
                                              const de_gpu_submit *submit, u64 value) {
    de_result r = shadow_original_vt->queue_submit(dev, queue, submit, value);
    if (r == DE_OK && submit->cmd_count && !shadow_reported_failure) {
        shadow_reported_failure = true;
        return DE_FAIL(DE_ERR_INTERNAL, "injected shadow reported submit failure");
    }
    return r;
}
static u32 shadow_texture_index;
static u32 shadow_texture_resolver(void *user, de_texture texture) {
    (void)user;
    return texture.id == 1 ? shadow_texture_index : 0;
}
static de_gpu_texture alpha_texture(lx *f) {
    de_gpu_texture texture = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                       .width = 64,
                                       .height = 64,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    ASSERT_TRUE(texture.id);
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){.size = 16384, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    ASSERT_TRUE(upload.id);
    u8 *pixels = de_gpu_buffer_map(f->dev, upload);
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x) {
            u32 i = 4 * (y * 64 + x);
            pixels[i] = pixels[i + 1] = pixels[i + 2] = 255;
            pixels[i + 3] = x < 32 ? 0 : 255;
        }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "shadow.alpha.upload");
    ASSERT_TRUE(cmd);
    de_gpu_barrier b = {.texture = texture, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, texture, 0, 0, upload, 0, 256);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, upload);
    return texture;
}
static void shadow_depth_test(lx *f, de_gpu_backend backend, bool mesh) {
    lx_open(f, backend);
    /* Exhaust a bounded graph arena, not the 64 GiB default reservation: Debug/sanitizers
     * poison every pushed/reset byte, and the failure case needs only a small scene graph. */
    de_arena_destroy(f->arena);
    f->arena = NULL;
    ASSERT_EQ(
        de_arena_create(&(de_arena_desc){.reserve_bytes = 1u << 20, .name = "bounded shadow depth"},
                        &f->arena),
        DE_OK);
    if (mesh && !de_gpu_get_caps(f->dev)->mesh_shaders)
        SKIP("no mesh shaders; compute family executes");
    cvar_set("r.raster_path", mesh ? "mesh" : "compute");
    de_gpu_texture alpha = alpha_texture(f);
    shadow_texture_index = de_gpu_texture_bindless(f->dev, alpha, 0, false);
    de__gpu_scene_destroy(f->gs);
    f->gs = NULL;
    ASSERT_EQ(de__gpu_scene_create(
                  &(de__gpu_scene_desc){.device = f->dev,
                                        .allocator = &f->alloc,
                                        .resolver = {.mesh = fake_mesh,
                                                     .material = fake_material,
                                                     .texture_bindless = shadow_texture_resolver},
                                        .staging_bytes = (u64)16 << 20},
                  &f->gs),
              DE_OK);
    u32 material = material_add(.5f, .5f, 0, .5f), mask = material_add(.5f, .5f, 0, .5f);
    g_mat[mask - 1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[mask - 1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[mask - 1].standard.alpha_cutoff = .5f;
    g_mat[mask - 1].standard.base_color.w = .1f;
    u32 plane = lx_mesh_plane(f, 1, 1, .8f);
    for (u32 i = 0; i < 3; ++i) {
        world_set(&f->w, i, plane, at((f32)i - 1.f, 0, 1), i == 1 ? mask : material);
        if (i != 2)
            f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
    }
    de__shadow_views *sv = NULL;
    ASSERT_EQ(de__shadow_views_create(f->dev, &f->alloc, &sv), DE_OK);
    de_gpu_texture depth = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.dim = DE_TEX_2D_ARRAY,
                                       .format = DE_FMT_D32_FLOAT,
                                       .width = 64,
                                       .height = 64,
                                       .depth_or_layers = 8,
                                       .usage = DE_USAGE_DEPTH | DE_USAGE_COPY_SRC,
                                       .name = "test canonical shadow array"});
    de_gpu_texture mirror = de_gpu_texture_create(
        f->dev,
        &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                               .width = 64,
                               .height = 64,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                               .name = "test shadow mirror"});
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = 49152,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "test shadow readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(depth) && DE_HANDLE_VALID(mirror) && DE_HANDLE_VALID(rb));
    f32 prior[4096] = {0};
    for (u32 pass = 0; pass < 14; ++pass) {
        if (pass) {
            g_mat[mask - 1].standard.base_color.w = .9f;
            f->w.touch_all = true;
        }
        if (pass == 2)
            for (u32 i = 0; i < 3; ++i) {
                world_set(&f->w, i, plane,
                          trs((de_vec3){(f32)i - 1, 0, 7}, (de_vec3){0, 1, 0}, .12f, 80),
                          i == 1 ? mask : material);
                if (i != 2)
                    f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
            }
        if (pass == 12) {
            g_mat[mask - 1].textures[DE_COOKED_TEX_BASE_COLOR] = (de_texture){.id = 1};
            for (u32 i = 0; i < 3; ++i) {
                world_set(&f->w, i, plane, at((f32)i - 1, 0, 1), i == 1 ? mask : material);
                if (i != 2)
                    f->w.f.instances.flags[i] |= DE__RW_INST_CAST_SHADOWS;
            }
        }
        world_begin(&f->w);
        ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
        de_rg_resource dr =
            de_rg_import_texture(f->g, depth, pass ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE);
        de_rg_resource mr =
            de_rg_import_texture(f->g, mirror, pass ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE);
        de__vis_context *vc = NULL;
        de__shadow_view_desc desc = {
            .scene = f->gs,
            .depth = dr,
            .context = 0,
            .layer = 3,
            .width = 64,
            .height = 64,
            .view = de_m4_look_at((de_vec3){0, 0, 10}, (de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}),
            .proj = de_m4_ortho_rz(-2, 2, -2, 2, 0, 20),
            .camera = {0, 0, 10},
            .frame = pass,
            .frames_completed = pass,
            .clear_depth = true};
        if (pass >= 2 && pass < 12) {
            de__csm_camera cam = {.position = {.123f, .456f, 10},
                                  .forward = {0, 0, -1},
                                  .origin = {1000000, 1000000, 0},
                                  .near_z = .1f,
                                  .distance = 50,
                                  .tan_half_fov = .6f,
                                  .aspect = 1};
            de_vec3 direction = {.2f, .3f, -1};
            if (pass < 4) {
                cam.position = (de_vec3){0, 0, 10};
                cam.origin = (de_dvec3){0};
                direction = (de_vec3){0, 0, -1};
            }
            de__csm_cascade c[4], base[4];
            ASSERT_EQ(de__csm_fit(&cam, direction, 4, 64, base), DE_OK);
            if (pass & 1u) {
                cam.position.x += base[0].texel * (pass < 4 ? .05f : .001f);
                cam.forward = de_v3_normalize((de_vec3){pass < 4 ? 1e-7f : 1e-8f, 0, -1});
            }
            ASSERT_EQ(de__csm_fit(&cam, direction, 4, 64, c), DE_OK);
            for (u32 i = 0; i < 4; ++i)
                expect_same_fit(&base[i], &c[i]);
            u32 cascade = pass < 4 ? 0 : (pass - 4) / 2;
            desc.view = c[cascade].view;
            desc.proj = c[cascade].proj;
        }
        if (pass >= 12)
            desc.proj = de_m4_perspective_rz(.8f, 1, .1f);
        if (pass == 0) {
            de_arena_mark mark = de_arena_mark_get(f->arena);
            de_arena_info ai = de_arena_get_info(f->arena);
            ASSERT_TRUE(de_arena_push(f->arena, ai.reserved - ai.used - 128, 1) != NULL);
            EXPECT_EQ(de__shadow_view_build(sv, f->g, &desc, &vc), DE_ERR_OUT_OF_MEMORY);
            de_arena_reset_to(f->arena, mark);
            de_error_clear();
        }
        ASSERT_EQ(de__shadow_view_build(sv, f->g, &desc, &vc), DE_OK);
        EXPECT_EQ(de__shadow_view_build(sv, f->g, &desc, NULL), DE_ERR_INVALID_ARGUMENT);
        de_error_clear();
        if (pass == 0) {
            de__shadow_view_desc failed = desc;
            failed.context = 1;
            shadow_original_vt = f->dev->vt;
            de__gpu_backend_vtable failing = *shadow_original_vt;
            failing.buffer_create = fail_shadow_cull_upload;
            f->dev->vt = &failing;
            de_result result = de__shadow_view_build(sv, f->g, &failed, NULL);
            f->dev->vt = shadow_original_vt;
            EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
            de_error_clear();
            de__shadow_views_end(sv, f->g, false);
            de__gpu_scene_frame_end(f->gs, f->g, false);
            de__rg_reset(f->g);
            de_arena_reset(f->arena);
            ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
            ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
            ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
            dr = de_rg_import_texture(f->g, depth, DE_ACCESS_NONE);
            mr = de_rg_import_texture(f->g, mirror, DE_ACCESS_NONE);
            desc.depth = dr;
            ASSERT_EQ(de__shadow_view_build(sv, f->g, &desc, &vc), DE_OK);
            failed.depth = dr;
            ASSERT_EQ(de__shadow_view_build(sv, f->g, &failed, NULL), DE_OK);
        }
        if (pass == 0) {
            de__shadow_view_desc distant = desc;
            distant.context = 65;
            distant.layer = 6;
            distant.view =
                de_m4_look_at((de_vec3){.5f, 0, 10}, (de_vec3){.5f, 0, 0}, (de_vec3){0, 1, 0});
            ASSERT_EQ(de__shadow_view_build(sv, f->g, &distant, NULL), DE_OK);
            EXPECT_GT(de__shadow_views_capacity(sv), 64u);
        }
        ASSERT_TRUE(vc && vc->depth_only && !DE_HANDLE_VALID(vc->vis_ids) &&
                    !DE_HANDLE_VALID(vc->hzb));
        de_rg_resource scratch = de_rg_create_buffer(
            f->g, &(de_gpu_buffer_desc){.size = 16384,
                                        .usage = DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                        .name = "shadow depth copy"});
        depth_copy *cp = de_arena_push(f->arena, sizeof *cp, 16);
        *cp = (depth_copy){
            dr,      mr,
            scratch, de_rg_import_buffer(f->g, rb, pass ? DE_ACCESS_COPY_DST : DE_ACCESS_NONE),
            3,       256};
        ASSERT_EQ(de__shadow_copy_layer(sv, f->g, dr, 3, mr, 64, 64), DE_OK);
        de_rg_pass *p;
        p = de_rg_add_pass(f->g, "shadow.test.readback", DE_QUEUE_GRAPHICS, read_depth, cp);
        de_rg_pass_side_effect(p);
        de_rg_read(p, dr, DE_ACCESS_COPY_SRC);
        de_rg_read(p, mr, DE_ACCESS_COPY_SRC);
        de_rg_write(p, cp->readback, DE_ACCESS_COPY_DST);
        de_rg_export(f->g, mr, DE_ACCESS_SHADER_READ);
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        ASSERT_EQ(de__rg_compile(f->g), DE_OK);
        shadow_original_vt = f->dev->vt;
        de__gpu_backend_vtable failing = *shadow_original_vt;
        if (pass == 1) {
            failing.queue_submit = report_shadow_submit_failure;
            shadow_reported_failure = false;
            f->dev->vt = &failing;
        }
        de_result executed = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = pass,
                                                                         .frame_fence = f->fence,
                                                                         .frame_value = pass + 1});
        f->dev->vt = shadow_original_vt;
        EXPECT_EQ(executed, pass == 1 ? DE_ERR_INTERNAL : DE_OK);
        if (pass == 1)
            EXPECT_TRUE(shadow_reported_failure);
        de_error_clear();
        de__shadow_views_end(sv, f->g, true);
        EXPECT_EQ(de__shadow_view_build(sv, f->g, &desc, NULL), DE_ERR_INVALID_ARGUMENT);
        de_error_clear();
        ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, pass + 1, FIRST_FRAME_WAIT_NS), DE_OK);
        de__shadow_views_end(sv, f->g, true);
        de__gpu_scene_frame_end(f->gs, f->g, true);
        const f32 *got = de_gpu_buffer_map(f->dev, rb);
        ASSERT_TRUE(got);
        EXPECT_EQ(memcmp(got, got + 4096, 16384), 0);
        if (pass == 0) {
            u32 checked = 0;
            EXPECT_NE(memcmp(got, got + 8192, 16384), 0);
            for (u32 y = 0; y < 64; ++y)
                for (u32 x = 0; x < 64; ++x) {
                    double wx = ((double)x + .5) / 16. - 1.5, wy = 2. - ((double)y + .5) / 16.;
                    if (fabs(fabs(wx + 1) - .4) < .02 || fabs(fabs(wy) - .4) < .02)
                        continue;
                    bool caster = fabs(wx + 1) < .4 && fabs(wy) < .4;
                    EXPECT_FLOAT_EQ(got[8192 + y * 64 + x], caster ? .55 : 0, 2e-6);
                    checked += caster;
                }
            EXPECT_GT(checked, 100u);
        }

        if (pass >= 2 && pass < 12) {
            u32 lit = 0;
            for (u32 i = 0; i < 4096; ++i)
                lit += got[i] > 0;
            EXPECT_TRUE(lit > 100);
            if (pass & 1u)
                EXPECT_EQ(memcmp(prior, got, sizeof prior), 0);
            memcpy(prior, got, sizeof prior);
            continue;
        }
        if (pass >= 12) {
            u32 covered = 0, transparent = 0;
            for (u32 y = 0; y < 64; ++y)
                for (u32 x = 0; x < 64; ++x) {
                    double wx = (((double)x + .5) / 32. - 1) * 9 * tan(.4),
                           wy = (1 - ((double)y + .5) / 32.) * 9 * tan(.4);
                    if (fabs(fabs(wx + 1) - .4) < .04 || fabs(fabs(wx) - .4) < .04 ||
                        fabs(fabs(wy) - .4) < .04 || fabs(wx) < .04)
                        continue;
                    bool left = fabs(wx + 1) < .4 && fabs(wy) < .4,
                         masked = wx > 0 && wx < .4 && fabs(wy) < .4;
                    EXPECT_FLOAT_EQ(got[y * 64 + x], left || masked ? .1 / 9 : 0, 2e-6);
                    covered += left || masked;
                    transparent += wx < 0 && wx > -.4 && fabs(wy) < .4;
                }
            EXPECT_GT(covered, 30u);
            EXPECT_GT(transparent, 5u);
            continue;
        }
        u32 covered = 0;
        for (u32 y = 0; y < 64; ++y)
            for (u32 x = 0; x < 64; ++x) {
                f64 wx = ((f64)x + .5) / 16. - 2., wy = 2. - ((f64)y + .5) / 16.;
                bool left = fabs(wx + 1.) < .39 && fabs(wy) < .39;
                bool center = fabs(wx) < .39 && fabs(wy) < .39;
                bool edge = (fabs(fabs(wx + 1.) - .4) < .02 || fabs(fabs(wx) - .4) < .02 ||
                             fabs(fabs(wy) - .4) < .02);
                if (edge)
                    continue;
                f64 expected = (left || (pass && center)) ? .55 : 0.;
                EXPECT_FLOAT_EQ(got[y * 64 + x], expected, 2e-6);
                covered += got[y * 64 + x] > 0;
            }
        EXPECT_TRUE(covered > 100);
        memcpy(prior, got, sizeof prior);
    }
    EXPECT_EQ(validation_errors(f->dev), 0);
    de__shadow_views_destroy(sv);
    de_gpu_texture_destroy(f->dev, depth);
    de_gpu_texture_destroy(f->dev, mirror);
    de_gpu_buffer_destroy(f->dev, rb);
    de_gpu_texture_destroy(f->dev, alpha);
    de_gpu_wait_idle(f->dev);
}
#define DEPTH_TESTS(tag, backend)                                                                  \
    TEST_F(csm, shadow_depth_compute_##tag, lx) {                                                  \
        shadow_depth_test(f, backend, false);                                                      \
    }                                                                                              \
    TEST_F(csm, shadow_depth_mesh_##tag, lx) {                                                     \
        shadow_depth_test(f, backend, true);                                                       \
    }
DEPTH_TESTS(metal, DE_GPU_METAL)
DEPTH_TESTS(vulkan, DE_GPU_VULKAN)
DEPTH_TESTS(d3d12, DE_GPU_D3D12)
TEST(csm, quality_and_coverage) {
    for (u32 quality = 1; quality <= 4; ++quality) {
        u32 n = 0, res = 0, taps = 0;
        de__csm_quality((de_quality)quality, &n, &res, &taps);
        EXPECT_EQ(n, quality == 1 ? 2u : quality == 2 ? 3u : 4u);
        EXPECT_EQ(res, quality == 1 ? 1024u : quality == 4 ? 4096u : 2048u);
        EXPECT_EQ(taps, 1u << (quality + 1));
        de__csm_camera cam = {.position = {0, 0, 10},
                              .forward = {0, 0, -1},
                              .near_z = .1f,
                              .distance = 200,
                              .tan_half_fov = .6f,
                              .aspect = 1.7f};
        de__csm_cascade c[4];
        ASSERT_EQ(de__csm_fit(&cam, (de_vec3){.2f, .3f, -1}, n, res, c), DE_OK);
        for (u32 z = 0; z < 101; ++z)
            for (u32 y = 0; y < 9; ++y)
                for (u32 x = 0; x < 9; ++x) {
                    f32 d = .1f + 199.9f * (f32)z / 100.f;
                    de_vec3 p = {(2.f * (f32)x / 8.f - 1) * d * cam.tan_half_fov * cam.aspect,
                                 (2.f * (f32)y / 8.f - 1) * d * cam.tan_half_fov, 10 - d};
                    bool covered = false;
                    for (u32 i = 0; i < n; ++i)
                        covered |=
                            de_v3_length_sq(de_v3_sub(p, c[i].center)) <= c[i].radius * c[i].radius;
                    ASSERT_TRUE(covered);
                }
    }
}
TEST(csm, stable_cpu_snapping) {
    EXPECT_EQ(sizeof(de_mat4), 64u);
    EXPECT_EQ(sizeof(de_vec3), 12u);
    EXPECT_EQ(offsetof(de__csm_cascade, center), 128u);
    EXPECT_EQ(offsetof(de__csm_cascade, far_distance) + sizeof(f32), 156u);
    printf("CSM fit layout: sizeof=%zu, payload_end=%zu, tail_padding=%zu\n",
           sizeof(de__csm_cascade), offsetof(de__csm_cascade, far_distance) + sizeof(f32),
           sizeof(de__csm_cascade) - (offsetof(de__csm_cascade, far_distance) + sizeof(f32)));
    de__csm_camera a = {.position = {0, 0, 10},
                        .forward = {0, 0, -1},
                        .near_z = .1f,
                        .distance = 200,
                        .tan_half_fov = .6f,
                        .aspect = 1.7f};
    de__csm_cascade c[4], d[4];
    ASSERT_EQ(de__csm_fit(&a, (de_vec3){0, 0, -1}, 4, 2048, c), DE_OK);
    a.position.x += c[0].texel * .05f;
    a.forward = de_v3_normalize((de_vec3){1e-7f, 0, -1});
    ASSERT_EQ(de__csm_fit(&a, (de_vec3){0, 0, -1}, 4, 2048, d), DE_OK);
    for (u32 i = 0; i < 4; ++i)
        expect_same_fit(&c[i], &d[i]);
}
typedef struct shadow_query {
    f32 position[4], normal[4];
    u32 identity[4];
} shadow_query;
static void shadow_queries(lx *f, const de__csm_gpu records[9], const shadow_query *queries,
                           u32 count, u32 mode, f32 radius, f32 *result) {
    fprintf(stderr, "CSM query count=%u mode=%u: buffers begin\n", count, mode);
    fflush(stderr);
    de_gpu_buffer metadata = upload_buf(f->dev, records, 9 * sizeof *records);
    de_gpu_buffer input = upload_buf(f->dev, queries, (u64)count * sizeof *queries);
    de_gpu_buffer output = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16,
                                      .stride = 16,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 16,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(metadata.id && input.id && output.id && rb.id);
    const char *entry = "cs_shadow_query";
    const u32 group[3] = {64, 1, 1};
    de__engine_shader_code code;
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(f->vis, "shadow_sample", &entry, 1, &code), DE_OK);
    fprintf(stderr, "CSM query count=%u mode=%u: kernel create begin\n", count, mode);
    fflush(stderr);
    ASSERT_EQ(de__vis_kernel_create(f->vis, &code, 0, entry, group, 32, &kernel), DE_OK);
    fprintf(stderr, "CSM query count=%u mode=%u: kernel create complete\n", count, mode);
    fflush(stderr);
    de__engine_shader_code_free(&code);
    u32 pc[8] = {de_gpu_buffer_bindless(f->dev, metadata),
                 de_gpu_buffer_bindless(f->dev, input),
                 de_gpu_buffer_bindless(f->dev, output),
                 count,
                 mode,
                 0,
                 0,
                 0};
    memcpy(&pc[5], &radius, 4);
    fprintf(stderr, "CSM query count=%u mode=%u: record begin\n", count, mode);
    fflush(stderr);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "shadow.query");
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
    fprintf(stderr, "CSM query count=%u mode=%u: submit begin\n", count, mode);
    fflush(stderr);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    fprintf(stderr, "CSM query count=%u mode=%u: submit complete; wait begin\n", count, mode);
    fflush(stderr);
    de_gpu_wait_idle(f->dev);
    fprintf(stderr, "CSM query count=%u mode=%u: wait complete; readback begin\n", count, mode);
    fflush(stderr);
    memcpy(result, de_gpu_buffer_map(f->dev, rb), (size_t)count * 16);
    fprintf(stderr, "CSM query count=%u mode=%u: readback complete\n", count, mode);
    fflush(stderr);
    de__vis_kernel_destroy(f->vis, &kernel);
    de_gpu_buffer_destroy(f->dev, metadata);
    de_gpu_buffer_destroy(f->dev, input);
    de_gpu_buffer_destroy(f->dev, output);
    de_gpu_buffer_destroy(f->dev, rb);
}
static de_gpu_texture shadow_test_texture(lx *f, const f32 pixels[4096]) {
    de_gpu_texture texture = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                       .width = 64,
                                       .height = 64,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    ASSERT_TRUE(texture.id);
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){.size = 16384, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    ASSERT_TRUE(upload.id);
    memcpy(de_gpu_buffer_map(f->dev, upload), pixels, 16384);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "shadow.test.upload");
    ASSERT_TRUE(cmd);
    de_gpu_barrier b = {.texture = texture, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, texture, 0, 0, upload, 0, 256);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, upload);
    return texture;
}
static void filter_test(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    f32 pixels[4096];
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x)
            pixels[y * 64 + x] = (x >= 16 && x < 48 && y >= 16 && y < 48)
                                     ? ((x + 3 * y) % 7 < 3 ? .75f : .25f)
                                     : 1.f;
    de_gpu_texture texture = shadow_test_texture(f, pixels);
    de__csm_gpu records[9] = {0};
    records[0].map =
        (de__shadow_map_ref){.texture = de_gpu_texture_bindless(f->dev, texture, 0, false),
                             .layer = 7,
                             .resolution = 64,
                             .uv_clamp = {16.5f / 64, 16.5f / 64, 47.5f / 64, 47.5f / 64}};
    shadow_query query[96] = {0};
    f32 result[96][4];
    for (u32 i = 0; i < 96; ++i) {
        query[i].position[0] = .24f + .52f * (f32)(i % 12) / 11;
        query[i].position[1] = .24f + .52f * (f32)(i / 12) / 7;
        query[i].position[2] = .5f;
    }
    u32 partial = 0;
    for (u32 taps = 4; taps <= 32; taps *= 2) {
        records[0].map.taps = taps;
        shadow_queries(f, records, query, 96, 0, 3.5f / 64, &result[0][0]);
        for (u32 i = 0; i < 96; ++i) {
            u32 lit = 0;
            for (u32 k = 0; k < taps; ++k) {
                double r = sqrt(((double)k + .5) / taps) * 3.5 / 64, angle = k * 2.399963229728653;
                double u = fmin(fmax(query[i].position[0] + r * cos(angle), 16.5 / 64), 47.5 / 64);
                double v = fmin(fmax(query[i].position[1] + r * sin(angle), 16.5 / 64), 47.5 / 64);
                lit += query[i].position[2] >= pixels[(u32)floor(v * 64) * 64 + (u32)floor(u * 64)];
            }
            EXPECT_FLOAT_EQ(result[i][0], (f64)lit / taps, 1e-6);
            partial += lit > 0 && lit < taps;
        }
    }
    EXPECT_GT(partial, 200u);
    // Reverse-Z and absent-map endpoints must remain exact for every kernel size.
    for (u32 i = 0; i < 96; ++i)
        query[i].position[2] = (i & 1) ? 0.f : 1.f;
    shadow_queries(f, records, query, 96, 0, 3.5f / 64, &result[0][0]);
    for (u32 i = 0; i < 96; ++i)
        EXPECT_FLOAT_EQ(result[i][0], (i & 1) ? 0 : 1, 0);
    EXPECT_EQ(validation_errors(f->dev), 0);
    de_gpu_texture_destroy(f->dev, texture);
}
static void sphere_blend_test(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    f32 pixels[4096];
    memset(pixels, 0, sizeof pixels);
    de_gpu_texture lit = shadow_test_texture(f, pixels);
    for (u32 i = 0; i < 4096; ++i)
        pixels[i] = .75f;
    de_gpu_texture dark = shadow_test_texture(f, pixels);
    const u32 count = 8001;
    shadow_query *queries = zalloc(count * sizeof *queries);
    f32 *result = zalloc(count * 16);
    u32 transitions = 0;
    for (u32 projection = 0; projection < 2; ++projection)
        for (u32 ray = 0; ray < 5; ++ray) {
            de__csm_camera cam = {.position = {3, -7, 10},
                                  .forward = de_v3_normalize((de_vec3){.3f, -.2f, -1}),
                                  .origin = {1000000, 1000000, 0},
                                  .near_z = .1f,
                                  .distance = 200,
                                  .tan_half_fov = .6f,
                                  .aspect = 1.7f,
                                  .ortho_half_height = projection ? 5.f : 0.f};
            de__csm_cascade cascades[4];
            ASSERT_EQ(de__csm_fit(&cam, (de_vec3){.2f, .3f, -1}, 4, 2048, cascades), DE_OK);
            de_vec3 right = de_v3_normalize(de_v3_cross(cam.forward, (de_vec3){0, 1, 0})),
                    up = de_v3_cross(right, cam.forward);
            de__csm_gpu records[9] = {0};
            records[0].identity[0] = 1;
            records[0].identity[1] = 4;
            records[0].sphere[0] = cam.position.x;
            records[0].sphere[1] = cam.position.y;
            records[0].sphere[2] = cam.position.z;
            records[0].sphere[3] = 200;
            records[0].light[0] = cam.forward.x;
            records[0].light[1] = cam.forward.y;
            records[0].light[2] = cam.forward.z;
            for (u32 c = 0; c < 4; ++c) {
                de__csm_gpu *r = &records[1 + c];
                r->identity[0] = 123;
                r->sphere[0] = cascades[c].center.x;
                r->sphere[1] = cascades[c].center.y;
                r->sphere[2] = cascades[c].center.z;
                r->sphere[3] = cascades[c].radius;
                r->matrix[2][3] = .5f;
                r->map = (de__shadow_map_ref){
                    .texture = de_gpu_texture_bindless(f->dev, (c & 1) ? dark : lit, 0, false),
                    .layer = c,
                    .resolution = 64,
                    .taps = 32,
                    .uv_clamp = {.5f / 64, .5f / 64, 63.5f / 64, 63.5f / 64}};
            }
            for (u32 i = 0; i < count; ++i) {
                float d = .1f + 199.8f * (f32)i / (f32)(count - 1), h = projection ? 5 : d * .6f;
                float rx = ray == 1   ? -.95f
                           : ray == 2 ? .95f
                                      : 0,
                      ry = ray == 3   ? -.95f
                           : ray == 4 ? .95f
                                      : 0;
                de_vec3 p = de_v3_add(cam.position, de_v3_scale(cam.forward, d));
                p = de_v3_add(
                    p, de_v3_add(de_v3_scale(right, rx * h * cam.aspect), de_v3_scale(up, ry * h)));
                memcpy(queries[i].position, &p, 12);
                queries[i].normal[2] = 1;
                queries[i].identity[0] = 123;
            }
            shadow_queries(f, records, queries, count, 1, 0, result);
            u32 previous_layer = 0;
            float previous = result[0];
            for (u32 i = 0; i < count; ++i) {
                de_vec3 p;
                memcpy(&p, queries[i].position, 12);
                bool found = false;
                for (u32 c = 0; c < 4; ++c) {
                    double radius =
                        de_v3_length(de_v3_sub(p, cascades[c].center)) / cascades[c].radius;
                    if (radius > 1)
                        continue;
                    found = true;
                    double expected = (c & 1) ? 0 : 1;
                    if (c < 3 && radius > .9) {
                        // Every visible receiver in a selected fade band must also have next-map
                        // coverage.
                        EXPECT_TRUE(de_v3_length(de_v3_sub(p, cascades[c + 1].center)) <=
                                    cascades[c + 1].radius);
                        double t = (radius - .9) / .1;
                        t = fmin(fmax(t, 0), 1);
                        t = t * t * (3 - 2 * t);
                        expected = expected * (1 - t) + (1 - expected) * t;
                    }
                    EXPECT_FLOAT_EQ(result[i * 4], expected, 3e-5);
                    if (i) {
                        EXPECT_TRUE(fabsf(result[i * 4] - previous) < .04f);
                        transitions += c != previous_layer;
                    }
                    previous_layer = c;
                    break;
                }
                EXPECT_TRUE(found);
                previous = result[i * 4];
            }
        }
    EXPECT_GT(transitions, 10u);
    printf(
        "actual sphere-boundary selection transitions=%u (angled perspective/orthographic rays)\n",
        transitions);
    EXPECT_EQ(validation_errors(f->dev), 0);
    free(queries);
    free(result);
    de_gpu_texture_destroy(f->dev, lit);
    de_gpu_texture_destroy(f->dev, dark);
}
#define BLEND_TEST(tag, backend)                                                                   \
    TEST_F(csm, sphere_selection_blend_##tag, lx) {                                                \
        sphere_blend_test(f, backend);                                                             \
    }
BLEND_TEST(metal, DE_GPU_METAL)
BLEND_TEST(vulkan, DE_GPU_VULKAN)
BLEND_TEST(d3d12, DE_GPU_D3D12)
#define FILTER_TEST(tag, backend)                                                                  \
    TEST_F(csm, pcf_kernel_and_clamp_##tag, lx) {                                                  \
        filter_test(f, backend);                                                                   \
    }
FILTER_TEST(metal, DE_GPU_METAL)
FILTER_TEST(vulkan, DE_GPU_VULKAN)
FILTER_TEST(d3d12, DE_GPU_D3D12)

static void forward_shadow_test(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    f->width = 129;
    f->height = 83;
    f->eye = (de_vec3){0, 0, 10};
    f->target = (de_vec3){0};
    ASSERT_EQ(de__csm_create_ex(f->dev, &f->alloc, &f->csm), DE_OK);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    f->shadow_quality = DE_QUALITY_LOW;
    // Exactly representable deferred values (sRGB128, roughness153) isolate shadow sampling.
    u32 mat = material_add(.2158605f, .6f, 0, .5f), receiver = material_add(.2158605f, .6f, 0, .5f);
    g_mat[mat - 1].standard.ior = g_mat[receiver - 1].standard.ior = 1.5f;
    g_mat[mat - 1].standard.specular_color = g_mat[receiver - 1].standard.specular_color =
        (de_vec3){1, 1, 1};
    u32 floor = lx_mesh_plane(f, 1, 1, 30), caster = lx_mesh_plane(f, 1, 1, 4),
        surface = lx_mesh_plane(f, 1, 1, 8);
    world_set(&f->w, 0, floor, at(0, 0, 0), mat);
    world_set(&f->w, 1, caster, at(0, 0, 2), mat);
    f->w.f.instances.flags[1] |= DE__RW_INST_CAST_SHADOWS;
    DeLight sun = {
        .kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 10, .cast_shadows = true};
    world_light(&f->w, 0, &sun, trs((de_vec3){0}, (de_vec3){0, 1, 0}, .6f, 1));
    u32 pixels = f->width * f->height;
    f32 *forward = zalloc((size_t)pixels * 16), *unshadowed = zalloc((size_t)pixels * 16);
    u32 dark = 0;
    for (u32 phase = 0; phase < 2; ++phase) {
        f32 z = phase ? .5f : 3;
        world_set(&f->w, 2, surface, at(0, 0, z), receiver);
        g_mat[receiver - 1].alpha_mode = DE_COOKED_ALPHA_BLEND;
        g_mat[receiver - 1].standard.alpha_mode = DE_COOKED_ALPHA_BLEND;
        f->shadow_quality = DE_QUALITY_OFF;
        ASSERT_TRUE(lx_frame(f));
        memcpy(unshadowed, f->res.lighting, (size_t)pixels * 16);
        f->shadow_quality = DE_QUALITY_LOW;
        ASSERT_TRUE(lx_frame(f));
        memcpy(forward, f->res.lighting, (size_t)pixels * 16);
        g_mat[receiver - 1].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
        g_mat[receiver - 1].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
        f->w.touch_all = true;
        ASSERT_TRUE(lx_frame(f));
        u32 checked = 0;
        for (u32 i = 0; i < pixels; ++i) {
            f32 *p = f->res.pos + i * 4;
            if (p[3] < .5f || fabsf(p[2] - z) > .001f || fabsf(p[0]) > 3.8f || fabsf(p[1]) > 3.8f)
                continue;
            ++checked;
            for (u32 ch = 0; ch < 3; ++ch)
                EXPECT_FLOAT_EQ(forward[i * 4 + ch], f->res.lighting[i * 4 + ch],
                                .01f * fmaxf(f->res.lighting[i * 4 + ch], .01f));
            if (!phase)
                EXPECT_FLOAT_EQ(forward[i * 4], unshadowed[i * 4], .002f);
            else
                dark += forward[i * 4] < unshadowed[i * 4] * .1f;
        }
        EXPECT_GT(checked, 100u);
    }
    EXPECT_GT(dark, 10u);
    printf("own-surface forward shadows: %u shadowed pixels, above-caster surface stays lit\n",
           dark);
    EXPECT_EQ(validation_errors(f->dev), 0);
    free(forward);
    free(unshadowed);
}
#define FORWARD_TEST(tag, backend)                                                                 \
    TEST_F(csm, forward_own_surface_##tag, lx) {                                                   \
        forward_shadow_test(f, backend);                                                           \
    }
FORWARD_TEST(metal, DE_GPU_METAL)
FORWARD_TEST(vulkan, DE_GPU_VULKAN)
FORWARD_TEST(d3d12, DE_GPU_D3D12)
static void shadow_image_bytes(const f32 *hdr, u32 pixels, u8 *image) {
    for (u32 i = 0; i < pixels * 4; ++i) {
        f32 v = fmaxf(hdr[i], 0);
        v = v / (1 + v);
        v = v <= .0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - .055f;
        image[i] = (u8)floorf(fminf(v, 1) * 255 + .5f);
    }
}
static void large_world_images(lx *f) {
    u32 pixels = f->width * f->height;
    u8 *reference = zalloc((size_t)pixels * 4), *shifted = zalloc((size_t)pixels * 4);
    f32 *errors = zalloc((size_t)pixels * 4);
    shadow_image_bytes(f->res.lighting, pixels, reference);
    f->w.f.origin = (de_dvec3){1000000, 1000000, 0};
    f->w.touch_all = true;
    ASSERT_TRUE(lx_frame(f));
    shadow_image_bytes(f->res.lighting, pixels, shifted);
    de_flip_image a = {reference, f->width, f->height, 4, 0},
                  b = {shifted, f->width, f->height, 4, 0};
    ASSERT_TRUE(de_flip_ldr(&a, &b, 0, errors));
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(errors, pixels, &stats));
    printf("large-world shadow image FLIP: mean=%.8f p99=%.8f max=%.8f\n", stats.mean, stats.p99,
           stats.max);
    EXPECT_TRUE(stats.mean <= .005);
    EXPECT_TRUE(stats.p99 <= .05);
    f->w.f.origin = (de_dvec3){0};
    f->w.touch_all = true;
    free(reference);
    free(shifted);
    free(errors);
}
static void capacity_captures(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_type_registry *types = NULL;
    de_world *ecs = NULL;
    de_renderer *renderer = NULL;
    ASSERT_EQ(de_type_registry_create(&f->alloc, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = types}, &ecs),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .types = types,
                                                     .allocator = &f->alloc,
                                                     .frames_in_flight = 2},
                                 &renderer),
              DE_OK);
    de_renderer_attach(renderer, ecs);
    cvar_set("r.pipelined", "0");
    cvar_set("r.csm_lights", "2");
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    settings.shadows = DE_QUALITY_ULTRA;
    settings.gi = DE_QUALITY_OFF;
    settings.reflections = DE_QUALITY_OFF;
    settings.ao = DE_QUALITY_OFF;
    settings.upscaler = DE_UPSCALE_NONE;
    settings.render_scale = .25f;
    de_renderer_set_settings(renderer, &settings);
    de__probe_capture captures[4] = {0};
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_EQ(de__probe_capture_init(&captures[i], renderer, 32), DE_OK);
        de_render_settings capture_settings = settings;
        capture_settings.shadows = DE_QUALITY_LOW;
        capture_settings.render_scale = 1;
        for (u32 face = 0; face < 6; ++face)
            de_view_set_settings(renderer, captures[i].views[face], &capture_settings);
        de__probe_capture_at_position(&captures[i], (de_dvec3){2 * (f64)i, 0, 10}, (de_dvec3){0},
                                      1u << 5);
    }
    de_gpu_texture odd_target = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = 131,
                                       .height = 85,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    ASSERT_TRUE(odd_target.id);
    de_view spare[7];
    for (u32 i = 0; i < 7; ++i) {
        spare[i] = de_view_create(renderer,
                                  &(de_view_desc){.name = "csm ordinary reserved",
                                                  .target = i ? (de_gpu_texture){0} : odd_target});
        ASSERT_TRUE(spare[i].id);
    }
    EXPECT_FALSE(
        de_view_create(renderer, &(de_view_desc){.name = "must exceed unchanged public capacity"})
            .id);
    de_error_clear();
    u32 mat = material_add(.7f, .7f, 0, .5f), plane = lx_mesh_plane(f, 1, 1, 20);
    world_set(&f->w, 0, plane, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    DeLight light = {
        .kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 5, .cast_shadows = true};
    world_light(&f->w, 0, &light, at(0, 0, 0));
    world_light(&f->w, 1, &light, trs((de_vec3){0}, (de_vec3){0, 1, 0}, .3f, 1));
    de__gpu_scene_destroy(renderer->gpu_scene);
    renderer->gpu_scene = NULL;
    ASSERT_EQ(
        de__gpu_scene_create(&(de__gpu_scene_desc){.device = f->dev,
                                                   .allocator = &f->alloc,
                                                   .resolver = {.mesh = fake_mesh,
                                                                .material = fake_material,
                                                                .texture_bindless = fake_texture},
                                                   .staging_bytes = 16u << 20,
                                                   .views_in_view_graphs = true,
                                                   .initial_capacity[DE__GS_VIEWS] = DE__VIEW_MAX},
                             &renderer->gpu_scene),
        DE_OK);
    de__rw_view camera = {
        .entity = 999,
        .camera = {.near_z = .1f, .fov_y = 1.04719755f, .view = de_renderer_main_view(renderer)},
        .offset = {0, 0, 10},
        .rotation = {0, 0, 0, 1},
        .transform = at(0, 0, 10)};
    f->w.f.views = &camera;
    f->w.f.view_count = 1;
    f->w.f.main_view = 0;
    world_begin(&f->w);
    renderer->render_world->last = &f->w.f;
    renderer->render_world->current = &f->w.f;
    de__renderer_kick_prepare(renderer);
    ASSERT_EQ(de_renderer_render_frame(renderer), DE_OK);
    de_gpu_wait_idle(f->dev);
    de__csm_info main;
    ASSERT_TRUE(de__csm_get(renderer->csm, de_renderer_main_view(renderer), &main));
    EXPECT_EQ(main.lights, 2);
    EXPECT_EQ(main.cascades, 4);
    EXPECT_EQ(main.draws, 8);
    EXPECT_TRUE(main.ready);
    EXPECT_GT(main.capacity, 32u);
    for (u32 i = 0; i < 4; ++i) {
        de__csm_info capture;
        ASSERT_TRUE(de__csm_get(renderer->csm, captures[i].views[5], &capture));
        EXPECT_TRUE(capture.ready);
        EXPECT_EQ(capture.draws, 4);
        EXPECT_EQ(capture.lights, 2);
        EXPECT_FLOAT_EQ(capture.data[0].sphere[0], 2 * (f32)i, 0);
        EXPECT_NE(capture.depth.id, main.depth.id);
        if (i) {
            de__csm_info first;
            de__csm_get(renderer->csm, captures[0].views[5], &first);
            EXPECT_EQ(first.depth.id, capture.depth.id);
        }
    }
    de_gpu_texture retained_main = main.depth;
    world_begin(&f->w);
    de__renderer_kick_prepare(renderer);
    ASSERT_EQ(de_renderer_render_frame(renderer), DE_OK);
    de_gpu_wait_idle(f->dev);
    ASSERT_TRUE(de__csm_get(renderer->csm, de_renderer_main_view(renderer), &main));
    EXPECT_EQ(main.depth.id, retained_main.id);
    EXPECT_EQ(main.resolution, 4096);
    // All32 public slots remain occupied after real main+capture shading and24 active shadow
    // contexts.
    EXPECT_FALSE(de_view_create(renderer, &(de_view_desc){0}).id);
    de_error_clear();
    printf("capacity coexistence: 24 real capture-cache views + main +7 reserved ordinary, "
           "main8cascades and4active captures, internal pool capacity=%u pass\n",
           main.capacity);
    for (u32 i = 0; i < 4; ++i)
        de__probe_capture_at_position(&captures[i], (de_dvec3){0}, (de_dvec3){0}, 0);
    de_render_settings odd_settings = settings;
    odd_settings.shadows = DE_QUALITY_LOW;
    odd_settings.render_scale = .43f;
    de_view_set_settings(renderer, spare[0], &odd_settings);
    camera.camera.view = spare[0];
    world_begin(&f->w);
    de__renderer_kick_prepare(renderer);
    ASSERT_EQ(de_renderer_render_frame(renderer), DE_OK);
    de_gpu_wait_idle(f->dev);
    de__csm_info odd;
    ASSERT_TRUE(de__csm_get(renderer->csm, spare[0], &odd));
    de__csm_camera effective = {.position = {0, 0, 10},
                                .forward = {0, 0, -1},
                                .near_z = .1f,
                                .distance = 200,
                                .tan_half_fov = tanf(.5f * camera.camera.fov_y),
                                .aspect = 131.f / 85};
    de__csm_cascade expected[4];
    ASSERT_EQ(de__csm_fit(&effective, (de_vec3){0, 0, -1}, 2, 1024, expected), DE_OK);
    EXPECT_FLOAT_EQ(odd.data[1].sphere[3], expected[0].radius, 1e-6);
    printf("odd scaled RTT coverage:131x85 at.43 retains unscaled projection aspect\n");
    for (u32 i = 0; i < 4; ++i)
        de__probe_capture_destroy(&captures[i]);
    for (u32 i = 0; i < 7; ++i)
        de_view_destroy(renderer, spare[i]);
    renderer->render_world->current = renderer->render_world->last = NULL;
    de_renderer_destroy(renderer);
    de_gpu_texture_destroy(f->dev, odd_target);
    de_world_destroy(ecs);
    de_type_registry_destroy(types);
    f->w.f.views = NULL;
    f->w.f.view_count = 0;
    EXPECT_EQ(validation_errors(f->dev), 0);
    cvar_set("r.csm_lights", "1");
}
#define CAPACITY_TEST(tag, backend)                                                                \
    TEST_F(csm, capture_public_capacity_##tag, lx) {                                               \
        capacity_captures(f, backend);                                                             \
    }
CAPACITY_TEST(metal, DE_GPU_METAL)
CAPACITY_TEST(vulkan, DE_GPU_VULKAN)
CAPACITY_TEST(d3d12, DE_GPU_D3D12)
typedef struct scratch_pass {
    de_rg_resource depth, mirror, readback;
    u32 pitch;
    f32 value;
} scratch_pass;
static void scratch_clear(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    scratch_pass *p = user;
    de_gpu_attachment a = {.texture = de_rg_texture(pass, p->depth),
                           .layer = 0,
                           .clear = true,
                           .clear_depth = p->value,
                           .store = true};
    de_gpu_cmd_begin_render(cmd, NULL, 0, &a);
    de_gpu_cmd_end_render(cmd);
}
static void scratch_read(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    scratch_pass *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->readback), 0, p->pitch,
                                      de_rg_texture(pass, p->mirror), 0, 0);
}
static void scratch_growth_test(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de__shadow_views *pool = NULL;
    ASSERT_EQ(de__shadow_views_create(f->dev, &f->alloc, &pool), DE_OK);
    de_rg *graphs[2] = {f->g, NULL};
    de_arena *arenas[2] = {f->arena, NULL};
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc,
                                           .device = f->dev,
                                           .name = "shadow second pending graph"},
                            &graphs[1]),
              DE_OK);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = 1u << 20,
                                               .name = "shadow second pending arena"},
                              &arenas[1]),
              DE_OK);
    de_gpu_texture depths[3] = {0}, mirrors[3] = {0};
    de_gpu_buffer readbacks[3] = {0};
    scratch_pass passes[3];
    // Grow twice inside one graph, then again while a second graph is unsubmitted.
    for (u32 i = 0; i < 3; ++i) {
        u32 n = 64u << i, gi = i == 2 ? 1 : 0;
        depths[i] = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.dim = DE_TEX_2D_ARRAY,
                                           .format = DE_FMT_D32_FLOAT,
                                           .width = n,
                                           .height = n,
                                           .depth_or_layers = 2,
                                           .usage = DE_USAGE_DEPTH | DE_USAGE_COPY_SRC});
        mirrors[i] = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                           .width = n,
                                           .height = n,
                                           .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST |
                                                    DE_USAGE_COPY_SRC});
        readbacks[i] =
            de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)n * n * 4,
                                                               .memory = DE_MEM_READBACK,
                                                               .usage = DE_USAGE_COPY_DST});
        ASSERT_TRUE(depths[i].id && mirrors[i].id && readbacks[i].id);
        if (i != 1)
            ASSERT_EQ(de__rg_begin(graphs[gi], arenas[gi]), DE_OK);
        passes[i] = (scratch_pass){
            .depth = de_rg_import_texture(graphs[gi], depths[i], DE_ACCESS_NONE),
            .mirror = de_rg_import_texture(graphs[gi], mirrors[i], DE_ACCESS_NONE),
            .readback = de_rg_import_buffer(graphs[gi], readbacks[i], DE_ACCESS_NONE),
            .pitch = n * 4,
            .value = .25f * (f32)(i + 1)};
        de_rg_pass *p = de_rg_add_pass(graphs[gi], "shadow ownership clear", DE_QUEUE_GRAPHICS,
                                       scratch_clear, &passes[i]);
        de_rg_write(p, passes[i].depth, DE_ACCESS_DEPTH_WRITE);
        ASSERT_EQ(
            de__shadow_copy_layer(pool, graphs[gi], passes[i].depth, 0, passes[i].mirror, n, n),
            DE_OK);
        p = de_rg_add_pass(graphs[gi], "shadow ownership readback", DE_QUEUE_GRAPHICS, scratch_read,
                           &passes[i]);
        de_rg_pass_side_effect(p);
        de_rg_read(p, passes[i].mirror, DE_ACCESS_COPY_SRC);
        de_rg_write(p, passes[i].readback, DE_ACCESS_COPY_DST);
        if (i) {
            ASSERT_EQ(de__rg_finalize(graphs[gi]), DE_OK);
            ASSERT_EQ(de__rg_compile(graphs[gi]), DE_OK);
        }
    }
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_EQ(de__rg_execute(graphs[i], &(de__rg_execute_desc){.frame_index = 0,
                                                                   .frame_fence = f->fence,
                                                                   .frame_value = i + 1}),
                  DE_OK);
        de__shadow_views_end(pool, graphs[i], true);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, i + 1, FIRST_FRAME_WAIT_NS), DE_OK);
    }
    for (u32 i = 0; i < 3; ++i) {
        const f32 *pixels = de_gpu_buffer_map(f->dev, readbacks[i]);
        ASSERT_TRUE(pixels);
        for (u32 k = 0; k < (64u << i) * (64u << i); ++k)
            EXPECT_FLOAT_EQ(pixels[k], passes[i].value, 0);
    }
    EXPECT_EQ(validation_errors(f->dev), 0);
    de__shadow_views_destroy(pool);
    de__rg_destroy(graphs[1]);
    de_arena_destroy(arenas[1]);
    for (u32 i = 0; i < 3; ++i) {
        de_gpu_texture_destroy(f->dev, depths[i]);
        de_gpu_texture_destroy(f->dev, mirrors[i]);
        de_gpu_buffer_destroy(f->dev, readbacks[i]);
    }
}
#define SCRATCH_TEST(tag, backend)                                                                 \
    TEST_F(csm, scratch_growth_pending_graphs_##tag, lx) {                                         \
        scratch_growth_test(f, backend);                                                           \
    }
SCRATCH_TEST(metal, DE_GPU_METAL)
SCRATCH_TEST(vulkan, DE_GPU_VULKAN)
SCRATCH_TEST(d3d12, DE_GPU_D3D12)
static bool fail_metadata_map;
static de_result metadata_map_failure(de_gpu_device *dev, de__gpu_buffer *buffer,
                                      const de_gpu_buffer_desc *desc) {
    de_result r = shadow_original_vt->buffer_create(dev, buffer, desc);
    if (r == DE_OK && fail_metadata_map && desc->name && strcmp(desc->name, "csm.metadata") == 0) {
        fail_metadata_map = false;
        buffer->mapped = NULL;
    }
    return r;
}
static de_result standalone_csm_build(lx *f, u64 frame, u64 completed, u32 budget) {
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
    if (budget) {
        de_arena_info info = de_arena_get_info(f->arena);
        ASSERT_TRUE(de_arena_push(f->arena, info.reserved - info.used - budget, 1));
    }
    de_mat4 view = de_m4_look_at(f->eye, f->target, (de_vec3){0, 1, 0});
    de_mat4 proj = de_m4_perspective_rz(f->fov, 1.7f, .1f);
    de__rw_view camera = {.offset = f->eye, .camera = {.near_z = .1f}};
    de_render_settings settings = {.shadows = DE_QUALITY_HIGH};
    de__view_frame vf = {.view = {.id = 1},
                         .graph = f->g,
                         .world = &f->w.f,
                         .camera = &camera,
                         .settings = &settings,
                         .frame_index = frame,
                         .width = 65,
                         .height = 43,
                         .view_matrix = view,
                         .proj = proj};
    de__vis_context vc = {.gpu_scene = f->gs, .frames_completed = completed, .layer_mask = 1};
    return de__csm_build(f->csm, &vf, &vc);
}
static void standalone_csm_end(lx *f, u64 frame, bool execute, bool reported_failure) {
    de_result result = DE_OK;
    if (execute) {
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        ASSERT_EQ(de__rg_compile(f->g), DE_OK);
        shadow_original_vt = f->dev->vt;
        de__gpu_backend_vtable vt = *shadow_original_vt;
        if (reported_failure) {
            vt.queue_submit = report_shadow_submit_failure;
            shadow_reported_failure = false;
            f->dev->vt = &vt;
        }
        result = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = frame,
                                                             .frame_fence = f->fence,
                                                             .frame_value = frame + 1});
        f->dev->vt = shadow_original_vt;
        EXPECT_EQ(result, reported_failure ? DE_ERR_INTERNAL : DE_OK);
    }
    de__csm_complete(f->csm, (de_view){.id = 1}, f->g, execute, execute && result == DE_OK);
    de__gpu_scene_frame_end(f->gs, f->g, execute);
    if (execute)
        ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, frame + 1, FIRST_FRAME_WAIT_NS), DE_OK);
    de_error_clear();
}
static void csm_retry_test(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    cvar_set("r.raster_path", "compute");
    de_arena_destroy(f->arena);
    f->arena = NULL;
    ASSERT_EQ(
        de_arena_create(&(de_arena_desc){.reserve_bytes = 1u << 20, .name = "bounded shadow retry"},
                        &f->arena),
        DE_OK);
    ASSERT_EQ(de__csm_create_ex(f->dev, &f->alloc, &f->csm), DE_OK);
    cvar_set("r.csm_far_update_interval", "16");
    u32 mat = material_add(.7f, .6f, 0, .5f), plane = lx_mesh_plane(f, 1, 1, 20);
    world_set(&f->w, 0, plane, at(0, 0, 0), mat);
    f->w.f.instances.flags[0] |= DE__RW_INST_CAST_SHADOWS;
    DeLight sun = {
        .kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 5, .cast_shadows = true};
    world_light(&f->w, 0, &sun, at(0, 0, 0));
    shadow_original_vt = f->dev->vt;
    de__gpu_backend_vtable vt = *shadow_original_vt;
    vt.buffer_create = metadata_map_failure;
    fail_metadata_map = true;
    f->dev->vt = &vt;
    de_result result = standalone_csm_build(f, 0, 0, 0);
    f->dev->vt = shadow_original_vt;
    EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_FALSE(fail_metadata_map);
    standalone_csm_end(f, 0, false, false);
    ASSERT_EQ(standalone_csm_build(f, 0, 0, 0), DE_OK);
    standalone_csm_end(f, 0, false, false);
    // Find a bounded late-cascade arena failure from the current backend's actual builder
    // footprint. No backend-dependent byte constant defines whether this regression runs.
    u32 late_budget = 0;
    for (u32 budget = 2048; budget <= 65536 && !late_budget; budget += 512) {
        de_result probe = standalone_csm_build(f, 0, 0, budget);
        de__csm_info measured;
        de__csm_get(f->csm, (de_view){.id = 1}, &measured);
        EXPECT_TRUE(probe == DE_OK || probe == DE_ERR_OUT_OF_MEMORY);
        if (probe == DE_ERR_OUT_OF_MEMORY && measured.draws >= 2 && measured.draws < 4)
            late_budget = budget;
        standalone_csm_end(f, 0, false, false);
    }
    ASSERT_GT(late_budget, 0u);
    ASSERT_EQ(standalone_csm_build(f, 0, 0, 0), DE_OK);
    de__csm_info reserved, unchanged;
    de__csm_get(f->csm, (de_view){.id = 1}, &reserved);
    de_render_settings different = {.shadows = DE_QUALITY_ULTRA};
    de__rw_view camera = {0};
    de__view_frame duplicate = {.view = {.id = 1},
                                .graph = f->g,
                                .world = &f->w.f,
                                .camera = &camera,
                                .settings = &different,
                                .frame_index = 0};
    de__vis_context vc = {.gpu_scene = f->gs};
    EXPECT_EQ(de__csm_build(f->csm, &duplicate, &vc), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de__csm_get(f->csm, (de_view){.id = 1}, &unchanged);
    EXPECT_EQ(unchanged.depth.id, reserved.depth.id);
    EXPECT_EQ(unchanged.resolution, reserved.resolution);
    EXPECT_EQ(memcmp(unchanged.data, reserved.data, sizeof reserved.data), 0);
    standalone_csm_end(f, 0, true, false);

    ASSERT_EQ(standalone_csm_build(f, 1, 1, 0), DE_OK);
    de__csm_info before;
    de__csm_get(f->csm, (de_view){.id = 1}, &before);
    EXPECT_EQ(before.draws, 2);
    standalone_csm_end(f, 1, true, false);
    f->eye.x += 10;
    f->target.x += 10;
    result = standalone_csm_build(f, 2, 2, late_budget);
    de__csm_info partial;
    de__csm_get(f->csm, (de_view){.id = 1}, &partial);
    printf("late-cascade arena failure: result=%d completed cascades=%u\n", result, partial.draws);
    EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(partial.draws >= 2 && partial.draws < 4);
    standalone_csm_end(f, 2, false, false);
    de__csm_get(f->csm, (de_view){.id = 1}, &partial);
    EXPECT_FALSE(partial.ready);
    ASSERT_EQ(standalone_csm_build(f, 2, 2, 0), DE_OK);
    de__csm_get(f->csm, (de_view){.id = 1}, &partial);
    EXPECT_EQ(partial.draws, 4);
    standalone_csm_end(f, 2, true, false);
    // Standalone callers must supply a completed-frame fence; metadata cannot wrap over live work.
    EXPECT_EQ(standalone_csm_build(f, 6, 2, 0), DE_ERR_INVALID_ARGUMENT);
    standalone_csm_end(f, 6, false, false);
    ASSERT_EQ(standalone_csm_build(f, 6, 3, 0), DE_OK);
    standalone_csm_end(f, 6, true, true);
    EXPECT_EQ(standalone_csm_build(f, 6, 6, 0), DE_ERR_INVALID_ARGUMENT);
    standalone_csm_end(f, 6, false, false);
    ASSERT_EQ(standalone_csm_build(f, 10, 7, 0), DE_OK);
    de__csm_get(f->csm, (de_view){.id = 1}, &partial);
    EXPECT_EQ(partial.draws, 4);
    standalone_csm_end(f, 10, true, false);
    EXPECT_EQ(validation_errors(f->dev), 0);
    cvar_set("r.csm_far_update_interval", "1");
}
#define RETRY_TEST(tag, backend)                                                                   \
    TEST_F(csm, cadence_metadata_failure_retry_##tag, lx) {                                        \
        csm_retry_test(f, backend);                                                                \
    }
RETRY_TEST(metal, DE_GPU_METAL)
RETRY_TEST(vulkan, DE_GPU_VULKAN)
RETRY_TEST(d3d12, DE_GPU_D3D12)
static void runtime_shadows(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    f->width = 129;
    f->height = 83;
    f->eye = (de_vec3){6, -8, 6};
    f->target = (de_vec3){0, 0, 0};
    u32 mat = material_add(.7f, .7f, 0, .5f);
    u32 floor = lx_mesh_plane(f, 1, 1, 30), caster = lx_mesh_plane(f, 1, 1, 4);
    world_set(&f->w, 0, floor, at(0, 0, 0), mat);
    world_set(&f->w, 1, caster, at(0, 0, 2), mat);
    f->w.f.instances.flags[1] |= DE__RW_INST_CAST_SHADOWS;
    DeLight sun = {.kind = DE_LIGHT_DIRECTIONAL,
                   .color = {1, 1, 1},
                   .intensity = 10,
                   .cast_shadows = true,
                   .source_radius = .00465f};
    world_light(&f->w, 0, &sun, at(0, 0, 0));
    ASSERT_EQ(de__csm_create_ex(f->dev, &f->alloc, &f->csm), DE_OK);
    f->shadow_quality = DE_QUALITY_OFF;
    ASSERT_TRUE(lx_frame(f));
    u32 pixels = f->width * f->height;
    f32 *baseline = zalloc((size_t)pixels * 4 * sizeof(f32));
    memcpy(baseline, f->res.lighting, (size_t)pixels * 4 * sizeof(f32));
    de__csm_info info;
    ASSERT_TRUE(de__csm_get(f->csm, (de_view){.id = 1}, &info));
    EXPECT_EQ(info.draws, 0);
    EXPECT_FALSE(DE_HANDLE_VALID(info.depth));
    f->shadow_quality = DE_QUALITY_LOW;
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(de__csm_get(f->csm, (de_view){.id = 1}, &info));
    EXPECT_EQ(info.cascades, 2);
    EXPECT_EQ(info.resolution, 1024);
    EXPECT_EQ(info.lights, 1);
    EXPECT_EQ(info.draws, 2);
    EXPECT_TRUE(info.ready);
    u32 dark = 0, lit = 0;
    for (u32 i = 0; i < pixels; ++i) {
        const f32 *p = f->res.pos + i * 4;
        if (p[3] < .5f || fabsf(p[2]) > .001f)
            continue;
        if (fabsf(p[0]) < 1.65f && fabsf(p[1]) < 1.65f && baseline[i * 4] > .05f) {
            EXPECT_TRUE(f->res.lighting[i * 4] < baseline[i * 4] * .01f);
            ++dark;
        }
        if (fabsf(p[0]) > 2.5f && fabsf(p[1]) > 2.5f && baseline[i * 4] > .05f) {
            EXPECT_FLOAT_EQ(f->res.lighting[i * 4], baseline[i * 4], .002f);
            ++lit;
        }
    }
    printf("independent shadow receiver checks: dark=%u lit=%u\n", dark, lit);
    EXPECT_TRUE(dark > 10);
    EXPECT_TRUE(lit > 100);
    large_world_images(f);
    cvar_set("r.csm_lights", "2");
    world_light(&f->w, 1, &sun, trs((de_vec3){0}, (de_vec3){0, 1, 0}, .35f, 1));
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(de__csm_get(f->csm, (de_view){.id = 1}, &info));
    EXPECT_EQ(info.lights, 2);
    EXPECT_EQ(info.draws, 4);
    f->shadow_quality = DE_QUALITY_MEDIUM;
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(de__csm_get(f->csm, (de_view){.id = 1}, &info));
    EXPECT_EQ(info.cascades, 3);
    EXPECT_EQ(info.draws, 6);
    printf("CSM Medium/two-light actual chunks=%u warm handles=%u\n", f->g->exec->chunk_count,
           f->g->exec->primed[DE_QUEUE_GRAPHICS]);
    for (u32 quality = DE_QUALITY_HIGH; quality <= DE_QUALITY_ULTRA; ++quality) {
        f->shadow_quality = (de_quality)quality;
        ASSERT_TRUE(lx_frame(f));
        ASSERT_TRUE(de__csm_get(f->csm, (de_view){.id = 1}, &info));
        EXPECT_EQ(info.cascades, 4);
        EXPECT_EQ(info.lights, 2);
        EXPECT_EQ(info.draws, 8);
        EXPECT_EQ(info.resolution, quality == DE_QUALITY_ULTRA ? 4096 : 2048);
        printf("quality=%u two-light actual chunks=%u\n", quality, f->g->exec->chunk_count);
    }
    f->shadow_quality = DE_QUALITY_OFF;
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(de__csm_get(f->csm, (de_view){.id = 1}, &info));
    EXPECT_EQ(info.draws, 0);
    EXPECT_EQ(validation_errors(f->dev), 0);
    free(baseline);
    cvar_set("r.csm_lights", "1");
}
#define RUNTIME_TEST(tag, backend)                                                                 \
    TEST_F(csm, real_receiver_and_settings_##tag, lx) {                                            \
        runtime_shadows(f, backend);                                                               \
    }
RUNTIME_TEST(metal, DE_GPU_METAL)
RUNTIME_TEST(vulkan, DE_GPU_VULKAN)
RUNTIME_TEST(d3d12, DE_GPU_D3D12)
