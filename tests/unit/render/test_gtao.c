#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include <dengine/de_reflect.h>
#include "gpu/gpu_backend.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/ao/gtao_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"
#include "render/materials/brdf_luts.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_material.h"
#include "render/materials/material_models.h"
#include "render/loaders/render_assets.h"
#include "render/passes/sss/sss.h"
#include "render/passes/forward/forward.h"
#include "render/probes/irradiance_internal.h"
#include "render_d3d12_test.h"
#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lighting_fixture.h"

/* Fixed before GPU measurements: raw geometry only, no albedo/multibounce or filtering. */
#define CORNER_RELATIVE_ERROR       0.05
#define FLAT_ABSOLUTE_ERROR         0.01
#define BENT_MEAN_ANGLE_DEGREES     10.0
#define TEMPORAL_VARIANCE_LIMIT     0.0001
#define TEMPORAL_P99_VARIANCE_LIMIT 0.0005
#define PI64                        3.14159265358979323846
TEST(gtao, quality_contract) {
    const u32 expected[][4] = {
        {129, 81, 2, 4}, {129, 81, 2, 8}, {257, 161, 3, 8}, {257, 161, 4, 12}};
    for (u32 q = 1; q <= 4; ++q) {
        u32 tw, th, slices, steps;
        de__gtao_quality((de_quality)q, 257, 161, &tw, &th, &slices, &steps);
        EXPECT_EQ(tw, expected[q - 1][0]);
        EXPECT_EQ(th, expected[q - 1][1]);
        EXPECT_EQ(slices, expected[q - 1][2]);
        EXPECT_EQ(steps, expected[q - 1][3]);
    }
}
/* Independent cosine-sampled geometric rays. No horizon/slice algebra or GPU depth samples.
 * Uniform unit-disk samples are cosine hemisphere directions; hit the perpendicular wall plane
 * at distance d/(-omega.x), with the same documented finite-radius linear falloff. */
