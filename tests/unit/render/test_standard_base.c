/*
 * test_standard_base.c: the Standard material model's base (DENG-299): the energy LUT, energy
 * compensation, the KHR F0 rules, sampling, emission. Executable test_render_standard; CTest
 * names render_standard.standard.<test>.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device or the shader
 * importer). They cook tests/unit/render/data/standard_kernels.slang at test time with the
 * engine's shaders/ on the same root, and make the energy LUT with the renderer's code
 * (de__brdf_luts_create: the engine shader brdf_lut_gen).
 *
 *   lut_reference_sane          the CPU reference LUT: E <= 1, E -> 1 for smooth surfaces, the
 *                               averages match the rows
 *   luts_match_cpu_*            the GPU LUT against the CPU reference within 1e-3, and two GPU
 *                               generations byte-identical
 *   white_furnace_metal_*       base 1, metallic 1, under a uniform unit environment: the albedo
 *                               (an importance-sampled estimate) is 1 +- 2 % for every roughness
 *                               and view angle with compensation, and below 0.9 for rough
 *                               surfaces without it
 *   furnace_dielectric_no_gain_* base 1, metallic 0: at most 1 + 1 %
 *   sample_pdf_consistency_*    the lobe mixture: the pdf integrates to 1 (+- 1 %), sample's weight
 *                               is evaluate / pdf, and the estimator equals a quadrature (+- 1 %)
 *   ior_specular_f0_*           F0 and F90 against hand-computed KHR values
 *   emissive_strength_*         resolve_gbuffer's emission is factor x strength (and the KHR
 *                               inputs reach SurfaceData)
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "render/materials/brdf_luts.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#if defined(DE_STD_TEST_COOK) && DE_STD_TEST_COOK
#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)120 * 1000000000u)
#define LUT_N   DE__BRDF_LUT_SIZE

/* ---------- lut_reference_sane (CPU) ---------- */

TEST(standard, lut_reference_sane) {
    float *t = (float *)malloc(sizeof(float) * 4u * LUT_N * LUT_N);
    ASSERT_TRUE(t != NULL);
    de__brdf_lut_reference(4096u, t);
    for (u32 j = 0; j < LUT_N; ++j) {
        double avg_a = 0.0, avg_b = 0.0;
        for (u32 i = 0; i < LUT_N; ++i) {
            const float *v = t + ((size_t)j * LUT_N + i) * 4u;
            const double mu = ((double)i + 0.5) / LUT_N;
            EXPECT_TRUE(v[0] >= 0.0f && v[1] >= 0.0f && v[0] + v[1] <= 1.0f + 1e-4f);
            avg_a += 2.0 * v[0] * mu / LUT_N;
            avg_b += 2.0 * v[1] * mu / LUT_N;
            EXPECT_FLOAT_EQ(v[2], t[(size_t)j * LUT_N * 4u + 2u], 0.0f); /* one per row */
        }
        const float *v0 = t + (size_t)j * LUT_N * 4u;
        EXPECT_FLOAT_EQ(v0[2], (float)avg_a, 1e-6f);
        EXPECT_FLOAT_EQ(v0[3], (float)avg_b, 1e-6f);
    }
    /* Smooth: E = A + B close to 1 except at grazing angles; rough: well below 1. */
    const float *smooth = t + ((size_t)0 * LUT_N + LUT_N / 2u) * 4u;
    const float *rough = t + ((size_t)(LUT_N - 1u) * LUT_N + LUT_N / 2u) * 4u;
    EXPECT_TRUE(smooth[0] + smooth[1] > 0.99f);
    EXPECT_TRUE(rough[0] + rough[1] < 0.8f);
    free(t);
}

/* ---------- GPU fixture ---------- */

enum { K_SAMPLE, K_QUAD, K_F0, K_RESOLVE, K_COUNT };
static const char *const k_entries[K_COUNT] = {"cs_sample", "cs_quad", "cs_f0", "cs_resolve"};

