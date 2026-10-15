/*
 * test_transmission_unlit.c: the Standard model's transmission lobes (KHR_materials_transmission,
 * volume, diffuse_transmission) and the Unlit model (DENG-301). Executable
 * test_render_transmission; CTest names render_transmission.transmission.<test>.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device or the shader
 * importer). They cook tests/unit/render/data/transmission_kernels.slang at test time with the
 * engine's shaders/ on the same root, and make the energy LUT with the renderer's code.
 *
 *   energy_conservation_*          reflection + transmission <= 1 + 1 % (IOR 1..2.4 x roughness)
 *   ior_one_passthrough_*          IOR 1, roughness 0: straight through with all the energy
 *   tir_*                          from inside past the critical angle: full reflection, nothing
 * out volume_beer_lambert_*          volume_transmittance = exp(-sigma_a d), KHR sigma_a
 *   diffuse_transmission_energy_*  reflectance + transmittance <= 1 + 1 %; factor 0.5 splits evenly
 *   sample_pdf_consistency_*       the mixture with transmission (and from inside): integral of
 *                                  pdf 1, weight = evaluate / pdf, estimator = quadrature
 *   unlit_exact_output_*           Unlit emits exactly base colour x vertex colour, no BSDF
 *   sets_forward_flag_*            the TRANSMISSION + FORWARD, VOLUME, DIFFUSE_TRANSMISSION bits
 *                                  and the G-buffer transmission flag
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

#if defined(DE_TRN_TEST_COOK) && DE_TRN_TEST_COOK
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

enum { K_SAMPLE, K_QUAD, K_VOLUME, K_RESOLVE, K_COUNT };
static const char *const k_entries[K_COUNT] = {"cs_sample", "cs_quad", "cs_volume", "cs_resolve"};

/** One case of transmission_kernels.slang (five float4: see case_surface there). */
typedef struct sk_case {
    float p0[4]; /* roughness, metallic, base, mu_o */
    float p1[4]; /* transmission, thickness, ior, diffuse transmission */
    float p2[4]; /* attenuation colour, attenuation distance */
    float p3[4]; /* diffuse transmission colour, - */
    float p4[4]; /* wo, 1 when given */
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
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "transmission test");
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

#if defined(DE_TRN_TEST_COOK) && DE_TRN_TEST_COOK

/* "trnsrc:/": data/ over the engine's shaders/. */
static bool mount_sources(void) {
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("trnsrc", DE_TRN_TEST_SHADER_DIR, 0, 0) == DE_OK &&
                  de_fs_mount("trnsrc", DE_TRN_TEST_DATA_DIR, 0, 1) == DE_OK;
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
    a.id = "transmission_kernels";
    a.type = "shader";
    a.source = "transmission_kernels.slang";
    a.file = "trnsrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = DE_OK;
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK)
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "trnsrc:/", .source = NULL};
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
        snprintf(f->why, sizeof f->why, "transmission_kernels.slang did not cook or link: %.600s",
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
#if defined(DE_TRN_TEST_COOK) && DE_TRN_TEST_COOK
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
                                                              .name = "transmission cases"});
    x.out = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .stride = 4,
                                      .name = "transmission out"});
    x.rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = x.out_bytes,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = "transmission readback"});
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

/* Feature bits (material_models.h DE__MATERIAL_FEATURE_*) and G-buffer flags (gbuffer.h). */
#define F_TRANSMISSION         0x10u
#define F_VOLUME               0x20u
#define F_DIFFUSE_TRANSMISSION 0x40u
#define F_UNLIT                0x100u
#define F_FORWARD              0x200u
#define GB_TRANSMISSION        2u
#define GB_UNLIT               16u
#define NO_DISTANCE            1.0e30f

static sk_case base_case(float roughness, float base, float mu_o) {
    sk_case c;
    memset(&c, 0, sizeof c);
    c.p0[0] = roughness;
    c.p0[2] = base;
    c.p0[3] = mu_o;
    c.p1[2] = 1.5f;                     /* ior */
    c.p2[0] = c.p2[1] = c.p2[2] = 1.0f; /* no absorption */
    c.p2[3] = NO_DISTANCE;
    c.p3[0] = c.p3[1] = c.p3[2] = 1.0f; /* diffuse transmission colour */
    return c;
}

