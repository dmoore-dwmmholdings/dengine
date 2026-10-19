#define SW 64u
#define SH 64u
#include "rt_shadow_fixture.h"
#include "render/rt/rt_reflections.h"
#include "render/rt/rtao.h"
#include "render/probes/probes_internal.h"
#include "flip.h"
#include "render/shaders/engine_shaders.h"

typedef struct reflection_fixture {
    shadow_fixture shadow;
    de_gpu_buffer rb;
    de__rt_reflections_info info;
    f32 pixels[SW * SH * 4];
    de_gpu_buffer sphere_indices;
    de_cooked_mesh_range sphere_range;
    de_render_submesh sphere_submesh;
} reflection_fixture;
static void camera_up(reflection_fixture *f, de_vec3 eye, de_vec3 target, de_vec3 up) {
    de_mat4 v = de_m4_look_at(eye, target, up), inv = de_m4_inverse_affine(&v);
    de__rw_view *camera = &f->shadow.camera;
    camera->transform = de_affine_from_m4(&inv);
    camera->offset = eye;
    de_mat3 rotation;
    memcpy(rotation.m, camera->transform.m, sizeof rotation.m);
    camera->rotation = de_quat_from_m3(&rotation);
}
static void camera_set(reflection_fixture *f, de_vec3 eye, de_vec3 target) {
    camera_up(f, eye, target, (de_vec3){0, 0, 1});
}
static void reflection_fixture_setup(reflection_fixture *f) {
    shadow_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    de_render_settings s = de_renderer_get_settings(r->r);
    s.rt_shadows = false;
    s.shadows = DE_QUALITY_OFF;
    s.reflections = DE_QUALITY_HIGH;
    s.rt_reflections = true;
    de_renderer_set_settings(r->r, &s);
    (void)cvar_set("r.emissive_light_max", "0");
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    g_mat[0].standard.roughness = .01f;
    g_mat[0].standard.metallic = 1;
    g_mat[0].standard.specular = 1;
    g_mat_count = 1;
    for (u32 face = 0; face < 6; ++face) {
        u32 material = material_add(1, .8f, 0, 1);
        g_mat[material - 1].standard.unlit = true;
        g_mat[material - 1].standard.base_color =
            (de_vec4){.1f + (face % 3 == 0 ? .6f : 0), .1f + (face % 3 == 1 ? .6f : 0),
                      .1f + (face % 3 == 2 ? .6f : 0), 1};
        g_mat[material - 1].standard.double_sided = true;
        de__rw_view c;
        de__probe_face_camera((de_dvec3){0}, (de_dvec3){0}, face, &c);
        de_affine xf = c.transform;
        for (u32 j = 0; j < 3; ++j) {
            xf.m[9 + j] = -xf.m[6 + j];
            xf.m[6 + j] = -xf.m[6 + j];
        }
        xf.m[9] += 3;
        xf.m[11] += 1;
        world_set(&r->w, face + 1, 2, xf, material);
        r->w.f.instances.flags[face + 1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[face + 1] = 1u << 24;
    }
    f->shadow.camera.camera.projection = DE_PROJ_PERSPECTIVE;
    f->shadow.camera.camera.fov_y = 1.0f;
    camera_set(f, (de_vec3){3, -6, 3}, (de_vec3){3, 0, .5f});
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = SW * SH * 80,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id);
}
static void reflection_fixture_teardown(reflection_fixture *f) {
    if (f->shadow.rt.d) {
        de_gpu_wait_idle(f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->rb);
    }
    if (f->sphere_indices.id) {
        mesh_free(&g_mesh[2], f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->sphere_indices);
    }
    (void)cvar_set("r.emissive_light_max", "64");
    shadow_fixture_teardown(&f->shadow);
}
static void reflection_frame(reflection_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    shadow_frame(&f->shadow);
    ASSERT_TRUE(de__rt_reflections_get(r->r->rt_reflections, r->view, &f->info));
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Reflection acceptance readback");
    de_gpu_barrier b = {
        .texture = f->info.radiance, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, 0, SW * 8, f->info.radiance, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    b.texture = f->info.distance;
    b.before = DE_ACCESS_SHADER_READ;
    b.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, SW * SH * 72, SW * 8, f->info.distance, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_barrier buffers[2] = {
        {.buffer = f->info.rays, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC},
        {.buffer = f->info.hits, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC}};
    de_gpu_cmd_barriers(cmd, buffers, 2);
    de_gpu_cmd_copy_buffer(cmd, f->rb, SW * SH * 8, f->info.rays, 0, SW * SH * 32);
    de_gpu_cmd_copy_buffer(cmd, f->rb, SW * SH * 40, f->info.hits, 0, SW * SH * 32);
    for (u32 i = 0; i < 2; ++i) {
        buffers[i].before = DE_ACCESS_COPY_SRC;
        buffers[i].after = DE_ACCESS_SHADER_READ;
    }
    de_gpu_cmd_barriers(cmd, buffers, 2);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    const u16 *data = de_gpu_buffer_map(r->d, f->rb);
    ASSERT_TRUE(data);
    for (u32 i = 0; i < SW * SH * 4; ++i)
        f->pixels[i] = half_to_f32(data[i]);
}

#include "render/denoise/reblur_internal.h"
#include "render/rt/restir_di.h"
#define PIXELS (SW * SH)
static void add_sphere(reflection_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    const u32 cols = 12, rows = 8;
    fmesh *fm = &g_mesh[2];
    mesh_plane(fm, r->d, cols, rows, 1);
    g_mesh_count = 3;
    de_render_mesh *m = &fm->rm;
    u8 *vertices = de_gpu_buffer_map(r->d, fm->buf[0]),
       *records = de_gpu_buffer_map(r->d, fm->buf[1]);
    u32 *indices = malloc(fm->meshlets * PATCH_TRIS * 3 * 4);
    ASSERT_TRUE(indices);
    u32 ni = 0;
    for (u32 ml = 0; ml < fm->meshlets; ++ml) {
        de_vec3 center = {0};
        for (u32 y = 0; y < PATCH; ++y)
            for (u32 x = 0; x < PATCH; ++x) {
                double longitude =
                           (((double)(ml % cols) + (double)x / (PATCH - 1)) / cols) * 2 * DE_PI,
                       latitude =
                           (-.5 + ((double)(ml / cols) + (double)y / (PATCH - 1)) / rows) * DE_PI;
                de_vec3 n = {(f32)(cos(longitude) * cos(latitude)),
                             (f32)(sin(longitude) * cos(latitude)), (f32)sin(latitude)};
                u32 v = ml * PATCH_VERT + y * PATCH + x;
                de_vec3 pos = de_v3_scale(n, .8f);
                memcpy(vertices + v * 12, &pos, 12);
                center = de_v3_add(center, de_v3_scale(pos, 1.f / PATCH_VERT));
                f32 length = fabsf(n.x) + fabsf(n.y) + fabsf(n.z), ox = n.x / length,
                    oy = n.y / length;
                if (n.z < 0) {
                    f32 old = ox;
                    ox = (1 - fabsf(oy)) * (old < 0 ? -1 : 1);
                    oy = (1 - fabsf(old)) * (oy < 0 ? -1 : 1);
                }
                u32 packed =
                    (u16)(i16)lrintf(ox * 32767) | ((u32)(u16)(i16)lrintf(oy * 32767) << 16);
                put_u32(vertices + m->stream_offset[DE_COOKED_STREAM_NORMAL] + v * 4, packed);
            }
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 16, center.x);
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 20, center.y);
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 24, center.z);
        put_f32(records + ml * DE_COOKED_MESHLET_SIZE + 28, .4f);
        for (u32 y = 0; y + 1 < PATCH; ++y)
            for (u32 x = 0; x + 1 < PATCH; ++x) {
                u32 a = ml * PATCH_VERT + y * PATCH + x, b = a + 1, c = b + PATCH, d = a + PATCH;
                indices[ni++] = a;
                indices[ni++] = b;
                indices[ni++] = c;
                indices[ni++] = a;
                indices[ni++] = c;
                indices[ni++] = d;
            }
    }
    f->sphere_indices = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){.size = (u64)ni * 4,
                                    .memory = DE_MEM_UPLOAD,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT});
    memcpy(de_gpu_buffer_map(r->d, f->sphere_indices), indices, ni * 4);
    free(indices);
    m->buffers[DE_RENDER_MESH_INDICES] = f->sphere_indices;
    m->bindless[DE_RENDER_MESH_INDICES] = de_gpu_buffer_bindless(r->d, f->sphere_indices);
    m->index_count = ni;
    m->lods[0].index_count = ni;
    f->sphere_range = (de_cooked_mesh_range){.index_count = ni};
    m->lod_submeshes = &f->sphere_range;
    m->submeshes = &f->sphere_submesh;
    for (u32 i = 0; i < 3; ++i) {
        m->bounds_min[i] = -.8f;
        m->bounds_max[i] = .8f;
        m->sphere[i] = 0;
    }
    m->sphere[3] = .8f;
    ASSERT_EQ(de__rt_mesh_publish(r->r->rt_scene, m), DE_OK);
    g_mat[0].standard.roughness = .4f;
    g_mat[0].standard.base_color = (de_vec4){.7f, .5f, .2f, 1};
    world_set(&r->w, 0, 3, at(3, -1, .8f), 1);
    r->w.f.instances.flags[0] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[0] = 1u << 24;
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.2"));
}
typedef struct di_fixture {
    shadow_fixture shadow;
    de_gpu_buffer readback;
    f64 sum[PIXELS * 3];
    f32 reference[PIXELS * 3];
    f32 current[PIXELS * 3];
    de__di_reservoir reservoirs[PIXELS];
    de__di_info info;
} di_fixture;
static void set_lights(di_fixture *f, u32 count) {
    world *w = &f->shadow.rt.w;
    de__rw_lights *lights = &w->f.lights;
    memset(lights->entity, 0, 1024 * sizeof *lights->entity);
    lights->count = count;
    for (u32 i = 0; i < count; ++i) {
        lights->entity[i] = 8000 + i;
        lights->transform[i] =
            at(-.75f + 7.5f * (f32)(i % 32) / 31, -1.8f + 3.6f * (f32)((i / 32) % 16) / 15,
               3.5f + .5f * (f32)(i % 7) / 6);
        lights->light[i] =
            (DeLight){.kind = DE_LIGHT_POINT,
                      .color = {.7f + .3f * (f32)(i % 5) / 4, .8f + .2f * (f32)(i % 3) / 2, 1},
                      .intensity = 20,
                      .range = 20,
                      .source_radius = 0,
                      .cast_shadows = true};
    }
    w->touch_lights = true;
}
static void di_fixture_setup(di_fixture *f) {
    shadow_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    de__rw_lights *lights = &r->w.f.lights;
    free(lights->entity);
    free(lights->transform);
    free(lights->light);
    free(r->w.f.dirty_lights);
    lights->entity = calloc(1024, sizeof *lights->entity);
    lights->transform = calloc(1024, sizeof *lights->transform);
    lights->light = calloc(1024, sizeof *lights->light);
    r->w.f.dirty_lights = calloc(1024, sizeof *r->w.f.dirty_lights);
    ASSERT_TRUE(lights->entity && lights->transform && lights->light && r->w.f.dirty_lights);
    set_lights(f, 512);
    ASSERT_TRUE(cvar_set("r.rt.restir_di", "1"));
    ASSERT_TRUE(cvar_set("r.rt.restir_di_unbiased", "1"));
    f->readback = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 1u << 20,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->readback.id != 0);
}
static void di_fixture_teardown(di_fixture *f) {
    if (f->shadow.rt.d) {
        de_gpu_wait_idle(f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->readback);
    }
    (void)cvar_set("r.rt.restir_di_unbiased", "0");
    (void)cvar_set("r.rt.restir_di_initial_visibility", "0");
    shadow_fixture_teardown(&f->shadow);
}
static void read_di(di_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    ASSERT_TRUE(de__restir_di_get(r->r->restir_di, r->view, &f->info));
    de_gpu_cmd *c = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "ReSTIR acceptance readback");
    de_gpu_barrier b[4] = {
        {.texture = f->info.diffuse, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC},
        {.texture = f->info.specular, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC},
        {.buffer = f->info.reservoir, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC},
        {.buffer = f->info.sources, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC}};
    de_gpu_cmd_barriers(c, b, 4);
    de_gpu_cmd_copy_texture_to_buffer(c, f->readback, 0, SW * 16, f->info.diffuse, 0, 0);
    de_gpu_cmd_copy_texture_to_buffer(c, f->readback, PIXELS * 16, SW * 16, f->info.specular, 0, 0);
    de_gpu_cmd_copy_buffer(c, f->readback, PIXELS * 32, f->info.reservoir, 0, PIXELS * 32);
    de_gpu_cmd_copy_buffer(c, f->readback, PIXELS * 64, f->info.sources, 0,
                           (u64)f->info.source_count * 192);
    for (u32 i = 0; i < 4; ++i) {
        b[i].before = DE_ACCESS_COPY_SRC;
        b[i].after = DE_ACCESS_SHADER_READ;
    }
    de_gpu_cmd_barriers(c, b, 4);
    ASSERT_EQ(
        de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(r->d);
    const f32 *values = de_gpu_buffer_map(r->d, f->readback), *specular = values + PIXELS * 4;
    for (u32 i = 0; i < PIXELS; ++i)
        for (u32 j = 0; j < 3; ++j)
            f->current[i * 3 + j] = values[i * 4 + j] + specular[i * 4 + j];
    memcpy(f->reservoirs, values + PIXELS * 8, sizeof f->reservoirs);
}
static void di_frame(di_fixture *f) {
    shadow_frame(&f->shadow);
    read_di(f);
}
static u8 display(float value) {
    value = fmaxf(0, value);
    return (u8)lrintf(
        fminf(1, value <= .0031308f ? 12.92f * value : 1.055f * powf(value, 1 / 2.4f) - .055f) *
        255);
}
/* Fixed before first measurement: 64 production frames, 512 independent Hammersley samples;
 * FLIP .06 rough/DI and .03 mirror, mirror edge displacement <=2 pixels. */