/** One case of standard_kernels.slang (three float4: see case_surface there). */
typedef struct sk_case {
    float p0[4]; /* roughness, metallic, base, specular */
    float p1[4]; /* specular colour, ior */
    float p2[4]; /* mu_o, multiscatter (cs_sample, cs_quad); base colour / emissive otherwise */
} sk_case;

#define SK_OUT 4u

typedef struct sb_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_gpu_backend backend;
    de_gpu_pipeline pipe[K_COUNT];
    de__brdf_luts *luts;
    u32 lut;
    char why[700];
} sb_fx;

static void sb_fx_setup(sb_fx *f) {
    memset(f, 0, sizeof *f);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    de_error_clear();
}

static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void sb_fx_teardown(sb_fx *f) {
    if (f->dev != NULL) {
        de_gpu_wait_idle(f->dev);
        for (u32 k = 0; k < K_COUNT; ++k)
            if (DE_HANDLE_VALID(f->pipe[k]))
                de_gpu_pipeline_destroy(f->dev, f->pipe[k]);
        de__brdf_luts_destroy(f->luts);
        de_gpu_wait_idle(f->dev);
        EXPECT_EQ(validation_errors(f->dev), 0u);
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

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

/* Records `record` into one list on the compute queue, submits and waits. */
typedef void (*sb_record_fn)(de_gpu_cmd *c, void *user);
static void sb_submit(sb_fx *f, sb_record_fn record, void *user) {
    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "standard test");
    ASSERT_TRUE(c != NULL);
    record(c, user);
    if (de_error_last() != NULL)
        printf("recording: %s\n", de_error_last()->message);
    EXPECT_TRUE(de_error_last() == NULL);
    const de_gpu_fence fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s), DE_OK);
    /* The first dispatch includes the driver's compile of the kernel (DENG-428): a long bound. */
    ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(f->dev, fence);
}

#if defined(DE_STD_TEST_COOK) && DE_STD_TEST_COOK

/* "stdsrc:/": data/ over the engine's shaders/. */
static bool mount_sources(void) {
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("stdsrc", DE_STD_TEST_SHADER_DIR, 0, 0) == DE_OK &&
                  de_fs_mount("stdsrc", DE_STD_TEST_DATA_DIR, 0, 1) == DE_OK;
    return mounted == 1;
}

static const char *target_name(de__shader_target t) {
    switch (t) {
    case DE__SHADER_TARGET_SPIRV:
        return "spirv";
    case DE__SHADER_TARGET_DXIL:
        return "dxil";
    case DE__SHADER_TARGET_METALLIB:
        return "metallib";
    default:
        return "none";
    }
}

static bool find_entry(const de__shader_view *v, u32 target, const char *name,
                       de_gpu_shader_blob *out, de__shader_record *rec_out) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *n = NULL;
        (void)de__shader_view_entry(v, e, &n);
        if (strcmp(n, name) != 0)
            continue;
        const i32 ri = de__shader_view_find(v, target, 0, e);
        if (ri < 0)
            return false;
        const de__shader_record rec = de__shader_view_record(v, (u32)ri);
        out->code = v->base + rec.code_offset;
        out->size = rec.code_size;
        out->entry = n;
        *rec_out = rec;
        return true;
    }
    return false;
}

