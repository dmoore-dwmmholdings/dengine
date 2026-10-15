/*
 * brdf_luts.c: the energy LUT of the Standard model (brdf_luts.h; DENG-299): the GPU generation
 * at renderer start and the CPU reference.
 */
#include "render/materials/brdf_luts.h"

#include "render/renderer_internal.h"
#include "render/shaders/engine_shaders.h"

#include <math.h>
#include <string.h>

struct de__brdf_luts {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_gpu_texture tex;
    u32 bindless;
    de_gpu_fence fence;
    u64 fence_value;
};

/** brdf_lut_gen's push constants (lut_gen.slang LutParams). */
typedef struct lut_params {
    u32 target, samples, reserved0, reserved1; /* target: the staging buffer (float4 per texel) */
} lut_params;

static void luts_free(de__brdf_luts *l) {
    if (DE_HANDLE_VALID(l->tex))
        de_gpu_texture_destroy(l->dev, l->tex);
    if (DE_HANDLE_VALID(l->fence))
        de_gpu_fence_destroy(l->dev, l->fence);
    DE_FREE(l->alloc, l, sizeof *l);
}

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

static de_gpu_barrier tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t;
    b.before = before;
    b.after = after;
    return b;
}

de_result de__brdf_luts_create(de_gpu_device *dev, const de_allocator *alloc, de__brdf_luts **out) {
    if (out != NULL)
        *out = NULL;
    if (dev == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__brdf_luts_create: NULL argument");
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__brdf_luts *l = DE_NEW(alloc, de__brdf_luts, DE_MEM_RENDER);
    if (l == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "brdf LUT: no memory");
    memset(l, 0, sizeof *l);
    l->dev = dev;
    l->alloc = alloc;
    l->bindless = DE_GPU_BINDLESS_INVALID;

    static const char *const k_entry[1] = {"cs_brdf_lut"};
    de__engine_shader_code code;
    char why[512] = "";
    de_result r =
        de__engine_shader_load(dev, alloc, "brdf_lut_gen", 0, k_entry, 1, &code, why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY || r == DE_ERR_INVALID_ARGUMENT) {
        luts_free(l);
        return DE_FAIL(r, "brdf LUT: engine shader brdf_lut_gen: %.400s", why);
    }
    if (r != DE_OK) {
        /* No code for this backend in this build: the shaders use the analytic fallback. */
        de_error_clear();
        *out = l;
        return DE_OK;
    }
    const de__engine_shader_entry *e = &code.entries[0];
    const de_gpu_pipeline pipe = de_gpu_pipeline_create(
        dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                     .cs = e->blob,
                                     .push_constant_bytes = (u32)sizeof(lut_params),
                                     .threadgroup_size = {DE__BRDF_LUT_SIZE, 1u, 1u},
                                     .name = "brdf LUT generation"});
    de__engine_shader_code_free(&code);
    l->tex = de_gpu_texture_create(
        dev,
        &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                               .width = DE__BRDF_LUT_SIZE,
                               .height = DE__BRDF_LUT_ROWS,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                               .name = "brdf energy LUT"});
    l->fence = de_gpu_fence_create(dev, 0);
    r = DE_HANDLE_VALID(pipe) && DE_HANDLE_VALID(l->tex) && DE_HANDLE_VALID(l->fence)
            ? DE_OK
            : DE_FAIL_WRAP(DE_ERR_INTERNAL, "brdf LUT: the pipeline, texture or fence");
    /* The kernel writes a storage buffer, and a copy fills the texture: no storage image. Vulkan's
     * GPU-assisted validation (VVL 1.3.275, CI's lavapipe job) checks a storage image's bindless
     * writes against the image's layout when it processes the submission, after later layout
     * changes; a buffer has no layout. */
    const u32 pitch = DE__BRDF_LUT_SIZE * 16u; /* 512 bytes: a multiple of 256 */
    const de_gpu_buffer buf =
        r == DE_OK ? de_gpu_buffer_create(
                         dev, &(de_gpu_buffer_desc){.size = (u64)pitch * DE__BRDF_LUT_ROWS,
                                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                    .stride = 16,
                                                    .name = "brdf energy LUT staging"})
                   : (de_gpu_buffer){0};
    if (r == DE_OK && !DE_HANDLE_VALID(buf))
        r = DE_FAIL_WRAP(DE_ERR_INTERNAL, "brdf LUT: the staging buffer");
    de_gpu_cmd *c = r == DE_OK ? de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, "brdf LUT") : NULL;
    if (r == DE_OK && c == NULL)
        r = DE_FAIL_WRAP(DE_ERR_INTERNAL, "brdf LUT: no command list");
    if (r == DE_OK) {
        de_gpu_barrier b[2] = {buf_barrier(buf, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE),
                               tex_barrier(l->tex, DE_ACCESS_NONE, DE_ACCESS_COPY_DST)};
        de_gpu_cmd_barriers(c, b, 2);
        de_gpu_cmd_set_pipeline(c, pipe);
        const lut_params pc = {.target = de_gpu_buffer_bindless(dev, buf),
                               .samples = DE__BRDF_LUT_SAMPLES};
        de_gpu_cmd_push_constants(c, &pc, (u32)sizeof pc);
        de_gpu_cmd_dispatch(c, DE__BRDF_LUT_ROWS, 1, 1); /* one group per row */
        b[0] = buf_barrier(buf, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
        de_gpu_cmd_barriers(c, b, 1);
        de_gpu_cmd_copy_buffer_to_texture(c, l->tex, 0, 0, buf, 0, pitch);
        b[0] = tex_barrier(l->tex, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
        de_gpu_cmd_barriers(c, b, 1);
        l->fence_value = 1;
        const de_gpu_submit s = {
            .cmds = &c, .cmd_count = 1, .signal = l->fence, .signal_value = l->fence_value};
        r = de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &s);
        if (r != DE_OK)
            r = DE_FAIL_WRAP(r, "brdf LUT: the submission");
    }
    if (DE_HANDLE_VALID(buf))
        de_gpu_buffer_destroy(dev, buf); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(pipe))
        de_gpu_pipeline_destroy(dev, pipe); /* deferred until the GPU is done */
    if (r != DE_OK) {
        luts_free(l);
        return r;
    }
    l->bindless = de_gpu_texture_bindless(dev, l->tex, 0, false);
    *out = l;
    return DE_OK;
}

