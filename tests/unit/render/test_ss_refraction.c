#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include <dengine/de_reflect.h>
#include "gpu/gpu_backend.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/forward/scene_color_pyramid.h"
#include "render/passes/forward/forward.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"
#include "render/materials/brdf_luts.h"
#include "render/lighting/lighting.h"
#include "render/passes/vis/gbuffer.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_material.h"
#include "render/materials/material_models.h"
#include "render/loaders/render_assets.h"
#include "render/passes/sss/sss.h"
#include "render_d3d12_test.h"
#include "flip.h"
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

/* Frozen before GPU measurements; see the independent prototype /tmp/deng342-sphere-contract.c.
 * Physical ray/sphere intersections and both Snell interfaces, with a texture-backed checker.
 * ROI depends only on the physical ray: one pixel inside silhouette and background viewport. */
#define SPHERE_W         193u
#define SPHERE_H         145u
#define SPHERE_FLIP_MEAN .02
#define SPHERE_FLIP_P99  .10
#define PI64             3.14159265358979323846
typedef struct ray_v {
    double x, y, z;
} ray_v;
static ray_v ra(ray_v a, ray_v b) {
    return (ray_v){a.x + b.x, a.y + b.y, a.z + b.z};
}
static ray_v rm(ray_v a, double b) {
    return (ray_v){a.x * b, a.y * b, a.z * b};
}
static double rd(ray_v a, ray_v b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static ray_v rn(ray_v a) {
    return rm(a, 1 / sqrt(rd(a, a)));
}
static ray_v rbend(ray_v i, ray_v n, double ratio) {
    double c = -rd(i, n), k = 1 - ratio * ratio * (1 - c * c);
    ASSERT_TRUE(k >= 0);
    return ra(rm(i, ratio), rm(n, ratio * c - sqrt(k)));
}
static ray_v plane_hit(ray_v p, ray_v d) {
    return ra(p, rm(d, (-3 - p.z) / d.z));
}
static double ray_tan(void) {
    return tan(55 * PI64 / 360);
}
static ray_v camera_ray(u32 x, u32 y) {
    return rn(
        (ray_v){(2 * ((double)x + .5) / SPHERE_W - 1) * (double)SPHERE_W / SPHERE_H * ray_tan(),
                (1 - 2 * ((double)y + .5) / SPHERE_H) * ray_tan(), -1});
}
static bool sphere_hit(ray_v i, ray_v *p, ray_v *n, ray_v *out, double *distance) {
    ray_v eye = {0, 0, 3};
    double radius = .75, b = rd(eye, i), d = b * b - rd(eye, eye) + radius * radius;
    if (d < 0)
        return false;
    *p = ra(eye, rm(i, -b - sqrt(d)));
    *n = rm(*p, 1 / radius);
    ray_v inside = rbend(i, *n, 1 / 1.5);
    *distance = -2 * rd(*p, inside);
    ray_v exit = ra(*p, rm(inside, *distance)),
          direction = rbend(inside, rm(exit, -1 / radius), 1.5);
    *out = plane_hit(exit, direction);
    return true;
}
static void plane_uv(ray_v p, double *u, double *v) {
    *u = .5 + .5 * p.x / (6 * ray_tan() * (double)SPHERE_W / SPHERE_H);
    *v = .5 - .5 * p.y / (6 * ray_tan());
}
static bool sphere_roi(u32 x, u32 y, ray_v hit) {
    double u, v;
    plane_uv(hit, &u, &v);
    double radius = .75 / sqrt(9 - .75 * .75) / ray_tan() * SPHERE_H * .5;
    return hypot((double)x + .5 - SPHERE_W * .5, (double)y + .5 - SPHERE_H * .5) <= radius - 1 &&
           u >= 1.0 / SPHERE_W && u <= 1 - 1.0 / SPHERE_W && v >= 1.0 / SPHERE_H &&
           v <= 1 - 1.0 / SPHERE_H;
}
static float checker(ray_v p) {
    int a = (int)floor((p.x + .07) / .3), b = (int)floor((p.y + .11) / .3);
    return half_to_f32(((a + b) & 1) ? 0x3a66u : 0x2e66u);
}
static float image_sample(const float *image, double u, double v) {
    double x = u * SPHERE_W - .5, y = v * SPHERE_H - .5;
    int ix = (int)floor(x), iy = (int)floor(y);
    double f = x - floor(x), g = y - floor(y), s = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int px = ix + i, py = iy + j;
            if (px < 0)
                px = 0;
            if (py < 0)
                py = 0;
            if (px >= (int)SPHERE_W)
                px = SPHERE_W - 1;
            if (py >= (int)SPHERE_H)
                py = SPHERE_H - 1;
            s += image[py * SPHERE_W + px] * (i ? f : 1 - f) * (j ? g : 1 - g);
        }
    return (float)s;
}
static u8 srgb_byte(float linear) {
    double c = fmin(fmax(linear, 0), 1);
    double s = c <= .0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - .055;
    return (u8)lround(s * 255);
}
static void matrix_rows(de__gs_float4 out[4], const de_mat4 *m) {
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            out[r].v[c] = m->m[c * 4 + r];
}
static de_gpu_buffer upload_buffer(lx *f, const void *data, u64 bytes, u32 stride) {
    de_gpu_buffer b = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){
            .size = bytes, .stride = stride, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_STORAGE});
    ASSERT_TRUE(b.id);
    void *p = de_gpu_buffer_map(f->dev, b);
    ASSERT_TRUE(p);
    memcpy(p, data, (size_t)bytes);
    return b;
}
static de_gpu_texture upload_image_usage(lx *f, u32 w, u32 h, de_gpu_format format,
                                         const void *data, u32 pitch, u32 usage) {
    de_gpu_texture texture = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){
                    .width = w, .height = h, .format = format, .usage = usage | DE_USAGE_COPY_DST});
    ASSERT_TRUE(texture.id);
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){
                    .size = (u64)pitch * h, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_COPY_SRC});
    ASSERT_TRUE(upload.id);
    memcpy(de_gpu_buffer_map(f->dev, upload), data, (size_t)pitch * h);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "refraction.fixture.upload");
    ASSERT_TRUE(cmd);
    de_gpu_barrier barrier = {.texture = texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, texture, 0, 0, upload, 0, pitch);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = usage == DE_USAGE_COPY_SRC ? DE_ACCESS_COPY_SRC : DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, upload);
    return texture;
}
static de_gpu_texture upload_image(lx *f, u32 w, u32 h, de_gpu_format format, const void *data,
                                   u32 pitch) {
    return upload_image_usage(f, w, h, format, data, pitch, DE_USAGE_COPY_SRC);
}
typedef struct reference_environment {
    de_gpu_texture texture;
    de_gpu_buffer records, list;
} reference_environment;
typedef struct refraction_query {
    f32 position_thickness[4], view_ior[4], normal_roughness[4], geometry_curvature[4],
        attenuation[4];
} refraction_query;
typedef struct refraction_params {
    u32 scene, slot, view, entry, queries, output, count, reserved;
    u32 color, depth, width, height, depth_pitch, levels, reserved0, reserved1;
} refraction_params;
DE_STATIC_ASSERT(sizeof(refraction_params) == 64, "RefractionQueryParams is 64 bytes");
typedef struct reference_pass {
    lx *f;
    de__vis_kernel *kernel;
    refraction_params pc;
    de_rg_resource color, depth, output, readback;
} reference_pass;
static void reference_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    reference_pass *p = user;
    refraction_params pc = p->pc;
    pc.color = de_gpu_buffer_bindless(p->f->dev, de_rg_buffer(pass, p->color));
    pc.depth = de_gpu_buffer_bindless(p->f->dev, de_rg_buffer(pass, p->depth));
    pc.output = de_gpu_buffer_bindless(p->f->dev, de_rg_buffer(pass, p->output));
    de_gpu_cmd_set_pipeline(cmd, p->kernel->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.count + 63) / 64, 1, 1);
}
static void reference_readback(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    reference_pass *p = user;
    de_gpu_cmd_copy_buffer(cmd, de_rg_buffer(pass, p->readback), 0, de_rg_buffer(pass, p->output),
                           0, (u64)p->pc.count * 64);
}
static void save_image(const char *backend, const char *kind, const u8 *pixels) {
    char path[128];
    snprintf(path, sizeof path, "deng342-sphere-%s-%s.ppm", backend, kind);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f);
    fprintf(f, "P6\n%u %u\n255\n", SPHERE_W, SPHERE_H);
    for (u32 i = 0; i < SPHERE_W * SPHERE_H; ++i)
        fwrite(pixels + i * 4, 1, 3, f);
    fclose(f);
}
static float *run_reference(lx *f, de_gpu_texture color, de_gpu_texture depth, u32 width,
                            u32 height, const de__gs_view *constants, const de__lighting_view *lv,
                            const refraction_query *queries, u32 count,
                            const reference_environment *environment) {
    de_gpu_buffer scene = upload_buffer(f, constants, sizeof *constants, sizeof *constants),
                  lighting = upload_buffer(f, lv, sizeof *lv, sizeof *lv),
                  query = upload_buffer(f, queries, count * sizeof *queries, sizeof *queries);
    de_gpu_buffer readback =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)count * 64,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(readback.id);
    de__scene_color_pyramid *pyramid = NULL;
    ASSERT_EQ(de__scene_color_create(f->vis, &pyramid), DE_OK);
    const char *entry = "cs_ss_refraction_reference";
    const u32 group[] = {64, 1, 1};
    de__engine_shader_code code;
    de__vis_kernel kernel;
    ASSERT_EQ(de__vis_shader_load(f->vis, "ss_refraction", &entry, 1, &code), DE_OK);
    ASSERT_EQ(
        de__vis_kernel_create(f->vis, &code, 0, entry, group, sizeof(refraction_params), &kernel),
        DE_OK);
    de__engine_shader_code_free(&code);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    de__scene_color_context context;
    ASSERT_EQ(de__scene_color_build(
                  pyramid, f->g, f->arena, de_rg_import_texture(f->g, color, DE_ACCESS_COPY_SRC),
                  de_rg_import_texture(f->g, depth, DE_ACCESS_COPY_SRC), width, height, &context),
              DE_OK);
    reference_pass *pd = de_arena_push_zero(f->arena, sizeof *pd, 16);
    ASSERT_TRUE(pd);
    *pd = (reference_pass){
        .f = f,
        .kernel = &kernel,
        .pc = {.scene = de_gpu_buffer_bindless(f->dev, scene),
               .view = de_gpu_buffer_bindless(f->dev, lighting),
               .queries = de_gpu_buffer_bindless(f->dev, query),
               .count = count,
               .width = width,
               .height = height,
               .depth_pitch = context.layout.depth_pitch,
               .levels = context.layout.count},
        .color = context.color,
        .depth = context.depth,
        .output = de_rg_create_buffer(
            f->g, &(de_gpu_buffer_desc){.size = (u64)count * 64,
                                        .stride = 16,
                                        .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC}),
        .readback = de_rg_import_buffer(f->g, readback, DE_ACCESS_NONE)};
    de_rg_pass *pass =
        de_rg_add_pass(f->g, "refraction.reference", DE_QUEUE_GRAPHICS, reference_execute, pd);
    de_rg_read(pass, context.color, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, context.depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, de_rg_import_buffer(f->g, scene, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_read(pass, de_rg_import_buffer(f->g, lighting, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_read(pass, de_rg_import_buffer(f->g, query, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    if (environment) {
        de_rg_read(pass, de_rg_import_texture(f->g, environment->texture, DE_ACCESS_SHADER_READ),
                   DE_ACCESS_SHADER_READ);
        if (environment->records.id)
            de_rg_read(pass, de_rg_import_buffer(f->g, environment->records, DE_ACCESS_SHADER_READ),
                       DE_ACCESS_SHADER_READ);
        if (environment->list.id)
            de_rg_read(pass, de_rg_import_buffer(f->g, environment->list, DE_ACCESS_SHADER_READ),
                       DE_ACCESS_SHADER_READ);
    }
    de_rg_write(pass, pd->output, DE_ACCESS_SHADER_WRITE);
    pass = de_rg_add_pass(f->g, "refraction.reference.readback", DE_QUEUE_GRAPHICS,
                          reference_readback, pd);
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, pd->output, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, pd->readback, DE_ACCESS_COPY_DST);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    u64 fence = ++f->frames;
    ASSERT_EQ(
        de__rg_execute(f->g, &(de__rg_execute_desc){.frame_fence = f->fence, .frame_value = fence}),
        DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, fence, FIRST_FRAME_WAIT_NS), DE_OK);
    const float *result = de_gpu_buffer_map(f->dev, readback);
    ASSERT_TRUE(result);
    float *copy = zalloc((size_t)count * 64);
    memcpy(copy, result, (size_t)count * 64);
    de__rg_reset(f->g);
    de__scene_color_destroy(pyramid);
    de__vis_kernel_destroy(f->vis, &kernel);
    de_gpu_buffer_destroy(f->dev, scene);
    de_gpu_buffer_destroy(f->dev, lighting);
    de_gpu_buffer_destroy(f->dev, query);
    de_gpu_buffer_destroy(f->dev, readback);
    return copy;
}
static void sphere_reference(lx *f, de_gpu_backend backend, const char *name) {
    lx_open(f, backend);
    de_arena_destroy(f->arena);
    ASSERT_EQ(
        de_arena_create(&(de_arena_desc){.reserve_bytes = (u64)16 << 20, .name = "refraction.test"},
                        &f->arena),
        DE_OK);
    const u32 count = SPHERE_W * SPHERE_H, pitch = (SPHERE_W * 8 + 255u) & ~255u,
              zpitch = (SPHERE_W * 4 + 255u) & ~255u;
    u8 *raw = zalloc((size_t)pitch * SPHERE_H), *zraw = zalloc((size_t)zpitch * SPHERE_H),
       *mask = zalloc(count), *reference = zalloc(count * 4), *actual = zalloc(count * 4);
    float *image = zalloc(count * sizeof *image), *errors = zalloc(count * sizeof *errors),
          *roi = zalloc(count * sizeof *roi);
    refraction_query *queries = zalloc(count * sizeof *queries);
    double *physical_x = zalloc(count * sizeof *physical_x);
    for (u32 y = 0; y < SPHERE_H; ++y)
        for (u32 x = 0; x < SPHERE_W; ++x) {
            u32 i = y * SPHERE_W + x;
            ray_v ray = camera_ray(x, y), p = plane_hit((ray_v){0, 0, 3}, ray), n = {0, 0, 1},
                  hit = p;
            double distance = 0;
            image[i] = checker(p);
            u16 h = image[i] > .5f ? 0x3a66u : 0x2e66u;
            u16 pixel[4] = {h, h, h, 0x3c00u};
            memcpy(raw + (size_t)y * pitch + x * 8, pixel, 8);
            float depth = .1f / 6;
            memcpy(zraw + (size_t)y * zpitch + x * 4, &depth, 4);
            bool sphere = sphere_hit(ray, &p, &n, &hit, &distance);
            if (!sphere)
                p = plane_hit((ray_v){0, 0, 3}, ray);
            queries[i] = (refraction_query){
                .position_thickness = {(f32)p.x, (f32)p.y, (f32)p.z, sphere ? 1.5f : 0},
                .view_ior = {(f32)-ray.x, (f32)-ray.y, (f32)-ray.z, sphere ? 1.5f : 1},
                .normal_roughness = {(f32)n.x, (f32)n.y, (f32)n.z, 0},
                .geometry_curvature = {(f32)n.x, (f32)n.y, (f32)n.z, sphere ? 1 / .75f : 0},
                .attenuation = {1, 1, 1, 1e30f}};
            mask[i] = (u8)(sphere && sphere_roi(x, y, hit));
            physical_x[i] = hit.x;
        }
    for (u32 y = 0; y < SPHERE_H; ++y)
        for (u32 x = 0; x < SPHERE_W; ++x) {
            u32 i = y * SPHERE_W + x;
            ray_v p, n, hit;
            double d;
            float c = image[i];
            double confidence = 1;
            if (sphere_hit(camera_ray(x, y), &p, &n, &hit, &d)) {
                double u, v;
                plane_uv(hit, &u, &v);
                c = image_sample(image, u, v);
                double edge = fmin(fmin(u, 1 - u) * SPHERE_W, fmin(v, 1 - v) * SPHERE_H);
                double t = fmin(1, fmax(0, edge / 2));
                confidence = t * t * (3 - 2 * t);
            }
            const double ambient[3] = {.17, .37, .71};
            for (u32 channel = 0; channel < 3; ++channel)
                reference[i * 4 + channel] =
                    srgb_byte((float)(ambient[channel] * (1 - confidence) + c * confidence));
            reference[i * 4 + 3] = 255;
        }
    de_gpu_texture color = upload_image(f, SPHERE_W, SPHERE_H, DE_FMT_RGBA16_FLOAT, raw, pitch),
                   depth = upload_image(f, SPHERE_W, SPHERE_H, DE_FMT_R32_FLOAT, zraw, zpitch);
    de_mat4 view = de_m4_look_at((de_vec3){0, 0, 3}, (de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}),
            proj = de_m4_perspective_rz((f32)(55 * PI64 / 180), (f32)SPHERE_W / SPHERE_H, .1f),
            vp = de_m4_mul(&proj, &view), inv = de_m4_inverse(&vp);
    de__gs_view constants = {0};
    matrix_rows(constants.view, &view);
    matrix_rows(constants.proj, &proj);
    matrix_rows(constants.view_proj, &vp);
    matrix_rows(constants.inv_view_proj, &inv);
    constants.camera_position = (de__gs_float4){.v = {0, 0, 3, 1}};
    de__lighting_view lv = {.pre_exposure = 1};
    lv.ibl.ambient[0] = .17f;
    lv.ibl.ambient[1] = .37f;
    lv.ibl.ambient[2] = .71f;
    float *result =
        run_reference(f, color, depth, SPHERE_W, SPHERE_H, &constants, &lv, queries, count, NULL);
    u32 covered = 0;
    double max_uv = 0, max_rgb = 0, sum_rgb = 0;
    for (u32 i = 0; i < count; ++i) {
        for (u32 c = 0; c < 3; ++c)
            actual[i * 4 + c] = srgb_byte(result[i * 16 + c]);
        actual[i * 4 + 3] = 255;
        if (mask[i]) {
            ray_v p, n, hit;
            double d, u, v;
            sphere_hit(camera_ray(i % SPHERE_W, i / SPHERE_W), &p, &n, &hit, &d);
            plane_uv(hit, &u, &v);
            double edge = fmin(fmin(u, 1 - u) * SPHERE_W, fmin(v, 1 - v) * SPHERE_H),
                   t = fmin(1, fmax(0, edge / 2));
            double confidence = t * t * (3 - 2 * t);
            max_uv = fmax(max_uv, fmax(fabs(result[i * 16 + 4] - u), fabs(result[i * 16 + 5] - v)));
            const double ambient[3] = {.17, .37, .71};
            double sample = image_sample(image, u, v);
            for (u32 c = 0; c < 3; ++c) {
                double error = fabs(result[i * 16 + c] -
                                    (sample * confidence + ambient[c] * (1 - confidence)));
                max_rgb = fmax(max_rgb, error);
                sum_rgb += error;
            }
            EXPECT_FLOAT_EQ(result[i * 16 + 3], t * t * (3 - 2 * t), 1e-4);
            EXPECT_FLOAT_EQ(result[i * 16 + 7], 0.f, 0);
            EXPECT_TRUE(result[i * 16 + 11] <= 8);
            covered++;
        }
    }
    printf("physical ROI UV max %.9g linear RGB max %.9g MAE %.9g\n", max_uv, max_rgb,
           sum_rgb / (covered * 3));
    de_flip_image ref = {reference, SPHERE_W, SPHERE_H, 4, 0},
                  gpu = {actual, SPHERE_W, SPHERE_H, 4, 0};
    ASSERT_TRUE(de_flip_ldr(&ref, &gpu, 0, errors));
    u32 nr = 0;
    for (u32 i = 0; i < count; ++i)
        if (mask[i])
            roi[nr++] = errors[i];
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(roi, nr, &stats));
    printf("physical sphere helper %s ROI%u FLIP mean%.9f p99%.9f max%.9f (fixed .02/.10)\n", name,
           nr, stats.mean, stats.p99, stats.max);
    EXPECT_EQ(covered, 3817u);
    EXPECT_TRUE(stats.mean <= SPHERE_FLIP_MEAN);
    EXPECT_TRUE(stats.p99 <= SPHERE_FLIP_P99);
    const u32 left = (SPHERE_H / 2) * SPHERE_W + SPHERE_W / 2 - 2, right = left + 4;
    double spacing = 8.0 / SPHERE_W * (double)SPHERE_W / SPHERE_H * ray_tan();
    double cpu_slope = (physical_x[right] - physical_x[left]) / spacing;
    double gpu_left = (result[left * 16 + 4] * 2 - 1) * 6 * ray_tan() * (double)SPHERE_W / SPHERE_H,
           gpu_right =
               (result[right * 16 + 4] * 2 - 1) * 6 * ray_tan() * (double)SPHERE_W / SPHERE_H;
    double gpu_slope = (gpu_right - gpu_left) / spacing, cpu_magnification = 6 / fabs(cpu_slope),
           gpu_magnification = 6 / fabs(gpu_slope);
    printf("measured mapping CPU slope%.8f GPU%.8f magnification CPU%.8f GPU%.8f\n", cpu_slope,
           gpu_slope, cpu_magnification, gpu_magnification);
    EXPECT_TRUE(cpu_slope < 0 && gpu_slope < 0);
    EXPECT_TRUE(cpu_magnification > 1.5 && gpu_magnification > 1.5);
    EXPECT_TRUE(fabs(gpu_magnification / cpu_magnification - 1) < .02);
    save_image(name, "cpu", reference);
    save_image(name, "gpu", actual);
    EXPECT_EQ(validation_errors(f->dev), 0u);
    de_gpu_texture_destroy(f->dev, color);
    de_gpu_texture_destroy(f->dev, depth);
    free(result);
    free(raw);
    free(zraw);
    free(mask);
    free(reference);
    free(actual);
    free(image);
    free(errors);
    free(roi);
    free(queries);
    free(physical_x);
}
TEST_F(refraction, sphere_helper_metal, lx) {
    sphere_reference(f, DE_GPU_METAL, "metal");
}
TEST_F(refraction, sphere_helper_vulkan, lx) {
    sphere_reference(f, DE_GPU_VULKAN, "vulkan");
}
TEST_F(refraction, sphere_helper_d3d12, lx) {
    sphere_reference(f, DE_GPU_D3D12, "d3d12");
}

static de__gs_view reference_camera(u32 w, u32 h, bool ortho) {
    de_mat4 view = de_m4_look_at((de_vec3){0, 0, 3}, (de_vec3){0, 0, 0}, (de_vec3){0, 1, 0});
    de_mat4 proj =
        ortho ? de_m4_ortho_rz(-2, 2, -2, 2, .1f, 20) : de_m4_perspective_rz(1, (float)w / h, .1f);
    de_mat4 vp = de_m4_mul(&proj, &view), inv = de_m4_inverse(&vp);
    de__gs_view sc = {0};
    matrix_rows(sc.view, &view);
    matrix_rows(sc.proj, &proj);
    matrix_rows(sc.view_proj, &vp);
    matrix_rows(sc.inv_view_proj, &inv);
    sc.camera_position = (de__gs_float4){.v = {0, 0, 3, 1}};
    return sc;
}
static void optics_reference(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    enum { w = 17, h = 11, n = w * h, pitch = 256 };
    u8 raw[pitch * h], depth_raw[pitch * h];
    memset(raw, 0, sizeof raw);
    memset(depth_raw, 0, sizeof depth_raw);
    refraction_query q[n];
    memset(q, 0, sizeof q);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 pixel[4] = {(u16)(0x3000 + x * 16), (u16)(0x3400 + y * 16), 0x3800, 0x3c00};
            memcpy(raw + y * pitch + x * 8, pixel, 8);
            float z = .1f / 6;
            memcpy(depth_raw + y * pitch + x * 4, &z, 4);
            u32 i = y * w + x;
            double angle = (i % 4 == 0 ? PI64 / 2 : (double)(i % 89) * PI64 / 180);
            q[i] = (refraction_query){.position_thickness = {0, 0, 0, 0},
                                      .view_ior = {(float)sin(angle), 0, (float)cos(angle), 1},
                                      .normal_roughness = {0, 0, 1, 0},
                                      .geometry_curvature = {0, 0, 1, 0},
                                      .attenuation = {.2f, .5f, .8f, 2}};
            if (i % 4 == 0)
                q[i].view_ior[2] = 0; // exact grazing equal-index interface
        }
    de_gpu_texture color = upload_image(f, w, h, DE_FMT_RGBA16_FLOAT, raw, pitch),
                   depth = upload_image(f, w, h, DE_FMT_R32_FLOAT, depth_raw, pitch);
    de__gs_view sc = reference_camera(w, h, false);
    de__lighting_view lv = {.pre_exposure = 2};
    lv.ibl.ambient[0] = .3f;
    lv.ibl.ambient[1] = .6f;
    lv.ibl.ambient[2] = .9f;
    float *out = run_reference(f, color, depth, w, h, &sc, &lv, q, n, NULL);
    double max_identity = 0;
    for (u32 i = 0; i < n; ++i) {
        u16 pixel[4];
        memcpy(pixel, raw + (i / w) * pitch + (i % w) * 8, 8);
        for (u32 c = 0; c < 3; ++c)
            max_identity = fmax(max_identity, fabs(out[i * 16 + c] - half_to_f32(pixel[c])));
        EXPECT_FLOAT_EQ(out[i * 16 + 7], 0, 0);
        EXPECT_FLOAT_EQ(out[i * 16 + 3], 1, 0);
        EXPECT_FLOAT_EQ(out[i * 16 + 11], 0, 0);
    }
    printf("IOR1 %u normal/oblique/exact-grazing mip0 queries max %.9g (fixed .001)\n", n,
           max_identity);
    EXPECT_TRUE(max_identity <= .001);
    free(out);
    for (u32 i = 0; i < n; ++i) {
        double a = (double)(i % 80) * PI64 / 180;
        q[i].position_thickness[3] = .65f;
        q[i].view_ior[0] = (float)sin(a);
        q[i].view_ior[2] = (float)cos(a);
        q[i].view_ior[3] = 1.5f;
    }
    out = run_reference(f, color, depth, w, h, &sc, &lv, q, n, NULL);
    double max_beer = 0, max_direction = 0;
    for (u32 i = 0; i < n; ++i) {
        double a = (double)(i % 80) * PI64 / 180, inside_cos = sqrt(1 - pow(sin(a) / 1.5, 2)),
               distance = (double).65f / inside_cos;
        EXPECT_FLOAT_EQ(out[i * 16 + 6], distance, 2e-6);
        EXPECT_FLOAT_EQ(out[i * 16 + 7], 0, 0);
        max_direction = fmax(max_direction, fabs(out[i * 16 + 8] + sin(a)));
        max_direction = fmax(max_direction, fabs(out[i * 16 + 10] + cos(a)));
        for (u32 c = 0; c < 3; ++c) {
            double expected = exp(log((double)q[i].attenuation[c]) * distance / 2);
            max_beer = fmax(max_beer, fabs(out[i * 16 + 12 + c] - expected));
        }
        EXPECT_TRUE(out[i * 16 + 11] <= 8);
    }
    printf("parallel slab %u queries Beer max %.9g direction max %.9g\n", n, max_beer,
           max_direction);
    EXPECT_TRUE(max_beer < 2e-6);
    EXPECT_TRUE(max_direction < 2e-6);
    free(out);
    // Perspective and orthographic behind-camera points must use nonblack environment fallback.
    for (u32 mode = 0; mode < 2; ++mode) {
        sc = reference_camera(w, h, mode != 0);
        for (u32 i = 0; i < n; ++i) {
            q[i].position_thickness[2] = 4;
            q[i].position_thickness[3] = 0;
            q[i].view_ior[3] = 1;
            q[i].view_ior[0] = 0;
            q[i].view_ior[2] = 1;
        }
        out = run_reference(f, color, depth, w, h, &sc, &lv, q, n, NULL);
        for (u32 i = 0; i < n; ++i) {
            EXPECT_FLOAT_EQ(out[i * 16 + 3], 0, 0);
            for (u32 c = 0; c < 3; ++c)
                EXPECT_FLOAT_EQ(out[i * 16 + c], lv.ibl.ambient[c] * 2, 1e-6);
        }
        free(out);
    }
    EXPECT_EQ(validation_errors(f->dev), 0u);
    de_gpu_texture_destroy(f->dev, color);
    de_gpu_texture_destroy(f->dev, depth);
}
TEST_F(refraction, identity_beer_orthographic_metal, lx) {
    optics_reference(f, DE_GPU_METAL);
}
TEST_F(refraction, identity_beer_orthographic_vulkan, lx) {
    optics_reference(f, DE_GPU_VULKAN);
}
TEST_F(refraction, identity_beer_orthographic_d3d12, lx) {
    optics_reference(f, DE_GPU_D3D12);
}

static u16 reference_half(double v) {
    if (v == 0)
        return 0;
    int exponent;
    double fraction = frexp(v, &exponent);
    long mantissa = lrint((fraction * 2 - 1) * 1024);
    if (mantissa == 1024) {
        mantissa = 0;
        ++exponent;
    }
    return (u16)(((exponent + 14) << 10) | mantissa);
}
typedef struct pyramid_readback {
    de_rg_resource color, depth;
    de_gpu_buffer target;
    u64 bytes, depth_offset, depth_bytes;
} pyramid_readback;
static void pyramid_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    pyramid_readback *p = user;
    de_gpu_cmd_copy_buffer(cmd, p->target, 0, de_rg_buffer(pass, p->color), 0, p->bytes);
    de_gpu_cmd_copy_buffer(cmd, p->target, p->depth_offset, de_rg_buffer(pass, p->depth), 0,
                           p->depth_bytes);
}
static double reference_level_sample(const u8 *bytes, de__scene_color_level level, double x,
                                     double y, u32 channel) {
    int ix = (int)floor(x), iy = (int)floor(y);
    double fx = x - floor(x), fy = y - floor(y), sum = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int px = ix + i, py = iy + j;
            px = px < 0 ? 0 : px >= (int)level.width ? (int)level.width - 1 : px;
            py = py < 0 ? 0 : py >= (int)level.height ? (int)level.height - 1 : py;
            u16 h;
            memcpy(&h, bytes + level.offset + (u32)py * level.pitch + (u32)px * 8 + channel * 2, 2);
            sum += half_to_f32(h) * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        }
    return sum;
}
static void consumer_reference(lx *f, de_gpu_texture color, const u8 *expected,
                               de__scene_color_layout layout) {
    enum { w = 19, h = 13, pitch = 256, count = 35 };
    u8 depth_raw[pitch * h];
    memset(depth_raw, 0, sizeof depth_raw);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            float z = .1f / 6;
            memcpy(depth_raw + y * pitch + x * 4, &z, 4);
        }
    de_gpu_texture depth = upload_image(f, w, h, DE_FMT_R32_FLOAT, depth_raw, pitch);
    const float roughness[5] = {0, .13f, .375f, .73f, 1};
    double max_error = 0, max_uv = 0;
    for (u32 ortho = 0; ortho < 2; ++ortho)
        for (u32 volume = 0; volume < 2; ++volume) {
            de__gs_view sc = reference_camera(w, h, ortho != 0);
            de__lighting_view lv = {.pre_exposure = 1};
            refraction_query queries[count];
            for (u32 i = 0; i < count; ++i) {
                double u = (3.5 + (i % 7) * 2) / w, v = (3.5 + (i / 7)) / h;
                double sx = ortho ? 2 : 3 * tan(.5) * (double)w / h, sy = ortho ? 2 : 3 * tan(.5);
                queries[i] = (refraction_query){.position_thickness = {(float)((u * 2 - 1) * sx),
                                                                       (float)((1 - v * 2) * sy), 0,
                                                                       volume ? .4f : 0},
                                                .view_ior = {0, 0, 1, 1.5f},
                                                .normal_roughness = {0, 0, 1, roughness[i % 5]},
                                                .geometry_curvature = {0, 0, 1, 0},
                                                .attenuation = {1, 1, 1, 1e30f}};
            }
            de_gpu_texture selected_depth = depth;
            if (ortho) {
                for (u32 y = 0; y < h; ++y)
                    for (u32 x = 0; x < w; ++x) {
                        float z = (20.f - 6.f) / (20.f - .1f);
                        memcpy(depth_raw + y * pitch + x * 4, &z, 4);
                    }
                selected_depth = upload_image(f, w, h, DE_FMT_R32_FLOAT, depth_raw, pitch);
            }
            float *out =
                run_reference(f, color, selected_depth, w, h, &sc, &lv, queries, count, NULL);
            for (u32 i = 0; i < count; ++i) {
                double u = (3.5 + (i % 7) * 2) / w, v = (3.5 + (i / 7)) / h;
                if (volume && !ortho) {
                    u = .5 + (u - .5) * .5;
                    v = .5 + (v - .5) * .5;
                }
                double r = roughness[i % 5],
                       pixels_per_metre = (double)h * .5 / (ortho ? 2 : tan(.5) * 6),
                       travel = volume ? 3 - (double).4f : 0;
                double lod = fmin(layout.count - 1,
                                  r * (layout.count - 1) + log2(1 + r * travel * pixels_per_metre));
                u32 lo = (u32)floor(lod), hi = lo + 1 < layout.count ? lo + 1 : lo;
                for (u32 c = 0; c < 3; ++c) {
                    de__scene_color_level a = layout.level[lo], b = layout.level[hi];
                    double low = reference_level_sample(expected, a, u * a.width - .5,
                                                        v * a.height - .5, c),
                           high = reference_level_sample(expected, b, u * b.width - .5,
                                                         v * b.height - .5, c),
                           want = low + (high - low) * (lod - lo);
                    max_error = fmax(max_error, fabs(out[i * 16 + c] - want));
                }
                max_uv = fmax(max_uv, fmax(fabs(out[i * 16 + 4] - u), fabs(out[i * 16 + 5] - v)));
                EXPECT_FLOAT_EQ(out[i * 16 + 3], 1, 1e-6);
                EXPECT_TRUE(out[i * 16 + 11] <= 8);
            }
            free(out);
            if (ortho)
                de_gpu_texture_destroy(f->dev, selected_depth);
        }
    printf("consumer thin/volume perspective/ortho %u queries rough0/.13/.375/.73/1 RGBmax%.9g "
           "UVmax%.9g (.002 fixed)\n",
           count * 4, max_error, max_uv);
    EXPECT_TRUE(max_error <= .002);
    EXPECT_TRUE(max_uv < 2e-6);
    de_gpu_texture_destroy(f->dev, depth);
}
static de_arena *failure_arena_begin(lx *f) {
    de__rg_reset(f->g);
    de_arena *original = f->arena, *bounded = NULL;
    ASSERT_EQ(de_arena_create(
                  &(de_arena_desc){.reserve_bytes = (u64)1 << 20, .name = "refraction.failure"},
                  &bounded),
              DE_OK);
    f->arena = bounded;
    return original;
}
static void failure_arena_end(lx *f, de_arena *original) {
    de__rg_reset(f->g);
    de_arena_destroy(f->arena);
    f->arena = original;
}
static void pyramid_reference(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    enum { w = 19, h = 13, pitch = 256 };
    de__scene_color_layout layout;
    ASSERT_EQ(de__scene_color_layout_make(w, h, &layout), DE_OK);
    EXPECT_EQ(layout.count, 5u);
    EXPECT_EQ(layout.level[1].width, 9u);
    EXPECT_EQ(layout.level[1].height, 6u);
    de__scene_color_pyramid *p = NULL;
    ASSERT_EQ(de__scene_color_create(f->vis, &p), DE_OK);
    u64 depth_offset = (layout.bytes + 511) & ~(u64)511;
    de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = depth_offset + layout.depth_bytes,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK});
    ASSERT_TRUE(rb.id);
    double max_error = 0;
    u32 checked = 0;
    for (u32 frame = 0; frame < 3; ++frame) {
        u8 raw[pitch * h], depth_raw[pitch * h];
        memset(raw, 0xcd, sizeof raw);
        memset(depth_raw, 0xef, sizeof depth_raw);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                u16 pixel[4];
                for (u32 c = 0; c < 4; ++c)
                    pixel[c] = reference_half(
                        (double)(1 + ((x * 7 + y * 11 + c * 13 + frame * 23) % 47)) / 32);
                memcpy(raw + y * pitch + x * 8, pixel, 8);
                float z = .02f + (float)(x + y + frame) / 1000;
                memcpy(depth_raw + y * pitch + x * 4, &z, 4);
            }
        u8 *expected = zalloc((size_t)layout.bytes);
        memcpy(expected, raw, sizeof raw);
        const double weights[5] = {1, 4, 6, 4, 1};
        for (u32 level = 1; level < layout.count; ++level) {
            de__scene_color_level a = layout.level[level - 1], b = layout.level[level];
            for (u32 y = 0; y < b.height; ++y)
                for (u32 x = 0; x < b.width; ++x)
                    for (u32 c = 0; c < 4; ++c) {
                        double sum = 0, cx = ((double)x + .5) * a.width / b.width - .5,
                               cy = ((double)y + .5) * a.height / b.height - .5;
                        for (int j = -2; j <= 2; ++j)
                            for (int i = -2; i <= 2; ++i)
                                sum += reference_level_sample(expected, a, cx + i, cy + j, c) *
                                       weights[i + 2] * weights[j + 2];
                        u16 value = reference_half(sum / 256);
                        memcpy(expected + b.offset + y * b.pitch + x * 8 + c * 2, &value, 2);
                    }
        }
        de_gpu_texture color = upload_image(f, w, h, DE_FMT_RGBA16_FLOAT, raw, pitch),
                       depth = upload_image(f, w, h, DE_FMT_R32_FLOAT, depth_raw, pitch);
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        de_rg_resource source = de_rg_import_texture(f->g, color, DE_ACCESS_COPY_SRC),
                       z = de_rg_import_texture(f->g, depth, DE_ACCESS_COPY_SRC);
        de_tracking_stats before = de_tracking_allocator_stats(&f->track);
        de__scene_color_context context;
        ASSERT_EQ(de__scene_color_build(p, f->g, f->arena, source, z, w, h, &context), DE_OK);
        de_tracking_stats after = de_tracking_allocator_stats(&f->track);
        if (frame > 0) {
            EXPECT_EQ(after.alloc_count, before.alloc_count);
            EXPECT_EQ(after.realloc_count, before.realloc_count);
        }
        EXPECT_EQ(f->g->pass_count, layout.count);
        pyramid_readback *pd = de_arena_push_zero(f->arena, sizeof *pd, 16);
        ASSERT_TRUE(pd);
        *pd = (pyramid_readback){context.color, context.depth, rb,
                                 layout.bytes,  depth_offset,  layout.depth_bytes};
        de_rg_pass *pass = de_rg_add_pass(f->g, "refraction.pyramid.readback", DE_QUEUE_GRAPHICS,
                                          pyramid_copy, pd);
        de_rg_pass_side_effect(pass);
        de_rg_read(pass, context.color, DE_ACCESS_COPY_SRC);
        de_rg_read(pass, context.depth, DE_ACCESS_COPY_SRC);
        de_rg_write(pass, de_rg_import_buffer(f->g, rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        ASSERT_EQ(de__rg_compile(f->g), DE_OK);
        u64 fence = ++f->frames;
        ASSERT_EQ(de__rg_execute(
                      f->g, &(de__rg_execute_desc){.frame_fence = f->fence, .frame_value = fence}),
                  DE_OK);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, fence, FIRST_FRAME_WAIT_NS), DE_OK);
        const u8 *got = de_gpu_buffer_map(f->dev, rb);
        ASSERT_TRUE(got);
        for (u32 y = 0; y < h; ++y) {
            EXPECT_MEMEQ(got + y * pitch, raw + y * pitch, w * 8);
            EXPECT_MEMEQ(got + depth_offset + y * pitch, depth_raw + y * pitch, w * 4);
        }
        for (u32 level = 1; level < layout.count; ++level) {
            de__scene_color_level b = layout.level[level];
            for (u32 y = 0; y < b.height; ++y)
                for (u32 x = 0; x < b.width; ++x)
                    for (u32 c = 0; c < 4; ++c) {
                        u16 actual, value;
                        u32 offset = b.offset + y * b.pitch + x * 8 + c * 2;
                        memcpy(&actual, got + offset, 2);
                        memcpy(&value, expected + offset, 2);
                        max_error = fmax(max_error, fabs(half_to_f32(actual) - half_to_f32(value)));
                        ++checked;
                    }
        }
        if (frame == 0)
            consumer_reference(f, color, expected, layout);
        de__rg_reset(f->g);
        de_gpu_texture_destroy(f->dev, color);
        de_gpu_texture_destroy(f->dev, depth);
        free(expected);
    }
    printf("odd pyramid %u Gaussian components, current-frame0/1/2, max %.9g (fixed .002)\n",
           checked, max_error);
    EXPECT_TRUE(max_error <= .002);
    // Build OOM is explicit; a discarded graph has no persistent ownership and retries cleanly.
    de_arena *original_arena = failure_arena_begin(f);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    de_arena_info stats = de_arena_get_info(f->arena);
    ASSERT_TRUE(de_arena_push(f->arena, stats.reserved - stats.used - 32, 1));
    de__scene_color_context failed;
    EXPECT_EQ(de__scene_color_build(p, f->g, f->arena, (de_rg_resource){1}, (de_rg_resource){2}, w,
                                    h, &failed),
              DE_ERR_OUT_OF_MEMORY);
    EXPECT_FALSE(failed.color.id);
    EXPECT_EQ(f->g->pass_count, 0u);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    de_error_clear();
    failure_arena_end(f, original_arena);
    de_gpu_buffer_destroy(f->dev, rb);
    de__scene_color_destroy(p);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
