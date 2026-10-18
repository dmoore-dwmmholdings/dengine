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
TEST_F(rt_reflections, mirror_readback, reflection_fixture) {
    reflection_frame(f);
    u32 valid = 0, colored = 0;
    double sum = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        float *p = f->pixels + i * 4;
        ASSERT_TRUE(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]));
        if (p[3] > .9f) {
            ++valid;
            sum += p[0] + p[1] + p[2];
            if (fmaxf(p[0], fmaxf(p[1], p[2])) - fminf(p[0], fminf(p[1], p[2])) > .1f)
                ++colored;
        }
    }
    const u8 *debug = de_gpu_buffer_map(f->shadow.rt.d, f->rb);
    const de__rt_ray *rays = (const de__rt_ray *)(debug + SW * SH * 8);
    const de__rt_hit *hits = (const de__rt_hit *)(debug + SW * SH * 40);
    u32 hit_count = 0, active = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        active += rays[i].direction[3] > 0;
        if (!(rays[i].direction[3] > rays[i].origin[3]) || !(rays[i].direction[3] > 0)) {
            EXPECT_TRUE(hits[i].t == -1.f);
            EXPECT_EQ(hits[i].instance, UINT32_MAX);
            EXPECT_EQ(hits[i].primitive, 0u);
            EXPECT_EQ(hits[i].lod, 0u);
        }
        if (hits[i].t >= 0) {
            const u16 *distance = (const u16 *)(debug + SW * SH * 72) + i * 4;
            EXPECT_TRUE(fabsf(half_to_f32(distance[0]) - hits[i].t) < .001f * fmaxf(hits[i].t, 1));
            ++hit_count;
            if (hit_count == 1)
                printf("firsthit %u t %g instance %u, origin %g %g %g dir %g %g %g\n", i, hits[i].t,
                       hits[i].instance, rays[i].origin[0], rays[i].origin[1], rays[i].origin[2],
                       rays[i].direction[0], rays[i].direction[1], rays[i].direction[2]);
        }
    }
    EXPECT_TRUE(active < SW * SH);
    printf("active rays %u hits %u; inactive records are canonical misses\n", active, hit_count);
    printf("RT reflection native mirror: %u valid, %u colored, sum %g\n", valid, colored, sum);
    EXPECT_TRUE(valid > 100);
    EXPECT_TRUE(colored > 20);
}

