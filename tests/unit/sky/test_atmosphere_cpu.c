/*
 * test_atmosphere_cpu.c: the GPU transmittance LUT against the double-precision CPU reference
 * (DENG-206 AC: within 1 %), and the multi-scattering LUT's sanity, on each GPU backend built.
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

#define ATM_GPU_TESTS(sfx, backend)                                                                \
    TEST_F(atmosphere, test_atmosphere_transmittance_cpu_vs_gpu_##sfx, atm_fx) {                   \
        transmittance_cpu_vs_gpu(f, backend);                                                      \
    }

ATM_GPU_TESTS(metal, DE_GPU_METAL)
ATM_GPU_TESTS(vulkan, DE_GPU_VULKAN)
ATM_GPU_TESTS(d3d12, DE_GPU_D3D12)