TEST_F(refraction, gaussian_pyramid_lifecycle_metal, lx) {
    pyramid_reference(f, DE_GPU_METAL);
}
TEST_F(refraction, gaussian_pyramid_lifecycle_vulkan, lx) {
    pyramid_reference(f, DE_GPU_VULKAN);
}
TEST_F(refraction, gaussian_pyramid_lifecycle_d3d12, lx) {
    pyramid_reference(f, DE_GPU_D3D12);
}

static u32 reference_face(ray_v d) {
    double x = fabs(d.x), y = fabs(d.y), z = fabs(d.z);
    return x >= y && x >= z ? (d.x >= 0 ? 0u : 1u)
           : y >= z         ? (d.y >= 0 ? 2u : 3u)
                            : (d.z >= 0 ? 4u : 5u);
}
static ray_v reference_exit_direction(const refraction_query *q, bool *tir) {
    ray_v v = {q->view_ior[0], q->view_ior[1], q->view_ior[2]},
          n = {q->normal_roughness[0], q->normal_roughness[1], q->normal_roughness[2]};
    ray_v i = rm(v, -1);
    *tir = false;
    if (q->view_ior[3] == 1)
        return i;
    bool inside = rd(v, n) < 0;
    if (inside)
        n = rm(n, -1);
    double eta = inside ? q->view_ior[3] : 1 / q->view_ior[3], c = -rd(i, n),
           disc = 1 - eta * eta * (1 - c * c);
    if (disc < 0) {
        *tir = true;
        return ra(i, rm(n, 2 * c));
    }
    ray_v out = ra(rm(i, eta), rm(n, eta * c - sqrt(disc)));
    if (!inside && q->position_thickness[3] > 0)
        out = i; // independent parallel exit interface
    return out;
}
static void fallback_reference(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    enum { w = 17, h = 11, pitch = 256, n = 12, face_size = 4 };
    u8 raw[pitch * h], zraw[pitch * h], strip[pitch * face_size * 6];
    memset(raw, 0, sizeof raw);
    memset(zraw, 0, sizeof zraw);
    memset(strip, 0, sizeof strip);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 pixel[4] = {0x3000, 0x3400, 0x3800, 0x3c00};
            memcpy(raw + y * pitch + x * 8, pixel, 8);
            float z = .1f / 6;
            memcpy(zraw + y * pitch + x * 4, &z, 4);
        }
    double colors[6][3];
    for (u32 face = 0; face < 6; ++face)
        for (u32 c = 0; c < 3; ++c)
            colors[face][c] = (double)(1 + ((face * 3 + c * 5) % 13)) / 16;
    for (u32 face = 0; face < 6; ++face)
        for (u32 y = 0; y < face_size; ++y)
            for (u32 x = 0; x < face_size; ++x) {
                u16 pixel[4];
                for (u32 c = 0; c < 3; ++c)
                    pixel[c] = reference_half(colors[face][c]);
                pixel[3] = 0x3c00;
                memcpy(strip + (face * face_size + y) * pitch + x * 8, pixel, 8);
            }
    de_gpu_texture color = upload_image(f, w, h, DE_FMT_RGBA16_FLOAT, raw, pitch),
                   depth = upload_image(f, w, h, DE_FMT_R32_FLOAT, zraw, pitch);
    reference_environment env = {.texture = upload_image_usage(f, face_size, face_size * 6,
                                                               DE_FMT_RGBA16_FLOAT, strip, pitch,
                                                               DE_USAGE_SAMPLED)};
    de_gpu_sampler sampler =
        de_gpu_sampler_create(f->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                             .mag = DE_FILTER_LINEAR,
                                                             .u = DE_ADDR_CLAMP,
                                                             .v = DE_ADDR_CLAMP});
    ASSERT_TRUE(sampler.id);
    de__gs_view sc = reference_camera(w, h, false);
    de__lighting_view lv = {.pre_exposure = 2.3f};
    lv.ibl.flags = DE__IBL_ENVIRONMENT;
    lv.ibl.strip = de_gpu_texture_bindless(f->dev, env.texture, 0, false);
    lv.ibl.sampler = de_gpu_sampler_bindless(f->dev, sampler);
    lv.ibl.face_size = face_size;
    lv.ibl.rotation[1] = 1;
    lv.ibl.sh[0][3] = 1.7f;
    refraction_query q[n];
    memset(q, 0, sizeof q);
    for (u32 i = 0; i < n; ++i) {
        ray_v view = rn((ray_v){(i % 3 == 0   ? .9
                                 : i % 3 == 1 ? -.45
                                              : .1),
                                (i % 4 == 0 ? .8 : .1), (i % 2 == 0 ? .4 : .9)});
        q[i] = (refraction_query){
            .position_thickness = {(i % 3 == 0 ? 29.f : 20.f), 0, (i % 4 == 0 ? 4.f : 0.f),
                                   i % 2 == 0 ? 0 : .65f},
            .view_ior = {(f32)view.x, (f32)view.y, (f32)view.z, i % 3 == 0 ? 1 : 1.5f},
            .normal_roughness = {0, 0, 1, .25f},
            .geometry_curvature = {0, 0, 1, 0},
            .attenuation = {1, 1, 1, 1e30f}};
    }
    // Mixed-lane camera-inside TIR, ordinary behind-camera and offscreen directional fallbacks.
    q[0].position_thickness[0] = 0;
    q[0].position_thickness[2] = 0;
    q[0].view_ior[0] = .9f;
    q[0].view_ior[1] = 0;
    q[0].view_ior[2] = -sqrtf(.19f);
    q[0].view_ior[3] = 1.5f;
    q[1].position_thickness[0] = 29;
    q[1].position_thickness[2] = 0;
    q[1].view_ior[0] = -.45f;
    q[1].view_ior[1] = 0;
    q[1].view_ior[2] = sqrtf(1 - .45f * .45f);
    q[1].position_thickness[3] = 0;
    struct {
        float position[4], extent[4], rotation[4], blend[4];
        u32 info[4];
    } probe = {.extent = {30, 30, 30, .6f},
               .rotation = {0, 0, 0, 1},
               .blend = {75, face_size, 0, 0},
               .info = {0, 1, lv.ibl.strip, 1}};
    u32 list[65] = {1, 0};
    env.records = upload_buffer(f, &probe, sizeof probe, sizeof probe);
    env.list = upload_buffer(f, list, sizeof list, 4);
    double max_error = 0, max_direction = 0;
    u32 tir_count = 0, projected_different = 0;
    for (u32 local = 0; local < 2; ++local) {
        lv.inputs.probe_records = local ? de_gpu_buffer_bindless(f->dev, env.records) : 0;
        lv.inputs.probe_list = local ? de_gpu_buffer_bindless(f->dev, env.list) : 0;
        lv.inputs.probe_sampler = lv.ibl.sampler;
        float *out = run_reference(f, color, depth, w, h, &sc, &lv, q, n, &env);
        for (u32 i = 0; i < n; ++i) {
            bool tir;
            ray_v direction = reference_exit_direction(&q[i], &tir),
                  rotated = {direction.y, -direction.x, direction.z};
            u32 sky = reference_face(rotated);
            ray_v position = {q[i].position_thickness[0], q[i].position_thickness[1],
                              q[i].position_thickness[2]};
            double travel = 1e30, ds[3] = {direction.x, direction.y, direction.z},
                   ps[3] = {position.x, position.y, position.z};
            for (u32 c = 0; c < 3; ++c)
                if (fabs(ds[c]) > 1e-8) {
                    double distance = ((ds[c] > 0 ? 30 : -30) - ps[c]) / ds[c];
                    if (distance >= 0)
                        travel = fmin(travel, distance);
                }
            u32 face = reference_face(ra(position, rm(direction, travel)));
            projected_different += face != reference_face(direction);
            double coverage =
                local ? fmax(0, fmin(1, fmin(30 - fabs(position.x),
                                             fmin(30 - fabs(position.y), 30 - fabs(position.z))) /
                                            75))
                      : 0;
            for (u32 c = 0; c < 3; ++c) {
                double expected = (colors[sky][c] * (double)lv.ibl.sh[0][3] * (1 - coverage) +
                                   colors[face][c] * (double)probe.extent[3] * coverage) *
                                  (double)lv.pre_exposure;
                max_error = fmax(max_error, fabs(out[i * 16 + c] - expected));
                EXPECT_TRUE(out[i * 16 + c] > .01f);
            }
            max_direction = fmax(max_direction, fabs(out[i * 16 + 8] - direction.x));
            max_direction = fmax(max_direction, fabs(out[i * 16 + 9] - direction.y));
            max_direction = fmax(max_direction, fabs(out[i * 16 + 10] - direction.z));
            EXPECT_FLOAT_EQ(out[i * 16 + 3], 0, 0);
            EXPECT_FLOAT_EQ(out[i * 16 + 7], tir ? 1 : 0, 0);
            tir_count += tir;
        }
        free(out);
    }
    printf("colored rotated sky/local box %u queries, TIR%u, changed box faces%u RGBmax%.9g "
           "directionmax%.9g\n",
           n * 2, tir_count, projected_different, max_error, max_direction);
    EXPECT_TRUE(tir_count >= 2);
    EXPECT_TRUE(projected_different >= 2);
    EXPECT_TRUE(max_error < 2e-6);
    EXPECT_TRUE(max_direction < 2e-6);
    EXPECT_EQ(validation_errors(f->dev), 0u);
    de_gpu_texture_destroy(f->dev, color);
    de_gpu_texture_destroy(f->dev, depth);
    de_gpu_texture_destroy(f->dev, env.texture);
    de_gpu_buffer_destroy(f->dev, env.records);
    de_gpu_buffer_destroy(f->dev, env.list);
    de_gpu_sampler_destroy(f->dev, sampler);
}
TEST_F(refraction, directional_fallback_tir_metal, lx) {
    fallback_reference(f, DE_GPU_METAL);
}
TEST_F(refraction, directional_fallback_tir_vulkan, lx) {
    fallback_reference(f, DE_GPU_VULKAN);
}
TEST_F(refraction, directional_fallback_tir_d3d12, lx) {
    fallback_reference(f, DE_GPU_D3D12);
}

