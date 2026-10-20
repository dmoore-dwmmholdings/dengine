#include "scene_as_fixture.h"
#include "render/pathtracer/pt_internal.h"
#include "render/rt/restir_di.h"
#include "render/probes/probes_internal.h"
#include "flip.h"
#include "render/shaders/engine_shaders.h"

typedef struct pt_fixture {
    rt_fixture rt;
    de__rw_view camera;
    de_gpu_buffer rb, sphere_indices;
    de_cooked_mesh_range sphere_range;
    de_render_submesh sphere_submesh;
    de__pt_info info;
} pt_fixture;
static void pt_fixture_setup(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    rt_fixture_setup(r);
    if (!de_gpu_get_caps(r->d)->rt_pipelines)
        SKIP("native RT pipelines unavailable");
    de__view_unregister_stage(r->r, trace_stage, r);
    de__view_capture_camera(r->r, r->view, NULL);
    f->camera = (de__rw_view){.entity = 999,
                              .camera = {.projection = DE_PROJ_PERSPECTIVE,
                                         .fov_y = .9f,
                                         .near_z = .1f,
                                         .view = r->view,
                                         .exposure_ev100 = 0},
                              .offset = {3, 0, 4},
                              .rotation = {0, 0, 0, 1}};
    f->camera.transform = at(3, 0, 4);
    r->w.f.views = &f->camera;
    r->w.f.view_count = 1;
    g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[0].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[0].standard.base_color_tex = (de_texture){0};
    g_mat[0].textures[0] = (de_texture){0};
    g_mat[0].standard.base_color = (de_vec4){2, 1, .5f, 1};
    g_mat[0].standard.unlit = true;
    g_mat[0].standard.emissive = (de_vec3){2, 1, .5f};
    g_mat[0].standard.emissive_strength = 1;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.path_tracer = true;
    settings.path_max_bounces = 8;
    settings.render_scale = 1;
    de_renderer_set_settings(r->r, &settings);
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 64 * 64 * 16,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id);
    (void)cvar_set("r.gpu_profile", "0");
}
static void pt_fixture_teardown(pt_fixture *f) {
    if (f->rt.d) {
        de_gpu_wait_idle(f->rt.d);
        de_gpu_buffer_destroy(f->rt.d, f->rb);
        if (f->sphere_indices.id) {
            mesh_free(&g_mesh[1], f->rt.d);
            de_gpu_buffer_destroy(f->rt.d, f->sphere_indices);
        }
    }
    (void)cvar_set("r.ambient_fallback", "0");
    (void)cvar_set("r.pt.seed", "0");
    (void)cvar_set("r.pt.sampling_strategy", "0");
    rt_fixture_teardown(&f->rt);
}
static void pt_frame(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    world_begin(&r->w);
    r->handoff.last = &r->w.f;
    r->handoff.current = &r->w.f;
    r->r->render_world = &r->handoff;
    de__renderer_kick_prepare(r->r);
    ASSERT_EQ(de_renderer_render_frame(r->r), DE_OK);
    de_gpu_wait_idle(r->d);
    r->handoff.current = NULL;
    ASSERT_TRUE(de__pt_get(r->r, r->view, &f->info));
    ASSERT_FALSE(de_gpu_device_is_lost(r->d));
}
static float *pt_read_texture(pt_fixture *f, de_gpu_texture texture, u32 bytes_per_pixel) {
    rt_fixture *r = &f->rt;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "PT accumulation readback");
    de_gpu_barrier b = {
        .texture = texture, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, 0, f->info.width * bytes_per_pixel, texture, 0,
                                      0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->rb);
}
static float *pt_read(pt_fixture *f) {
    return pt_read_texture(f, f->info.accumulation, 16);
}