static void with_transmission(sk_case *c, float t, float thickness, float ior) {
    c->p1[0] = t;
    c->p1[1] = thickness;
    c->p1[2] = ior;
}

/* wo given explicitly (for directions inside the volume). */
static void with_wo(sk_case *c, float x, float y, float z) {
    c->p4[0] = x;
    c->p4[1] = y;
    c->p4[2] = z;
    c->p4[3] = 1.0f;
}

static void case_means(const float *res, u32 count, u32 per_case, u32 col, double *out) {
    for (u32 c = 0; c < count; ++c) {
        double s = 0.0;
        for (u32 k = 0; k < per_case; ++k)
            s += res[((size_t)c * per_case + k) * SK_OUT + col];
        out[c] = s / per_case;
    }
}

#define SAMPLES 4096u
#define GX      256u
#define GY      256u

/* ---------- transmission_energy_conservation ---------- */

/* Transmission 1 over a white dielectric: reflection + transmission (the albedo over the sphere,
 * an importance-sampled estimate) at most 1 + 1 %, for IOR 1 to 2.4, roughness 0 to 1, two view
 * angles, volumetric and thin-walled. */
static void energy_conservation(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float iors[5] = {1.0f, 1.33f, 1.5f, 2.0f, 2.4f};
    static const float rough[4] = {0.0f, 0.3f, 0.6f, 1.0f};
    enum { N = 80 };
    sk_case cases[N];
    for (u32 i = 0; i < N; ++i) {
        cases[i] = base_case(rough[(i / 2u) % 4u], 1.0f, (i % 2u) ? 0.9f : 0.3f);
        with_transmission(&cases[i], 1.0f, (i / 40u) ? 0.0f : 1.0f, iors[(i / 8u) % 5u]);
    }
    float *res = sb_run(f, K_SAMPLE, cases, N, SAMPLES, 0, N * SAMPLES * SK_OUT);
    ASSERT_TRUE(res != NULL);
    double e[N];
    case_means(res, N, SAMPLES, 0, e);
    double worst = 0.0;
    for (u32 i = 0; i < N; ++i) {
        if (e[i] > worst)
            worst = e[i];
        if (!(e[i] <= 1.01) || !(e[i] >= 0.3))
            printf("  %s ior %.2f roughness %.1f mu_o %.1f: %.4f\n",
                   cases[i].p1[1] > 0.0f ? "volume" : "thin", (double)cases[i].p1[2],
                   (double)cases[i].p0[0], (double)cases[i].p0[3], e[i]);
        EXPECT_TRUE(e[i] <= 1.01);
        /* Single scattering loses some (most at roughness 1, like GGX without compensation). */
        EXPECT_TRUE(e[i] >= 0.3);
    }
    printf("largest reflection + transmission %.4f\n", worst);
    free(res);
}

/* ---------- transmission_ior_one_passthrough ---------- */

/* IOR 1, roughness 0: the light goes straight through (wi = -wo) with all its energy, volumetric
 * and thin-walled. */
static void ior_one_passthrough(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    sk_case cases[4];
    for (u32 i = 0; i < 4u; ++i) {
        cases[i] = base_case(0.0f, 1.0f, i % 2u ? 0.95f : 0.7f);
        with_transmission(&cases[i], 1.0f, i < 2u ? 1.0f : 0.0f, 1.0f);
    }
    float *res = sb_run(f, K_SAMPLE, cases, 4, SAMPLES, 0, 4u * SAMPLES * SK_OUT);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < 4u; ++i) {
        const double mu = cases[i].p0[3];
        double through = 0.0, total = 0.0, straight = 0.0;
        for (u32 k = 0; k < SAMPLES; ++k) {
            const float *v = res + ((size_t)i * SAMPLES + k) * SK_OUT;
            total += v[0];
            if (v[3] < 0.0f) {
                through += v[0];
                /* wi.z of -wo is -mu_o; roughness 0 (alpha 0.002) blurs by about a degree (GGX's
                 * long tail sends a few samples further) */
                if (fabs((double)v[3] + mu) <= 0.02)
                    straight += v[0];
            }
        }
        through /= SAMPLES;
        total /= SAMPLES;
        straight /= SAMPLES;
        printf("  %s mu_o %.2f: transmitted %.4f of %.4f, %.4f within a degree of -wo\n",
               i < 2u ? "volume" : "thin", mu, through, total, straight);
        EXPECT_TRUE(through >= 0.99);
        EXPECT_TRUE(straight >= 0.97);
    }
    free(res);
}

