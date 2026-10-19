/* Standalone DENG-45 path: DENG-46 many-light replacement is explicitly disabled. */
#include "rt_shadow_fixture.h"
TEST(rt_shadows, light_sampling) {
    for (u32 kind = 0; kind < 5; ++kind) {
        de__cluster_light light = {.position = {0, 0, 10, 100},
                                   .direction = {0, 0, -1, .1f},
                                   .shape = {0, 2, 1, 0},
                                   .axis = {1, 0, 0, 0}};
        memcpy(&light.shape[3], &kind, 4);
        for (u32 i = 0; i < 1024; ++i) {
            de_vec2 u = {(i + .5f) / 1024, (f32)((i * 373u) % 1024) / 1024};
            de__rt_ray r = de__rt_shadow_ray(&light, (de_vec3){0}, (de_vec3){0, 0, 1}, u);
            f32 n = r.direction[0] * r.direction[0] + r.direction[1] * r.direction[1] +
                    r.direction[2] * r.direction[2];
            EXPECT_FLOAT_EQ(n, 1, 1e-5);
            EXPECT_TRUE(r.direction[3] > 0);
            if (kind == 0)
                EXPECT_TRUE(r.direction[2] >= cosf(.1f) - 1e-6f);
            else {
                f32 x = r.origin[0] + r.direction[0] * r.direction[3],
                    y = r.origin[1] + r.direction[1] * r.direction[3];
                EXPECT_TRUE(fabsf(x) <= 2.001f && fabsf(y) <= 1.001f);
                if (kind == 4)
                    EXPECT_TRUE(x * x / 4 + y * y <= 1.001f);
            }
        }
    }
}
TEST_F(rt_shadows, toggle, shadow_fixture) {
    shadow_frame(f);
    de__rt_shadows_info info;
    const u16 *mask = shadow_read(f, &info);
    printf("RT shadow extent %u x %u slots%u maskcenter%g rays%llu\n", info.width, info.height,
           info.slots, half_to_f32(mask[(SH / 2) * SW + SW / 2]), (unsigned long long)info.rays);
    EXPECT_TRUE(f->rt_pass);
    EXPECT_EQ(info.width, SW);
    EXPECT_EQ(info.spp, 1u);
    EXPECT_TRUE(de_renderer_stats(f->rt.r)->rays_traced > 0);
    de_gpu_pipeline pipeline = f->rt.r->rt_scene->trace;
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.rt_shadows = false;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    EXPECT_EQ(de_renderer_stats(f->rt.r)->rays_traced, 0u);
    EXPECT_FALSE(f->rt_pass);
    EXPECT_TRUE(f->raster);
    settings.rt_shadows = true;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    EXPECT_EQ(f->rt.r->rt_scene->trace.id, pipeline.id);
    settings.shadows = DE_QUALITY_LOW;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    ASSERT_TRUE(de__rt_shadows_get(f->rt.r->rt_shadows, f->rt.view, &info));
    EXPECT_EQ(info.width, SW / 2);
    EXPECT_EQ(info.scale, 2u);
    settings.shadows = DE_QUALITY_ULTRA;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    ASSERT_TRUE(de__rt_shadows_get(f->rt.r->rt_shadows, f->rt.view, &info));
    EXPECT_EQ(info.spp, 2u);
}
static f32 edge_crossing(const f32 *row, f32 level, u32 lo, u32 hi) {
    for (u32 x = lo + 1; x <= hi; ++x)
        if (row[x] <= level && row[x - 1] > level)
            return (f32)(x - 1) + (row[x - 1] - level) / fmaxf(row[x - 1] - row[x], 1e-6f);
    printf("missing edge level %g [%u,%u], ends %g %g\n", level, lo, hi, row[lo], row[hi]);
    return -1;
}
static f32 measure_width(shadow_fixture *f, f32 height) {
    rt_fixture *r = &f->rt;
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, height}, (de_vec3){0, 0, 1}, 0, 1), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    f32 row[SW] = {0};
    for (u32 frame = 0; frame < 64; ++frame) {
        shadow_frame(f);
        de__rt_shadows_info info;
        const u16 *mask = shadow_read(f, &info);
        for (u32 x = 0; x < SW; ++x)
            for (u32 y = SH / 2 - 4; y < SH / 2 + 4; ++y)
                row[x] += half_to_f32(mask[y * SW + x]) / (64 * 8);
    }
    const f32 edge = 3 - 1 - height * tanf(.55f);
    u32 lo = (u32)((edge - .7f + 1) * SW / 8), hi = (u32)((edge + .7f + 1) * SW / 8);
    if (hi >= SW)
        hi = SW - 1;
    f32 a = edge_crossing(row, .9f, lo, hi), b = edge_crossing(row, .1f, lo, hi);
    ASSERT_TRUE(a >= 0 && b > a);
    printf("blocker height %g, penumbra 10-90 width %g pixels (%g m), edge %g\n", height, b - a,
           (b - a) * 8 / SW, edge);
    return b - a;
}
TEST_F(rt_shadows, penumbra_width, shadow_fixture) {
    rt_fixture *r = &f->rt;
    (void)cvar_set("r.rt.shadows.max_lights", "1");
    r->w.f.lights.light[0].source_radius = .15f;
    r->w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .55f, 1);
    f32 first = measure_width(f, 1), second = measure_width(f, 2);
    printf("penumbra ratio %g, analytic distance ratio 2 (tolerance 10%%)\n", second / first);
    EXPECT_FLOAT_EQ(second / first, 2, .2);
    (void)cvar_set("r.rt.shadows.max_lights", "4");
}
TEST_F(rt_shadows, alpha_mask, shadow_fixture) {
    rt_fixture *r = &f->rt;
    (void)cvar_set("r.rt.shadows.max_lights", "1");
    r->w.f.lights.light[0].source_radius = 0;
    r->w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .65f, 1);
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, 2}, (de_vec3){0, 0, 1}, 0, .6f), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    g_mat[1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_cutoff = .5f;
    g_mat[1].standard.base_color_tex.id = 1;
    g_mat[1].textures[0].id = 1;
    shadow_frame(f);
    de__rt_shadows_info info;
    const u16 *mask = shadow_read(f, &info);
    const f32 center = 3 - 2 * tanf(.65f);
    const u32 left = (u32)((center - .3f + 1) * SW / 8), right = (u32)((center + .3f + 1) * SW / 8);
    f32 transparent = half_to_f32(mask[(SH / 2) * SW + left]),
        opaque = half_to_f32(mask[(SH / 2) * SW + right]);
    printf("foliage shadow transparent %g opaque %g at pixels %u/%u\n", transparent, opaque, left,
           right);
    EXPECT_TRUE(transparent > .95f);
    EXPECT_TRUE(opaque < .05f);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->hdr_rb);
    f32 lit = half_to_f32(hdr[((SH / 2) * SW + left) * 4]),
        dark = half_to_f32(hdr[((SH / 2) * SW + right) * 4]);
    printf("actual deferred HDR through cutout %g vs shadow %g\n", lit, dark);
    EXPECT_TRUE(lit > dark * 1.5f);
    r->w.f.instances.flags[1] &= ~DE__RW_INST_CAST_SHADOWS;
    shadow_frame(f);
    mask = shadow_read(f, &info);
    EXPECT_TRUE(half_to_f32(mask[(SH / 2) * SW + right]) > .95f);
    (void)cvar_set("r.rt.shadows.max_lights", "4");
}

