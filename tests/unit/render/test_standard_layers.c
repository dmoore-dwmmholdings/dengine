/*
 * test_standard_layers.c: the Standard model's layers (DENG-300): clearcoat, sheen, anisotropy and
 * iridescence (KHR_materials_*). Executable test_render_standard_layers; CTest names
 * render_standard_layers.layers.<test>.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device or the shader
 * importer). They cook tests/unit/render/data/standard_layers_kernels.slang at test time with the
 * engine's shaders/ on the same root, and make the energy and sheen LUT with the renderer's code.
 *
 *   clearcoat_furnace_*          clearcoat 1 over a white metal: albedo <= 1 + 1 %
 *   sheen_furnace_*              sheen 1 over a white dielectric: albedo <= 1 + 1 %
 *   sheen_lut_matches_cpu_*      the sheen rows of the LUT against the CPU reference, 1e-3
 *   anisotropy_zero_identity_*   strength 0 is the isotropic base (< 1e-6 over a direction grid)
 *   anisotropy_rotation_*        rotating the direction by pi is the identity
 *   iridescence_zero_identity_*  factor 0 or thickness 0 is the base (bitwise)
 *   iridescence_reference_hue_*  a film's colour at normal incidence against a spectral reference
 *                                (Airy reflectance x CIE functions), 0.01 per channel
 *   sample_pdf_consistency_*     each layer alone and all together: integral of pdf 1 (+- 1 %),
 *                                weight = evaluate / pdf, estimator = quadrature (+- 1 %)
 *   flags_compile_out            SPIR-V of the model with no layer compiled in ~ the base alone
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

#if defined(DE_LAY_TEST_COOK) && DE_LAY_TEST_COOK
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

/* ---------- GPU fixture ---------- */

enum { K_SAMPLE, K_QUAD, K_FILM, K_COUNT };
static const char *const k_entries[K_COUNT] = {"cs_sample", "cs_quad", "cs_film"};

/** One case of standard_layers_kernels.slang (four float4: see case_surface there). */
typedef struct sk_case {
    float p0[4]; /* roughness, metallic, base, mu_o */
    float p1[4]; /* clearcoat, clearcoat roughness, sheen roughness, anisotropy */
    float p2[4]; /* sheen colour, - */
    float p3[4]; /* iridescence, iridescence ior, thickness (nm), features */
    float p4[4]; /* anisotropy direction (x, y), -, - */
} sk_case;

#define SK_OUT  4u
#define WAIT_NS ((u64)120 * 1000000000u)

typedef struct sb_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_gpu_backend backend;
    de_gpu_pipeline pipe[K_COUNT];
    de__brdf_luts *luts;
    u32 lut;
    char why[700];
    /* Buffers of sb_run, destroyed at teardown: a buffer freed between runs can come back at the
     * same memory, and Vulkan's synchronization validation then sees two unordered copies into
     * it (the fence waits order them, but not in its model). */
    de_gpu_buffer keep[32];
    u32 keep_count;
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
        for (u32 k = 0; k < f->keep_count; ++k)
            de_gpu_buffer_destroy(f->dev, f->keep[k]);
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
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "layers test");
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

#if defined(DE_LAY_TEST_COOK) && DE_LAY_TEST_COOK

/* "laysrc:/": data/ over the engine's shaders/. */
static bool mount_sources(void) {
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("laysrc", DE_LAY_TEST_SHADER_DIR, 0, 0) == DE_OK &&
                  de_fs_mount("laysrc", DE_LAY_TEST_DATA_DIR, 0, 1) == DE_OK;
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
    a.id = "standard_layers_kernels";
    a.type = "shader";
    a.source = "standard_layers_kernels.slang";
    a.file = "laysrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = DE_OK;
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK)
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "laysrc:/", .source = NULL};
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
        snprintf(f->why, sizeof f->why,
                 "standard_layers_kernels.slang did not cook or link: %.600s",
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
#if defined(DE_LAY_TEST_COOK) && DE_LAY_TEST_COOK
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
                                                              .name = "layers cases"});
    x.out = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .stride = 4,
                                      .name = "layers out"});
    x.rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = "layers readback"});
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
    if (f->keep_count + 3u <= sizeof f->keep / sizeof f->keep[0]) {
        f->keep[f->keep_count++] = x.in;
        f->keep[f->keep_count++] = x.out;
        f->keep[f->keep_count++] = x.rb;
    } else {
        de_gpu_buffer_destroy(f->dev, x.in);
        de_gpu_buffer_destroy(f->dev, x.out);
        de_gpu_buffer_destroy(f->dev, x.rb);
    }
    return res;
}