void de__brdf_luts_destroy(de__brdf_luts *l) {
    if (l != NULL)
        luts_free(l);
}

u32 de__brdf_luts_bindless(const de__brdf_luts *l) {
    return l != NULL ? l->bindless : DE_GPU_BINDLESS_INVALID;
}

de_gpu_texture de__brdf_luts_texture(const de__brdf_luts *l) {
    return l != NULL ? l->tex : (de_gpu_texture){0};
}

de_gpu_fence de__brdf_luts_fence(const de__brdf_luts *l, u64 *value) {
    if (value != NULL)
        *value = l != NULL ? l->fence_value : 0u;
    return l != NULL ? l->fence : (de_gpu_fence){0};
}

u32 de__renderer_brdf_lut(const de_renderer *r) {
    return r != NULL ? de__brdf_luts_bindless(r->brdf_luts) : DE_GPU_BINDLESS_INVALID;
}

/* =================================================================================================
 * The CPU reference: lut_gen.slang's estimator in double precision
 * ============================================================================================== */

#define REF_PI 3.14159265358979323846

typedef struct dv3 {
    double x, y, z;
} dv3;

static dv3 dv3_norm(dv3 v) {
    const double s = 1.0 / sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return (dv3){v.x * s, v.y * s, v.z * s};
}

static double lambda(double a, dv3 w) {
    return (sqrt(1.0 + a * a * (w.x * w.x + w.y * w.y) / (w.z * w.z)) - 1.0) * 0.5;
}

static double radical_inverse(u32 i) {
    i = (i << 16) | (i >> 16);
    i = ((i & 0x00FF00FFu) << 8) | ((i & 0xFF00FF00u) >> 8);
    i = ((i & 0x0F0F0F0Fu) << 4) | ((i & 0xF0F0F0F0u) >> 4);
    i = ((i & 0x33333333u) << 2) | ((i & 0xCCCCCCCCu) >> 2);
    i = ((i & 0x55555555u) << 1) | ((i & 0xAAAAAAAAu) >> 1);
    return (double)i * 2.3283064365386963e-10;
}

/* A and B at (mu, alpha): the mean of G2 / G1 (1 - Fc) and G2 / G1 Fc over VNDF samples. */
static void integrate_ab(double mu, double a, u32 samples, double *out_a, double *out_b) {
    const dv3 wo = {sqrt(1.0 - mu * mu), 0.0, mu};
    const dv3 v = dv3_norm((dv3){wo.x * a, wo.y * a, wo.z});
    const double g1 = 1.0 / (1.0 + lambda(a, wo));
    double sa = 0.0, sb = 0.0;
    for (u32 k = 0; k < samples; ++k) {
        const double u0 = ((double)k + 0.5) / (double)samples, u1 = radical_inverse(k);
        const double phi = 2.0 * REF_PI * u0;
        const double z = (1.0 - u1) * (1.0 + v.z) - v.z;
        const double st = sqrt(fmax(0.0, 1.0 - z * z));
        const dv3 hc = {st * cos(phi) + v.x, st * sin(phi) + v.y, z + v.z};
        const dv3 h = dv3_norm((dv3){hc.x * a, hc.y * a, fmax(hc.z, 0.0)});
        const double wo_h = wo.x * h.x + wo.y * h.y + wo.z * h.z;
        const dv3 wi = {2.0 * wo_h * h.x - wo.x, 2.0 * wo_h * h.y - wo.y, 2.0 * wo_h * h.z - wo.z};
        if (!(wi.z > 0.0))
            continue;
        const double w = (1.0 / (1.0 + lambda(a, wo) + lambda(a, wi))) / g1;
        const double m = 1.0 - fmin(fmax(wo_h, 0.0), 1.0);
        const double fc = m * m * m * m * m;
        sa += w * (1.0 - fc);
        sb += w * fc;
    }
    *out_a = sa / (double)samples;
    *out_b = sb / (double)samples;
}

