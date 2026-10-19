#include "scene_as_fixture.h"
#include "render/pathtracer/pt_internal.h"
#include "render/graph/rg_internal.h"
#include "render/pathtracer/pt_sobol_table.h"
#include "render/rt/restir_di.h"
#include "render/probes/probes_internal.h"
#include "flip.h"

TEST(pathtracer, alias_table) {
    const f32 power[] = {1, 2, 7, 0};
    de__di_alias alias[4];
    u32 scratch[8];
    ASSERT_EQ(de__di_alias_build(power, 4, alias, scratch), DE_OK);
    EXPECT_FLOAT_EQ(alias[0].pdf, .1, 1e-7);
    EXPECT_FLOAT_EQ(alias[1].pdf, .2, 1e-7);
    EXPECT_FLOAT_EQ(alias[2].pdf, .7, 1e-7);
    EXPECT_FLOAT_EQ(alias[3].pdf, 0, 0);
    f64 frequencies[4] = {0};
    for (u32 i = 0; i < 4; ++i) {
        frequencies[i] += .25 * alias[i].probability;
        frequencies[alias[i].alias] += .25 * (1 - alias[i].probability);
    }
    for (u32 i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(frequencies[i], alias[i].pdf, 1e-7);
    EXPECT_FLOAT_EQ(de__pt_power(2, 3), 4.0 / 13, 1e-7);
}
TEST(pathtracer, sobol_table_matches_published_gray_order_points) {
    /* Joe-Kuo's published first ten three-dimensional points, in Gray order.
     * The renderer uses binary order, with the same power-of-two prefix sets. */
    const f32 expected[10][3] = {{0, 0, 0},
                                 {.5f, .5f, .5f},
                                 {.75f, .25f, .25f},
                                 {.25f, .75f, .75f},
                                 {.375f, .375f, .625f},
                                 {.875f, .875f, .125f},
                                 {.625f, .125f, .875f},
                                 {.125f, .625f, .375f},
                                 {.1875f, .3125f, .9375f},
                                 {.6875f, .8125f, .4375f}};
    for (u32 i = 0; i < 10; ++i)
        for (u32 dimension = 0; dimension < 3; ++dimension) {
            u32 value = 0, gray = i ^ (i >> 1);
            for (u32 bit = 0; gray; gray >>= 1, ++bit)
                if (gray & 1u)
                    value ^= de__pt_sobol_directions[dimension * 32 + bit];
            EXPECT_FLOAT_EQ((f64)value / 4294967296.0, expected[i][dimension], 0);
        }
}
TEST(pathtracer, env_cdf) {
    f32 weights[] = {0, 1, 3, 0}, cdf[4], pdf[4];
    de__pt_env_cdf(weights, 4, cdf, pdf);
    EXPECT_FLOAT_EQ(cdf[0], 0, 0);
    EXPECT_FLOAT_EQ(cdf[1], .25, 1e-7);
    EXPECT_FLOAT_EQ(cdf[2], 1, 0);
    EXPECT_FLOAT_EQ(cdf[3], 1, 0);
    for (u32 i = 0; i < 4; ++i)
        weights[i] = 0;
    de__pt_env_cdf(weights, 4, cdf, pdf);
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(pdf[i], .25, 1e-7);
        EXPECT_FLOAT_EQ(cdf[i], (i + 1) * .25, 1e-7);
    }
}
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
    (void)cvar_set("r.pt.fog.density", "0");
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
static float *pt_read(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->d, DE_QUEUE_GRAPHICS, "PT accumulation readback");
    de_gpu_barrier b = {.texture = f->info.accumulation,
                        .before = DE_ACCESS_SHADER_READ,
                        .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->rb, 0, f->info.width * 16, f->info.accumulation, 0,
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
TEST_F(pathtracer, sample_dimensions_cover_two_dimensional_cells, pt_fixture) {
#if DE_HAS_GPU_METAL
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    de_gpu_device *d = f->rt.d;
    de_gpu_pipeline pipeline = de_gpu_pipeline_create(
        d, &(de_gpu_pipeline_desc){
               .kind = DE_PIPE_COMPUTE,
               .cs = {de__pt_metal_library, de__pt_metal_library_bytes, "pt_sampling_probe"},
               .push_constant_bytes = 16,
               .threadgroup_size = {64, 1, 1}});
    ASSERT_TRUE(pipeline.id);
    de_gpu_buffer table =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = sizeof de__pt_sobol_directions,
                                                      .usage = DE_USAGE_STORAGE,
                                                      .memory = DE_MEM_UPLOAD});
    de_gpu_buffer output = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){.size = 3 * 256 * 16, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(table.id && output.id);
    memcpy(de_gpu_buffer_map(d, table), de__pt_sobol_directions, sizeof de__pt_sobol_directions);
    de_gpu_cmd *cmd =
        de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "PT multidimensional sampling acceptance");
    de_gpu_barrier barriers[] = {{.buffer = table, .after = DE_ACCESS_SHADER_READ},
                                 {.buffer = output, .after = DE_ACCESS_SHADER_WRITE}};
    de_gpu_cmd_barriers(cmd, barriers, 2);
    de_gpu_cmd_set_pipeline(cmd, pipeline);
    const u32 starts[] = {0, 1022, 1024};
    for (u32 block = 0; block < 3; ++block) {
        const u32 pc[] = {de_gpu_buffer_bindless(d, table), de_gpu_buffer_bindless(d, output),
                          starts[block], block};
        de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, 4, 1, 1);
    }
    de_gpu_barrier copy = {
        .buffer = output, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &copy, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 0, output, 0, 3 * 256 * 16);
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(d);
    const float *values = de_gpu_buffer_map(d, f->rb);
    u32 cells[256] = {0}, marginal[256] = {0};
    for (u32 i = 0; i < 256; ++i) {
        for (u32 c = 0; c < 3; ++c)
            ASSERT_TRUE(values[4 * i + c] >= 0 && values[4 * i + c] < 1);
        u32 x = (u32)(values[4 * i] * 16), y = (u32)(values[4 * i + 1] * 16);
        ++cells[y * 16 + x];
        ++marginal[(u32)(values[4 * i + 2] * 256)];
    }
    for (u32 i = 0; i < 256; ++i) {
        EXPECT_EQ(cells[i], 1u);
        EXPECT_EQ(marginal[i], 1u);
    }
    for (u32 block = 1; block < 3; ++block)
        for (u32 c = 0; c < 3; ++c) {
            u32 coarse[16] = {0};
            for (u32 i = 0; i < 256; ++i) {
                float u = values[(block * 256 + i) * 4 + c];
                ASSERT_TRUE(isfinite(u) && u >= 0 && u < 1);
                ++coarse[(u32)(u * 16)];
            }
            for (u32 i = 0; i < 16; ++i)
                EXPECT_TRUE(coarse[i] > 0);
        }
    de_gpu_pipeline_destroy(d, pipeline);
    de_gpu_buffer_destroy(d, output);
    de_gpu_buffer_destroy(d, table);