/* ---------- Cases ---------- */

/* Feature bits (material_models.h DE__MATERIAL_FEATURE_*). */
#define F_CLEARCOAT   0x1u
#define F_SHEEN       0x2u
#define F_ANISOTROPY  0x4u
#define F_IRIDESCENCE 0x8u

static sk_case base_case(float roughness, float metallic, float base, float mu_o) {
    sk_case c;
    memset(&c, 0, sizeof c);
    c.p0[0] = roughness;
    c.p0[1] = metallic;
    c.p0[2] = base;
    c.p0[3] = mu_o;
    c.p3[1] = 1.3f; /* iridescence ior */
    return c;
}

static void with_coat(sk_case *c, float factor, float roughness) {
    c->p1[0] = factor;
    c->p1[1] = roughness;
    c->p3[3] = (float)((u32)c->p3[3] | F_CLEARCOAT);
}
static void with_sheen(sk_case *c, float color, float roughness) {
    c->p2[0] = c->p2[1] = c->p2[2] = color;
    c->p1[2] = roughness;
    c->p3[3] = (float)((u32)c->p3[3] | F_SHEEN);
}
static void with_anisotropy(sk_case *c, float strength, float angle) {
    c->p1[3] = strength;
    c->p4[0] = cosf(angle);
    c->p4[1] = sinf(angle);
    c->p3[3] = (float)((u32)c->p3[3] | F_ANISOTROPY);
}
static void with_iridescence(sk_case *c, float factor, float ior, float thickness) {
    c->p3[0] = factor;
    c->p3[1] = ior;
    c->p3[2] = thickness;
    c->p3[3] = (float)((u32)c->p3[3] | F_IRIDESCENCE);
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

#define FURNACE_N 4096u

/* The albedo of every case under a uniform unit environment: E[weight] of sample(). */
static void furnace(sb_fx *f, const sk_case *cases, u32 n, double *out) {
    float *res = sb_run(f, K_SAMPLE, cases, n, FURNACE_N, 0, n * FURNACE_N * SK_OUT);
    ASSERT_TRUE(res != NULL);
    case_means(res, n, FURNACE_N, 0, out);
    free(res);
}

static const float k_mu3[3] = {0.2f, 0.6f, 1.0f};

/* ---------- layers_clearcoat_furnace ---------- */

/* Clearcoat 1 over a white metal (base 1, metallic 1): never above 1 + 1 %, for every coat and
 * base roughness and view angle. */
static void clearcoat_furnace(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float coat_r[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    static const float base_r[3] = {0.2f, 0.6f, 1.0f};
    sk_case cases[36];
    for (u32 i = 0; i < 36u; ++i) {
        cases[i] = base_case(base_r[(i / 3u) % 3u], 1.0f, 1.0f, k_mu3[i % 3u]);
        with_coat(&cases[i], 1.0f, coat_r[i / 9u]);
    }
    double e[36];
    furnace(f, cases, 36, e);
    for (u32 i = 0; i < 36u; ++i) {
        printf("  coat roughness %.2f base roughness %.1f mu_o %.1f: albedo %.4f\n",
               (double)cases[i].p1[1], (double)cases[i].p0[0], (double)cases[i].p0[3], e[i]);
        EXPECT_TRUE(e[i] <= 1.01);
        EXPECT_TRUE(e[i] >= 0.9); /* a white coated metal keeps almost everything */
    }
}

/* ---------- layers_sheen_furnace ---------- */

/* Sheen (colour 1) over a white dielectric (base 1, metallic 0): never above 1 + 1 %. */
static void sheen_furnace(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float sheen_r[4] = {0.1f, 0.3f, 0.6f, 1.0f};
    static const float base_r[2] = {0.3f, 0.8f};
    sk_case cases[24];
    for (u32 i = 0; i < 24u; ++i) {
        cases[i] = base_case(base_r[(i / 3u) % 2u], 0.0f, 1.0f, k_mu3[i % 3u]);
        with_sheen(&cases[i], 1.0f, sheen_r[i / 6u]);
    }
    double e[24];
    furnace(f, cases, 24, e);
    for (u32 i = 0; i < 24u; ++i) {
        printf("  sheen roughness %.1f base roughness %.1f mu_o %.1f: albedo %.4f\n",
               (double)cases[i].p1[2], (double)cases[i].p0[0], (double)cases[i].p0[3], e[i]);
        EXPECT_TRUE(e[i] <= 1.01);
        /* The KHR albedo scaling (min of both directions) loses some of what the sheen takes. */
        EXPECT_TRUE(e[i] >= 0.8);
    }
}

/* ---------- layers_sheen_lut_matches_cpu ---------- */

/* The sheen rows of the GPU LUT against the CPU reference within 1e-3. */
static void sheen_lut_matches_cpu(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, false);
    const u32 n = DE__BRDF_LUT_SIZE, rows = DE__BRDF_LUT_ROWS, floats = 4u * n * rows;
    const u32 pitch = n * 16u;
    float *ref = (float *)malloc(sizeof(float) * floats);
    float *got = (float *)malloc(sizeof(float) * floats);
    ASSERT_TRUE(ref != NULL && got != NULL);
    de__brdf_lut_reference(16384u, ref);
    const de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)pitch * rows,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "lut readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(rb));
    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "lut readback");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier bar;
    memset(&bar, 0, sizeof bar);
    bar.texture = de__brdf_luts_texture(f->luts);
    bar.before = DE_ACCESS_SHADER_READ;
    bar.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(c, &bar, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, de__brdf_luts_texture(f->luts), 0, 0);
    bar.before = DE_ACCESS_COPY_SRC;
    bar.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &bar, 1);
    EXPECT_TRUE(de_error_last() == NULL);
    const de_gpu_fence done = de_gpu_fence_create(f->dev, 0);
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = done, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, done, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(f->dev, done);
    memcpy(got, de_gpu_buffer_map(f->dev, rb), sizeof(float) * floats);
    de_gpu_buffer_destroy(f->dev, rb);
    double worst[2] = {0.0, 0.0};
    u32 bad = 0;
    for (u32 j = n; j < rows; ++j)
        for (u32 i = 0; i < n; ++i)
            for (u32 ch = 0; ch < 2u; ++ch) {
                const size_t t = ((size_t)j * n + i) * 4u + ch;
                const double d = fabs((double)got[t] - (double)ref[t]);
                if (d > worst[ch])
                    worst[ch] = d;
                if (!(d <= 1e-3) && bad++ < 5)
                    printf("  sheen texel (%u, %u) channel %u: gpu %.6f, cpu %.6f\n", i, j - n, ch,
                           (double)got[t], (double)ref[t]);
            }
    /* The table is a plausible albedo: positive, at most a little above 1 (the fitted visibility
     * of the Charlie lobe at grazing angles), rising with roughness at normal incidence. */
    float e_max = 0.0f;
    for (u32 j = n; j < rows; ++j)
        for (u32 i = 0; i < n; ++i) {
            const float e = ref[((size_t)j * n + i) * 4u];
            EXPECT_TRUE(e >= 0.0f);
            e_max = e > e_max ? e : e_max;
        }
    printf("largest sheen albedo %.4f\n", (double)e_max);
    EXPECT_TRUE(e_max <= 1.5f);
    EXPECT_TRUE(ref[((size_t)(rows - 1u) * n + n - 1u) * 4u] > ref[((size_t)n * n + n - 1u) * 4u]);
    printf("worst |gpu - cpu| of the sheen rows: E %.2e, E_avg %.2e\n", worst[0], worst[1]);
    EXPECT_EQ(bad, 0u);
    free(ref);
    free(got);
}