TEST(denoise_gi_spec, roughness_alpha_and_virtual_motion) {
    EXPECT_FLOAT_EQ(de__reblur_roughness_alpha(0), .5, 1e-7);
    EXPECT_FLOAT_EQ(de__reblur_roughness_alpha(.3f), .1715, 1e-6);
    EXPECT_FLOAT_EQ(de__reblur_roughness_alpha(1), .05, 1e-7);
    de_mat4 previous = de_m4_identity();
    previous.m[12] = .2f;
    /* Normal incidence: reflected hit (0,0,2), virtual image (0,0,-2).
     * Orthographic previous camera shifts projected x by .2 NDC. */
    de_vec2 actual = de__reblur_virtual_uv((de_vec3){0, 0, 2}, (de_vec3){0}, (de_vec3){0, 0, 1}, 2,
                                           &previous, (de_vec2){.5f, .5f}, (de_vec2){.2f, -.4f}, 0);
    EXPECT_FLOAT_EQ(actual.x, .6, 1e-6);
    EXPECT_FLOAT_EQ(actual.y, .5, 1e-6);
    actual = de__reblur_virtual_uv((de_vec3){0, 0, 2}, (de_vec3){0}, (de_vec3){0, 0, 1}, 2,
                                   &previous, (de_vec2){.5f, .5f}, (de_vec2){.2f, -.4f}, 1);
    EXPECT_FLOAT_EQ(actual.x, .4, 1e-6);
    EXPECT_FLOAT_EQ(actual.y, .3, 1e-6);
}
static double compare_rgb(const f32 *expected, const f32 *actual, u32 stride, f32 exposure) {
    u8 ref[PIXELS * 3], image[PIXELS * 3];
    f32 errors[PIXELS];
    double energy = 0;
    for (u32 i = 0; i < PIXELS; ++i)
        for (u32 c = 0; c < 3; ++c) {
            ref[i * 3 + c] = display(expected[i * stride + c] * exposure);
            image[i * 3 + c] = display(actual[i * stride + c] * exposure);
            energy += expected[i * stride + c];
            ASSERT_TRUE(isfinite(actual[i * stride + c]));
        }
    ASSERT_TRUE(energy > 1);
    de_flip_image a = {ref, SW, SH, 3, 0}, b = {image, SW, SH, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&a, &b, 0, errors));
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(errors, PIXELS, &stats));
    printf("Independent reference energy %.9g FLIP mean %.9g p99 %.9g\n", energy, stats.mean,
           (double)stats.p99);
    return stats.mean;
}
static void assert_reflected_coverage(reflection_fixture *f) {
    const u8 *bytes = de_gpu_buffer_map(f->shadow.rt.d, f->rb);
    const de__rt_ray *rays = (const de__rt_ray *)(bytes + PIXELS * 8);
    const de__rt_hit *hits = (const de__rt_hit *)(bytes + PIXELS * 40);
    u32 active = 0, hit = 0, colored = 0;
    for (u32 i = 0; i < PIXELS; ++i) {
        active += rays[i].direction[3] > 0;
        hit += rays[i].direction[3] > 0 && hits[i].t >= 0;
        const f32 *rgb = &f->pixels[i * 4];
        colored +=
            hits[i].t >= 0 &&
            fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])) - fminf(rgb[0], fminf(rgb[1], rgb[2])) > .01f;
    }
    printf("Actual reflection coverage: %u active rays, %u closest hits, %u colored hit pixels\n",
           active, hit, colored);
    EXPECT_TRUE(active > 50);
    EXPECT_TRUE(hit > 20);
    EXPECT_TRUE(colored > 20);
}
TEST_F(denoise_gi_spec, rough_metal_512spp, reflection_fixture) {
    add_sphere(f);
    g_mat[0].standard.roughness = .3f;
    for (u32 frame = 0; frame < 64; ++frame)
        reflection_frame(f);
    assert_reflected_coverage(f);
    f32 actual[PIXELS * 4];
    memcpy(actual, f->pixels, sizeof actual);
    EXPECT_EQ(f->info.samples, 1u);
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 512);
    reflection_frame(f);
    EXPECT_EQ(f->info.samples, 512u);
    printf("roughness .3, production 1spp denoised after64frames vs512spp\n");
    EXPECT_TRUE(compare_rgb(f->pixels, actual, 4, 1) <= .06);
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 0);
    (void)cvar_set("r.ambient_fallback", "0");
}
TEST_F(denoise_gi_spec, mirror_no_blur_512spp, reflection_fixture) {
    g_mat[0].standard.roughness = 0;
    for (u32 frame = 0; frame < 24; ++frame)
        reflection_frame(f);
    assert_reflected_coverage(f);
    f32 actual[PIXELS * 4];
    memcpy(actual, f->pixels, sizeof actual);
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 512);
    reflection_frame(f);
    EXPECT_EQ(f->info.samples, 512u);
    EXPECT_TRUE(compare_rgb(f->pixels, actual, 4, 1) <= .03);
}
static double colored_edge(const f32 *image) {
    /* Fixed middle row, physical red/green cube silhouette; no result-selected probe. */
    double weight = 0, moment = 0;
    u32 y = SH / 2;
    for (u32 x = 1; x < SW; ++x) {
        double delta = 0;
        for (u32 c = 0; c < 3; ++c)
            delta += fabs(image[(y * SW + x) * 4 + c] - image[(y * SW + x - 1) * 4 + c]);
        weight += delta;
        moment += delta * x;
    }
    printf("fixed row reflected edge strength %.9g\n", weight);
    ASSERT_TRUE(weight > .1);
    return moment / weight;
}
TEST_F(denoise_gi_spec, strafing_mirror_virtual_motion, reflection_fixture) {
    g_mat[0].standard.roughness = 0;
    // The fixed middle-row ray reaches the mirror near Y=1.2. The original cube at Y=0
    // ends before that point, so it cannot supply the reflected edge this test measures.
    // Put the same cube beyond that intersection; keep the probe, cameras and limits fixed.
    rt_fixture *r = &f->shadow.rt;
    for (u32 face = 0; face < 6; ++face) {
        de_affine xf = r->w.f.instances.transform[face + 1];
        xf.m[10] += 3;
        world_set(&r->w, face + 1, 2, xf, face + 2);
        r->w.f.instances.flags[face + 1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[face + 1] = 1u << 24;
    }
    camera_set(f, (de_vec3){3.3f, -6, 3}, (de_vec3){3.3f, 0, .5f});
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 512);
    reflection_frame(f);
    f32 reference[PIXELS * 4];
    memcpy(reference, f->pixels, sizeof reference);
    camera_set(f, (de_vec3){3, -6, 3}, (de_vec3){3, 0, .5f});
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 0);
    for (u32 frame = 0; frame < 24; ++frame)
        reflection_frame(f);
    assert_reflected_coverage(f);
    double old = colored_edge(f->pixels);
    camera_set(f, (de_vec3){3.3f, -6, 3}, (de_vec3){3.3f, 0, .5f});
    reflection_frame(f);
    reflection_frame(f);
    double target = colored_edge(reference), actual = colored_edge(f->pixels);
    printf("strafing mirror edge old%.9g target%.9g frame2%.9g\n", old, target, actual);
    EXPECT_TRUE(fabs(old - target) > .1);
    EXPECT_TRUE(fabs(actual - target) <= 2);
}
TEST_F(denoise_gi_spec, di_512_lights_reference, di_fixture) {
    rt_fixture *r = &f->shadow.rt;
    de__restir_di_debug(r->r->restir_di, &(de__di_debug){.reference = true});
    di_frame(f);
    memcpy(f->reference, f->current, sizeof f->reference);
    EXPECT_EQ(f->info.source_count, 512u);
    de__restir_di_debug(r->r->restir_di, NULL);
    for (u32 frame = 0; frame < 64; ++frame)
        di_frame(f);
    EXPECT_EQ(f->info.source_count, 512u);
    EXPECT_TRUE(compare_rgb(f->reference, f->current, 3, .05f) <= .06);
}
static void checker_texture(di_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    de_gpu_wait_idle(r->d);
    de_gpu_texture_destroy(r->d, r->texture);
    r->texture = de_gpu_texture_create(
        r->d, &(de_gpu_texture_desc){.width = 8,
                                     .height = 8,
                                     .depth_or_layers = 1,
                                     .mip_count = 1,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    de_gpu_buffer upload = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){
                  .size = 256 * 8, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *p = de_gpu_buffer_map(r->d, upload);
    memset(p, 0, 256 * 8);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            u8 value = (x + y) % 2 ? 230 : 35;
            for (u32 c = 0; c < 3; ++c)
                p[y * 256 + x * 4 + c] = value;
            p[y * 256 + x * 4 + 3] = 255;
        }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Denoiser checker albedo upload");
    de_gpu_barrier b = {.texture = r->texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, r->texture, 0, 0, upload, 0, 256);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    de_gpu_buffer_destroy(r->d, upload);
    g_mat[0].standard.base_color_tex = (de_texture){1};
    g_mat[0].textures[0] = (de_texture){1};
    r->w.touch_all = true;
}
TEST_F(denoise_gi_spec, checkerboard_demodulation_and_warm_guard, di_fixture) {
    checker_texture(f);
    rt_fixture *r = &f->shadow.rt;
    de__restir_di_debug(r->r->restir_di, &(de__di_debug){.reference = true});
    di_frame(f);
    memcpy(f->reference, f->current, sizeof f->reference);
    de__restir_di_debug(r->r->restir_di, NULL);
    for (u32 frame = 0; frame < 64; ++frame)
        di_frame(f);
    EXPECT_TRUE(compare_rgb(f->reference, f->current, 3, .05f) <= .06);
    /* Compare actual high-frequency checker gradients to the independent all-light image. */
    double expected = 0, actual = 0;
    for (u32 y = SH / 4; y < SH * 3 / 4; ++y)
        for (u32 x = 1; x < SW; ++x)
            for (u32 c = 0; c < 3; ++c) {
                u32 i = (y * SW + x) * 3 + c;
                expected += fabs(f->reference[i] - f->reference[i - 3]);
                actual += fabs(f->current[i] - f->current[i - 3]);
            }
    printf("checker gradient reference%.9g denoised%.9g ratio%.9g\n", expected, actual,
           actual / expected);
    EXPECT_TRUE(expected > 1);
    EXPECT_TRUE(actual / expected >= .8);
    EXPECT_TRUE(actual / expected <= 1.2);
    de_tracking_stats cpu = de_tracking_allocator_stats(&r->track),
                      gpu = de_tracking_allocator_stats(&r->dev_track);
    for (u32 frame = 0; frame < 8; ++frame)
        di_frame(f);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).alloc_count, cpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).realloc_count, cpu.realloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).alloc_count, gpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).realloc_count, gpu.realloc_count);
}
