/* de_math.h — math types, fast math (de_*) and deterministic math (de_d*).
 * Z-up, right-handed, meters. Column-major storage, column vectors. SPEC.md §5.4, §5.5, §5.7.
 *
 * Sections (each later S03 task appends only inside its own section):
 *   1. Types and constants                                   DENG-241
 *   2. Fast math, scalar reference (vectors ... frustum)     DENG-241
 *   3. Fast math, batch / SIMD APIs                          DENG-242
 *   4. Deterministic math (de_d*), RNG, FP environment       DENG-243
 *   5. Large-world transforms (de_dvec3, de_transform)       DENG-244
 *
 * Conventions (SPEC §5.4, §5.5):
 *
 *   World space: right-handed, Z up.          View space (de_m4_look_at): right-handed,
 *                                             camera looks down -Z.
 *         +Z up                                     +Y up
 *          |   +Y forward                            |
 *          |  /                                      |
 *          | /                                       |
 *          o------ +X right                          o------ +X right
 *                                                   /
 *                                                  /  +Z (behind the camera; it looks at -Z)
 *
 *   - Units: meters, radians.
 *   - Vectors are column vectors: v' = M * v. M = A * B applies B first, then A.
 *   - Matrices are stored column-major (the layouts are drawn at de_mat3, de_mat4, de_affine).
 *   - Quaternions are Hamilton quaternions (x, y, z, w) with w the scalar part. de_quat_mul(a, b)
 *     applies b first, then a, like matrices.
 *   - A positive angle rotates counter-clockwise when you look down the axis toward the origin
 *     (right-hand rule): rotating +X by +90 degrees about +Z gives +Y.
 *   - Projections are reverse-Z with clip z in [0, 1]: depth 1 at the near plane, 0 at the far
 *     plane (or at infinity). Per-API clip-space differences (Vulkan's flipped Y, ...) are applied
 *     by the RHI, never here.
 *
 * Fast math (sections 2 and 3) may use <math.h> and approximations. Never call it from a
 * deterministic module (SPEC §5.7 rule 2): use the de_d* functions of section 4.
 */
#ifndef DE_MATH_H
#define DE_MATH_H
#include "de_base.h"
DE_EXTERN_C_BEGIN

/* ================================ 1. Types and constants (DENG-241) ======================== */

/* MSVC C4324 ("structure was padded due to alignment specifier") fires for every DE_ALIGN(16)
 * struct below, which /W4 /WX turns into an error. The alignment is the point (SIMD loads). */
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
typedef struct de_vec2 {
    f32 x, y;
} de_vec2;
typedef struct de_vec3 {
    f32 x, y, z;
} de_vec3;
/* The alignment goes on x alone: on "f32 x, y, z, w" it would align every member to 16 bytes
 * and make the struct 64 bytes. */
typedef struct de_vec4 {
    DE_ALIGN(16) f32 x;
    f32 y, z, w;
} de_vec4;
typedef struct de_ivec2 {
    i32 x, y;
} de_ivec2;
typedef struct de_dvec3 {
    f64 x, y, z;
} de_dvec3; /* world positions */
/** Hamilton quaternion; w is the scalar part. Rotations use unit quaternions. */
typedef struct de_quat {
    DE_ALIGN(16) f32 x;
    f32 y, z, w;
} de_quat;
/**
 * 3x3 matrix, column-major: element (row r, column c) is m[c * 3 + r].
 *
 *     | m[0] m[3] m[6] |
 *     | m[1] m[4] m[7] |
 *     | m[2] m[5] m[8] |
 */
typedef struct de_mat3 {
    f32 m[9];
} de_mat3;
/**
 * 4x4 matrix, column-major: element (row r, column c) is m[c * 4 + r]. For an affine transform,
 * column 3 (m[12], m[13], m[14]) is the translation and row 3 is (0, 0, 0, 1).
 *
 *     | m[0] m[4] m[8]  m[12] |
 *     | m[1] m[5] m[9]  m[13] |
 *     | m[2] m[6] m[10] m[14] |
 *     | m[3] m[7] m[11] m[15] |
 */