static u32 sphere_mesh(lx *f) {
    const u32 cols = 32, rows = 24;
    u32 id = lx_mesh_plane(f, cols, rows, 1.f);
    fmesh *fm = &g_mesh[id - 1];
    de_render_mesh *m = &fm->rm;
    u8 *vertices = de_gpu_buffer_map(f->dev, fm->buf[0]),
       *records = de_gpu_buffer_map(f->dev, fm->buf[1]);
    ASSERT_TRUE(vertices && records);
    for (u32 ml = 0; ml < fm->meshlets; ++ml) {
        u32 cx = ml % cols, cy = ml / cols;
        ray_v center = {0};
        for (u32 j = 0; j < PATCH; ++j)
            for (u32 i = 0; i < PATCH; ++i) {
                double longitude = (-.5 + ((double)cx + (double)i / (PATCH - 1)) / cols) * PI64,
                       latitude = (-.5 + ((double)cy + (double)j / (PATCH - 1)) / rows) * PI64;
                ray_v normal = {sin(longitude) * cos(latitude), sin(latitude),
                                cos(longitude) * cos(latitude)};
                u32 v = ml * PATCH_VERT + j * PATCH + i;
                float position[3] = {(float)(normal.x * .75), (float)(normal.y * .75),
                                     (float)(normal.z * .75)};
                memcpy(vertices + v * 12, position, 12);
                center = ra(center, rm(normal, .75 / PATCH_VERT));
                double l = fabs(normal.x) + fabs(normal.y) + fabs(normal.z);
                i16 ox = (i16)lround(normal.x / l * 32767), oy = (i16)lround(normal.y / l * 32767);
                u32 packed = (u16)ox | ((u32)(u16)oy << 16);
                memcpy(vertices + m->stream_offset[DE_COOKED_STREAM_NORMAL] + v * 4, &packed, 4);
            }
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 16, (float)center.x);
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 20, (float)center.y);
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 24, (float)center.z);
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 28, .1f);
    }
    for (u32 c = 0; c < 3; ++c) {
        m->bounds_min[c] = -.75f;
        m->bounds_max[c] = .75f;
        m->sphere[c] = 0;
    }
    m->sphere[3] = .75f;
    return id;
}
typedef struct forward_fixture_pass {
    de_rg_resource target, depth, readback;
    de_gpu_buffer upload;
    u32 pitch;
    float reverse_z;
} forward_fixture_pass;
static void forward_opaque_fixture(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    forward_fixture_pass *p = user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->target), 0, 0, p->upload, 0,
                                      p->pitch);
    de_gpu_attachment attachment = {.texture = de_rg_texture(pass, p->depth),
                                    .clear = true,
                                    .store = true,
                                    .clear_depth = p->reverse_z};
    de_gpu_cmd_begin_render(cmd, NULL, 0, &attachment);
    de_gpu_cmd_end_render(cmd);
}
static void forward_image_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    forward_fixture_pass *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->readback), 0, p->pitch,
                                      de_rg_texture(pass, p->target), 0, 0);
}
static float *forward_frame(lx *f, const u8 *opaque, u32 pitch, float exposure, bool sparse,
                            const float *ambient, u32 *pyramid_passes) {
    const u64 frame = f->frames;
    world_begin(&f->w);
    ASSERT_EQ(de__gpu_scene_update(f->gs, &f->w.f), DE_OK);
    de_mat4 view = de_m4_look_at(f->eye, f->target, (de_vec3){0, 1, 0}),
            proj = de_m4_perspective_rz(f->fov, (float)f->width / f->height, .1f);
    ASSERT_EQ(de__gpu_scene_set_view(f->gs, 0,
                                     &(de__gpu_view_params){.view = view,
                                                            .proj = proj,
                                                            .camera_position = f->eye,
                                                            .width = f->width,
                                                            .height = f->height}),
              DE_OK);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    ASSERT_EQ(de__gpu_scene_add_passes(f->gs, f->g), DE_OK);
    de__vis_context *vc = NULL;
    ASSERT_EQ(de__vis_frame_build(f->vis, f->g,
                                  &(de__vis_frame_desc){.gpu_scene = f->gs,
                                                        .view = {1},
                                                        .view_slot = 0,
                                                        .frame_index = frame,
                                                        .frames_completed = frame,
                                                        .width = f->width,
                                                        .height = f->height,
                                                        .view_matrix = view,
                                                        .proj = proj,
                                                        .camera_position = f->eye},
                                  &vc),
              DE_OK);
    ASSERT_TRUE(vc);
    vc->layer_mask = 1;
    de__lighting_context *lc = NULL;
    ASSERT_EQ(de__lighting_build(f->lt, f->g, vc, &lc), DE_OK);
    ASSERT_TRUE(lc && lc->lit);
    lc->view.pre_exposure = exposure;
    memset(&lc->view.ibl, 0, sizeof lc->view.ibl);
    if (ambient)
        memcpy(lc->view.ibl.ambient, ambient, 3 * sizeof(float));
    memcpy(f->lt->views[0].constants_map + (size_t)lc->constants_entry * sizeof lc->view, &lc->view,
           sizeof lc->view);
    de_gpu_buffer scene_override = {0};
    if (sparse) {
        de__gs_view constants[2] = {0};
        matrix_rows(constants[1].view, &view);
        matrix_rows(constants[1].proj, &proj);
        de_mat4 vp = de_m4_mul(&proj, &view), inv = de_m4_inverse(&vp);
        matrix_rows(constants[1].view_proj, &vp);
        matrix_rows(constants[1].inv_view_proj, &inv);
        constants[1].camera_position = (de__gs_float4){.v = {f->eye.x, f->eye.y, f->eye.z, 1}};
        de__gpu_scene_table_info instances = de__gpu_scene_table(f->gs, DE__GS_INSTANCES),
                                 meshes = de__gpu_scene_table(f->gs, DE__GS_MESHES),
                                 materials = de__gpu_scene_table(f->gs, DE__GS_MATERIALS);
        constants[1].instances = instances.bindless;
        constants[1].instance_count = instances.count;
        constants[1].meshes = meshes.bindless;
        constants[1].mesh_count = meshes.count;
        constants[1].materials = materials.bindless;
        constants[1].material_count = materials.count;
        scene_override = upload_buffer(f, constants, sizeof constants, sizeof constants[0]);
        vc->scene_views = de_rg_import_buffer(f->g, scene_override, DE_ACCESS_SHADER_READ);
        vc->scene_view_slot = 1;
    }
    de_gpu_buffer upload =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * f->height,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .usage = DE_USAGE_COPY_SRC});
    ASSERT_TRUE(upload.id);
    memcpy(de_gpu_buffer_map(f->dev, upload), opaque, (size_t)pitch * f->height);
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * f->height,
                                                           .memory = DE_MEM_READBACK,
                                                           .usage = DE_USAGE_COPY_DST});
    ASSERT_TRUE(rb.id);
    lc->target = de_rg_create_texture(
        f->g, &(de_gpu_texture_desc){.width = f->width,
                                     .height = f->height,
                                     .format = DE_FMT_RGBA16_FLOAT,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_STORAGE |
                                              DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC});
    forward_fixture_pass *pd = de_arena_push_zero(f->arena, sizeof *pd, 16);
    ASSERT_TRUE(pd);
    *pd = (forward_fixture_pass){.target = lc->target,
                                 .depth = vc->depth,
                                 .readback = de_rg_import_buffer(f->g, rb, DE_ACCESS_NONE),
                                 .upload = upload,
                                 .pitch = pitch,
                                 .reverse_z = .1f / 6};
    de_rg_pass *pass = de_rg_add_pass(f->g, "refraction.opaque.fixture", DE_QUEUE_GRAPHICS,
                                      forward_opaque_fixture, pd);
    de_rg_read(pass, de_rg_import_buffer(f->g, upload, DE_ACCESS_COPY_SRC), DE_ACCESS_COPY_SRC);
    de_rg_write(pass, lc->target, DE_ACCESS_COPY_DST);
    de_rg_write(pass, vc->depth, DE_ACCESS_DEPTH_WRITE);
    u32 before = f->g->pass_count;
    de__forward_context *fc = NULL;
    ASSERT_EQ(de__forward_build(f->forward, f->g, vc, lc, &f->w.f, &fc), DE_OK);
    *pyramid_passes = 0;
    for (u32 i = before; i < f->g->pass_count; ++i)
        *pyramid_passes += strncmp(f->g->passes[i]->name, "forward.scene_color", 19) == 0;
    if (fc) {
        EXPECT_EQ(fc->scene_depth.id != 0,
                  de_cvar_bool(de_cvar_find("r.refraction")) && fc->scene_color.id != 0);
    }
    pass = de_rg_add_pass(f->g, "refraction.forward.readback", DE_QUEUE_GRAPHICS,
                          forward_image_copy, pd);
    de_rg_pass_side_effect(pass);
    de_rg_read(pass, lc->target, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, pd->readback, DE_ACCESS_COPY_DST);
    pass = de_rg_add_pass(f->g, "refraction.forward.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_pass_side_effect(pass);
    de_rg_write(pass, lc->target, DE_ACCESS_SHADER_WRITE);
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    u64 fence = ++f->frames;
    de_result executed =
        de__rg_execute(f->g, &(de__rg_execute_desc){.frame_fence = f->fence, .frame_value = fence});
    de__gpu_scene_frame_end(f->gs, f->g, executed == DE_OK);
    ASSERT_EQ(executed, DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, fence, FIRST_FRAME_WAIT_NS), DE_OK);
    const u8 *mapped = de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(mapped);
    float *result = zalloc((size_t)f->width * f->height * 4 * sizeof *result);
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x)
            for (u32 c = 0; c < 4; ++c) {
                u16 value;
                memcpy(&value, mapped + y * pitch + x * 8 + c * 2, 2);
                result[(y * f->width + x) * 4 + c] = half_to_f32(value);
            }
    de__rg_reset(f->g);
    de_gpu_buffer_destroy(f->dev, upload);
    de_gpu_buffer_destroy(f->dev, rb);
    de_gpu_buffer_destroy(f->dev, scene_override);
    return result;
}
static void forward_sphere_reference(lx *f, de_gpu_backend backend, const char *name) {
    lx_open(f, backend);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    ASSERT_TRUE(de_cvar_bool(de_cvar_find("r.refraction")));
    de_arena_destroy(f->arena);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (u64)32 << 20}, &f->arena), DE_OK);
    f->width = SPHERE_W;
    f->height = SPHERE_H;
    f->eye = (de_vec3){0, 0, 3};
    f->target = (de_vec3){0, 0, 0};
    f->fov = (float)(55 * PI64 / 180);
    u32 mat = material_add(1, 0, 0, 1);
    de_standard_material *m = &g_mat[mat - 1].standard;
    m->base_color = (de_vec4){.8f, .6f, .4f, 1};
    m->specular_color = (de_vec3){1, 1, 1};
    m->transmission = 1;
    m->thickness = 1.5f;
    m->ior = 1.5f;
    m->attenuation_color = (de_vec3){.7f, .8f, .9f};
    m->attenuation_distance = 2;
    world_set(&f->w, 0, sphere_mesh(f), at(0, 0, 0), mat);
    const u32 count = SPHERE_W * SPHERE_H, pitch = (SPHERE_W * 8 + 255) & ~255u;
    u8 *opaque = zalloc((size_t)pitch * SPHERE_H), *reference = zalloc(count * 4),
       *actual = zalloc(count * 4), *mask = zalloc(count);
    float *image = zalloc(count * sizeof *image), *errors = zalloc(count * sizeof *errors),
          *roi = zalloc(count * sizeof *roi);
    const float exposure = .7f;
    for (u32 y = 0; y < SPHERE_H; ++y)
        for (u32 x = 0; x < SPHERE_W; ++x) {
            u32 i = y * SPHERE_W + x;
            float c = checker(plane_hit((ray_v){0, 0, 3}, camera_ray(x, y)));
            u16 half = reference_half((double)c * exposure), pixel[4] = {half, half, half, 0x3c00};
            image[i] = half_to_f32(half);
            memcpy(opaque + y * pitch + x * 8, pixel, 8);
        }
    for (u32 y = 0; y < SPHERE_H; ++y)
        for (u32 x = 0; x < SPHERE_W; ++x) {
            u32 i = y * SPHERE_W + x;
            ray_v p, n, hit;
            double distance;
            double colors[3] = {image[i], image[i], image[i]};
            if (sphere_hit(camera_ray(x, y), &p, &n, &hit, &distance)) {
                double u, v;
                plane_uv(hit, &u, &v);
                double edge = fmin(fmin(u, 1 - u) * SPHERE_W, fmin(v, 1 - v) * SPHERE_H),
                       t = fmin(1, fmax(0, edge / 2)), fade = t * t * (3 - 2 * t);
                double radiance = image_sample(image, u, v) * fade;
                double nov = rd(n, rm(camera_ray(x, y), -1)), fresnel = .04 + .96 * pow(1 - nov, 5);
                const double base[3] = {m->base_color.x, m->base_color.y, m->base_color.z},
                             atten[3] = {m->attenuation_color.x, m->attenuation_color.y,
                                         m->attenuation_color.z};
                for (u32 c = 0; c < 3; ++c)
                    colors[c] =
                        radiance * base[c] * (1 - fresnel) * exp(log(atten[c]) * distance / 2);
                mask[i] = (u8)sphere_roi(x, y, hit);
            }
            for (u32 c = 0; c < 3; ++c)
                reference[i * 4 + c] = srgb_byte((float)colors[c]);
            reference[i * 4 + 3] = 255;
        }
    u32 passes = 0;
    float *out = forward_frame(f, opaque, pitch, exposure, true, NULL, &passes);
    EXPECT_EQ(passes, 8u);
    for (u32 i = 0; i < count; ++i) {
        for (u32 c = 0; c < 3; ++c)
            actual[i * 4 + c] = srgb_byte(out[i * 4 + c]);
        actual[i * 4 + 3] = 255;
    }
    de_flip_image ref = {reference, SPHERE_W, SPHERE_H, 4, 0},
                  gpu = {actual, SPHERE_W, SPHERE_H, 4, 0};
    ASSERT_TRUE(de_flip_ldr(&ref, &gpu, 0, errors));
    u32 nr = 0;
    for (u32 i = 0; i < count; ++i)
        if (mask[i])
            roi[nr++] = errors[i];
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(roi, nr, &stats));
    printf("full forward physical sphere %s ROI%u FLIPmean%.9f p99%.9f max%.9f (.02/.10 fixed), "
           "Fresnel/base/Beer/preexposure\n",
           name, nr, stats.mean, stats.p99, stats.max);
    EXPECT_EQ(nr, 3817u);
    EXPECT_TRUE(stats.mean <= SPHERE_FLIP_MEAN);
    EXPECT_TRUE(stats.p99 <= SPHERE_FLIP_P99);
    save_image(name, "forward-cpu", reference);
    save_image(name, "forward-gpu", actual);
    EXPECT_EQ(validation_errors(f->dev), 0u);
    free(out);
    free(opaque);
    free(reference);
    free(actual);
    free(mask);
    free(image);
    free(errors);
    free(roi);
}
TEST_F(refraction, forward_sphere_physical_metal, lx) {
    forward_sphere_reference(f, DE_GPU_METAL, "metal");
}
TEST_F(refraction, forward_sphere_physical_vulkan, lx) {
    forward_sphere_reference(f, DE_GPU_VULKAN, "vulkan");
}
TEST_F(refraction, forward_sphere_physical_d3d12, lx) {
    forward_sphere_reference(f, DE_GPU_D3D12, "d3d12");
}

