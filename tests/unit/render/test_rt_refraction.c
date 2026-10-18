#define SW 128u
#define SH 128u
#include "rt_shadow_fixture.h"
#include "render/rt/rt_refraction.h"
typedef struct refraction_fixture {
    shadow_fixture shadow;
    de__rt_refraction_info info;
    de_gpu_buffer rb, sphere_indices;
    de_cooked_mesh_range sphere_range;
    de_render_submesh sphere_submesh;
} refraction_fixture;
static void refraction_fixture_setup(refraction_fixture *f) {
    shadow_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    de__view_unregister_stage(r->r, hdr_stage, &f->shadow);
    ASSERT_EQ(de__view_register_stage(r->r, DE__VIEW_SLOT_TRANSPARENT, 1, "Refraction final HDR",
                                      hdr_stage, &f->shadow),
              DE_OK);
    g_mat_count = 0;
    material_add(1, 0, 0, 0);
    material_add(1, 0, 0, 0);
    g_mat[0].standard.unlit = true;
    g_mat[0].standard.base_color = (de_vec4){.4f, .8f, .2f, 1};
    g_mat[1].standard.transmission = 1;
    g_mat[1].standard.ior = 1;
    g_mat[1].standard.thickness = .5f;
    g_mat[1].standard.attenuation_color = (de_vec3){.5f, .7f, .9f};
    g_mat[1].standard.attenuation_distance = 1;
    world_set(&r->w, 0, 1, trs((de_vec3){3, 0, -3}, (de_vec3){0, 0, 1}, 0, 4), 1);
    world_set(&r->w, 1, 2, at(3, 0, 1), 2);
    for (u32 i = 0; i < 2; ++i) {
        r->w.f.instances.flags[i] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[i] = 1u << 24;
    }
    r->w.f.lights.count = 0;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.shadows = settings.ao = settings.gi = DE_QUALITY_OFF;
    settings.reflections = DE_QUALITY_HIGH;
    settings.rt_shadows = settings.rt_ao = settings.rt_reflections = settings.rt_gi = false;
    settings.rt_refraction = true;
    de_renderer_set_settings(r->r, &settings);
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = (u64)SW * SH * 128,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id);
}
static void refraction_fixture_teardown(refraction_fixture *f) {
    if (f->shadow.rt.d) {
        de_gpu_wait_idle(f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->rb);
        if (f->sphere_indices.id) {
            mesh_free(&g_mesh[2], f->shadow.rt.d);
            de_gpu_buffer_destroy(f->shadow.rt.d, f->sphere_indices);
        }
    }
    shadow_fixture_teardown(&f->shadow);
}
static const u8 *refraction_frame(refraction_fixture *f) {
    shadow_frame(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    ASSERT_TRUE(de__rt_refraction_get(r->r->rt_refraction, r->view, &f->info));
    de_gpu_cmd *c = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Refraction actual batch readback");
    de_gpu_buffer buffers[] = {f->info.radiance, f->info.path, f->info.rays, f->info.hits,
                               f->info.diagnostic};
    u64 offsets[] = {0, SW * SH * 16, SW * SH * 32, SW * SH * 64, SW * SH * 96},
        bytes[] = {SW * SH * 16, SW * SH * 16, SW * SH * 32, SW * SH * 32, 64};
    for (u32 i = 0; i < 5; ++i) {
        de_gpu_barrier b = {
            .buffer = buffers[i], .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_cmd_copy_buffer(c, f->rb, offsets[i], buffers[i], 0, bytes[i]);
        b.before = DE_ACCESS_COPY_SRC;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(c, &b, 1);
    }
    ASSERT_EQ(
        de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->rb);
}
TEST_F(rt_refraction, actual_forward_beer_lambert, refraction_fixture) {
    const float thickness[] = {.2f, .5f, 1.f};
    double worst = 0;
    for (u32 test = 0; test < 3; ++test) {
        g_mat[1].standard.thickness = thickness[test];
        const u8 *raw = refraction_frame(f);
        const float *incident = (const float *)raw, *path = (const float *)(raw + SW * SH * 16);
        const u16 *hdr = de_gpu_buffer_map(f->shadow.rt.d, f->shadow.hdr_rb);
        u32 pixel = (SH / 2) * SW + SW / 2;
        printf("RT refraction thickness%g incident(%g,%g,%g) path%g valid%g events%g\n",
               thickness[test], incident[pixel * 4], incident[pixel * 4 + 1],
               incident[pixel * 4 + 2], path[pixel * 4], incident[pixel * 4 + 3],
               path[pixel * 4 + 2]);
        ASSERT_TRUE(incident[pixel * 4 + 3] > 0);
        EXPECT_TRUE(fabs(path[pixel * 4] - thickness[test]) < 1e-4);
        const double color[] = {.4, .8, .2}, absorption[] = {.5, .7, .9};
        for (u32 channel = 0; channel < 3; ++channel) {
            double expected = color[channel] * pow(absorption[channel], thickness[test]),
                   actual = half_to_f32(hdr[pixel * 4 + channel]) / f->shadow.pre_exposure;
            double error = fabs(actual - expected) / expected;
            if (error > worst)
                worst = error;
            EXPECT_TRUE(error <= .03);
        }
    }
    printf("Actual forward Beer-Lambert worst relative error %g%%\n", worst * 100);
}

static void add_sphere(refraction_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    const u32 cols = 32, rows = 24;
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
                de_vec3 pos = de_v3_scale(n, .75f);
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
        m->bounds_min[i] = -.75f;
        m->bounds_max[i] = .75f;
        m->sphere[i] = 0;
    }
    m->sphere[3] = .75f;
    ASSERT_EQ(de__rt_mesh_publish(r->r->rt_scene, m), DE_OK);
}

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
    return tan(55 * 3.14159265358979323846 / 360);
}
static ray_v camera_ray(u32 x, u32 y) {
    return rn((ray_v){(2 * ((double)x + .5) / SW - 1) * (double)SW / SH * ray_tan(),
                      (1 - 2 * ((double)y + .5) / SH) * ray_tan(), -1});
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

static void checker_texture(refraction_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    de_gpu_wait_idle(r->d);
    de_gpu_texture_destroy(r->d, r->texture);
    const u32 n = 512, pitch = n * 4;
    r->texture = de_gpu_texture_create(
        r->d, &(de_gpu_texture_desc){.width = n,
                                     .height = n,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    de_gpu_buffer upload = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){
                  .size = (u64)n * pitch, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *dst = de_gpu_buffer_map(r->d, upload);
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            double px = ((x + .5) / n - .5) * 8, py = (.5 - (y + .5) / n) * 8;
            int cell = (int)floor((px + .07) / .3) + (int)floor((py + .11) / .3);
            u8 value = (cell & 1) ? 204 : 26;
            dst[y * pitch + x * 4] = dst[y * pitch + x * 4 + 1] = dst[y * pitch + x * 4 + 2] =
                value;
            dst[y * pitch + x * 4 + 3] = 255;
        }
    de_gpu_cmd *c = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Actual checker texture");
    de_gpu_barrier b = {.texture = r->texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(c, r->texture, 0, 0, upload, 0, pitch);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    ASSERT_EQ(
        de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    de_gpu_buffer_destroy(r->d, upload);
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    g_mat[0].standard.base_color_tex = (de_texture){1};
    g_mat[0].textures[0] = (de_texture){1};
}
static void physical_sphere_scene(refraction_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    add_sphere(f);
    checker_texture(f);
    f->shadow.camera.camera.projection = DE_PROJ_PERSPECTIVE;
    f->shadow.camera.camera.fov_y = 55.f * DE_PI / 180;
    f->shadow.camera.transform = at(0, 0, 3);
    f->shadow.camera.offset = (de_vec3){0, 0, 3};
    f->shadow.camera.rotation = (de_quat){0, 0, 0, 1};
    world_set(&r->w, 0, 1, trs((de_vec3){0, 0, -3}, (de_vec3){0, 0, 1}, 0, 4), 1);
    world_set(&r->w, 1, 3, at(0, 0, 0), 2);
    for (u32 i = 0; i < 2; ++i) {
        r->w.f.instances.flags[i] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[i] = 1u << 24;
        ASSERT_TRUE((r->w.f.instances.flags[i] & DE__RW_INST_VISIBLE_IN_RT) != 0);
    }
    g_mat[1].standard.ior = 1.5f;
    g_mat[1].standard.thickness = 1.5f;
    g_mat[1].standard.attenuation_distance = 1e30f;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.reflections = DE_QUALITY_ULTRA;
    de_renderer_set_settings(r->r, &settings);
}
TEST_F(rt_refraction, sphere_checker_five_independent_probes, refraction_fixture) {
    physical_sphere_scene(f);
    const u8 *raw = refraction_frame(f);
    const float *incident = (const float *)raw, *path = (const float *)(raw + SW * SH * 16);
    const de__rt_ray *rays = (const de__rt_ray *)(raw + SW * SH * 32);
    const de__rt_hit *hits = (const de__rt_hit *)(raw + SW * SH * 64);
    // Fixed before GPU measurement: four off-axis inversion/magnification probes and optical axis.
    const u32 probes[5][2] = {{48, 64}, {80, 64}, {64, 48}, {64, 80}, {64, 64}};
    double worst = 0;
    for (u32 i = 0; i < 5; ++i) {
        u32 x = probes[i][0], y = probes[i][1], pixel = y * SW + x;
        ray_v entry, normal, expected;
        double distance;
        ASSERT_TRUE(sphere_hit(camera_ray(x, y), &entry, &normal, &expected, &distance));
        printf("sphere raw(%u,%u): valid%g path(%g,%g,%g,%g) ray(%g,%g,%g -> %g,%g,%g) hit %g "
               "instance%u\n",
               x, y, incident[pixel * 4 + 3], path[pixel * 4], path[pixel * 4 + 1],
               path[pixel * 4 + 2], path[pixel * 4 + 3], rays[pixel].origin[0],
               rays[pixel].origin[1], rays[pixel].origin[2], rays[pixel].direction[0],
               rays[pixel].direction[1], rays[pixel].direction[2], hits[pixel].t,
               hits[pixel].instance);
        ASSERT_TRUE(hits[pixel].t >= 0);
        de_vec3 hit = {rays[pixel].origin[0] + rays[pixel].direction[0] * hits[pixel].t,
                       rays[pixel].origin[1] + rays[pixel].direction[1] * hits[pixel].t,
                       rays[pixel].origin[2] + rays[pixel].direction[2] * hits[pixel].t};
        double pixel_scale = SW / (12 * ray_tan()),
               error = hypot(hit.x - expected.x, hit.y - expected.y) * pixel_scale;
        if (error > worst)
            worst = error;
        printf("sphere fixed probe(%u,%u): expected(%g,%g) actual(%g,%g) pixel error%g events%g\n",
               x, y, expected.x, expected.y, hit.x, hit.y, error, path[pixel * 4 + 2]);
        EXPECT_TRUE(error <= 1);
        EXPECT_TRUE(path[pixel * 4 + 2] == 2);
        int cell = (int)floor((expected.x + .07) / .3) + (int)floor((expected.y + .11) / .3);
        double color = (cell & 1) ? 204.0 / 255 : 26.0 / 255;
        EXPECT_TRUE(fabs(incident[pixel * 4] / f->shadow.pre_exposure - color) < .015);
        const u16 *hdr = de_gpu_buffer_map(f->shadow.rt.d, f->shadow.hdr_rb);
        EXPECT_TRUE(fabs(half_to_f32(hdr[pixel * 4]) / f->shadow.pre_exposure - color) < .015);
    }
    printf("Actual sphere five-probe maximum independent spatial error %g pixels\n", worst);
}
static void make_prism(refraction_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    fmesh *fm = &g_mesh[2];
    mesh_plane(fm, r->d, 5, 1, 1);
    g_mesh_count = 3;
    de_render_mesh *m = &fm->rm;
    u8 *vertices = de_gpu_buffer_map(r->d, fm->buf[0]),
       *records = de_gpu_buffer_map(r->d, fm->buf[1]);
    u32 indices[5 * PATCH_TRIS * 3], ni = 0;
    const de_vec3 cross_section[3] = {{-.5f, 0, 2.5f}, {.5f, 0, 2.5f}, {.5f, 0, 2.f}};
    for (u32 face = 0; face < 5; ++face) {
        de_vec3 normal = face == 0   ? (de_vec3){0, 0, 1}
                         : face == 1 ? (de_vec3){1, 0, 0}
                         : face == 2 ? de_v3_normalize((de_vec3){-.5f, 0, -1})
                         : face == 3 ? (de_vec3){0, -1, 0}
                                     : (de_vec3){0, 1, 0};
        float l = fabsf(normal.x) + fabsf(normal.y) + fabsf(normal.z), ox = normal.x / l,
              oy = normal.y / l;
        if (normal.z < 0) {
            float old = ox;
            ox = (1 - fabsf(oy)) * (old < 0 ? -1 : 1);
            oy = (1 - fabsf(old)) * (oy < 0 ? -1 : 1);
        }
        u32 packed = (u16)(i16)lrintf(ox * 32767) | ((u32)(u16)(i16)lrintf(oy * 32767) << 16);
        for (u32 y = 0; y < PATCH; ++y)
            for (u32 x = 0; x < PATCH; ++x) {
                float u = (float)x / (PATCH - 1), v = (float)y / (PATCH - 1);
                de_vec3 p;
                if (face < 3) {
                    p = de_v3_add(de_v3_scale(cross_section[face], 1 - u),
                                  de_v3_scale(cross_section[(face + 1) % 3], u));
                    p.y = -.8f + v * 1.6f;
                } else {
                    if (face == 3)
                        u = 1 - u;
                    p = de_v3_add(de_v3_scale(cross_section[0], (1 - u) * (1 - v)),
                                  de_v3_add(de_v3_scale(cross_section[1], u * (1 - v)),
                                            de_v3_scale(cross_section[2], v)));
                    p.y = face == 3 ? -.8f : .8f;
                }
                u32 vertex = face * PATCH_VERT + y * PATCH + x;
                memcpy(vertices + vertex * 12, &p, 12);
                put_u32(vertices + m->stream_offset[DE_COOKED_STREAM_NORMAL] + vertex * 4, packed);
            }
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 16, 0);
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 20, 0);
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 24, 2.25f);
        put_f32(records + face * DE_COOKED_MESHLET_SIZE + 28, 1.2f);
        for (u32 y = 0; y + 1 < PATCH; ++y)
            for (u32 x = 0; x + 1 < PATCH; ++x) {
                u32 a = face * PATCH_VERT + y * PATCH + x, b = a + 1, c = b + PATCH, d = a + PATCH;
                indices[ni++] = a;
                indices[ni++] = b;
                indices[ni++] = c;
                indices[ni++] = a;
                indices[ni++] = c;
                indices[ni++] = d;
            }
    }
    f->sphere_indices = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){.size = sizeof indices,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT,
                                    .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(r->d, f->sphere_indices), indices, sizeof indices);
    m->buffers[DE_RENDER_MESH_INDICES] = f->sphere_indices;
    m->bindless[DE_RENDER_MESH_INDICES] = de_gpu_buffer_bindless(r->d, f->sphere_indices);
    m->index_count = ni;
    m->lods[0].index_count = ni;
    f->sphere_range = (de_cooked_mesh_range){.index_count = ni};
    m->lod_submeshes = &f->sphere_range;
    m->submeshes = &f->sphere_submesh;
    m->bounds_min[0] = -.5f;
    m->bounds_max[0] = .5f;
    m->bounds_min[1] = -.8f;
    m->bounds_max[1] = .8f;
    m->bounds_min[2] = 2;
    m->bounds_max[2] = 2.5f;
    m->sphere[0] = m->sphere[1] = 0;
    m->sphere[2] = 2.25f;
    m->sphere[3] = 1.2f;
    ASSERT_EQ(de__rt_mesh_publish(r->r->rt_scene, m), DE_OK);
}
TEST_F(rt_refraction, hidden_object_through_physical_prism, refraction_fixture) {
    rt_fixture *r = &f->shadow.rt;
    make_prism(f);
    g_mat[0].standard.base_color = (de_vec4){0, 0, 0, 1};
    g_mat[1].standard.ior = 1.5f;
    g_mat[1].standard.thickness = .5f;
    g_mat[1].standard.attenuation_distance = 1e30f;
    u32 wall = material_add(0, 0, 0, 0), red = material_add(1, 0, 0, 0);
    g_mat[wall - 1].standard.unlit = true;
    g_mat[red - 1].standard.unlit = true;
    g_mat[red - 1].standard.base_color = (de_vec4){1, 0, 0, 1};
    world_set(&r->w, 0, 1, trs((de_vec3){3, 0, -4}, (de_vec3){0, 0, 1}, 0, 4), 1);
    world_set(&r->w, 1, 3, at(3, 0, 0), 2);
    world_set(&r->w, 2, 1, trs((de_vec3){4.5f, 0, 1}, (de_vec3){0, 0, 1}, 0, .5f), wall);
    world_set(&r->w, 3, 1, trs((de_vec3){4.45f, 0, -3}, (de_vec3){0, 0, 1}, 0, .3f), red);
    for (u32 i = 0; i < 4; ++i) {
        r->w.f.instances.flags[i] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[i] = 1u << 24;
    }
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.reflections = DE_QUALITY_ULTRA;
    de_renderer_set_settings(r->r, &settings);
    const u8 *raw = refraction_frame(f);
    u32 pixel = (SH / 2) * SW + SW / 2;
    const de__rt_hit *hits = (const de__rt_hit *)(raw + SW * SH * 64);
    const float *path = (const float *)(raw + SW * SH * 16);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    double rt = half_to_f32(hdr[pixel * 4]) / f->shadow.pre_exposure;
    printf("prism RT red=%g hit instance%u events%g\n", rt, hits[pixel].instance,
           path[pixel * 4 + 2]);
    EXPECT_EQ(hits[pixel].instance, 3u);
    EXPECT_TRUE(rt > .5);
    EXPECT_TRUE(path[pixel * 4 + 2] == 2);
    // The target's entire orthographic projection lies inside the opaque wall, independently
    // of the refracted hit. Check its direct camera pixels remain black as well.
    for (u32 x = 101; x <= 119; ++x)
        EXPECT_TRUE(half_to_f32(hdr[((SH / 2) * SW + x) * 4]) / f->shadow.pre_exposure < .05);
    settings.rt_refraction = false;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    double ss = half_to_f32(hdr[pixel * 4]) / f->shadow.pre_exposure;
    printf("prism actual SS fallback red=%g\n", ss);
    EXPECT_TRUE(ss < .05);
}
static bool cpu_snell(ray_v incoming, ray_v normal, double ratio, ray_v *out) {
    double c = -rd(incoming, normal), k = 1 - ratio * ratio * (1 - c * c);
    if (k < 0)
        return false;
    *out = ra(rm(incoming, ratio), rm(normal, ratio * c - sqrt(k)));
    return true;
}
TEST(rt_refraction, cpu_snell_tir_mirror) {
    ray_v out;
    EXPECT_TRUE(cpu_snell((ray_v){0, 0, -1}, (ray_v){0, 0, 1}, 1 / 1.5, &out));
    EXPECT_TRUE(fabs(out.z + 1) < 1e-12);
    EXPECT_TRUE(cpu_snell((ray_v){.5, 0, -sqrt(.75)}, (ray_v){0, 0, 1}, 1 / 1.5, &out));
    EXPECT_TRUE(fabs(out.x - 1.0 / 3) < 1e-12);
    EXPECT_FALSE(cpu_snell((ray_v){sqrt(.75), 0, -.5}, (ray_v){0, 0, 1}, 1.5, &out));
    ray_v reflected = ra((ray_v){0, 0, -1}, rm((ray_v){-sqrt(.75), 0, .5}, 1));
    EXPECT_TRUE(fabs(rd(reflected, reflected) - 1) < 1e-12);
}
TEST_F(rt_refraction, actual_inside_tir, refraction_fixture) {
    rt_fixture *r = &f->shadow.rt;
    g_mat[1].standard.ior = 1.5f;
    g_mat[1].standard.thickness = .2f;
    g_mat[1].standard.double_sided = true;
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, 1}, (de_vec3){0, 1, 0}, 2 * DE_PI / 3, 1), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.5"));
    const u8 *raw = refraction_frame(f);
    const float *diagnostic = (const float *)(raw + SW * SH * 96);
    ray_v actual = {diagnostic[0], diagnostic[1], diagnostic[2]}, expected = {-sqrt(.75), 0, -.5};
    double dot = rd(actual, expected);
    printf("actual inside TIR dot=%g TIR=%g events=%g\n", dot, diagnostic[5], diagnostic[6]);
    EXPECT_TRUE(dot > .999);
    EXPECT_TRUE(diagnostic[5] == 1);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    double value = half_to_f32(hdr[((SH / 2) * SW + SW / 2) * 4]) / f->shadow.pre_exposure;
    EXPECT_TRUE(fabs(value - .5) < .005);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
}
TEST_F(rt_refraction, rough_transmission_and_thin_volume, refraction_fixture) {
    rt_fixture *r = &f->shadow.rt;
    g_mat[1].standard.ior = 1.5f;
    g_mat[1].standard.thickness = .2f;
    g_mat[1].standard.roughness = .4f;
    g_mat[1].standard.attenuation_distance = 1e30f;
    double angular = 0, energy_sum = 0;
    u32 pixel = (SH / 2) * SW + SW / 2;
    for (u32 frame = 0; frame < 32; ++frame) {
        const u8 *raw = refraction_frame(f);
        const de__rt_ray *rays = (const de__rt_ray *)(raw + SW * SH * 32);
        double angle = acos(fmax(-1, fmin(1, -rays[pixel].direction[2])));
        angular += angle * angle;
        const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
        double value = half_to_f32(hdr[pixel * 4]) / f->shadow.pre_exposure;
        EXPECT_TRUE(isfinite(value));
        energy_sum += value;
    }
    printf("Actual GGX transmission RMS angle=%g rad mean shaded red=%g\n", sqrt(angular / 32),
           energy_sum / 32);
    EXPECT_TRUE(sqrt(angular / 32) > .05);
    EXPECT_TRUE(energy_sum > 0);
    g_mat[1].standard.roughness = 0;
    g_mat[1].standard.thickness = 0;
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, 1}, (de_vec3){0, 1, 0}, .6f, 1), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    const u8 *raw = refraction_frame(f);
    const float *path = (const float *)(raw + SW * SH * 16);
    const de__rt_ray *rays = (const de__rt_ray *)(raw + SW * SH * 32);
    EXPECT_TRUE(path[pixel * 4] == 0);
    EXPECT_TRUE(rays[pixel].direction[2] < -.9999f);
    g_mat[1].standard.thickness = .5f;
    raw = refraction_frame(f);
    path = (const float *)(raw + SW * SH * 16);
    rays = (const de__rt_ray *)(raw + SW * SH * 32);
    EXPECT_TRUE(path[pixel * 4] > .5f);
    EXPECT_TRUE(fabs(rays[pixel].direction[0]) > .1f);
    EXPECT_TRUE(path[pixel * 4 + 2] == 1);
}
TEST_F(rt_refraction, ultra_exit_vs_single_event, refraction_fixture) {
    physical_sphere_scene(f);
    const u8 *raw = refraction_frame(f);
    const float *path = (const float *)(raw + SW * SH * 16);
    u32 pixel = 64 * SW + 48;
    const de__rt_ray *rays = (const de__rt_ray *)(raw + SW * SH * 32);
    de_vec3 ultra = {rays[pixel].direction[0], rays[pixel].direction[1], rays[pixel].direction[2]};
    EXPECT_TRUE(path[pixel * 4 + 2] == 2);
    de_render_settings settings = de_renderer_get_settings(f->shadow.rt.r);
    settings.reflections = DE_QUALITY_HIGH;
    de_renderer_set_settings(f->shadow.rt.r, &settings);
    raw = refraction_frame(f);
    path = (const float *)(raw + SW * SH * 16);
    rays = (const de__rt_ray *)(raw + SW * SH * 32);
    EXPECT_TRUE(path[pixel * 4 + 2] == 1);
    ray_v entry, normal, exit;
    double distance;
    ASSERT_TRUE(sphere_hit(camera_ray(48, 64), &entry, &normal, &exit, &distance));
    ray_v expected = rbend(camera_ray(48, 64), normal, 1 / 1.5);
    double dot = rays[pixel].direction[0] * expected.x + rays[pixel].direction[1] * expected.y +
                 rays[pixel].direction[2] * expected.z;
    EXPECT_TRUE(dot > .999);
    double change = acos(
        fmax(-1, fmin(1, ultra.x * rays[pixel].direction[0] + ultra.y * rays[pixel].direction[1] +
                             ultra.z * rays[pixel].direction[2])));
    printf("Actual Ultra exit versus lower single event direction change=%g rad\n", change);
    EXPECT_TRUE(change > .01);
}
TEST_F(rt_refraction, alpha_aware_closest_hits, refraction_fixture) {
    rt_fixture *r = &f->shadow.rt;
    g_mat[0].standard.base_color = (de_vec4){.1f, .8f, .1f, 1};
    g_mat[1].standard.attenuation_distance = 1e30f;
    u32 mask = material_add(1, 0, 0, 0);
    g_mat[mask - 1].standard.unlit = true;
    g_mat[mask - 1].standard.base_color = (de_vec4){1, 0, 0, 1};
    g_mat[mask - 1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[mask - 1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[mask - 1].standard.alpha_cutoff = .5f;
    g_mat[mask - 1].standard.base_color_tex = (de_texture){1};
    g_mat[mask - 1].textures[0] = (de_texture){1};
    world_set(&r->w, 2, 2, at(3, 0, -1), mask);
    r->w.f.instances.flags[2] |= DE__RW_INST_VISIBLE_IN_RT;
    r->w.f.instances.layer_mask[2] = 1u << 24;
    const u8 *raw = refraction_frame(f);
    const de__rt_hit *hits = (const de__rt_hit *)(raw + SW * SH * 64);
    u32 left = 64 * SW + 48, right = 64 * SW + 80;
    EXPECT_EQ(hits[left].instance, 0u);
    EXPECT_EQ(hits[right].instance, 2u);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    EXPECT_TRUE(half_to_f32(hdr[left * 4 + 1]) / f->shadow.pre_exposure > .7);
    EXPECT_TRUE(half_to_f32(hdr[right * 4]) / f->shadow.pre_exposure > .9);
}
TEST_F(rt_refraction, runtime_fallback_and_warmed_allocations, refraction_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 i = 0; i < 24; ++i)
        refraction_frame(f);
    de_tracking_stats cpu = de_tracking_allocator_stats(&r->track),
                      gpu = de_tracking_allocator_stats(&r->dev_track);
    for (u32 i = 0; i < 8; ++i)
        refraction_frame(f);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).alloc_count, cpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).realloc_count, cpu.realloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).alloc_count, gpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).realloc_count, gpu.realloc_count);
    u32 pixel = 64 * SW + 64;
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    double rt = half_to_f32(hdr[pixel * 4]) / f->shadow.pre_exposure;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_refraction = false;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_refraction_get(r->r->rt_refraction, r->view, &f->info));
    hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    double ss = half_to_f32(hdr[pixel * 4]) / f->shadow.pre_exposure;
    EXPECT_TRUE(fabs(rt - ss) < .005 && ss > 0);
    settings.tier = DE_TIER_BASE;
    settings.rt_refraction = true;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_refraction_get(r->r->rt_refraction, r->view, &f->info));
    ASSERT_TRUE(cvar_set("r.refraction", "0"));
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_refraction_get(r->r->rt_refraction, r->view, &f->info));
    ASSERT_TRUE(cvar_set("r.refraction", "1"));
}