/* ---------- Grid comparisons ---------- */

#define GRID_X 64u
#define GRID_Y 64u

/* evaluate (x, y), pdf and the diffuse share over the sphere grid of each case. */
static float *quad(sb_fx *f, const sk_case *cases, u32 n) {
    return sb_run(f, K_QUAD, cases, n, GRID_X * GRID_Y, GRID_X, n * GRID_X * GRID_Y * SK_OUT);
}

/* The largest difference between case `a` and case `b` of a quad() result, relative to the
 * larger value (plus `floor`). */
static double grid_diff(const float *res, u32 a, u32 b, double floor) {
    double worst = 0.0;
    const u32 g = GRID_X * GRID_Y;
    for (u32 k = 0; k < g; ++k)
        for (u32 col = 0; col < SK_OUT; ++col) {
            const double va = res[((size_t)a * g + k) * SK_OUT + col];
            const double vb = res[((size_t)b * g + k) * SK_OUT + col];
            const double d = fabs(va - vb) / (fmax(fabs(va), fabs(vb)) + floor);
            if (d > worst)
                worst = d;
        }
    return worst;
}

/* Anisotropy 0 (the feature on, strength 0) is the isotropic base. */
static void anisotropy_zero_identity(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float rough[3] = {0.1f, 0.5f, 0.9f};
    sk_case cases[12];
    for (u32 i = 0; i < 6u; ++i) {
        cases[2u * i] = base_case(rough[i % 3u], i < 3u ? 0.0f : 1.0f, 0.8f, 0.7f);
        cases[2u * i + 1u] = cases[2u * i];
        with_anisotropy(&cases[2u * i + 1u], 0.0f, 0.7f);
    }
    float *res = quad(f, cases, 12);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < 6u; ++i) {
        const double d = grid_diff(res, 2u * i, 2u * i + 1u, 1e-30);
        printf("  roughness %.1f metallic %.0f: largest relative difference %.2e\n",
               (double)cases[2u * i].p0[0], (double)cases[2u * i].p0[1], d);
        EXPECT_TRUE(d < 1e-6);
    }
    free(res);
}