/* ---------- transmission_tir ---------- */

/* From inside glass (IOR 1.5, critical angle 41.8 degrees), smooth: at 60 degrees nothing leaves
 * (every sample reflects back inside, carrying all the energy; evaluate towards the outside
 * integrates to nothing); at 20 degrees most of the light leaves. */
static void tir(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    const double deg = 3.14159265358979323846 / 180.0;
    sk_case cases[2];
    for (u32 i = 0; i < 2u; ++i) {
        const double a = (i == 0 ? 60.0 : 20.0) * deg;
        cases[i] = base_case(0.0f, 1.0f, 0.5f);
        with_transmission(&cases[i], 1.0f, 1.0f, 1.5f);
        with_wo(&cases[i], (float)sin(a), 0.0f, (float)-cos(a)); /* inside: wo.z < 0 */
    }
    float *smp = sb_run(f, K_SAMPLE, cases, 2, SAMPLES, 0, 2u * SAMPLES * SK_OUT);
    float *q = sb_run(f, K_QUAD, cases, 2, GX * GY, GX, 2u * GX * GY * SK_OUT);
    ASSERT_TRUE(smp != NULL && q != NULL);
    for (u32 i = 0; i < 2u; ++i) {
        double out = 0.0, back = 0.0, eval_out = 0.0;
        const double dw = 4.0 * 3.14159265358979323846 / (GX * GY);
        u32 n_out = 0;
        for (u32 k = 0; k < SAMPLES; ++k) {
            const float *v = smp + ((size_t)i * SAMPLES + k) * SK_OUT;
            if (v[3] > 0.0f) {
                out += v[0];
                ++n_out;
            } else {
                back += v[0];
            }
        }
        out /= SAMPLES;
        back /= SAMPLES;
        for (u32 g = 0; g < GX * GY; ++g) {
            const float *v = q + ((size_t)i * GX * GY + g) * SK_OUT;
            if (v[3] > 0.0f)
                eval_out += fabs((double)v[0]) * dw;
        }
        printf("  %.0f degrees inside: transmitted %.4f (%u samples), reflected %.4f, integral "
               "of evaluate outside %.3g\n",
               i == 0 ? 60.0 : 20.0, out, n_out, back, eval_out);
        if (i == 0) {
            EXPECT_EQ(n_out, 0u);
            /* Only facets tilted past the critical angle could pass light; at roughness 0
             * (alpha 0.002) they carry nothing measurable. */
            EXPECT_TRUE(eval_out < 1e-4);
            EXPECT_TRUE(back >= 0.99 && back <= 1.01);
        } else {
            EXPECT_TRUE(out > 0.9);
        }
    }
    free(smp);
    free(q);
}

/* ---------- volume_beer_lambert ---------- */

/* volume_transmittance = exp(-sigma_a d), sigma_a = -ln(attenuation colour) / attenuation
 * distance (KHR_materials_volume); at d = attenuation distance it is the attenuation colour; an
 * infinite distance absorbs nothing. */
static void volume_beer_lambert(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float color[4][3] = {
        {0.9f, 0.5f, 0.1f}, {1.0f, 1.0f, 1.0f}, {0.2f, 0.8f, 0.6f}, {0.5f, 0.5f, 0.5f}};
    static const float dist[4] = {2.0f, 3.0f, 0.5f, NO_DISTANCE};
    static const float path[3] = {0.0f, 1.0f, 2.0f};
    sk_case cases[12];
    for (u32 i = 0; i < 12u; ++i) {
        cases[i] = base_case(0.5f, 1.0f, 1.0f);
        memcpy(cases[i].p2, color[i / 3u], sizeof color[0]);
        cases[i].p2[3] = dist[i / 3u];
        cases[i].p0[0] = i / 3u == 0 && i % 3u == 2u ? 2.0f : path[i % 3u]; /* d = distance */
    }
    float *res = sb_run(f, K_VOLUME, cases, 12, 1, 0, 12u * 2u * SK_OUT);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < 12u; ++i) {
        const float *t = res + i * 2u * SK_OUT;
        const float *sig = t + SK_OUT;
        for (u32 c = 0; c < 3u; ++c) {
            const double d = cases[i].p0[0], dd = cases[i].p2[3];
            const double sigma = dd >= 1e30 ? 0.0 : -log((double)cases[i].p2[c]) / dd;
            const double want = exp(-sigma * d);
            EXPECT_TRUE(fabs((double)sig[c] - sigma) <= 1e-5 * sigma + 1e-7);
            EXPECT_TRUE(fabs((double)t[c] - want) <= 1e-5 * want + 1e-7);
        }
    }
    /* At d = the attenuation distance: the attenuation colour. */
    const float *t2 = res + 2u * 2u * SK_OUT;
    for (u32 c = 0; c < 3u; ++c)
        EXPECT_FLOAT_EQ(t2[c], color[0][c], 1e-5f);
    free(res);
}