/* standard_kernels.slang's entry points for the fixture's backend; false with f->why. */
static bool sb_pipelines(sb_fx *f) {
    const de__shader_target target = de__shader_target_for_backend(f->backend);
    char msg[200];
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(f->why, sizeof f->why, "no Slang compiler: %.150s", msg);
        return false;
    }
    if (!mount_sources()) {
        snprintf(f->why, sizeof f->why, "cannot mount the shader sources");
        return false;
    }
    de_cooker *c = NULL;
    de_cooker_desc cd = {.verify_determinism = DE_COOK_VERIFY_OFF};
    static const char *const exts[] = {".slang"};
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "shader";
    t.extensions = exts;
    t.extension_count = 1;
    t.importer_version = de_shader_importer_version();
    t.import = de_shader_import;
    const char *targets[1] = {target_name(target)};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "standard_kernels";
    a.type = "shader";
    a.source = "standard_kernels.slang";
    a.file = "stdsrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = DE_OK;
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK)
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "stdsrc:/", .source = NULL};
        r = de_cooker_cook(c, &in, &res);
    }
    de__shader_view view;
    bool ok = r == DE_OK && de__shader_view_init(res.data, res.size, &view) == DE_OK;
    for (u32 k = 0; ok && k < K_COUNT; ++k) {
        de_gpu_shader_blob cs = {0};
        de__shader_record rec;
        ok = find_entry(&view, (u32)target, k_entries[k], &cs, &rec);
        if (ok)
            f->pipe[k] = de_gpu_pipeline_create(
                f->dev,
                &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                        .cs = cs,
                                        .push_constant_bytes = 32,
                                        .threadgroup_size = {rec.threadgroup[0], rec.threadgroup[1],
                                                             rec.threadgroup[2]},
                                        .name = k_entries[k]});
        ok = ok && DE_HANDLE_VALID(f->pipe[k]);
    }
    if (!ok)
        snprintf(f->why, sizeof f->why, "standard_kernels.slang did not cook or link: %.600s",
                 de_error_last() ? de_error_last()->message : "?");
    de_cook_result_free(&res);
    de_cooker_destroy(c);
    return ok;
}

#endif

/* Opens backend `b`: the device, the kernels and the energy LUT (generated and waited for). */
static void sb_open(sb_fx *f, de_gpu_backend b, bool kernels) {
    if (!backend_built(b)) {
        snprintf(f->why, sizeof f->why, "the %s backend is not in this build",
                 de_gpu_backend_name(b));
        SKIP(f->why);
    }
    f->backend = b;
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
    printf("device: \"%s\" (%s)\n", caps->device_name, de_gpu_backend_name(b));
    render_d3d12_report(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(f->why, sizeof f->why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(f->why);
    }
#if defined(DE_STD_TEST_COOK) && DE_STD_TEST_COOK
    if (kernels && !sb_pipelines(f))
        SKIP(f->why);
#else
    if (kernels)
        SKIP("this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
#endif
    ASSERT_EQ(de__brdf_luts_create(f->dev, &f->alloc, &f->luts), DE_OK);
    f->lut = de__brdf_luts_bindless(f->luts);
    if (f->lut == DE_GPU_BINDLESS_INVALID)
        SKIP("no brdf_lut_gen code for this backend in this build");
    u64 v = 0;
    const de_gpu_fence fence = de__brdf_luts_fence(f->luts, &v);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, v, WAIT_NS), DE_OK);
}

/* ---------- Running a kernel ---------- */

typedef struct run_ctx {
    sb_fx *f;
    de_gpu_pipeline pipe;
    de_gpu_buffer in, out, rb;
    u64 out_bytes;
    u32 params[8];
    u32 threads;
} run_ctx;

