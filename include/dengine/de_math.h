/* de_math.h — math types, fast math (de_*) and deterministic math (de_d*).
 * Z-up, right-handed, meters. Column-major storage, column vectors. SPEC.md §5.4, §5.5, §5.7. */
#ifndef DE_MATH_H
#define DE_MATH_H
#include "de_base.h"
DE_EXTERN_C_BEGIN

typedef struct de_vec2 {
    f32 x, y;
} de_vec2;
typedef struct de_vec3 {
    f32 x, y, z;
} de_vec3;
typedef struct de_vec4 {
    DE_ALIGN(16) f32 x, y, z, w;
} de_vec4;
typedef struct de_ivec2 {
    i32 x, y;
} de_ivec2;
typedef struct de_dvec3 {
    f64 x, y, z;
} de_dvec3; /* world positions */
typedef struct de_quat {
    DE_ALIGN(16) f32 x, y, z, w;
} de_quat;
typedef struct de_mat3 {
    f32 m[9];
} de_mat3; /* column-major */
typedef struct de_mat4 {
    DE_ALIGN(16) f32 m[16];
} de_mat4; /* column-major */
typedef struct de_affine {
    DE_ALIGN(16) f32 m[12];
} de_affine; /* 3x4: rotation/scale + translation */
typedef struct de_aabb {
    de_vec3 min, max;
} de_aabb;
typedef struct de_plane {
    de_vec3 n;
    f32 d;
} de_plane;
typedef struct de_ray {
    de_vec3 origin, dir;
} de_ray;
typedef struct de_frustum {
    de_plane planes[6];
} de_frustum;

/* World transform: double position, float rotation/scale. */
typedef struct de_transform {
    de_dvec3 position;
    de_quat rotation;
    de_vec3 scale;
} de_transform;

#define DE_PI           3.14159265358979323846f
#define DE_TAU          6.28318530717958647692f
#define DE_AXIS_RIGHT   ((de_vec3){1, 0, 0})
#define DE_AXIS_FORWARD ((de_vec3){0, 1, 0})
#define DE_AXIS_UP      ((de_vec3){0, 0, 1})

/* ---------- Fast math (rendering, tools). May use approximations. ---------- */
DE_API de_vec3 de_v3_add(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_sub(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_scale(de_vec3 a, f32 s);
DE_API f32 de_v3_dot(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_cross(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_normalize(de_vec3 a);
DE_API f32 de_v3_length(de_vec3 a);

DE_API de_quat de_quat_identity(void);
DE_API de_quat de_quat_axis_angle(de_vec3 axis, f32 radians);
DE_API de_quat de_quat_mul(de_quat a, de_quat b);
DE_API de_vec3 de_quat_rotate(de_quat q, de_vec3 v);
DE_API de_quat de_quat_slerp(de_quat a, de_quat b, f32 t);

DE_API de_mat4 de_m4_identity(void);
DE_API de_mat4 de_m4_mul(const de_mat4 *a, const de_mat4 *b);
DE_API de_mat4 de_m4_inverse(const de_mat4 *m);
DE_API de_vec4 de_m4_mul_v4(const de_mat4 *m, de_vec4 v);
DE_API de_mat4 de_m4_trs(de_vec3 t, de_quat r, de_vec3 s);
DE_API de_mat4 de_m4_look_at(de_vec3 eye, de_vec3 target, de_vec3 up);
/* Reverse-Z, infinite far plane. Per-API clip-space conventions are applied by the RHI. */
DE_API de_mat4 de_m4_perspective_rz(f32 fov_y, f32 aspect, f32 near_z);
DE_API de_mat4 de_m4_ortho_rz(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z);

/* Camera-relative conversion for large worlds (double → float). */
DE_API de_vec3 de_dv3_relative(de_dvec3 p, de_dvec3 origin);
DE_API de_mat4 de_transform_to_m4_relative(const de_transform *t, de_dvec3 origin);

DE_API de_frustum de_frustum_from_m4(const de_mat4 *view_proj);
DE_API bool de_frustum_test_aabb(const de_frustum *f, de_aabb box);

/* ---------- Deterministic math (simulation). Strict IEEE, own transcendentals,
 * no approximate reciprocals, compiled with -ffp-contract=off. ---------- */
DE_API f32 de_dsin(f32 x);
DE_API f32 de_dcos(f32 x);
DE_API void de_dsincos(f32 x, f32 *s, f32 *c);
DE_API f32 de_datan2(f32 y, f32 x);
DE_API f32 de_dacos(f32 x);
DE_API f32 de_dexp(f32 x);
DE_API f32 de_dlog(f32 x);
DE_API f32 de_dpow(f32 x, f32 y);
DE_API f32 de_dsqrt(f32 x); /* IEEE sqrt is exact; wrapper documents intent */
DE_API f64 de_dsin64(f64 x);
DE_API f64 de_dcos64(f64 x);
DE_API de_vec3 de_dv3_normalize(de_vec3 a);
DE_API de_quat de_dquat_normalize(de_quat q);
DE_API de_quat de_dquat_integrate(de_quat q, de_vec3 angular_velocity, f32 dt);

/* Deterministic RNG stream (PCG32). */
typedef struct de_rng {
    u64 state, inc;
} de_rng;
DE_API de_rng de_rng_seed(u64 seed, u64 stream);
DE_API u32 de_rng_u32(de_rng *r);
DE_API f32 de_rng_f32(de_rng *r); /* [0, 1) */
DE_API u32 de_rng_range(de_rng *r, u32 lo, u32 hi_exclusive);

/* Applied once per thread at startup (FTZ/DAZ on x86 MXCSR, FZ on arm64 FPCR). */
DE_API void de_fp_env_init_thread(void);

DE_EXTERN_C_END
#endif
