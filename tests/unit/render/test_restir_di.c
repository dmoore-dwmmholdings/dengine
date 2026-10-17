/* Real renderer ReSTIR DI; reference bypasses every reservoir operation and traces every light. */
#define SW 64u
#define SH 32u
#include "rt_shadow_fixture.h"
#include "render/rt/restir_di.h"
#define PIXELS (SW * SH)
TEST(restir_di, reservoir_merge) {
    de__di_reservoir r = {.source = UINT32_MAX};
    de__di_update(&r, 1, (de_vec2){.2f, .7f}, 2, 64, 32, 0);
    de__di_finalize(&r, r.m);
    EXPECT_FLOAT_EQ(r.weight, 1, 1e-7);
    de__di_reservoir history = {
        .source = 9, .u = .8f, .v = .1f, .m = 1000, .sum = 8000, .target = 4};
    de__di_finalize(&history, history.m);
    EXPECT_FLOAT_EQ(history.weight, 2, 1e-7);
    de__di_merge(&r, &history, 2, 640, 0);
    de__di_finalize(&r, r.m);
    EXPECT_EQ(r.m, 672u);
    EXPECT_EQ(r.source, 9u);
    EXPECT_FLOAT_EQ(r.sum, 2624, 1e-6);
    EXPECT_FLOAT_EQ(r.weight, 2624.f / 1344, 1e-6);
    EXPECT_FLOAT_EQ(r.u, .8, 1e-6);
    de__di_update(&r, 5, (de_vec2){0}, 0, 0, 32, .1f);
    EXPECT_EQ(r.m, 704u);
    EXPECT_EQ(r.source, 9u);
}
TEST(restir_di, alias_table) {
    /* Areas x luminance x pi: actual emissive triangle power plus an analytic source. */
    const f32 powers[4] = {2, 6, 0, 4};
    de__di_alias a[4], b[4];
    u32 work[8];
    ASSERT_EQ(de__di_alias_build(powers, 4, a, work), DE_OK);
    ASSERT_EQ(de__di_alias_build(powers, 4, b, work), DE_OK);
    EXPECT_MEMEQ(a, b, sizeof a);
    f32 recovered[4] = {0};
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_TRUE(a[i].probability >= 0 && a[i].probability <= 1);
        recovered[i] += a[i].probability / 4;
        recovered[a[i].alias] += (1 - a[i].probability) / 4;
    }
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(a[i].pdf, powers[i] / 12, 1e-6);
        EXPECT_FLOAT_EQ(recovered[i], powers[i] / 12, 1e-6);
    }
    const f32 zero[4] = {0};
    ASSERT_EQ(de__di_alias_build(zero, 4, a, work), DE_OK);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(a[i].pdf, .25, 1e-7);
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
TEST_F(restir_di, converges_to_reference, di_fixture) {
    rt_fixture *r = &f->shadow.rt;
    de__restir_di_debug(r->r->restir_di, &(de__di_debug){.reference = true});
    di_frame(f);
    memcpy(f->reference, f->current, sizeof f->reference);
    de__restir_di_debug(r->r->restir_di, NULL);
    for (u32 frame = 0; frame < 64; ++frame) {
        di_frame(f);
        for (u32 i = 0; i < PIXELS * 3; ++i)
            f->sum[i] += f->current[i] / 64;
    }
    f64 error = 0, total = 0;
    for (u32 i = 0; i < PIXELS * 3; ++i) {
        error += fabs(f->sum[i] - f->reference[i]);
        total += f->reference[i];
    }
    f64 estimated = 0;
    for (u32 i = 0; i < PIXELS * 3; ++i)
        estimated += f->sum[i];
    printf("signed ratio %.8f,final M%u age%u\n", estimated / total, f->reservoirs[PIXELS / 2].m,
           f->reservoirs[PIXELS / 2].age);
    printf("512 shadowed lights,64frames normalized mean absolute error %.8f,reference sum "
           "%.8f,alias builds %llu\n",
           error / total, total, (unsigned long long)f->info.alias_builds);
    ASSERT_TRUE(total > 1);
    EXPECT_TRUE(error / total <= .02);
    EXPECT_EQ(f->info.source_count, 512u);
    EXPECT_EQ(f->info.alias_builds, 1u);
    EXPECT_TRUE(f->info.history);
    const u16 *hdr = de_gpu_buffer_map(r->d, f->shadow.hdr_rb);
    f64 hdr_sum = 0, signal_sum = 0;
    for (u32 i = 0; i < PIXELS; ++i) {
        hdr_sum += half_to_f32(hdr[i * 4]);
        signal_sum += f->current[i * 3];
    }
    printf("actual HDR red sum %.7f,DI signal red sum %.7f\n", hdr_sum, signal_sum);
    EXPECT_TRUE(hdr_sum > 0);
    EXPECT_TRUE(fabs(hdr_sum - signal_sum * f->shadow.pre_exposure) /
                    (signal_sum * f->shadow.pre_exposure) <
                .003);
    f64 edge_reference = 0, edge_estimated = 0;
    u32 edge_pixels = 0;
    for (u32 y = 1; y < SH - 1; ++y)
        for (u32 x = 1; x < SW - 1; ++x) {
            u32 i = (y * SW + x) * 3;
            f64 value = f->reference[i] + f->reference[i + 1] + f->reference[i + 2];
            f64 left = f->reference[i - 3] + f->reference[i - 2] + f->reference[i - 1];
            f64 right = f->reference[i + 3] + f->reference[i + 4] + f->reference[i + 5];
            if (value > 0 && left > 0 && right > 0 && fabs(left - right) > .03 * value) {
                edge_reference += value;
                edge_estimated += f->sum[i] + f->sum[i + 1] + f->sum[i + 2];
                ++edge_pixels;
            }
        }
    printf("unbiased edge mask %u pixels ratio %.8f\n", edge_pixels,
           edge_estimated / edge_reference);
    EXPECT_TRUE(edge_pixels > 20);
    EXPECT_TRUE(edge_estimated / edge_reference >= .98);
    EXPECT_TRUE(edge_estimated / edge_reference <= 1.02);
}