#endif
}
TEST_F(pathtracer, emissive_shadow_origin_uses_receiver_normal, pt_fixture) {
#if DE_HAS_GPU_METAL
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    de_gpu_device *d = f->rt.d;
    de_gpu_pipeline pipeline = de_gpu_pipeline_create(
        d, &(de_gpu_pipeline_desc){
               .kind = DE_PIPE_COMPUTE,
               .cs = {de__pt_metal_library, de__pt_metal_library_bytes, "pt_emissive_origin_probe"},
               .push_constant_bytes = 16,
               .threadgroup_size = {64, 1, 1}});
    ASSERT_TRUE(pipeline.id);
    de_gpu_buffer table =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = sizeof de__pt_sobol_directions,
                                                      .usage = DE_USAGE_STORAGE,
                                                      .memory = DE_MEM_UPLOAD});
    de_gpu_buffer query = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 32, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    de_gpu_buffer source = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 192, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    de_gpu_buffer output = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 512 * 32, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    ASSERT_TRUE(table.id && query.id && source.id && output.id);
    memcpy(de_gpu_buffer_map(d, table), de__pt_sobol_directions, sizeof de__pt_sobol_directions);
    memset(de_gpu_buffer_map(d, query), 0, 32);
    u32 *words = de_gpu_buffer_map(d, source);
    memset(words, 0, 192);
    words[0] = 1; /* DiSource: triangle, with constant emission (no material lookup). */
    words[3] = UINT32_MAX;
    f32 *v = (f32 *)words;
    const f32 points[] = {101, -1, 2, 2, 101, 1, 2, 0, 103, -1, 2, 0, 1, 1, 1, 0};
    memcpy(v + 4, points, sizeof points);
    v[44] = 2.f * (f32)DE_PI;
    v[45] = v[46] = 1; /* selection PDF and alias probability. */
    de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "Emissive receiver-normal acceptance");
    de_gpu_barrier barriers[] = {{.buffer = table, .after = DE_ACCESS_SHADER_READ},
                                 {.buffer = query, .after = DE_ACCESS_SHADER_READ},
                                 {.buffer = source, .after = DE_ACCESS_SHADER_READ},
                                 {.buffer = output, .after = DE_ACCESS_SHADER_WRITE}};
    de_gpu_cmd_barriers(cmd, barriers, 4);
    de_gpu_cmd_set_pipeline(cmd, pipeline);
    u32 pc[] = {de_gpu_buffer_bindless(d, table), de_gpu_buffer_bindless(d, output),
                de_gpu_buffer_bindless(d, query), de_gpu_buffer_bindless(d, source)};
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, 8, 1, 1);
    de_gpu_barrier copy = {
        .buffer = output, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &copy, 1);
    de_gpu_cmd_copy_buffer(cmd, f->rb, 0, output, 0, 512 * 32);
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(d);
    const f32 *rays = de_gpu_buffer_map(d, f->rb);
    u32 wrong_side = 0, self_hits = 0;
    for (u32 i = 0; i < 256; ++i) {
        const f32 *r = rays + 8 * i;
        ASSERT_TRUE(isfinite(r[7]) && r[7] > 0 && r[4] > 0 && r[3] > 0);
        wrong_side += r[0] <= 100;
        f32 t = (100 - r[0]) / r[4];
        self_hits += t >= .00001f && t < r[3];
    }
    printf("Emissive proposals: %u wrong-side origins, %u false receiver hits\n", wrong_side,
           self_hits);
    EXPECT_EQ(wrong_side, 0u);
    EXPECT_EQ(self_hits, 0u);
    u32 false_emitter_hits = 0, lost_blockers = 0;
    for (u32 i = 256; i < 512; ++i) {
        const f32 *ray = rays + 8 * i;
        ASSERT_TRUE(isfinite(ray[7]) && ray[7] > 0 && ray[6] < 0 && ray[3] > 0);
        f32 emitter_t = (2.f - ray[2]) / ray[6];
        false_emitter_hits += emitter_t >= .00001f && emitter_t < ray[3];
        /* An independent plane halfway to the emitter must remain inside the segment. */
        f32 blocker_t = (2.005f - ray[2]) / ray[6];
        lost_blockers += !(blocker_t >= .00001f && blocker_t < ray[3]);
        f32 x = ray[0] + ray[4] * emitter_t, y = ray[1] + ray[5] * emitter_t;
        EXPECT_TRUE(x >= 101.f - 1e-4f && y >= -1.f - 1e-4f && x + y <= 102.f + 1e-4f);
    }
    printf("Grazing emissive segments: %u false emitter hits, %u lost blockers\n",
           false_emitter_hits, lost_blockers);
    EXPECT_EQ(false_emitter_hits, 0u);
    EXPECT_EQ(lost_blockers, 0u);
    de_gpu_pipeline_destroy(d, pipeline);
    de_gpu_buffer buffers[] = {table, query, source, output};
    for (u32 i = 0; i < 4; ++i)
        de_gpu_buffer_destroy(d, buffers[i]);