TEST_F(pt_reference, progressive_sample_moments_match_observed_paths, pt_fixture) {
    g_mat[0].standard.unlit = false;
    g_mat[0].standard.emissive = (de_vec3){0};
    g_mat[0].standard.base_color = (de_vec4){.7f, .5f, .25f, 1};
    f->rt.w.touch_all = true;
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "1"));
    ASSERT_TRUE(cvar_set("r.pt.final_denoise", "0"));
    const u32 center = 32u * 64u + 32u;
    f32 previous[3] = {0};
    f64 total[3] = {0}, squared[3] = {0};
    for (u32 sample = 0; sample < 16; ++sample) {
        pt_frame(f);
        ASSERT_EQ(f->info.samples, sample + 1u);
        const f32 *beauty = pt_read(f) + center * 4u;
        f32 sum[3] = {beauty[0], beauty[1], beauty[2]};
        ASSERT_FLOAT_EQ(beauty[3], sample + 1u, 0);
        const f32 *moments = pt_read_texture(f, f->info.moments, 16) + center * 4u;
        for (u32 ch = 0; ch < 3; ++ch) {
            f64 value = (f64)sum[ch] - previous[ch];
            previous[ch] = sum[ch];
            total[ch] += value;
            squared[ch] += value * value;
            const f64 n = sample + 1u;
            const f64 expected_m2 = fmax(squared[ch] - total[ch] * total[ch] / n, 0);
            EXPECT_FLOAT_EQ(moments[ch], expected_m2, 2e-5 * fmax(1, expected_m2));
        }
    }
    for (u32 ch = 0; ch < 3; ++ch)
        EXPECT_TRUE(squared[ch] - total[ch] * total[ch] / 16 > 1e-5);
    de_renderer_path_reset(f->rt.r, f->rt.view);
    pt_frame(f);
    ASSERT_EQ(f->info.samples, 1u);
    const f32 *moments = pt_read_texture(f, f->info.moments, 16) + center * 4u;
    for (u32 ch = 0; ch < 3; ++ch)
        EXPECT_FLOAT_EQ(moments[ch], 0, 0);
}