static u8 display(float value) {
    value = fmaxf(0, value);
    return (u8)lrintf(
        fminf(1, value <= .0031308f ? 12.92f * value : 1.055f * powf(value, 1 / 2.4f) - .055f) *
        255);
}
TEST_F(rt_reflections, mirror_colored_cube_direct_view_flip, reflection_fixture) {
    reflection_frame(f);
    rt_fixture *r = &f->shadow.rt;
    f32 actual[SW * SH * 4];
    memcpy(actual, f->pixels, sizeof actual);
    const u8 *data = de_gpu_buffer_map(r->d, f->rb);
    de__rt_ray rays[SW * SH];
    de__rt_hit hits[SW * SH];
    memcpy(rays, data + SW * SH * 8, sizeof rays);
    memcpy(hits, data + SW * SH * 40, sizeof hits);
    // A real raster direct view from the analytically reflected camera, without the mirror.
    r->w.f.instances.entity[0] = 0;
    r->w.touch_all = true;
    camera_up(f, (de_vec3){3, -6, -3}, (de_vec3){3, 0, -.5f}, (de_vec3){0, 0, -1});
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_reflections = false;
    settings.reflections = DE_QUALITY_OFF;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    const u16 *direct = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    de_mat4 cam = de_affine_to_m4(&f->shadow.camera.transform), view = de_m4_inverse_affine(&cam),
            proj = de_m4_perspective_rz(1, 1, .1f), vp = de_m4_mul(&proj, &view);
    u8 expected[SW * SH * 3] = {0}, measured[SW * SH * 3] = {0}, mask[SW * SH] = {0};
    f32 errors[SW * SH], selected[SW * SH];
    u32 count = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        if (hits[i].t < 0 || actual[i * 4 + 3] < .99f)
            continue;
        de_vec3 point = {rays[i].origin[0] + rays[i].direction[0] * hits[i].t,
                         rays[i].origin[1] + rays[i].direction[1] * hits[i].t,
                         rays[i].origin[2] + rays[i].direction[2] * hits[i].t};
        de_vec4 clip = de_m4_mul_v4(&vp, (de_vec4){point.x, point.y, point.z, 1});
        float projected_x = (clip.x / clip.w * .5f + .5f) * SW,
              projected_y = (.5f - clip.y / clip.w * .5f) * SH;
        // Reflection reverses handedness: the independently mirrored camera flips X only.
        float expected_x = (f32)(SW - 1 - i % SW) + .5f, expected_y = (f32)(i / SW) + .5f;
        EXPECT_TRUE(fabsf(projected_x - expected_x) < .05f);
        EXPECT_TRUE(fabsf(projected_y - expected_y) < .05f);
        int x = (int)(SW - 1 - i % SW), y = (int)(i / SW);
        if (x < 1 || y < 1 || x >= (int)SW - 1 || y >= (int)SH - 1)
            continue;
        bool interior = true;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                for (u32 c = 0; c < 3; ++c)
                    if (fabsf(half_to_f32(direct[((y + dy) * SW + x + dx) * 4 + c]) -
                              half_to_f32(direct[(y * SW + x) * 4 + c])) > .01f)
                        interior = false;
        if (!interior)
            continue;
        mask[i] = 1;
        ++count;
        for (u32 c = 0; c < 3; ++c) {
            expected[i * 3 + c] =
                display(half_to_f32(direct[(y * SW + x) * 4 + c]) / f->shadow.pre_exposure);
            measured[i * 3 + c] = display(actual[i * 4 + c]);
        }
    }
    de_flip_image ref = {expected, SW, SH, 3, 0}, image = {measured, SW, SH, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&ref, &image, 0, errors));
    u32 n = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        if (mask[i])
            selected[n++] = errors[i];
    de_flip_stats stats;
    ASSERT_TRUE(count > 80);
    ASSERT_TRUE(de_flip_stats_compute(selected, n, &stats));
    printf("Actual mirrored-camera cube reference: %u interior pixels, FLIP mean %.8g p99 %.8g\n",
           count, stats.mean, (double)stats.p99);
    EXPECT_TRUE(stats.mean < .05);
}
TEST_F(rt_reflections, runtime_fallback_and_warmed_allocations, reflection_fixture) {
    for (u32 i = 0; i < 24; ++i)
        reflection_frame(f);
    rt_fixture *r = &f->shadow.rt;
    de_tracking_stats cpu = de_tracking_allocator_stats(&r->track),
                      gpu = de_tracking_allocator_stats(&r->dev_track);
    for (u32 i = 0; i < 8; ++i)
        reflection_frame(f);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).alloc_count, cpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).realloc_count, cpu.realloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).alloc_count, gpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).realloc_count, gpu.realloc_count);
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_reflections = false;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_reflections_get(r->r->rt_reflections, r->view, &f->info));
    settings.rt_reflections = true;
    settings.reflections = DE_QUALITY_LOW;
    de_renderer_set_settings(r->r, &settings);
    reflection_frame(f);
    EXPECT_EQ(f->info.scale, 2u);
    settings.tier = DE_TIER_BASE;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_reflections_get(r->r->rt_reflections, r->view, &f->info));
}

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
TEST_F(rt_reflections, rough_metal_sphere_multisample_reference, reflection_fixture) {
    add_sphere(f);
    double accumulated[SW * SH * 3] = {0};
    const u32 frames = 64;
    for (u32 frame = 0; frame < frames; ++frame) {
        reflection_frame(f);
        for (u32 i = 0; i < SW * SH; ++i)
            for (u32 c = 0; c < 3; ++c)
                accumulated[i * 3 + c] += f->pixels[i * 4 + c] / frames;
    }
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 1024);
    reflection_frame(f);
    double expected = 0, measured = 0, absolute = 0;
    u32 count = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        if (f->pixels[i * 4 + 3] > .99f) {
            ++count;
            for (u32 c = 0; c < 3; ++c) {
                expected += f->pixels[i * 4 + c];
                measured += accumulated[i * 3 + c];
                absolute += fabs(accumulated[i * 3 + c] - f->pixels[i * 4 + c]);
            }
        }
    double error = fabs(measured - expected) / expected;
    printf("Rough metal sphere: %u pixels, %u accumulated frames vs 1024 spp Hammersley, mean "
           "relative error %.8g; normalized pixel L1 %.8g\n",
           count, frames, error, absolute / expected);
    ASSERT_TRUE(count > 50 && expected > 1);
    EXPECT_TRUE(error <= .03);
    EXPECT_TRUE(absolute / expected <= .03);
    de__rt_reflections_reference(f->shadow.rt.r->rt_reflections, 0);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
}
TEST_F(rt_reflections, offscreen_geometry_and_alpha_cutouts, reflection_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 i = 1; i < 7; ++i) {
        r->w.f.instances.transform[i].m[10] += 2;
        r->w.f.instances.transform[i].m[11] += 7;
        r->w.f.instances.prev_transform[i] = r->w.f.instances.transform[i];
        r->w.f.instances.bounds[i] = de_aabb_transform((de_aabb){{-1, -1, -.01f}, {1, 1, .01f}},
                                                       &r->w.f.instances.transform[i]);
    }
    r->w.touch_all = true;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_reflections = false;
    shadow_frame(&f->shadow);
    // Disable the RT producer for a real direct raster view: no colored cube is in the camera.
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    u32 raster_colored = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        f32 a = half_to_f32(hdr[i * 4]), b = half_to_f32(hdr[i * 4 + 1]),
            c = half_to_f32(hdr[i * 4 + 2]);
        raster_colored += fmaxf(a, fmaxf(b, c)) - fminf(a, fminf(b, c)) > .1f;
    }
    EXPECT_EQ(raster_colored, 0u);
    settings.rt_reflections = true;
    de_renderer_set_settings(r->r, &settings);
    reflection_frame(f);
    u32 reflected = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        f32 *p = f->pixels + i * 4;
        reflected += fmaxf(p[0], fmaxf(p[1], p[2])) - fminf(p[0], fminf(p[1], p[2])) > .1f;
    }
    printf("Offscreen colored geometry: raster %u, reflected %u pixels\n", raster_colored,
           reflected);
    EXPECT_TRUE(reflected > 10);
}
TEST_F(rt_reflections, alpha_closest_hit_material_texture, reflection_fixture) {
    reflection_frame(f);
    rt_fixture *r = &f->shadow.rt;
    const u8 *data = de_gpu_buffer_map(r->d, f->rb);
    de__rt_hit opaque[SW * SH];
    memcpy(opaque, data + SW * SH * 40, sizeof opaque);
    for (u32 i = 1; i < 7; ++i) {
        g_mat[i].alpha_mode = DE_COOKED_ALPHA_MASK;
        g_mat[i].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
        g_mat[i].standard.alpha_cutoff = .5f;
        g_mat[i].textures[0] = (de_texture){1};
        g_mat[i].standard.base_color_tex = (de_texture){1};
    }
    r->w.touch_all = true;
    reflection_frame(f);
    data = de_gpu_buffer_map(r->d, f->rb);
    const de__rt_hit *masked = (const de__rt_hit *)(data + SW * SH * 40);
    u32 holes = 0, retained = 0, deeper = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        if (opaque[i].t >= 0) {
            holes += masked[i].t < 0;
            retained += masked[i].t >= 0;
            deeper += masked[i].t > opaque[i].t + .01f;
        }
    printf("Reflection texture alpha: %u holes, %u retained, %u deeper closest hits\n", holes,
           retained, deeper);
    EXPECT_TRUE(holes > 10);
    EXPECT_TRUE(retained > 10);
    EXPECT_TRUE(deeper > 10);
    for (u32 i = 1; i < 7; ++i)
        g_mat[i].standard.base_color.w = 0;
    r->w.touch_all = true;
    reflection_frame(f);
    data = de_gpu_buffer_map(r->d, f->rb);
    masked = (const de__rt_hit *)(data + SW * SH * 40);
    for (u32 i = 0; i < SW * SH; ++i)
        if (opaque[i].t >= 0)
            EXPECT_TRUE(masked[i].t < 0);
}
TEST_F(rt_reflections, secondary_local_light_rt_visibility, reflection_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 i = 1; i < 7; ++i) {
        g_mat[i].standard.unlit = false;
        g_mat[i].standard.base_color = (de_vec4){1, 1, 1, 1};
    }
    r->w.touch_all = true;
    world_light(&r->w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 10,
                           .cast_shadows = true},
                at(3, -4, 2));
    world_set(&r->w, 7, 2, trs((de_vec3){3, -2.5f, 1.5f}, (de_vec3){1, 0, 0}, DE_PI / 2, .6f), 2);
    r->w.f.instances.flags[7] |= DE__RW_INST_VISIBLE_IN_RT;
    r->w.f.instances.layer_mask[7] = 1u << 24;
    reflection_frame(f);
    f32 lit[SW * SH * 4];
    memcpy(lit, f->pixels, sizeof lit);
    const u8 *data = de_gpu_buffer_map(r->d, f->rb);
    de__rt_hit hits[SW * SH];
    memcpy(hits, data + SW * SH * 40, sizeof hits);
    r->w.f.instances.flags[7] |= DE__RW_INST_CAST_SHADOWS;
    r->w.touch_all = true;
    reflection_frame(f);
    double before = 0, after = 0;
    u32 count = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        if (hits[i].instance == 4 && hits[i].t >= 0) {
            ++count;
            before += lit[i * 4] + lit[i * 4 + 1] + lit[i * 4 + 2];
            after += f->pixels[i * 4] + f->pixels[i * 4 + 1] + f->pixels[i * 4 + 2];
        }
    printf(
        "Secondary local RT visibility: %u reflected front-face pixels, noncaster %g, caster %g\n",
        count, before, after);
    ASSERT_TRUE(count > 20 && before > 1);
    EXPECT_TRUE(after < before * .9);
}
TEST_F(rt_reflections, independent_registered_bsdf_weight_contract, reflection_fixture) {
    rt_fixture *r = &f->shadow.rt;
    const char *entry = "cs_weight_contract";
    de__engine_shader_code code;
    char why[512] = {0};
    ASSERT_EQ(de__engine_shader_load(r->d, &r->alloc, "rt_reflections", 0, &entry, 1, &code, why,
                                     sizeof why),
              DE_OK);
    de_gpu_pipeline pipe =
        de_gpu_pipeline_create(r->d, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                             .cs = code.entries[0].blob,
                                                             .push_constant_bytes = 16,
                                                             .threadgroup_size = {64, 1, 1}});
    de__engine_shader_code_free(&code);
    ASSERT_TRUE(pipe.id);
    const u32 count = 1024;
    de_gpu_buffer output = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){
                  .size = count * 16, .stride = 16, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(output.id);
    de_gpu_cmd *cmd =
        de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Registered material throughput oracle");
    u32 push[] = {de_gpu_buffer_bindless(r->d, output), count, 0, 0};
    de_gpu_cmd_set_pipeline(cmd, pipe);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    de_gpu_cmd_dispatch(cmd, count / 64, 1, 1);
    de_gpu_barrier barrier = {
        .buffer = output, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 0, output, 0, count * 16);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    const f32 *values = de_gpu_buffer_map(r->d, f->rb);
    u32 oblique = 0;
    double maximum = 0;
    for (u32 i = 0; i < count; ++i)
        if (values[i * 4 + 3] > .1f && values[i * 4 + 3] < .8f) {
            ++oblique;
            for (u32 c = 0; c < 3; ++c) {
                EXPECT_TRUE(isfinite(values[i * 4 + c]));
                maximum = fmax(maximum, values[i * 4 + c]);
            }
        }
    printf("Registered BSDF sample/pdf oracle: %u oblique directions, maximum relative throughput "
           "difference %.8g\n",
           oblique, maximum);
    EXPECT_TRUE(oblique > 100);
    EXPECT_TRUE(maximum < 1e-4);
    de_gpu_pipeline_destroy(r->d, pipe);
    de_gpu_buffer_destroy(r->d, output);
}
static void material_texture(reflection_fixture *f, bool gradient) {
    rt_fixture *r = &f->shadow.rt;
    de_gpu_texture_destroy(r->d, r->texture);
    r->texture = de_gpu_texture_create(
        r->d, &(de_gpu_texture_desc){.width = 256,
                                     .height = 1,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    de_gpu_buffer upload = de_gpu_buffer_create(
        r->d,
        &(de_gpu_buffer_desc){.size = 1024, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *data = de_gpu_buffer_map(r->d, upload);
    for (u32 i = 0; i < 256; ++i) {
        data[i * 4] = gradient ? 255 : 204;
        data[i * 4 + 1] = gradient ? (u8)lrintf(255 * (.4f + .4f * i / 255)) : 255;
        data[i * 4 + 2] = data[i * 4 + 3] = 255;
    }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Material acceptance texture");
    de_gpu_barrier b = {.texture = r->texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, r->texture, 0, 0, upload, 0, 1024);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_buffer_destroy(r->d, upload);
    r->w.touch_all = true;
}
TEST_F(rt_reflections, roughness_gradient_probe_fallback, reflection_fixture) {
    material_texture(f, true);
    rt_fixture *r = &f->shadow.rt;
    g_mat[0].standard.roughness = 1;
    g_mat[0].standard.metal_rough_tex = (de_texture){1};
    g_mat[0].textures[DE_COOKED_TEX_METAL_ROUGH] = (de_texture){1};
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.2"));
    reflection_frame(f);
    f32 alpha[SW * SH], hdr[SW * SH * 3];
    const u16 *data = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    u32 partial = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        alpha[i] = f->pixels[i * 4 + 3];
        partial += alpha[i] > .01f && alpha[i] < .99f;
        for (u32 c = 0; c < 3; ++c)
            hdr[i * 3 + c] = half_to_f32(data[i * 4 + c]);
    }
    for (u32 x = 1; x < SW; ++x)
        if (alpha[55 * SW + x - 1] > 0 && alpha[55 * SW + x] > 0)
            EXPECT_TRUE(fabsf(alpha[55 * SW + x] - alpha[55 * SW + x - 1]) < .2f);
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_reflections = false;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    data = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    u32 fallback = 0;
    double max_difference = 0;
    for (u32 i = SW * SH * 3 / 4; i < SW * SH; ++i)
        if (alpha[i] == 0 && hdr[i * 3] + hdr[i * 3 + 1] + hdr[i * 3 + 2] > .02f) {
            ++fallback;
            for (u32 c = 0; c < 3; ++c)
                max_difference =
                    fmax(max_difference, fabs(hdr[i * 3 + c] - half_to_f32(data[i * 4 + c])));
        }
    printf("Textured roughness gradient: %u partial confidence pixels, %u probe fallback pixels, "
           "max on/off HDR difference %.8g\n",
           partial, fallback, max_difference);
    EXPECT_TRUE(partial > 100);
    EXPECT_TRUE(fallback > 100);
    EXPECT_TRUE(max_difference < .002);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
}
static f32 cpu_specular_ao(f32 nov, f32 ao, f32 roughness) {
    return fminf(1, fmaxf(0, powf(fmaxf(nov + ao, 0), exp2f(-16 * roughness - 1)) - 1 + ao));
}
TEST_F(rt_reflections, material_ao_and_rtao_once_only_composition, reflection_fixture) {
    reflection_frame(f);
    rt_fixture *r = &f->shadow.rt;
    f32 unit[SW * SH * 4];
    memcpy(unit, f->pixels, sizeof unit);
    const u8 *data = de_gpu_buffer_map(r->d, f->rb);
    de__rt_ray rays[SW * SH];
    de__rt_hit hits[SW * SH];
    memcpy(rays, data + SW * SH * 8, sizeof rays);
    memcpy(hits, data + SW * SH * 40, sizeof hits);
    material_texture(f, false);
    g_mat[0].standard.occlusion_tex = (de_texture){1};
    g_mat[0].textures[DE_COOKED_TEX_OCCLUSION] = (de_texture){1};
    reflection_frame(f);
    f32 material[SW * SH * 4], hdr_off[SW * SH * 3];
    memcpy(material, f->pixels, sizeof material);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    for (u32 i = 0; i < SW * SH; ++i)
        for (u32 c = 0; c < 3; ++c)
            hdr_off[i * 3 + c] = half_to_f32(hdr[i * 4 + c]);
    double material_error = 0, material_energy = 0;
    u32 count = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        if (hits[i].t >= 0 && unit[i * 4 + 3] > .99f) {
            ++count;
            de_vec3 delta = {3 - rays[i].origin[0], -6 - rays[i].origin[1], 3};
            f32 nov = delta.z / de_v3_length(delta), ao = cpu_specular_ao(nov, .8f, .01f);
            for (u32 c = 0; c < 3; ++c) {
                f32 expected = unit[i * 4 + c] * ao;
                material_energy += expected;
                material_error += fabs(material[i * 4 + c] - expected);
            }
        }
    ASSERT_TRUE(count > 100 && material_energy > 1);
    EXPECT_TRUE(material_error / material_energy < .005);
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.ao = DE_QUALITY_HIGH;
    settings.rt_ao = true;
    de_renderer_set_settings(r->r, &settings);
    ASSERT_TRUE(cvar_set("r.rt.ao.radius", "1.5"));
    reflection_frame(f);
    de__rtao_info ao;
    ASSERT_TRUE(de__rtao_get(r->r->rtao, r->view, &ao));
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "AO composition readback");
    de_gpu_barrier b = {
        .texture = ao.ao, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, SW * SH * 72, 256, ao.ao, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    data = de_gpu_buffer_map(r->d, f->rb);
    hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    double error = 0, energy = 0;
    u32 affected = 0;
    for (u32 i = 0; i < SW * SH; ++i)
        if (hits[i].t >= 0 && unit[i * 4 + 3] > .99f) {
            f32 raw = data[SW * SH * 72 + (i / SW) * 256 + i % SW] / 255.f;
            affected += raw < .79f;
            de_vec3 delta = {3 - rays[i].origin[0], -6 - rays[i].origin[1], 3};
            f32 nov = delta.z / de_v3_length(delta), old = cpu_specular_ao(nov, .8f, .01f),
                combined = cpu_specular_ao(nov, fminf(.8f, raw), .01f);
            for (u32 c = 0; c < 3; ++c) {
                f32 expected = hdr_off[i * 3 + c] +
                               material[i * 4 + c] * (combined / old - 1) * f->shadow.pre_exposure;
                energy += fabs(expected);
                error += fabs(half_to_f32(hdr[i * 4 + c]) - expected);
            }
        }
    printf("Material AO oracle L1 %.8g; actual RTAO once-only HDR composition L1 %.8g, %u affected "
           "reflection pixels\n",
           material_error / material_energy, error / energy, affected);
    EXPECT_TRUE(affected > 10);
    EXPECT_TRUE(error / energy < .01);
}
