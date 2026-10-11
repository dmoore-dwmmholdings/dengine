/* simd_dispatch.c: picks the fast-math ISA once and defines the dispatched public functions
 * (DENG-242, SPEC §5.4). See math_simd.h for the table and the ISA files.
 *
 * Compiled with the module's default flags (no -mavx2 here), so the CPU checks run on any x86-64
 * CPU. The active table pointer is resolved on the first call; racing threads all compute the
 * same table, so the last store wins harmlessly. */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif

#include "core/math/math_simd.h"

#include "core/math/math_internal.h"
#include "platform/de_atomic.h"

#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#define DE__MATH_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

/* ---- Scalar batch loops (the reference for the batch kernels) ---- */

void de__m4_transform_points_scalar(const de_mat4 *m, const de_vec3 *in, de_vec4 *out, u32 n) {
    const f32 *a = m->m;
    for (u32 i = 0; i < n; ++i) {
        de_vec3 p = in[i];
        de_vec4 r;
        r.x = a[0] * p.x + a[4] * p.y + a[8] * p.z + a[12];
        r.y = a[1] * p.x + a[5] * p.y + a[9] * p.z + a[13];
        r.z = a[2] * p.x + a[6] * p.y + a[10] * p.z + a[14];
        r.w = a[3] * p.x + a[7] * p.y + a[11] * p.z + a[15];
        out[i] = r;
    }
}

void de__affine_transform_aabbs_scalar(const de_affine *a, const de_aabb *in, de_aabb *out, u32 n) {
    for (u32 i = 0; i < n; ++i)
        out[i] = de_aabb_transform(in[i], a);
}

void de__frustum_cull_aabbs_scalar(const de_frustum *f, const de_aabb *boxes, u32 n, u8 *visible) {
    for (u32 i = 0; i < n; ++i)
        visible[i] = de__frustum_test_aabb_scalar(f, boxes[i]) ? 1 : 0;
}

const de__math_table de__math_table_scalar = {
    DE_MATH_ISA_SCALAR,
    de__m4_mul_scalar,
    de__m4_inverse_scalar,
    de__m4_mul_v4_scalar,
    de__quat_mul_scalar,
    de__quat_rotate_scalar,
    de__affine_mul_scalar,
    de__frustum_test_aabb_scalar,
    de__m4_transform_points_scalar,
    de__affine_transform_aabbs_scalar,
    de__frustum_cull_aabbs_scalar,
};

/* ---- CPU features ---- */

#if defined(DE__MATH_X86)
static void de__cpuid(u32 leaf, u32 sub, u32 r[4]) {
#if defined(_MSC_VER) && !defined(__clang__)
    int v[4];
    __cpuidex(v, (int)leaf, (int)sub);
    for (int i = 0; i < 4; ++i)
        r[i] = (u32)v[i];
#else
    unsigned a, b, c, d;
    __cpuid_count(leaf, sub, a, b, c, d);
    r[0] = a;
    r[1] = b;
    r[2] = c;
    r[3] = d;
#endif
}

static u64 de__xgetbv0(void) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (u64)_xgetbv(0);
#else
    /* Inline asm: the _xgetbv intrinsic needs -mxsave, and this file is built without it. */
    u32 lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((u64)hi << 32) | lo;
#endif
}

/* bit 0: SSE4.2, bit 1: AVX2 + FMA with OS YMM support. */
static u32 de__x86_features(void) {
    u32 r[4];
    de__cpuid(0, 0, r);
    u32 max_leaf = r[0];
    if (max_leaf < 1)
        return 0;
    de__cpuid(1, 0, r);
    u32 ecx1 = r[2];
    u32 features = 0;
    if (ecx1 & (1u << 20)) /* SSE4.2 (implies SSE4.1, SSSE3, SSE3) */
        features |= 1u;
    bool fma = (ecx1 & (1u << 12)) != 0;
    bool osxsave = (ecx1 & (1u << 27)) != 0;
    bool avx = (ecx1 & (1u << 28)) != 0;
    if (max_leaf >= 7 && fma && osxsave && avx && (features & 1u)) {
        /* XCR0 bits 1 (XMM) and 2 (YMM): the OS saves the AVX state on a context switch. */
        if ((de__xgetbv0() & 6u) == 6u) {
            de__cpuid(7, 0, r);
            if (r[1] & (1u << 5)) /* EBX bit 5: AVX2 */
                features |= 2u;
        }
    }
    return features;
}
#endif

static bool de__isa_supported(de_math_isa isa) {
    switch (isa) {
    case DE_MATH_ISA_SCALAR:
        return true;
#if defined(DE__MATH_X86)
    case DE_MATH_ISA_SSE42:
        return (de__x86_features() & 1u) != 0;
    case DE_MATH_ISA_AVX2:
        return (de__x86_features() & 2u) != 0;
#endif
    case DE_MATH_ISA_NEON:
        return true; /* part of the arm64 base ISA; the table is NULL elsewhere */
    default:
        return false;
    }
}