static double corner_visibility(double distance, double radius, double falloff, double bent[3]) {
    const u32 rays = 65536;
    double sum = 0;
    bent[0] = bent[1] = bent[2] = 0;
    for (u32 i = 0; i < rays; ++i) {
        double r = sqrt(((double)i + .5) / rays),
               phi = 2 * PI64 * fmod((double)i * .6180339887498948482, 1);
        double x = r * cos(phi), y = r * sin(phi), z = sqrt(fmax(0, 1 - r * r)), visibility = 1;
        if (x < 0) {
            double t = distance / (-x);
            visibility = 1 - fmin(fmax((radius - t) / fmax(radius * (1 - falloff), 1e-12), 0), 1);
        }
        sum += visibility;
        bent[0] += visibility * x;
        bent[1] += visibility * y;
        bent[2] += visibility * z;
    }
    double length = sqrt(bent[0] * bent[0] + bent[1] * bent[1] + bent[2] * bent[2]);
    for (u32 j = 0; j < 3; ++j)
        bent[j] /= length;
    return sum / rays;
}
TEST(gtao, independent_corner_oracle) {
    double b[3];
    EXPECT_FLOAT_EQ(corner_visibility(0, 1, 1, b), .5, .0001);
    EXPECT_FLOAT_EQ(corner_visibility(1, 1, 1, b), 1, .0001);
    for (u32 i = 1; i < 8; ++i) {
        double d = (double)i / 8;
        double analytic = .5 + (asin(d) + d * sqrt(1 - d * d)) / PI64;
        EXPECT_FLOAT_EQ(corner_visibility(d, 1, 1, b), analytic, .0002);
    }
}
static void matrix_rows(de__gs_float4 out[4], const de_mat4 *m) {
    for (u32 row = 0; row < 4; ++row)
        for (u32 col = 0; col < 4; ++col)
            out[row].v[col] = m->m[col * 4 + row];
}
static void transform64(const de_mat4 *m, const double v[4], double out[4]) {
    for (u32 row = 0; row < 4; ++row) {
        out[row] = 0;
        for (u32 col = 0; col < 4; ++col)
            out[row] += (double)m->m[col * 4 + row] * v[col];
    }
}
static de_gpu_texture uploaded_texture_usage(lx *f, u32 w, u32 h, de_gpu_format format,
                                             const void *pixels, u32 pitch, bool storage) {
    de_gpu_texture texture = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = w,
                                       .height = h,
                                       .format = format,
                                       .usage = (storage ? DE_USAGE_STORAGE : DE_USAGE_SAMPLED) |
                                                DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(texture.id);
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = (u64)pitch * h, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_COPY_SRC});
    ASSERT_TRUE(upload.id);
    memcpy(de_gpu_buffer_map(f->dev, upload), pixels, (size_t)pitch * h);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gtao.oracle.upload");
    ASSERT_TRUE(cmd);
    de_gpu_barrier barrier = {
        .texture = texture, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, texture, 0, 0, upload, 0, pitch);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = DE_ACCESS_SHADER_READ | (storage ? DE_ACCESS_SHADER_WRITE : 0u);
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, upload);
    return texture;
}
static de_gpu_texture uploaded_texture(lx *f, u32 w, u32 h, de_gpu_format format,
                                       const void *pixels, u32 pitch) {
    return uploaded_texture_usage(f, w, h, format, pixels, pitch, false);
}
typedef struct ao_readback {
    de_rg_resource ao, bent, buffer;
    u32 pitch, height;
} ao_readback;
static void read_ao(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    ao_readback *r = user;
    de_gpu_buffer buffer = de_rg_buffer(pass, r->buffer);
    de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, 0, r->pitch, de_rg_texture(pass, r->ao), 0, 0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, ((u64)r->pitch * r->height + 511u) & ~(u64)511u,
                                      r->pitch, de_rg_texture(pass, r->bent), 0, 0);
}
static int compare_double(const void *a, const void *b) {
    double aa = *(const double *)a, bb = *(const double *)b;
    return (aa > bb) - (aa < bb);
}
static float *filtered_geometry(lx *f, de__gtao *s, de_gpu_buffer scene, de_gpu_texture depth,
                                de_gpu_texture normal, u32 w, u32 h, const de_mat4 *vp,
                                de_quality quality) {
    u32 pitch = (w * 2 + 255u) & ~255u;
    u64 bent_offset = ((u64)pitch * h + 511u) & ~(u64)511u;
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bent_offset + (u64)pitch * h,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(rb.id);
    u8 *samples = zalloc((size_t)w * h * 8);
    u8 *zero = zalloc((size_t)((w * 4 + 255u) & ~255u) * h);
    de_gpu_texture motion =
        uploaded_texture(f, w, h, DE_FMT_RG16_FLOAT, zero, (w * 4 + 255u) & ~255u);
    free(zero);
    double *sum = zalloc((size_t)w * h * sizeof *sum), *sum2 = zalloc((size_t)w * h * sizeof *sum2);
    float *pixels = zalloc((size_t)w * h * 4 * sizeof *pixels);
    for (u32 frame = 0; frame < 16; ++frame) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        de_render_settings settings = {.ao = quality};
        de__view_frame vf = {.view = {.id = 1},
                             .graph = f->g,
                             .settings = &settings,
                             .frame_index = frame,
                             .width = w,
                             .height = h,
                             .view_proj = *vp,
                             .prev_view_proj = *vp};
        de__vis_context vc = {
            .depth = de_rg_import_texture(f->g, depth, DE_ACCESS_SHADER_READ),
            .scene_views = de_rg_import_buffer(f->g, scene, DE_ACCESS_SHADER_READ),
            .scene_views_bindless = de_gpu_buffer_bindless(f->dev, scene),
            .scene_view_slot = 1,
            .gbuffer = {[DE__GB1] = de_rg_import_texture(f->g, normal, DE_ACCESS_SHADER_READ),
                        [DE__GB_MOTION] =
                            de_rg_import_texture(f->g, motion, DE_ACCESS_SHADER_READ)}};
        de__lighting_inputs inputs = {0};
        de__gtao_config config = {.radius = 1, .falloff = .8f, .thickness = .2f, .bent = true};
        ASSERT_EQ(de__gtao_build(s, &vf, &vc, &config, &inputs), DE_OK);
        de__gtao_info info;
        ASSERT_TRUE(de__gtao_get(s, vf.view, &info));
        EXPECT_EQ(info.history_valid, frame > 0);
        EXPECT_EQ(de__gtao_build(s, &vf, &vc, &config, &inputs), DE_ERR_BUSY);
        de_error_clear();
        ao_readback *r = de_arena_push_zero(f->arena, sizeof *r, 16);
        ASSERT_TRUE(r);
        de_rg_resource bent = inputs.ao_bent;
        ASSERT_TRUE(bent.id);
        *r = (ao_readback){.ao = inputs.ao,
                           .bent = bent,
                           .buffer = de_rg_import_buffer(f->g, rb, frame ? DE_ACCESS_COPY_DST : 0),
                           .pitch = pitch,
                           .height = h};
        de_rg_pass *pass =
            de_rg_add_pass(f->g, "gtao.test.readback", DE_QUEUE_GRAPHICS, read_ao, r);
        de_rg_pass_side_effect(pass);
        de_rg_read(pass, r->ao, DE_ACCESS_COPY_SRC);
        de_rg_read(pass, r->bent, DE_ACCESS_COPY_SRC);
        de_rg_write(pass, r->buffer, DE_ACCESS_COPY_DST);
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        ASSERT_EQ(de__rg_compile(f->g), DE_OK);
        /* Logical noise frames restart per camera case; timeline fence values never do. */
        const u64 fence_value = ++f->frames;
        de_result result = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = frame,
                                                                       .frame_fence = f->fence,
                                                                       .frame_value = fence_value});
        de__gtao_complete(s, vf.view, f->g, true, result == DE_OK);
        ASSERT_EQ(result, DE_OK);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, fence_value, FIRST_FRAME_WAIT_NS), DE_OK);
        const u8 *data = de_gpu_buffer_map(f->dev, rb);
        ASSERT_TRUE(data);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                u32 index = y * w + x;
                double ao = data[(size_t)y * pitch + x] / 255.0;
                pixels[index * 4] = (float)ao;
                pixels[index * 4 + 1] = data[bent_offset + (size_t)y * pitch + x * 2] / 255.f;
                pixels[index * 4 + 2] = data[bent_offset + (size_t)y * pitch + x * 2 + 1] / 255.f;
                if (frame >= 8) {
                    sum[index] += ao;
                    sum2[index] += ao * ao;
                    samples[(size_t)(frame - 8) * w * h + index] = data[(size_t)y * pitch + x];
                }
            }
        ASSERT_TRUE(de__gtao_get(s, vf.view, &info));
        EXPECT_EQ(info.dispatches, 4u);
    }
    double variance = 0;
    u32 worst = 0;
    for (u32 i = 0; i < w * h; ++i) {
        sum2[i] = fmax(0, sum2[i] / 8 - (sum[i] / 8) * (sum[i] / 8));
        variance += sum2[i];
        if (sum2[i] > sum2[worst])
            worst = i;
    }
    qsort(sum2, (size_t)w * h, sizeof *sum2, compare_double);
    double p99 = sum2[(size_t)(w * h - 1) * 99 / 100];
    double maximum = sum2[(size_t)w * h - 1];
    printf("quality %u temporal8frames variance %.9f p99 %.9f max %.9f\n", quality,
           variance / (w * h), p99, maximum);
    if (maximum > TEMPORAL_VARIANCE_LIMIT) {
        printf("worst pixel %u,%u samples:", worst % w, worst / w);
        for (u32 i = 0; i < 8; ++i)
            printf(" %u", samples[(size_t)i * w * h + worst]);
        printf("\n");
    }
    EXPECT_TRUE(variance / (w * h) <= TEMPORAL_VARIANCE_LIMIT);
    EXPECT_TRUE(maximum <= TEMPORAL_VARIANCE_LIMIT);
    EXPECT_TRUE(p99 <= TEMPORAL_P99_VARIANCE_LIMIT);
    free(sum);
    free(sum2);
    free(samples);
    de__rg_reset(f->g);
    de_gpu_texture_destroy(f->dev, motion);
    de_gpu_buffer_destroy(f->dev, rb);
    return pixels;
}
static void raw_geometry(lx *f, de_gpu_backend backend, bool corner, de_quality quality) {
    lx_open(f, backend);
    const char *entry = "cs_gtao";
    const u32 groups[3] = {8, 8, 1};
    de__engine_shader_code code;
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(f->vis, "gtao", &entry, 1, &code), DE_OK);
    ASSERT_EQ(
        de__vis_kernel_create(f->vis, &code, 0, entry, groups, sizeof(de__gtao_params), &kernel),
        DE_OK);
    de__engine_shader_code_free(&code);
    de__gtao *s = NULL;
    if (quality != DE_QUALITY_OFF) {
        de_arena_destroy(f->arena);
        ASSERT_EQ(
            de_arena_create(&(de_arena_desc){.reserve_bytes = (u64)8 << 20, .name = "gtao.test"},
                            &f->arena),
            DE_OK);
        ASSERT_EQ(de__gtao_create_ex(f->dev, &f->alloc, f->vis, &s), DE_OK);
    }
    for (u32 c = 0; c < (corner ? 3u : 4u); ++c) {
        const u32 w = c == 0   ? 256u
                      : c == 1 ? 257u
                      : c == 2 ? 191u
                               : 193u,
                  h = c == 0   ? 256u
                      : c == 1 ? 161u
                      : c == 2 ? 255u
                               : 127u;
        const double angle = (c == 3 ? 48.0 : 15.0 + 22.5 * c) * PI64 / 180, rotation = .63 * c;
        de_vec3 plane = c == 3 ? de_v3_normalize((de_vec3){.37f, -.58f, .72f}) : (de_vec3){0, 0, 1};
        de_vec3 axis = {(f32)cos(rotation), (f32)sin(rotation), 0};
        if (c == 3)
            axis = de_v3_normalize(de_v3_cross((de_vec3){0, 1, 0}, plane));
        de_vec3 tangent = de_v3_cross(plane, axis);
        de_vec3 eye = {3 * ((f32)sin(angle) * axis.x + (f32)cos(angle) * plane.x),
                       3 * ((f32)sin(angle) * axis.y + (f32)cos(angle) * plane.y),
                       3 * ((f32)sin(angle) * axis.z + (f32)cos(angle) * plane.z)};
        de_mat4 view = de_m4_look_at(eye, (de_vec3){0, 0, 0}, plane);
        de_mat4 proj = de_m4_perspective_rz((f32)(55 * PI64 / 180), (f32)w / (f32)h, .1f);
        de_mat4 vp = de_m4_mul(&proj, &view), inv = de_m4_inverse(&vp);
        de__gs_view constants = {0};
        matrix_rows(constants.view, &view);
        matrix_rows(constants.proj, &proj);
        matrix_rows(constants.view_proj, &vp);
        matrix_rows(constants.inv_view_proj, &inv);
        matrix_rows(constants.prev_view_proj, &vp);
        constants.camera_position = (de__gs_float4){.v = {eye.x, eye.y, eye.z, 1}};
        de_gpu_buffer scene =
            de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = 2 * sizeof constants,
                                                               .stride = sizeof constants,
                                                               .memory = DE_MEM_UPLOAD,
                                                               .usage = DE_USAGE_STORAGE});
        ASSERT_TRUE(scene.id);
        u8 *scene_data = de_gpu_buffer_map(f->dev, scene);
        ASSERT_TRUE(scene_data);
        memset(scene_data, 0, sizeof constants);
        memcpy(scene_data + sizeof constants, &constants, sizeof constants);
        const u32 pitch = ((w * 4 + 255u) / 256u) * 256u, opitch = ((w * 16 + 255u) / 256u) * 256u;
        u8 *depth = zalloc((size_t)pitch * h), *normals = zalloc((size_t)pitch * h);
        double *distance = zalloc((size_t)w * h * sizeof *distance);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                double clip[4] = {2 * ((double)x + .5) / w - 1, 1 - 2 * ((double)y + .5) / h, 1, 1},
                       near_point[4];
                transform64(&inv, clip, near_point);
                double ray[3] = {near_point[0] / near_point[3] - eye.x,
                                 near_point[1] / near_point[3] - eye.y,
                                 near_point[2] / near_point[3] - eye.z};
                double ray_plane = ray[0] * plane.x + ray[1] * plane.y + ray[2] * plane.z;
                double eye_plane =
                    (double)eye.x * plane.x + (double)eye.y * plane.y + (double)eye.z * plane.z;
                double t = ray_plane < 0 ? -eye_plane / ray_plane : 1e30;
                double origin_axis = (double)eye.x * axis.x + (double)eye.y * axis.y;
                double ray_axis = ray[0] * axis.x + ray[1] * axis.y;
                double wall = corner && ray_axis < 0 ? -origin_axis / ray_axis : 1e30;
                bool floor = t <= wall;
                if (!floor)
                    t = wall;
                double p[4] = {eye.x + t * ray[0], eye.y + t * ray[1], eye.z + t * ray[2], 1},
                       ndc[4];
                transform64(&vp, p, ndc);
                float d = (float)(ndc[2] / ndc[3]);
                memcpy(depth + (size_t)y * pitch + x * 4, &d, 4);
                float n[3] = {floor ? plane.x : axis.x, floor ? plane.y : axis.y,
                              floor ? plane.z : axis.z};
                u32 packed = de__gb1_pack(n, .5f);
                memcpy(normals + (size_t)y * pitch + x * 4, &packed, 4);
                distance[(size_t)y * w + x] =
                    floor ? p[0] * axis.x + p[1] * axis.y + p[2] * axis.z : -1;
            }
        de_gpu_wait_idle(f->dev);
        de_gpu_texture d = uploaded_texture(f, w, h, DE_FMT_R32_FLOAT, depth, pitch);
        de_gpu_texture n = uploaded_texture(f, w, h, DE_FMT_RGB10A2_UNORM, normals, pitch);
        de_gpu_texture output = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.width = w,
                                           .height = h,
                                           .format = DE_FMT_RGBA32_FLOAT,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
        de_gpu_buffer readback =
            de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)opitch * h,
                                                               .memory = DE_MEM_READBACK,
                                                               .usage = DE_USAGE_COPY_DST});
        ASSERT_TRUE(output.id && readback.id);
        de__gtao_params pc = {.scene = de_gpu_buffer_bindless(f->dev, scene),
                              .slot = 1,
                              .depth = de_gpu_texture_bindless(f->dev, d, 0, false),
                              .normal = de_gpu_texture_bindless(f->dev, n, 0, false),
                              .width = w,
                              .height = h,
                              .tw = w,
                              .th = h,
                              .slices = 4,
                              .steps = 12,
                              .bent = 1,
                              .radius = 1,
                              .falloff = .8f,
                              .thickness = .2f,
                              .raw = de_gpu_texture_bindless(f->dev, output, 0, true)};
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gtao.raw.oracle");
        ASSERT_TRUE(cmd);
        de_gpu_barrier barrier = {
            .texture = output, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE};
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        barrier.before = DE_ACCESS_SHADER_WRITE;
        barrier.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, readback, 0, opitch, output, 0, 0);
        ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                      &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
                  DE_OK);
        de_gpu_wait_idle(f->dev);
        const u8 *mapped = de_gpu_buffer_map(f->dev, readback);
        ASSERT_TRUE(mapped);
        float *filtered = s ? filtered_geometry(f, s, scene, d, n, w, h, &vp, quality) : NULL;
        double sum = 0, expected = 0, error = 0, max_error = 0, bent_angle = 0, far_error = 0;
        u32 count = 0, far_count = 0, max_x = 0, max_y = 0;
        for (u32 y = 0; y < h; ++y)
            for (u32 x = w / 3; x < 2 * w / 3; ++x) {
                double dist = distance[(size_t)y * w + x];
                const float *pixel = filtered
                                         ? filtered + ((size_t)y * w + x) * 4
                                         : (const float *)(mapped + (size_t)y * opitch + x * 16);
                if (corner && dist >= 1.1 && dist <= 1.3) {
                    far_error = fmax(far_error, fabs(pixel[0] - 1));
                    ++far_count;
                }
                if (corner ? (dist < .0001 || dist > .05) : (dist < 0))
                    continue;
                double bent[3] = {0, 0, 1},
                       reference = corner ? corner_visibility(dist, 1, .8, bent) : 1;
                double bx = 2 * pixel[1] - 1, by = 2 * pixel[2] - 1, bz = 1 - fabs(bx) - fabs(by);
                if (bz < 0) {
                    double old = bx;
                    bx = (1 - fabs(by)) * (old >= 0 ? 1 : -1);
                    by = (1 - fabs(old)) * (by >= 0 ? 1 : -1);
                }
                double length = sqrt(bx * bx + by * by + bz * bz);
                double dot = (bx * (bent[0] * axis.x + bent[1] * tangent.x + bent[2] * plane.x) +
                              by * (bent[0] * axis.y + bent[1] * tangent.y + bent[2] * plane.y) +
                              bz * (bent[0] * axis.z + bent[1] * tangent.z + bent[2] * plane.z)) /
                             length;
                bent_angle += acos(fmin(fmax(dot, -1), 1)) * 180 / PI64;
                EXPECT_TRUE(isfinite(pixel[0]));
                sum += pixel[0];
                expected += reference;
                error += fabs(pixel[0] - reference);
                if (fabs(pixel[0] - reference) > max_error) {
                    max_error = fabs(pixel[0] - reference);
                    max_x = x;
                    max_y = y;
                }
                ++count;
            }
        ASSERT_GT(count, 32u);
        printf("bent mean angle %.6f degrees, far queries %u max error %.6f\n", bent_angle / count,
               far_count, far_error);
        printf("max error pixel %u,%u world corner-distance %.8f\n", max_x, max_y,
               distance[(size_t)max_y * w + max_x]);
        EXPECT_TRUE(bent_angle / count <= BENT_MEAN_ANGLE_DEGREES);
        printf("raw %s camera=%.1fdeg rotated=%.3f size=%ux%u queries=%u GPU=%.8f reference=%.8f "
               "relative=%.6f MAE=%.6f max=%.6f\n",
               corner ? "corner" : "flat", angle * 180 / PI64, rotation, w, h, count, sum / count,
               expected / count, fabs(sum - expected) / expected, error / expected, max_error);
        if (corner) {
            ASSERT_GT(far_count, 32u);
            EXPECT_TRUE(far_error <= FLAT_ABSOLUTE_ERROR);
            EXPECT_TRUE(fabs(sum - expected) / expected <= CORNER_RELATIVE_ERROR);
            EXPECT_TRUE(error / expected <= CORNER_RELATIVE_ERROR);
        } else
            EXPECT_TRUE(max_error <= FLAT_ABSOLUTE_ERROR);
        de_gpu_buffer_destroy(f->dev, scene);
        de_gpu_buffer_destroy(f->dev, readback);
        de_gpu_texture_destroy(f->dev, d);
        de_gpu_texture_destroy(f->dev, n);
        de_gpu_texture_destroy(f->dev, output);
        free(depth);
        free(normals);
        free(distance);
        free(filtered);
    }
    de__vis_kernel_destroy(f->vis, &kernel);
    de__gtao_destroy(s);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
