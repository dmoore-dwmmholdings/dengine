/* vec.c: de_vec2, de_vec3 and de_vec4 operations, scalar reference (SPEC §5.4, DENG-241). */
#include "core/math/math_internal.h"

/* ---- de_vec2 ---- */

de_vec2 de_v2_add(de_vec2 a, de_vec2 b) {
    de_vec2 r = {a.x + b.x, a.y + b.y};
    return r;
}

de_vec2 de_v2_sub(de_vec2 a, de_vec2 b) {
    de_vec2 r = {a.x - b.x, a.y - b.y};
    return r;
}

de_vec2 de_v2_mul(de_vec2 a, de_vec2 b) {
    de_vec2 r = {a.x * b.x, a.y * b.y};
    return r;
}

de_vec2 de_v2_scale(de_vec2 a, f32 s) {
    de_vec2 r = {a.x * s, a.y * s};
    return r;
}

f32 de_v2_dot(de_vec2 a, de_vec2 b) {
    return a.x * b.x + a.y * b.y;
}

f32 de_v2_length_sq(de_vec2 a) {
    return de_v2_dot(a, a);
}

f32 de_v2_length(de_vec2 a) {
    return sqrtf(de_v2_length_sq(a));
}

de_vec2 de_v2_normalize(de_vec2 a) {
    return de_v2_scale(a, 1.0f / de_v2_length(a));
}

de_vec2 de_v2_normalize_safe(de_vec2 a, de_vec2 fallback) {
    f32 len = de_v2_length(a);
    if (!(len > 0.0f) || !isfinite(len))
        return fallback;
    return de_v2_scale(a, 1.0f / len);
}

de_vec2 de_v2_lerp(de_vec2 a, de_vec2 b, f32 t) {
    de_vec2 r = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    return r;
}

de_vec2 de_v2_min(de_vec2 a, de_vec2 b) {
    de_vec2 r = {fminf(a.x, b.x), fminf(a.y, b.y)};
    return r;
}

de_vec2 de_v2_max(de_vec2 a, de_vec2 b) {
    de_vec2 r = {fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
    return r;
}

de_vec2 de_v2_abs(de_vec2 a) {
    de_vec2 r = {fabsf(a.x), fabsf(a.y)};
    return r;
}

bool de_v2_eq_eps(de_vec2 a, de_vec2 b, f32 eps) {
    return fabsf(a.x - b.x) <= eps && fabsf(a.y - b.y) <= eps;
}

/* ---- de_vec3 ---- */

de_vec3 de_v3_add(de_vec3 a, de_vec3 b) {
    return de__v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

de_vec3 de_v3_sub(de_vec3 a, de_vec3 b) {
    return de__v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

de_vec3 de_v3_mul(de_vec3 a, de_vec3 b) {
    return de__v3(a.x * b.x, a.y * b.y, a.z * b.z);
}

de_vec3 de_v3_scale(de_vec3 a, f32 s) {
    return de__v3(a.x * s, a.y * s, a.z * s);
}

f32 de_v3_dot(de_vec3 a, de_vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

de_vec3 de_v3_cross(de_vec3 a, de_vec3 b) {
    return de__v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

f32 de_v3_length_sq(de_vec3 a) {
    return de_v3_dot(a, a);
}

f32 de_v3_length(de_vec3 a) {
    return sqrtf(de_v3_length_sq(a));
}

de_vec3 de_v3_normalize(de_vec3 a) {
    return de_v3_scale(a, 1.0f / de_v3_length(a));
}

de_vec3 de_v3_normalize_safe(de_vec3 a, de_vec3 fallback) {
    f32 len = de_v3_length(a);
    if (!(len > 0.0f) || !isfinite(len))
        return fallback;
    return de_v3_scale(a, 1.0f / len);
}

de_vec3 de_v3_lerp(de_vec3 a, de_vec3 b, f32 t) {
    return de__v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

de_vec3 de_v3_min(de_vec3 a, de_vec3 b) {
    return de__v3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z));
}

de_vec3 de_v3_max(de_vec3 a, de_vec3 b) {
    return de__v3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z));
}

de_vec3 de_v3_abs(de_vec3 a) {
    return de__v3(fabsf(a.x), fabsf(a.y), fabsf(a.z));
}

bool de_v3_eq_eps(de_vec3 a, de_vec3 b, f32 eps) {
    return fabsf(a.x - b.x) <= eps && fabsf(a.y - b.y) <= eps && fabsf(a.z - b.z) <= eps;
}

/* ---- de_vec4 ---- */

static de_vec4 de__v4(f32 x, f32 y, f32 z, f32 w) {
    de_vec4 r;
    r.x = x;
    r.y = y;
    r.z = z;
    r.w = w;
    return r;
}

de_vec4 de_v4_add(de_vec4 a, de_vec4 b) {
    return de__v4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}

de_vec4 de_v4_sub(de_vec4 a, de_vec4 b) {
    return de__v4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
}

de_vec4 de_v4_mul(de_vec4 a, de_vec4 b) {
    return de__v4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

de_vec4 de_v4_scale(de_vec4 a, f32 s) {
    return de__v4(a.x * s, a.y * s, a.z * s, a.w * s);
}

f32 de_v4_dot(de_vec4 a, de_vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

f32 de_v4_length_sq(de_vec4 a) {
    return de_v4_dot(a, a);
}

f32 de_v4_length(de_vec4 a) {
    return sqrtf(de_v4_length_sq(a));
}

de_vec4 de_v4_normalize(de_vec4 a) {
    return de_v4_scale(a, 1.0f / de_v4_length(a));
}

de_vec4 de_v4_normalize_safe(de_vec4 a, de_vec4 fallback) {
    f32 len = de_v4_length(a);
    if (!(len > 0.0f) || !isfinite(len))
        return fallback;
    return de_v4_scale(a, 1.0f / len);
}

de_vec4 de_v4_lerp(de_vec4 a, de_vec4 b, f32 t) {
    return de__v4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

de_vec4 de_v4_min(de_vec4 a, de_vec4 b) {
    return de__v4(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z), fminf(a.w, b.w));
}

de_vec4 de_v4_max(de_vec4 a, de_vec4 b) {
    return de__v4(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z), fmaxf(a.w, b.w));
}

de_vec4 de_v4_abs(de_vec4 a) {
    return de__v4(fabsf(a.x), fabsf(a.y), fabsf(a.z), fabsf(a.w));
}

bool de_v4_eq_eps(de_vec4 a, de_vec4 b, f32 eps) {
    return fabsf(a.x - b.x) <= eps && fabsf(a.y - b.y) <= eps && fabsf(a.z - b.z) <= eps &&
           fabsf(a.w - b.w) <= eps;
}
