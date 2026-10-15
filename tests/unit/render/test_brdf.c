/*
 * test_brdf.c: the BSDF modules of shaders/brdf/, the material interface and the material model
 * registry (DENG-298). Executable test_render_brdf; CTest names render_brdf.<suite>.<test>.
 *
 *   brdf.*              the C reference (brdf_ref.c): the VNDF reflection pdf integrates to 1,
 *                       the albedo estimator (f cos / pdf over VNDF samples) matches a quadrature
 *                       of the BRDF, reciprocity, the F82-tint and dielectric Fresnel properties
 *   brdf.gpu_*_<b>      tests/unit/render/data/brdf_kernels.slang, cooked at test time with the
 *                       engine's shaders/ on the same root, run on backend <b> (metal, vulkan,
 *                       d3d12; SKIP without the device or the shader importer): every function's
 *                       GPU value against the C reference (1e-4 relative), the albedo estimator
 *                       from the GPU's samples, and reciprocity of a GGX + Lambert test BSDF
 *                       built from the modules (the S13 placeholder Standard; DENG-299)
 *   material_model.*    the feature flags (material_interface.slang == material_models.h), the
 *                       registry's ids and limits (null device), and de_material_model_register
 *                       with shader assets cooked from
 * tests/unit/render/data/material_model_*.slang (null backend; SKIP without the shader importer)
 *
 * Directions are in the local shading frame (Z = the normal), as everywhere in shaders/brdf/.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, sscanf: test code reading the shader sources */
#endif
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_shader.h>
#include <dengine/de_task.h>

#include "brdf_ref.h"
#include "render/materials/material_models.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#if defined(DE_BRDF_TEST_COOK) && DE_BRDF_TEST_COOK
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
#define PI_F    3.14159265358979323846f

/* ---------- Cases ---------- */

static const float k_alphas[4] = {0.05f, 0.3f, 0.7f, 1.0f};

static brdf_v3 dir(float theta_deg, float phi_deg) {
    const float t = theta_deg * PI_F / 180.0f, p = phi_deg * PI_F / 180.0f;
    return brdf_v3_make(sinf(t) * cosf(p), sinf(t) * sinf(p), cosf(t));
}

/** One case of brdf_kernels.slang: four float4 (see the kernel). */
typedef struct bk_case {
    float a[4];  /* ax, ay, u0, u1 */
    float o[4];  /* wo, roughness */
    float n[4];  /* wi, metallic */
    float fr[4]; /* cos, f0, tint (eta - 1), base */
} bk_case;

/** Floats a case writes (brdf_kernels.slang BK_OUT). */
#define BK_OUT 24u
#define GRID   512u /* 4 alphas x 2 anisotropies x 4 theta_o x 4 theta_i x 4 phi */
/** Albedo cases: 4 alphas x 3 theta_o x BK_ALBEDO_N VNDF samples. */
#define BK_ALBEDO_N 4096u
#define BK_CASES    (GRID + 12u * BK_ALBEDO_N)

static const char *const k_out_names[BK_OUT] = {
    "ggx_d",           "ggx_d_aniso",     "ggx_g1",       "ggx_g2",          "schlick",
    "f82_tint",        "dielectric",      "f0_from_ior",  "lambert",         "burley",
    "vndf_h.x",        "vndf_h.y",        "vndf_h.z",     "vndf_pdf",        "reflect_pdf",
    "standard(wo,wi)", "standard(wi,wo)", "standard_pdf", "cosine.x",        "cosine.y",
    "cosine.z",        "radical_inverse", "ggx_specular", "estimator_weight"};

static void hammersley(u32 i, u32 n, float *u0, float *u1) {
    *u0 = ((float)i + 0.5f) / (float)n;
    *u1 = brdf_radical_inverse2(i);
}

static void set_case(bk_case *c, float ax, float ay, brdf_v3 wo, brdf_v3 wi, float u0, float u1,
                     u32 i) {
    static const float f0s[4] = {0.02f, 0.04f, 0.5f, 0.95f};
    static const float tints[4] = {0.5f, 0.33f, 1.0f, 0.8f};
    static const float metals[3] = {0.0f, 0.3f, 1.0f};
    c->a[0] = ax;
    c->a[1] = ay;
    c->a[2] = u0;
    c->a[3] = u1;
    c->o[0] = wo.x;
    c->o[1] = wo.y;
    c->o[2] = wo.z;
    c->o[3] = sqrtf(ax);
    c->n[0] = wi.x;
    c->n[1] = wi.y;
    c->n[2] = wi.z;
    c->n[3] = metals[i % 3u];
    c->fr[0] = wo.z;
    c->fr[1] = f0s[i % 4u];
    c->fr[2] = tints[(i / 4u) % 4u];
    c->fr[3] = 0.8f;
}