#endif
}
TEST_F(pathtracer, directional_power_split_preserves_samples, pt_fixture) {
    rt_fixture *r = &f->rt;
    g_mat[0].standard.unlit = false;
    g_mat[0].standard.emissive = (de_vec3){0};
    g_mat[0].standard.emissive_strength = 0;
    g_mat[0].standard.base_color = (de_vec4){.8f, .8f, .8f, 1};
    r->w.touch_all = true;
    de_render_settings settings = de_renderer_get_settings(r->r);
    settings.path_max_bounces = 1;
    de_renderer_set_settings(r->r, &settings);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0"));
    ASSERT_TRUE(cvar_set("r.pt.sampling_strategy", "1"));
    f64 grouped[64], maximum_error = 0, signal = 0;
    for (u32 split = 0; split < 2; ++split) {
        DeLight sun = {
            .kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = split ? 1.f : 100.f};
        world_light(&r->w, 0, &sun, at(0, 0, 0));
        sun.intensity = split ? 99.f : 0.f;
        world_light(&r->w, 1, &sun, at(0, 0, 0));
        /* Keep an unrelated slot and a dead slot in the authored table. */
        DeLight point = {
            .kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 100, .range = .1f};
        world_light(&r->w, 2, &point, at(100, 100, 100));
        world_light(&r->w, 3, &sun, at(0, 0, 0));
        r->w.f.lights.entity[3] = 0;
        de_renderer_path_reset(r->r, r->view);
        f64 previous = 0;
        de_tracking_stats warm = {0}, device_warm = {0};
        for (u32 i = 0; i < 64; ++i) {
            pt_frame(f);
            const float *pixels = pt_read(f);
            ASSERT_EQ(f->info.directional_count, 4u);
            const de__di_alias *alias = de_gpu_buffer_map(r->d, f->info.directional_alias);
            ASSERT_TRUE(alias != NULL);
            alias += f->info.directional_offset;
            EXPECT_FLOAT_EQ(alias[0].pdf, split ? .01 : 1, 1e-7);
            EXPECT_FLOAT_EQ(alias[1].pdf, split ? .99 : 0, 1e-7);
            EXPECT_FLOAT_EQ(alias[2].pdf, 0, 0);
            EXPECT_FLOAT_EQ(alias[3].pdf, 0, 0);
            const f64 total = pixels[(32 * 64 + 32) * 4];
            const f64 value = total - previous;
            previous = total;
            ASSERT_TRUE(isfinite(value));
            if (i == 16) {
                warm = de_tracking_allocator_stats(&r->track);
                device_warm = de_tracking_allocator_stats(&r->dev_track);
            }
            if (!split) {
                grouped[i] = value;
                signal += value;
            } else
                maximum_error = fmax(maximum_error, fabs(value - grouped[i]));
        }
        EXPECT_EQ(de_tracking_allocator_stats(&r->track).alloc_count, warm.alloc_count);
        EXPECT_EQ(de_tracking_allocator_stats(&r->track).realloc_count, warm.realloc_count);
        EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).alloc_count, device_warm.alloc_count);
        EXPECT_EQ(de_tracking_allocator_stats(&r->dev_track).realloc_count,
                  device_warm.realloc_count);
    }
    r->w.f.lights.light[0].intensity = .7152f;
    r->w.f.lights.light[1].intensity = .2126f;
    r->w.f.lights.light[0].color = (de_vec3){1, 0, 0};
    r->w.f.lights.light[1].color = (de_vec3){0, 1, 0};
    r->w.touch_lights = true;
    de_renderer_path_reset(r->r, r->view);
    u32 red_count = 0, green_count = 0;
    f64 previous_red = 0, previous_green = 0;
    for (u32 sample = 0; sample < 256; ++sample) {
        pt_frame(f);
        const float *pixels = pt_read(f);
        const f64 red = pixels[(32 * 64 + 32) * 4];
        const f64 green = pixels[(32 * 64 + 32) * 4 + 1];
        red_count += red - previous_red > 1e-5;
        green_count += green - previous_green > 1e-5;
        previous_red = red;
        previous_green = green;
    }
    const de__di_alias *colored = de_gpu_buffer_map(r->d, f->info.directional_alias);
    ASSERT_TRUE(colored != NULL);
    colored += f->info.directional_offset;
    EXPECT_FLOAT_EQ(colored[0].pdf, .5, 1e-7);
    EXPECT_FLOAT_EQ(colored[1].pdf, .5, 1e-7);
    printf("equal-power directional selections red%u green%u\n", red_count, green_count);
    EXPECT_TRUE(red_count > 30 && green_count > 30);
    EXPECT_TRUE(abs((int)red_count - (int)green_count) <= 2);
    r->w.f.lights.light[0].intensity = 0;
    r->w.f.lights.light[1].intensity = 0;
    r->w.touch_lights = true;
    de_renderer_path_reset(r->r, r->view);
    pt_frame(f);
    EXPECT_EQ(f->info.directional_count, 0u);
    EXPECT_FLOAT_EQ(pt_read(f)[(32 * 64 + 32) * 4], 0, 1e-6);
    printf("directional split signal %g max per-sample difference %g\n", signal, maximum_error);
    EXPECT_TRUE(signal > 1);
    EXPECT_TRUE(maximum_error <= .001);
}
TEST_F(pathtracer, progressive_hdr_and_reset, pt_fixture) {
    pt_frame(f);
    ASSERT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    float *pixels = pt_read(f);
    u32 i = (32 * 64 + 32) * 4;
    printf("PT first actual center HDR: %g %g %g, samples %g\n", pixels[i], pixels[i + 1],
           pixels[i + 2], pixels[i + 3]);
    EXPECT_FLOAT_EQ(pixels[i], 2, 1e-4);
    EXPECT_FLOAT_EQ(pixels[i + 1], 1, 1e-4);
    EXPECT_FLOAT_EQ(pixels[i + 2], .5, 1e-4);
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 2u);
    f->camera.offset.x += .01f;
    f->camera.transform.m[9] += .01f;
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 2u);
    de_renderer_path_reset(f->rt.r, f->rt.view);
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    g_mat[0].standard.base_color = (de_vec4){1, 2, 3, 1};
    f->rt.w.touch_all = true;
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    pixels = pt_read(f);
    EXPECT_FLOAT_EQ(pixels[i + 2], 3, 1e-4);
}
static void add_sphere(pt_fixture *f) {
    rt_fixture *r = &f->rt;
    const u32 cols = 12, rows = 8;
    fmesh *fm = &g_mesh[1];
    mesh_plane(fm, r->d, cols, rows, 1);
    g_mesh_count = 2;
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
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    world_set(&r->w, 0, 2, at(3, 0, 0), 1);
    r->w.f.instances.flags[0] |= DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    r->w.f.instances.layer_mask[0] = 1u << 24;
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "1"));
}