TEST_F(restir_di, motion_disocclusion, di_fixture) {
    for (u32 i = 0; i < 8; ++i)
        di_frame(f);
    u64 epoch = f->info.epoch, builds = f->info.alias_builds;
    f->shadow.camera.offset.x += .4f;
    f->shadow.camera.rotation = de_quat_axis_angle((de_vec3){0, 1, 0}, .08f);
    f->shadow.camera.transform = trs(f->shadow.camera.offset, (de_vec3){0, 1, 0}, .08f, 1);
    di_frame(f);
    bool fresh[PIXELS] = {0};
    u32 disoccluded = 0, reprojected = 0;
    for (u32 i = 0; i < PIXELS; ++i)
        if (f->reservoirs[i].target > 0) {
            if (f->reservoirs[i].age == 0) {
                fresh[i] = true;
                ++disoccluded;
            } else
                ++reprojected;
        }
    printf("camera disocclusion: %u rejected history, %u reprojected\n", disoccluded, reprojected);
    EXPECT_TRUE(disoccluded > 0);
    EXPECT_TRUE(reprojected > 100);
    EXPECT_EQ(f->info.epoch, epoch);
    EXPECT_EQ(f->info.alias_builds, builds);
    for (u32 frame = 0; frame < 2; ++frame) {
        di_frame(f);
        for (u32 i = 0; i < PIXELS; ++i)
            if (fresh[i])
                EXPECT_TRUE(f->reservoirs[i].age <= frame + 1);
    }
}
TEST_F(restir_di, no_heap_after_warmup, di_fixture) {
    /* Warm the 512-light raster-atlas fallback as well as both DI histories/rings. */
    for (u32 i = 0; i < 64; ++i)
        di_frame(f);
    de_tracking_stats before = de_tracking_allocator_stats(&f->shadow.rt.track);
    de_tracking_stats gpu = de_tracking_allocator_stats(&f->shadow.rt.dev_track);
    u64 builds = f->info.alias_builds;
    for (u32 i = 0; i < 8; ++i)
        di_frame(f);
    de_tracking_stats after = de_tracking_allocator_stats(&f->shadow.rt.track);
    de_tracking_stats gpu_after = de_tracking_allocator_stats(&f->shadow.rt.dev_track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(gpu_after.alloc_count, gpu.alloc_count);
    EXPECT_EQ(gpu_after.realloc_count, gpu.realloc_count);
    EXPECT_EQ(f->info.alias_builds, builds);
}

TEST_F(restir_di, emissive_triangles, di_fixture) {
    rt_fixture *r = &f->shadow.rt;
    set_lights(f, 0);
    g_mat[1].standard.emissive = (de_vec3){2, 4, 1};
    g_mat[1].standard.emissive_strength = 3;
    g_mat[1].standard.double_sided = true;
    di_frame(f);
    EXPECT_EQ(f->info.source_count, PATCH_TRIS);
    EXPECT_EQ(f->info.emissive_count, PATCH_TRIS);
    const u8 *bytes = de_gpu_buffer_map(r->d, f->readback);
    f64 area = 0, power = 0, pdf = 0;
    for (u32 i = 0; i < f->info.source_count; ++i) {
        const u32 *head = (const u32 *)(bytes + PIXELS * 64 + i * 192);
        const f32 *source = (const f32 *)head;
        EXPECT_EQ(head[0], 1u);
        EXPECT_EQ(head[1], 1u);
        area += source[7];
        power += source[44];
        pdf += source[45];
        EXPECT_FLOAT_EQ(source[16], 6, 1e-6);
        EXPECT_FLOAT_EQ(source[17], 12, 1e-6);
        EXPECT_FLOAT_EQ(source[18], 3, 1e-6);
    }
    EXPECT_FLOAT_EQ(area, .64, 1e-5);
    EXPECT_FLOAT_EQ(pdf, 1, 1e-5);
    EXPECT_FLOAT_EQ(power, area * DE_PI * (.2126 * 6 + .7152 * 12 + .0722 * 3) * 2, .001);
    f64 energy = 0;
    for (u32 i = 0; i < PIXELS * 3; ++i)
        energy += f->current[i];
    EXPECT_TRUE(energy > 1);
    u64 builds = f->info.alias_builds, epoch = f->info.epoch;
    di_frame(f);
    EXPECT_EQ(f->info.alias_builds, builds);
    EXPECT_EQ(f->info.epoch, epoch);
    g_mat[1].standard.emissive_strength = 6;
    di_frame(f);
    EXPECT_EQ(f->info.alias_builds, builds + 1);
    EXPECT_EQ(f->info.epoch, epoch + 1);
    EXPECT_FALSE(f->info.history);
    bytes = de_gpu_buffer_map(r->d, f->readback);
    f64 changed = 0;
    for (u32 i = 0; i < f->info.source_count; ++i)
        changed += ((const f32 *)(bytes + PIXELS * 64 + i * 192))[44];
    EXPECT_FLOAT_EQ(changed, power * 2, .002);
    r->w.f.instances.entity[1] = 0;
    r->w.f.removed_instances[0] = 1;
    r->w.f.removed_instance_count = 1;
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__restir_di_get(r->r->restir_di, r->view, &f->info));
}
TEST_F(restir_di, alpha_visibility, di_fixture) {
    rt_fixture *r = &f->shadow.rt;
    set_lights(f, 1);
    ASSERT_TRUE(cvar_set("r.rt.restir_di_unbiased", "0"));
    r->w.f.lights.transform[0] = at(5.6f, 0, 4);
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, 2}, (de_vec3){0, 0, 1}, 0, .6f), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    g_mat[1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.alpha_cutoff = .5f;
    g_mat[1].standard.base_color_tex.id = 1;
    g_mat[1].textures[0].id = 1;
    di_frame(f);
    memcpy(f->reference, f->current, sizeof f->reference);
    g_mat[1].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    g_mat[1].standard.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    di_frame(f);
    for (u32 i = 0; i < PIXELS * 3; ++i)
        f->sum[i] = f->current[i];
    r->w.f.instances.flags[1] &= ~DE__RW_INST_CAST_SHADOWS;
    di_frame(f);
    u32 cutouts = 0, solid = 0;
    for (u32 i = 0; i < PIXELS; ++i) {
        f32 clear = f->current[i * 3], alpha = f->reference[i * 3];
        if (clear > 1e-5 && f->sum[i * 3] < .05 * clear) {
            if (alpha > .95 * clear)
                ++cutouts;
            if (alpha < .05 * clear)
                ++solid;
        }
    }
    printf("production DI alpha: %u cutout pixels, %u solid shadow pixels\n", cutouts, solid);
    EXPECT_TRUE(cutouts > 2);
    EXPECT_TRUE(solid > 2);
}

