/*
 * brdf_ref.c: the C reference of shaders/brdf/ (brdf_ref.h; DENG-298). Test-only, scalar float.
 */
#include "brdf_ref.h"

#include <math.h>

#define BRDF_PI     3.14159265358979323846f
#define BRDF_INV_PI 0.31830988618379067154f

static float saturate(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}
static float pow5(float x) {
    const float x2 = x * x;
    return x2 * x2 * x;
}

brdf_v3 brdf_v3_make(float x, float y, float z) {
    brdf_v3 v = {x, y, z};
    return v;
}
float brdf_dot(brdf_v3 a, brdf_v3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
brdf_v3 brdf_normalize(brdf_v3 v) {
    const float s = 1.0f / sqrtf(brdf_dot(v, v));
    return brdf_v3_make(v.x * s, v.y * s, v.z * s);
}

/* ---------- ggx.slang ---------- */

float brdf_ggx_d(float alpha, float cos_h) {
    if (!(cos_h > 0.0f))
        return 0.0f;
    const float a2 = alpha * alpha;
    const float d = (1.0f - cos_h) * (1.0f + cos_h) + a2 * cos_h * cos_h;
    return a2 / (BRDF_PI * d * d);
}

float brdf_ggx_d_aniso(float ax, float ay, brdf_v3 h) {
    if (!(h.z > 0.0f))
        return 0.0f;
    const brdf_v3 s = brdf_v3_make(h.x / ax, h.y / ay, h.z);
    const float l2 = brdf_dot(s, s);
    return 1.0f / (BRDF_PI * ax * ay * l2 * l2);
}

float brdf_ggx_lambda(float ax, float ay, brdf_v3 w) {
    const float z2 = w.z * w.z;
    const float t = ax * ax * w.x * w.x + ay * ay * w.y * w.y;
    return (sqrtf(1.0f + t / z2) - 1.0f) * 0.5f;
}

float brdf_ggx_g1(float ax, float ay, brdf_v3 w) {
    if (w.z == 0.0f)
        return 0.0f;
    return 1.0f / (1.0f + brdf_ggx_lambda(ax, ay, w));
}

float brdf_ggx_g2(float ax, float ay, brdf_v3 wo, brdf_v3 wi) {
    if (wo.z == 0.0f || wi.z == 0.0f)
        return 0.0f;
    return 1.0f / (1.0f + brdf_ggx_lambda(ax, ay, wo) + brdf_ggx_lambda(ax, ay, wi));
}

brdf_v3 brdf_ggx_vndf_sample(float ax, float ay, brdf_v3 wo, float u0, float u1) {
    const brdf_v3 v = brdf_normalize(brdf_v3_make(wo.x * ax, wo.y * ay, wo.z));
    const float phi = 2.0f * BRDF_PI * u0;
    const float z = (1.0f - u1) * (1.0f + v.z) - v.z;
    const float sin_theta = sqrtf(saturate(1.0f - z * z));
    const brdf_v3 c = brdf_v3_make(sin_theta * cosf(phi), sin_theta * sinf(phi), z);
    const brdf_v3 h = brdf_v3_make(c.x + v.x, c.y + v.y, c.z + v.z);
    return brdf_normalize(brdf_v3_make(h.x * ax, h.y * ay, h.z > 0.0f ? h.z : 0.0f));
}

float brdf_ggx_vndf_pdf(float ax, float ay, brdf_v3 wo, brdf_v3 h) {
    if (!(wo.z > 0.0f))
        return 0.0f;
    const float wo_h = brdf_dot(wo, h);
    return brdf_ggx_g1(ax, ay, wo) * (wo_h > 0.0f ? wo_h : 0.0f) * brdf_ggx_d_aniso(ax, ay, h) /
           wo.z;
}

float brdf_ggx_reflect_pdf(float ax, float ay, brdf_v3 wo, brdf_v3 wi) {
    brdf_v3 h = brdf_v3_make(wo.x + wi.x, wo.y + wi.y, wo.z + wi.z);
    const float l2 = brdf_dot(h, h);
    if (!(wo.z > 0.0f) || !(l2 > 0.0f))
        return 0.0f;
    const float s = 1.0f / sqrtf(l2);
    h = brdf_v3_make(h.x * s, h.y * s, h.z * s);
    const float wo_h = brdf_dot(wo, h);
    if (!(wo_h > 0.0f))
        return 0.0f;
    return brdf_ggx_vndf_pdf(ax, ay, wo, h) / (4.0f * wo_h);
}

brdf_v3 brdf_reflect(brdf_v3 wo, brdf_v3 h) {
    const float k = 2.0f * brdf_dot(wo, h);
    return brdf_v3_make(k * h.x - wo.x, k * h.y - wo.y, k * h.z - wo.z);
}

float brdf_ggx_specular(float ax, float ay, brdf_v3 wo, brdf_v3 wi) {
    if (!(wo.z > 0.0f) || !(wi.z > 0.0f))
        return 0.0f;
    const brdf_v3 h = brdf_normalize(brdf_v3_make(wo.x + wi.x, wo.y + wi.y, wo.z + wi.z));
    return brdf_ggx_d_aniso(ax, ay, h) * brdf_ggx_g2(ax, ay, wo, wi) / (4.0f * wo.z * wi.z);
}

/* ---------- fresnel.slang ---------- */

float brdf_fresnel_schlick(float f0, float f90, float cos_theta) {
    return f0 + (f90 - f0) * pow5(1.0f - saturate(cos_theta));
}

float brdf_f0_from_ior(float ior, float outside) {
    const float r = (ior - outside) / (ior + outside);
    return r * r;
}

float brdf_fresnel_dielectric(float cos_i, float eta) {
    cos_i = saturate(cos_i);
    const float sin2_t = (1.0f - cos_i * cos_i) / (eta * eta);
    if (sin2_t >= 1.0f)
        return 1.0f;
    const float cos_t = sqrtf(1.0f - sin2_t);
    const float rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
    const float rp = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
    return 0.5f * (rs * rs + rp * rp);
}

float brdf_fresnel_f82_tint(float f0, float tint, float cos_theta) {
    const float mu_bar = 1.0f / 7.0f;
    const float denom = 0.056652780f; /* (1/7)(6/7)^6 */
    const float schlick_bar = brdf_fresnel_schlick(f0, 1.0f, mu_bar);
    const float a = schlick_bar * (1.0f - tint) / denom;
    const float mu = saturate(cos_theta);
    const float m = 1.0f - mu;
    float m6 = m * m * m;
    m6 *= m6;
    return saturate(brdf_fresnel_schlick(f0, 1.0f, mu) - a * (mu * m6));
}

/* ---------- diffuse.slang ---------- */

float brdf_diffuse_lambert(void) {
    return BRDF_INV_PI;
}

float brdf_diffuse_burley(float roughness, float cos_o, float cos_i, float cos_ih) {
    const float f90 = 0.5f + 2.0f * roughness * cos_ih * cos_ih;
    const float fo = 1.0f + (f90 - 1.0f) * pow5(1.0f - saturate(cos_o));
    const float fi = 1.0f + (f90 - 1.0f) * pow5(1.0f - saturate(cos_i));
    return BRDF_INV_PI * fo * fi;
}

/* ---------- sampling.slang ---------- */

brdf_v3 brdf_sample_cosine_hemisphere(float u0, float u1) {
    const float ox = u0 * 2.0f - 1.0f, oy = u1 * 2.0f - 1.0f;
    float dx = 0.0f, dy = 0.0f;
    if (ox != 0.0f || oy != 0.0f) {
        float r, theta;
        if (fabsf(ox) > fabsf(oy)) {
            r = ox;
            theta = (BRDF_PI / 4.0f) * (oy / ox);
        } else {
            r = oy;
            theta = (BRDF_PI / 2.0f) - (BRDF_PI / 4.0f) * (ox / oy);
        }
        dx = r * cosf(theta);
        dy = r * sinf(theta);
    }
    const float z2 = 1.0f - (dx * dx + dy * dy);
    return brdf_v3_make(dx, dy, sqrtf(z2 > 0.0f ? z2 : 0.0f));
}

float brdf_cosine_hemisphere_pdf(float cos_theta) {
    return (cos_theta > 0.0f ? cos_theta : 0.0f) * BRDF_INV_PI;
}

brdf_v3 brdf_sample_uniform_sphere(float u0, float u1) {
    const float z = 1.0f - 2.0f * u0;
    const float r2 = 1.0f - z * z;
    const float r = sqrtf(r2 > 0.0f ? r2 : 0.0f);
    const float phi = 2.0f * BRDF_PI * u1;
    return brdf_v3_make(r * cosf(phi), r * sinf(phi), z);
}

float brdf_radical_inverse2(u32 i) {
    i = (i << 16) | (i >> 16);
    i = ((i & 0x00FF00FFu) << 8) | ((i & 0xFF00FF00u) >> 8);
    i = ((i & 0x0F0F0F0Fu) << 4) | ((i & 0xF0F0F0F0u) >> 4);
    i = ((i & 0x33333333u) << 2) | ((i & 0xCCCCCCCCu) >> 2);
    i = ((i & 0x55555555u) << 1) | ((i & 0xAAAAAAAAu) >> 1);
    return (float)i * 2.3283064365386963e-10f;
}

/* ---------- The S13 Standard model (resolve_model.slang ResolveStandardModel) ---------- */

static float standard_alpha(float roughness) {
    const float a = roughness * roughness;
    return a > 1e-3f ? a : 1e-3f;
}

float brdf_standard_evaluate(float base, float roughness, float metallic, float specular,
                             brdf_v3 wo, brdf_v3 wi) {
    if (!(wo.z > 0.0f) || !(wi.z > 0.0f))
        return 0.0f;
    const float a = standard_alpha(roughness);
    const brdf_v3 h = brdf_normalize(brdf_v3_make(wo.x + wi.x, wo.y + wi.y, wo.z + wi.z));
    const float d = 0.04f * specular;
    const float f0 = d + (base - d) * metallic;
    const float f = brdf_fresnel_schlick(f0, 1.0f, brdf_dot(wo, h));
    const float spec = f * brdf_ggx_specular(a, a, wo, wi);
    const float diff = (1.0f - f) * ((1.0f - metallic) * BRDF_INV_PI) * base;
    return (spec + diff) * wi.z;
}

float brdf_standard_pdf(float roughness, float metallic, brdf_v3 wo, brdf_v3 wi) {
    if (!(wo.z > 0.0f) || !(wi.z > 0.0f))
        return 0.0f;
    const float a = standard_alpha(roughness);
    const float ps = 0.5f + 0.5f * metallic;
    return ps * brdf_ggx_reflect_pdf(a, a, wo, wi) + (1.0f - ps) * brdf_cosine_hemisphere_pdf(wi.z);
}
