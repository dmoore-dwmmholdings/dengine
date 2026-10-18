#include "rt_shadow_fixture.h"
#include "render/denoise/denoise_policy.h"
#include "render/rt/rtao.h"
#include "flip.h"
/* Fixed original oracles: shadow/AO FLIP<=.05; <.05m contact width<=2px;
 * moved-shadow edge within2frames. No thresholds selected from measured output. */
typedef shadow_fixture ds_fixture;
static void ds_fixture_setup(ds_fixture *f) {
    shadow_fixture_setup(f);
    rt_fixture *r = &f->rt;
    ASSERT_TRUE(cvar_set("r.rt.shadows.max_lights", "1"));
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "0"));
    (void)de_cvar_register(
        &(de_cvar_desc){.name = "r.rg_gpu_timers", .type = DE_CVAR_BOOL, .default_number = 1});
    ASSERT_TRUE(cvar_set("r.rg_gpu_timers", "1"));
    g_mat[1].standard.double_sided = true;
    /* Vertical rectangular pole: basez0, topz1, width1m. Seen edge-on by camera,
     * leaving the ground shadow visible all the way to its physical contact. */
    world_set(&r->w, 1, 2, trs((de_vec3){3, 0, .5f}, (de_vec3){0, 1, 0}, DE_PI / 2, .5f), 2);
    r->w.f.instances.flags[1] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[1] = 1u << 24;
    r->w.f.lights.light[0].source_radius = .08f;
    r->w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .65f, 1);
}
static void ds_fixture_teardown(ds_fixture *f) {
    (void)cvar_set("r.rt.reference_spp", "0");
    (void)cvar_set("r.rt.shadows.max_lights", "4");
    (void)cvar_set("r.rg_gpu_timers", "0");
    shadow_fixture_teardown(f);
}
static void timings(ds_fixture *f) {
    const de_render_stats *stats = de_renderer_stats(f->rt.r);
    u32 count = 0;
    double sum = 0;
    for (u32 i = 0; i < stats->pass_count; ++i)
        if (strncmp(stats->pass_names[i], "Denoise.", 8) == 0) {
            printf("GPU %s %.9g ms\n", stats->pass_names[i], stats->pass_gpu_ms[i]);
            sum += stats->pass_gpu_ms[i];
            count++;
        }
    printf("GPU denoiser passes%u total %.9g ms\n", count, sum);
    EXPECT_TRUE(count > 0 && sum > 0);
}
static float compare_flip(const float *reference, const float *actual) {
    u8 *a = malloc(SW * SH * 3), *b = malloc(SW * SH * 3);
    float *errors = malloc(SW * SH * sizeof *errors);
    ASSERT_TRUE(a && b && errors);
    for (u32 i = 0; i < SW * SH; ++i)
        for (u32 c = 0; c < 3; ++c) {
            a[i * 3 + c] = (u8)lrintf(255 * de__gb_srgb_encode(reference[i]));
            b[i * 3 + c] = (u8)lrintf(255 * de__gb_srgb_encode(actual[i]));
        }
    de_flip_image ref = {a, SW, SH, 3, 0}, image = {b, SW, SH, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&ref, &image, 0, errors));
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(errors, SW * SH, &stats));
    float result = stats.mean;
    free(a);
    free(b);
    free(errors);
    return result;
}
static void mask_copy(ds_fixture *f, float *out, u32 expected_spp) {
    de__rt_shadows_info info;
    const u16 *p = shadow_read(f, &info);
    ASSERT_EQ(info.spp, expected_spp);
    ASSERT_EQ(info.rays, (u64)info.width * info.height * info.slots * info.spp);
    for (u32 i = 0; i < SW * SH; ++i)
        out[i] = half_to_f32(p[i]);
}
static f32 row_edge(const float *image, u32 row, u32 lo, u32 hi, float level) {
    for (u32 x = lo + 1; x <= hi; ++x) {
        float a = image[row * SW + x - 1], b = image[row * SW + x];
        if (a > level && b <= level)
            return x - 1 + (a - level) / fmaxf(a - b, 1e-8f);
    }
    return -1;
}
TEST(denoise_shadow_ao, shadow_penumbra_estimate) {
    EXPECT_FLOAT_EQ(de__shadow_penumbra(.1f, 2, 100, 10, true), 2 * tan(.1) * 10, 1e-5);
    EXPECT_FLOAT_EQ(de__shadow_penumbra(.5f, 2, 10, 8, false), 1, 1e-6);
    EXPECT_FLOAT_EQ(de__shadow_penumbra(0, 2, 10, 8, false), 0, 0);
    EXPECT_FLOAT_EQ(de__shadow_penumbra(.5f, 0, 10, 8, false), 0, 0);
}
TEST_F(denoise_shadow_ao, shadow_vs_reference, ds_fixture) {
    float *reference = malloc(SW * SH * sizeof *reference),
          *actual = malloc(SW * SH * sizeof *actual);
    ASSERT_TRUE(reference && actual);
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "256"));
    shadow_frame(f);
    mask_copy(f, reference, 256);
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "0"));
    ASSERT_TRUE(cvar_set("r.denoise.reset", "1"));
    shadow_frame(f);
    ASSERT_TRUE(cvar_set("r.denoise.reset", "0"));
    for (u32 i = 0; i < 32; ++i)
        shadow_frame(f);
    mask_copy(f, actual, 1);
    float flip = compare_flip(reference, actual);
    double shadow = 0, lit = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        shadow += reference[i] < .2f;
        lit += reference[i] > .8f;
    }
    printf("shadow FLIP %.9g lit%g dark%g pixels\n", flip, lit, shadow);
    EXPECT_TRUE(lit > 100 && shadow > 100);
    EXPECT_TRUE(flip <= .05f);
    timings(f);
    /* The column immediately behind base x3 has intersection height <5cm at theta.65.
     * Measure transverse contact edge in y, away from the pole's top penumbra. */
    u32 x = SW / 2 - 1;
    float ground_x = -1 + (x + .5f) * 8 / SW, blocker_height = (3 - ground_x) / tanf(.65f);
    EXPECT_TRUE(blocker_height > 0 && blocker_height < .05f);
    float upper = -1, lower = -1;
    for (u32 y = 1; y < SH / 2; ++y) {
        float a = actual[(y - 1) * SW + x], b = actual[y * SW + x];
        if (upper < 0 && a > .9 && b <= .9)
            upper = y - 1 + (a - .9f) / fmaxf(a - b, 1e-8f);
        if (lower < 0 && a > .1 && b <= .1)
            lower = y - 1 + (a - .1f) / fmaxf(a - b, 1e-8f);
    }
    printf("contact blocker height %.9g edge crossings %.9g/%.9g width %.9gpx\n", blocker_height,
           upper, lower, lower - upper);
    EXPECT_TRUE(upper >= 0 && lower >= upper);
    EXPECT_TRUE(lower - upper <= 2);
    free(reference);
    free(actual);
}
TEST_F(denoise_shadow_ao, shadow_moving_light_lag, ds_fixture) {
    float *reference = malloc(SW * SH * sizeof *reference),
          *actual = malloc(SW * SH * sizeof *actual);
    ASSERT_TRUE(reference && actual);
    /* Capture moved reference first, then genuinely warm old-position history. No
     * reference-mode frame is inserted between that history and measured motion. */
    f->rt.w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .35f, 1);
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "256"));
    shadow_frame(f);
    mask_copy(f, reference, 256);
    float target = row_edge(reference, SH / 2, SW / 4, SW / 2, .5f);
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "0"));
    f->rt.w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .65f, 1);
    for (u32 i = 0; i < 24; ++i)
        shadow_frame(f);
    mask_copy(f, actual, 1);
    float old = row_edge(actual, SH / 2, SW / 4, SW / 2, .5f);
    f->rt.w.f.lights.transform[0] = trs((de_vec3){0}, (de_vec3){0, 1, 0}, .35f, 1);
    f32 positions[2];
    for (u32 i = 0; i < 2; ++i) {
        shadow_frame(f);
        mask_copy(f, actual, 1);
        positions[i] = row_edge(actual, SH / 2, SW / 4, SW / 2, .5f);
    }
    printf("moving light old %.9g target %.9g frame1 %.9g frame2 %.9g\n", old, target, positions[0],
           positions[1]);
    EXPECT_TRUE(old >= 0 && target >= 0 && fabsf(old - target) > 2);
    EXPECT_TRUE(positions[1] >= 0 && fabsf(positions[1] - target) <= 1);
    free(reference);
    free(actual);
}
TEST_F(denoise_shadow_ao, warm_and_fallback, ds_fixture) {
    for (u32 i = 0; i < 24; ++i)
        shadow_frame(f);
    de_tracking_stats cpu = de_tracking_allocator_stats(&f->rt.track),
                      gpu = de_tracking_allocator_stats(&f->rt.dev_track);
    for (u32 i = 0; i < 8; ++i)
        shadow_frame(f);
    de_tracking_stats ca = de_tracking_allocator_stats(&f->rt.track),
                      ga = de_tracking_allocator_stats(&f->rt.dev_track);
    EXPECT_EQ(cpu.alloc_count, ca.alloc_count);
    EXPECT_EQ(cpu.realloc_count, ca.realloc_count);
    EXPECT_EQ(gpu.alloc_count, ga.alloc_count);
    EXPECT_EQ(gpu.realloc_count, ga.realloc_count);
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.shadows = DE_QUALITY_LOW;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    de__rt_shadows_info info;
    ASSERT_TRUE(de__rt_shadows_get(f->rt.r->rt_shadows, f->rt.view, &info));
    EXPECT_EQ(info.width, SW / 2);
    EXPECT_EQ(info.height, SH / 2);
    EXPECT_EQ(info.scale, 2u);
    settings.rt_shadows = false;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    EXPECT_FALSE(f->rt_pass);
    EXPECT_TRUE(f->raster);
    settings.tier = DE_TIER_BASE;
    settings.rt_shadows = true;
    de_renderer_set_settings(f->rt.r, &settings);
    shadow_frame(f);
    EXPECT_FALSE(f->rt_pass);
}
typedef struct da_fixture {
    ds_fixture shadow;
    de_gpu_buffer rb;
    de__rtao_info info;
} da_fixture;
static void da_fixture_setup(da_fixture *f) {
    ds_fixture_setup(&f->shadow);
    rt_fixture *r = &f->shadow.rt;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.rt_shadows = false;
    settings.ao = DE_QUALITY_HIGH;
    settings.rt_ao = true;
    de_renderer_set_settings(r->r, &settings);
    world_set(&r->w, 1, 2, trs((de_vec3){1, 0, 2}, (de_vec3){0, 1, 0}, DE_PI / 2, 2), 2);
    world_set(&r->w, 2, 2, trs((de_vec3){3, -1.5f, 2}, (de_vec3){1, 0, 0}, DE_PI / 2, 2), 2);
    for (u32 i = 1; i < 3; ++i) {
        r->w.f.instances.flags[i] |= DE__RW_INST_VISIBLE_IN_RT;
        r->w.f.instances.layer_mask[i] = 1u << 24;
    }
    f->rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = SW * SH * 4,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->rb.id);
}
static void da_fixture_teardown(da_fixture *f) {
    if (f->shadow.rt.d) {
        de_gpu_wait_idle(f->shadow.rt.d);
        de_gpu_buffer_destroy(f->shadow.rt.d, f->rb);
    }
    ds_fixture_teardown(&f->shadow);
}
static void ao_copy(da_fixture *f, float *out, u32 spp) {
    rt_fixture *r = &f->shadow.rt;
    ASSERT_TRUE(de__rtao_get(r->r->rtao, r->view, &f->info));
    ASSERT_EQ(f->info.spp, spp);
    ASSERT_EQ(f->info.rays, (u64)f->info.trace_width * f->info.trace_height * spp);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Denoised AO independent readback");
    de_gpu_barrier b = {
        .texture = f->info.ao, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, 0, SW, f->info.ao, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    const u8 *pixels = de_gpu_buffer_map(r->d, f->rb);
    for (u32 i = 0; i < SW * SH; ++i)
        out[i] = pixels[i] / 255.f;
}
TEST_F(denoise_shadow_ao, ao_vs_reference, da_fixture) {
    float *reference = malloc(SW * SH * sizeof *reference),
          *actual = malloc(SW * SH * sizeof *actual);
    ASSERT_TRUE(reference && actual);
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "256"));
    shadow_frame(&f->shadow);
    ao_copy(f, reference, 256);
    ASSERT_TRUE(cvar_set("r.rt.reference_spp", "0"));
    for (u32 i = 0; i < 32; ++i)
        shadow_frame(&f->shadow);
    ao_copy(f, actual, 1);
    float flip = compare_flip(reference, actual);
    double minimum = 1, maximum = 0;
    for (u32 i = 0; i < SW * SH; ++i) {
        minimum = fmin(minimum, reference[i]);
        maximum = fmax(maximum, reference[i]);
    }
    printf("AO FLIP %.9g reference range %.9g..%.9g\n", flip, minimum, maximum);
    EXPECT_TRUE(maximum - minimum > .2);
    EXPECT_TRUE(flip <= .05);
    timings(&f->shadow);
    free(reference);
    free(actual);
}