/* ---------- diffuse_transmission_energy ---------- */

/* Diffuse transmission over a white dielectric (no specular transmission): hemispherical
 * reflectance + transmittance at most 1 + 1 %. With factor 1 the diffuse reflection is gone; with
 * 0.5 the diffuse reflectance and transmittance are equal. */
static void diffuse_transmission_energy(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float dts[3] = {1.0f, 0.5f, 0.0f};
    sk_case cases[6];
    for (u32 i = 0; i < 6u; ++i) {
        cases[i] = base_case(0.5f, 1.0f, i % 2u ? 0.9f : 0.4f);
        cases[i].p1[3] = dts[i / 2u];
    }
    float *q = sb_run(f, K_QUAD, cases, 6, GX * GY, GX, 6u * GX * GY * SK_OUT);
    ASSERT_TRUE(q != NULL);
    const double dw = 4.0 * 3.14159265358979323846 / (GX * GY);
    for (u32 i = 0; i < 6u; ++i) {
        double r = 0.0, t = 0.0, rd = 0.0, td = 0.0;
        for (u32 g = 0; g < GX * GY; ++g) {
            const float *v = q + ((size_t)i * GX * GY + g) * SK_OUT;
            if (v[3] > 0.0f) {
                r += v[0] * dw;
                rd += v[2] * dw;
            } else {
                t += v[0] * dw;
                td += v[2] * dw;
            }
        }
        printf("  dt %.1f mu_o %.1f: reflectance %.4f (diffuse %.4f), transmittance %.4f "
               "(diffuse %.4f), sum %.4f\n",
               (double)cases[i].p1[3], (double)cases[i].p0[3], r, rd, t, td, r + t);
        EXPECT_TRUE(r + t <= 1.01);
        EXPECT_TRUE(r + t >= 0.95);
        if (i / 2u == 0)
            EXPECT_TRUE(rd < 1e-6);
        if (i / 2u == 1)
            EXPECT_TRUE(fabs(rd - td) <= 0.01 * td);
        if (i / 2u == 2)
            EXPECT_TRUE(t < 1e-6);
    }
    free(q);
}

/* ---------- transmission_sample_pdf_consistency ---------- */

/* The lobe mixture with transmission: the pdf integrates to 1 over the sphere (+- 1 %), sample's
 * weight is evaluate / pdf, and the estimator equals the quadrature of evaluate (+- 1 %); also
 * from inside a volume (smoother: reflections that a rough facet sends out of the volume are
 * dropped there, about 2 % at roughness 0.4). */