TEST_F(pathtracer, furnace, pt_fixture) {
    add_sphere(f);
    g_mat[0].standard.unlit = false;
    g_mat[0].standard.emissive = (de_vec3){0};
    g_mat[0].standard.specular = 0;
    f->rt.w.touch_all = true;
    for (u32 metal = 0; metal < 2; ++metal) {
        g_mat[0].standard.metallic = (f32)metal;
        g_mat[0].standard.specular = metal ? 1.f : 0.f;
        de_renderer_path_reset(f->rt.r, f->rt.view);
        for (u32 sample = 0; sample < 256; ++sample)
            pt_frame(f);
        float *pixels = pt_read(f);
        ASSERT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 256u);
        ASSERT_FLOAT_EQ(pixels[(32 * 64 + 32) * 4 + 3], 256, 0);
        f64 sum = 0;
        u32 n = 0;
        // Fixed central disk is strictly inside the independent projected sphere silhouette.
        for (u32 y = 26; y < 38; ++y)
            for (u32 x = 26; x < 38; ++x) {
                u32 i = (y * 64 + x) * 4;
                sum += pixels[i] / pixels[i + 3];
                ++n;
            }
        f64 mean = sum / n;
        printf("PT furnace %s: %g, absolute energy error %g%%\n", metal ? "GGX" : "Lambert", mean,
               100 * fabs(mean - 1));
        EXPECT_FLOAT_EQ(mean, 1, .01);
    }
}
TEST_F(pathtracer, warmed_allocations, pt_fixture) {
    for (u32 i = 0; i < 24; ++i)
        pt_frame(f);
    de_tracking_stats cpu = de_tracking_allocator_stats(&f->rt.track),
                      gpu = de_tracking_allocator_stats(&f->rt.dev_track);
    for (u32 i = 0; i < 8; ++i)
        pt_frame(f);
    de_tracking_stats after = de_tracking_allocator_stats(&f->rt.track),
                      device = de_tracking_allocator_stats(&f->rt.dev_track);
    EXPECT_EQ(after.alloc_count, cpu.alloc_count);
    EXPECT_EQ(after.realloc_count, cpu.realloc_count);
    EXPECT_EQ(device.alloc_count, gpu.alloc_count);
    EXPECT_EQ(device.realloc_count, gpu.realloc_count);
}