#define RAW_TESTS(tag, backend)                                                                    \
    TEST_F(gtao, corner_analytic_##tag, lx) {                                                      \
        raw_geometry(f, backend, true, DE_QUALITY_OFF);                                            \
    }                                                                                              \
    TEST_F(gtao, flat_plane_##tag, lx) {                                                           \
        raw_geometry(f, backend, false, DE_QUALITY_OFF);                                           \
    }                                                                                              \
    TEST_F(gtao, filtered_corner_medium_##tag, lx) {                                               \
        raw_geometry(f, backend, true, DE_QUALITY_MEDIUM);                                         \
    }                                                                                              \
    TEST_F(gtao, filtered_corner_high_##tag, lx) {                                                 \
        raw_geometry(f, backend, true, DE_QUALITY_HIGH);                                           \
    }                                                                                              \
    TEST_F(gtao, filtered_flat_medium_##tag, lx) {                                                 \
        raw_geometry(f, backend, false, DE_QUALITY_MEDIUM);                                        \
    }                                                                                              \
    TEST_F(gtao, filtered_flat_high_##tag, lx) {                                                   \
        raw_geometry(f, backend, false, DE_QUALITY_HIGH);                                          \
    }
RAW_TESTS(metal, DE_GPU_METAL)
RAW_TESTS(vulkan, DE_GPU_VULKAN)
RAW_TESTS(d3d12, DE_GPU_D3D12)

static void temporal_disocclusion(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    const u32 w = 33, h = 19, tw = 17, th = 10, pitch = 256, fpitch = 512;
    u8 depth[256 * 19] = {0}, normal[256 * 19] = {0}, motion[256 * 19] = {0};
    u8 spatial[512 * 10] = {0}, previous[512 * 10] = {0}, surface[512 * 10] = {0};
    float n[3] = {0, 0, 1};
    u32 packed = de__gb1_pack(n, .5f);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            float d = .001f;
            /* Alternating full pixels prove closest representative selection, including the
             * final partial odd 2x2 footprints; shift by one full pixel across half boundaries. */
            if ((x & 1u) != 0)
                d = .00101f;
            memcpy(depth + y * pitch + x * 4, &d, 4);
            memcpy(normal + y * pitch + x * 4, &packed, 4);
            u16 m[2] = {0x2bc2u, 0};
            memcpy(motion + y * pitch + x * 4, m, 4); /* nearest half to 2/33 */
        }
    for (u32 y = 0; y < th; ++y)
        for (u32 x = 0; x < tw; ++x) {
            float current[4] = {.4f, .5f, .5f, x == tw - 1 ? .001f : .00101f};
            float old[4] = {.41f, .5f, .5f, 7};
            float s[4] = {x % 3 == 0 ? .0009f : (x == tw - 1 ? .001f : .00101f), 0, 0, 1};
            if (x % 3 == 1) {
                s[1] = 1;
                s[3] = 0;
            }
            memcpy(spatial + y * fpitch + x * 16, current, 16);
            memcpy(previous + y * fpitch + x * 16, old, 16);
            memcpy(surface + y * fpitch + x * 16, s, 16);
        }
    de_gpu_texture textures[8];
    textures[0] = uploaded_texture(f, w, h, DE_FMT_R32_FLOAT, depth, pitch);
    textures[1] = uploaded_texture(f, w, h, DE_FMT_RGB10A2_UNORM, normal, pitch);
    textures[2] = uploaded_texture(f, w, h, DE_FMT_RG16_FLOAT, motion, pitch);
    textures[3] = uploaded_texture_usage(f, tw, th, DE_FMT_RGBA32_FLOAT, spatial, fpitch, true);
    textures[4] = uploaded_texture_usage(f, tw, th, DE_FMT_RGBA32_FLOAT, previous, fpitch, true);
    textures[5] = uploaded_texture_usage(f, tw, th, DE_FMT_RGBA32_FLOAT, surface, fpitch, true);
    for (u32 i = 6; i < 8; ++i)
        textures[i] = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.width = tw,
                                           .height = th,
                                           .format = DE_FMT_RGBA32_FLOAT,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de__gs_view sc = {0};
    de_mat4 identity = de_m4_identity(), proj = de_m4_perspective_rz(1, (f32)w / (f32)h, .1f);
    de_mat4 inv = de_m4_inverse(&proj);
    matrix_rows(sc.view, &identity);
    matrix_rows(sc.proj, &proj);
    matrix_rows(sc.view_proj, &proj);
    matrix_rows(sc.prev_view_proj, &proj);
    matrix_rows(sc.inv_view_proj, &inv);
    de_gpu_buffer scene =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof sc,
                                                           .stride = sizeof sc,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(scene.id);
    memcpy(de_gpu_buffer_map(f->dev, scene), &sc, sizeof sc);
    de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = fpitch * th, .memory = DE_MEM_READBACK, .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(rb.id);
    const char *entry = "cs_gtao_temporal";
    const u32 group[] = {8, 8, 1};
    de__engine_shader_code code;
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(f->vis, "gtao_filter", &entry, 1, &code), DE_OK);
    ASSERT_EQ(
        de__vis_kernel_create(f->vis, &code, 0, entry, group, sizeof(de__gtao_params), &kernel),
        DE_OK);
    de__engine_shader_code_free(&code);
    de__gtao_params pc = {.scene = de_gpu_buffer_bindless(f->dev, scene),
                          .width = w,
                          .height = h,
                          .tw = tw,
                          .th = th,
                          .radius = 1,
                          .history_valid = 1,
                          .history_weight = .875f};
    pc.depth = de_gpu_texture_bindless(f->dev, textures[0], 0, false);
    pc.normal = de_gpu_texture_bindless(f->dev, textures[1], 0, false);
    pc.motion = de_gpu_texture_bindless(f->dev, textures[2], 0, false);
    pc.spatial = de_gpu_texture_bindless(f->dev, textures[3], 0, true);
    pc.previous = de_gpu_texture_bindless(f->dev, textures[4], 0, true);
    pc.previous_surface = de_gpu_texture_bindless(f->dev, textures[5], 0, true);
    pc.history = de_gpu_texture_bindless(f->dev, textures[6], 0, true);
    pc.history_surface = de_gpu_texture_bindless(f->dev, textures[7], 0, true);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gtao.temporal.oracle");
    ASSERT_TRUE(cmd);
    de_gpu_barrier barriers[2] = {{.texture = textures[6], .after = DE_ACCESS_SHADER_WRITE},
                                  {.texture = textures[7], .after = DE_ACCESS_SHADER_WRITE}};
    de_gpu_cmd_barriers(cmd, barriers, 2);
    de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (tw + 7) / 8, (th + 7) / 8, 1);
    barriers[0].before = DE_ACCESS_SHADER_WRITE;
    barriers[0].after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, barriers, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, fpitch, textures[6], 0, 0);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    const u8 *result = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(result);
    u32 accepted = 0, rejected = 0;
    for (u32 y = 0; y < th; ++y)
        for (u32 x = 0; x < tw; ++x) {
            u32 full_x = x == tw - 1 ? w - 1 : x * 2 + 1;
            double motion_ndc = half_to_f32(0x2bc2u);
            double prev_x = (double)full_x + .5 - motion_ndc * .5 * w;
            bool valid = prev_x >= 0;
            u32 old_x = valid ? (u32)prev_x / 2 : 0;
            valid = valid && old_x % 3 == 2;
            /* Last partial pixel has a different real depth from its previous representative. */
            if (x == tw - 1)
                valid = false;
            const float *value = (const float *)(result + y * fpitch + x * 16);
            EXPECT_FLOAT_EQ(value[3], valid ? 8 : 1, 1e-6);
            EXPECT_FLOAT_EQ(value[0], valid ? .40875 : .4, 2e-6);
            EXPECT_TRUE(isfinite(value[1]) && isfinite(value[2]));
            if (valid)
                ++accepted;
            else
                ++rejected;
        }
    EXPECT_GT(accepted, 0u);
    EXPECT_GT(rejected, 0u);
    printf("far100m temporal: %u motion-reprojected accepted, %u metre/normal/partial-footprint "
           "rejected\n",
           accepted, rejected);
    de__vis_kernel_destroy(f->vis, &kernel);
    for (u32 i = 0; i < 8; ++i)
        de_gpu_texture_destroy(f->dev, textures[i]);
    de_gpu_buffer_destroy(f->dev, scene);
    de_gpu_buffer_destroy(f->dev, rb);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