/* The grid cases [0, GRID), then the albedo cases. */
static void make_cases(bk_case *c) {
    static const float thetas[4] = {5.0f, 30.0f, 60.0f, 85.0f};
    static const float phis[4] = {0.0f, 60.0f, 135.0f, 250.0f};
    u32 i = 0;
    for (u32 ai = 0; ai < 4u; ++ai)
        for (u32 an = 0; an < 2u; ++an)
            for (u32 to = 0; to < 4u; ++to)
                for (u32 ti = 0; ti < 4u; ++ti)
                    for (u32 p = 0; p < 4u; ++p, ++i) {
                        const float ax = k_alphas[ai];
                        const float ay = an == 0u ? ax : fmaxf(ax * 0.4f, 0.02f);
                        float u0, u1;
                        hammersley(i, GRID, &u0, &u1);
                        set_case(&c[i], ax, ay, dir(thetas[to], 20.0f),
                                 dir(thetas[ti], 20.0f + phis[p]), u0, u1, i);
                    }
    static const float albedo_thetas[3] = {10.0f, 45.0f, 75.0f};
    for (u32 ai = 0; ai < 4u; ++ai)
        for (u32 to = 0; to < 3u; ++to)
            for (u32 s = 0; s < BK_ALBEDO_N; ++s, ++i) {
                float u0, u1;
                hammersley(s, BK_ALBEDO_N, &u0, &u1);
                const brdf_v3 wo = dir(albedo_thetas[to], 0.0f);
                set_case(&c[i], k_alphas[ai], k_alphas[ai], wo, wo, u0, u1, i);
            }
}

/* The albedo estimator's weight of the VNDF sample for (wo, u): f cos / pdf, F = 1. */
static float estimator_weight(float ax, float ay, brdf_v3 wo, float u0, float u1) {
    const brdf_v3 h = brdf_ggx_vndf_sample(ax, ay, wo, u0, u1);
    const brdf_v3 wi = brdf_reflect(wo, h);
    const float pdf = brdf_ggx_reflect_pdf(ax, ay, wo, wi);
    return wi.z > 0.0f && pdf > 0.0f ? brdf_ggx_specular(ax, ay, wo, wi) * wi.z / pdf : 0.0f;
}

/* What brdf_kernels.slang writes for case `c` (index i), from the C reference. */
static void reference(const bk_case *c, u32 i, float *v) {
    const float ax = c->a[0], ay = c->a[1];
    const brdf_v3 wo = brdf_v3_make(c->o[0], c->o[1], c->o[2]);
    const brdf_v3 wi = brdf_v3_make(c->n[0], c->n[1], c->n[2]);
    const brdf_v3 h = brdf_normalize(brdf_v3_make(wo.x + wi.x, wo.y + wi.y, wo.z + wi.z));
    v[0] = brdf_ggx_d(ax, c->fr[0]);
    v[1] = brdf_ggx_d_aniso(ax, ay, h);
    v[2] = brdf_ggx_g1(ax, ay, wo);
    v[3] = brdf_ggx_g2(ax, ay, wo, wi);
    v[4] = brdf_fresnel_schlick(c->fr[1], 1.0f, c->fr[0]);
    v[5] = brdf_fresnel_f82_tint(c->fr[1], c->fr[2], c->fr[0]);
    v[6] = brdf_fresnel_dielectric(c->fr[0], 1.0f + c->fr[2]);
    v[7] = brdf_f0_from_ior(1.0f + c->fr[2], 1.0f);
    v[8] = brdf_diffuse_lambert();
    v[9] = brdf_diffuse_burley(c->o[3], wo.z, wi.z, brdf_dot(wi, h));
    const brdf_v3 hs = brdf_ggx_vndf_sample(ax, ay, wo, c->a[2], c->a[3]);
    v[10] = hs.x;
    v[11] = hs.y;
    v[12] = hs.z;
    v[13] = brdf_ggx_vndf_pdf(ax, ay, wo, hs);
    v[14] = brdf_ggx_reflect_pdf(ax, ay, wo, wi);
    v[15] = brdf_standard_evaluate(c->fr[3], c->o[3], c->n[3], 1.0f, wo, wi);
    v[16] = brdf_standard_evaluate(c->fr[3], c->o[3], c->n[3], 1.0f, wi, wo);
    v[17] = brdf_standard_pdf(c->o[3], c->n[3], wo, wi);
    const brdf_v3 cs = brdf_sample_cosine_hemisphere(c->a[2], c->a[3]);
    v[18] = cs.x;
    v[19] = cs.y;
    v[20] = cs.z;
    v[21] = brdf_radical_inverse2(i * 2654435761u);
    v[22] = brdf_ggx_specular(ax, ay, wo, wi);
    v[23] = estimator_weight(ax, ay, wo, c->a[2], c->a[3]);
}

/* ---------- Integrals (C reference) ---------- */

/*
 * The integral over half vectors h of g(h), with h distributed as GGX D(h) cos(theta_h) (the
 * classic D sampling: tan(theta_h) = alpha sqrt(s / (1 - s)), phi = 2 pi t) at points (s, t), as
 * the mean of g(h) / (D(h) cos(theta_h)). `points` gives (s, t) for index k.
 */
typedef double (*h_integrand)(float alpha, brdf_v3 wo, brdf_v3 h);
typedef void (*h_points)(u32 k, u32 n, float *s, float *t);

static double integrate_h(float alpha, brdf_v3 wo, h_integrand g, h_points pts, u32 n) {
    double sum = 0.0;
    for (u32 k = 0; k < n; ++k) {
        float s, t;
        pts(k, n, &s, &t);
        const float tan_t = alpha * sqrtf(s / (1.0f - s));
        const float cos_t = 1.0f / sqrtf(1.0f + tan_t * tan_t);
        const float sin_t = tan_t * cos_t;
        const float phi = 2.0f * PI_F * t;
        const brdf_v3 h = brdf_v3_make(sin_t * cosf(phi), sin_t * sinf(phi), cos_t);
        const double q = (double)brdf_ggx_d(alpha, cos_t) * (double)cos_t;
        if (q > 0.0)
            sum += g(alpha, wo, h) / q;
    }
    return sum / (double)n;
}

