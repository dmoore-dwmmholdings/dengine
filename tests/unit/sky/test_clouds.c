/*
 * test_clouds.c: the CPU side of the volumetric clouds (DENG-208, S34). DeSky's cloud fields
 * resolved into the layer settings and the quality table; the pass list on a null-backend device
 * (clouds off, coverage 0 or volumetrics OFF add no cloud pass at all, High adds the shadow map and
 * the three view passes, and later frames allocate nothing); and the weather-map offset at large
 * world coordinates (double precision, no f32 drift).
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/graph/rg_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "world/sky/clouds_internal.h"

#include <math.h>
#include <string.h>

TEST(clouds, test_clouds_settings_reflect) {
    DeSky sky;
    memset(&sky, 0, sizeof sky);
    sky.clouds = true;
    sky.cloud_coverage = 0.7f;
    de__clouds_settings c = de__clouds_resolve(&sky);
    /* Zero fields take the defaults. */
    EXPECT_TRUE(c.on);
    EXPECT_FLOAT_EQ(c.coverage, 0.7, 1e-7);
    EXPECT_FLOAT_EQ(c.bottom_m, DE__CLOUDS_BOTTOM_M, 0);
    EXPECT_FLOAT_EQ(c.top_m, DE__CLOUDS_TOP_M, 0);
    EXPECT_FLOAT_EQ(c.density, 1.0, 0);
    EXPECT_FLOAT_EQ(c.erosion, DE__CLOUDS_EROSION, 0);
    EXPECT_FLOAT_EQ(c.wind_x, 0.0, 0);
    EXPECT_FLOAT_EQ(c.wind_y, 0.0, 0);

    /* Authored values pass through; out-of-range ones clamp. */
    sky.cloud_coverage = 3.0f;
    sky.cloud_bottom_m = 2000.0f;
    sky.cloud_top_m = 2010.0f; /* thinner than the minimum */
    sky.cloud_density = 0.25f;
    sky.cloud_erosion = 2.0f;
    sky.cloud_wind_dir_deg = 90.0f; /* toward the east (+x) */
    sky.cloud_wind_speed_mps = 10.0f;
    c = de__clouds_resolve(&sky);
    EXPECT_FLOAT_EQ(c.coverage, 1.0, 0);
    EXPECT_FLOAT_EQ(c.bottom_m, 2000.0, 0);
    EXPECT_FLOAT_EQ(c.top_m, 2000.0 + DE__CLOUDS_MIN_DEPTH_M, 0);
    EXPECT_FLOAT_EQ(c.density, 0.25, 0);
    EXPECT_FLOAT_EQ(c.erosion, 1.0, 0);
    EXPECT_FLOAT_EQ(c.wind_x, 10.0, 1e-5);
    EXPECT_FLOAT_EQ(c.wind_y, 0.0, 1e-5);

    /* Enabled: on, some coverage, volumetrics not OFF. */
    de_render_settings rs = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    EXPECT_TRUE(de__clouds_enabled(&c, &rs));
    EXPECT_FALSE(de__clouds_enabled(&c, NULL));
    rs.volumetrics = DE_QUALITY_OFF;
    EXPECT_FALSE(de__clouds_enabled(&c, &rs));
    rs.volumetrics = DE_QUALITY_HIGH;
    c.coverage = 0.0f;
    EXPECT_FALSE(de__clouds_enabled(&c, &rs));
    c.coverage = 0.5f;
    c.on = false;
    EXPECT_FALSE(de__clouds_enabled(&c, &rs));

    /* The quality table. */
    const de__clouds_quality hi = de__clouds_quality_for(DE_QUALITY_HIGH);
    EXPECT_EQ(hi.primary_steps, 96u);
    EXPECT_EQ(hi.light_steps, 6u);
    EXPECT_EQ(hi.octaves, 3u);
    EXPECT_EQ(de__clouds_quality_for(DE_QUALITY_LOW).primary_steps, 48u);
    EXPECT_EQ(de__clouds_quality_for(DE_QUALITY_ULTRA).primary_steps, 128u);
    EXPECT_EQ(de__clouds_quality_for(DE_QUALITY_OFF).primary_steps, 0u);
    /* The GPU header layouts the shaders read. */
    EXPECT_EQ((u32)sizeof(de__clouds_header), 304u);
    EXPECT_EQ((u32)sizeof(de__cloud_shadow_gpu), 64u);
}

static u32 cloud_passes(const de_rg *g) {
    u32 n = 0;
    for (u32 i = 0; i < g->pass_count; ++i)
        if (strncmp(g->passes[i]->name, "clouds.", 7) == 0)
            ++n;
    return n;
}