#define TEMPORAL_TEST(tag, backend)                                                                \
    TEST_F(gtao, temporal_disocclusion_##tag, lx) {                                                \
        temporal_disocclusion(f, backend);                                                         \
    }
TEMPORAL_TEST(metal, DE_GPU_METAL)
TEMPORAL_TEST(vulkan, DE_GPU_VULKAN)
TEMPORAL_TEST(d3d12, DE_GPU_D3D12)

static const de__gpu_backend_vtable *ao_original_vt;
static bool ao_fail_texture, ao_fail_submit;
static de_result fail_ao_texture(de_gpu_device *dev, de__gpu_texture *t,
                                 const de_gpu_texture_desc *d) {
    if (ao_fail_texture && d->name && strcmp(d->name, "gtao.surface") == 0) {
        ao_fail_texture = false;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected AO surface allocation");
    }
    return ao_original_vt->texture_create(dev, t, d);
}
static de_result report_ao_submit_failure(de_gpu_device *dev, de_gpu_queue queue,
                                          const de_gpu_submit *submit, u64 value) {
    de_result result = ao_original_vt->queue_submit(dev, queue, submit, value);
    if (result == DE_OK && submit->cmd_count && ao_fail_submit) {
        ao_fail_submit = false;
        return DE_FAIL(DE_ERR_INTERNAL, "injected AO reported submit failure");
    }
    return result;
}
typedef struct ao_fixture {
    lx *f;
    de__gtao *s;
    de_gpu_texture depth, normal, motion;
    de_gpu_buffer scene;
    de__view_frame vf;
    de__vis_context vc;
    de_render_settings settings;
    de__gtao_config config;
    de__lighting_inputs inputs;
} ao_fixture;
static void ao_begin(ao_fixture *a, u64 frame) {
    lx *f = a->f;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    a->vf.frame_index = frame;
    a->vc.depth = de_rg_import_texture(f->g, a->depth, DE_ACCESS_SHADER_READ);
    a->vc.gbuffer[DE__GB1] = de_rg_import_texture(f->g, a->normal, DE_ACCESS_SHADER_READ);
    a->vc.gbuffer[DE__GB_MOTION] = de_rg_import_texture(f->g, a->motion, DE_ACCESS_SHADER_READ);
    a->vc.scene_views = de_rg_import_buffer(f->g, a->scene, DE_ACCESS_SHADER_READ);
    memset(&a->inputs, 0, sizeof a->inputs);
}
static de_result ao_build(ao_fixture *a) {
    return de__gtao_build(a->s, &a->vf, &a->vc, &a->config, &a->inputs);
}
static void ao_end(ao_fixture *a, bool execute_graph, bool fail) {
    lx *f = a->f;
    de_result result = DE_OK;
    if (execute_graph) {
        de_rg_export(f->g, a->inputs.ao, DE_ACCESS_SHADER_READ);
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        ASSERT_EQ(de__rg_compile(f->g), DE_OK);
        ao_original_vt = f->dev->vt;
        de__gpu_backend_vtable vt = *ao_original_vt;
        if (fail) {
            vt.queue_submit = report_ao_submit_failure;
            ao_fail_submit = true;
            f->dev->vt = &vt;
        }
        result = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = a->vf.frame_index});
        f->dev->vt = ao_original_vt;
        EXPECT_EQ(result, fail ? DE_ERR_INTERNAL : DE_OK);
        if (fail)
            EXPECT_FALSE(ao_fail_submit);
        de_gpu_wait_idle(f->dev);
    }
    de__gtao_complete(a->s, a->vf.view, f->g, execute_graph, execute_graph && result == DE_OK);
    de_error_clear();
}
static void lifetime_retry(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_arena_destroy(f->arena);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (u64)8 << 20}, &f->arena), DE_OK);
    ao_fixture a = {.f = f,
                    .settings = {.ao = DE_QUALITY_LOW},
                    .config = {.radius = 1, .falloff = .8f, .thickness = .2f, .bent = true}};
    ASSERT_EQ(de__gtao_create_ex(f->dev, &f->alloc, f->vis, &a.s), DE_OK);
    const u32 w = 33, h = 19, pitch = 256;
    u8 d[256 * 19] = {0}, n[256 * 19] = {0}, m[256 * 19] = {0};
    float normal[3] = {0, 0, 1};
    u32 packed = de__gb1_pack(normal, .5f);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            float z = .02f;
            memcpy(d + y * pitch + x * 4, &z, 4);
            memcpy(n + y * pitch + x * 4, &packed, 4);
        }
    a.depth = uploaded_texture(f, w, h, DE_FMT_R32_FLOAT, d, pitch);
    a.normal = uploaded_texture(f, w, h, DE_FMT_RGB10A2_UNORM, n, pitch);
    a.motion = uploaded_texture(f, w, h, DE_FMT_RG16_FLOAT, m, pitch);
    de_mat4 identity = de_m4_identity(), proj = de_m4_perspective_rz(1, (f32)w / (f32)h, .1f),
            inv = de_m4_inverse(&proj);
    de__gs_view sc = {0};
    matrix_rows(sc.view, &identity);
    matrix_rows(sc.proj, &proj);
    matrix_rows(sc.view_proj, &proj);
    matrix_rows(sc.prev_view_proj, &proj);
    matrix_rows(sc.inv_view_proj, &inv);
    a.scene = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof sc,
                                                                 .stride = sizeof sc,
                                                                 .memory = DE_MEM_UPLOAD,
                                                                 .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(a.scene.id);
    memcpy(de_gpu_buffer_map(f->dev, a.scene), &sc, sizeof sc);
    a.vf = (de__view_frame){.view = {.id = 1},
                            .graph = f->g,
                            .settings = &a.settings,
                            .width = w,
                            .height = h,
                            .view_proj = proj,
                            .prev_view_proj = proj};
    a.vc.scene_views_bindless = de_gpu_buffer_bindless(f->dev, a.scene);
    ao_begin(&a, 0);
    ao_original_vt = f->dev->vt;
    de__gpu_backend_vtable vt = *ao_original_vt;
    vt.texture_create = fail_ao_texture;
    ao_fail_texture = true;
    f->dev->vt = &vt;
    EXPECT_EQ(ao_build(&a), DE_ERR_OUT_OF_MEMORY);
    f->dev->vt = ao_original_vt;
    EXPECT_FALSE(ao_fail_texture);
    ao_end(&a, false, false);
    ao_begin(&a, 0);
    ASSERT_EQ(ao_build(&a), DE_OK);
    ao_end(&a, true, false);
    de__gtao_info before, after;
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &before));
    EXPECT_EQ(before.slices, 2u);
    EXPECT_EQ(before.steps, 4u);
    for (u32 q = 2; q <= 4; ++q) {
        a.settings.ao = (de_quality)q;
        ao_begin(&a, q);
        ASSERT_EQ(ao_build(&a), DE_OK);
        ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
        EXPECT_EQ(after.slices, q == 2 ? 2u : q == 3 ? 3u : 4u);
        EXPECT_EQ(after.steps, q == 4 ? 12u : 8u);
        ao_end(&a, true, false);
    }
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &before));
    ao_begin(&a, 5);
    ASSERT_EQ(ao_build(&a), DE_OK);
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
    EXPECT_TRUE(after.history_valid);
    a.settings.ao = DE_QUALITY_LOW;
    EXPECT_EQ(ao_build(&a), DE_ERR_BUSY);
    a.settings.ao = DE_QUALITY_ULTRA;
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
    EXPECT_EQ(before.history.id, after.history.id);
    ao_end(&a, false, false);
    ao_begin(&a, 5);
    ASSERT_EQ(ao_build(&a), DE_OK);
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
    EXPECT_TRUE(after.history_valid);
    ao_end(&a, true, false);
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &before));
    a.settings.ao = DE_QUALITY_LOW;
    ao_begin(&a, 6);
    ao_fail_texture = true;
    f->dev->vt = &vt;
    EXPECT_EQ(ao_build(&a), DE_ERR_OUT_OF_MEMORY);
    f->dev->vt = ao_original_vt;
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
    EXPECT_EQ(before.history.id, after.history.id);
    ao_end(&a, false, false);
    a.settings.ao = DE_QUALITY_ULTRA;
    ao_begin(&a, 6);
    de_arena_info arena = de_arena_get_info(f->arena);
    ASSERT_TRUE(de_arena_push(f->arena, arena.reserved - arena.used - 32, 1));
    EXPECT_EQ(ao_build(&a), DE_ERR_OUT_OF_MEMORY);
    ao_end(&a, false, false);
    ao_begin(&a, 6);
    ASSERT_EQ(ao_build(&a), DE_OK);
    ao_end(&a, true, true);
    ao_begin(&a, 6);
    ASSERT_EQ(ao_build(&a), DE_OK);
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
    EXPECT_FALSE(after.history_valid);
    ao_end(&a, true, false);
    ao_begin(&a, 7);
    a.vc.occlusion_reset = true;
    ASSERT_EQ(ao_build(&a), DE_OK);
    ASSERT_TRUE(de__gtao_get(a.s, a.vf.view, &after));
    EXPECT_FALSE(after.history_valid);
    ao_end(&a, false, false);
    a.vc.occlusion_reset = false;
    ao_begin(&a, 7);
    a.settings.ao = DE_QUALITY_OFF;
    ASSERT_EQ(ao_build(&a), DE_OK);
    EXPECT_EQ(f->g->pass_count, 0u);
    EXPECT_FALSE(a.inputs.ao.id);
    ao_begin(&a, 8);
    a.settings.ao = DE_QUALITY_ULTRA;
    a.vf.probe_capture = true;
    ASSERT_EQ(ao_build(&a), DE_OK);
    EXPECT_EQ(f->g->pass_count, 0u);
    EXPECT_FALSE(a.inputs.ao.id);
    a.vf.probe_capture = false;
    ao_begin(&a, 9);
    de__vis_context ready = a.vc;
    a.vc.depth = (de_rg_resource){0};
    a.vc.gbuffer[DE__GB1] = (de_rg_resource){0};
    ASSERT_EQ(ao_build(&a), DE_OK);
    EXPECT_EQ(f->g->pass_count, 0u);
    EXPECT_FALSE(a.inputs.ao.id);
    a.vc.instance_count = 1;
    EXPECT_EQ(ao_build(&a), DE_ERR_INTERNAL);
    EXPECT_EQ(f->g->pass_count, 0u);
    ao_end(&a, false, false);
    a.vc = ready;
    a.vf.view.id = 257;
    a.settings.ao = DE_QUALITY_OFF;
    ao_begin(&a, 9);
    ASSERT_EQ(ao_build(&a), DE_OK);
    EXPECT_FALSE(de__gtao_get(a.s, (de_view){.id = 1}, &after));
    EXPECT_EQ(f->g->pass_count, 0u);
    a.settings.ao = DE_QUALITY_ULTRA;
    ao_begin(&a, 10);
    ASSERT_EQ(ao_build(&a), DE_OK);
    EXPECT_FALSE(de__gtao_get(a.s, (de_view){.id = 1}, &after));
    ao_end(&a, true, false);
    de__rg_reset(f->g);
    de__gtao_destroy(a.s);
    de_gpu_texture_destroy(f->dev, a.depth);
    de_gpu_texture_destroy(f->dev, a.normal);
    de_gpu_texture_destroy(f->dev, a.motion);
    de_gpu_buffer_destroy(f->dev, a.scene);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