/* The reflected direction's pdf in h-space: pdf(wi(h)) |d wi / d h| = pdf 4 (wo.h). */
static double g_pdf(float alpha, brdf_v3 wo, brdf_v3 h) {
    const float wo_h = brdf_dot(wo, h);
    if (!(wo_h > 0.0f))
        return 0.0;
    const brdf_v3 wi = brdf_reflect(wo, h);
    return (double)brdf_ggx_reflect_pdf(alpha, alpha, wo, wi) * 4.0 * (double)wo_h;
}

/* f cos in h-space (F = 1). */
static double g_albedo(float alpha, brdf_v3 wo, brdf_v3 h) {
    const float wo_h = brdf_dot(wo, h);
    if (!(wo_h > 0.0f))
        return 0.0;
    const brdf_v3 wi = brdf_reflect(wo, h);
    return (double)brdf_ggx_specular(alpha, alpha, wo, wi) * (double)wi.z * 4.0 * (double)wo_h;
}

static void pts_hammersley(u32 k, u32 n, float *s, float *t) {
    hammersley(k, n, s, t);
}
/* A midpoint grid of 2048 x (n / 2048). */
static void pts_grid(u32 k, u32 n, float *s, float *t) {
    const u32 ns = 2048u, nt = n / ns;
    *s = ((float)(k % ns) + 0.5f) / (float)ns;
    *t = ((float)(k / ns) + 0.5f) / (float)nt;
}

/** The directional albedo of the GGX reflection (F = 1) at `wo`, by quadrature. */
static double albedo_quadrature(float alpha, brdf_v3 wo) {
    return integrate_h(alpha, wo, g_albedo, pts_grid, 2048u * 256u);
}

static bool within(double got, double want, double rel) {
    return fabs(got - want) <= rel * fabs(want);
}

/* ---------- brdf.* (C reference) ---------- */

/* The VNDF reflection pdf integrates to 1 over the sphere, for every alpha and view angle: a
 * quasi-Monte-Carlo estimate in half-vector space (an independent parameterisation: GGX D
 * sampling), +- 1 %. */
TEST(brdf, vndf_pdf_integrates_to_one) {
    static const float thetas[4] = {5.0f, 40.0f, 70.0f, 88.0f};
    for (u32 a = 0; a < 4u; ++a)
        for (u32 t = 0; t < 4u; ++t) {
            const brdf_v3 wo = dir(thetas[t], 30.0f);
            const double v = integrate_h(k_alphas[a], wo, g_pdf, pts_hammersley, 1u << 16);
            if (!within(v, 1.0, 0.01))
                printf("  alpha %.2f theta_o %.0f: integral %.5f\n", (double)k_alphas[a],
                       (double)thetas[t], v);
            EXPECT_TRUE(within(v, 1.0, 0.01));
        }
}

/* E[f cos / pdf] over VNDF samples equals the quadrature of f cos (+- 1 %): sampling and pdf
 * agree, for alpha in {0.05, 0.3, 0.7, 1}. */
TEST(brdf, albedo_estimator) {
    static const float thetas[3] = {10.0f, 45.0f, 75.0f};
    for (u32 a = 0; a < 4u; ++a)
        for (u32 t = 0; t < 3u; ++t) {
            const float alpha = k_alphas[a];
            const brdf_v3 wo = dir(thetas[t], 0.0f);
            const double ref = albedo_quadrature(alpha, wo);
            double est = 0.0;
            const u32 n = 1u << 16;
            for (u32 i = 0; i < n; ++i) {
                float u0, u1;
                hammersley(i, n, &u0, &u1);
                est += estimator_weight(alpha, alpha, wo, u0, u1);
            }
            est /= (double)n;
            printf("  alpha %.2f theta_o %2.0f: albedo %.5f, estimator %.5f\n", (double)alpha,
                   (double)thetas[t], ref, est);
            EXPECT_TRUE(ref > 0.0 && ref <= 1.0 + 1e-3);
            EXPECT_TRUE(within(est, ref, 0.01));
        }
}

/* f(wo, wi) == f(wi, wo) for the reflection lobes (evaluate returns f cos(theta_i), so the cos is
 * divided out): GGX specular, Burley, the S13 Standard BSDF; 1e-5 relative. */