TEST_F(pt_reference, final_prepare_preserves_sample_mean_and_measured_variance, pt_fixture) {
    de_gpu_device *d = f->rt.d;
    const u32 side = 64, pixels = side * side;
    f->info.width = side;
    de_gpu_texture textures[6] = {0};
    for (u32 i = 0; i < 6; ++i) {
        textures[i] = de_gpu_texture_create(
            d, &(de_gpu_texture_desc){.width = side,
                                      .height = side,
                                      .mip_count = 1,
                                      .format = DE_FMT_RGBA32_FLOAT,
                                      .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE |
                                               DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC});
        ASSERT_TRUE(textures[i].id);
    }
    de_gpu_buffer upload = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 4 * pixels * 16,
                                                                         .usage = DE_USAGE_COPY_SRC,
                                                                         .memory = DE_MEM_UPLOAD});
    de_gpu_buffer params = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 64, .stride = 64, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    ASSERT_TRUE(upload.id && params.id);
    f32 *data = de_gpu_buffer_map(d, upload);
    for (u32 i = 0; i < pixels; ++i) {
        f32 mean = i == 32 * side + 32 ? 10.f : 1.f;
        const f32 values[4][4] = {
            {8 * mean, 2 * mean, .5f * mean, 16},
            {.5f, .25f, .125f, 1},
            {0, 0, 1, .5f},
            {i % 2 ? 0.f : 60.f, i % 2 ? 0.f : 60.f, i % 2 ? 0.f : 33.75f, 0}};
        for (u32 t = 0; t < 4; ++t)
            memcpy(data + t * pixels * 4 + i * 4, values[t], 16);
    }
    const char *entries[] = {"cs_pt_final_prepare"};
    de__engine_shader_code code = {0};
    char why[256];
    ASSERT_EQ(de__engine_shader_load(d, f->rt.r->alloc, "pt_final_denoise", 0, entries, 1, &code,
                                     why, sizeof why),
              DE_OK);
    de_gpu_pipeline pipeline = de_gpu_pipeline_create(
        d, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                   .cs = code.entries[0].blob,
                                   .push_constant_bytes = 8,
                                   .threadgroup_size = {8, 8, 1},
                                   .name = "PT sample mean preparation oracle"});
    de__engine_shader_code_free(&code);
    ASSERT_TRUE(pipeline.id);
    /* Private 64-byte PtFinalParams ABI, indexed independently from production C setup. */
    u32 pc[16] = {side, side};
    pc[2] = de_gpu_texture_bindless(d, textures[0], 0, false);
    pc[3] = pc[6] = pc[7] = de_gpu_texture_bindless(d, textures[1], 0, false);
    pc[4] = de_gpu_texture_bindless(d, textures[2], 0, false);
    pc[9] = de_gpu_texture_bindless(d, textures[4], 0, true);
    pc[10] = de_gpu_texture_bindless(d, textures[5], 0, true);
    pc[11] = de_gpu_texture_bindless(d, textures[3], 0, false);
    memcpy(de_gpu_buffer_map(d, params), pc, sizeof pc);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "PT sample mean preparation oracle");
    for (u32 i = 0; i < 6; ++i) {
        de_gpu_barrier b = {.texture = textures[i],
                            .before = DE_ACCESS_NONE,
                            .after = i < 4 ? DE_ACCESS_COPY_DST : DE_ACCESS_SHADER_WRITE};
        de_gpu_cmd_barriers(cmd, &b, 1);
        if (i < 4) {
            de_gpu_cmd_copy_buffer_to_texture(cmd, textures[i], 0, 0, upload, (u64)i * pixels * 16,
                                              side * 16);
            b.before = DE_ACCESS_COPY_DST;
            b.after = DE_ACCESS_SHADER_READ;
            de_gpu_cmd_barriers(cmd, &b, 1);
        }
    }
    u32 push[2] = {de_gpu_buffer_bindless(d, params), 0};
    de_gpu_cmd_set_pipeline(cmd, pipeline);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    de_gpu_cmd_dispatch(cmd, side / 8, side / 8, 1);
    for (u32 i = 4; i < 6; ++i) {
        de_gpu_barrier b = {.texture = textures[i],
                            .before = DE_ACCESS_SHADER_WRITE,
                            .after = DE_ACCESS_SHADER_READ};
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(d);
    const f32 *color = pt_read_texture(f, textures[4], 16);
    for (u32 i = 0; i < pixels; ++i) {
        f32 mean = i == 32 * side + 32 ? 10.f : 1.f;
        EXPECT_FLOAT_EQ(color[i * 4], mean, 1e-6);
        EXPECT_FLOAT_EQ(color[i * 4 + 1], mean / 2, 1e-6);
        EXPECT_FLOAT_EQ(color[i * 4 + 2], mean / 4, 1e-6);
    }
    const f32 *variance = pt_read_texture(f, textures[5], 16);
    for (u32 i = 0; i < pixels; ++i)
        for (u32 ch = 0; ch < 3; ++ch)
            EXPECT_FLOAT_EQ(variance[i * 4 + ch], i % 2 ? 1e-8 : (f32)((ch + 1) * (ch + 1)), 1e-7);
    de_gpu_pipeline_destroy(d, pipeline);
    de_gpu_buffer_destroy(d, params);
    de_gpu_buffer_destroy(d, upload);
    for (u32 i = 0; i < 6; ++i)
        de_gpu_texture_destroy(d, textures[i]);
}