typedef struct de_mat4 {
    DE_ALIGN(16) f32 m[16];
} de_mat4;
/**
 * Affine transform [L | t]: a 3x4 matrix, the top three rows of a 4x4 whose bottom row is
 * (0, 0, 0, 1). Column-major like de_mat4: element (row r, column c) is m[c * 3 + r]. Columns
 * 0..2 are the linear part (rotation and scale), column 3 is the translation.
 *
 *     | m[0] m[3] m[6] m[9]  |
 *     | m[1] m[4] m[7] m[10] |
 *     | m[2] m[5] m[8] m[11] |
 */
typedef struct de_affine {
    DE_ALIGN(16) f32 m[12];
} de_affine;
/** Axis-aligned box. Empty when min > max on any axis (see de_aabb_empty). */
typedef struct de_aabb {
    de_vec3 min, max;
} de_aabb;
/**
 * Plane: the points p with dot(n, p) + d = 0. The signed distance of p is dot(n, p) + d when n
 * is unit length; the positive side (the side n points to) is "in front". For a frustum plane
 * the inside is the positive side.
 */
typedef struct de_plane {
    de_vec3 n;
    f32 d;
} de_plane;
/** Ray origin + t * dir, t >= 0. dir does not have to be unit length; t is in units of dir. */
typedef struct de_ray {
    de_vec3 origin, dir;
} de_ray;
/** Six planes with normals pointing inward, indexed by de_frustum_plane. */
typedef struct de_frustum {
    de_plane planes[6];
} de_frustum;

/* World transform: double position, float rotation/scale. */
typedef struct de_transform {
    de_dvec3 position;
    de_quat rotation;
    de_vec3 scale;
} de_transform;
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/** Index of each plane in de_frustum.planes. */
typedef enum de_frustum_plane {
    DE_FRUSTUM_LEFT = 0,
    DE_FRUSTUM_RIGHT = 1,
    DE_FRUSTUM_BOTTOM = 2,
    DE_FRUSTUM_TOP = 3,
    DE_FRUSTUM_NEAR = 4,
    DE_FRUSTUM_FAR = 5,
} de_frustum_plane;

#define DE_PI           3.14159265358979323846f
#define DE_TAU          6.28318530717958647692f
#define DE_AXIS_RIGHT   ((de_vec3){1, 0, 0})
#define DE_AXIS_FORWARD ((de_vec3){0, 1, 0})
#define DE_AXIS_UP      ((de_vec3){0, 0, 1})

/* ================ 2. Fast math, scalar reference (DENG-241). May use <math.h>. ============== */

/* ---- de_vec2 ---- */
DE_API de_vec2 de_v2_add(de_vec2 a, de_vec2 b);
DE_API de_vec2 de_v2_sub(de_vec2 a, de_vec2 b);
DE_API de_vec2 de_v2_mul(de_vec2 a, de_vec2 b); /**< component-wise */
DE_API de_vec2 de_v2_scale(de_vec2 a, f32 s);
DE_API f32 de_v2_dot(de_vec2 a, de_vec2 b);
DE_API f32 de_v2_length(de_vec2 a);
DE_API f32 de_v2_length_sq(de_vec2 a);
/** a / length(a). A zero vector gives a non-finite result: use de_v2_normalize_safe. */
DE_API de_vec2 de_v2_normalize(de_vec2 a);
/** a / length(a), or `fallback` when length(a) is zero or not finite. */
DE_API de_vec2 de_v2_normalize_safe(de_vec2 a, de_vec2 fallback);
/** a + (b - a) * t. t is not clamped. */
DE_API de_vec2 de_v2_lerp(de_vec2 a, de_vec2 b, f32 t);
DE_API de_vec2 de_v2_min(de_vec2 a, de_vec2 b);
DE_API de_vec2 de_v2_max(de_vec2 a, de_vec2 b);
DE_API de_vec2 de_v2_abs(de_vec2 a);
/** true when every component differs by at most eps. */
DE_API bool de_v2_eq_eps(de_vec2 a, de_vec2 b, f32 eps);