TEST(brdf, reciprocity) {
    bk_case *c = (bk_case *)calloc(GRID, sizeof *c);
    ASSERT_TRUE(c != NULL);
    bk_case *all = (bk_case *)calloc(BK_CASES, sizeof *all);
    ASSERT_TRUE(all != NULL);
    make_cases(all);
    memcpy(c, all, GRID * sizeof *c);
    free(all);
    u32 bad = 0, checked = 0;
    for (u32 i = 0; i < GRID; ++i) {
        const brdf_v3 wo = brdf_v3_make(c[i].o[0], c[i].o[1], c[i].o[2]);
        const brdf_v3 wi = brdf_v3_make(c[i].n[0], c[i].n[1], c[i].n[2]);
        const float ax = c[i].a[0], ay = c[i].a[1];
        const double s1 = brdf_ggx_specular(ax, ay, wo, wi), s2 = brdf_ggx_specular(ax, ay, wi, wo);
        const double f1 =
            brdf_standard_evaluate(c[i].fr[3], c[i].o[3], c[i].n[3], 1.0f, wo, wi) / (double)wi.z;
        const double f2 =
            brdf_standard_evaluate(c[i].fr[3], c[i].o[3], c[i].n[3], 1.0f, wi, wo) / (double)wo.z;
        const brdf_v3 h = brdf_normalize(brdf_v3_make(wo.x + wi.x, wo.y + wi.y, wo.z + wi.z));
        const double b1 = brdf_diffuse_burley(c[i].o[3], wo.z, wi.z, brdf_dot(wi, h));
        const double b2 = brdf_diffuse_burley(c[i].o[3], wi.z, wo.z, brdf_dot(wo, h));
        const bool ok = fabs(s1 - s2) <= 1e-5 * fmax(fabs(s1), 1e-30) &&
                        fabs(f1 - f2) <= 1e-5 * fmax(fabs(f1), 1e-30) &&
                        fabs(b1 - b2) <= 1e-5 * fabs(b1);
        if (!ok && bad++ < 5)
            printf("  case %u: specular %g / %g, standard %g / %g, burley %g / %g\n", i, s1, s2, f1,
                   f2, b1, b2);
        ++checked;
    }
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(checked, GRID);
    free(c);
}

/* F82-tint: tint 1 is Schlick; at mu = 1/7 it is tint x Schlick; F(1) = F0, F(0) = 1. The exact
 * dielectric Fresnel: 0 for eta 1, F0 at normal incidence, total internal reflection gives 1. */
TEST(brdf, fresnel_f82) {
    static const float f0s[4] = {0.04f, 0.3f, 0.6f, 0.95f};
    static const float tints[3] = {0.2f, 0.7f, 1.0f};
    for (u32 i = 0; i < 4u; ++i) {
        for (u32 k = 0; k <= 20u; ++k) {
            const float mu = (float)k / 20.0f;
            EXPECT_FLOAT_EQ(brdf_fresnel_f82_tint(f0s[i], 1.0f, mu),
                            brdf_fresnel_schlick(f0s[i], 1.0f, mu), 1e-6f);
        }
        for (u32 t = 0; t < 3u; ++t) {
            const float mu = 1.0f / 7.0f;
            const float want = tints[t] * brdf_fresnel_schlick(f0s[i], 1.0f, mu);
            EXPECT_FLOAT_EQ(brdf_fresnel_f82_tint(f0s[i], tints[t], mu), want, 1e-5f * want);
            EXPECT_FLOAT_EQ(brdf_fresnel_f82_tint(f0s[i], tints[t], 1.0f), f0s[i], 1e-6f);
            EXPECT_FLOAT_EQ(brdf_fresnel_f82_tint(f0s[i], tints[t], 0.0f), 1.0f, 1e-6f);
        }
    }
    EXPECT_FLOAT_EQ(brdf_fresnel_dielectric(0.7f, 1.0f), 0.0f, 1e-7f);
    EXPECT_FLOAT_EQ(brdf_fresnel_dielectric(1.0f, 1.5f), brdf_f0_from_ior(1.5f, 1.0f), 1e-6f);
    EXPECT_FLOAT_EQ(brdf_f0_from_ior(1.5f, 1.0f), 0.04f, 1e-6f);
    EXPECT_FLOAT_EQ(brdf_fresnel_dielectric(0.2f, 1.0f / 1.5f), 1.0f, 0.0f); /* inside glass */
}

/* ---------- brdf.gpu_* ---------- */

typedef struct bk_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_gpu_backend backend;
    char why[700];
    bk_case *cases;
    float *gpu; /* BK_OUT per case, as the GPU wrote them */
} bk_fx;

static void bk_fx_setup(bk_fx *f) {
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

static void bk_fx_teardown(bk_fx *f) {
    if (f->dev != NULL) {
        de_gpu_wait_idle(f->dev);
        if (f->backend != DE_GPU_NULL)
            EXPECT_EQ(validation_errors(f->dev), 0u);
        de_gpu_device_destroy(f->dev);
    }
    free(f->cases);
    free(f->gpu);
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

#if defined(DE_BRDF_TEST_COOK) && DE_BRDF_TEST_COOK

/* "brdfsrc:/" (and the registration tests' root): this directory's data/ over the engine's
 * shaders/, so the test sources import the engine modules ("brdf/ggx", "visbuffer/...") by
 * their paths below shaders/. */
static bool mount_sources(void) {
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("brdfsrc", DE_BRDF_TEST_SHADER_DIR, 0, 0) == DE_OK &&
                  de_fs_mount("brdfsrc", DE_BRDF_TEST_DATA_DIR, 0, 1) == DE_OK;
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

/* brdf_kernels.slang's cs_brdf for the fixture's backend; invalid with f->why. */
static de_gpu_pipeline bk_pipeline(bk_fx *f) {
    const de__shader_target target = de__shader_target_for_backend(f->backend);
    char msg[200];
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(f->why, sizeof f->why, "no Slang compiler: %.150s", msg);
        return (de_gpu_pipeline){0};
    }
    if (!mount_sources()) {
        snprintf(f->why, sizeof f->why, "cannot mount the shader sources");
        return (de_gpu_pipeline){0};
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
    a.id = "brdf_kernels";
    a.type = "shader";
    a.source = "brdf_kernels.slang";
    a.file = "brdfsrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = DE_OK;
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK)
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "brdfsrc:/", .source = NULL};
        r = de_cooker_cook(c, &in, &res);
    }
    de__shader_view view;
    de_gpu_shader_blob cs = {0};
    de__shader_record rec;
    de_gpu_pipeline p = {0};
    if (r == DE_OK && de__shader_view_init(res.data, res.size, &view) == DE_OK &&
        find_entry(&view, (u32)target, "cs_brdf", &cs, &rec)) {
        p = de_gpu_pipeline_create(
            f->dev,
            &(de_gpu_pipeline_desc){
                .kind = DE_PIPE_COMPUTE,
                .cs = cs,
                .push_constant_bytes = 16,
                .threadgroup_size = {rec.threadgroup[0], rec.threadgroup[1], rec.threadgroup[2]},
                .name = "brdf kernels"});
    }
    if (!DE_HANDLE_VALID(p))
        snprintf(f->why, sizeof f->why, "brdf_kernels.slang did not cook or link: %.600s",
                 de_error_last() ? de_error_last()->message : "?");
    de_cook_result_free(&res);
    de_cooker_destroy(c);
    return p;
}

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