static void final_draw_failure(lx *f) {
    de_gpu_texture target = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = 17,
                                       .height = 11,
                                       .format = DE_FMT_RGBA16_FLOAT,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_STORAGE |
                                                DE_USAGE_COPY_SRC});
    de_gpu_texture depth = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = 17,
                                       .height = 11,
                                       .format = DE_FMT_D32_FLOAT,
                                       .usage = DE_USAGE_DEPTH | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(target.id && depth.id);
    de_arena *original_arena = failure_arena_begin(f);
    bool found = false;
    u32 calibrated = 0;
    for (u32 budget = 512; budget < 65536 && !found; budget += 16) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        de__vis_context vc = {
            .arena = f->arena,
            .gpu_scene = f->gs,
            .depth_target = true,
            .layer_mask = 1,
            .view_matrix = de_m4_identity(),
            .depth = de_rg_import_texture(f->g, depth, DE_ACCESS_DEPTH_WRITE),
            .scene_views = de_rg_import_buffer(
                f->g, de__gpu_scene_table(f->gs, DE__GS_VIEWS).buffer, DE_ACCESS_SHADER_READ)};
        de__lighting_context lc = {.lit = true,
                                   .width = 17,
                                   .height = 11,
                                   .target =
                                       de_rg_import_texture(f->g, target, DE_ACCESS_SHADER_WRITE)};
        de_arena_info info = de_arena_get_info(f->arena);
        ASSERT_TRUE(de_arena_push(f->arena, info.reserved - info.used - budget, 1));
        de__forward_context *context = NULL;
        de_result result = de__forward_build(f->forward, f->g, &vc, &lc, &f->w.f, &context);
        const de_error *error = de_error_last();
        const char *last = f->g->pass_count ? f->g->passes[f->g->pass_count - 1]->name : NULL;
        if (result != DE_OK && ((last && strcmp(last, "forward.transparent") == 0) ||
                                (error && strstr(error->message, "forward: draw pass")))) {
            EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
            EXPECT_FALSE(context);
            EXPECT_TRUE(f->g->oom);
            found = true;
            calibrated = budget;
        }
        de_error_clear();
    }
    printf("final forward draw declaration OOM calibrated at%u bytes; no context published\n",
           calibrated);
    EXPECT_TRUE(found);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    failure_arena_end(f, original_arena);
    de_gpu_texture_destroy(f->dev, target);
    de_gpu_texture_destroy(f->dev, depth);
}
static void forward_modes_lifecycle(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    ASSERT_TRUE(de_cvar_bool(de_cvar_find("r.refraction")));
    f->width = 17;
    f->height = 11;
    f->eye = (de_vec3){0, 0, 3};
    f->target = (de_vec3){0, 0, 0};
    f->fov = 1;
    enum { pitch = 256, h = 11, w = 17 };
    u8 opaque[pitch * h];
    memset(opaque, 0, sizeof opaque);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 p[4] = {reference_half((double)(x + 1) / 32), reference_half((double)(y + 1) / 16),
                        0x3800, 0x3c00};
            memcpy(opaque + y * pitch + x * 8, p, 8);
        }
    u32 mat = material_add(1, 0, 0, 1), mesh = lx_mesh_plane(f, 1, 1, 100);
    de_standard_material *m = &g_mat[mat - 1].standard;
    m->ior = 1;
    m->transmission = 1;
    m->specular_color = (de_vec3){1, 1, 1};
    const double angles[3] = {0, 65 * PI64 / 180, 85 * PI64 / 180};
    double max_identity = 0;
    u32 covered = 0, passes;
    for (u32 angle = 0; angle < 3; ++angle) {
        world_set(&f->w, 0, mesh,
                  trs((de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}, (float)angles[angle], 1), mat);
        float *out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
        EXPECT_EQ(passes, 5u);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                ray_v d = rn((ray_v){(2 * ((double)x + .5) / w - 1) * (double)w / h * tan(.5),
                                     (1 - 2 * ((double)y + .5) / h) * tan(.5), -1}),
                      normal = {sin(angles[angle]), 0, cos(angles[angle])};
                double denominator = rd(normal, d), distance = -3 * normal.z / denominator;
                if (distance > 0) {
                    ray_v p = ra((ray_v){0, 0, 3}, rm(d, distance));
                    double local_x = cos(angles[angle]) * p.x - sin(angles[angle]) * p.z;
                    if (fabs(local_x) < 49 && fabs(p.y) < 49)
                        ++covered;
                }
                u16 source[4];
                memcpy(source, opaque + y * pitch + x * 8, 8);
                for (u32 c = 0; c < 3; ++c)
                    max_identity =
                        fmax(max_identity, fabs(out[(y * w + x) * 4 + c] - half_to_f32(source[c])));
            }
        free(out);
    }
    printf("full forward IOR1 normal/65deg/85deg %u covered pixels max%.9g (.001 fixed)\n", covered,
           max_identity);
    EXPECT_TRUE(covered > 250);
    EXPECT_TRUE(max_identity <= .001);
    ASSERT_TRUE(cvar_set("r.refraction", "0"));
    float *legacy = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 1u);
    double changed = 0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 source;
            memcpy(&source, opaque + y * pitch + x * 8, 2);
            changed = fmax(changed, fabs(legacy[(y * w + x) * 4] - half_to_f32(source)));
        }
    EXPECT_TRUE(changed > .01);
    free(legacy);
    ASSERT_TRUE(cvar_set("r.refraction", "1"));
    float *again = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 5u);
    free(again);
    // Object-scale is applied once in resolve; Beer uses the world optical path, not local depth.
    m->ior = 1.5f;
    m->thickness = .2f;
    m->attenuation_color = (de_vec3){.2f, .4f, .8f};
    m->attenuation_distance = 2;
    m->specular = 0;
    world_set(&f->w, 0, mesh, trs((de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}, 0, 2), mat);
    float *beer = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    double max_beer = 0;
    // Parallel normal incidence exits in the original direction. Use constant background for
    // the independent energy proof to avoid conflating a refracted sample location with Beer.
    free(beer);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 p[4] = {0x3800, 0x3800, 0x3800, 0x3c00};
            memcpy(opaque + y * pitch + x * 8, p, 8);
        }
    beer = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            double vx = (2 * ((double)x + .5) / w - 1) * (double)w / h * tan(.5),
                   vy = (1 - 2 * ((double)y + .5) / h) * tan(.5),
                   cosine = 1 / sqrt(1 + vx * vx + vy * vy),
                   internal = sqrt(1 - (1 - cosine * cosine) / (1.5 * 1.5)),
                   distance = (double).2f * 2 / internal;
            const double attenuation[3] = {m->attenuation_color.x, m->attenuation_color.y,
                                           m->attenuation_color.z};
            for (u32 c = 0; c < 3; ++c) {
                ray_v incoming = rn((ray_v){vx, vy, -1}), position = {vx * 3, vy * 3, 0};
                ray_v internal_ray = rbend(incoming, (ray_v){0, 0, 1}, 1 / 1.5);
                ray_v exit = ra(position, rm(internal_ray, distance)),
                      hit = plane_hit(exit, incoming);
                double u = .5 + .5 * hit.x / (6 * tan(.5) * (double)w / h),
                       v = .5 - .5 * hit.y / (6 * tan(.5));
                double edge = fmin(fmin(u, 1 - u) * w, fmin(v, 1 - v) * h),
                       fade = fmin(1, fmax(0, edge / 2));
                fade = fade * fade * (3 - 2 * fade);
                double expected = .5 * fade * exp(log(attenuation[c]) * distance / 2);
                max_beer = fmax(max_beer, fabs(beer[(y * w + x) * 4 + c] - expected));
            }
        }
    printf("full forward scaled slab Beer max%.9g (.001 fixed)\n", max_beer);
    EXPECT_TRUE(max_beer < .001);
    free(beer);
    // Alpha-only, hidden, masked and out-of-frustum instances never allocate a pyramid.
    m->transmission = 0;
    g_mat[mat - 1].alpha_mode = DE_COOKED_ALPHA_BLEND;
    m->base_color.w = .5f;
    float *out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 0u);
    free(out);
    m->transmission = 1;
    g_mat[mat - 1].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    m->base_color.w = 1;
    f->w.f.instances.flags[0] = 0;
    f->w.touch_all = true;
    out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 0u);
    free(out);
    world_set(&f->w, 0, mesh, at(1000, 0, 0), mat);
    out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 0u);
    free(out);
    world_set(&f->w, 0, mesh, at(0, 0, 0), mat);
    f->w.f.instances.layer_mask[0] = 2;
    f->w.touch_all = true;
    out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 0u);
    free(out);
    f->w.f.instances.layer_mask[0] = 1;
    f->w.touch_all = true;
    out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 5u);
    free(out);
    // Initial material scratch and instance-list growth failures are distinct from empty success.
    de_arena *original_arena = failure_arena_begin(f);
    for (u32 spare = 0; spare <= 64; spare += 64) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        de_arena_info info = de_arena_get_info(f->arena);
        ASSERT_TRUE(de_arena_push(f->arena, info.reserved - info.used - spare, 1));
        de__vis_context vc = {.arena = f->arena,
                              .gpu_scene = f->gs,
                              .depth_target = true,
                              .depth = {1},
                              .scene_views = {2},
                              .layer_mask = 1,
                              .view_matrix = de_m4_identity()};
        de__lighting_context lc = {.lit = true, .target = {3}, .width = w, .height = h};
        de__forward_context *fc = NULL;
        EXPECT_EQ(de__forward_build(f->forward, f->g, &vc, &lc, &f->w.f, &fc),
                  DE_ERR_OUT_OF_MEMORY);
        EXPECT_FALSE(fc);
        EXPECT_EQ(f->g->pass_count, 0u);
        de_error_clear();
    }
    failure_arena_end(f, original_arena);
    final_draw_failure(f);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    out = forward_frame(f, opaque, pitch, .7f, true, NULL, &passes);
    EXPECT_EQ(passes, 5u);
    free(out);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