static void sample_pdf_consistency(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    enum { N_CASES = 6 };
    sk_case cases[N_CASES];
    cases[0] = base_case(0.4f, 0.8f, 0.7f);
    with_transmission(&cases[0], 1.0f, 1.0f, 1.5f); /* volumetric */
    cases[1] = base_case(0.4f, 0.8f, 0.4f);
    with_transmission(&cases[1], 0.7f, 0.0f, 1.5f); /* thin-walled, partial */
    cases[2] = base_case(0.5f, 0.8f, 0.6f);
    cases[2].p1[3] = 0.5f; /* diffuse transmission */
    cases[3] = base_case(0.5f, 0.8f, 0.8f);
    with_transmission(&cases[3], 0.5f, 1.0f, 1.33f);
    cases[3].p1[3] = 0.6f; /* both */
    cases[3].p0[1] = 0.3f; /* and some metal */
    cases[4] = base_case(0.15f, 0.8f, 0.5f);
    with_transmission(&cases[4], 1.0f, 1.0f, 1.5f);
    with_wo(&cases[4], 0.3f, 0.2f, -0.93f); /* inside, below the critical angle */
    cases[5] = cases[4];
    with_wo(&cases[5], 0.8f, 0.1f, -0.59f); /* inside, past it */
    const u32 gx = 512u, gy = 512u, n = 8192u;
    float *q = sb_run(f, K_QUAD, cases, N_CASES, gx * gy, gx, N_CASES * gx * gy * SK_OUT);
    float *smp = sb_run(f, K_SAMPLE, cases, N_CASES, n, 0, N_CASES * n * SK_OUT);
    ASSERT_TRUE(q != NULL && smp != NULL);
    double eval_q[N_CASES], pdf_q[N_CASES], est[N_CASES];
    case_means(q, N_CASES, gx * gy, 0, eval_q);
    case_means(q, N_CASES, gx * gy, 1, pdf_q);
    case_means(smp, N_CASES, n, 0, est);
    const double dw = 4.0 * 3.14159265358979323846;
    static const char *const names[N_CASES] = {"volume",       "thin",         "diffuse",
                                               "both + metal", "inside below", "inside past"};
    u32 weight_bad = 0;
    for (u32 i = 0; i < N_CASES; ++i) {
        const double albedo = eval_q[i] * dw, ipdf = pdf_q[i] * dw;
        printf("  %-12s: integral of pdf %.4f, albedo %.4f, estimator %.4f\n", names[i], ipdf,
               albedo, est[i]);
        EXPECT_TRUE(fabs(ipdf - 1.0) <= 0.01);
        EXPECT_TRUE(fabs(est[i] - albedo) <= 0.01 * albedo);
        for (u32 k = 0; k < n; ++k) {
            const float *v = smp + ((size_t)i * n + k) * SK_OUT;
            const bool ok = fabsf(v[0] - v[2]) <= 1e-4f * fabsf(v[2]) + 1e-6f;
            if (!ok && weight_bad++ < 5)
                printf("  case %u sample %u: weight %g, evaluate / pdf %g\n", i, k, (double)v[0],
                       (double)v[2]);
        }
    }
    EXPECT_EQ(weight_bad, 0u);
    free(q);
    free(smp);
}

/* ---------- unlit_exact_output / transmission_sets_forward_flag ---------- */

/* Unlit: the emission is exactly base colour x vertex colour (the material's emissive is not
 * added), with the UNLIT feature and G-buffer flag and no BSDF. Lighting never dispatches Unlit
 * (DENG-303: render_clustered_lights checks it), so the lighting target keeps this, times the
 * view's pre-exposure. */
static void unlit_exact_output(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    static const float base[3][3] = {
        {0.2f, 0.4f, 0.8f}, {1.0f, 0.0f, 0.5f}, {0.123f, 0.456f, 0.789f}};
    static const float vcol[3][3] = {{1.0f, 1.0f, 1.0f}, {0.5f, 0.25f, 1.0f}, {0.9f, 0.8f, 0.7f}};
    sk_case cases[3];
    for (u32 i = 0; i < 3u; ++i) {
        cases[i] = base_case(0.5f, 1.0f, 1.0f);
        memcpy(cases[i].p3, base[i], sizeof base[i]);
        memcpy(cases[i].p4, vcol[i], sizeof vcol[i]);
        cases[i].p4[3] = 1.0f; /* Unlit */
    }
    float *res = sb_run(f, K_RESOLVE, cases, 3, 1, 0, 3u * 3u * SK_OUT);
    ASSERT_TRUE(res != NULL);
    for (u32 i = 0; i < 3u; ++i) {
        const float *v = res + i * 3u * SK_OUT;
        for (u32 c = 0; c < 3u; ++c) {
            const float want = (base[i][c] * vcol[i][c]) * 1.0f; /* x texture 1 */
            EXPECT_TRUE(v[c] == want);
        }
        EXPECT_EQ((u32)v[4], F_UNLIT);  /* features */
        EXPECT_EQ((u32)v[5], GB_UNLIT); /* G-buffer flags */
        EXPECT_EQ((u32)v[6], 0u);       /* lobes */
        EXPECT_TRUE(v[7] == 0.0f);      /* evaluate */
        EXPECT_TRUE(v[8] == 0.0f);      /* pdf, sample pdf */
    }
    free(res);
}