/* Opens backend `b`, runs brdf_kernels.slang over every case and fills f->cases and f->gpu. */
static void bk_run(bk_fx *f, de_gpu_backend b) {
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
    const de_gpu_pipeline pipe = bk_pipeline(f);
    if (!DE_HANDLE_VALID(pipe))
        SKIP(f->why);

    f->cases = (bk_case *)calloc(BK_CASES, sizeof *f->cases);
    f->gpu = (float *)calloc((size_t)BK_CASES * BK_OUT, sizeof(float));
    ASSERT_TRUE(f->cases != NULL && f->gpu != NULL);
    make_cases(f->cases);
    const u64 in_bytes = (u64)BK_CASES * sizeof(bk_case);
    const u64 out_bytes = (u64)BK_CASES * BK_OUT * sizeof(float);
    const de_gpu_buffer in =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = in_bytes,
                                                           .usage = DE_USAGE_STORAGE,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .stride = 16,
                                                           .name = "brdf cases"});
    const de_gpu_buffer out = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = out_bytes,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .stride = 4,
                                      .name = "brdf out"});
    const de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = out_bytes,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "brdf readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(in) && DE_HANDLE_VALID(out) && DE_HANDLE_VALID(rb));
    void *map_in = de_gpu_buffer_map(f->dev, in);
    const float *map_rb = (const float *)de_gpu_buffer_map(f->dev, rb);
    ASSERT_TRUE(map_in != NULL && map_rb != NULL);
    memcpy(map_in, f->cases, (size_t)in_bytes);

    de_error_clear(); /* the cook's context */
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_COMPUTE, "brdf");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier bars[2] = {buf_barrier(in, DE_ACCESS_NONE, DE_ACCESS_SHADER_READ),
                              buf_barrier(out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
    de_gpu_cmd_barriers(c, bars, 2);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 pc[4] = {de_gpu_buffer_bindless(f->dev, in), de_gpu_buffer_bindless(f->dev, out),
                       BK_CASES, 0};
    de_gpu_cmd_push_constants(c, pc, sizeof pc);
    de_gpu_cmd_dispatch(c, (BK_CASES + 63u) / 64u, 1, 1);
    bars[0] = buf_barrier(out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, bars, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, out, 0, out_bytes);
    if (de_error_last() != NULL)
        printf("recording: %s\n", de_error_last()->message);
    EXPECT_TRUE(de_error_last() == NULL);
    const de_gpu_fence fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_COMPUTE, &s), DE_OK);
    /* The first dispatch includes the driver's compile of the kernel (DENG-428): a long bound. */
    ASSERT_EQ(de_gpu_fence_wait(f->dev, fence, 1, WAIT_NS), DE_OK);
    memcpy(f->gpu, map_rb, (size_t)out_bytes);
    de_gpu_fence_destroy(f->dev, fence);
    de_gpu_buffer_destroy(f->dev, in);
    de_gpu_buffer_destroy(f->dev, out);
    de_gpu_buffer_destroy(f->dev, rb);
    de_gpu_pipeline_destroy(f->dev, pipe);
}

#else

