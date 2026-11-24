/*
 * test_sky_ibl.c: the sky as the IBL environment when there is no HDRI (DENG-838; DENG-206 AC4).
 * CPU: the SH9 projection of a face strip (a constant radiance L gives the irradiance pi L; a lit
 * +Z face lights an up normal, not a down one) and the staleness test (the 0.5 degree sun turn).
 * GPU, per backend built: the environment generated for the sun at noon and at sunset, whose
 * irradiance at an up normal changes (noon much brighter), with a finite prefiltered strip.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* setenv/snprintf */
#endif
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "render/lighting/lighting.h"
#include "world/sky/sky_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)120 * 1000000000u)
#define PI_F    3.14159265f

static float half_to_float(u16 h) {
    const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 bits;
    if (e == 0u) {
        if (m == 0u) {
            bits = s;
        } else {
            f32 v = (f32)m / 1024.0f / 16384.0f;
            memcpy(&bits, &v, sizeof bits);
            bits |= s;
        }
    } else if (e == 31u) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    f32 out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

static void irradiance_of(const f32 sh9[9][3], const f32 n[3], f32 out[3]) {
    de__ibl_environment env;
    memset(&env, 0, sizeof env);
    env.valid = true;
    env.face_size = 16;
    env.mip_count = 1;
    env.intensity = 1.0f;
    memcpy(env.sh9, sh9, sizeof env.sh9);
    de__ibl_gpu g;
    de__ibl_pack(&env, 1u, 1u, 1u, 0.0f, &g);
    de__ibl_irradiance(&g, n, out);
}

/* ---------- CPU ---------- */

TEST(sky_ibl, test_sky_ibl_sh_constant_radiance) {
    enum { N = 16 };
    static u16 texels[N * 6 * N * 4];
    for (u32 i = 0; i < N * 6 * N * 4; ++i)
        texels[i] = 0x3C00u; /* 1.0 */
    f32 sh[9][3];
    de__sky_ibl_project_sh(texels, N, N, sh);
    const f32 dirs[4][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0.6f, -0.8f, 0}};
    for (u32 d = 0; d < 4; ++d) {
        f32 e[3];
        irradiance_of(sh, dirs[d], e);
        for (u32 c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(e[c], PI_F, 0.01 * PI_F);
    }
}

TEST(sky_ibl, test_sky_ibl_sh_lit_up_face) {
    enum { N = 16 };
    static u16 texels[N * 6 * N * 4];
    memset(texels, 0, sizeof texels);
    for (u32 j = 0; j < N; ++j) /* face 4 is +Z (up) */
        for (u32 i = 0; i < N; ++i)
            for (u32 c = 0; c < 4; ++c)
                texels[((4u * N + j) * N + i) * 4u + c] = 0x3C00u;
    f32 sh[9][3];
    de__sky_ibl_project_sh(texels, N, N, sh);
    f32 up[3], down[3];
    irradiance_of(sh, (const f32[3]){0, 0, 1}, up);
    irradiance_of(sh, (const f32[3]){0, 0, -1}, down);
    EXPECT_TRUE(up[0] > 1.0f);
    EXPECT_TRUE(down[0] < 0.2f * up[0]);
}

TEST(sky_ibl, test_sky_ibl_stale_threshold) {
    de__sky_snapshot a;
    memset(&a, 0, sizeof a);
    a.valid = true;
    a.sun_dir = (de_vec3){0.0f, -0.70710678f, 0.70710678f};
    a.moon_dir = (de_vec3){0.0f, 0.0f, -1.0f};
    a.sun_weight = 120000.0f;
    a.mie_scale = 1.0f;
    a.albedo = 0.3f;
    de__sky_snapshot b = a;
    EXPECT_TRUE(de__sky_ibl_stale(NULL, 6360.0f, &a, 6360.0f));
    EXPECT_FALSE(de__sky_ibl_stale(&a, 6360.0f, &b, 6360.0f));
    /* 0.3 degrees: kept; 0.7 degrees: regenerated. */
    const f32 small = 0.3f * PI_F / 180.0f, large = 0.7f * PI_F / 180.0f;
    b.sun_dir = (de_vec3){sinf(small), -0.70710678f * cosf(small), 0.70710678f * cosf(small)};
    EXPECT_FALSE(de__sky_ibl_stale(&a, 6360.0f, &b, 6360.0f));
    b.sun_dir = (de_vec3){sinf(large), -0.70710678f * cosf(large), 0.70710678f * cosf(large)};
    EXPECT_TRUE(de__sky_ibl_stale(&a, 6360.0f, &b, 6360.0f));
    b = a;
    b.albedo = 0.5f;
    EXPECT_TRUE(de__sky_ibl_stale(&a, 6360.0f, &b, 6360.0f));
    b = a;
    EXPECT_TRUE(de__sky_ibl_stale(&a, 6360.0f, &b, 6360.5f));
}

/* ---------- GPU ---------- */

typedef struct ibl_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de__sky_luts *luts;
    de__sky_ibl *ibl;
    char why[512];
} ibl_fx;