static void run_record(de_gpu_cmd *c, void *user) {
    run_ctx *x = (run_ctx *)user;
    de_gpu_barrier bars[2] = {buf_barrier(x->in, DE_ACCESS_NONE, DE_ACCESS_SHADER_READ),
                              buf_barrier(x->out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
    de_gpu_cmd_barriers(c, bars, 2);
    de_gpu_cmd_set_pipeline(c, x->pipe);
    /* At most 65535 groups per dispatch (Vulkan's guaranteed maxComputeWorkGroupCount). */
    const u32 chunk = 65535u * 64u;
    for (u32 first = 0; first < x->threads; first += chunk) {
        x->params[6] = first;
        const u32 n = x->threads - first < chunk ? x->threads - first : chunk;
        de_gpu_cmd_push_constants(c, x->params, sizeof x->params);
        de_gpu_cmd_dispatch(c, (n + 63u) / 64u, 1, 1);
    }
    bars[0] = buf_barrier(x->out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, bars, 1);
    de_gpu_cmd_copy_buffer(c, x->rb, 0, x->out, 0, x->out_bytes);
}

/* Runs kernel `k` over `count` cases with `per_case` threads each (and `grid_x` for cs_quad);
 * returns `out_floats` floats (malloc; the caller frees). */
static float *sb_run(sb_fx *f, u32 k, const sk_case *cases, u32 count, u32 per_case, u32 grid_x,
                     u32 out_floats) {
    run_ctx x;
    memset(&x, 0, sizeof x);
    x.f = f;
    x.pipe = f->pipe[k];
    x.threads = count * per_case;
    x.out_bytes = (u64)out_floats * sizeof(float);
    const u64 in_bytes = (u64)count * sizeof(sk_case);
    x.in = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = in_bytes,
                                                              .usage = DE_USAGE_STORAGE,
                                                              .memory = DE_MEM_UPLOAD,
                                                              .stride = 16,
                                                              .name = "standard cases"});
    x.out = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .stride = 4,
                                      .name = "standard out"});
    x.rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = "standard readback"});
    float *res = (float *)calloc(out_floats, sizeof(float));
    if (!DE_HANDLE_VALID(x.in) || !DE_HANDLE_VALID(x.out) || !DE_HANDLE_VALID(x.rb) ||
        res == NULL) {
        free(res);
        return NULL;
    }
    memcpy(de_gpu_buffer_map(f->dev, x.in), cases, (size_t)in_bytes);
    x.params[0] = de_gpu_buffer_bindless(f->dev, x.in);
    x.params[1] = de_gpu_buffer_bindless(f->dev, x.out);
    x.params[2] = count;
    x.params[3] = f->lut;
    x.params[4] = per_case;
    x.params[5] = grid_x;
    sb_submit(f, run_record, &x);
    memcpy(res, de_gpu_buffer_map(f->dev, x.rb), (size_t)x.out_bytes);
    de_gpu_buffer_destroy(f->dev, x.in);
    de_gpu_buffer_destroy(f->dev, x.out);
    de_gpu_buffer_destroy(f->dev, x.rb);
    return res;
}

static sk_case surface_case(float roughness, float metallic, float base, float mu_o, bool ms) {
    sk_case c;
    memset(&c, 0, sizeof c);
    c.p0[0] = roughness;
    c.p0[1] = metallic;
    c.p0[2] = base;
    c.p0[3] = 1.0f; /* specular */
    c.p1[0] = c.p1[1] = c.p1[2] = 1.0f;
    c.p1[3] = 1.5f; /* ior */
    c.p2[0] = mu_o;
    c.p2[1] = ms ? 1.0f : 0.0f;
    return c;
}

/* The mean of output `col` over each case's `per_case` threads. */
static void case_means(const float *res, u32 count, u32 per_case, u32 col, double *out) {
    for (u32 c = 0; c < count; ++c) {
        double s = 0.0;
        for (u32 k = 0; k < per_case; ++k)
            s += res[((size_t)c * per_case + k) * SK_OUT + col];
        out[c] = s / per_case;
    }
}

/* ---------- The tests ---------- */