/* Rotating the anisotropy direction by pi (the direction negated, exactly) changes nothing;
 * by pi / 2 it does. */
static void anisotropy_rotation(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    sk_case cases[9];
    for (u32 i = 0; i < 3u; ++i) {
        const float angle = 0.3f + 0.9f * (float)i;
        cases[3u * i] = base_case(0.4f, 0.5f, 0.8f, 0.6f);
        with_anisotropy(&cases[3u * i], 0.8f, angle);
        const float dx = cases[3u * i].p4[0], dy = cases[3u * i].p4[1];
        cases[3u * i + 1u] = cases[3u * i];
        cases[3u * i + 1u].p4[0] = -dx;
        cases[3u * i + 1u].p4[1] = -dy;
        cases[3u * i + 2u] = cases[3u * i];
        cases[3u * i + 2u].p4[0] = -dy;
        cases[3u * i + 2u].p4[1] = dx;
    }
    float *res = quad(f, cases, 9);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < 3u; ++i) {
        const double same = grid_diff(res, 3u * i, 3u * i + 1u, 1e-6);
        const double other = grid_diff(res, 3u * i, 3u * i + 2u, 1e-6);
        printf("  angle %.1f: rotated by pi %.2e, by pi / 2 %.2e\n", 0.3 + 0.9 * (double)i, same,
               other);
        EXPECT_TRUE(same < 1e-6);
        EXPECT_TRUE(other > 1e-2);
    }
    free(res);
}

/* Iridescence factor 0 or thickness 0 (the feature on) is the base: evaluate over the grid and
 * the base Fresnel. */
static void iridescence_zero_identity(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    sk_case cases[6];
    for (u32 i = 0; i < 2u; ++i) {
        cases[3u * i] = base_case(0.4f, i == 0 ? 0.0f : 1.0f, 0.7f, 0.6f);
        cases[3u * i + 1u] = cases[3u * i];
        with_iridescence(&cases[3u * i + 1u], 0.0f, 1.3f, 400.0f);
        cases[3u * i + 2u] = cases[3u * i];
        with_iridescence(&cases[3u * i + 2u], 1.0f, 1.3f, 0.0f);
    }
    float *res = quad(f, cases, 6);
    float *film = sb_run(f, K_FILM, cases, 6, 1, 0, 6u * 2u * SK_OUT);
    ASSERT_TRUE(res != NULL && film != NULL);
    for (u32 i = 0; i < 2u; ++i)
        for (u32 k = 1; k <= 2u; ++k) {
            const double d = grid_diff(res, 3u * i, 3u * i + k, 1e-30);
            const float *f0 = film + (3u * i) * 2u * SK_OUT + SK_OUT;
            const float *fk = film + (3u * i + k) * 2u * SK_OUT + SK_OUT;
            EXPECT_TRUE(d == 0.0);
            for (u32 c = 0; c < 3u; ++c)
                EXPECT_TRUE(f0[c] == fk[c]);
        }
    /* And a real film changes the Fresnel. */
    sk_case on = base_case(0.4f, 0.0f, 0.7f, 0.6f);
    with_iridescence(&on, 1.0f, 1.3f, 400.0f);
    float *f_on = sb_run(f, K_FILM, &on, 1, 1, 0, 2u * SK_OUT);
    ASSERT_TRUE(f_on != NULL);
    EXPECT_TRUE(fabs((double)f_on[SK_OUT] - (double)film[SK_OUT]) > 1e-3 ||
                fabs((double)f_on[SK_OUT + 2u] - (double)film[SK_OUT + 2u]) > 1e-3);
    free(f_on);
    free(res);
    free(film);
}