static void ibl_fx_setup(ibl_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
}

static void ibl_fx_teardown(ibl_fx *f) {
    if (f->dev != NULL) {
        de_gpu_wait_idle(f->dev);
        de__sky_ibl_destroy(f->ibl);
        de__sky_luts_destroy(f->luts);
        de_gpu_device_destroy(f->dev);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

static bool backend_built(de_gpu_backend b) {
    switch (b) {
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN:
        return true;
#endif
#if DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return true;
#endif
    default:
        return false;
    }
}

static void ibl_open(ibl_fx *f, de_gpu_backend b) {
    if (!backend_built(b)) {
        snprintf(f->why, sizeof f->why, "the %s backend is not in this build",
                 de_gpu_backend_name(b));
        SKIP(f->why);
    }
#if defined(__APPLE__)
    if (b == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = true,
                               .gpu_validation = b == DE_GPU_VULKAN,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->dev);
    if (r == DE_ERR_UNSUPPORTED && b == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->dev);
    }
    if (r != DE_OK) {
        f->dev = NULL;
        snprintf(f->why, sizeof f->why, "no usable %s device: %.400s", de_gpu_backend_name(b),
                 de_error_last() ? de_error_last()->message : "?");
        SKIP(f->why);
    }
    const de_gpu_caps *caps = de_gpu_get_caps(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(f->why, sizeof f->why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(f->why);
    }
    ASSERT_EQ(de__sky_luts_create(f->dev, &f->alloc, &f->luts), DE_OK);
    ASSERT_EQ(de__sky_ibl_create(f->dev, &f->alloc, &f->ibl), DE_OK);
    if (!de__sky_luts_ready(f->luts) || !de__sky_ibl_ready(f->ibl))
        SKIP("no sky shader code for this backend");
}

/* The last mip (1 x 6, roughness 1) of `strip` as floats (rgba per face); false on failure. */
static bool read_last_mip(ibl_fx *f, de_gpu_texture strip, f32 out[6][4]) {
    const de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = 512u * 6u,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = "sky ibl readback"});
    if (!DE_HANDLE_VALID(rb))
        return false;
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "sky ibl readback");
    de_gpu_barrier bar;
    memset(&bar, 0, sizeof bar);
    bar.texture = strip;
    bar.before = DE_ACCESS_SHADER_READ;
    bar.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(c, &bar, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, 512u, strip, DE__SKY_IBL_MIPS - 1u, 0);
    bar.before = DE_ACCESS_COPY_SRC;
    bar.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &bar, 1);
    const de_gpu_fence done = de_gpu_fence_create(f->dev, 0);
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = done, .signal_value = 1};
    bool ok = false;
    if (de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s) == DE_OK &&
        de_gpu_fence_wait(f->dev, done, 1, WAIT_NS) == DE_OK) {
        const u8 *p = (const u8 *)de_gpu_buffer_map(f->dev, rb);
        if (p != NULL) {
            for (u32 face = 0; face < 6u; ++face) {
                const u16 *t = (const u16 *)(p + (size_t)face * 512u);
                for (u32 k = 0; k < 4u; ++k)
                    out[face][k] = half_to_float(t[k]);
            }
            ok = true;
        }
    }
    de_gpu_fence_destroy(f->dev, done);
    de_gpu_buffer_destroy(f->dev, rb);
    return ok;
}

static de__sky_snapshot sun_at(f32 elev_deg, f32 azimuth_deg) {
    const f32 e = elev_deg * PI_F / 180.0f, a = azimuth_deg * PI_F / 180.0f;
    de__sky_snapshot sn;
    memset(&sn, 0, sizeof sn);
    sn.valid = true;
    /* azimuth from north toward east; x east, y north, z up */
    sn.sun_dir = (de_vec3){cosf(e) * sinf(a), cosf(e) * cosf(a), sinf(e)};
    sn.moon_dir = (de_vec3){0.0f, 0.0f, -1.0f};
    sn.sun_weight = 120000.0f;
    sn.moon_weight = 0.0f;
    sn.mie_scale = 1.0f;
    sn.albedo = 0.3f;
    return sn;
}