static void luts_match_cpu(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, false);
    const u32 floats = 4u * LUT_N * LUT_N;
    const u32 pitch = LUT_N * 16u; /* 512 bytes: a multiple of 256 */
    float *ref = (float *)malloc(sizeof(float) * floats);
    ASSERT_TRUE(ref != NULL);
    de__brdf_lut_reference(16384u, ref);
    /* Two generations: this fixture's, and a second one. */
    de__brdf_luts *second = NULL;
    ASSERT_EQ(de__brdf_luts_create(f->dev, &f->alloc, &second), DE_OK);
    float *got[2] = {NULL, NULL};
    for (u32 g = 0; g < 2u; ++g) {
        de__brdf_luts *l = g == 0 ? f->luts : second;
        u64 v = 0;
        const de_gpu_fence fence = de__brdf_luts_fence(l, &v);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, v, WAIT_NS), DE_OK);
        const de_gpu_buffer rb =
            de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * LUT_N,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "lut readback"});
        ASSERT_TRUE(DE_HANDLE_VALID(rb));
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "lut readback");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier bar;
        memset(&bar, 0, sizeof bar);
        bar.texture = de__brdf_luts_texture(l);
        bar.before = DE_ACCESS_SHADER_READ;
        bar.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(c, &bar, 1);
        de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, de__brdf_luts_texture(l), 0, 0);
        bar.before = DE_ACCESS_COPY_SRC;
        bar.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(c, &bar, 1);
        EXPECT_TRUE(de_error_last() == NULL);
        const de_gpu_fence done = de_gpu_fence_create(f->dev, 0);
        const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = done, .signal_value = 1};
        ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s), DE_OK);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, done, 1, WAIT_NS), DE_OK);
        de_gpu_fence_destroy(f->dev, done);
        got[g] = (float *)malloc(sizeof(float) * floats);
        ASSERT_TRUE(got[g] != NULL);
        memcpy(got[g], de_gpu_buffer_map(f->dev, rb), sizeof(float) * floats);
        de_gpu_buffer_destroy(f->dev, rb);
    }
    double worst[4] = {0, 0, 0, 0};
    u32 bad = 0;
    for (u32 t = 0; t < LUT_N * LUT_N; ++t)
        for (u32 ch = 0; ch < 4u; ++ch) {
            const double d = fabs((double)got[0][t * 4u + ch] - (double)ref[t * 4u + ch]);
            if (d > worst[ch])
                worst[ch] = d;
            if (!(d <= 1e-3) && bad++ < 5)
                printf("  texel (%u, %u) channel %u: gpu %.6f, cpu %.6f\n", t % LUT_N, t / LUT_N,
                       ch, (double)got[0][t * 4u + ch], (double)ref[t * 4u + ch]);
        }
    printf("worst |gpu - cpu|: A %.2e, B %.2e, A_avg %.2e, B_avg %.2e\n", worst[0], worst[1],
           worst[2], worst[3]);
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(memcmp(got[0], got[1], sizeof(float) * floats), 0); /* deterministic */
    free(got[0]);
    free(got[1]);
    free(ref);
    de_gpu_wait_idle(f->dev);
    de__brdf_luts_destroy(second);
}

static const float k_rough[6] = {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f};
static const float k_mu[4] = {0.1f, 0.5f, 0.9f, 1.0f};
#define FURNACE_N 8192u

/* The furnace albedo of base `base`, metallic `metal` (multi-scatter on / off) for every
 * roughness x view angle: E[weight] of sample(). `out` has 24 entries. */
static void furnace(sb_fx *f, float base, float metal, bool ms, double *out) {
    sk_case cases[24];
    for (u32 r = 0; r < 6u; ++r)
        for (u32 m = 0; m < 4u; ++m)
            cases[r * 4u + m] = surface_case(k_rough[r], metal, base, k_mu[m], ms);
    float *res = sb_run(f, K_SAMPLE, cases, 24, FURNACE_N, 0, 24u * FURNACE_N * SK_OUT);
    ASSERT_TRUE(res != NULL);
    case_means(res, 24, FURNACE_N, 0, out);
    free(res);
}