TEST_F(rt_shadows, no_heap_after_warmup, shadow_fixture) {
    for (u32 i = 0; i < 24; ++i)
        shadow_frame(f);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->rt.track);
    const de_tracking_stats gpu_before = de_tracking_allocator_stats(&f->rt.dev_track);
    for (u32 i = 0; i < 8; ++i)
        shadow_frame(f);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->rt.track);
    const de_tracking_stats gpu_after = de_tracking_allocator_stats(&f->rt.dev_track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(gpu_after.alloc_count, gpu_before.alloc_count);
    EXPECT_EQ(gpu_after.realloc_count, gpu_before.realloc_count);
}

TEST_F(rt_shadows, analytic_lights, shadow_fixture) {
    rt_fixture *r = &f->rt;
    (void)cvar_set("r.rt.shadows.max_lights", "1");
    for (u32 kind = DE_LIGHT_POINT; kind <= DE_LIGHT_DISK; ++kind) {
        DeLight l = {.kind = (de_light_kind)kind,
                     .color = {1, 1, 1},
                     .intensity = 100,
                     .range = 20,
                     .source_radius = .1f,
                     .area_size = {.5f, .5f},
                     .spot_inner = .6f,
                     .spot_outer = .8f,
                     .cast_shadows = true};
        world_light(&r->w, 0, &l, trs((de_vec3){0, 0, 4}, (de_vec3){0, 1, 0}, -.785398163f, 1));
        shadow_frame(f);
        de__rt_shadows_info info;
        const u16 *mask = shadow_read(f, &info);
        f32 minimum = 1;
        for (u32 i = 0; i < SW * SH; ++i)
            minimum = fminf(minimum, half_to_f32(mask[i]));
        printf("analytic light %u minimum visibility %g\n", kind, minimum);
        EXPECT_TRUE(minimum < .05f);
        r->w.f.lights.light[0].cast_shadows = false;
        shadow_frame(f);
        mask = shadow_read(f, &info);
        minimum = 1;
        for (u32 i = 0; i < SW * SH; ++i)
            minimum = fminf(minimum, half_to_f32(mask[i]));
        EXPECT_TRUE(minimum > .95f);
    }
    (void)cvar_set("r.rt.shadows.max_lights", "4");
}