const de__math_table *de__math_table_for(de_math_isa isa) {
    const de__math_table *t = NULL;
    switch (isa) {
    case DE_MATH_ISA_SCALAR:
        return &de__math_table_scalar;
    case DE_MATH_ISA_SSE42:
        t = de__math_table_sse42();
        break;
    case DE_MATH_ISA_AVX2:
        t = de__math_table_avx2();
        break;
    case DE_MATH_ISA_NEON:
        t = de__math_table_neon();
        break;
    default:
        return NULL;
    }
    return t != NULL && de__isa_supported(isa) ? t : NULL;
}

/* ---- Selection ---- */

const char *de_math_isa_name(de_math_isa isa) {
    switch (isa) {
    case DE_MATH_ISA_SCALAR:
        return "scalar";
    case DE_MATH_ISA_SSE42:
        return "sse4.2";
    case DE_MATH_ISA_AVX2:
        return "avx2";
    case DE_MATH_ISA_NEON:
        return "neon";
    default:
        return "unknown";
    }
}

static bool de__env_set(const char *name) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static const de__math_table *de__math_choose(void) {
#if defined(DE_MATH_FORCE_SCALAR)
    return &de__math_table_scalar;
#else
    if (de__env_set("DE_MATH_FORCE_SCALAR"))
        return &de__math_table_scalar;
    const char *want = getenv("DE_MATH_ISA");
    if (want != NULL) {
        for (int i = 0; i < DE_MATH_ISA_COUNT; ++i) {
            if (strcmp(want, de_math_isa_name((de_math_isa)i)) == 0) {
                const de__math_table *t = de__math_table_for((de_math_isa)i);
                if (t != NULL)
                    return t;
                break; /* not available here: fall through to the default choice */
            }
        }
    }
    static const de_math_isa order[] = {DE_MATH_ISA_AVX2, DE_MATH_ISA_SSE42, DE_MATH_ISA_NEON};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; ++i) {
        const de__math_table *t = de__math_table_for(order[i]);
        if (t != NULL)
            return t;
    }
    return &de__math_table_scalar;
#endif
}

/* The tables are static const objects, complete before any code runs, so the pointer publishes
 * no data of its own: relaxed loads and stores are enough. */
static de_atomic_ptr de__math_active = DE_ATOMIC_INIT(NULL);

static const de__math_table *de__math_resolve(void) {
    const de__math_table *t = de__math_choose();
    de_atomic_store_ptr(&de__math_active, (void *)(uintptr_t)t, DE_ATOMIC_RELAXED);
    return t;
}

static inline const de__math_table *de__tbl(void) {
    const de__math_table *t =
        (const de__math_table *)de_atomic_load_ptr(&de__math_active, DE_ATOMIC_RELAXED);
    if (t == NULL)
        t = de__math_resolve();
    return t;
}

const de__math_table *de__math_active_table(void) {
    return de__tbl();
}

de_math_isa de_math_init(void) {
    return de__tbl()->isa;
}

de_math_isa de_math_active_isa(void) {
    return de__tbl()->isa;
}

/* ---- Dispatched public functions ---- */

de_mat4 de_m4_mul(const de_mat4 *a, const de_mat4 *b) {
    return de__tbl()->m4_mul(a, b);
}

de_mat4 de_m4_inverse(const de_mat4 *m) {
    return de__tbl()->m4_inverse(m);
}

de_vec4 de_m4_mul_v4(const de_mat4 *m, de_vec4 v) {
    return de__tbl()->m4_mul_v4(m, v);
}

de_quat de_quat_mul(de_quat a, de_quat b) {
    return de__tbl()->quat_mul(a, b);
}

de_vec3 de_quat_rotate(de_quat q, de_vec3 v) {
    return de__tbl()->quat_rotate(q, v);
}

de_affine de_affine_mul(const de_affine *a, const de_affine *b) {
    return de__tbl()->affine_mul(a, b);
}

bool de_frustum_test_aabb(const de_frustum *f, de_aabb box) {
    return de__tbl()->frustum_test_aabb(f, box);
}

void de_m4_transform_points(const de_mat4 *m, const de_vec3 *in, de_vec4 *out, u32 n) {
    if (n == 0)
        return;
    de__tbl()->m4_transform_points(m, in, out, n);
}

void de_affine_transform_aabbs(const de_affine *a, const de_aabb *in, de_aabb *out, u32 n) {
    if (n == 0)
        return;
    de__tbl()->affine_transform_aabbs(a, in, out, n);
}

void de_frustum_cull_aabbs(const de_frustum *f, const de_aabb *boxes, u32 n, u8 *visible_out) {
    if (n == 0)
        return;
    de__tbl()->frustum_cull_aabbs(f, boxes, n, visible_out);
}