static void bk_run(bk_fx *f, de_gpu_backend b) {
    (void)f;
    (void)b;
    SKIP("this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
}

#endif

/* The tolerance of output `k` for reference value `want`: 1e-4 relative plus 1e-6 absolute (values
 * near 0). Sampled directions are unit vectors, compared per component within 1e-4 of their
 * length; the cosine sample's z = sqrt(1 - r^2) is ill-conditioned at the disk's rim (an ulp of
 * r^2 moves it by ~3e-4 there), so it gets 1e-3. The estimator weight is f cos / pdf at the
 * sampled direction, which carries the sample's rounding (WARP: 4e-4 at a grazing view): 1e-3
 * relative here; gpu_albedo_estimator checks it statistically. */
static double out_tolerance(u32 k, float want) {
    if (k >= 10u && k <= 12u)
        return 1e-4;
    if (k >= 18u && k <= 19u)
        return 1e-5;
    if (k == 20u)
        return 1e-3;
    if (k == 23u)
        return 1e-3 * fabs((double)want) + 1e-6;
    return 1e-4 * fabs((double)want) + 1e-6;
}

/* Every output of every grid case: the GPU's value against the C reference's (out_tolerance). */
static void gpu_matches_ref(bk_fx *f, de_gpu_backend b) {
    bk_run(f, b);
    u32 bad[BK_OUT] = {0};
    double worst[BK_OUT] = {0};
    for (u32 i = 0; i < GRID; ++i) {
        float want[BK_OUT];
        reference(&f->cases[i], i, want);
        const float *got = f->gpu + (size_t)i * BK_OUT;
        for (u32 k = 0; k < BK_OUT; ++k) {
            const double d = fabs((double)got[k] - (double)want[k]);
            const double tol = out_tolerance(k, want[k]);
            const double rel = d / (fabs((double)want[k]) + 1e-6);
            if (rel > worst[k])
                worst[k] = rel;
            if (!(d <= tol) && bad[k]++ < 3)
                printf("  case %u %s: gpu %.9g, ref %.9g\n", i, k_out_names[k], (double)got[k],
                       (double)want[k]);
        }
    }
    for (u32 k = 0; k < BK_OUT; ++k) {
        printf("  %-16s worst relative difference %.3g%s\n", k_out_names[k], worst[k],
               bad[k] ? "  FAIL" : "");
        EXPECT_EQ(bad[k], 0u);
    }
}

/* The albedo estimator from the GPU's VNDF samples (BK_ALBEDO_N per alpha and view angle) against
 * the quadrature, +- 1 %. */
static void gpu_albedo_estimator(bk_fx *f, de_gpu_backend b) {
    bk_run(f, b);
    for (u32 cfg = 0; cfg < 12u; ++cfg) {
        const u32 first = GRID + cfg * BK_ALBEDO_N;
        const bk_case *c = &f->cases[first];
        const brdf_v3 wo = brdf_v3_make(c->o[0], c->o[1], c->o[2]);
        double est = 0.0;
        for (u32 s = 0; s < BK_ALBEDO_N; ++s)
            est += f->gpu[(size_t)(first + s) * BK_OUT + 23u];
        est /= (double)BK_ALBEDO_N;
        const double ref = albedo_quadrature(c->a[0], wo);
        printf("  alpha %.2f cos_o %.3f: albedo %.5f, GPU estimator %.5f\n", (double)c->a[0],
               (double)wo.z, ref, est);
        EXPECT_TRUE(within(est, ref, 0.01));
    }
}

/* f(wo, wi) == f(wi, wo) (evaluate / cos) of the GGX + Lambert test BSDF on the GPU, 1e-5
 * relative. */
static void gpu_reciprocity(bk_fx *f, de_gpu_backend b) {
    bk_run(f, b);
    u32 bad = 0, nonzero = 0;
    for (u32 i = 0; i < GRID; ++i) {
        const float *v = f->gpu + (size_t)i * BK_OUT;
        const double a = (double)v[15] / (double)f->cases[i].n[2];
        const double c = (double)v[16] / (double)f->cases[i].o[2];
        nonzero += a > 0.0;
        if (!(fabs(a - c) <= 1e-5 * fmax(fabs(a), 1e-30)) && bad++ < 5)
            printf("  case %u: f(wo, wi) %.9g, f(wi, wo) %.9g\n", i, a, c);
    }
    EXPECT_EQ(bad, 0u);
    EXPECT_GT(nonzero, GRID / 2u);
}

#define BRDF_GPU_TESTS(sfx, backend)                                                               \
    TEST_F(brdf, gpu_matches_ref_##sfx, bk_fx) {                                                   \
        gpu_matches_ref(f, backend);                                                               \
    }                                                                                              \
    TEST_F(brdf, gpu_albedo_estimator_##sfx, bk_fx) {                                              \
        gpu_albedo_estimator(f, backend);                                                          \
    }                                                                                              \
    TEST_F(brdf, gpu_reciprocity_##sfx, bk_fx) {                                                   \
        gpu_reciprocity(f, backend);                                                               \
    }

BRDF_GPU_TESTS(metal, DE_GPU_METAL)
BRDF_GPU_TESTS(vulkan, DE_GPU_VULKAN)
BRDF_GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ---------- material_model.* ---------- */

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *s = n >= 0 ? (char *)malloc((size_t)n + 1u) : NULL;
    if (s != NULL && fread(s, 1, (size_t)n, fp) != (size_t)n) {
        free(s);
        s = NULL;
    }
    if (s != NULL)
        s[n] = '\0';
    fclose(fp);
    return s;
}

/* The DE_MATERIAL_FEATURE_* values of material_interface.slang equal material_models.h's, every
 * one is a single bit, and no two share one. */
