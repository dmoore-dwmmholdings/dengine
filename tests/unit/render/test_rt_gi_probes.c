#define SW 32u
#define SH 32u
#include "rt_shadow_fixture.h"
#include "render/rt/rt_gi_probes.h"
#include "render/probes/probes_internal.h"
#include "render/shaders/engine_shaders.h"

typedef struct ddgi_fixture {
    shadow_fixture shadow;
    de_gpu_buffer readback, queries, values;
    de_gpu_pipeline sample;
    de__rt_gi_info info;
    u32 irradiance_pitch, moments_pitch, moments_offset, state_offset;
} ddgi_fixture;
static void ddgi_fixture_setup(ddgi_fixture *f) {
    shadow_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    (void)cvar_set("r.emissive_light_max", "0");
    g_mat_count = 0;
    for (u32 face = 0; face < 6; ++face) {
        u32 material = material_add(.7f, .8f, 0, 1);
        g_mat[material - 1].standard.double_sided = true;
        if (face == 0)
            g_mat[material - 1].standard.base_color = (de_vec4){.8f, .08f, .08f, 1};
        if (face == 1)
            g_mat[material - 1].standard.base_color = (de_vec4){.08f, .8f, .08f, 1};
        de__rw_view c;
        de__probe_face_camera((de_dvec3){0}, (de_dvec3){0}, face, &c);
        de_affine xf = c.transform;
        for (u32 row = 0; row < 3; ++row)
            xf.m[row] = -xf.m[row];
        de_vec3 right = {xf.m[0], xf.m[1], xf.m[2]}, up = {xf.m[3], xf.m[4], xf.m[5]},
                normal = {xf.m[6], xf.m[7], xf.m[8]};
        ASSERT_TRUE(de_v3_dot(de_v3_cross(right, up), normal) > .999f);
        for (u32 axis = 0; axis < 3; ++axis) {
            xf.m[9 + axis] = -xf.m[6 + axis] * 2;
            for (u32 row = 0; row < 3; ++row)
                xf.m[axis * 3 + row] *= 2;
        }
        xf.m[9] += 3;
        xf.m[11] += 2;
        world_set(&r->w, face, 2, xf, material);
        r->w.f.instances.flags[face] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[face] = 1u << 24;
    }
    r->w.f.instances.count = 6;
    world_light(&r->w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 20,
                           .cast_shadows = true},
                at(3, 0, 3));
    f->shadow.camera.camera.projection = DE_PROJ_PERSPECTIVE;
    f->shadow.camera.camera.fov_y = 1;
    de_mat4 view = de_m4_look_at((de_vec3){3, 0, 2}, (de_vec3){3, 1, 1.7f}, (de_vec3){0, 0, 1}),
            inv = de_m4_inverse_affine(&view);
    f->shadow.camera.transform = de_affine_from_m4(&inv);
    f->shadow.camera.offset = (de_vec3){3, 0, 2};
    de_mat3 rotation;
    memcpy(rotation.m, f->shadow.camera.transform.m, sizeof rotation.m);
    f->shadow.camera.rotation = de_quat_from_m3(&rotation);
    r->w.f.irradiance_count = 1;
    r->w.f.irradiance[0] = (de__irradiance_snapshot){
        .entity = 77,
        .position = {3, 0, 2},
        .rotation = {0, 0, 0, 1},
        .volume = {
            .extent = {2, 2, 2}, .counts = {3, 3, 3}, .intensity = 1, .blend_distance = .1f}};
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.shadows = settings.ao = settings.reflections = DE_QUALITY_OFF;
    settings.rt_shadows = settings.rt_ao = settings.rt_reflections = false;
    settings.gi = DE_QUALITY_HIGH;
    settings.rt_gi = true;
    settings.gi_mode = DE_GI_RT;
    de_renderer_set_settings(r->r, &settings);
    f->readback = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = 8u << 20,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK});
    f->queries = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){
                  .size = 4096, .stride = 16, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    f->values = de_gpu_buffer_create(
        r->d, &(de_gpu_buffer_desc){
                  .size = 4096, .stride = 16, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(f->readback.id && f->queries.id && f->values.id);
    const char *entry = "cs_gi_sample_reference";
    de__engine_shader_code code;
    char why[512] = {0};
    ASSERT_EQ(de__engine_shader_load(r->d, &r->alloc, "irradiance_bake", 0, &entry, 1, &code, why,
                                     sizeof why),
              DE_OK);
    f->sample =
        de_gpu_pipeline_create(r->d, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                             .cs = code.entries[0].blob,
                                                             .push_constant_bytes = 48,
                                                             .threadgroup_size = {64, 1, 1}});
    de__engine_shader_code_free(&code);
    ASSERT_TRUE(f->sample.id);
}
static void ddgi_fixture_teardown(ddgi_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    if (r->d) {
        de_gpu_wait_idle(r->d);
        de_gpu_buffer_destroy(r->d, f->readback);
        de_gpu_buffer_destroy(r->d, f->queries);
        de_gpu_buffer_destroy(r->d, f->values);
        de_gpu_pipeline_destroy(r->d, f->sample);
    }
    (void)cvar_set("r.emissive_light_max", "64");
    shadow_fixture_teardown(&f->shadow);
}
static void ddgi_frame(ddgi_fixture *f) {
    shadow_frame(&f->shadow);
    ASSERT_TRUE(de__rt_gi_get(f->shadow.rt.r->rt_gi, f->shadow.rt.view, 0, &f->info));
}
static const u8 *ddgi_read(ddgi_fixture *f) {
    rt_fixture *r = &f->shadow.rt;
    const de__gi_layout *l = &f->info.layout;
    f->irradiance_pitch = (l->iw * 4 + 255) & ~255u;
    f->moments_pitch = (l->dw * 4 + 255) & ~255u;
    f->moments_offset = (f->irradiance_pitch * l->ih + 511) & ~511u;
    f->state_offset = (f->moments_offset + f->moments_pitch * l->dh + 511) & ~511u;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "DDGI actual S15 atlas readback");
    de_gpu_barrier b[3] = {
        {.texture = f->info.irradiance, .before = DE__GI_ACCESS, .after = DE_ACCESS_COPY_SRC},
        {.texture = f->info.distance, .before = DE__GI_ACCESS, .after = DE_ACCESS_COPY_SRC},
        {.buffer = f->info.classification,
         .before = DE_ACCESS_SHADER_READ,
         .after = DE_ACCESS_COPY_SRC}};
    de_gpu_cmd_barriers(cmd, b, 3);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, 0, f->irradiance_pitch, f->info.irradiance,
                                      0, 0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, f->moments_offset, f->moments_pitch,
                                      f->info.distance, 0, 0);
    de_gpu_cmd_copy_buffer(cmd, f->readback, f->state_offset, f->info.classification, 0,
                           (u64)l->probes * 16);
    for (u32 i = 0; i < 3; ++i) {
        b[i].before = DE_ACCESS_COPY_SRC;
        b[i].after = i == 2 ? DE_ACCESS_SHADER_READ : DE__GI_ACCESS;
    }
    de_gpu_cmd_barriers(cmd, b, 3);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    return de_gpu_buffer_map(r->d, f->readback);
}
static f32 component(u32 bits, u32 mantissa) {
    u32 exponent = bits >> mantissa, m = bits & ((1u << mantissa) - 1);
    return exponent ? ldexpf(1 + (f32)m / (1u << mantissa), (i32)exponent - 15)
                    : ldexpf((f32)m, 1 - 15 - (i32)mantissa);
}
static de_vec3 irradiance(u32 bits) {
    return (de_vec3){component(bits & 2047, 6), component((bits >> 11) & 2047, 6),
                     component(bits >> 22, 5)};
}
static void ddgi_assert_borders(const ddgi_fixture *f, const u8 *data) {
    for (u32 kind = 0; kind < 2; ++kind) {
        u32 n = kind ? 16 : 8, tile = n + 2, pitch = kind ? f->moments_pitch : f->irradiance_pitch,
            offset = kind ? f->moments_offset : 0;
        for (u32 probe = 0; probe < f->info.layout.probes; ++probe)
            for (u32 y = 0; y < tile; ++y)
                for (u32 x = 0; x < tile; ++x) {
                    if (x && y && x <= n && y <= n)
                        continue;
                    u32 sx = x, sy = y;
                    if ((x == 0 || x == n + 1) && (y == 0 || y == n + 1)) {
                        sx = x == 0 ? n : 1;
                        sy = y == 0 ? n : 1;
                    } else if (x == 0 || x == n + 1) {
                        sx = x == 0 ? 1 : n;
                        sy = n + 1 - y;
                    } else {
                        sx = n + 1 - x;
                        sy = y == 0 ? 1 : n;
                    }
                    u32 base_y = probe / f->info.layout.columns * tile,
                        base_x = probe % f->info.layout.columns * tile, a, b;
                    memcpy(&a, data + offset + (base_y + y) * pitch + (base_x + x) * 4, 4);
                    memcpy(&b, data + offset + (base_y + sy) * pitch + (base_x + sx) * 4, 4);
                    EXPECT_EQ(a, b);
                }
    }
}
TEST(ddgi, octahedral_roundtrip_and_hysteresis) {
    for (u32 i = 0; i < 4096; ++i) {
        f32 z = 1 - 2 * (i + .5f) / 4096, phi = i * 2.39996323f;
        de_vec3 n = {sqrtf(1 - z * z) * cosf(phi), sqrtf(1 - z * z) * sinf(phi), z};
        de_vec3 decoded = de__ddgi_oct_decode(de__ddgi_oct_encode(n));
        EXPECT_TRUE(de_v3_dot(n, decoded) > .99999f);
    }
    f32 value = 3;
    for (u32 i = 0; i < 120; ++i)
        value = de__ddgi_hysteresis(value, 4, .97f, true);
    EXPECT_TRUE(fabsf(value - (4 - powf(.97f, 120))) < 1e-5f);
    EXPECT_TRUE(value > 3.97f);
    EXPECT_TRUE(de__ddgi_hysteresis(1, 4, .97f, false) == 4);
}
TEST_F(ddgi, constant_radiance_true_irradiance_layout, ddgi_fixture) {
    for (u32 i = 0; i < 6; ++i) {
        g_mat[i].standard.unlit = true;
        g_mat[i].standard.base_color = (de_vec4){.5f, .5f, .5f, 1};
    }
    f->shadow.rt.w.touch_all = true;
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.5"));
    ddgi_frame(f);
    const u8 *data = ddgi_read(f);
    ddgi_assert_borders(f, data);
    const de__gi_layout *l = &f->info.layout;
    double sum = 0;
    u32 active = 0;
    const f32 *state = (const f32 *)(data + f->state_offset);
    for (u32 probe = 0; probe < l->probes; ++probe)
        if (state[probe * 4 + 3] == 0) {
            ++active;
            for (u32 y = 1; y <= 8; ++y)
                for (u32 x = 1; x <= 8; ++x) {
                    u32 bits;
                    memcpy(&bits,
                           data + (probe / l->columns * 10 + y) * f->irradiance_pitch +
                               (probe % l->columns * 10 + x) * 4,
                           4);
                    de_vec3 e = irradiance(bits);
                    sum += e.x + e.y + e.z;
                }
        }
    double mean = sum / (active * 64 * 3), expected = DE_PI * .5;
    printf("DDGI constant environment: %u active probes, true irradiance %.9g vs pi*L %.9g\n",
           active, mean, expected);
    ASSERT_TRUE(active > 20);
    EXPECT_TRUE(fabs(mean - expected) / expected < .02);
    EXPECT_EQ(l->iw, l->columns * 10);
    EXPECT_EQ(l->dw, l->columns * 18);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
}