/* ---- de_vec3 ---- */
DE_API de_vec3 de_v3_add(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_sub(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_mul(de_vec3 a, de_vec3 b); /**< component-wise */
DE_API de_vec3 de_v3_scale(de_vec3 a, f32 s);
DE_API f32 de_v3_dot(de_vec3 a, de_vec3 b);
/** Right-handed: cross(DE_AXIS_RIGHT, DE_AXIS_FORWARD) = DE_AXIS_UP. */
DE_API de_vec3 de_v3_cross(de_vec3 a, de_vec3 b);
DE_API f32 de_v3_length(de_vec3 a);
DE_API f32 de_v3_length_sq(de_vec3 a);
/** a / length(a). A zero vector gives a non-finite result: use de_v3_normalize_safe. */
DE_API de_vec3 de_v3_normalize(de_vec3 a);
/** a / length(a), or `fallback` when length(a) is zero or not finite. */
DE_API de_vec3 de_v3_normalize_safe(de_vec3 a, de_vec3 fallback);
/** a + (b - a) * t. t is not clamped. */
DE_API de_vec3 de_v3_lerp(de_vec3 a, de_vec3 b, f32 t);
DE_API de_vec3 de_v3_min(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_max(de_vec3 a, de_vec3 b);
DE_API de_vec3 de_v3_abs(de_vec3 a);
/** true when every component differs by at most eps. */
DE_API bool de_v3_eq_eps(de_vec3 a, de_vec3 b, f32 eps);

/* ---- de_vec4 ---- */
DE_API de_vec4 de_v4_add(de_vec4 a, de_vec4 b);
DE_API de_vec4 de_v4_sub(de_vec4 a, de_vec4 b);
DE_API de_vec4 de_v4_mul(de_vec4 a, de_vec4 b); /**< component-wise */
DE_API de_vec4 de_v4_scale(de_vec4 a, f32 s);
DE_API f32 de_v4_dot(de_vec4 a, de_vec4 b);
DE_API f32 de_v4_length(de_vec4 a);
DE_API f32 de_v4_length_sq(de_vec4 a);
/** a / length(a). A zero vector gives a non-finite result: use de_v4_normalize_safe. */
DE_API de_vec4 de_v4_normalize(de_vec4 a);
/** a / length(a), or `fallback` when length(a) is zero or not finite. */
DE_API de_vec4 de_v4_normalize_safe(de_vec4 a, de_vec4 fallback);
/** a + (b - a) * t. t is not clamped. */
DE_API de_vec4 de_v4_lerp(de_vec4 a, de_vec4 b, f32 t);
DE_API de_vec4 de_v4_min(de_vec4 a, de_vec4 b);
DE_API de_vec4 de_v4_max(de_vec4 a, de_vec4 b);
DE_API de_vec4 de_v4_abs(de_vec4 a);
/** true when every component differs by at most eps. */
DE_API bool de_v4_eq_eps(de_vec4 a, de_vec4 b, f32 eps);

/* ---- de_quat ---- */
DE_API de_quat de_quat_identity(void);
/**
 * Rotation by `radians` about `axis` (right-hand rule). `axis` must be unit length.
 * de_quat_rotate(de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2), DE_AXIS_RIGHT) = DE_AXIS_FORWARD.
 */
DE_API de_quat de_quat_axis_angle(de_vec3 axis, f32 radians);
/**
 * Rotation from yaw, pitch and roll, in this engine's Z-up, Y-forward frame:
 *   yaw   about +Z (up):      positive turns +Y (forward) toward -X (left),
 *   pitch about +X (right):   positive turns +Y (forward) toward +Z (up),
 *   roll  about +Y (forward): positive turns +Z (up) toward +X (right).
 * The result is q = yaw(Z) * pitch(X) * roll(Y): intrinsic yaw, then pitch about the yawed X
 * axis, then roll about the new forward axis. This is the classic "ZYX" yaw-pitch-roll order
 * mapped onto Z-up, Y-forward axes, so by axis letter it reads Z, X, Y.
 */
DE_API de_quat de_quat_from_euler(f32 yaw, f32 pitch, f32 roll);
/**
 * Inverse of de_quat_from_euler. Outputs pitch in [-pi/2, pi/2], yaw and roll in [-pi, pi]. At
 * pitch = +-pi/2 (gimbal lock) the roll is set to 0 and the whole turn goes into yaw. Any output
 * pointer may be NULL.
 */
DE_API void de_quat_to_euler(de_quat q, f32 *yaw, f32 *pitch, f32 *roll);
/** a * b (Hamilton product): rotates by b first, then by a. */
DE_API de_quat de_quat_mul(de_quat a, de_quat b);
/** Conjugate (-x, -y, -z, w): the inverse of a unit quaternion. */
DE_API de_quat de_quat_conj(de_quat q);
/** conj(q) / |q|^2: the inverse of any non-zero quaternion. */
DE_API de_quat de_quat_inverse(de_quat q);
DE_API f32 de_quat_dot(de_quat a, de_quat b);
/** q / |q|. A zero quaternion gives the identity. */
DE_API de_quat de_quat_normalize(de_quat q);
/** v rotated by the unit quaternion q (q * v * conj(q)). */
DE_API de_vec3 de_quat_rotate(de_quat q, de_vec3 v);
/**
 * Spherical interpolation between unit quaternions, along the shortest path (b is negated when
 * dot(a, b) < 0). t = 0 returns a exactly; t = 1 returns b (or -b, the same rotation) exactly.
 * Falls back to de_quat_nlerp when a and b are nearly parallel.
 */
DE_API de_quat de_quat_slerp(de_quat a, de_quat b, f32 t);
/** Normalized linear interpolation along the shortest path. Cheaper than slerp, not constant
 *  speed. */
DE_API de_quat de_quat_nlerp(de_quat a, de_quat b, f32 t);
/** Rotation matrix of a unit quaternion. */
DE_API de_mat3 de_quat_to_m3(de_quat q);
/** Unit quaternion of a pure rotation matrix (orthonormal, determinant +1). Shepperd's method. */
DE_API de_quat de_quat_from_m3(const de_mat3 *m);

/* ---- de_mat3 ---- */
DE_API de_mat3 de_m3_identity(void);
/** a * b: applies b first, then a. */
DE_API de_mat3 de_m3_mul(const de_mat3 *a, const de_mat3 *b);
DE_API de_vec3 de_m3_mul_v3(const de_mat3 *m, de_vec3 v);
DE_API de_mat3 de_m3_transpose(const de_mat3 *m);
DE_API f32 de_m3_determinant(const de_mat3 *m);
/** Inverse. A singular matrix (determinant exactly 0) gives the zero matrix. */
DE_API de_mat3 de_m3_inverse(const de_mat3 *m);
/** Same as de_quat_to_m3. */
DE_API de_mat3 de_m3_from_quat(de_quat q);

/* ---- de_mat4 ---- */
DE_API de_mat4 de_m4_identity(void);
/** a * b: applies b first, then a. */
DE_API de_mat4 de_m4_mul(const de_mat4 *a, const de_mat4 *b);
/** General inverse (cofactors). A singular matrix (determinant exactly 0) gives the zero matrix.
 *  Use de_m4_inverse_affine when the bottom row is (0, 0, 0, 1). */
DE_API de_mat4 de_m4_inverse(const de_mat4 *m);
/** Inverse of an affine matrix (bottom row (0, 0, 0, 1); the row is not read). Cheaper and more
 *  accurate than de_m4_inverse. A singular linear part gives the zero matrix. */
DE_API de_mat4 de_m4_inverse_affine(const de_mat4 *m);
DE_API de_mat4 de_m4_transpose(const de_mat4 *m);
/** m * v. */
DE_API de_vec4 de_m4_mul_v4(const de_mat4 *m, de_vec4 v);
/** m * (p, 1), divided by the resulting w (a projective transform; for an affine m, w is 1). */
DE_API de_vec3 de_m4_mul_point(const de_mat4 *m, de_vec3 p);
/** The upper 3x3 of m times d: a direction, no translation. */
DE_API de_vec3 de_m4_mul_dir(const de_mat4 *m, de_vec3 d);
/** T * R * S: scales by s, then rotates by r, then translates by t. */
DE_API de_mat4 de_m4_trs(de_vec3 t, de_quat r, de_vec3 s);
/**
 * Splits an affine matrix with no shear into translation, rotation and scale (de_m4_trs inverse).
 * A negative determinant (a mirror) is reported as a negative x scale. Returns false, with
 * *r = identity, when a column of the linear part has zero length. Any output may be NULL.
 */
DE_API bool de_m4_decompose(const de_mat4 *m, de_vec3 *t, de_quat *r, de_vec3 *s);
/**
 * World -> view matrix of a camera at `eye` looking at `target`.
 *
 * View space is right-handed: the camera looks down -Z, +Y is up, +X is right (the GL/Vulkan
 * style view space). The world direction (target - eye) maps to view -Z, the part of `up`
 * orthogonal to it to view +Y. With the engine's world axes (Z up, +Y forward, SPEC §5.5), a
 * camera at the origin with target (0, 1, 0) and up DE_AXIS_UP maps world (0, 5, 0) to view
 * (0, 0, -5), world +X to view +X and world +Z to view +Y.
 *
 * `up` does not need to be unit length or orthogonal to the view direction. When it is parallel
 * to (target - eye), DE_AXIS_FORWARD is used instead (DE_AXIS_RIGHT if that is parallel too).
 */
DE_API de_mat4 de_m4_look_at(de_vec3 eye, de_vec3 target, de_vec3 up);
/**
 * View -> clip projection: reverse-Z with an infinite far plane, for a view space that looks
 * down -Z (de_m4_look_at). fov_y is the full vertical field of view in radians, aspect is
 * width / height, near_z > 0 is the distance to the near plane. With f = 1 / tan(fov_y / 2):
 *
 *     clip = (x * f / aspect,  y * f,  near_z,  -z)      NDC z = near_z / -z
 *
 *     | f/aspect 0  0   0      |
 *     | 0        f  0   0      |
 *     | 0        0  0   near_z |
 *     | 0        0  -1  0      |
 *
 * A point on the near plane has NDC z = 1; depth falls toward 0 at infinity (clip z in [0, 1]).
 * Points behind the camera (z > 0) have w < 0. NDC y points up; the RHI flips it for Vulkan.
 */
DE_API de_mat4 de_m4_perspective_rz(f32 fov_y, f32 aspect, f32 near_z);
/**
 * View -> clip orthographic projection, reverse-Z. The box [left, right] x [bottom, top] x
 * [-far_z, -near_z] in view space (looking down -Z) maps to NDC [-1, 1] x [-1, 1] x [0, 1], with
 * z = 1 at the near plane (view z = -near_z) and z = 0 at the far plane (view z = -far_z).
 */
DE_API de_mat4 de_m4_ortho_rz(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z);

/* ---- de_affine ---- */
DE_API de_affine de_affine_identity(void);
/** T * R * S, like de_m4_trs. */
DE_API de_affine de_affine_from_trs(de_vec3 t, de_quat r, de_vec3 s);
/** a * b: applies b first, then a. */
DE_API de_affine de_affine_mul(const de_affine *a, const de_affine *b);
/** Inverse. A singular linear part gives the zero transform. */
DE_API de_affine de_affine_inverse(const de_affine *a);
/** The 4x4 matrix with bottom row (0, 0, 0, 1). */
DE_API de_mat4 de_affine_to_m4(const de_affine *a);
/** The top three rows of m (its bottom row is ignored). */
DE_API de_affine de_affine_from_m4(const de_mat4 *m);
/** L * p + t. */
DE_API de_vec3 de_affine_transform_point(const de_affine *a, de_vec3 p);
/** L * d (no translation). */
DE_API de_vec3 de_affine_transform_dir(const de_affine *a, de_vec3 d);

/* ---- de_aabb ---- */
/** The empty box: min = +FLT_MAX, max = -FLT_MAX. Merging anything into it gives that thing. */
DE_API de_aabb de_aabb_empty(void);
/** true when min > max on any axis. */
DE_API bool de_aabb_is_empty(de_aabb b);
/** Smallest box holding `count` points. No points (or a NULL `points`) give the empty box. */
DE_API de_aabb de_aabb_from_points(const de_vec3 *points, size_t count);
DE_API de_aabb de_aabb_add_point(de_aabb b, de_vec3 p);
DE_API de_aabb de_aabb_merge(de_aabb a, de_aabb b);
/** (min + max) / 2. Meaningless for an empty box. */
DE_API de_vec3 de_aabb_center(de_aabb b);
/** Half size, (max - min) / 2. Meaningless for an empty box. */
DE_API de_vec3 de_aabb_extents(de_aabb b);
/** Smallest box holding the transformed box (Arvo's method). The empty box stays empty. */
DE_API de_aabb de_aabb_transform(de_aabb b, const de_affine *a);
/** true when p is inside or on the boundary. */
DE_API bool de_aabb_contains(de_aabb b, de_vec3 p);
/** true when the boxes overlap or touch. An empty box intersects nothing. */
DE_API bool de_aabb_intersects(de_aabb a, de_aabb b);

/* ---- de_plane ---- */
/** The plane through `point` with normal `normal` (normalized here). */
DE_API de_plane de_plane_from_point_normal(de_vec3 point, de_vec3 normal);
/** Scales n and d by 1 / |n|. A plane with |n| = 0 is returned unchanged. */
DE_API de_plane de_plane_normalize(de_plane p);
/** dot(n, point) + d: the signed distance when n is unit length (positive in front). */
DE_API f32 de_plane_distance(de_plane p, de_vec3 point);

/* ---- de_ray ---- */
/**
 * Slab test. true when the ray hits the box (boundary included) at some t >= 0; *t_hit (may be
 * NULL) is the entry distance, 0 when the origin is inside the box. A zero dir component is
 * handled: the ray then hits only when the origin is within that slab.
 */
DE_API bool de_ray_aabb(de_ray r, de_aabb box, f32 *t_hit);
/** true when the ray hits the plane at some t >= 0. A ray parallel to the plane never hits. */
DE_API bool de_ray_plane(de_ray r, de_plane p, f32 *t_hit);
/**
 * Moller-Trumbore, both faces. true when the ray hits triangle (a, b, c) at some t >= 0. On a
 * hit, *t_hit is the distance and *uv the barycentrics: hit = a + u * (b - a) + v * (c - a).
 * Degenerate triangles never hit. Outputs may be NULL.
 */
DE_API bool de_ray_triangle(de_ray r, de_vec3 a, de_vec3 b, de_vec3 c, f32 *t_hit, de_vec2 *uv);

/* ---- de_frustum ---- */
/**
 * The six planes of the clip volume of `view_proj` (Gribb-Hartmann), normalized, normals inward,
 * in world space when view_proj = proj * view. Assumes the reverse-Z clip volume of
 * de_m4_perspective_rz / de_m4_ortho_rz: -w <= x <= w, -w <= y <= w, 0 <= z <= w. NEAR is
 * z <= w (depth 1), FAR is z >= 0 (depth 0).
 *
 * With an infinite far plane the FAR row is degenerate (normal 0, d = near_z > 0). It is stored
 * as n = (0, 0, 0), d = FLT_MAX, a plane every point is in front of, so the tests below always
 * pass it. Any other plane whose normal has zero length is stored the same way.
 */
DE_API de_frustum de_frustum_from_m4(const de_mat4 *view_proj);
/** false when the box is entirely outside one plane (culled); true when it may be visible.
 *  Conservative: a box near a frustum corner may be kept. An empty box is culled. */
DE_API bool de_frustum_test_aabb(const de_frustum *f, de_aabb box);
/** false when the sphere is entirely outside one plane (culled); true when it may be visible. */
DE_API bool de_frustum_test_sphere(const de_frustum *f, de_vec3 center, f32 radius);

/* ================================ 3. Fast math, batch / SIMD APIs (DENG-242) =============== */

/* ================================ 4. Deterministic math (DENG-243) ========================= */

/* Deterministic math (simulation). Strict IEEE, own transcendentals, no approximate
 * reciprocals, compiled with -ffp-contract=off. */
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

/* ================================ 5. Large-world transforms (DENG-244) ===================== */

/* Camera-relative conversion for large worlds (double → float). */
DE_API de_vec3 de_dv3_relative(de_dvec3 p, de_dvec3 origin);
DE_API de_mat4 de_transform_to_m4_relative(const de_transform *t, de_dvec3 origin);

DE_EXTERN_C_END
#endif
