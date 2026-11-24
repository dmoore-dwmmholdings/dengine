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
            EXPECT_EQ(view, 5u); /* march, reconstruct, composite, ground, its readback */
            EXPECT_EQ(cloud_passes(g), 6u);
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

/* DENG-854: the fine cascade is on from Medium, its detail noise from High. */
TEST(clouds, test_clouds_shadow_quality_gating) {
    EXPECT_FALSE(de__clouds_quality_for(DE_QUALITY_OFF).shadow_fine);
    EXPECT_FALSE(de__clouds_quality_for(DE_QUALITY_OFF).shadow_detail);
    EXPECT_FALSE(de__clouds_quality_for(DE_QUALITY_LOW).shadow_fine);
    EXPECT_FALSE(de__clouds_quality_for(DE_QUALITY_LOW).shadow_detail);
    EXPECT_TRUE(de__clouds_quality_for(DE_QUALITY_MEDIUM).shadow_fine);
    EXPECT_FALSE(de__clouds_quality_for(DE_QUALITY_MEDIUM).shadow_detail);
    EXPECT_TRUE(de__clouds_quality_for(DE_QUALITY_HIGH).shadow_fine);
    EXPECT_TRUE(de__clouds_quality_for(DE_QUALITY_HIGH).shadow_detail);
    EXPECT_TRUE(de__clouds_quality_for(DE_QUALITY_ULTRA).shadow_fine);
    EXPECT_TRUE(de__clouds_quality_for(DE_QUALITY_ULTRA).shadow_detail);
    /* The fine cascade is finer than the coarse one and lies inside it. */
    EXPECT_TRUE(DE__CLOUDS_SHADOW_FINE_EXTENT_M < DE__CLOUDS_SHADOW_EXTENT_M);
    EXPECT_FLOAT_EQ(DE__CLOUDS_SHADOW_FINE_EXTENT_M / (f32)DE__CLOUDS_SHADOW_N, 8.0f, 1e-4f);
}

/* DENG-854: which cascade covers a point, and the blend at the fine cascade's boundary. */
TEST(clouds, test_clouds_shadow_cascade_blend) {
    /* Outside the area: none. Inside, further than the fade from every edge: all. */
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(-0.01f, 0.5f), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.5f, 1.01f), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.5f, 0.5f), 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.1f, 0.9f), 1.0f, 1e-5f);
    /* The edge itself is 0 and the fade is linear, continuous across the whole width. */
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.0f, 0.5f), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.05f, 0.5f), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.95f, 0.5f), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.05f, 0.05f), 0.25f, 1e-5f);
    f32 prev = 0.0f;
    for (u32 i = 0; i <= 100u; ++i) {
        const f32 w = de__clouds_shadow_weight((f32)i / 100.0f, 0.5f);
        EXPECT_TRUE(w >= prev - 0.11f && w <= prev + 0.11f); /* 0.1 per percent of width */
        prev = w;
    }

    /* The blend: the coarse value at weight 0, the fine one at 1, the mix between. */
    EXPECT_FLOAT_EQ(de__clouds_shadow_blend(0.8f, 0.2f, 0.0f), 0.8f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_blend(0.8f, 0.2f, 1.0f), 0.2f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_blend(0.8f, 0.2f, 0.5f), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_blend(0.8f, 0.2f, 2.0f), 0.2f, 1e-6f);
    EXPECT_FLOAT_EQ(de__clouds_shadow_blend(0.8f, 0.2f, -1.0f), 0.8f, 1e-6f);

    /* Selection along a line through the camera: 1 km each side of the fine centre is its edge.
     * At the camera the fine cascade owns the point; at the fine edge the coarse one does; 1.2 km
     * from the camera (inside the coarse cascade, outside the fine one) only the coarse one. */
    const f32 fine_half = DE__CLOUDS_SHADOW_FINE_EXTENT_M * 0.5f;
    const f32 coarse_half = DE__CLOUDS_SHADOW_EXTENT_M * 0.5f;
    f32 d = 0.0f;
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.5f + d / DE__CLOUDS_SHADOW_FINE_EXTENT_M, 0.5f), 1.0f,
                1e-6f);
    d = fine_half;
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.5f + d / DE__CLOUDS_SHADOW_FINE_EXTENT_M, 0.5f), 0.0f,
                1e-5f);
    d = 1.2f * fine_half;
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.5f + d / DE__CLOUDS_SHADOW_FINE_EXTENT_M, 0.5f), 0.0f,
                1e-6f);
    EXPECT_TRUE(de__clouds_shadow_weight(0.5f + d / DE__CLOUDS_SHADOW_EXTENT_M, 0.5f) > 0.99f);
    d = coarse_half * 1.01f;
    EXPECT_FLOAT_EQ(de__clouds_shadow_weight(0.5f + d / DE__CLOUDS_SHADOW_EXTENT_M, 0.5f), 0.0f, 1e-6f);
}