static void cornell(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    g_mat_count = 0;
    for (u32 face = 0; face < 6; ++face) {
        u32 material = material_add(.7f, .65f, 0, 0);
        g_mat[material - 1].standard.double_sided = true;
        if (face == 0)
            g_mat[material - 1].standard.base_color = (de_vec4){.75f, .08f, .08f, 1};
        if (face == 1)
            g_mat[material - 1].standard.base_color = (de_vec4){.08f, .75f, .08f, 1};
        de__rw_view c;
        de__probe_face_camera((de_dvec3){0}, (de_dvec3){0}, face, &c);
        de_affine x = c.transform;
        for (u32 row = 0; row < 3; ++row)
            x.m[row] = -x.m[row];
        for (u32 axis = 0; axis < 3; ++axis) {
            x.m[9 + axis] = -x.m[6 + axis] * 2;
            for (u32 row = 0; row < 3; ++row)
                x.m[axis * 3 + row] *= 2;
        }
        x.m[9] += 3;
        x.m[11] += 2;
        world_set(&r->w, face, 1, x, material);
        r->w.f.instances.flags[face] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[face] = 1u << 24;
    }
    u32 emitter = material_add(0, .5f, 0, 0);
    g_mat[emitter - 1].standard.emissive = (de_vec3){15, 15, 15};
    de_affine light = at(3, 0, 3.98f);
    light.m[0] = .35f;
    light.m[4] = -.35f;
    light.m[8] = -.35f;
    world_set(&r->w, 6, 1, light, emitter);
    r->w.f.instances.flags[6] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[6] = 1u << 24;
    r->w.f.instances.count = 7;
    r->w.touch_all = true;
    de_mat4 view = de_m4_look_at((de_vec3){3, -1.5f, 2}, (de_vec3){3, 1, 2}, (de_vec3){0, 0, 1}),
            inverse = de_m4_inverse_affine(&view);
    f->camera.transform = de_affine_from_m4(&inverse);
    f->camera.offset = (de_vec3){3, -1.5f, 2};
    de_mat3 rotation;
    memcpy(rotation.m, f->camera.transform.m, sizeof rotation.m);
    f->camera.rotation = de_quat_from_m3(&rotation);
    de_view_destroy(r->r, r->view);
    de_gpu_texture_destroy(r->d, r->target);
    r->target = de_gpu_texture_create(
        r->d, &(de_gpu_texture_desc){.width = 16,
                                     .height = 16,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    r->view = de_view_create(
        r->r, &(de_view_desc){.target = r->target, .viewport = {0, 0, 1, 1}, .name = "PT Cornell"});
    f->camera.camera.view = r->view;
    de_view_set_layer_mask(r->r, r->view, 1u << 24);
}
static u8 pt_display(f32 value) {
    value = fmaxf(value, 0);
    value = value / (1 + value);
    return (u8)lrintf(
        fminf(1, value <= .0031308f ? 12.92f * value : 1.055f * powf(value, 1 / 2.4f) - .055f) *
        255);
}

#include "render/graph/rg_internal.h"
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#include "cook/importers/texture/texture_internal.h"
#include <tinyexr.h>
#endif
TEST_F(pt_reference, exr_roundtrip_and_aovs, pt_fixture) {
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
    rt_fixture *r = &f->rt;
    EXPECT_TRUE(de_renderer_export_exr(r->r, r->view, "ptexr:/empty.exr", true) != DE_OK);
    ASSERT_EQ(de_fs_mount("ptexr", DE_SCENE_AS_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    pt_frame(f);
    pt_frame(f);
    ASSERT_EQ(f->info.samples, 2u);
    f32 expected[64 * 64 * 4];
    memcpy(expected, pt_read(f), sizeof expected);
    for (u32 i = 0; i < 64 * 64; ++i) {
        for (u32 c = 0; c < 3; ++c)
            expected[i * 4 + c] /= expected[i * 4 + 3];
        expected[i * 4 + 3] = 1;
    }
    ASSERT_EQ(de_renderer_export_exr(r->r, r->view, "ptexr:/roundtrip.exr", true), DE_OK);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("ptexr:/roundtrip.exr", de_heap_allocator(), &data, &size), DE_OK);
    de_tex_image decoded;
    ASSERT_EQ(de_tex_decode_exr(de_heap_allocator(), data, size, &decoded), DE_OK);
    EXPECT_EQ(decoded.width, 64u);
    EXPECT_EQ(decoded.height, 64u);
    EXPECT_MEMEQ(decoded.rgba, expected, sizeof expected);
    EXRVersion version;
    ASSERT_EQ(ParseEXRVersionFromMemory(&version, data, (size_t)size), TINYEXR_SUCCESS);
    EXPECT_FALSE(version.multipart);
    EXRHeader header;
    InitEXRHeader(&header);
    const char *error = NULL;
    ASSERT_EQ(ParseEXRHeaderFromMemory(&header, &version, data, (size_t)size, &error),
              TINYEXR_SUCCESS);
    EXRImage image;
    InitEXRImage(&image);
    ASSERT_EQ(LoadEXRImageFromMemory(&image, &header, data, (size_t)size, &error), TINYEXR_SUCCESS);
    EXPECT_EQ(header.num_channels, 12);
    de_gpu_buffer rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 64 * 64 * 16 * 3,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK});
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "First-hit AOV exact oracle");
    de_gpu_texture aov[] = {f->info.albedo, f->info.normal, f->info.depth};
    for (u32 i = 0; i < 3; ++i) {
        de_gpu_barrier b = {
            .texture = aov[i], .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, rb, i * 64 * 64 * 16, 64 * 16, aov[i], 0, 0);
        b.before = DE_ACCESS_COPY_SRC;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    const u8 *gpu = de_gpu_buffer_map(r->d, rb);
    u32 named = 0;
    for (int c = 0; c < header.num_channels; ++c) {
        const char *name = header.channels[c].name;
        int texture = -1, component = 0;
        if (!strncmp(name, "albedo.", 7)) {
            texture = 0;
            component = name[7] == 'R' ? 0 : name[7] == 'G' ? 1 : 2;
        }
        if (!strncmp(name, "normal.", 7)) {
            texture = 1;
            component = name[7] == 'X' ? 0 : name[7] == 'Y' ? 1 : 2;
        }
        if (!strcmp(name, "depth.Z"))
            texture = 2;
        if (texture >= 0) {
            ++named;
            const f32 *plane = (const f32 *)image.images[c];
            for (u32 y = 0; y < 64; ++y)
                for (u32 x = 0; x < 64; ++x) {
                    const f32 *row = (const f32 *)(gpu + (u64)texture * 64 * 64 * 16 + y * 64 * 16);
                    f32 value = row[x * (texture == 2 ? 1u : 4u) + (u32)component];
                    EXPECT_MEMEQ(&plane[y * 64 + x], &value, sizeof value);
                }
        }
        if (!strcmp(name, "samples.Y")) {
            ++named;
            EXPECT_FLOAT_EQ(((f32 *)image.images[c])[32 * 64 + 32], 2, 0);
        }
    }
    EXPECT_EQ(named, 8u);
    const f32 *albedo = (const f32 *)gpu, *normal = (const f32 *)(gpu + 64 * 64 * 16),
              *depth = (const f32 *)(gpu + 64 * 64 * 32);
    u32 center = 32 * 64 + 32;
    printf(
        "Actual first-hit AOV center: albedo %g %g %g normal %g %g %g depth %g; EXR %llu bytes\n",
        albedo[center * 4], albedo[center * 4 + 1], albedo[center * 4 + 2], normal[center * 4],
        normal[center * 4 + 1], normal[center * 4 + 2], depth[32 * 256 + 32],
        (unsigned long long)size);
    EXPECT_TRUE(depth[32 * 256 + 32] > 3.9f && depth[32 * 256 + 32] < 4.1f);
    EXPECT_TRUE(normal[center * 4 + 2] > .99f);
    EXPECT_TRUE(de_renderer_export_exr(r->r, r->view, "missingwrite:/denied.exr", false) != DE_OK);
    de_gpu_buffer_destroy(r->d, rb);
    FreeEXRImage(&image);
    FreeEXRHeader(&header);
    de_tex_image_free(de_heap_allocator(), &decoded);
    DE_FREE(de_heap_allocator(), data, size);
    de_view stale = r->view;
    de_view_destroy(r->r, r->view);
    r->view = (de_view){0};
    EXPECT_TRUE(de_renderer_export_exr(r->r, stale, "ptexr:/stale.exr", true) != DE_OK);
#else
    (void)f;
    SKIP("Explicit EXR API is Dev/Debug only");
#endif
}
typedef struct presentation {
    pt_fixture *f;
    de_gpu_buffer rb;
    bool filtered;
} presentation;
typedef struct presentation_copy {
    presentation *p;
    de_rg_resource color;
} presentation_copy;
static void copy_presentation(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    presentation_copy *x = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, x->p->rb, 0, 256, de_rg_texture(pass, x->color), 0, 0);
}
static void presentation_stage(de__view_frame *frame, void *user) {
    presentation *p = user;
    if (frame->view.id != p->f->rt.view.id)
        return;
    p->filtered = false;
    for (u32 i = 0; i < frame->graph->pass_count; ++i)
        if (!strcmp(frame->graph->passes[i]->name, "PT.FinalDenoise.Publish"))
            p->filtered = true;
    presentation_copy *x = de_arena_push_zero(frame->graph->arena, sizeof *x, 16);
    ASSERT_TRUE(x);
    *x = (presentation_copy){p, frame->color};
    de_rg_pass *pass = de_rg_add_pass(frame->graph, "Actual PT presentation readback",
                                      DE_QUEUE_GRAPHICS, copy_presentation, x);
    de_rg_read(pass, x->color, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, de_rg_import_buffer(frame->graph, p->rb, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(pass);
}
/* Fixed before GPU measurement: original Cornell geometry/resolution,16spp filtered versus
 * independent4096spp seed0,FLIP<=.05. No sample/fixture/threshold tuning follows measurement. */
TEST_F(pt_reference, final_denoise_quality, pt_fixture) {
    cornell(f);
    rt_fixture *r = &f->rt;
    presentation p = {
        .f = f,
        .rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 256 * 16,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK})};
    ASSERT_EQ(de__view_register_stage(r->r, DE__VIEW_SLOT_POST, INT32_MIN, "PT presentation oracle",
                                      presentation_stage, &p),
              DE_OK);
    ASSERT_TRUE(cvar_set("r.pt.final_denoise", "0"));
    for (u32 sample = 0; sample < 4096; ++sample)
        pt_frame(f);
    ASSERT_EQ(f->info.samples, 4096u);
    f32 *sum = pt_read(f);
    ASSERT_FLOAT_EQ(sum[(8 * 16 + 8) * 4 + 3], 4096, 0);
    u8 expected[16 * 16 * 3], actual[16 * 16 * 3];
    double energy = 0;
    for (u32 i = 0; i < 16 * 16; ++i)
        for (u32 c = 0; c < 3; ++c) {
            f32 value = sum[i * 4 + c] / 4096;
            expected[i * 3 + c] = pt_display(value);
            energy += value;
        }
    ASSERT_TRUE(energy > 1);
    de_renderer_path_reset(r->r, r->view);
    ASSERT_TRUE(cvar_set("r.pt.final_denoise", "1"));
    for (u32 sample = 0; sample < 16; ++sample)
        pt_frame(f);
    ASSERT_EQ(f->info.samples, 16u);
    ASSERT_TRUE(p.filtered);
    const u16 *display = de_gpu_buffer_map(r->d, p.rb);
    /* Production presentation includes fixed camera EV0 exposure1/1.2, normalized away. */
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x)
            for (u32 c = 0; c < 3; ++c)
                actual[(y * 16 + x) * 3 + c] =
                    pt_display(half_to_f32(display[y * 128 + x * 4 + c]) * 1.2f);
    sum = pt_read(f);
    ASSERT_FLOAT_EQ(sum[(8 * 16 + 8) * 4 + 3], 16, 0);
    f32 raw[16 * 16 * 4];
    memcpy(raw, sum, sizeof raw);
    f32 errors[16 * 16];
    de_flip_image a = {expected, 16, 16, 3, 0}, b = {actual, 16, 16, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&a, &b, 0, errors));
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(errors, 256, &stats));
    printf("Actual final-denoised16spp vs independent4096: FLIP %.9g,reference energy %.9g\n",
           stats.mean, energy);
    EXPECT_TRUE(stats.mean <= .05);
    ASSERT_TRUE(cvar_set("r.pt.max_samples", "16"));
    ASSERT_TRUE(cvar_set("r.pt.golden", "1"));
    pt_frame(f);
    EXPECT_FALSE(p.filtered);
    EXPECT_EQ(f->info.samples, 16u);
    EXPECT_MEMEQ(pt_read(f), raw, sizeof raw);
    (void)cvar_set("r.pt.golden", "0");
    (void)cvar_set("r.pt.final_denoise", "0");
    (void)cvar_set("r.pt.max_samples", "0");
    de__view_unregister_stage(r->r, presentation_stage, &p);
    de_gpu_buffer_destroy(r->d, p.rb);
}
TEST_F(pt_reference, denoise_warm_lifetime, pt_fixture) {
    ASSERT_TRUE(cvar_set("r.pt.final_denoise", "1"));
    for (u32 i = 0; i < 24; ++i)
        pt_frame(f);
    de_tracking_stats cpu = de_tracking_allocator_stats(&f->rt.track),
                      gpu = de_tracking_allocator_stats(&f->rt.dev_track);
    for (u32 i = 0; i < 8; ++i)
        pt_frame(f);
    EXPECT_EQ(de_tracking_allocator_stats(&f->rt.track).alloc_count, cpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->rt.track).realloc_count, cpu.realloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->rt.dev_track).alloc_count, gpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->rt.dev_track).realloc_count, gpu.realloc_count);
    /* A separate16-spp publication check does not rerun/tune the numerical reference. */
    cornell(f);
    rt_fixture *r = &f->rt;
    presentation capture = {
        .f = f,
        .rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 256 * 16,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK})};
    ASSERT_EQ(de__view_register_stage(r->r, DE__VIEW_SLOT_POST, INT32_MIN, "PT toggle presentation",
                                      presentation_stage, &capture),
              DE_OK);
    ASSERT_TRUE(cvar_set("r.pt.max_samples", "16"));
    for (u32 sample = 0; sample < 16; ++sample)
        pt_frame(f);
    ASSERT_EQ(f->info.samples, 16u);
    ASSERT_TRUE(capture.filtered);
    u16 filtered[16 * 128];
    memcpy(filtered, de_gpu_buffer_map(r->d, capture.rb), sizeof filtered);
    f32 raw[16 * 16 * 4];
    memcpy(raw, pt_read(f), sizeof raw);
    ASSERT_TRUE(cvar_set("r.pt.final_denoise", "0"));
    pt_frame(f);
    ASSERT_FALSE(capture.filtered);
    EXPECT_MEMEQ(pt_read(f), raw, sizeof raw);
    const u16 *plain = de_gpu_buffer_map(r->d, capture.rb);
    double difference = 0;
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x)
            for (u32 c = 0; c < 3; ++c)
                difference += fabs(half_to_f32(filtered[y * 128 + x * 4 + c]) -
                                   half_to_f32(plain[y * 128 + x * 4 + c]));
    printf("Actual optional presentation delta %.9g; raw16sample sum unchanged\n", difference);
    EXPECT_TRUE(difference > .001);
    ASSERT_TRUE(cvar_set("r.pt.final_denoise", "1"));
    ASSERT_TRUE(cvar_set("r.pt.golden", "1"));
    pt_frame(f);
    EXPECT_FALSE(capture.filtered);
    EXPECT_MEMEQ(pt_read(f), raw, sizeof raw);
    de__view_unregister_stage(r->r, presentation_stage, &capture);
    de_gpu_buffer_destroy(r->d, capture.rb);
    de_gpu_texture old_aov = f->info.albedo;
    de_view_destroy(r->r, r->view);
    de_gpu_texture_destroy(r->d, r->target);
    r->target = de_gpu_texture_create(
        r->d, &(de_gpu_texture_desc){.width = 32,
                                     .height = 32,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    r->view = de_view_create(
        r->r,
        &(de_view_desc){.target = r->target, .viewport = {0, 0, 1, 1}, .name = "PT AOV resized"});
    f->camera.camera.view = r->view;
    de_view_set_layer_mask(r->r, r->view, 1u << 24);
    pt_frame(f);
    EXPECT_EQ(f->info.width, 32u);
    EXPECT_EQ(f->info.height, 32u);
    EXPECT_EQ(f->info.samples, 1u);
    EXPECT_TRUE(f->info.albedo.id != old_aov.id);
    (void)cvar_set("r.pt.golden", "0");
    (void)cvar_set("r.pt.max_samples", "0");
    (void)cvar_set("r.pt.final_denoise", "0");
}