TEST_F(restir_di, per_view_resets_and_fallback, di_fixture) {
    rt_fixture *r = &f->shadow.rt;
    for (u32 i = 0; i < 3; ++i)
        di_frame(f);
    de_gpu_buffer first = f->info.reservoir;
    de_view second = de_view_create(r->r, &(de_view_desc){.target = r->target,
                                                          .viewport = {0, 0, 1, 1},
                                                          .name = "independent DI history"});
    ASSERT_TRUE(second.id != 0);
    de_view_set_layer_mask(r->r, second, 1u << 24);
    de__rw_view cameras[2] = {f->shadow.camera, f->shadow.camera};
    cameras[1].entity = 1000;
    cameras[1].camera.view = second;
    r->w.f.views = cameras;
    r->w.f.view_count = 2;
    di_frame(f);
    de__di_info other;
    ASSERT_TRUE(de__restir_di_get(r->r->restir_di, second, &other));
    EXPECT_FALSE(other.history);
    EXPECT_TRUE(other.reservoir.id != first.id);
    EXPECT_TRUE(f->info.history);
    di_frame(f);
    ASSERT_TRUE(de__restir_di_get(r->r->restir_di, second, &other));
    EXPECT_TRUE(other.history);
    cameras[0].offset.x += 2;
    cameras[0].transform = at(cameras[0].offset.x, 0, 8);
    di_frame(f);
    EXPECT_FALSE(f->info.history);
    ASSERT_TRUE(de__restir_di_get(r->r->restir_di, second, &other));
    EXPECT_TRUE(other.history);
    ASSERT_TRUE(cvar_set("r.rt.restir_di", "0"));
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__restir_di_get(r->r->restir_di, r->view, &f->info));
    EXPECT_TRUE(f->shadow.local_raster);
    EXPECT_FALSE(f->shadow.di_active);
    ASSERT_TRUE(cvar_set("r.rt.restir_di", "1"));
    di_frame(f);
    EXPECT_FALSE(f->info.history);
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.tier = DE_TIER_BASE;
    de_renderer_set_settings(r->r, &settings);
    shadow_frame(&f->shadow);
    EXPECT_FALSE(de__restir_di_get(r->r->restir_di, r->view, &f->info));
    settings.tier = DE_TIER_RT;
    de_renderer_set_settings(r->r, &settings);
    di_frame(f);
    EXPECT_FALSE(f->info.history);
    r->w.f.views = &f->shadow.camera;
    r->w.f.view_count = 1;
    de_view_destroy(r->r, second);
}