/* ---------- layers_iridescence_reference_hue ---------- */

/* CIE 1931 colour matching functions, the multi-lobe Gaussian fit of Wyman, Sloan and Shirley
 * 2013 ("Simple Analytic Approximations to the CIE XYZ Color Matching Functions"). */
static double piecewise_gauss(double x, double mu, double s1, double s2) {
    const double t = (x - mu) / (x < mu ? s1 : s2);
    return exp(-0.5 * t * t);
}
static void cie_xyz(double nm, double *x, double *y, double *z) {
    *x = 1.056 * piecewise_gauss(nm, 599.8, 37.9, 31.0) +
         0.362 * piecewise_gauss(nm, 442.0, 16.0, 26.7) -
         0.065 * piecewise_gauss(nm, 501.1, 20.4, 26.2);
    *y = 0.821 * piecewise_gauss(nm, 568.8, 46.9, 40.5) +
         0.286 * piecewise_gauss(nm, 530.9, 16.3, 31.1);
    *z = 1.217 * piecewise_gauss(nm, 437.0, 11.8, 36.0) +
         0.681 * piecewise_gauss(nm, 459.0, 26.0, 13.8);
}

static void xyz_to_rgb(const double xyz[3], double rgb[3]) {
    rgb[0] = 3.2404542 * xyz[0] - 1.5371385 * xyz[1] - 0.4985314 * xyz[2];
    rgb[1] = -0.9692660 * xyz[0] + 1.8760108 * xyz[1] + 0.0415560 * xyz[2];
    rgb[2] = 0.0556434 * xyz[0] - 0.2040259 * xyz[1] + 1.0572252 * xyz[2];
}

/* The reference: the exact reflectance of a film (index n2, thickness d nm) between air and a
 * dielectric of index n3 at normal incidence (Airy summation), integrated over 380..780 nm with
 * the CIE functions, in linear Rec.709 white-balanced so a flat spectrum is grey (the convention
 * of the Belcour-Barla fit, whose incoherent term is added as grey). */
static void film_reference(double n2, double n3, double d, double rgb[3]) {
    const double r12 = (1.0 - n2) / (1.0 + n2), r23 = (n2 - n3) / (n2 + n3);
    double xyz[3] = {0, 0, 0}, white[3] = {0, 0, 0};
    for (double nm = 380.0; nm <= 780.0; nm += 0.5) {
        const double delta = 4.0 * 3.14159265358979323846 * n2 * d / nm;
        const double cr = r12 + r23 * cos(delta), ci = r23 * sin(delta);
        const double dr = 1.0 + r12 * r23 * cos(delta), di = r12 * r23 * sin(delta);
        const double rr = (cr * cr + ci * ci) / (dr * dr + di * di);
        double x, y, z;
        cie_xyz(nm, &x, &y, &z);
        xyz[0] += rr * x;
        xyz[1] += rr * y;
        xyz[2] += rr * z;
        white[0] += x;
        white[1] += y;
        white[2] += z;
    }
    double w[3];
    xyz_to_rgb(xyz, rgb);
    xyz_to_rgb(white, w);
    for (u32 c = 0; c < 3u; ++c)
        rgb[c] /= w[c];
}

/* A film of index 1.3 on glass (F0 0.04, index 1.5) at normal incidence, several thicknesses:
 * the GPU's Belcour-Barla Fresnel against the spectral reference, within 0.01 per channel. */