TEST_F(pathtracer, actual_alpha, pt_fixture) {
    g_mat[0].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[0].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[0].standard.base_color_tex = (de_texture){1};
    g_mat[0].textures[0] = (de_texture){1};
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.1"));
    f->rt.w.touch_all = true;
    pt_frame(f);
    float *pixels = pt_read(f);
    u32 left = (32 * 64 + 24) * 4, right = (32 * 64 + 40) * 4;
    printf("PT alpha fixed probes: left %g, right %g\n", pixels[left], pixels[right]);
    EXPECT_FLOAT_EQ(pixels[left], .1, .0001);
    EXPECT_FLOAT_EQ(pixels[right], 2, .0001);
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
typedef struct pt_timing_capture {
    de_view view;
    de_rg *graph;
    u64 rendered_frame;
} pt_timing_capture;

static void pt_timing_stage(de__view_frame *frame, void *user) {
    pt_timing_capture *capture = user;
    if (frame->view.id == capture->view.id) {
        capture->graph = frame->graph;
        capture->rendered_frame = frame->frame_index;
    }
}

/* DENG-450: capture completed samples25..120, excluding all24 warm-up samples.
 * Timings are diagnostic output, never a machine-load-dependent unit-test limit. */
TEST_F(pathtracer, no_media_gpu_timings_label_completed_samples, pt_fixture) {
    if (de_gpu_get_caps(f->rt.d)->timestamp_frequency == 0)
        SKIP("GPU timestamps unavailable");
    cornell(f);
    for (u32 i = 0; i < g_mat_count; ++i) {
        ASSERT_FLOAT_EQ(g_mat[i].standard.transmission, 0, 0);
        ASSERT_FLOAT_EQ(g_mat[i].standard.subsurface, 0, 0);
    }
    ASSERT_EQ(de_renderer_get_settings(f->rt.r).path_max_bounces, 8u);
    pt_timing_capture capture = {.view = f->rt.view};
    ASSERT_EQ(de__view_register_stage(f->rt.r, DE__VIEW_SLOT_POST, INT32_MIN,
                                      "PT timing frame identity", pt_timing_stage, &capture),
              DE_OK);
    pt_frame(f);
    de_cvar timers = de_cvar_find("r.rg_gpu_timers");
    ASSERT_TRUE(DE_HANDLE_VALID(timers));
    bool was_enabled = de_cvar_bool(timers);
    ASSERT_EQ(de_cvar_set(timers, "1"), DE_OK);
    for (u32 sample = 1; sample < 24; ++sample)
        pt_frame(f);
    ASSERT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 24u);
    ASSERT_EQ(f->info.width, 16u);
    ASSERT_EQ(f->info.height, 16u);
    u64 first_frame = capture.rendered_frame + 1u;
    u32 captured = 0;
    for (u32 sample = 25; sample <= 120; ++sample) {
        pt_frame(f); /* Includes a GPU wait: this frame's resolves are already complete. */
        ASSERT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), sample);
        /* Normal stats publish at the next frame start. Collect the already-completed
         * resolves here so the last warm-up frame is excluded and sample120 is included. */
        de__rg_timing_collect(capture.graph, capture.graph->exec, false);
        const de__rg_timings *t = de__rg_timings_get(capture.graph);
        ASSERT_TRUE(t->valid);
        EXPECT_EQ(capture.rendered_frame, first_frame + sample - 25u);
        EXPECT_EQ(t->frame_index, capture.rendered_frame);
        u32 found = 0;
        for (u32 pass = 0; pass < t->pass_count; ++pass) {
            if (strcmp(t->pass_names[pass], "PT.TraceAccumulate") != 0)
                continue;
            EXPECT_TRUE(isfinite(t->pass_gpu_ms[pass]) && t->pass_gpu_ms[pass] > 0);
            printf("PT_GPU_FRAME sample=%u rendered=%llu timed=%llu ms=%.9g\n", sample,
                   (unsigned long long)capture.rendered_frame, (unsigned long long)t->frame_index,
                   (double)t->pass_gpu_ms[pass]);
            ++found;
        }
        EXPECT_EQ(found, 1u);
        captured += found;
    }
    EXPECT_EQ(captured, 96u);
    EXPECT_FLOAT_EQ(pt_read(f)[(8 * 16 + 8) * 4 + 3], 120, 0);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 120u);
    printf("PT_GPU_CAPTURE warm=24 captured=%u actual_samples=120 extra_frames=0\n", captured);
    de__view_unregister_stage(f->rt.r, pt_timing_stage, &capture);
    EXPECT_EQ(de_cvar_set(timers, was_enabled ? "1" : "0"), DE_OK);
}