TEST_F(refraction, forward_modes_lifecycle_metal, lx) {
    forward_modes_lifecycle(f, DE_GPU_METAL);
}
TEST_F(refraction, forward_modes_lifecycle_vulkan, lx) {
    forward_modes_lifecycle(f, DE_GPU_VULKAN);
}
TEST_F(refraction, forward_modes_lifecycle_d3d12, lx) {
    forward_modes_lifecycle(f, DE_GPU_D3D12);
}

static double independent_ggx_tir(ray_v view, ray_v light, ray_v normal, double roughness) {
    double no_v = -rd(normal, view), no_l = -rd(normal, light);
    if (no_v <= 0 || no_l <= 0)
        return 0;
    ray_v half = rn(ra(view, light));
    double no_h = fabs(rd(normal, half)), alpha = roughness * roughness, a2 = alpha * alpha;
    double d = a2 / (PI64 * pow(1 + (a2 - 1) * no_h * no_h, 2));
    double lambda_v = (sqrt(1 + a2 * (1 - no_v * no_v) / (no_v * no_v)) - 1) / 2,
           lambda_l = (sqrt(1 + a2 * (1 - no_l * no_l) / (no_l * no_l)) - 1) / 2;
    double cosine = rd(view, half), eta = 1 / 1.5, sin2 = (1 - cosine * cosine) / (eta * eta),
           fresnel = 1;
    if (sin2 < 1) {
        double transmitted = sqrt(1 - sin2),
               rs = (cosine - eta * transmitted) / (cosine + eta * transmitted),
               rp = (eta * cosine - transmitted) / (eta * cosine + transmitted);
        fresnel = (rs * rs + rp * rp) / 2;
    }
    return fresnel * d / (1 + lambda_v + lambda_l) / (4 * no_v);
}
static void forward_tir_reference(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    f->width = 33;
    f->height = 23;
    f->eye = (de_vec3){0, 0, 3};
    f->target = (de_vec3){0, 0, 0};
    f->fov = .3f;
    enum { w = 33, h = 23, pitch = 512 };
    u8 opaque[pitch * h];
    memset(opaque, 0, sizeof opaque);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 p[4] = {0x3c00, 0x3800, 0x3400, 0x3c00};
            memcpy(opaque + y * pitch + x * 8, p, 8);
        }
    u32 mat = material_add(.3f, .6f, 0, 1);
    de_standard_material *m = &g_mat[mat - 1].standard;
    m->transmission = 1;
    m->thickness = .3f;
    m->ior = 1.5f;
    m->specular_color = (de_vec3){1, 1, 1};
    m->double_sided = true;
    m->emissive = (de_vec3){.03f, .05f, .07f};
    m->emissive_strength = 1;
    double angle = acos(-sqrt(.19));
    ray_v normal = {sin(angle), 0, cos(angle)};
    world_set(&f->w, 0, lx_mesh_plane(f, 1, 1, 20),
              trs((de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}, (float)angle, 1), mat);
    DeLight sun = {.kind = DE_LIGHT_DIRECTIONAL, .color = {.7f, .5f, .3f}, .intensity = 1.3f};
    ray_v light = ra((ray_v){0, 0, -1}, rm(normal, 2 * normal.z));
    const float ambient[3] = {.17f, .37f, .71f};
    const double emission[3] = {m->emissive.x, m->emissive.y, m->emissive.z},
                 light_color[3] = {sun.color.x, sun.color.y, sun.color.z};
    double max_error = 0;
    u32 checked = 0;
    for (u32 orientation = 0; orientation < 2; ++orientation) {
        light = orientation ? (ray_v){0, 0, 1} : ra((ray_v){0, 0, -1}, rm(normal, 2 * normal.z));
        world_light(&f->w, 0, &sun,
                    trs((de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}, (float)atan2(light.x, light.z), 1));
        float *baseline = NULL;
        for (u32 mix = 0; mix < 2; ++mix) {
            m->transmission = mix ? .4f : 1;
            u32 passes;
            float *out = forward_frame(f, opaque, pitch, .7f, true, ambient, &passes);
            EXPECT_EQ(passes, 6u);
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    ray_v ray =
                              rn((ray_v){(2 * ((double)x + .5) / w - 1) * (double)w / h * tan(.15),
                                         (1 - 2 * ((double)y + .5) / h) * tan(.15), -1}),
                          view = rm(ray, -1);
                    double distance = -3 * normal.z / rd(normal, ray);
                    ray_v p = ra((ray_v){0, 0, 3}, rm(ray, distance));
                    view = rn(ra((ray_v){0, 0, 3}, rm(p, -1)));
                    double cosine = fabs(rd(normal, view));
                    if (distance <= 0 || 1.5 * 1.5 * (1 - cosine * cosine) <= 1)
                        continue;
                    double ggx = independent_ggx_tir(view, light, normal, (double)m->roughness);
                    for (u32 c = 0; c < 3; ++c) {
                        double expected = ((double)ambient[c] + emission[c] +
                                           ggx * light_color[c] * (double)sun.intensity) *
                                          (double).7f;
                        max_error = fmax(max_error, fabs(out[(y * w + x) * 4 + c] - expected));
                        if (baseline)
                            EXPECT_FLOAT_EQ(out[(y * w + x) * 4 + c], baseline[(y * w + x) * 4 + c],
                                            .001);
                    }
                    ++checked;
                }
            if (!baseline)
                baseline = out;
            else
                free(out);
        }
        free(baseline);
    }
    printf("full forward inside TIR direct+colored environment+emission t1/.4 %u queries max%.9g "
           "(.002 fixed)\n",
           checked, max_error);
    EXPECT_TRUE(checked > 2500);
    EXPECT_TRUE(max_error < .002);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