#define LIFETIME_TEST(tag, backend)                                                                \
    TEST_F(gtao, lifetime_retry_##tag, lx) {                                                       \
        lifetime_retry(f, backend);                                                                \
    }
LIFETIME_TEST(metal, DE_GPU_METAL)
LIFETIME_TEST(vulkan, DE_GPU_VULKAN)
LIFETIME_TEST(d3d12, DE_GPU_D3D12)

static double lagarde64(double nov, double ao, double roughness) {
    return fmin(1, fmax(0, pow(fmax(nov + ao, 0), exp2(-16 * roughness - 1)) - 1 + ao));
}
static double multibounce64(double visibility, double rho) {
    double polynomial = (2.0404 * rho - .3324) * visibility * visibility * visibility +
                        (-4.7951 * rho + .6417) * visibility * visibility +
                        (2.7552 * rho + .6903) * visibility;
    return fmin(1, fmax(visibility, polynomial));
}
static void composition_oracle(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    const u32 count = 1152, pitch = count * 16;
    typedef struct ao_query {
        float color_ao[4], view_roughness[4], material[4];
    } ao_query;
    ao_query queries[1152];
    float ao[1152 * 4] = {0}, diffuse[1152 * 4] = {0}, specular[1152 * 4] = {0};
    const float materials[] = {0, .2f, .7f, 1}, raw[] = {0, .35f, .8f, 1},
                confidence[] = {0, .4f, 1};
    u32 at = 0;
    for (u32 environment = 0; environment < 3; ++environment)
        for (u32 flag = 0; flag < 2; ++flag)
            for (u32 m = 0; m < 4; ++m)
                for (u32 a = 0; a < 4; ++a)
                    for (u32 rough = 0; rough < 2; ++rough)
                        for (u32 view = 0; view < 2; ++view)
                            for (u32 c = 0; c < 3; ++c) {
                                float nov = view ? .9f : .2f, r = rough ? .7f : .05f;
                                queries[at] = (ao_query){
                                    .color_ao = {.17f, .53f, .81f, materials[m]},
                                    .view_roughness = {(float)sqrt(1 - (double)nov * nov), 0, nov,
                                                       r},
                                    .material = {.23f, (float)flag, (float)environment, 0}};
                                ao[at * 4] = raw[a];
                                diffuse[at * 4] = .13f;
                                diffuse[at * 4 + 1] = .31f;
                                diffuse[at * 4 + 2] = .61f;
                                diffuse[at * 4 + 3] = confidence[c];
                                double so = lagarde64(nov, materials[m], r);
                                specular[at * 4] = (float)(.23 * so);
                                specular[at * 4 + 1] = (float)(.47 * so);
                                specular[at * 4 + 2] = (float)(.79 * so);
                                specular[at * 4 + 3] = confidence[c];
                                ++at;
                            }
    ASSERT_EQ(at, count);
    de_gpu_texture inputs[3] = {
        uploaded_texture(f, count, 1, DE_FMT_RGBA32_FLOAT, ao, pitch),
        uploaded_texture(f, count, 1, DE_FMT_RGBA32_FLOAT, diffuse, pitch),
        uploaded_texture(f, count, 1, DE_FMT_RGBA32_FLOAT, specular, pitch)};
    u8 *irr = zalloc(768 * 20), *dist = zalloc(1280 * 36), *radiance = zalloc(256 * 24);
    const float irradiance[4] = {.7f, 1.1f, 1.9f, 1}, moments[4] = {1, 1, 0, 0},
                probe_color[4] = {.2f, .7f, 1.3f, 1};
    for (u32 y = 0; y < 20; ++y)
        for (u32 x = 0; x < 40; ++x)
            memcpy(irr + y * 768 + x * 16, irradiance, 16);
    for (u32 y = 0; y < 36; ++y)
        for (u32 x = 0; x < 72; ++x)
            memcpy(dist + y * 1280 + x * 16, moments, 16);
    for (u32 y = 0; y < 24; ++y)
        for (u32 x = 0; x < 4; ++x)
            memcpy(radiance + y * 256 + x * 16, probe_color, 16);
    de_gpu_texture gi_textures[2] = {
        uploaded_texture_usage(f, 40, 20, DE_FMT_RGBA32_FLOAT, irr, 768, true),
        uploaded_texture_usage(f, 72, 36, DE_FMT_RGBA32_FLOAT, dist, 1280, true)};
    de_gpu_texture probe_texture = uploaded_texture(f, 4, 24, DE_FMT_RGBA32_FLOAT, radiance, 256);
    free(irr);
    free(dist);
    free(radiance);
    de_gpu_buffer classification = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){
            .size = 8 * 16, .stride = 16, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(classification.id);
    memset(de_gpu_buffer_map(f->dev, classification), 0, 8 * 16);
    de__gi_gpu gi[2] = {0};
    gi[0].counts[0] = 1;
    gi[1] = (de__gi_gpu){.local = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
                         .extent = {.6f, .6f, .6f, 0},
                         .step = {.6f, .6f, .6f, 0},
                         .rotation = {0, 0, 0, 1},
                         .counts = {2, 2, 2, 4},
                         .textures = {de_gpu_texture_bindless(f->dev, gi_textures[0], 0, true),
                                      de_gpu_texture_bindless(f->dev, gi_textures[1], 0, true),
                                      de_gpu_buffer_bindless(f->dev, classification), 0},
                         .params = {1, 1, 10, 1}};
    de_gpu_buffer gi_records =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof gi,
                                                           .stride = sizeof gi[0],
                                                           .memory = DE_MEM_UPLOAD,
                                                           .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(gi_records.id);
    memcpy(de_gpu_buffer_map(f->dev, gi_records), gi, sizeof gi);
    struct {
        float position[4], extent[4], rotation[4], blend[4];
        u32 info[4];
    } probe = {.extent = {1, 1, 1, 1},
               .rotation = {0, 0, 0, 1},
               .blend = {2.5f, 4, 0, 0},
               .info = {0, 0, de_gpu_texture_bindless(f->dev, probe_texture, 0, false), 1}};
    de_gpu_buffer probe_records =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof probe,
                                                           .stride = sizeof probe,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(probe_records.id);
    memcpy(de_gpu_buffer_map(f->dev, probe_records), &probe, sizeof probe);
    de_gpu_buffer probe_list = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){
            .size = 65 * 4, .stride = 4, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(probe_list.id);
    memset(de_gpu_buffer_map(f->dev, probe_list), 0, 65 * 4);
    ((u32 *)de_gpu_buffer_map(f->dev, probe_list))[0] = 1;
    de_gpu_sampler sampler = de_gpu_sampler_create(f->dev, &(de_gpu_sampler_desc){0});
    ASSERT_TRUE(sampler.id);
    de_gpu_buffer query =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof queries,
                                                           .stride = sizeof(ao_query),
                                                           .memory = DE_MEM_UPLOAD,
                                                           .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(query.id);
    memcpy(de_gpu_buffer_map(f->dev, query), queries, sizeof queries);
    de_gpu_buffer out = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = count * 64,
                                      .stride = 16,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = count * 64, .memory = DE_MEM_READBACK, .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(out.id && rb.id);
    const char *entry = "cs_gtao_lighting_queries";
    const u32 group[] = {8, 8, 1};
    de__engine_shader_code code;
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(f->vis, "gtao_filter", &entry, 1, &code), DE_OK);
    ASSERT_EQ(
        de__vis_kernel_create(f->vis, &code, 0, entry, group, sizeof(de__gtao_params), &kernel),
        DE_OK);
    de__engine_shader_code_free(&code);
    de__gtao_params pc = {.width = count,
                          .previous = de_gpu_buffer_bindless(f->dev, query),
                          .packed = de_gpu_buffer_bindless(f->dev, out),
                          .scene = de_gpu_buffer_bindless(f->dev, gi_records),
                          .slot = de_gpu_buffer_bindless(f->dev, probe_records),
                          .previous_surface = de_gpu_buffer_bindless(f->dev, probe_list),
                          .history = de_gpu_sampler_bindless(f->dev, sampler),
                          .normal = de__brdf_luts_bindless(f->luts),
                          .depth = de_gpu_texture_bindless(f->dev, inputs[0], 0, false),
                          .motion = de_gpu_texture_bindless(f->dev, inputs[1], 0, false),
                          .raw = de_gpu_texture_bindless(f->dev, inputs[2], 0, false)};
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gtao.lighting.oracle");
    ASSERT_TRUE(cmd);
    de_gpu_barrier barrier = {.buffer = out, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_set_pipeline(cmd, kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (count + 7) / 8, 1, 1);
    barrier.before = DE_ACCESS_SHADER_WRITE;
    barrier.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer(cmd, rb, 0, out, 0, count * 64);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    const float *result = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(result);
    for (u32 i = 0; i < count; ++i) {
        const ao_query *q = &queries[i];
        bool flag = q->material[1] > 0;
        double material = q->color_ao[3],
               visibility = flag ? fmin(material, ao[i * 4]) : material * ao[i * 4];
        double nov = q->view_roughness[2], rough = q->view_roughness[3];
        double combined = lagarde64(nov, visibility, rough),
               original = lagarde64(nov, material, rough);
        double confidence = diffuse[i * 4 + 3];
        for (u32 channel = 0; channel < 3; ++channel) {
            double factor =
                flag ? multibounce64(visibility, q->color_ao[channel] * (1 - q->material[0]))
                     : visibility;
            double gi_scale = q->material[2] == 1 ? .4 + .6 * irradiance[channel] / PI64 : 1;
            double probe_scale = q->material[2] == 2 ? .6 + .4 * probe_color[channel] : 1;
            double expected_d = (result[i * 16 + 8 + channel] * gi_scale * (1 - confidence) +
                                 diffuse[i * 4 + channel] * confidence) *
                                factor;
            double override =
                specular[i * 4 + channel] * (flag ? (original > 0 ? combined / original : 0) : 1);
            double expected_s =
                result[i * 16 + 12 + channel] * probe_scale * combined * (1 - confidence) +
                override * confidence;
            EXPECT_FLOAT_EQ(result[i * 16 + channel], expected_d, 3e-5);
            EXPECT_FLOAT_EQ(result[i * 16 + 4 + channel], expected_s, 3e-5);
        }
    }
    printf("1152 actual IBL/GI8probeinterpolation/localreflectionprobe, nonwhite "
           "material/GTAO/min/multibounce/roughness/view/SSRconfidence/endpoints/legacy queries "
           "passed\n");
    de__vis_kernel_destroy(f->vis, &kernel);
    for (u32 i = 0; i < 3; ++i)
        de_gpu_texture_destroy(f->dev, inputs[i]);
    de_gpu_buffer_destroy(f->dev, query);
    de_gpu_buffer_destroy(f->dev, out);
    de_gpu_buffer_destroy(f->dev, rb);
    EXPECT_EQ(validation_errors(f->dev), 0u);
    de_gpu_texture_destroy(f->dev, gi_textures[0]);
    de_gpu_texture_destroy(f->dev, gi_textures[1]);
    de_gpu_texture_destroy(f->dev, probe_texture);
    de_gpu_buffer_destroy(f->dev, classification);
    de_gpu_buffer_destroy(f->dev, gi_records);
    de_gpu_buffer_destroy(f->dev, probe_records);
    de_gpu_buffer_destroy(f->dev, probe_list);
    de_gpu_sampler_destroy(f->dev, sampler);
}
#define COMPOSITION_TEST(tag, backend)                                                             \
    TEST_F(gtao, composition_oracle_##tag, lx) {                                                   \
        composition_oracle(f, backend);                                                            \
    }
