/*
 * test_atmosphere_cpu.c: the GPU transmittance LUT against the double-precision CPU reference
 * (DENG-206 AC: within 1 %), and the multi-scattering LUT's sanity, on each GPU backend built.
 * DENG-840: the aerial-perspective volume's depth mapping (CPU), and the volume built on the GPU:
 * its transmittance against the double-precision reference along each froxel ray (within 1 %),
 * its in-scatter finite, non-negative and growing with the distance.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* setenv/snprintf */
#endif
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "world/sky/sky_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)120 * 1000000000u)

typedef struct atm_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de__sky_luts *luts;
    char why[512];
} atm_fx;

static void atm_fx_setup(atm_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
}

static void atm_fx_teardown(atm_fx *f) {
    if (f->dev != NULL) {
        de_gpu_wait_idle(f->dev);
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

static void atm_open(atm_fx *f, de_gpu_backend b) {
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
    if (!de__sky_luts_ready(f->luts))
        SKIP("no sky shader code for this backend");
}

/* Copies `tex` (RGBA32F, `w` x `h`) to a new heap array; NULL on failure. */
static float *read_texture(atm_fx *f, de_gpu_texture tex, u32 w, u32 h) {
    const u32 pitch = w * 16u;
    const de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * h,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = "sky lut readback"});
    if (!DE_HANDLE_VALID(rb))
        return NULL;
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "sky lut readback");
    de_gpu_barrier bar;
    memset(&bar, 0, sizeof bar);
    bar.texture = tex;
    bar.before = DE_ACCESS_SHADER_READ;
    bar.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(c, &bar, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, tex, 0, 0);
    bar.before = DE_ACCESS_COPY_SRC;
    bar.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &bar, 1);
    const de_gpu_fence done = de_gpu_fence_create(f->dev, 0);
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = done, .signal_value = 1};
    float *out = NULL;
    if (de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s) == DE_OK &&
        de_gpu_fence_wait(f->dev, done, 1, WAIT_NS) == DE_OK) {
        out = (float *)malloc((size_t)pitch * h);
        if (out != NULL)
            memcpy(out, de_gpu_buffer_map(f->dev, rb), (size_t)pitch * h);
    }
    de_gpu_fence_destroy(f->dev, done);
    de_gpu_buffer_destroy(f->dev, rb);
    return out;
}

static void transmittance_cpu_vs_gpu(atm_fx *f, de_gpu_backend b) {
    atm_open(f, b);
    ASSERT_EQ(de__sky_luts_update(f->luts, 1.0f, 0.3f), DE_OK);
    u64 v = 0;
    const de_gpu_fence fence = de__sky_luts_fence(f->luts, &v);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, v, WAIT_NS), DE_OK);

    float *t = read_texture(f, de__sky_luts_transmittance(f->luts), DE__SKY_TRANSMITTANCE_W,
                            DE__SKY_TRANSMITTANCE_H);
    ASSERT_TRUE(t != NULL);
    f64 worst_rel = 0.0;
    u32 bad = 0;
    for (u32 j = 0; j < DE__SKY_TRANSMITTANCE_H; ++j)
        for (u32 i = 0; i < DE__SKY_TRANSMITTANCE_W; ++i) {
            f64 r, mu, ref[3];
            de__sky_transmittance_rmu(((f64)i + 0.5) / DE__SKY_TRANSMITTANCE_W,
                                      ((f64)j + 0.5) / DE__SKY_TRANSMITTANCE_H, &r, &mu);
            de__sky_transmittance_cpu(r, mu, 1.0, 1024, ref);
            const float *g = &t[(j * DE__SKY_TRANSMITTANCE_W + i) * 4u];
            for (u32 ch = 0; ch < 3u; ++ch) {
                const f64 d = fabs((f64)g[ch] - ref[ch]);
                const bool ok = ref[ch] >= 0.01 ? d <= 0.01 * ref[ch] : d <= 1e-4;
                if (ref[ch] >= 0.01 && d / ref[ch] > worst_rel)
                    worst_rel = d / ref[ch];
                if (!ok && bad++ < 5)
                    printf("  texel (%u, %u) ch %u: gpu %.6f cpu %.6f\n", i, j, ch, (f64)g[ch],
                           ref[ch]);
            }
        }
    printf("transmittance: worst relative error %.3g%% (%s)\n", worst_rel * 100.0,
           de_gpu_backend_name(b));
    EXPECT_EQ(bad, 0u);
    free(t);

    /* The multi-scattering LUT (the same submission): finite and non-negative, some energy. */
    float *ms = read_texture(f, de__sky_luts_multiscatter(f->luts), DE__SKY_MULTISCATTER_N,
                             DE__SKY_MULTISCATTER_N);
    ASSERT_TRUE(ms != NULL);
    u32 bad_ms = 0;
    f64 sum = 0.0;
    for (u32 k = 0; k < DE__SKY_MULTISCATTER_N * DE__SKY_MULTISCATTER_N; ++k)
        for (u32 ch = 0; ch < 3u; ++ch) {
            const float x = ms[k * 4u + ch];
            if (!isfinite(x) || x < 0.0f)
                ++bad_ms;
            else
                sum += (f64)x;
        }
    EXPECT_EQ(bad_ms, 0u);
    EXPECT_TRUE((sum) > (0.0));
    free(ms);
}