TEST(material_model, feature_flags_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/materials/material_interface.slang", DE_BRDF_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 count = 0, found = 0, seen = 0;
    const de__material_const *k = de__material_feature_consts(&count);
    static const char key[] = "public static const uint DE_MATERIAL_FEATURE_";
    for (const char *at = text; (at = strstr(at, key)) != NULL;) {
        at += strlen("public static const uint ");
        char name[96];
        long long value = 0;
        if (sscanf(at, "%95[A-Za-z0-9_] = %lli", name, &value) != 2) {
            EXPECT_TRUE(false);
            continue;
        }
        bool ok = false;
        for (u32 i = 0; i < count; ++i)
            if (strcmp(k[i].name, name) == 0) {
                ok = true;
                EXPECT_EQ((u32)value, k[i].value);
            }
        if (!ok)
            printf("  %s is not in de__material_feature_consts\n", name);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(value > 0 && (value & (value - 1)) == 0);
        EXPECT_EQ(seen & (u32)value, 0u);
        seen |= (u32)value;
        ++found;
    }
    free(text);
    EXPECT_EQ(found, count);
    EXPECT_EQ(count, 12u);
    EXPECT_EQ((u32)DE__MATERIAL_FEATURE_ALPHA_MASK, 0x800u);
}

/* A fake resolve kernel for the null device (it compiles nothing). */
static const u32 k_fake_code[4] = {0x07230203u, 0x00010600u, 0u, 0u};

static de_shader_reflection resolve_refl(void) {
    de_shader_reflection r;
    memset(&r, 0, sizeof r);
    r.stage = DE_SHADER_STAGE_COMPUTE;
    r.threadgroup[0] = 64u;
    r.threadgroup[1] = 1u;
    r.threadgroup[2] = 1u;
    r.push_constant_bytes = 112u;
    return r;
}

static bool error_mentions(const char *what) {
    const de_error *e = de_error_last();
    const bool ok = e != NULL && strstr(e->message, what) != NULL;
    if (!ok)
        printf("  error: %s\n", e != NULL ? e->message : "(none)");
    return ok;
}

/* The built-in ids are Standard 0, Unlit 1, Sprite 2, Terrain 3, Water 4; game models get 5 to
 * 31 in order; a 33rd model, a taken name and a kernel that is not the resolve's are refused. */
TEST(material_model, register_builtin_ids) {
    de_gpu_device *dev = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &dev), DE_OK);
    de__material_models *m = NULL;
    ASSERT_EQ(de__material_models_create(dev, NULL, &m), DE_OK);
    static const char *const builtin[5] = {"standard", "unlit", "sprite", "terrain", "water"};
    EXPECT_EQ(de__material_models_count(m), 5u);
    for (u32 i = 0; i < 5u; ++i) {
        EXPECT_STREQ(de__material_models_name(m, i), builtin[i]);
        EXPECT_EQ(de__material_models_find(m, builtin[i]), i);
        EXPECT_FALSE(DE_HANDLE_VALID(de__material_models_resolve_pipeline(m, i)));
    }
    EXPECT_EQ(de__material_models_find(m, "toon"), DE__MATERIAL_MODEL_MAX);
    EXPECT_TRUE(de__material_models_name(m, 5) == NULL);

    const de_gpu_shader_blob code = {
        .code = k_fake_code, .size = sizeof k_fake_code, .entry = "cs_resolve_model"};
    de_shader_reflection refl = resolve_refl();
    u32 id = 0;
    /* Not the resolve kernel: 8 x 8 threads, a vertex shader, too many push constants. */
    refl.threadgroup[0] = 8u;
    refl.threadgroup[1] = 8u;
    EXPECT_EQ(de__material_models_add(m, "bad", &code, &refl, &id), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_mentions("does not implement IMaterialModel"));
    refl = resolve_refl();
    refl.stage = DE_SHADER_STAGE_VERTEX;
    EXPECT_EQ(de__material_models_add(m, "bad", &code, &refl, &id), DE_ERR_INVALID_ARGUMENT);
    refl = resolve_refl();
    refl.push_constant_bytes = 128u;
    EXPECT_EQ(de__material_models_add(m, "bad", &code, &refl, &id), DE_ERR_INVALID_ARGUMENT);
    refl = resolve_refl();
    EXPECT_EQ(de__material_models_add(m, "", &code, &refl, &id), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__material_models_add(m, "unlit", &code, &refl, &id), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de__material_models_count(m), 5u);

    for (u32 want = 5u; want < 32u; ++want) {
        char name[16];
        snprintf(name, sizeof name, "game%u", want);
        ASSERT_EQ(de__material_models_add(m, name, &code, &refl, &id), DE_OK);
        EXPECT_EQ(id, want);
        EXPECT_EQ(de__material_models_find(m, name), want);
        EXPECT_TRUE(DE_HANDLE_VALID(de__material_models_resolve_pipeline(m, want)));
    }
    EXPECT_EQ(de__material_models_count(m), 32u);
    EXPECT_EQ(de__material_models_add(m, "one_too_many", &code, &refl, &id), DE_ERR_BUSY);
    EXPECT_TRUE(error_mentions("all 32 material model ids are taken"));
    EXPECT_EQ(de__material_models_count(m), 32u);
    EXPECT_FALSE(DE_HANDLE_VALID(de__material_models_resolve_pipeline(m, 32u)));
    de__material_models_destroy(m);
    de_gpu_device_destroy(dev);
    de_error_clear();
}

/* The registration test needs the runtime `shader` type to cook sources: Debug and Dev builds with
 * the tools (DE_HAS_COOKER). */
#if defined(DE_BRDF_TEST_COOK) && DE_BRDF_TEST_COOK && defined(DE_HAS_COOKER) && DE_HAS_COOKER