static void white_furnace_metal(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    double on[24], off[24];
    furnace(f, 1.0f, 1.0f, true, on);
    furnace(f, 1.0f, 1.0f, false, off);
    for (u32 r = 0; r < 6u; ++r)
        for (u32 m = 0; m < 4u; ++m) {
            const u32 i = r * 4u + m;
            printf("  roughness %.2f mu_o %.1f: albedo %.4f with compensation, %.4f without\n",
                   (double)k_rough[r], (double)k_mu[m], on[i], off[i]);
            EXPECT_TRUE(fabs(on[i] - 1.0) <= 0.02);
            EXPECT_TRUE(off[i] <= on[i] + 1e-3);
        }
    /* Rough metals lose a visible share without the multi-scatter lobe. */
    EXPECT_TRUE(off[5 * 4u + 1u] < 0.9);
    EXPECT_TRUE(off[5 * 4u + 2u] < 0.9);
}

static void furnace_dielectric(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    double on[24];
    furnace(f, 1.0f, 0.0f, true, on);
    for (u32 i = 0; i < 24u; ++i) {
        printf("  roughness %.2f mu_o %.1f: albedo %.4f\n", (double)k_rough[i / 4u],
               (double)k_mu[i % 4u], on[i]);
        EXPECT_TRUE(on[i] <= 1.01);
        EXPECT_TRUE(on[i] >= 0.95); /* a white dielectric loses almost nothing either */
    }
}

/* The mixture of lobes: pdf integrates to 1 over the sphere, sample's weight is
 * evaluate / pdf, the estimator equals the quadrature of evaluate, and StandardModel's entry
 * points agree with the base. */
static void sample_pdf_consistency(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float rough[3] = {0.3f, 0.6f, 1.0f};
    static const float metal[3] = {0.0f, 0.5f, 1.0f};
    static const float mus[2] = {0.3f, 0.8f};
    sk_case cases[18];
    for (u32 i = 0; i < 18u; ++i)
        cases[i] = surface_case(rough[i / 6u], metal[(i / 2u) % 3u], 0.8f, mus[i % 2u], true);
    const u32 gx = 512u, gy = 512u, n = 8192u;
    float *quad = sb_run(f, K_QUAD, cases, 18, gx * gy, gx, 18u * gx * gy * SK_OUT);
    float *smp = sb_run(f, K_SAMPLE, cases, 18, n, 0, 18u * n * SK_OUT);
    ASSERT_TRUE(quad != NULL && smp != NULL);
    double eval_q[18], pdf_q[18], model_eval_q[18], model_pdf_q[18], est[18];
    case_means(quad, 18, gx * gy, 0, eval_q);
    case_means(quad, 18, gx * gy, 1, pdf_q);
    case_means(quad, 18, gx * gy, 2, model_eval_q);
    case_means(quad, 18, gx * gy, 3, model_pdf_q);
    case_means(smp, 18, n, 0, est);
    const double dw = 4.0 * 3.14159265358979323846; /* mean x the sphere's measure */
    u32 weight_bad = 0;
    for (u32 i = 0; i < 18u; ++i) {
        const double albedo = eval_q[i] * dw, ipdf = pdf_q[i] * dw;
        printf("  roughness %.1f metallic %.1f mu_o %.1f: integral of pdf %.4f, albedo %.4f, "
               "estimator %.4f\n",
               (double)cases[i].p0[0], (double)cases[i].p0[1], (double)cases[i].p2[0], ipdf, albedo,
               est[i]);
        EXPECT_TRUE(fabs(ipdf - 1.0) <= 0.01);
        EXPECT_TRUE(fabs(est[i] - albedo) <= 0.01 * albedo);
        EXPECT_TRUE(fabs(model_eval_q[i] - eval_q[i]) <= 1e-6 * fabs(eval_q[i]) + 1e-9);
        EXPECT_TRUE(fabs(model_pdf_q[i] - pdf_q[i]) <= 1e-6 * fabs(pdf_q[i]) + 1e-9);
        for (u32 k = 0; k < n; ++k) {
            const float *v = smp + ((size_t)i * n + k) * SK_OUT;
            const bool ok = fabsf(v[0] - v[2]) <= 1e-4f * fabsf(v[2]) + 1e-6f &&
                            fabsf(v[3] - v[0]) <= 1e-5f * fabsf(v[0]) + 1e-6f;
            if (!ok && weight_bad++ < 5)
                printf("  case %u sample %u: weight %g, evaluate / pdf %g, model %g\n", i, k,
                       (double)v[0], (double)v[2], (double)v[3]);
        }
    }
    EXPECT_EQ(weight_bad, 0u);
    free(quad);
    free(smp);
}