TEST_F(refraction, forward_tir_energy_metal, lx) {
    forward_tir_reference(f, DE_GPU_METAL);
}
TEST_F(refraction, forward_tir_energy_vulkan, lx) {
    forward_tir_reference(f, DE_GPU_VULKAN);
}
TEST_F(refraction, forward_tir_energy_d3d12, lx) {
    forward_tir_reference(f, DE_GPU_D3D12);
}

static u32 normal_texture_bindless(void *user, de_texture texture) {
    return texture.id == 1 ? *(const u32 *)user : 0;
}
static void forward_proxy_exit_reference(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->forward), DE_OK);
    enum { w = 17, h = 11, pitch = 256 };
    f->width = w;
    f->height = h;
    f->eye = (de_vec3){0, 0, 3};
    f->target = (de_vec3){0, 0, 0};
    f->fov = .02f;
    u8 normal_raw[256] = {0};
    u16 encoded[4] = {0x3c00, 0x3800, reference_half(.52), 0x3c00};
    memcpy(normal_raw, encoded, 8);
    de_gpu_texture normal =
        upload_image_usage(f, 1, 1, DE_FMT_RGBA16_FLOAT, normal_raw, 256, DE_USAGE_SAMPLED);
    u32 normal_index = de_gpu_texture_bindless(f->dev, normal, 0, false);
    de__gpu_scene_destroy(f->gs);
    f->gs = NULL;
    de__gpu_scene_desc gd = {.device = f->dev,
                             .allocator = &f->alloc,
                             .resolver = {.user = &normal_index,
                                          .mesh = fake_mesh,
                                          .material = fake_material,
                                          .texture_bindless = normal_texture_bindless},
                             .staging_bytes = (u64)16 << 20};
    gd.initial_capacity[DE__GS_INSTANCES] = 32;
    gd.initial_capacity[DE__GS_LIGHTS] = MAX_LIGHTS;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    u32 mat = material_add(.3f, .4f, 0, 1);
    de_render_material *record = &g_mat[mat - 1];
    de_standard_material *m = &record->standard;
    m->base_color = (de_vec4){.3f, .5f, .8f, 1};
    m->ior = 1.5f;
    m->specular_color = (de_vec3){1, 1, 1};
    m->thickness = .5f;
    m->attenuation_color = (de_vec3){.2f, .3f, .4f};
    m->attenuation_distance = 1;
    record->textures[DE_COOKED_TEX_NORMAL].id = 1;
    record->alpha_mode = DE_COOKED_ALPHA_BLEND;
    world_set(&f->w, 0, lx_mesh_plane(f, 1, 1, 5), at(0, 0, 0), mat);
    u8 opaque[pitch * h];
    memset(opaque, 0, sizeof opaque);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 p[4] = {0x3800, 0x3400, 0x3000, 0x3c00};
            memcpy(opaque + y * pitch + x * 8, p, 8);
        }
    const float ambient[3] = {.17f, .37f, .71f};
    u32 passes;
    float *base = forward_frame(f, opaque, pitch, .7f, true, ambient, &passes);
    EXPECT_EQ(passes, 0u);
    // Prove all fixture rays enter the perturbed facet and exceed the independent planar
    // exit critical angle; the earlier .57 normal did not actually reach exit TIR.
    ray_v perturbed = rn((ray_v){1, 0, 2 * (double)half_to_f32(encoded[2]) - 1});
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            ray_v incoming = rn((ray_v){(2 * ((double)x + .5) / w - 1) * (double)w / h * tan(.01),
                                        (1 - 2 * ((double)y + .5) / h) * tan(.01), -1});
            ASSERT_TRUE(rd(rm(incoming, -1), perturbed) > 0);
            ray_v interior = rbend(incoming, perturbed, 1 / 1.5);
            ASSERT_TRUE(1.5 * 1.5 * (1 - interior.z * interior.z) > 1);
        }
    double max_error = 0;
    for (u32 mix = 0; mix < 2; ++mix) {
        float t = mix ? .4f : 1;
        m->transmission = t;
        float *out = forward_frame(f, opaque, pitch, .7f, true, ambient, &passes);
        EXPECT_EQ(passes, 5u);
        for (u32 i = 0; i < w * h; ++i)
            for (u32 c = 0; c < 3; ++c) {
                double expected =
                    (1 - (double)t) * base[i * 4 + c] + (double)t * ambient[c] * (double).7f;
                max_error = fmax(max_error, fabs(out[i * 4 + c] - expected));
            }
        free(out);
    }
    printf(
        "full forward normal-mapped proxy exit TIR t1/.4 %u mixture queries max%.9g (.002 fixed)\n",
        w * h * 2, max_error);
    EXPECT_TRUE(max_error < .002);
    free(base);
    EXPECT_EQ(validation_errors(f->dev), 0u);
    de_gpu_texture_destroy(f->dev, normal);
}
TEST_F(refraction, forward_proxy_exit_mixture_metal, lx) {
    forward_proxy_exit_reference(f, DE_GPU_METAL);
}
TEST_F(refraction, forward_proxy_exit_mixture_vulkan, lx) {
    forward_proxy_exit_reference(f, DE_GPU_VULKAN);
}
TEST_F(refraction, forward_proxy_exit_mixture_d3d12, lx) {
    forward_proxy_exit_reference(f, DE_GPU_D3D12);
}