TEST(clouds, test_clouds_disabled_culls_passes) {
    de_tracking_allocator track;
    const de_allocator alloc = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_gpu_device *dev = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &alloc},
                                   &dev),
              DE_OK);
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "clouds_test"}, &arena), DE_OK);
    de_rg *g = NULL;
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &alloc, .name = "clouds"}, &g), DE_OK);
    de__clouds *clouds = NULL;
    ASSERT_EQ(de__clouds_create(dev, &alloc, &clouds), DE_OK);
    ASSERT_TRUE(de__clouds_ready(clouds));

    const u32 w = 64u, h = 36u;
    de_gpu_texture depth = de_gpu_texture_create(
        dev, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT, .width = w, .height = h,
                                    .usage = DE_USAGE_DEPTH | DE_USAGE_SAMPLED,
                                    .name = "depth"});
    de_gpu_texture hdr = de_gpu_texture_create(
        dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT, .width = w, .height = h,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                    .name = "hdr"});
    ASSERT_TRUE(DE_HANDLE_VALID(depth) && DE_HANDLE_VALID(hdr));

    DeSky sky;
    memset(&sky, 0, sizeof sky);
    sky.clouds = true;
    sky.cloud_coverage = 0.5f;
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    de__clouds_inputs in;
    memset(&in, 0, sizeof in);
    in.frame.settings = &settings;
    in.frame.clouds = de__clouds_resolve(&sky);
    in.frame.camera_world[2] = 2.0;
    in.frame.camera = (de_vec3){0.0f, 0.0f, 2.0f};
    in.frame.sun_dir = (de_vec3){0.0f, 0.6f, 0.8f};
    in.frame.sun_illuminance = 100000.0f;
    in.width = w;
    in.height = h;
    in.view_matrix = de_m4_identity();
    in.proj = de_m4_perspective_rz(1.0f, (f32)w / (f32)h, 0.1f);
    in.prev_view_proj = in.proj;
    in.ray00 = (de_vec3){-1.0f, 1.0f, -1.0f};
    in.ray_du = (de_vec3){2.0f, 0.0f, 0.0f};
    in.ray_dv = (de_vec3){0.0f, -2.0f, 0.0f};
    in.history_ok = true;
    in.pre_exposure = 1.0f;

    /* Frame 0 clouds off, 1 coverage 0, 2 volumetrics OFF; then High. */
    de_tracking_stats warm = {0};
    for (u32 frame = 0; frame < 6u; ++frame) {
        in.frame.clouds.on = frame != 0u;
        in.frame.clouds.coverage = frame == 1u ? 0.0f : 0.5f;
        settings.volumetrics = frame == 2u ? DE_QUALITY_OFF : DE_QUALITY_HIGH;
        in.frame.frame_index = frame;
        ASSERT_EQ(de__rg_begin(g, arena), DE_OK);
        in.depth = de_rg_import_texture(g, depth, DE_ACCESS_DEPTH_READ);
        in.target = de_rg_import_texture(g, hdr, DE_ACCESS_SHADER_READ);
        de__clouds_shadow_output so;
        const u32 shadow = de__clouds_add_shadow_pass(clouds, g, arena, &in.frame, &so);
        const u32 view = de__clouds_add_passes(clouds, g, arena, &in);
        if (frame < 3u) {
            EXPECT_EQ(shadow, 0u);
            EXPECT_EQ(view, 0u);
            EXPECT_EQ(cloud_passes(g), 0u);
            EXPECT_FALSE(DE_HANDLE_VALID(so.texture));
        } else {
            EXPECT_EQ(shadow, 1u);
            EXPECT_EQ(view, 3u);
            EXPECT_EQ(cloud_passes(g), 4u);
            EXPECT_TRUE(DE_HANDLE_VALID(so.texture));
            EXPECT_TRUE(so.header != 0u);
            EXPECT_EQ(so.entry, (u32)(frame % DE__VIS_FRAME_RING));
            EXPECT_EQ(g->error_count, 0u);
        }
        de__rg_reset(g);
        de_arena_reset(arena);
        if (frame == 4u)
            warm = de_tracking_allocator_stats(&track);
        if (frame == 5u) /* no heap allocation once the view's resources exist */
            EXPECT_EQ(de_tracking_allocator_stats(&track).alloc_count, warm.alloc_count);
    }
    /* An orthographic view has no clouds. */
    ASSERT_EQ(de__rg_begin(g, arena), DE_OK);
    in.depth = de_rg_import_texture(g, depth, DE_ACCESS_DEPTH_READ);
    in.target = de_rg_import_texture(g, hdr, DE_ACCESS_SHADER_READ);
    in.proj = de_m4_identity();
    EXPECT_EQ(de__clouds_add_passes(clouds, g, arena, &in), 0u);
    de__rg_reset(g);
    de_arena_reset(arena);

    de__clouds_destroy(clouds);
    de__rg_destroy(g);
    de_gpu_texture_destroy(dev, depth);
    de_gpu_texture_destroy(dev, hdr);
    de_gpu_device_destroy(dev);
    de_arena_destroy(arena);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