/* The Charlie sheen lobe (sheen.slang), double precision. */
static double sheen_l(double x, double a) {
    const double t = (1.0 - a) * (1.0 - a);
    const double pa = 21.5473 + (25.3245 - 21.5473) * t, pb = 3.82987 + (3.32435 - 3.82987) * t;
    const double pc = 0.19823 + (0.16801 - 0.19823) * t, pd = -1.97760 + (-1.27393 + 1.97760) * t;
    const double pe = -4.32054 + (-4.85967 + 4.32054) * t;
    return pa / (1.0 + pb * pow(x, pc)) + pd * x + pe;
}
static double sheen_lambda(double mu, double a) {
    return mu < 0.5 ? exp(sheen_l(mu, a)) : exp(2.0 * sheen_l(0.5, a) - sheen_l(1.0 - mu, a));
}
static double sheen_brdf(double r, dv3 wo, dv3 wi) {
    if (!(wo.z > 0.0) || !(wi.z > 0.0))
        return 0.0;
    const dv3 h = dv3_norm((dv3){wo.x + wi.x, wo.y + wi.y, wo.z + wi.z});
    const double a = r * r, inv = 1.0 / a;
    const double d = (2.0 + inv) * pow(fmax(1.0 - h.z * h.z, 0.0), inv * 0.5) / (2.0 * REF_PI);
    const double l = 1.0 + sheen_lambda(wo.z, a) + sheen_lambda(wi.z, a);
    const double v = fmin(fmax(1.0 / (l * 4.0 * wo.z * wi.z), 0.0), 1.0);
    return d * v;
}

/* The sheen albedo at (mu, r): uniform hemisphere samples, 2 pi f cos. */
static double integrate_sheen(double mu, double r, u32 samples) {
    const dv3 wo = {sqrt(1.0 - mu * mu), 0.0, mu};
    double e = 0.0;
    for (u32 k = 0; k < samples; ++k) {
        const double z = ((double)k + 0.5) / (double)samples,
                     phi = 2.0 * REF_PI * radical_inverse(k);
        const double st = sqrt(fmax(0.0, 1.0 - z * z));
        const dv3 wi = {st * cos(phi), st * sin(phi), z};
        e += sheen_brdf(r, wo, wi) * wi.z;
    }
    return e * 2.0 * REF_PI / (double)samples;
}

void de__brdf_lut_reference(u32 samples, float *out) {
    const u32 n = DE__BRDF_LUT_SIZE;
    for (u32 j = 0; j < DE__BRDF_LUT_ROWS; ++j) {
        const bool sheen = j >= n;
        const double t = (double)(sheen ? j - n : j) / (double)(n - 1u);
        const double r = sheen ? DE__SHEEN_ROUGHNESS_MIN + (1.0 - DE__SHEEN_ROUGHNESS_MIN) * t : t;
        const double a = fmax(r * r, DE__GGX_ALPHA_MIN);
        double row_a[DE__BRDF_LUT_SIZE], row_b[DE__BRDF_LUT_SIZE];
        double avg_a = 0.0, avg_b = 0.0;
        for (u32 i = 0; i < n; ++i) {
            const double mu = ((double)i + 0.5) / (double)n;
            if (sheen) {
                row_a[i] = integrate_sheen(mu, r, samples);
                row_b[i] = 0.0;
            } else {
                integrate_ab(mu, a, samples, &row_a[i], &row_b[i]);
            }
            avg_a += row_a[i] * mu;
            avg_b += row_b[i] * mu;
        }
        avg_a *= 2.0 / (double)n;
        avg_b *= 2.0 / (double)n;
        for (u32 i = 0; i < n; ++i) {
            float *o = out + ((size_t)j * n + i) * 4u;
            o[0] = (float)row_a[i];
            o[1] = sheen ? (float)avg_a : (float)row_b[i];
            o[2] = sheen ? 0.0f : (float)avg_a;
            o[3] = sheen ? 0.0f : (float)avg_b;
        }
    }
}