static u8 pt_display(f32 value) {
    value = fmaxf(value, 0);
    value = value / (1 + value);
    return (u8)lrintf(
        fminf(1, value <= .0031308f ? 12.92f * value : 1.055f * powf(value, 1 / 2.4f) - .055f) *
        255);
}
TEST_F(pathtracer, cornell_reference, pt_fixture) {
    cornell(f);
    bool record = getenv("DE_PT_RECORD_REFERENCE") != NULL;
    u32 samples = record ? 4096 : 1024;
    if (!record)
        ASSERT_TRUE(cvar_set("r.pt.seed", "1"));
    for (u32 i = 0; i < samples; ++i)
        pt_frame(f);
    ASSERT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), samples);
    float *pixels = pt_read(f);
    ASSERT_FLOAT_EQ(pixels[(8 * 16 + 8) * 4 + 3], samples, 0);
    f32 actual[16 * 16 * 3], reference[16 * 16 * 3];
    f64 energy = 0;
    for (u32 i = 0; i < 16 * 16; ++i)
        for (u32 c = 0; c < 3; ++c) {
            actual[i * 3 + c] = pixels[i * 4 + c] / samples;
            ASSERT_TRUE(isfinite(actual[i * 3 + c]));
            energy += actual[i * 3 + c];
        }
    ASSERT_TRUE(energy > 1);
    if (record) {
        FILE *out = fopen("/tmp/deng72-cornell-4096.f32", "wb");
        ASSERT_TRUE(out);
        ASSERT_EQ(fwrite(actual, sizeof actual, 1, out), 1u);
        ASSERT_EQ(fclose(out), 0);
        printf("Stored actual Cornell4096 reference: energy %g\n", energy);
        return;
    }
    FILE *in = fopen(DE_PT_REFERENCE, "rb");
    ASSERT_TRUE(in);
    ASSERT_EQ(fread(reference, sizeof reference, 1, in), 1u);
    ASSERT_EQ(fclose(in), 0);
    u8 a[16 * 16 * 3], b[16 * 16 * 3];
    f32 errors[16 * 16];
    for (u32 i = 0; i < 16 * 16 * 3; ++i) {
        a[i] = pt_display(actual[i]);
        b[i] = pt_display(reference[i]);
    }
    de_flip_image ai = {a, 16, 16, 3, 0}, bi = {b, 16, 16, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&bi, &ai, 0, errors));
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(errors, 16 * 16, &stats));
    printf("PT Cornell1024 seed1 vs stored4096 seed0: FLIP mean %g, energy %g\n", stats.mean,
           energy);
    EXPECT_TRUE(stats.mean <= .02);
}
TEST_F(pathtracer, terminal_bounce_records_continuation_emission, pt_fixture) {
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "1"));
    g_mat[0].standard.unlit = false;
    g_mat[0].standard.emissive = (de_vec3){0};
    g_mat[0].standard.specular = 0;
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    f->rt.w.touch_all = true;
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.path_max_bounces = 1;
    de_renderer_set_settings(f->rt.r, &settings);
    ASSERT_TRUE(cvar_set("r.pt.sampling_strategy", "2"));
    for (u32 sample = 0; sample < 16; ++sample)
        pt_frame(f);
    const f32 *pixels = pt_read(f);
    f64 mean = 0;
    for (u32 y = 26; y < 38; ++y)
        for (u32 x = 26; x < 38; ++x) {
            u32 i = (y * 64 + x) * 4;
            ASSERT_FLOAT_EQ(pixels[i + 3], 16, 0);
            for (u32 c = 0; c < 3; ++c)
                EXPECT_FLOAT_EQ(pixels[i + c] / 16, 1, 1e-3);
            mean += pixels[i] / 16;
        }
    printf("Terminal depth1 BSDF-only white furnace mean%g (expected1)\n", mean / 144);
}