typedef struct mm_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_task_system *tasks;
    de_type_registry *types;
    de_asset_system *assets;
    de_renderer *r;
} mm_fx;

static void mm_fx_setup(mm_fx *f) {
    memset(f, 0, sizeof *f);
    de_fp_env_init_thread();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    de_error_clear();
}

static void mm_fx_teardown(mm_fx *f) {
    if (f->r != NULL)
        de_renderer_destroy(f->r);
    if (f->assets != NULL)
        de_asset_system_destroy(f->assets);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    if (f->dev != NULL)
        de_gpu_device_destroy(f->dev);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

/* A renderer on the null backend with a SOURCE-mode asset system on brdfsrc:/, and the shader
 * type; skips without Slang. */
static void mm_open(mm_fx *f) {
    char msg[200];
    if (!de__slang_available(msg, sizeof msg))
        SKIP("no Slang compiler for this host");
    ASSERT_TRUE(mount_sources());
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->dev), DE_OK);
    ASSERT_EQ(
        de_task_system_create(
            &(de_task_system_desc){.worker_count = 2, .background_worker_count = 1}, &f->tasks),
        DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .mode = DE_ASSET_MODE_SOURCE,
                                                             .types = f->types},
                                     &f->assets),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .assets = f->assets,
                                                     .types = f->types,
                                                     .allocator = &f->alloc},
                                 &f->r),
              DE_OK);
    ASSERT_EQ(de_shader_register_type(f->assets, &f->alloc), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "brdfsrc:/material_models.toml"), DE_OK);
}

/* Loads shader asset `id`; returns the shader and the final state. */
static de_shader mm_load(mm_fx *f, const char *id, de_load_state *state, de_load_handle *h) {
    const de_asset_id aid = de_asset_id_from(id);
    *h = de_asset_load(f->assets, aid, 0);
    *state = de_asset_wait(f->assets, *h, WAIT_NS);
    return de_asset_shader(f->assets, aid);
}

/* de_material_model_register: a shader implementing IMaterialModel gets id 5 and the built-in
 * names resolve; a shader without the resolve kernel, one whose type does not implement the
 * interface (its cook fails) and a taken name are refused with a clear error. */
TEST_F(material_model, register_invalid_shader, mm_fx) {
    mm_open(f);
    de_load_state st_flat, st_none, st_bad;
    de_load_handle h_flat, h_none, h_bad;
    const de_shader flat = mm_load(f, "models/flat", &st_flat, &h_flat);
    const de_shader none = mm_load(f, "models/none", &st_none, &h_none);
    const de_shader bad = mm_load(f, "models/bad", &st_bad, &h_bad);
    EXPECT_EQ((int)st_flat, (int)DE_LOAD_READY);
    EXPECT_EQ((int)st_none, (int)DE_LOAD_READY);
    EXPECT_EQ((int)st_bad, (int)DE_LOAD_FAILED); /* BadModel lacks evaluate, sample, pdf, lobes */
    de_load_info info;
    memset(&info, 0, sizeof info);
    (void)de_asset_status(f->assets, h_bad, &info);
    printf("  models/bad: %s\n", info.first_error_msg);
    EXPECT_TRUE(strstr(info.first_error_msg, "IMaterialModel") != NULL);

    de_error_clear();
    const de_material_model m_none = de_material_model_register(f->r, "none", none);
    EXPECT_FALSE(DE_HANDLE_VALID(m_none));
    EXPECT_TRUE(error_mentions("does not implement IMaterialModel"));
    const de_material_model m_bad = de_material_model_register(f->r, "bad", bad);
    EXPECT_FALSE(DE_HANDLE_VALID(m_bad));
    EXPECT_TRUE(de_error_last() != NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de_material_model_register(f->r, NULL, flat)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_material_model_register(f->r, "flat", (de_shader){0})));

    de_error_clear();
    const de_material_model m_flat = de_material_model_register(f->r, "flat", flat);
    ASSERT_TRUE(DE_HANDLE_VALID(m_flat));
    EXPECT_TRUE(de_error_last() == NULL);
    EXPECT_EQ(de_material_model_id(m_flat), 5u);
    EXPECT_EQ(de_material_model_find(f->r, "flat").id, m_flat.id);
    EXPECT_EQ(de_material_model_id(de_material_model_find(f->r, "standard")), 0u);
    EXPECT_EQ(de_material_model_id(de_material_model_find(f->r, "unlit")), 1u);
    EXPECT_EQ(de_material_model_id(de_material_model_find(f->r, "water")), 4u);
    EXPECT_FALSE(DE_HANDLE_VALID(de_material_model_find(f->r, "none")));
    EXPECT_FALSE(DE_HANDLE_VALID(de_material_model_register(f->r, "flat", flat)));
    EXPECT_TRUE(error_mentions("already registered"));
    EXPECT_FALSE(DE_HANDLE_VALID(de_material_model_register(f->r, "unlit", flat)));
    const de_material_model m_flat2 = de_material_model_register(f->r, "flat2", flat);
    EXPECT_EQ(de_material_model_id(m_flat2), 6u);
    de_error_clear();

    de_asset_release(f->assets, h_flat);
    de_asset_release(f->assets, h_none);
    de_asset_release(f->assets, h_bad);
}

#else

TEST(material_model, register_invalid_shader) {
    SKIP("the runtime shader type has no importer in this build (Release, Shipping or no tools)");
}

#endif