static void iridescence_reference_hue(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float thick[6] = {150.0f, 250.0f, 350.0f, 450.0f, 600.0f, 800.0f};
    sk_case cases[6];
    for (u32 i = 0; i < 6u; ++i) {
        cases[i] = base_case(0.5f, 0.0f, 0.04f, 1.0f); /* p0.z: the base F0 for cs_film */
        with_iridescence(&cases[i], 1.0f, 1.3f, thick[i]);
    }
    float *film = sb_run(f, K_FILM, cases, 6, 1, 0, 6u * 2u * SK_OUT);
    ASSERT_TRUE(film != NULL);
    for (u32 i = 0; i < 6u; ++i) {
        double ref[3];
        film_reference(1.3, 1.5, thick[i], ref);
        const float *g = film + i * 2u * SK_OUT;
        printf("  %3.0f nm: gpu (%.4f, %.4f, %.4f), spectral (%.4f, %.4f, %.4f)\n",
               (double)thick[i], (double)g[0], (double)g[1], (double)g[2], ref[0], ref[1], ref[2]);
        for (u32 c = 0; c < 3u; ++c)
            EXPECT_TRUE(fabs((double)g[c] - ref[c]) <= 0.01);
    }
    free(film);
}

/* ---------- layers_sample_pdf_consistency ---------- */

/* Each layer alone and all together: the pdf integrates to 1 over the sphere (+- 1 %), sample's
 * weight is evaluate / pdf, and the estimator equals the quadrature of evaluate (+- 1 %). */
static void sample_pdf_consistency(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    enum { N_CASES = 10 };
    sk_case cases[N_CASES];
    for (u32 i = 0; i < N_CASES; ++i)
        cases[i] = base_case(0.5f, 0.3f, 0.8f, i % 2u ? 0.8f : 0.35f);
    with_coat(&cases[0], 1.0f, 0.3f);
    with_coat(&cases[1], 0.6f, 0.6f);
    with_sheen(&cases[2], 1.0f, 0.5f);
    with_sheen(&cases[3], 0.6f, 0.9f);
    with_anisotropy(&cases[4], 0.8f, 0.4f);
    with_anisotropy(&cases[5], 0.5f, 1.2f);
    with_iridescence(&cases[6], 1.0f, 1.3f, 400.0f);
    with_iridescence(&cases[7], 0.7f, 1.6f, 650.0f);
    for (u32 i = 8u; i < 10u; ++i) {
        with_coat(&cases[i], 0.8f, 0.4f);
        with_sheen(&cases[i], 0.8f, 0.6f);
        with_anisotropy(&cases[i], 0.6f, 0.9f);
        with_iridescence(&cases[i], 0.8f, 1.4f, 500.0f);
    }
    const u32 gx = 512u, gy = 512u, n = 8192u;
    float *q = sb_run(f, K_QUAD, cases, N_CASES, gx * gy, gx, N_CASES * gx * gy * SK_OUT);
    float *smp = sb_run(f, K_SAMPLE, cases, N_CASES, n, 0, N_CASES * n * SK_OUT);
    ASSERT_TRUE(q != NULL && smp != NULL);
    double eval_q[N_CASES], pdf_q[N_CASES], est[N_CASES];
    case_means(q, N_CASES, gx * gy, 0, eval_q);
    case_means(q, N_CASES, gx * gy, 1, pdf_q);
    case_means(smp, N_CASES, n, 0, est);
    const double dw = 4.0 * 3.14159265358979323846;
    static const char *const names[N_CASES] = {"coat",       "coat",       "sheen", "sheen",
                                               "anisotropy", "anisotropy", "film",  "film",
                                               "all layers", "all layers"};
    u32 weight_bad = 0;
    for (u32 i = 0; i < N_CASES; ++i) {
        const double albedo = eval_q[i] * dw, ipdf = pdf_q[i] * dw;
        printf("  %-10s mu_o %.2f: integral of pdf %.4f, albedo %.4f, estimator %.4f\n", names[i],
               (double)cases[i].p0[3], ipdf, albedo, est[i]);
        EXPECT_TRUE(fabs(ipdf - 1.0) <= 0.01);
        EXPECT_TRUE(fabs(est[i] - albedo) <= 0.01 * albedo);
        for (u32 k = 0; k < n; ++k) {
            const float *v = smp + ((size_t)i * n + k) * SK_OUT;
            const bool ok = fabsf(v[0] - v[2]) <= 1e-4f * fabsf(v[2]) + 1e-6f &&
                            fabsf(v[1] - v[3]) <= 1e-5f * fabsf(v[3]) + 1e-9f;
            if (!ok && weight_bad++ < 5)
                printf("  case %u sample %u: weight %g, evaluate / pdf %g, pdf %g / %g\n", i, k,
                       (double)v[0], (double)v[2], (double)v[1], (double)v[3]);
        }
    }
    EXPECT_EQ(weight_bad, 0u);
    free(q);
    free(smp);
}