/* DENG-854: both cascades snap to their own texel in world space. */
TEST(clouds, test_clouds_shadow_snap) {
    const f64 e[2] = {DE__CLOUDS_SHADOW_EXTENT_M, DE__CLOUDS_SHADOW_FINE_EXTENT_M};
    for (u32 k = 0; k < 2u; ++k) {
        const f64 texel = e[k] / (f64)DE__CLOUDS_SHADOW_N;
        const f64 cams[4] = {0.0, 1234.567, -987.65, 3.0e6 + 0.3};
        for (u32 i = 0; i < 4u; ++i) {
            const f32 off = de__clouds_shadow_snap(cams[i], 0.0, e[k], DE__CLOUDS_SHADOW_N);
            /* The centre lies on a texel edge of the world grid, at or below the camera, less
             * than one texel from it. */
            EXPECT_TRUE(off <= 0.0f && off > -(f32)texel - 1e-3f);
            const f64 centre = cams[i] + (f64)off;
            const f64 r = fmod(fabs(centre), texel);
            EXPECT_TRUE(fmin(r, texel - r) < 0.5);
        }
        /* Moving less than a texel keeps the centre (no shimmer). */
        const f64 c0 = 100.0 * texel + 0.25 * texel;
        const f64 s0 = c0 + (f64)de__clouds_shadow_snap(c0, 0.0, e[k], DE__CLOUDS_SHADOW_N);
        const f64 c1 = c0 + 0.5 * texel;
        const f64 s1 = c1 + (f64)de__clouds_shadow_snap(c1, 0.0, e[k], DE__CLOUDS_SHADOW_N);
        EXPECT_FLOAT_EQ((f32)(s1 - s0), 0.0f, 1e-3f);
    }
}

/* DENG-854: the fine cascade follows the sun's crossing of the layer bottom. */
TEST(clouds, test_clouds_shadow_sun_shift) {
    /* Sun 45 degrees up toward +x: a camera 500 m below a 1500 m layer shifts 1000 m. */
    const f64 s = 0.70710678118654752;
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(s, s, 500.0, 1500.0), 1000.0f, 0.01f);
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(0.0, s, 500.0, 1500.0), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(-s, s, 500.0, 1500.0), -1000.0f, 0.01f);
    /* A low sun would shift it past the coarse cascade: clamped to stay inside. */
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(0.99, 0.1, 0.0, 1500.0),
                    DE__CLOUDS_SHADOW_SHIFT_MAX_M, 0.01f);
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(-0.99, 0.1, 0.0, 1500.0),
                    -DE__CLOUDS_SHADOW_SHIFT_MAX_M, 0.01f);
    EXPECT_FLOAT_EQ(DE__CLOUDS_SHADOW_SHIFT_MAX_M + DE__CLOUDS_SHADOW_FINE_EXTENT_M * 0.5f,
                    DE__CLOUDS_SHADOW_EXTENT_M * 0.5f, 1e-3f);
    /* A camera above the layer, and a sun at the horizon: no shift. */
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(s, s, 2000.0, 1500.0), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ((f32)de__clouds_shadow_shift(1.0, 0.01, 0.0, 1500.0), 0.0f, 1e-6f);
    /* The shifted centre is snapped to the texel grid and the receiver is inside the area. */
    const f64 ext = DE__CLOUDS_SHADOW_FINE_EXTENT_M, texel = ext / (f64)DE__CLOUDS_SHADOW_N;
    const f64 cam = 12345.678;
    const f64 shift = 1000.0;
    const f32 off = de__clouds_shadow_snap(cam, shift, ext, DE__CLOUDS_SHADOW_N);
    EXPECT_TRUE(fabs((double)off - shift) <= texel);
    const f64 u = (cam + shift - (cam + (f64)off)) / ext + 0.5; /* the receiver's crossing */
    EXPECT_TRUE(de__clouds_shadow_weight((f32)u, 0.5f) > 0.99f);
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

/* DENG-867: the cascades' centres: the DENG-854 values while they fit and cover the receivers,
 * the receivers' crossing covered below a high camera, and the coarse cascade following a low
 * sun. */