TEST_F(pathtracer, mis_variance, pt_fixture) {
    g_mat[0].standard.unlit = false;
    g_mat[0].standard.emissive = (de_vec3){0};
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    g_mat[0].standard.metallic = 1;
    g_mat[0].standard.specular = 1;
    g_mat[0].standard.roughness = .2f;
    world_light(&f->rt.w, 0,
                &(DeLight){.kind = DE_LIGHT_RECT,
                           .color = {1, 1, 1},
                           .intensity = 100,
                           .range = 100,
                           .area_size = {.5f, .5f},
                           .cast_shadows = true},
                at(3, 0, 2));
    f->rt.w.touch_all = true;
    de_render_settings settings = de_renderer_get_settings(f->rt.r);
    settings.path_max_bounces = 2;
    de_renderer_set_settings(f->rt.r, &settings);
    f64 variance[3], mean[3];
    for (u32 mode = 0; mode < 3; ++mode) {
        char value[2] = {(char)('0' + mode), 0};
        ASSERT_TRUE(cvar_set("r.pt.sampling_strategy", value));
        de_renderer_path_reset(f->rt.r, f->rt.view);
        f64 sum = 0, squares = 0, previous = 0;
        const u32 samples = 256;
        for (u32 i = 0; i < samples; ++i) {
            pt_frame(f);
            float *pixels = pt_read(f);
            ASSERT_FLOAT_EQ(pixels[(32 * 64 + 32) * 4 + 3], i + 1, 0);
            f64 total = pixels[(32 * 64 + 32) * 4], sample = total - previous;
            previous = total;
            sum += sample;
            squares += sample * sample;
            ASSERT_TRUE(isfinite(sample) && fabs(sample) < 1e5);
        }
        ASSERT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), samples);
        mean[mode] = sum / samples;
        variance[mode] = (squares - sum * sum / samples) / (samples - 1);
    }
    printf("PT area MIS variance %g mean %g; NEE-only %g mean %g; BSDF-only %g mean %g\n",
           variance[0], mean[0], variance[1], mean[1], variance[2], mean[2]);
    EXPECT_TRUE(variance[0] < variance[1]);
    EXPECT_TRUE(variance[0] < variance[2]);
}

