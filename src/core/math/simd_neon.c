/* simd_neon.c: arm64 NEON (Advanced SIMD) fast-math kernels (DENG-242, SPEC §5.4).
 *
 * NEON is part of the arm64 base ISA, so this file needs no extra compiler flags and its table is
 * always available on arm64. v4_madd is a fused multiply-add (vfmaq_f32). Shuffles use the
 * compiler's generic vector shuffle, so this file needs Clang or GCC; with any other compiler (or
 * on another architecture) it compiles to nothing and de__math_table_neon() returns NULL. */
#include "core/math/math_simd.h"

#if (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__clang__) || defined(__GNUC__))

#include <arm_neon.h>

typedef float32x4_t v4;

#define DE__SIMD_FN(name) de__##name##_neon

#define v4_load(p)       vld1q_f32(p)
#define v4_store(p, v)   vst1q_f32((p), (v))
#define v4_set1(x)       vdupq_n_f32(x)
#define v4_add(a, b)     vaddq_f32((a), (b))
#define v4_sub(a, b)     vsubq_f32((a), (b))
#define v4_mul(a, b)     vmulq_f32((a), (b))
#define v4_div(a, b)     vdivq_f32((a), (b))
#define v4_madd(a, b, c) vfmaq_f32((c), (a), (b))
#define v4_abs(v)        vabsq_f32(v)
#define v4_cmpge(a, b)   vreinterpretq_f32_u32(vcgeq_f32((a), (b)))
#define v4_cmplt(a, b)   vreinterpretq_f32_u32(vcltq_f32((a), (b)))
#define v4_cmpgt(a, b)   vreinterpretq_f32_u32(vcgtq_f32((a), (b)))
#define v4_or(a, b)                                                                                \
    vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(a), vreinterpretq_u32_f32(b)))
#define v4_select(m, a, b) vbslq_f32(vreinterpretq_u32_f32(m), (a), (b))

#if defined(__clang__)
#define v4_shuf(a, b, i, j, k, l) __builtin_shufflevector((a), (b), i, j, (k) + 4, (l) + 4)
#else
#define v4_shuf(a, b, i, j, k, l) __builtin_shuffle((a), (b), (uint32x4_t){i, j, (k) + 4, (l) + 4})
#endif

static inline int v4_movemask(v4 m) {
    static const int32_t shifts[4] = {0, 1, 2, 3};
    uint32x4_t bits = vshrq_n_u32(vreinterpretq_u32_f32(m), 31);
    return (int)vaddvq_u32(vshlq_u32(bits, vld1q_s32(shifts)));
}

#include "core/math/simd_kernels.h"

static const de__math_table de__table_neon = DE__SIMD_TABLE_INIT(DE_MATH_ISA_NEON);

const de__math_table *de__math_table_neon(void) {
    return &de__table_neon;
}

#else

const de__math_table *de__math_table_neon(void) {
    return NULL;
}

#endif