TEST_F(denoise_shadow_ao, light_keys_and_low_guides, ds_fixture) {
    rt_fixture *r = &f->rt;
    ASSERT_TRUE(cvar_set("r.rt.shadows.max_lights", "4"));
    DeLight second = r->w.f.lights.light[0];
    second.source_radius = 0;
    world_light(&r->w, 1, &second, trs((de_vec3){0}, (de_vec3){0, 1, 0}, -.65f, 1));
    DeLight point = {.kind = DE_LIGHT_POINT,
                     .intensity = 20,
                     .color = {1, 1, 1},
                     .range = 3,
                     .source_radius = .1f,
                     .cast_shadows = true};
    world_light(&r->w, 2, &point, at(1, 0, 2));
    world_light(&r->w, 3, &point, at(5, 0, 2));
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.shadows = DE_QUALITY_LOW;
    de_renderer_set_settings(r->r, &settings);
    for (u32 i = 0; i < 24; ++i)
        shadow_frame(f);
    de__rt_shadows_info info;
    ASSERT_TRUE(de__rt_shadows_get(r->r->rt_shadows, r->view, &info));
    ASSERT_EQ(info.slots, 4u);
    ASSERT_EQ(info.width, SW / 2);
    ASSERT_EQ(info.height, SH / 2);
    ASSERT_EQ(info.scale, 2u);
    u32 pixels = info.width * info.height;
    de_gpu_buffer rb = de_gpu_buffer_create(r->d, &(de_gpu_buffer_desc){.size = (u64)pixels * 4 * 4,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK});
    ASSERT_TRUE(rb.id);
    de_gpu_cmd *cmd =
        de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Actual per-pixel shadow light keys");
    de_gpu_barrier b = {
        .buffer = info.keys, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, rb, 0, info.keys, 0, (u64)pixels * 4 * 4);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    const u32 *keys = de_gpu_buffer_map(r->d, rb);
    EXPECT_EQ(keys[(info.height / 2) * info.width + info.width / 2], 0x80000000u);
    EXPECT_EQ(keys[pixels + (info.height / 2) * info.width + info.width / 2], 0x80000001u);
    bool varied = false;
    for (u32 i = 1; i < pixels; ++i)
        varied |= keys[2 * pixels + i] != keys[2 * pixels];
    EXPECT_TRUE(varied);
    /* Ordinal slot0 changes to a different light in one frame. It must not retain
     * the first sun's shadow, even though material and object motion are unchanged. */
    r->w.f.lights.light[0].cast_shadows = false;
    shadow_frame(f);
    ASSERT_TRUE(de__rt_shadows_get(r->r->rt_shadows, r->view, &info));
    cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "Key-swapped filtered shadow");
    b = (de_gpu_barrier){
        .texture = info.visibility, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, info.width * 2, info.visibility, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(r->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(r->d);
    const u16 *visibility = de_gpu_buffer_map(r->d, rb);
    u32 x = (u32)((2.6f + 1) * info.width / 8);
    float value = half_to_f32(visibility[(info.height / 2) * info.width + x]);
    printf("Low guide/key swap pixel%u visibility %.9g\n", x, value);
    EXPECT_TRUE(value > .95f);
    de_gpu_buffer_destroy(r->d, rb);
}