/* ---------- DENG-840: the aerial perspective ---------- */

TEST(atmosphere, test_atmosphere_aerial_slice_mapping) {
    const f64 range = DE__SKY_AERIAL_RANGE_KM;
    EXPECT_TRUE(de__sky_aerial_distance(0.0, range) == 0.0);
    EXPECT_TRUE(fabs(de__sky_aerial_distance(1.0, range) - range) < 1e-12);
    f64 prev = -1.0;
    for (u32 i = 0; i <= 64u; ++i) {
        const f64 w = (f64)i / 64.0, d = de__sky_aerial_distance(w, range);
        EXPECT_TRUE(d > prev); /* strictly increasing */
        prev = d;
        EXPECT_TRUE(fabs(de__sky_aerial_slice(d, range) - w) < 1e-12);
    }
    EXPECT_TRUE(de__sky_aerial_slice(-1.0, range) == 0.0);
    EXPECT_TRUE(de__sky_aerial_slice(10.0 * range, range) == 1.0);
    /* The first slice is thin (quadratic): under 1/N of the range. */
    EXPECT_TRUE(de__sky_aerial_distance(1.0 / DE__SKY_AERIAL_N, range) < range / DE__SKY_AERIAL_N);
    /* No distance: no extinction; more distance: less transmittance; blue is the most attenuated. */
    const f64 dir[3] = {1.0, 0.0, 0.0};
    f64 t0[3], t1[3], t2[3];
    de__sky_aerial_transmittance_cpu(DE__SKY_BOTTOM + 0.002, dir, 0.0, 1.0, 64, t0);
    de__sky_aerial_transmittance_cpu(DE__SKY_BOTTOM + 0.002, dir, 5.0, 1.0, 256, t1);
    de__sky_aerial_transmittance_cpu(DE__SKY_BOTTOM + 0.002, dir, 30.0, 1.0, 256, t2);
    for (u32 c = 0; c < 3u; ++c) {
        EXPECT_TRUE(t0[c] == 1.0);
        EXPECT_TRUE(t2[c] < t1[c] && t1[c] < 1.0);
    }
    EXPECT_TRUE(t2[2] < t2[1] && t2[1] < t2[0]);
}