TEST(clouds, test_clouds_shadow_centres) {
    const f64 s = 0.70710678118654752, r = DE__CLOUDS_SHADOW_FINE_INNER_M,
              m = DE__CLOUDS_SHADOW_SHIFT_MAX_M;
    f64 fine = -1.0, coarse = -1.0;
    EXPECT_FLOAT_EQ(DE__CLOUDS_SHADOW_FINE_INNER_M, 819.2f, 1e-3f);
    /* No target: the DENG-854 shift, the coarse cascade on the camera. */
    de__clouds_shadow_centres(s, s, 500.0, 1500.0, false, 0.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, 1000.0f, 0.01f);
    EXPECT_FLOAT_EQ((f32)coarse, 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ((f32)fine, (f32)de__clouds_shadow_shift(s, s, 500.0, 1500.0), 1e-6f);
    /* A target already inside the fully weighted part: unchanged. */
    de__clouds_shadow_centres(s, s, 600.0, 1500.0, true, 900.0 + 800.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, 900.0f, 0.01f);
    EXPECT_FLOAT_EQ((f32)coarse, 0.0f, 1e-6f);
    /* A target 1400 m beyond: the fine cascade moves just enough to cover it fully. */
    de__clouds_shadow_centres(s, s, 1400.0, 1500.0, true, 1500.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, (f32)(1500.0 - r), 0.01f);
    EXPECT_FLOAT_EQ((f32)coarse, 0.0f, 1e-6f);
    {
        const f64 u = (1500.0 - fine) / (f64)DE__CLOUDS_SHADOW_FINE_EXTENT_M + 0.5;
        EXPECT_TRUE(de__clouds_shadow_weight((f32)u, 0.5f) > 0.99f);
    }
    de__clouds_shadow_centres(-s, s, 3000.0, 1500.0, true, -1500.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, (f32)(-1500.0 + r), 0.01f);
    /* A low sun (5.7 degrees): the fine cascade on the camera's crossing, 14.8 km out, and the
     * coarse cascade moved by the excess so that it still holds the fine one. */
    de__clouds_shadow_centres(0.99, 0.1, 0.0, 1500.0, false, 0.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, 14850.0f, 0.1f);
    EXPECT_FLOAT_EQ((f32)coarse, (f32)(14850.0 - m), 0.1f);
    EXPECT_TRUE(fabs(fine - coarse) <= m + 1e-6);
    de__clouds_shadow_centres(-0.99, 0.1, 0.0, 1500.0, true, -20000.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, (f32)(-20000.0 + r), 0.1f);
    EXPECT_FLOAT_EQ((f32)coarse, (f32)(-20000.0 + r + m), 0.1f);
    /* The sun at the horizon without a target, and a NaN target: as DENG-854. */
    de__clouds_shadow_centres(1.0, 0.01, 0.0, 1500.0, false, 0.0, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ((f32)coarse, 0.0f, 1e-6f);
    de__clouds_shadow_centres(s, s, 1400.0, 1500.0, true, NAN, &fine, &coarse);
    EXPECT_FLOAT_EQ((f32)fine, 100.0f, 0.01f);
}

/* DENG-867: the receivers' crossing from the depth samples. */
TEST(clouds, test_clouds_receiver_crossing) {
    f32 s[DE__CLOUDS_GROUND_SAMPLES * 4u];
    memset(s, 0, sizeof s);
    const f32 k = 0.70710678f;
    const de_vec3 sun = {0.0f, k, k}; /* 45 degrees up toward +y */
    f64 x = 7.0, y = 7.0;
    EXPECT_FALSE(de__clouds_receiver_crossing(s, DE__CLOUDS_GROUND_SAMPLES, 1400.0, sun, 1500.0,
                                              &x, &y)); /* all sky */
    EXPECT_FALSE(de__clouds_receiver_crossing(NULL, 4u, 1400.0, sun, 1500.0, &x, &y));
    EXPECT_FLOAT_EQ((f32)x, 7.0f, 0.0f);
    /* One hit on the ground 1400 m below and 2000 m south: it crosses 1500 m north of itself. */
    s[0] = 0.0f, s[1] = -2000.0f, s[2] = -1400.0f, s[3] = 1.0f;
    ASSERT_TRUE(de__clouds_receiver_crossing(s, DE__CLOUDS_GROUND_SAMPLES, 1400.0, sun, 1500.0,
                                             &x, &y));
    EXPECT_FLOAT_EQ((f32)x, 0.0f, 1e-3f);
    EXPECT_FLOAT_EQ((f32)y, -500.0f, 0.05f);
    /* The sun at the horizon: none. */
    EXPECT_FALSE(de__clouds_receiver_crossing(s, DE__CLOUDS_GROUND_SAMPLES, 1400.0,
                                              (de_vec3){1.0f, 0.0f, 0.01f}, 1500.0, &x, &y));
    /* Ground at four distances and two far mountains: the nearer half's median per axis. */
    const f32 pts[6][3] = {{100, -2000, -1400}, {-100, -2500, -1400}, {0, -3000, -1400},
                           {0, -5000, -1400},   {0, -40000, 600},     {0, -30000, 600}};
    memset(s, 0, sizeof s);
    for (u32 i = 0; i < 6u; ++i) {
        s[i * 8u + 0u] = pts[i][0];
        s[i * 8u + 1u] = pts[i][1];
        s[i * 8u + 2u] = pts[i][2];
        s[i * 8u + 3u] = 1.0f; /* every other sample is sky */
    }
    ASSERT_TRUE(de__clouds_receiver_crossing(s, DE__CLOUDS_GROUND_SAMPLES, 1400.0, sun, 1500.0,
                                             &x, &y));
    EXPECT_FLOAT_EQ((f32)x, 0.0f, 1e-3f);
    EXPECT_FLOAT_EQ((f32)y, -1000.0f, 0.05f); /* -2500 + 1500 */
    /* A non-finite hit is skipped. */
    s[1u * 4u + 0u] = NAN, s[1u * 4u + 2u] = 5.0f, s[1u * 4u + 3u] = 1.0f;
    ASSERT_TRUE(de__clouds_receiver_crossing(s, DE__CLOUDS_GROUND_SAMPLES, 1400.0, sun, 1500.0,
                                             &x, &y));
    EXPECT_FLOAT_EQ((f32)y, -1000.0f, 0.05f);
}