/* KHR_materials_ior and KHR_materials_specular: dielectric F0 = min(F0(ior) x specular_color,
 * 1) x specular, F90 = specular; metal F0 = base colour, F90 = 1; blended by metallic. */
static void ior_specular_f0(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    typedef struct f0_case {
        float ior, specular, color[3], metallic, base[3];
        float f0[3], f90;
    } f0_case;
    const float f0_133 = (0.33f / 2.33f) * (0.33f / 2.33f);
    const f0_case k[] = {
        {1.5f, 1.0f, {1, 1, 1}, 0.0f, {0, 0, 0}, {0.04f, 0.04f, 0.04f}, 1.0f},
        {1.33f, 1.0f, {1, 1, 1}, 0.0f, {0, 0, 0}, {f0_133, f0_133, f0_133}, 1.0f},
        {1.5f, 0.5f, {1, 1, 1}, 0.0f, {0, 0, 0}, {0.02f, 0.02f, 0.02f}, 0.5f},
        {1.5f, 1.0f, {1.0f, 0.5f, 2.0f}, 0.0f, {0, 0, 0}, {0.04f, 0.02f, 0.08f}, 1.0f},
        {1.5f, 0.7f, {50.0f, 1.0f, 1.0f}, 0.0f, {0, 0, 0}, {0.7f, 0.028f, 0.028f}, 0.7f},
        {2.0f, 1.0f, {1, 1, 1}, 0.0f, {0, 0, 0}, {1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f}, 1.0f},
        {1.0f, 1.0f, {1, 1, 1}, 0.0f, {0, 0, 0}, {0.0f, 0.0f, 0.0f}, 1.0f},
        {0.5f, 1.0f, {1, 1, 1}, 0.0f, {0, 0, 0}, {0.0f, 0.0f, 0.0f}, 1.0f}, /* clamped to 1 */
        {1.5f, 1.0f, {1, 1, 1}, 1.0f, {0.9f, 0.6f, 0.3f}, {0.9f, 0.6f, 0.3f}, 1.0f},
        {1.5f, 0.5f, {1, 1, 1}, 0.5f, {0.9f, 0.6f, 0.3f}, {0.46f, 0.31f, 0.16f}, 0.75f},
    };
    const u32 n = (u32)(sizeof k / sizeof k[0]);
    sk_case cases[16];
    memset(cases, 0, sizeof cases);
    for (u32 i = 0; i < n; ++i) {
        cases[i].p0[1] = k[i].metallic;
        cases[i].p0[3] = k[i].specular;
        memcpy(cases[i].p1, k[i].color, sizeof k[i].color);
        cases[i].p1[3] = k[i].ior;
        memcpy(cases[i].p2, k[i].base, sizeof k[i].base);
    }
    float *res = sb_run(f, K_F0, cases, n, 1, 0, n * 2u * SK_OUT);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < n; ++i) {
        const float *blend = res + (i * 2u + 1u) * SK_OUT;
        for (u32 c = 0; c < 3u; ++c)
            EXPECT_FLOAT_EQ(blend[c], k[i].f0[c], 1e-6f + 1e-5f * k[i].f0[c]);
        EXPECT_FLOAT_EQ(blend[3], k[i].f90, 1e-6f);
        if (k[i].metallic == 0.0f) { /* the dielectric terms are the blend */
            const float *d = res + (i * 2u) * SK_OUT;
            for (u32 c = 0; c < 3u; ++c)
                EXPECT_FLOAT_EQ(d[c], blend[c], 1e-7f);
        }
    }
    free(res);
}

