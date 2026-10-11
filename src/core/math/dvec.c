/* dvec.c: de_dvec3 operations for large worlds (SPEC §5.5, DENG-244).
 *
 * Only + - * / and sqrt (correctly rounded IEEE operations), and this file is compiled with the
 * deterministic flags (src/core/CMakeLists.txt: no FP contraction), so simulation code may call
 * it. It must not include math_internal.h (the fast math). */
#include <dengine/de_math.h>

#include <math.h> /* sqrt only */

static de_dvec3 de__dv3(f64 x, f64 y, f64 z) {
    de_dvec3 r = {x, y, z};
    return r;
}

de_dvec3 de_dv3_add(de_dvec3 a, de_dvec3 b) {
    return de__dv3(a.x + b.x, a.y + b.y, a.z + b.z);
}

de_dvec3 de_dv3_sub(de_dvec3 a, de_dvec3 b) {
    return de__dv3(a.x - b.x, a.y - b.y, a.z - b.z);
}

de_dvec3 de_dv3_scale(de_dvec3 a, f64 s) {
    return de__dv3(a.x * s, a.y * s, a.z * s);
}

f64 de_dv3_dot(de_dvec3 a, de_dvec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

f64 de_dv3_length(de_dvec3 a) {
    return sqrt(de_dv3_dot(a, a));
}

f64 de_dv3_distance(de_dvec3 a, de_dvec3 b) {
    return de_dv3_length(de_dv3_sub(b, a));
}

de_dvec3 de_dv3_lerp(de_dvec3 a, de_dvec3 b, f64 t) {
    /* The two-weight form is exact at both ends; a + (b - a) * t can miss b at t = 1. */
    f64 u = 1.0 - t;
    return de__dv3(a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t);
}

de_dvec3 de_dv3_from_v3(de_vec3 v) {
    return de__dv3((f64)v.x, (f64)v.y, (f64)v.z);
}

de_vec3 de_dv3_to_v3(de_dvec3 v) {
    de_vec3 r = {(f32)v.x, (f32)v.y, (f32)v.z};
    return r;
}

bool de_dv3_eq_eps(de_dvec3 a, de_dvec3 b, f64 eps) {
    /* Written as "<= eps" so a NaN component compares unequal. */
    f64 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return (dx <= eps && -dx <= eps) && (dy <= eps && -dy <= eps) && (dz <= eps && -dz <= eps);
}

de_vec3 de_dv3_relative(de_dvec3 p, de_dvec3 origin) {
    /* Subtract in f64 first: the difference is small and exact enough, and only it is rounded to
     * f32. Converting p and origin to f32 first would lose up to ~4 mm each at 100 km. */
    de_vec3 r = {(f32)(p.x - origin.x), (f32)(p.y - origin.y), (f32)(p.z - origin.z)};
    return r;
}