static float half_to_float(u16 h) {
    const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 bits;
    if (e == 0u) {
        if (m == 0u) {
            bits = s;
        } else { /* subnormal */
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

static void normalize3(f64 v[3]) {
    const f64 l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
}

static void aerial_volume_gpu(atm_fx *f, de_gpu_backend b) {
    atm_open(f, b);
    if (!de__sky_luts_aerial_ready(f->luts))
        SKIP("no aerial-perspective shader code for this backend");
    const f32 mie_scale = 1.0f;
    ASSERT_EQ(de__sky_luts_update(f->luts, mie_scale, 0.3f), DE_OK);
    u64 v = 0;
    ASSERT_EQ(de_gpu_fence_wait(f->dev, de__sky_luts_fence(f->luts, &v), v, WAIT_NS), DE_OK);

    const u32 N = DE__SKY_AERIAL_N, W = N * N, pitch = W * 8u; /* RGBA16F */
    const de_gpu_texture tex = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = W,
                                       .height = N,
                                       .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED |
                                                DE_USAGE_COPY_SRC,
                                       .name = "sky aerial test"});
    ASSERT_TRUE(DE_HANDLE_VALID(tex));
    const de_gpu_buffer rb = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * N,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = "sky aerial readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(rb));

    /* A camera 2 m above the ground looking north, 90 x 60 degrees, the sun 20 degrees up in the
     * east: rays from 30 degrees down to 30 degrees up. */
    const f32 r_km = (f32)(DE__SKY_BOTTOM + 0.002), tx = 1.0f, ty = 0.57735f;
    de__sky_aerial_params p;
    memset(&p, 0, sizeof p);
    p.idx[0] = de_gpu_texture_bindless(f->dev, tex, 0, true);
    const f32 ray00[3] = {-tx, 1.0f, ty}, du[3] = {2.0f * tx, 0.0f, 0.0f},
              dv[3] = {0.0f, 0.0f, -2.0f * ty};
    for (u32 k = 0; k < 3u; ++k) {
        p.ray00[k] = ray00[k];
        p.ray_du[k] = du[k];
        p.ray_dv[k] = dv[k];
    }
    p.ray00[3] = r_km;
    p.ray_du[3] = DE__SKY_AERIAL_RANGE_KM;
    p.ray_dv[3] = mie_scale;
    const f32 se = 20.0f * 3.14159265f / 180.0f;
    p.sun[0] = cosf(se);
    p.sun[2] = sinf(se);
    p.sun[3] = 1.0f; /* unit illuminance: values stay well inside fp16 */
    p.moon[2] = -1.0f;

    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "sky aerial test");
    de_gpu_barrier bar;
    memset(&bar, 0, sizeof bar);
    bar.texture = tex;
    bar.before = DE_ACCESS_NONE;
    bar.after = DE_ACCESS_SHADER_WRITE;
    de_gpu_cmd_barriers(c, &bar, 1);
    de__sky_luts_aerial_record(f->luts, c, &p);
    bar.before = DE_ACCESS_SHADER_WRITE;
    bar.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(c, &bar, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, tex, 0, 0);
    const de_gpu_fence done = de_gpu_fence_create(f->dev, 0);
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = done, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, done, 1, WAIT_NS), DE_OK);
    const u16 *px = (const u16 *)de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(px != NULL);

    u32 bad_t = 0, bad_s = 0;
    f64 worst = 0.0, far_s = 0.0;
    for (u32 y = 0; y < N; y += 3u)
        for (u32 x = 0; x < N; x += 3u) {
            f64 dir[3];
            const f64 u = ((f64)x + 0.5) / N, vv = ((f64)y + 0.5) / N;
            for (u32 k = 0; k < 3u; ++k)
                dir[k] = (f64)ray00[k] + (f64)du[k] * u + (f64)dv[k] * vv;
            normalize3(dir);
            f64 prev[3] = {0.0, 0.0, 0.0};
            for (u32 sl = 0; sl < N; ++sl) {
                const u16 *t = &px[(size_t)y * (pitch / 2u) + (size_t)(sl * N + x) * 4u];
                const f64 d = de__sky_aerial_distance(((f64)sl + 1.0) / N, DE__SKY_AERIAL_RANGE_KM);
                f64 ref[3];
                de__sky_aerial_transmittance_cpu(r_km, dir, d, mie_scale, 2048, ref);
                const f64 ref_a = (ref[0] + ref[1] + ref[2]) / 3.0, a = half_to_float(t[3]);
                const f64 rel = fabs(a - ref_a) / ref_a;
                if (rel > worst)
                    worst = rel;
                if (rel > 0.01 && bad_t++ < 5)
                    printf("  froxel (%u, %u, %u): gpu T %.5f cpu %.5f\n", x, y, sl, a, ref_a);
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const f64 sc = half_to_float(t[ch]);
                    /* Growing with the distance (fp16 rounding allowed). */
                    if (!isfinite(sc) || sc < 0.0 || sc < prev[ch] - 1e-3 * prev[ch] - 1e-6) {
                        if (bad_s++ < 5)
                            printf("  froxel (%u, %u, %u) ch %u: S %.6g after %.6g\n", x, y, sl,
                                   ch, sc, prev[ch]);
                    }
                    prev[ch] = sc;
                }
            }
            far_s += prev[2];
        }
    printf("aerial perspective: worst transmittance error %.3g%% (%s)\n", worst * 100.0,
           de_gpu_backend_name(b));
    EXPECT_EQ(bad_t, 0u);
    EXPECT_EQ(bad_s, 0u);
    EXPECT_TRUE(far_s > 0.0); /* some blue in-scatter at the range */
    de_gpu_fence_destroy(f->dev, done);
    de_gpu_buffer_destroy(f->dev, rb);
    de_gpu_texture_destroy(f->dev, tex);
}

#define ATM_GPU_TESTS(sfx, backend)                                                                \
    TEST_F(atmosphere, test_atmosphere_transmittance_cpu_vs_gpu_##sfx, atm_fx) {                   \
        transmittance_cpu_vs_gpu(f, backend);                                                      \
    }                                                                                              \
    TEST_F(atmosphere, test_atmosphere_aerial_volume_##sfx, atm_fx) {                              \
        aerial_volume_gpu(f, backend);                                                             \
    }

ATM_GPU_TESTS(metal, DE_GPU_METAL)
ATM_GPU_TESTS(vulkan, DE_GPU_VULKAN)
ATM_GPU_TESTS(d3d12, DE_GPU_D3D12)