COMPOSITION_TEST(metal, DE_GPU_METAL)
COMPOSITION_TEST(vulkan, DE_GPU_VULKAN)
COMPOSITION_TEST(d3d12, DE_GPU_D3D12)

typedef struct scene_readback {
    de_rg_resource textures[8], buffer;
    u32 pitch[8];
    u64 offset[8];
} scene_readback;
static void read_scene(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    scene_readback *p = user;
    for (u32 i = 0; i < 8; ++i)
        if (p->textures[i].id)
            de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->buffer), p->offset[i],
                                              p->pitch[i], de_rg_texture(pass, p->textures[i]), 0,
                                              0);
}
typedef struct scene_images {
    float *hdr, *position, *depth, *diffuse, *specular;
    u8 *ao, *reference;
    u16 *motion;
} scene_images;
static scene_images scene_frame(lx *f, de__gtao *ao, de__gtao *reference, de_quality quality,
                                de_vec3 eye, de_mat4 previous, u64 frame, float exposure) {
    const u32 w = 129, h = 97;
    world_begin(&f->w);
    de_mat4 view = de_m4_look_at(eye, (de_vec3){0, 0, 0}, (de_vec3){0, 1, 0});
    de_mat4 proj = de_m4_perspective_rz(1.04719755f, (f32)w / (f32)h, .1f),
            vp = de_m4_mul(&proj, &view);
    ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
    ASSERT_EQ(de__gpu_scene_set_view(f->gs, 0,
                                     &(de__gpu_view_params){.view = view,
                                                            .proj = proj,
                                                            .camera_position = eye,
                                                            .width = w,
                                                            .height = h,
                                                            .pre_exposure = exposure,
                                                            .prev_pre_exposure = exposure}),
              DE_OK);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
    de_render_settings settings = {.ao = quality};
    de__vis_context *vc = NULL;
    ASSERT_EQ(de__vis_frame_build(f->vis, f->g,
                                  &(de__vis_frame_desc){.gpu_scene = f->gs,
                                                        .view = {.id = 1},
                                                        .view_slot = 0,
                                                        .frame_index = frame,
                                                        .frames_completed = frame,
                                                        .width = w,
                                                        .height = h,
                                                        .view_matrix = view,
                                                        .proj = proj,
                                                        .prev_view_proj = previous,
                                                        .camera_position = eye,
                                                        .settings = &settings},
                                  &vc),
              DE_OK);
    ASSERT_TRUE(vc);
    de__view_frame vf = {.view = {.id = 1},
                         .graph = f->g,
                         .settings = &settings,
                         .frame_index = frame,
                         .width = w,
                         .height = h,
                         .view_matrix = view,
                         .proj = proj,
                         .view_proj = vp,
                         .prev_view_proj = previous};
    de__gtao_config config = {.radius = 1, .falloff = .8f, .thickness = .2f, .bent = false};
    de__lighting_inputs *inputs = de__lighting_view_inputs(f->lt, 0);
    inputs->pre_exposure = exposure;
    ASSERT_EQ(de__gtao_build(ao, &vf, vc, &config, inputs), DE_OK);
    de_rg_resource ao_output = inputs->ao;
    de__lighting_inputs fresh = {0};
    de__vis_context fresh_vc = *vc;
    fresh_vc.occlusion_reset = true;
    ASSERT_EQ(de__gtao_build(reference, &vf, &fresh_vc, &config, &fresh), DE_OK);
    de__lighting_context *lc = NULL;
    ASSERT_EQ(de__lighting_build(f->lt, f->g, vc, &lc), DE_OK);
    ASSERT_TRUE(lc && lc->lit);
    if (f->forward) {
        de__forward_context *fc = NULL;
        ASSERT_EQ(de__forward_build(f->forward, f->g, vc, lc, &f->w.f, &fc), DE_OK);
        ASSERT_TRUE(fc);
    }
    scene_readback *r = de_arena_push_zero(f->arena, sizeof *r, 16);
    ASSERT_TRUE(r);
    *r = (scene_readback){.textures = {ao_output, fresh.ao, vc->resolve_debug_pos, lc->target,
                                       vc->depth, vc->gbuffer[DE__GB_MOTION], lc->diffuse,
                                       lc->specular}};
    const u32 bpp[] = {1, 1, 16, 8, 4, 4, 8, 8};
    u64 bytes = 0;
    for (u32 i = 0; i < 8; ++i) {
        r->pitch[i] = (w * bpp[i] + 255u) & ~255u;
        r->offset[i] = bytes;
        bytes = align_up(bytes + (u64)r->pitch[i] * h, 512);
    }
    de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = bytes, .memory = DE_MEM_READBACK, .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(rb.id);
    r->buffer = de_rg_import_buffer(f->g, rb, DE_ACCESS_NONE);
    de_rg_pass *pass =
        de_rg_add_pass(f->g, "gtao.scene.readback", DE_QUEUE_GRAPHICS, read_scene, r);
    de_rg_pass_side_effect(pass);
    for (u32 i = 0; i < 8; ++i)
        if (r->textures[i].id)
            de_rg_read(pass, r->textures[i], DE_ACCESS_COPY_SRC);
    de_rg_write(pass, r->buffer, DE_ACCESS_COPY_DST);
    pass = de_rg_add_pass(f->g, "gtao.scene.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < 3; ++i)
        de_rg_read(pass, vc->gbuffer[i], DE_ACCESS_SHADER_READ);
    de_rg_write(pass, vc->gbuffer[DE__GB_MOTION], DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, vc->resolve_debug_pos, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, lc->target, DE_ACCESS_SHADER_WRITE);
    for (u32 i = 0; i < 2; ++i)
        if (r->textures[i].id)
            de_rg_read(pass, r->textures[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 6; i < 8; ++i)
        if (r->textures[i].id)
            de_rg_write(pass, r->textures[i], DE_ACCESS_SHADER_WRITE);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    const u64 fence_value = ++f->frames;
    de_result result = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = frame,
                                                                   .frame_fence = f->fence,
                                                                   .frame_value = fence_value});
    de__gtao_complete(ao, vf.view, f->g, true, result == DE_OK);
    de__gtao_complete(reference, vf.view, f->g, true, result == DE_OK);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    ASSERT_EQ(result, DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, fence_value, FIRST_FRAME_WAIT_NS), DE_OK);
    const u8 *data = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(data);
    scene_images image = {.hdr = zalloc((size_t)w * h * 4 * sizeof(float)),
                          .position = zalloc((size_t)w * h * 4 * sizeof(float)),
                          .depth = zalloc((size_t)w * h * sizeof(float)),
                          .ao = zalloc((size_t)w * h),
                          .reference = zalloc((size_t)w * h),
                          .motion = zalloc((size_t)w * h * 2 * sizeof(u16)),
                          .diffuse = zalloc((size_t)w * h * 4 * sizeof(float)),
                          .specular = zalloc((size_t)w * h * 4 * sizeof(float))};
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u32 i = y * w + x;
            image.ao[i] = r->textures[0].id ? data[r->offset[0] + (u64)y * r->pitch[0] + x] : 255;
            image.reference[i] =
                r->textures[1].id ? data[r->offset[1] + (u64)y * r->pitch[1] + x] : 255;
            memcpy(image.position + i * 4, data + r->offset[2] + (u64)y * r->pitch[2] + x * 16, 16);
            for (u32 c = 0; c < 4; ++c) {
                u16 value;
                memcpy(&value, data + r->offset[3] + (u64)y * r->pitch[3] + x * 8 + c * 2, 2);
                image.hdr[i * 4 + c] = half_to_f32(value);
            }
            for (u32 t = 6; t < 8; ++t)
                for (u32 c = 0; c < 4; ++c) {
                    u16 value;
                    memcpy(&value, data + r->offset[t] + (u64)y * r->pitch[t] + x * 8 + c * 2, 2);
                    (t == 6 ? image.diffuse : image.specular)[i * 4 + c] = half_to_f32(value);
                }
            memcpy(image.depth + i, data + r->offset[4] + (u64)y * r->pitch[4] + x * 4, 4);
            memcpy(image.motion + i * 2, data + r->offset[5] + (u64)y * r->pitch[5] + x * 4, 4);
        }
    de_gpu_buffer_destroy(f->dev, rb);
    return image;
}
static void free_scene(scene_images *s) {
    free(s->hdr);
    free(s->position);
    free(s->depth);
    free(s->ao);
    free(s->reference);
    free(s->motion);
    free(s->diffuse);
    free(s->specular);
    memset(s, 0, sizeof *s);
}
static void moving_camera_and_composition(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_arena_destroy(f->arena);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (u64)32 << 20}, &f->arena), DE_OK);
    cvar_set("r.raster_path", "compute");
    cvar_set("r.ambient_fallback", "1");
    cvar_set("r.split_lighting", "1");
    de__gtao *ao = NULL, *reference = NULL;
    ASSERT_EQ(de__gtao_create_ex(f->dev, &f->alloc, f->vis, &ao), DE_OK);
    ASSERT_EQ(de__gtao_create_ex(f->dev, &f->alloc, f->vis, &reference), DE_OK);
    u32 mat = material_add(.6f, .7f, 0, 0);
    g_mat[mat - 1].standard.base_color = (de_vec4){.17f, .53f, .81f, 1};
    u32 floor = lx_mesh_plane(f, 1, 1, 8), blocker = lx_mesh_plane(f, 1, 1, .7f);
    world_set(&f->w, 0, floor, at(0, 0, 0), mat);
    world_set(&f->w, 1, blocker, at(-.7f, 0, .35f), mat);
    world_set(&f->w, 2, floor, trs((de_vec3){-1, 0, 2}, (de_vec3){0, 1, 0}, (f32)(PI64 * .5), 1),
              mat);
    de_mat4 previous = de_m4_identity();
    scene_images old = {0};
    u32 ghost_queries = 0, motion_queries = 0, nontrivial_history = 0;
    for (u32 frame = 0; frame < 16; ++frame) {
        de_vec3 eye = {1 + (frame >= 8 ? .03f * (f32)(frame - 7) : 0), 0, 3};
        if (frame == 8)
            world_set(&f->w, 1, blocker, at(3, 0, 1), mat);
        de_mat4 view = de_m4_look_at(eye, (de_vec3){0}, (de_vec3){0, 1, 0}),
                proj = de_m4_perspective_rz(1.04719755f, 129.f / 97, .1f),
                vp = de_m4_mul(&proj, &view);
        if (frame == 0)
            previous = vp;
        scene_images image =
            scene_frame(f, ao, reference, DE_QUALITY_HIGH, eye, previous, frame, 1);
        if (frame == 8) {
            for (u32 y = 3; y < 94; ++y)
                for (u32 x = 3; x < 126; ++x) {
                    u32 i = y * 129 + x;
                    float *p = image.position + i * 4;
                    if (image.depth[i] <= 0 || fabs(p[2]) > .001 || fabs(p[0] + .7f) > .35 ||
                        fabs(p[1]) > .35)
                        continue;
                    /* Project this newly exposed floor point into the real previous image. The
                     * previous foreground is .35 metres above it with the same +Z normal. */
                    double point[4] = {p[0], p[1], p[2], 1}, clip[4];
                    transform64(&previous, point, clip);
                    double u = clip[0] / clip[3] * .5 + .5, v = .5 - clip[1] / clip[3] * .5;
                    if (u < 0 || u >= 1 || v < 0 || v >= 1)
                        continue;
                    u32 q = (u32)(v * 97) * 129 + (u32)(u * 129);
                    if (old.position[q * 4 + 2] < .3f)
                        continue;
                    EXPECT_TRUE(abs((int)image.ao[i] - (int)image.reference[i]) <= 2);
                    ++ghost_queries;
                    if (abs((int)old.ao[q] - (int)image.reference[i]) >= 13)
                        ++nontrivial_history;
                    if (fabs(half_to_f32(image.motion[i * 2])) +
                            fabs(half_to_f32(image.motion[i * 2 + 1])) >
                        1e-4)
                        ++motion_queries;
                }
        }
        free_scene(&old);
        old = image;
        previous = vp;
    }
    printf("moving camera/foreground: %u newly exposed same-normal floor queries, %u nonzero "
           "actual motion\n",
           ghost_queries, motion_queries);
    EXPECT_GT(ghost_queries, 20u);
    EXPECT_GT(motion_queries, 20u);
    free_scene(&old);
    EXPECT_GT(nontrivial_history, 20u);
    printf("nontrivial prior-vs-fresh visibility differences >=5%%: %u\n", nontrivial_history);
    de_vec3 eye = {1.24f, 0, 3};
    DeLight light = {.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 3};
    world_light(&f->w, 0, &light, at(0, 0, 0));
    cvar_set("r.ambient_fallback", "0");
    scene_images direct_on = scene_frame(f, ao, reference, DE_QUALITY_HIGH, eye, previous, 16, 1);
    scene_images direct_off = scene_frame(f, ao, reference, DE_QUALITY_OFF, eye, previous, 17, 1);
    EXPECT_EQ(memcmp(direct_on.hdr, direct_off.hdr, 129 * 97 * 4 * sizeof(float)), 0);
    double direct_energy = 0;
    for (u32 i = 0; i < 129 * 97; ++i)
        for (u32 c = 0; c < 3; ++c)
            direct_energy += direct_off.hdr[i * 4 + c];
    EXPECT_TRUE(direct_energy > 1.0);
    free_scene(&direct_on);
    free_scene(&direct_off);
    f->w.f.lights.count = 0;
    f->w.f.removed_lights[0] = 0;
    f->w.f.removed_light_count = 1;
    cvar_set("r.ambient_fallback", "1");
    scene_images indirect_on = scene_frame(f, ao, reference, DE_QUALITY_HIGH, eye, previous, 18, 1);
    scene_images indirect_off = scene_frame(f, ao, reference, DE_QUALITY_OFF, eye, previous, 19, 1);
    de__lighting_stats stats = de__lighting_view_stats(f->lt, 0);
    EXPECT_TRUE(stats.valid);
    EXPECT_EQ(stats.directional, 0u);
    EXPECT_EQ(stats.local, 0u);
    scene_images exposed = scene_frame(f, ao, reference, DE_QUALITY_HIGH, eye, previous, 20, 2);
    u32 dark = 0;
    for (u32 i = 0; i < 129 * 97; ++i)
        if (indirect_on.depth[i] > 0) {
            float *p = indirect_on.position + i * 4;
            de_vec3 normal = fabs(p[0] + 1) < .001f ? (de_vec3){1, 0, 0} : (de_vec3){0, 0, 1};
            de_vec3 v = de_v3_normalize((de_vec3){eye.x - p[0], eye.y - p[1], eye.z - p[2]});
            double nov = fmax(de_v3_dot(normal, v), 1e-4);
            for (u32 c = 0; c < 3; ++c) {
                const double albedo[] = {.17, .53, .81};
                double factor = multibounce64(indirect_on.ao[i] / 255.0, albedo[c]);
                double specular_factor = lagarde64(nov, indirect_on.ao[i] / 255.0, .7);
                EXPECT_FLOAT_EQ(indirect_on.diffuse[i * 4 + c],
                                indirect_off.diffuse[i * 4 + c] * factor, .002);
                EXPECT_FLOAT_EQ(indirect_on.hdr[i * 4 + c],
                                indirect_off.diffuse[i * 4 + c] * factor +
                                    indirect_off.specular[i * 4 + c] * specular_factor,
                                .002);
                double exposed_factor = multibounce64(exposed.ao[i] / 255.0, albedo[c]);
                double exposed_specular = lagarde64(nov, exposed.ao[i] / 255.0, .7);
                EXPECT_FLOAT_EQ(exposed.hdr[i * 4 + c],
                                (indirect_off.diffuse[i * 4 + c] * exposed_factor +
                                 indirect_off.specular[i * 4 + c] * exposed_specular) *
                                    2,
                                .004);
            }
            if (indirect_on.ao[i] < 230)
                ++dark;
        }
    EXPECT_GT(dark, 20u);
    printf("indirect-only material/exposure composition: %u genuinely occluded pixels\n", dark);
    free_scene(&indirect_on);
    free_scene(&indirect_off);
    free_scene(&exposed);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    ASSERT_TRUE(de__forward_enabled(f->forward));
    u32 transparent = material_add(.5f, .7f, 0, 0);
    g_mat[transparent - 1].alpha_mode = DE_COOKED_ALPHA_BLEND;
    g_mat[transparent - 1].standard.base_color = (de_vec4){.8f, .2f, .4f, .65f};
    world_set(&f->w, 3, blocker, at(-.7f, 0, .25f), transparent);
    scene_images forward_on = scene_frame(f, ao, reference, DE_QUALITY_HIGH, eye, previous, 21, 1);
    scene_images forward_off = scene_frame(f, ao, reference, DE_QUALITY_OFF, eye, previous, 22, 1);
    u32 forward_queries = 0, occluded_forward = 0;
    for (u32 i = 0; i < 129 * 97; ++i) {
        float *p = forward_on.position + i * 4;
        if (forward_on.depth[i] <= 0 || fabs(p[2]) > .001)
            continue;
        double t = (.25 - eye.z) / (p[2] - eye.z);
        double x = eye.x + t * (p[0] - eye.x), y = eye.y + t * (p[1] - eye.y);
        if (fabs(x + .7) > .25 || fabs(y) > .25)
            continue;
        for (u32 c = 0; c < 3; ++c) {
            double contribution_on =
                forward_on.hdr[i * 4 + c] -
                .35 * (forward_on.diffuse[i * 4 + c] + forward_on.specular[i * 4 + c]);
            double contribution_off =
                forward_off.hdr[i * 4 + c] -
                .35 * (forward_off.diffuse[i * 4 + c] + forward_off.specular[i * 4 + c]);
            EXPECT_FLOAT_EQ(contribution_on, contribution_off, .002);
        }
        ++forward_queries;
        if (forward_on.ao[i] < 230)
            ++occluded_forward;
    }
    EXPECT_GT(forward_queries, 20u);
    EXPECT_GT(occluded_forward, 20u);
    printf(
        "forward own-surface AO: %u transparent interior queries, %u over occluded opaque pixels\n",
        forward_queries, occluded_forward);
    free_scene(&forward_on);
    free_scene(&forward_off);
    de__rg_reset(f->g);
    de__gtao_destroy(ao);
    de__gtao_destroy(reference);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