/* Transmissive materials carry TRANSMISSION + FORWARD (and the G-buffer transmission flag), a
 * thickness adds VOLUME (in world units: x the instance's scale), diffuse transmission sets
 * DIFFUSE_TRANSMISSION (the G-buffer transmission flag, but not FORWARD), opaque ones none. */
static void sets_forward_flag(sb_fx *f, de_gpu_backend b) {
    sb_open(f, b, true);
    sk_case cases[4];
    for (u32 i = 0; i < 4u; ++i) {
        cases[i] = base_case(0.5f, 2.0f /* instance scale */, 1.0f);
        cases[i].p4[0] = cases[i].p4[1] = cases[i].p4[2] = 1.0f; /* vertex colour */
    }
    with_transmission(&cases[1], 0.8f, 0.0f, 1.5f);  /* thin-walled glass */
    with_transmission(&cases[2], 1.0f, 0.25f, 1.5f); /* volumetric */
    cases[3].p1[3] = 0.6f;                           /* diffuse transmission (a leaf) */
    float *res = sb_run(f, K_RESOLVE, cases, 4, 1, 0, 4u * 3u * SK_OUT);
    ASSERT_TRUE(res != NULL);
    static const u32 want_features[4] = {0u, F_TRANSMISSION | F_FORWARD,
                                         F_TRANSMISSION | F_FORWARD | F_VOLUME,
                                         F_DIFFUSE_TRANSMISSION};
    static const u32 want_gb[4] = {0u, GB_TRANSMISSION, GB_TRANSMISSION, GB_TRANSMISSION};
    for (u32 i = 0; i < 4u; ++i) {
        const float *v = res + i * 3u * SK_OUT;
        printf("  case %u: features 0x%x, G-buffer flags 0x%x, lobes 0x%x, thickness %.3f\n", i,
               (u32)v[4], (u32)v[5], (u32)v[6], (double)v[3]);
        EXPECT_EQ((u32)v[4], want_features[i]);
        EXPECT_EQ((u32)v[5], want_gb[i]);
        EXPECT_EQ(((u32)v[6] & 4u) != 0u, i != 0u); /* DE_BSDF_LOBE_TRANSMISSION */
    }
    EXPECT_FLOAT_EQ(res[2u * 3u * SK_OUT + 3u], 0.5f, 1e-6f); /* 0.25 x scale 2 */
    free(res);
}

#define TRN_GPU_TESTS(sfx, backend)                                                                \
    TEST_F(transmission, energy_conservation_##sfx, sb_fx) {                                       \
        energy_conservation(f, backend);                                                           \
    }                                                                                              \
    TEST_F(transmission, ior_one_passthrough_##sfx, sb_fx) {                                       \
        ior_one_passthrough(f, backend);                                                           \
    }                                                                                              \
    TEST_F(transmission, tir_##sfx, sb_fx) {                                                       \
        tir(f, backend);                                                                           \
    }                                                                                              \
    TEST_F(transmission, volume_beer_lambert_##sfx, sb_fx) {                                       \
        volume_beer_lambert(f, backend);                                                           \
    }                                                                                              \
    TEST_F(transmission, diffuse_transmission_energy_##sfx, sb_fx) {                               \
        diffuse_transmission_energy(f, backend);                                                   \
    }                                                                                              \
    TEST_F(transmission, sample_pdf_consistency_##sfx, sb_fx) {                                    \
        sample_pdf_consistency(f, backend);                                                        \
    }                                                                                              \
    TEST_F(transmission, unlit_exact_output_##sfx, sb_fx) {                                        \
        unlit_exact_output(f, backend);                                                            \
    }                                                                                              \
    TEST_F(transmission, sets_forward_flag_##sfx, sb_fx) {                                         \
        sets_forward_flag(f, backend);                                                             \
    }

TRN_GPU_TESTS(metal, DE_GPU_METAL)
TRN_GPU_TESTS(vulkan, DE_GPU_VULKAN)
TRN_GPU_TESTS(d3d12, DE_GPU_D3D12)