/* resolve_gbuffer: emissive = factor x strength (x texture, white without one), in cd/m^2 before
 * exposure; the KHR inputs reach SurfaceData; no LUT is set (the shading pass sets it). */
static void emissive_strength(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float em[3][4] = {
        {0.5f, 0.25f, 1.0f, 4.0f}, {1.0f, 1.0f, 1.0f, 1000.0f}, {0.2f, 0.4f, 0.6f, 0.0f}};
    sk_case cases[3];
    memset(cases, 0, sizeof cases);
    for (u32 i = 0; i < 3u; ++i) {
        cases[i].p0[0] = 0.4f;  /* roughness */
        cases[i].p0[1] = 0.25f; /* metallic */
        cases[i].p0[2] = 0.7f;  /* base */
        cases[i].p0[3] = 0.6f;  /* specular */
        cases[i].p1[0] = 0.9f;  /* specular colour */
        cases[i].p1[1] = 0.8f;
        cases[i].p1[2] = 0.7f;
        cases[i].p1[3] = 1.45f; /* ior */
        memcpy(cases[i].p2, em[i], sizeof em[i]);
    }
    float *res = sb_run(f, K_RESOLVE, cases, 3, 1, 0, 3u * 3u * SK_OUT);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < 3u; ++i) {
        const float *v = res + i * 3u * SK_OUT;
        for (u32 c = 0; c < 3u; ++c)
            EXPECT_FLOAT_EQ(v[c], em[i][c] * em[i][3], 1e-6f * em[i][3] + 1e-7f);
        EXPECT_FLOAT_EQ(v[3], 1.45f, 1e-7f); /* ior */
        EXPECT_FLOAT_EQ(v[4], 0.9f, 1e-7f);  /* specular colour */
        EXPECT_FLOAT_EQ(v[6], 0.7f, 1e-7f);
        EXPECT_FLOAT_EQ(v[7], 0.6f, 1e-7f);   /* specular */
        EXPECT_FLOAT_EQ(v[8], 0.7f, 1e-7f);   /* base colour */
        EXPECT_FLOAT_EQ(v[9], 0.4f, 1e-7f);   /* roughness */
        EXPECT_FLOAT_EQ(v[10], 0.25f, 1e-7f); /* metallic */
        EXPECT_FLOAT_EQ(v[11], 1.0f, 0.0f);   /* brdf_lut: DE_BINDLESS_INVALID */
    }
    free(res);
}

#define STD_GPU_TESTS(sfx, backend)                                                                \
    TEST_F(standard, luts_match_cpu_##sfx, sb_fx) {                                                \
        luts_match_cpu(f, backend);                                                                \
    }                                                                                              \
    TEST_F(standard, white_furnace_metal_##sfx, sb_fx) {                                           \
        white_furnace_metal(f, backend);                                                           \
    }                                                                                              \
    TEST_F(standard, furnace_dielectric_no_gain_##sfx, sb_fx) {                                    \
        furnace_dielectric(f, backend);                                                            \
    }                                                                                              \
    TEST_F(standard, sample_pdf_consistency_##sfx, sb_fx) {                                        \
        sample_pdf_consistency(f, backend);                                                        \
    }                                                                                              \
    TEST_F(standard, ior_specular_f0_##sfx, sb_fx) {                                               \
        ior_specular_f0(f, backend);                                                               \
    }                                                                                              \
    TEST_F(standard, emissive_strength_##sfx, sb_fx) {                                             \
        emissive_strength(f, backend);                                                             \
    }

STD_GPU_TESTS(metal, DE_GPU_METAL)
STD_GPU_TESTS(vulkan, DE_GPU_VULKAN)
STD_GPU_TESTS(d3d12, DE_GPU_D3D12)
