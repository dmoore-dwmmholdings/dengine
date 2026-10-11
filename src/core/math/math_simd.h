/* math_simd.h: the fast-math dispatch table and the scalar reference (DENG-242, SPEC §5.4).
 *
 * The hot fast-math functions and the batch APIs of de_math.h section 3 go through one table of
 * function pointers per instruction set. simd_dispatch.c picks the table once (de_math_init or
 * lazily on the first call) and defines the public de_* entry points, which call through it.
 *
 *   ISA       file           selected when
 *   scalar    mat.c, ...     always available; DE_MATH_FORCE_SCALAR forces it
 *   sse4.2    simd_sse.c     x86-64 and cpuid reports SSE4.2
 *   avx2      simd_avx2.c    x86-64 and cpuid + XCR0 report AVX2, FMA and YMM state
 *   neon      simd_neon.c    arm64 (NEON is part of the base ISA)
 *
 * This header has no intrinsics, so the dispatcher and the tests can include it anywhere. The
 * ISA files include simd_kernels.h instead, under their per-file -msse4.2 / -mavx2 flags.
 *
 * Fast math only. Deterministic code (SPEC §5.7) must never reach these tables: the SIMD paths
 * use FMA, and their rounding differs from the scalar reference in the last bits. */
#ifndef DE_CORE_MATH_SIMD_H
#define DE_CORE_MATH_SIMD_H

#include <dengine/de_math.h>

/** One implementation of every dispatched function. Signatures match the public API. */
typedef struct de__math_table {
    de_math_isa isa;
    de_mat4 (*m4_mul)(const de_mat4 *a, const de_mat4 *b);
    de_mat4 (*m4_inverse)(const de_mat4 *m);
    de_vec4 (*m4_mul_v4)(const de_mat4 *m, de_vec4 v);
    de_quat (*quat_mul)(de_quat a, de_quat b);
    de_vec3 (*quat_rotate)(de_quat q, de_vec3 v);
    de_affine (*affine_mul)(const de_affine *a, const de_affine *b);
    bool (*frustum_test_aabb)(const de_frustum *f, de_aabb box);
    void (*m4_transform_points)(const de_mat4 *m, const de_vec3 *in, de_vec4 *out, u32 n);
    void (*affine_transform_aabbs)(const de_affine *a, const de_aabb *in, de_aabb *out, u32 n);
    void (*frustum_cull_aabbs)(const de_frustum *f, const de_aabb *boxes, u32 n, u8 *visible);
} de__math_table;

/* ---- Scalar reference (DENG-241 files plus the scalar batch loops in simd_dispatch.c) ---- */
de_mat4 de__m4_mul_scalar(const de_mat4 *a, const de_mat4 *b);
de_mat4 de__m4_inverse_scalar(const de_mat4 *m);
de_vec4 de__m4_mul_v4_scalar(const de_mat4 *m, de_vec4 v);
de_quat de__quat_mul_scalar(de_quat a, de_quat b);
de_vec3 de__quat_rotate_scalar(de_quat q, de_vec3 v);
de_affine de__affine_mul_scalar(const de_affine *a, const de_affine *b);
/* The plane distance is evaluated one operation per statement (no contraction), in the order
 * ((nx * px + ny * py) + nz * pz) + d. The SIMD culls use the same operations, so the visible
 * sets are bitwise identical. */
bool de__frustum_test_aabb_scalar(const de_frustum *f, de_aabb box);
void de__m4_transform_points_scalar(const de_mat4 *m, const de_vec3 *in, de_vec4 *out, u32 n);
void de__affine_transform_aabbs_scalar(const de_affine *a, const de_aabb *in, de_aabb *out, u32 n);
void de__frustum_cull_aabbs_scalar(const de_frustum *f, const de_aabb *boxes, u32 n, u8 *visible);

/* ---- Tables ---- */
extern const de__math_table de__math_table_scalar;
/* Each ISA file defines its table only on the architecture it targets, and returns NULL from
 * these getters elsewhere. Availability on this CPU is checked by the dispatcher, not here. */
const de__math_table *de__math_table_sse42(void);
const de__math_table *de__math_table_avx2(void);
const de__math_table *de__math_table_neon(void);

/**
 * The table of `isa` when it is compiled in and this CPU supports it, else NULL. The scalar table
 * is always available. Tests use it to run every available ISA against the scalar reference.
 */
const de__math_table *de__math_table_for(de_math_isa isa);

/** The table the public functions use (resolves it on first use). */
const de__math_table *de__math_active_table(void);

#endif