#define SCENE_TEST(tag, backend)                                                                   \
    TEST_F(gtao, moving_camera_and_composition_##tag, lx) {                                        \
        moving_camera_and_composition(f, backend);                                                 \
    }
SCENE_TEST(metal, DE_GPU_METAL)
SCENE_TEST(vulkan, DE_GPU_VULKAN)
SCENE_TEST(d3d12, DE_GPU_D3D12)

typedef struct capture_diag {
    de_renderer *renderer;
    de_gpu_buffer buffer;
} capture_diag;
static void capture_diagnostic_stage(de__view_frame *vf, void *user) {
    if (!vf->probe_capture)
        return;
    capture_diag *d = user;
    de__vis_shared *vis = de__views_vis(d->renderer);
    const de__vis_context *vc = de__vis_frame_context(vis, vf->view, vf->graph, vf->frame_index);
    de__lighting_context *lc = de__lighting_frame_context(de__views_lighting(d->renderer), vf->view,
                                                          vf->graph, vf->frame_index);
    ASSERT_TRUE(vc && lc);
    scene_readback *r = de_arena_push_zero(vf->graph->arena, sizeof *r, 16);
    ASSERT_TRUE(r);
    *r = (scene_readback){.textures = {vc->depth, lc->target},
                          .pitch = {256, 256},
                          .offset = {0, 8192},
                          .buffer = de_rg_import_buffer(vf->graph, d->buffer, DE_ACCESS_NONE)};
    de_rg_pass *p =
        de_rg_add_pass(vf->graph, "gtao.capture.resolved", DE_QUEUE_GRAPHICS, read_scene, r);
    de_rg_pass_side_effect(p);
    de_rg_read(p, vc->depth, DE_ACCESS_COPY_SRC);
    de_rg_read(p, lc->target, DE_ACCESS_COPY_SRC);
    de_rg_write(p, r->buffer, DE_ACCESS_COPY_DST);
    p = de_rg_add_pass(vf->graph, "gtao.capture.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_pass_side_effect(p);
    de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
    de_rg_write(p, lc->target, DE_ACCESS_SHADER_WRITE);
}
static void renderer_registration_captures(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    de_type_registry *types = NULL;
    de_world *ecs = NULL;
    de_renderer *r = NULL;
    ASSERT_EQ(de_type_registry_create(&f->alloc, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = types}, &ecs),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .types = types,
                                                     .allocator = &f->alloc,
                                                     .frames_in_flight = 2},
                                 &r),
              DE_OK);
    de_renderer_attach(r, ecs);
    cvar_set("r.pipelined", "0");
    cvar_set("r.ambient_fallback", "1");
    cvar_set("r.raster_path", "compute");
    cvar_set("r.cull_disable", "0");
    cvar_set("r.raster_tri_cull", "1");
    capture_diag diag = {
        .renderer = r,
        .buffer = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = 16384,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .usage = DE_USAGE_COPY_DST})};
    ASSERT_TRUE(diag.buffer.id);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, 10, "gtao.capture.proof",
                                      capture_diagnostic_stage, &diag),
              DE_OK);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    settings.ao = DE_QUALITY_MEDIUM;
    settings.shadows = DE_QUALITY_OFF;
    settings.gi = DE_QUALITY_OFF;
    settings.reflections = DE_QUALITY_OFF;
    settings.sss = DE_QUALITY_OFF;
    settings.upscaler = DE_UPSCALE_NONE;
    settings.render_scale = .125f;
    de_renderer_set_settings(r, &settings);
    de__probe_capture capture = {0};
    ASSERT_EQ(de__probe_capture_init(&capture, r, 32), DE_OK);
    de_render_settings capture_settings = settings;
    capture_settings.ao = DE_QUALITY_ULTRA;
    capture_settings.render_scale = 1;
    for (u32 i = 0; i < 6; ++i)
        de_view_set_settings(r, capture.views[i], &capture_settings);
    u32 mat = material_add(.6f, .7f, 0, 0), mesh = lx_mesh_plane(f, 1, 1, 8);
    g_mat[mat - 1].standard.double_sided = true;
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    de__gpu_scene_destroy(r->gpu_scene);
    r->gpu_scene = NULL;
    ASSERT_EQ(
        de__gpu_scene_create(&(de__gpu_scene_desc){.device = f->dev,
                                                   .allocator = &f->alloc,
                                                   .resolver = {.mesh = fake_mesh,
                                                                .material = fake_material,
                                                                .texture_bindless = fake_texture},
                                                   .staging_bytes = 16u << 20,
                                                   .views_in_view_graphs = true,
                                                   .initial_capacity[DE__GS_VIEWS] = DE__VIEW_MAX},
                             &r->gpu_scene),
        DE_OK);
    de__rw_view camera = {
        .entity = 999,
        .camera = {.near_z = .1f, .fov_y = 1.04719755f, .view = de_renderer_main_view(r)},
        .offset = {0, 0, 3},
        .rotation = {0, 0, 0, 1},
        .transform = at(0, 0, 3)};
    f->w.f.views = &camera;
    f->w.f.view_count = 1;
    f->w.f.main_view = 0;
    u8 captured[2][256 * 32];
    for (u32 frame = 0; frame < 2; ++frame) {
        if (frame) {
            settings.ao = DE_QUALITY_OFF;
            de_renderer_set_settings(r, &settings);
            capture_settings.ao = DE_QUALITY_OFF;
            for (u32 i = 0; i < 6; ++i)
                de_view_set_settings(r, capture.views[i], &capture_settings);
        }
        de__probe_capture_at_position(&capture, (de_dvec3){0, 0, 2}, (de_dvec3){0}, 1u << 5);
        world_begin(&f->w);
        r->render_world->last = &f->w.f;
        r->render_world->current = &f->w.f;
        de__renderer_kick_prepare(r);
        ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
        de_gpu_wait_idle(f->dev);
        de__gtao_info info;
        ASSERT_TRUE(de__gtao_get(r->gtao, de_renderer_main_view(r), &info));
        EXPECT_EQ(info.dispatches, frame ? 0u : 4u);
        if (!frame) {
            EXPECT_EQ(info.slices, 2u);
            EXPECT_EQ(info.steps, 8u);
            EXPECT_TRUE(info.output.id);
        }
        for (u32 i = 0; i < 6; ++i)
            EXPECT_FALSE(de__gtao_get(r->gtao, capture.views[i], &info));
        de__view_info captured_view = {0};
        ASSERT_TRUE(de__view_info_get(r, capture.views[5], &captured_view));
        EXPECT_EQ(captured_view.executed, frame + 1);
        const u8 *debug = de_gpu_buffer_map(f->dev, diag.buffer);
        u32 covered = 0;
        double lit = 0;
        for (u32 y = 0; y < 32; ++y)
            for (u32 x = 0; x < 32; ++x) {
                float z;
                memcpy(&z, debug + y * 256 + x * 4, 4);
                covered += z > 0;
                u16 value;
                memcpy(&value, debug + 8192 + y * 256 + x * 8, 2);
                lit += half_to_f32(value);
            }
        printf("capture resolved pixels %u HDR red %.9f ambient %.3f\n", covered, lit,
               de_cvar_float(de_cvar_find("r.ambient_fallback")));
        EXPECT_GT(covered, 20u);
        EXPECT_TRUE(lit > 1);
        de_gpu_buffer rb =
            de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = sizeof captured[0],
                                                               .memory = DE_MEM_READBACK,
                                                               .usage = DE_USAGE_COPY_DST});
        ASSERT_TRUE(rb.id);
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "gtao.capture.compare");
        ASSERT_TRUE(cmd);
        de_gpu_barrier barrier = {.texture = capture.targets[5],
                                  .before = DE_ACCESS_SHADER_READ,
                                  .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, 256, capture.targets[5], 0, 0);
        barrier.before = DE_ACCESS_COPY_SRC;
        barrier.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                      &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
                  DE_OK);
        de_gpu_wait_idle(f->dev);
        memcpy(captured[frame], de_gpu_buffer_map(f->dev, rb), sizeof captured[0]);
        de_gpu_buffer_destroy(f->dev, rb);
    }
    EXPECT_EQ(memcmp(captured[0], captured[1], sizeof captured[0]), 0);
    double energy = 0;
    for (u32 i = 0; i < 32 * 32; ++i) {
        u16 color[4];
        memcpy(color, captured[0] + i * 8, 8);
        energy += half_to_f32(color[0]) + half_to_f32(color[1]) + half_to_f32(color[2]);
    }
    printf("capture radiance energy %.9f\n", energy);
    EXPECT_TRUE(energy > 1);
    printf("registered renderer: Medium4dispatches/OFF0; six capture views never own AO, Ultra/OFF "
           "physical capture bitwise identical\n");
    de__probe_capture_destroy(&capture);
    de__view_unregister_stage(r, capture_diagnostic_stage, &diag);
    de_gpu_buffer_destroy(f->dev, diag.buffer);
    de_renderer_destroy(r);
    de_world_destroy(ecs);
    de_type_registry_destroy(types);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
#define RENDERER_TEST(tag, backend)                                                                \
    TEST_F(gtao, renderer_registration_captures_##tag, lx) {                                       \
        renderer_registration_captures(f, backend);                                                \
    }
RENDERER_TEST(metal, DE_GPU_METAL)
RENDERER_TEST(vulkan, DE_GPU_VULKAN)
RENDERER_TEST(d3d12, DE_GPU_D3D12)