static const de__gpu_backend_vtable *refraction_original_vt;
static bool refraction_fail_buffer, refraction_fail_submit;
static de_result fail_refraction_buffer(de_gpu_device *dev, de__gpu_buffer *buffer,
                                        const de_gpu_buffer_desc *desc) {
    if (refraction_fail_buffer && (desc->usage & DE_USAGE_STORAGE)) {
        refraction_fail_buffer = false;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected refraction transient buffer failure");
    }
    return refraction_original_vt->buffer_create(dev, buffer, desc);
}
static de_result fail_refraction_buffer_placed(de_gpu_device *dev, de__gpu_buffer *buffer,
                                               const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                               u64 offset) {
    if (refraction_fail_buffer && (desc->usage & DE_USAGE_STORAGE)) {
        refraction_fail_buffer = false;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected placed refraction buffer failure");
    }
    return refraction_original_vt->buffer_create_placed(dev, buffer, desc, heap, offset);
}
static de_result report_refraction_submit_failure(de_gpu_device *dev, de_gpu_queue queue,
                                                  const de_gpu_submit *submit, u64 value) {
    de_result result = refraction_original_vt->queue_submit(dev, queue, submit, value);
    if (result == DE_OK && refraction_fail_submit) {
        refraction_fail_submit = false;
        return DE_FAIL(DE_ERR_INTERNAL, "injected refraction failure after actual submit");
    }
    return result;
}
static void pyramid_gpu_failure_retry(lx *f, de_gpu_backend backend) {
    lx_open(f, backend);
    enum { w = 113, h = 79, pitch = 1024, zpitch = 512 };
    u8 *raw = zalloc(pitch * h), *zraw = zalloc(zpitch * h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u16 color[4] = {0x3400, 0x3800, 0x3a00, 0x3c00};
            memcpy(raw + y * pitch + x * 8, color, 8);
            float z = .1f / 6;
            memcpy(zraw + y * zpitch + x * 4, &z, 4);
        }
    de_gpu_texture color = upload_image(f, w, h, DE_FMT_RGBA16_FLOAT, raw, pitch),
                   depth = upload_image(f, w, h, DE_FMT_R32_FLOAT, zraw, zpitch);
    de__scene_color_layout layout;
    ASSERT_EQ(de__scene_color_layout_make(w, h, &layout), DE_OK);
    u64 depth_offset = (layout.bytes + 511) & ~(u64)511;
    de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = depth_offset + layout.depth_bytes,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK});
    ASSERT_TRUE(rb.id);
    de__scene_color_pyramid *p = NULL;
    ASSERT_EQ(de__scene_color_create(f->vis, &p), DE_OK);
    for (u32 attempt = 0; attempt < 4; ++attempt) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        de__scene_color_context context;
        ASSERT_EQ(de__scene_color_build(
                      p, f->g, f->arena, de_rg_import_texture(f->g, color, DE_ACCESS_COPY_SRC),
                      de_rg_import_texture(f->g, depth, DE_ACCESS_COPY_SRC), w, h, &context),
                  DE_OK);
        pyramid_readback *pd = de_arena_push_zero(f->arena, sizeof *pd, 16);
        ASSERT_TRUE(pd);
        *pd = (pyramid_readback){context.color, context.depth, rb,
                                 layout.bytes,  depth_offset,  layout.depth_bytes};
        de_rg_pass *pass = de_rg_add_pass(f->g, "refraction.failure.readback", DE_QUEUE_GRAPHICS,
                                          pyramid_copy, pd);
        de_rg_pass_side_effect(pass);
        de_rg_read(pass, context.color, DE_ACCESS_COPY_SRC);
        de_rg_read(pass, context.depth, DE_ACCESS_COPY_SRC);
        de_rg_write(pass, de_rg_import_buffer(f->g, rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        if (attempt == 0)
            continue; // discard a completely recorded graph before execution
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        refraction_original_vt = f->dev->vt;
        de__gpu_backend_vtable injected = *refraction_original_vt;
        injected.buffer_create = fail_refraction_buffer;
        injected.buffer_create_placed = fail_refraction_buffer_placed;
        injected.queue_submit = report_refraction_submit_failure;
        refraction_fail_buffer = attempt == 1;
        refraction_fail_submit = attempt == 2;
        f->dev->vt = &injected;
        de_result compiled = de__rg_compile(f->g);
        f->dev->vt = refraction_original_vt;
        if (attempt == 1) {
            EXPECT_FALSE(refraction_fail_buffer);
            EXPECT_EQ(compiled, DE_ERR_INVALID_ARGUMENT); /* RG wraps a failed placed resource. */
            de_error_clear();
            continue;
        }
        ASSERT_EQ(compiled, DE_OK);
        u64 fence = ++f->frames;
        f->dev->vt = &injected;
        de_result executed = de__rg_execute(
            f->g, &(de__rg_execute_desc){.frame_fence = f->fence, .frame_value = fence});
        f->dev->vt = refraction_original_vt;
        if (attempt == 2) {
            EXPECT_FALSE(refraction_fail_submit);
            EXPECT_EQ(executed, DE_ERR_INTERNAL);
            de_gpu_wait_idle(f->dev);
            de_error_clear();
            continue;
        }
        ASSERT_EQ(executed, DE_OK);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, fence, FIRST_FRAME_WAIT_NS), DE_OK);
        const u8 *got = de_gpu_buffer_map(f->dev, rb);
        ASSERT_TRUE(got);
        for (u32 level = 0; level < layout.count; ++level) {
            de__scene_color_level a = layout.level[level];
            for (u32 y = 0; y < a.height; ++y)
                for (u32 x = 0; x < a.width; ++x) {
                    const u16 expected[4] = {0x3400, 0x3800, 0x3a00, 0x3c00};
                    EXPECT_MEMEQ(got + a.offset + y * a.pitch + x * 8, expected, 8);
                }
        }
    }
    printf("pyramid preexecute abort + GPU allocation failure + reported postsubmit failure => "
           "complete retry across%u mips\n",
           layout.count);
    de__rg_reset(f->g);
    de__scene_color_destroy(p);
    de_gpu_texture_destroy(f->dev, color);
    de_gpu_texture_destroy(f->dev, depth);
    de_gpu_buffer_destroy(f->dev, rb);
    free(raw);
    free(zraw);
    EXPECT_EQ(validation_errors(f->dev), 0u);
}
TEST_F(refraction, gpu_failure_retry_metal, lx) {
    pyramid_gpu_failure_retry(f, DE_GPU_METAL);
}
TEST_F(refraction, gpu_failure_retry_vulkan, lx) {
    pyramid_gpu_failure_retry(f, DE_GPU_VULKAN);
}
TEST_F(refraction, gpu_failure_retry_d3d12, lx) {
    pyramid_gpu_failure_retry(f, DE_GPU_D3D12);
}
TEST(refraction, packed_layout_contract) {
    de__scene_color_layout layout;
    EXPECT_EQ(de__scene_color_layout_make(0, 1, &layout), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__scene_color_layout_make(1, 0, &layout), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__scene_color_layout_make(32769, 1, &layout), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__scene_color_layout_make(32768, 32768, &layout), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__scene_color_layout_make(17, 11, NULL), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de__scene_color_layout_make(32768, 1, &layout), DE_OK);
    EXPECT_EQ(layout.count, 16u);
    EXPECT_EQ(layout.level[15].width, 1u);
    EXPECT_EQ(layout.level[15].height, 1u);
    ASSERT_EQ(de__scene_color_layout_make(1, 32768, &layout), DE_OK);
    EXPECT_EQ(layout.count, 16u);
    for (u32 i = 0; i < layout.count; ++i) {
        EXPECT_EQ(layout.level[i].offset % 512u, 0u);
        EXPECT_EQ(layout.level[i].pitch % 256u, 0u);
        EXPECT_TRUE(layout.level[i].offset + (u64)layout.level[i].pitch * layout.level[i].height <=
                    layout.bytes);
    }
    de__scene_color_pyramid *p = NULL;
    EXPECT_EQ(de__scene_color_create(NULL, &p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(p);
    EXPECT_EQ(sizeof(de__scene_color_params), 48u);
    EXPECT_EQ(sizeof(refraction_params), 64u);
    EXPECT_EQ(sizeof(de__fwd_params), 96u);
    EXPECT_EQ(sizeof(de__lighting_view), 400u);
    EXPECT_EQ(sizeof(de__lighting_inputs_gpu), 64u);
    de_error_clear();
}
