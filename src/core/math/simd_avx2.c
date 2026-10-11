/* simd_avx2.c: x86-64 AVX2 + FMA fast-math kernels (DENG-242, SPEC §5.4).
 *
 * Compiled with -mavx2 -mfma (/arch:AVX2 on MSVC and clang-cl; src/core/CMakeLists.txt). The
 * dispatcher selects this table only when cpuid reports AVX2 and FMA and XCR0 shows that the OS
 * saves the YMM registers. The 4-wide kernels of simd_kernels.h get the VEX encoding and a fused
 * v4_madd; the batch frustum cull is 8 boxes wide. On other architectures (or without the
 * flags) the file compiles to nothing and de__math_table_avx2() returns NULL. */
#include "core/math/math_simd.h"

/* Only when the file was built with AVX2 and FMA enabled (MSVC's /arch:AVX2 defines __AVX2__
 * and implies FMA); otherwise the table is left out. */
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__) &&                               \
    (defined(__FMA__) || (defined(_MSC_VER) && !defined(__clang__)))

#include <immintrin.h>

typedef __m128 v4;

#define DE__SIMD_FN(name) de__##name##_avx2

#define v4_load(p)                _mm_loadu_ps(p)
#define v4_store(p, v)            _mm_storeu_ps((p), (v))
#define v4_set1(x)                _mm_set1_ps(x)
#define v4_add(a, b)              _mm_add_ps((a), (b))
#define v4_sub(a, b)              _mm_sub_ps((a), (b))
#define v4_mul(a, b)              _mm_mul_ps((a), (b))
#define v4_div(a, b)              _mm_div_ps((a), (b))
#define v4_madd(a, b, c)          _mm_fmadd_ps((a), (b), (c))
#define v4_abs(v)                 _mm_andnot_ps(_mm_set1_ps(-0.0f), (v))
#define v4_cmpge(a, b)            _mm_cmp_ps((a), (b), _CMP_GE_OQ)
#define v4_cmplt(a, b)            _mm_cmp_ps((a), (b), _CMP_LT_OQ)
#define v4_cmpgt(a, b)            _mm_cmp_ps((a), (b), _CMP_GT_OQ)
#define v4_or(a, b)               _mm_or_ps((a), (b))
#define v4_select(m, a, b)        _mm_blendv_ps((b), (a), (m))
#define v4_movemask(m)            _mm_movemask_ps(m)
#define v4_shuf(a, b, i, j, k, l) _mm_shuffle_ps((a), (b), _MM_SHUFFLE(l, k, j, i))

#define DE__SIMD_CUSTOM_CULL
#define DE__SIMD_CULL_FN de__frustum_cull_aabbs_avx2

static void de__frustum_cull_aabbs_avx2(const de_frustum *f, const de_aabb *boxes, u32 n,
                                        u8 *visible);

#include "core/math/simd_kernels.h"

/* Eight boxes per step: two 4-box transposes, joined into 8-lane registers. Multiplies and adds
 * stay separate (no FMA) and in the scalar order, so the result is the scalar test's. */
static void de__frustum_cull_aabbs_avx2(const de_frustum *f, const de_aabb *boxes, u32 n,
                                        u8 *visible) {
    de__k_cull_plane pl[6];
    de__k_cull_planes(f, pl);
    __m256 pn[6][4];
    for (int k = 0; k < 6; ++k) {
        pn[k][0] = _mm256_set1_ps(pl[k].n[0]);
        pn[k][1] = _mm256_set1_ps(pl[k].n[1]);
        pn[k][2] = _mm256_set1_ps(pl[k].n[2]);
        pn[k][3] = _mm256_set1_ps(pl[k].d);
    }
    const __m256 zero = _mm256_setzero_ps();
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        v4 lo[6], hi[6];
        de__k_load_boxes4(boxes + i, lo);
        de__k_load_boxes4(boxes + i + 4, hi);
        __m256 b[6];
        for (int k = 0; k < 6; ++k)
            b[k] = _mm256_insertf128_ps(_mm256_castps128_ps256(lo[k]), hi[k], 1);
        __m256 culled = _mm256_or_ps(_mm256_or_ps(_mm256_cmp_ps(b[0], b[3], _CMP_GT_OQ),
                                                  _mm256_cmp_ps(b[1], b[4], _CMP_GT_OQ)),
                                     _mm256_cmp_ps(b[2], b[5], _CMP_GT_OQ));
        for (int k = 0; k < 6; ++k) {
            __m256 dist = _mm256_mul_ps(pn[k][0], b[pl[k].sel[0]]);
            dist = _mm256_add_ps(dist, _mm256_mul_ps(pn[k][1], b[pl[k].sel[1]]));
            dist = _mm256_add_ps(dist, _mm256_mul_ps(pn[k][2], b[pl[k].sel[2]]));
            dist = _mm256_add_ps(dist, pn[k][3]);
            culled = _mm256_or_ps(culled, _mm256_cmp_ps(dist, zero, _CMP_LT_OQ));
        }
        int mask = _mm256_movemask_ps(culled);
        for (int k = 0; k < 8; ++k)
            visible[i + (u32)k] = (u8)(((mask >> k) & 1) ^ 1);
    }
    for (; i < n; ++i)
        visible[i] = de__frustum_test_aabb_scalar(f, boxes[i]) ? 1 : 0;
}

static const de__math_table de__table_avx2 = DE__SIMD_TABLE_INIT(DE_MATH_ISA_AVX2);

const de__math_table *de__math_table_avx2(void) {
    return &de__table_avx2;
}

#else

const de__math_table *de__math_table_avx2(void) {
    return NULL;
}

#endif