TEST(clouds, test_clouds_weather_offset_large_coords) {
    const f64 tile = DE__CLOUDS_WEATHER_TILE_M;
    /* In [0, tile) for any input, negative included. */
    EXPECT_FLOAT_EQ(de__clouds_tile_offset(0.0, 0.0, tile), 0.0, 0);
    EXPECT_FLOAT_EQ(de__clouds_tile_offset(-1.0, 0.0, tile), tile - 1.0, 1e-3);
    EXPECT_FLOAT_EQ(de__clouds_tile_offset(tile * 3.0 + 5.0, 0.0, tile), 5.0, 1e-6);
    EXPECT_FLOAT_EQ(de__clouds_tile_offset(1.0, 0.0, 0.0), 0.0, 0);
    /* Far from the origin (10 000 km, where an f32 metre is 1 m apart) the offset still moves by
     * the camera's centimetre steps: double precision before the reduction. */
    const f64 far = 1.0e10 + 0.25;
    const f32 a = de__clouds_tile_offset(far, 0.0, tile);
    const f32 b = de__clouds_tile_offset(far + 0.01, 0.0, tile);
    EXPECT_TRUE(a >= 0.0f && a < (f32)tile);
    EXPECT_FLOAT_EQ((f64)b - (f64)a, 0.01, 2e-3);
    /* The drift wraps the same way: world + drift. */
    EXPECT_FLOAT_EQ(de__clouds_tile_offset(far, 100.0, tile),
                    de__clouds_tile_offset(far + 100.0, 0.0, tile), 1e-3);
    /* At +-100 km (the issue's coordinates): the values in [0, tile), a 1 cm step stays 1 cm (to an
     * f32 ulp), and -x wraps to tile - (x mod tile). */
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const f64 x = (f64)sgn * 100000.0 + 0.37;
        const f32 o = de__clouds_tile_offset(x, 0.0, tile);
        EXPECT_TRUE(o >= 0.0f && o < (f32)tile);
        /* 4e-3: an f32 ulp at 40 km. */
        EXPECT_FLOAT_EQ((f64)de__clouds_tile_offset(x + 0.01, 0.0, tile) - (f64)o, 0.01, 4e-3);
        EXPECT_FLOAT_EQ((f64)o, x - tile * floor(x / tile), 4e-3);
    }
    /* The exact value at a large coordinate. */
    const f64 want = fmod(far, tile);
    EXPECT_FLOAT_EQ((f64)a, want, 1e-2);
}

/* DENG-852: the blue-noise tile is a rank permutation (every value 16 times), deterministic, and
 * decorrelated: neighbours differ more than white noise's mean 85.3 and the tile wraps. */
TEST(clouds, test_clouds_blue_noise_tile) {
    enum { N = DE__CLOUDS_BLUE_N };
    static u8 a[N * N], b[N * N];
    de__clouds_blue_noise(a);
    de__clouds_blue_noise(b);
    EXPECT_TRUE(memcmp(a, b, sizeof a) == 0);
    u32 hist[256] = {0};
    for (u32 i = 0; i < N * N; ++i)
        ++hist[a[i]];
    for (u32 v = 0; v < 256u; ++v)
        EXPECT_EQ(hist[v], (N * N) / 256u);
    f64 diff = 0.0;
    for (u32 y = 0; y < N; ++y)
        for (u32 x = 0; x < N; ++x) {
            const int p = a[y * N + x];
            diff += fabs((f64)(p - (int)a[y * N + (x + 1u) % N]));
            diff += fabs((f64)(p - (int)a[((y + 1u) % N) * N + x]));
            /* A 4x4 box of a blue tile is near the mean: little low-frequency energy. */
        }
    diff /= 2.0 * N * N;
    EXPECT_TRUE(diff > 100.0);
    f64 var = 0.0;
    for (u32 by = 0; by < N; by += 4u)
        for (u32 bx = 0; bx < N; bx += 4u) {
            f64 s = 0.0;
            for (u32 j = 0; j < 4u; ++j)
                for (u32 i = 0; i < 4u; ++i)
                    s += a[(by + j) * N + bx + i];
            s /= 16.0;
            var += (s - 127.5) * (s - 127.5);
        }
    var /= (N / 4) * (N / 4);
    /* White noise: the 4x4 mean has sigma 73.9 / 4 = 18.5 (variance 341); blue is far lower. */
    EXPECT_TRUE(var < 150.0);
}