#define LAYER_GPU_TESTS(sfx, backend)                                                              \
    TEST_F(layers, clearcoat_furnace_##sfx, sb_fx) {                                               \
        clearcoat_furnace(f, backend);                                                             \
    }                                                                                              \
    TEST_F(layers, sheen_furnace_##sfx, sb_fx) {                                                   \
        sheen_furnace(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(layers, sheen_lut_matches_cpu_##sfx, sb_fx) {                                           \
        sheen_lut_matches_cpu(f, backend);                                                         \
    }                                                                                              \
    TEST_F(layers, anisotropy_zero_identity_##sfx, sb_fx) {                                        \
        anisotropy_zero_identity(f, backend);                                                      \
    }                                                                                              \
    TEST_F(layers, anisotropy_rotation_##sfx, sb_fx) {                                             \
        anisotropy_rotation(f, backend);                                                           \
    }                                                                                              \
    TEST_F(layers, iridescence_zero_identity_##sfx, sb_fx) {                                       \
        iridescence_zero_identity(f, backend);                                                     \
    }                                                                                              \
    TEST_F(layers, iridescence_reference_hue_##sfx, sb_fx) {                                       \
        iridescence_reference_hue(f, backend);                                                     \
    }                                                                                              \
    TEST_F(layers, sample_pdf_consistency_##sfx, sb_fx) {                                          \
        sample_pdf_consistency(f, backend);                                                        \
    }

LAYER_GPU_TESTS(metal, DE_GPU_METAL)
LAYER_GPU_TESTS(vulkan, DE_GPU_VULKAN)
LAYER_GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ---------- layers_flags_compile_out ---------- */

#if defined(DE_LAY_TEST_COOK) && DE_LAY_TEST_COOK

/* data/standard_layers_flags.slang cooked to SPIR-V: the Standard model with no layer compiled in
 * is about the size of the base alone (within 10 %), and every layer adds code. */
TEST(layers, flags_compile_out) {
    char msg[200];
    if (!de__slang_available(msg, sizeof msg))
        SKIP("no Slang compiler for this host");
    ASSERT_TRUE(mount_sources());
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
    const char *targets[1] = {"spirv"};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "standard_layers_flags";
    a.type = "shader";
    a.source = "standard_layers_flags.slang";
    a.file = "laysrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    ASSERT_EQ(de_cooker_create(&cd, &c), DE_OK);
    ASSERT_EQ(de_cooker_register_type(c, &t), DE_OK);
    de_cook_input in = {.asset = &a, .asset_root = "laysrc:/", .source = NULL};
    ASSERT_EQ(de_cooker_cook(c, &in, &res), DE_OK);
    de__shader_view view;
    ASSERT_EQ(de__shader_view_init(res.data, res.size, &view), DE_OK);
    static const char *const entries[3] = {"cs_none", "cs_base", "cs_all"};
    u64 size[3] = {0, 0, 0};
    for (u32 e = 0; e < 3u; ++e) {
        de_gpu_shader_blob blob = {0};
        de__shader_record rec;
        ASSERT_TRUE(find_entry(&view, (u32)DE__SHADER_TARGET_SPIRV, entries[e], &blob, &rec));
        size[e] = blob.size;
    }
    printf("SPIR-V bytes: no layers %llu, base only %llu, every layer %llu\n",
           (unsigned long long)size[0], (unsigned long long)size[1], (unsigned long long)size[2]);
    /* The model with no layer: the base plus the layer stack's bookkeeping (lobe probabilities),
     * within 10 % of the base alone; every layer compiled in: well over twice that. */
    EXPECT_TRUE((double)size[0] <= 1.10 * (double)size[1]);
    EXPECT_TRUE((double)size[2] > 2.0 * (double)size[0]);
    de_cook_result_free(&res);
    de_cooker_destroy(c);
}

#else

TEST(layers, flags_compile_out) {
    SKIP("this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
}

#endif