static void ddgi_sample(ddgi_fixture *f, const de_vec3 *points, u32 count, de_vec4 *result) {
    rt_fixture *r = &f->shadow.rt;
    de_vec4 *queries = de_gpu_buffer_map(r->d, f->queries);
    for (u32 i = 0; i < count; ++i) {
        queries[i * 3] = (de_vec4){points[i].x, points[i].y, points[i].z, 0};
        queries[i * 3 + 1] = (de_vec4){0, 0, 1, 0};
        queries[i * 3 + 2] = (de_vec4){0, 0, 1, 0};
    }
    u32 pc[12] = {de_gpu_buffer_bindless(r->d, f->info.metadata),
                  de_gpu_buffer_bindless(r->d, f->queries),
                  de_gpu_buffer_bindless(r->d, f->values),
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  count};
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Actual S15 DDGI lighting sampler");
    de_gpu_barrier b = {
        .buffer = f->values, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_set_pipeline(cmd, f->sample);
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (count + 63) / 64, 1, 1);
    b.before = DE_ACCESS_SHADER_WRITE;
    b.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, f->readback, 0, f->values, 0, count * 16);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    memcpy(result, de_gpu_buffer_map(r->d, f->readback), count * 16);
}
static double energy(de_vec4 v) {
    return v.x + v.y + v.z;
}
TEST_F(ddgi, cornell_bleeding_and_moving_light, ddgi_fixture) {
    const de_vec3 points[] = {{4.2f, 0, .3f}, {3, 0, .3f}, {1.8f, 0, .3f}};
    de_vec4 early[3], late[3], moved[3], steady[3];
    for (u32 frame = 0; frame < 60; ++frame) {
        ddgi_frame(f);
        if (frame == 44)
            ddgi_sample(f, points, 3, early);
    }
    ddgi_sample(f, points, 3, late);
    const u8 *raw = ddgi_read(f);
    const float *st = (const float *)(raw + f->state_offset);
    double atlas = 0;
    u32 active = 0;
    for (u32 i = 0; i < f->info.layout.probes; ++i) {
        active += st[i * 4 + 3] == 0;
        u32 b;
        memcpy(&b,
               raw + (i / f->info.layout.columns * 10 + 4) * f->irradiance_pitch +
                   (i % f->info.layout.columns * 10 + 4) * 4,
               4);
        atlas += irradiance(b).x;
    }
    printf("diagnostic atlas red=%g active=%u local=(%g,%g,%g) range=%g ready=%g\n", atlas, active,
           f->info.gpu.local[0][3], f->info.gpu.local[1][3], f->info.gpu.local[2][3],
           f->info.gpu.params[2], f->info.gpu.params[3]);
    double change = 0, total = 0;
    for (u32 i = 0; i < 3; ++i) {
        change += fabs(energy(late[i]) - energy(early[i]));
        total += energy(late[i]);
    }
    printf("Cornell frame60: red receiver=(%g,%g,%g), convergence45->60=%g%%, E=%g\n", late[0].x,
           late[0].y, late[0].z, 100 * change / total, total);
    EXPECT_TRUE(late[0].x >= late[0].z * 1.15f);
    EXPECT_TRUE(total > 0);
    EXPECT_TRUE(change / total <= .10);
    world_light(&f->shadow.rt.w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 400,
                           .range = 20,
                           .cast_shadows = true},
                at(3, 1, 2.5f));
    for (u32 frame = 0; frame < 150; ++frame) {
        ddgi_frame(f);
        if (frame == 29)
            ddgi_sample(f, points, 3, moved);
    }
    ddgi_sample(f, points, 3, steady);
    double base = 0, step = 0, target = 0;
    for (u32 i = 0; i < 3; ++i) {
        base += energy(late[i]);
        step += energy(moved[i]);
        target += energy(steady[i]);
    }
    printf("Moving light: baseline=%g frame30=%g steady=%g response=%g%%\n", base, step, target,
           100 * (step - base) / (target - base));
    EXPECT_TRUE(fabs(step - base) / base >= .25);
    EXPECT_TRUE((step - base) / (target - base) >= .5);
}
TEST_F(ddgi, thin_wall_visibility_and_embedded_relocation, ddgi_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 side = 0; side < 2; ++side) {
        de__rw_view c;
        de__probe_face_camera((de_dvec3){0}, (de_dvec3){0}, side, &c);
        de_affine xf = c.transform;
        for (u32 row = 0; row < 3; ++row)
            xf.m[row] = -xf.m[row];
        for (u32 i = 0; i < 9; ++i)
            xf.m[i] *= 2;
        xf.m[9] = side ? 3.05f : 2.95f;
        xf.m[10] = 0;
        xf.m[11] = 2;
        world_set(&r->w, 6 + side, 2, xf, 3);
        r->w.f.instances.flags[6 + side] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
        r->w.f.instances.layer_mask[6 + side] = 1u << 24;
    }
    world_light(&r->w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT,
                           .color = {1, 1, 1},
                           .intensity = 400,
                           .range = 20,
                           .cast_shadows = true},
                at(2, 0, 2.5f));
    for (u32 frame = 0; frame < 10; ++frame)
        ddgi_frame(f);
    const u8 *data = ddgi_read(f);
    const float *state = (const float *)(data + f->state_offset);
    u32 inside = 0, moved = 0;
    for (u32 i = 0; i < f->info.layout.probes; ++i) {
        u32 x = i % 3;
        float px = 3 - 2 + (x + .5f) * 4 / 3 + state[i * 4];
        inside += px > 2.95f && px < 3.05f;
        moved += fabsf(state[i * 4]) > .001f;
    }
    printf("DDGI embedded probes: %u remaining inside, %u relocated\n", inside, moved);
    EXPECT_EQ(inside, 0u);
    EXPECT_TRUE(moved >= 9);
    for (u32 frame = 10; frame < 60; ++frame)
        ddgi_frame(f);
    const de_vec3 points[] = {{2, 0, .3f}, {4, 0, .3f}};
    de_vec4 samples[2];
    ddgi_sample(f, points, 2, samples);
    double bright = energy(samples[0]) / 3, dark = energy(samples[1]) / 3;
    printf("DDGI 10 cm wall: bright E=%g dark E=%g leakage=%g%%\n", bright, dark,
           100 * dark / bright);
    EXPECT_TRUE(bright >= .5);
    EXPECT_TRUE(dark / bright <= .02);
}
TEST_F(ddgi, scroll_budget_quality_fallback_and_warm_allocations, ddgi_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 i = 0; i < 24; ++i)
        ddgi_frame(f);
    EXPECT_EQ(f->info.cascades, 3u);
    EXPECT_EQ(f->info.rays_per_probe, 128u);
    EXPECT_EQ(f->info.probes_updated, 27u);
    de_tracking_stats cpu = de_tracking_allocator_stats(&r->track),
                      gpu = de_tracking_allocator_stats(&r->dev_track);
    for (u32 i = 0; i < 8; ++i)
        ddgi_frame(f);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).alloc_count, cpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->track).realloc_count, cpu.realloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).alloc_count, gpu.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).realloc_count, gpu.realloc_count);
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.gi = DE_QUALITY_LOW;
    de_renderer_set_settings(r->r, &settings);
    ddgi_frame(f);
    EXPECT_EQ(f->info.cascades, 1u);
    EXPECT_EQ(f->info.volumes, 1u);
    ASSERT_TRUE(cvar_set("r.rt.gi.probes_per_frame", "1"));
    f->shadow.camera.transform.m[9] += 4.f / 3;
    f->shadow.camera.offset.x += 4.f / 3;
    ddgi_frame(f);
    EXPECT_EQ(f->info.invalidated, 9u);
    EXPECT_EQ(f->info.probes_updated, 1u);
    const u8 *raw = ddgi_read(f);
    const float *state = (const float *)(raw + f->state_offset);
    u32 inactive = 0;
    for (u32 i = 0; i < 27; ++i)
        inactive += state[i * 4 + 3] != 0;
    printf("DDGI scroll: %llu invalidated, %u inactive, budget %u\n",
           (unsigned long long)f->info.invalidated, inactive, f->info.probes_updated);
    EXPECT_TRUE(inactive >= 8);
    u32 checked = 0;
    for (u32 i = 0; i < 27; ++i)
        if (i % 3 == 2 && state[i * 4 + 3] != 0) {
            ++checked;
            for (u32 y = 1; y <= 8; ++y)
                for (u32 x = 1; x <= 8; ++x) {
                    u32 bits;
                    memcpy(&bits,
                           raw + (i / f->info.layout.columns * 10 + y) * f->irradiance_pitch +
                               (i % f->info.layout.columns * 10 + x) * 4,
                           4);
                    EXPECT_EQ(bits, 0u);
                }
        }
    EXPECT_TRUE(checked >= 8);
    settings.rt_gi = false;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_gi_get(r->r->rt_gi, r->view, 0, &f->info));
    settings.tier = DE_TIER_BASE;
    settings.rt_gi = true;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__rt_gi_get(r->r->rt_gi, r->view, 0, &f->info));
    ASSERT_TRUE(cvar_set("r.rt.gi.probes_per_frame", "128"));
}