TEST_F(pathtracer, pipelines_are_lazy_selected_once_and_reused, pt_fixture) {
    de_renderer *r = f->rt.r;
    de_render_settings selected = de_renderer_get_settings(r);
    ASSERT_TRUE(selected.path_tracer && selected.tier == DE_TIER_RT);
    EXPECT_FALSE(de__pt_enabled(r, &selected));
    u32 before = de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE);
    de_render_settings inactive = selected;
    inactive.tier = DE_TIER_BASE;
    ASSERT_EQ(de__pt_prepare(r, &inactive), DE_OK);
    EXPECT_FALSE(de__pt_enabled(r, &inactive));
    inactive = selected;
    inactive.path_tracer = false;
    ASSERT_EQ(de__pt_prepare(r, &inactive), DE_OK);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before);
    ASSERT_EQ(de__pt_prepare(r, &selected), DE_OK);
    EXPECT_TRUE(de__pt_enabled(r, &selected));
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before + 2);
    ASSERT_EQ(de__pt_prepare(r, &selected), DE_OK);
    ASSERT_EQ(de__pt_prepare(r, &inactive), DE_OK);
    EXPECT_FALSE(de__pt_enabled(r, &inactive));
    ASSERT_EQ(de__pt_prepare(r, &selected), DE_OK);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before + 2);
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(r, f->rt.view), 1u);
}

TEST_F(pathtracer, lazy_pipeline_failure_cleans_partial_handles_and_is_cached, pt_fixture) {
    de_renderer *r = f->rt.r;
    de_render_settings selected = de_renderer_get_settings(r);
    u32 before = de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE);
    de_tracking_allocator_fail_after(&f->rt.dev_track, 0);
    de_result result = de__pt_prepare(r, &selected);
    de_tracking_allocator_fail_after(&f->rt.dev_track, -1);
    EXPECT_NE(result, DE_OK);
    EXPECT_GT(de_tracking_allocator_stats(&f->rt.dev_track).fail_count, 0u);
    EXPECT_FALSE(de__pt_enabled(r, &selected));
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before);
    /* Restored memory does not repeat an expensive failed compiler attempt. */
    EXPECT_EQ(de__pt_prepare(r, &selected), result);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before);
    selected.path_tracer = false;
    EXPECT_EQ(de__pt_prepare(r, &selected), DE_OK);
    EXPECT_FALSE(de_gpu_device_is_lost(f->rt.d));
}

TEST_F(pathtracer, native_media_pipeline_is_lazy_and_reused, pt_fixture) {
    if (de_gpu_get_caps(f->rt.d)->backend != DE_GPU_METAL)
        SKIP("separate native Metal integrators only");
    pt_frame(f);
    u32 before = de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE);
    pt_frame(f);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before);
    ASSERT_TRUE(cvar_set("r.pt.fog.density", ".01"));
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before + 1);
    ASSERT_EQ(de__pt_prepare_media(f->rt.r), DE_OK);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before + 1);
    ASSERT_TRUE(cvar_set("r.pt.fog.density", "0"));
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before + 1);
    ASSERT_TRUE(cvar_set("r.pt.fog.density", ".01"));
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 1u);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before + 1);
    ASSERT_TRUE(cvar_set("r.pt.fog.density", "0"));
}

TEST_F(pathtracer, native_media_pipeline_failure_is_cached_without_losing_surface_pipeline,
       pt_fixture) {
    if (de_gpu_get_caps(f->rt.d)->backend != DE_GPU_METAL)
        SKIP("separate native Metal integrators only");
    pt_frame(f);
    u32 before = de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE);
    de_tracking_allocator_fail_after(&f->rt.dev_track, 0);
    de_result result = de__pt_prepare_media(f->rt.r);
    de_tracking_allocator_fail_after(&f->rt.dev_track, -1);
    EXPECT_NE(result, DE_OK);
    EXPECT_GT(de_tracking_allocator_stats(&f->rt.dev_track).fail_count, 0u);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before);
    EXPECT_EQ(de__pt_prepare_media(f->rt.r), result);
    EXPECT_EQ(de__gpu_live_count(f->rt.d, DE__GPU_PIPELINE), before);
    pt_frame(f);
    EXPECT_EQ(de_renderer_path_samples(f->rt.r, f->rt.view), 2u);
    EXPECT_FALSE(de_gpu_device_is_lost(f->rt.d));
}