/* Generates for `sn` and returns the irradiance (rgb) at an up normal, and the strip. */
static void generate(ibl_fx *f, const de__sky_snapshot *sn, f32 up[3], de__ibl_sky *cur) {
    const f32 r_km = (f32)(DE__SKY_BOTTOM + 0.002);
    ASSERT_TRUE(de__sky_ibl_wants(f->ibl, sn, r_km));
    ASSERT_EQ(de__sky_ibl_generate(f->ibl, f->luts, sn, r_km), DE_OK);
    EXPECT_TRUE(de__sky_ibl_pending(f->ibl));
    EXPECT_EQ(de__sky_ibl_generate(f->ibl, f->luts, sn, r_km), DE_ERR_BUSY);
    de_error_clear();
    ASSERT_TRUE(de__sky_ibl_poll(f->ibl, true));
    EXPECT_FALSE(de__sky_ibl_wants(f->ibl, sn, r_km));
    de__sky_ibl_current(f->ibl, cur);
    ASSERT_TRUE(cur->valid);
    irradiance_of(cur->sh9, (const f32[3]){0, 0, 1}, up);
}

static void noon_vs_sunset(ibl_fx *f, de_gpu_backend b) {
    ibl_open(f, b);
    ASSERT_EQ(de__sky_luts_update(f->luts, 1.0f, 0.3f), DE_OK);
    const de__sky_snapshot noon = sun_at(45.0f, 180.0f), sunset = sun_at(2.0f, 270.0f);
    f32 e_noon[3], e_sunset[3];
    de__ibl_sky a, s;
    generate(f, &noon, e_noon, &a);
    f32 m_noon[6][4];
    ASSERT_TRUE(read_last_mip(f, a.strip, m_noon));
    generate(f, &sunset, e_sunset, &s);
    EXPECT_NE(a.strip.id, s.strip.id); /* double-buffered */
    f32 m_sunset[6][4];
    ASSERT_TRUE(read_last_mip(f, s.strip, m_sunset));
    printf("sky IBL irradiance (up, lux): noon %.0f %.0f %.0f, sunset %.0f %.0f %.0f\n",
           (double)e_noon[0], (double)e_noon[1], (double)e_noon[2], (double)e_sunset[0],
           (double)e_sunset[1], (double)e_sunset[2]);
    for (u32 c = 0; c < 3; ++c) {
        EXPECT_TRUE(isfinite(e_noon[c]) && isfinite(e_sunset[c]));
        EXPECT_TRUE(e_sunset[c] > 0.0f);
        /* A clear noon sky gives some 10-30 klx of diffuse light at the ground; sunset far less. */
        EXPECT_TRUE(e_noon[c] > 2000.0f);
        EXPECT_TRUE(e_noon[c] > 1.5f * e_sunset[c]);
    }
    /* The luminance: noon at least three times sunset. */
    const f32 y_noon = 0.2126f * e_noon[0] + 0.7152f * e_noon[1] + 0.0722f * e_noon[2];
    const f32 y_sunset = 0.2126f * e_sunset[0] + 0.7152f * e_sunset[1] + 0.0722f * e_sunset[2];
    EXPECT_TRUE(y_noon > 3.0f * y_sunset);
    /* Sunset: the sky is redder than at noon (blue / red falls). */
    EXPECT_TRUE(e_sunset[2] / e_sunset[0] < e_noon[2] / e_noon[0]);
    /* The roughest mip: finite, positive, the up face brighter than the down one at noon. */
    for (u32 face = 0; face < 6u; ++face)
        for (u32 c = 0; c < 3u; ++c) {
            EXPECT_TRUE(isfinite(m_noon[face][c]) && m_noon[face][c] >= 0.0f);
            EXPECT_TRUE(isfinite(m_sunset[face][c]) && m_sunset[face][c] >= 0.0f);
        }
    printf("sky IBL roughest mip (+Z, -Z): noon %.0f %.0f %.0f / %.0f %.0f %.0f, sunset %.0f %.0f "
           "%.0f / %.0f %.0f %.0f\n",
           (double)m_noon[4][0], (double)m_noon[4][1], (double)m_noon[4][2], (double)m_noon[5][0],
           (double)m_noon[5][1], (double)m_noon[5][2], (double)m_sunset[4][0],
           (double)m_sunset[4][1], (double)m_sunset[4][2], (double)m_sunset[5][0],
           (double)m_sunset[5][1], (double)m_sunset[5][2]);
    /* The sky above is bluer than the sunlit ground below (albedo 0.3, grey). */
    EXPECT_TRUE(m_noon[4][2] / m_noon[4][0] > m_noon[5][2] / m_noon[5][0]);
    EXPECT_TRUE(m_noon[4][1] > m_sunset[4][1]);
}

#define IBL_GPU_TESTS(sfx, backend)                                                                \
    TEST_F(sky_ibl, test_sky_ibl_noon_vs_sunset_##sfx, ibl_fx) {                                   \
        noon_vs_sunset(f, backend);                                                                \
    }

IBL_GPU_TESTS(metal, DE_GPU_METAL)
IBL_GPU_TESTS(vulkan, DE_GPU_VULKAN)
IBL_GPU_TESTS(d3d12, DE_GPU_D3D12)
